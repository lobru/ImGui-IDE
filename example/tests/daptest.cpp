//	DAP integration gate — spawns a REAL debug adapter (debugpy) and asserts a
//	full end-to-end session: breakpoint → launch → stopped → stack/scopes/
//	variables → evaluate → continue → exit. Headless (no GUI). Exit codes:
//	  0  = all checks passed
//	  1  = a check failed
//	  77 = python/debugpy not found (ctest treats this as SKIP, keeping CI green)
//
//	Copyright (c) 2026 Logan Brunet (ImGui-IDE). All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.

#define _CRT_SECURE_NO_WARNINGS   // std::getenv (MSVC C4996 vs /WX)

#include "dap_client.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_timer.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static int gFailures = 0, gChecks = 0;
#define CHECK(cond, msg) do { ++gChecks; if (!(cond)) { ++gFailures; \
	std::fprintf(stderr, "FAIL: %s\n", msg); } else { std::fprintf(stderr, "ok: %s\n", msg); } } while (0)

// Run `argv` to completion, capture stdout+exit code. Used to probe for debugpy.
static bool runOk(const std::vector<const char*>& argv)
{
	std::vector<const char*> args = argv;
	args.push_back(nullptr);
	SDL_Process* p = SDL_CreateProcess(args.data(), true);
	if (!p)
		return false;
	int code = -1;
	SDL_WaitProcess(p, true, &code);
	SDL_DestroyProcess(p);
	return code == 0;
}

static std::string findPythonWithDebugpy()
{
	if (const char* env = std::getenv("DAPTEST_PYTHON"); env && *env)
		return env;
	const char* candidates[] = {"python3", "python"};
	for (const char* py : candidates)
		if (runOk({py, "-c", "import debugpy"}))
			return py;
	return {};
}

// Poll the client until pred() is true or the deadline passes; drains into sink.
template <class Pred>
static bool pump(dap::DapClient& c, std::vector<dap::DapResult>& sink, int timeoutMs, Pred pred)
{
	int waited = 0;
	while (waited < timeoutMs)
	{
		for (auto& r : c.poll()) sink.push_back(std::move(r));
		if (pred()) return true;
		SDL_Delay(20);
		waited += 20;
	}
	for (auto& r : c.poll()) sink.push_back(std::move(r));
	return pred();
}

int main(int, char**)
{
	SDL_Init(0);   // base init (no subsystems) for the process API

	std::string py = findPythonWithDebugpy();
	if (py.empty())
	{
		std::fprintf(stderr, "SKIP: no python with debugpy importable\n");
		SDL_Quit();
		return 77;
	}
	std::fprintf(stderr, "using python: %s\n", py.c_str());

	// Write the debuggee: a script whose line 4 (1-based) is breakpoint-worthy
	// and has locals to inspect when stopped there.
	std::filesystem::path script = std::filesystem::current_path() / "daptest_target.py";
	{
		std::ofstream f(script);
		f << "def add(a, b):\n"          // line 1
		     "    total = a + b\n"       // line 2
		     "    return total\n"        // line 3
		     "answer = add(19, 23)\n"    // line 4  <- breakpoint
		     "print('answer', answer)\n";// line 5
	}

	dap::DapClient client;
	if (!client.start({py, "-m", "debugpy.adapter"}))
	{
		std::fprintf(stderr, "SKIP: debugpy adapter failed to spawn\n");
		SDL_Quit();
		return 77;
	}

	std::vector<dap::DapResult> sink;

	// initialize response → ready.
	bool ready = pump(client, sink, 15000, [&] { return client.ready(); });
	CHECK(ready, "debugpy initialize handshake completed");
	if (!ready) { client.stop(); SDL_Quit(); return 1; }

	// launch, then configure on the initialized event.
	client.launch("python", script.string(), script.parent_path().string(), false);
	bool configured = false;
	pump(client, sink, 15000, [&] {
		for (auto& r : sink)
			if (r.kind == dap::ResultKind::EvInitialized)
				return true;
		return false;
	});
	for (auto& r : sink)
		if (r.kind == dap::ResultKind::EvInitialized)
			configured = true;
	CHECK(configured, "initialized event received");
	if (configured)
	{
		client.setBreakpoints(script.string(), {2});   // inside add() — line 2, 1-based
		client.configurationDone();
	}

	// Expect a stopped event (reason breakpoint) with a thread id.
	dap::StoppedInfo stop;
	bool stopped = pump(client, sink, 20000, [&] {
		for (auto& r : sink)
			if (r.kind == dap::ResultKind::EvStopped) { stop = r.stopped; return true; }
		return false;
	});
	CHECK(stopped, "stopped event arrived after launch");
	CHECK(stop.reason == "breakpoint", "stop reason is 'breakpoint'");
	CHECK(stop.threadId != 0, "stop carries a thread id");

	int frameId = 0;
	if (stopped)
	{
		// Stack trace: top frame should be add() at line 2 of our script.
		client.stackTrace(stop.threadId);
		std::vector<dap::StackFrame> frames;
		pump(client, sink, 10000, [&] {
			for (auto& r : sink)
				if (r.kind == dap::ResultKind::StackTrace && !r.frames.empty()) { frames = r.frames; return true; }
			return false;
		});
		CHECK(!frames.empty(), "stack trace returned frames");
		if (!frames.empty())
		{
			CHECK(frames.front().name.find("add") != std::string::npos, "top frame is add()");
			CHECK(frames.front().line == 2, "top frame stopped at line 2");
			frameId = frames.front().id;
		}

		// Scopes → variables: locals of add() must contain a and b.
		client.scopes(frameId);
		int localsRef = 0;
		pump(client, sink, 10000, [&] {
			for (auto& r : sink)
				if (r.kind == dap::ResultKind::Scopes && !r.scopes.empty())
				{
					for (auto& s : r.scopes)
						if (localsRef == 0 || s.name.find("Local") != std::string::npos)
							localsRef = s.variablesReference;
					return true;
				}
			return false;
		});
		CHECK(localsRef != 0, "scopes include a locals reference");
		if (localsRef)
		{
			client.variables(localsRef);
			bool sawArgs = false;
			pump(client, sink, 10000, [&] {
				for (auto& r : sink)
					if (r.kind == dap::ResultKind::Variables && r.requestContext == localsRef)
					{
						bool a = false, b = false;
						for (auto& v : r.variables)
						{
							if (v.name == "a" && v.value == "19") a = true;
							if (v.name == "b" && v.value == "23") b = true;
						}
						sawArgs = a && b;
						return true;
					}
				return false;
			});
			CHECK(sawArgs, "locals show a=19 and b=23");
		}

		// Evaluate in the stopped frame.
		client.evaluate("a + b", frameId);
		bool evalOk = false;
		pump(client, sink, 10000, [&] {
			for (auto& r : sink)
				if (r.kind == dap::ResultKind::Evaluate) { evalOk = r.evaluateResult == "42"; return true; }
			return false;
		});
		CHECK(evalOk, "evaluate 'a + b' in the frame returns 42");

		// Continue to completion: program prints and exits 0.
		client.continueExec(stop.threadId);
		bool exited = false, sawOutput = false;
		pump(client, sink, 20000, [&] {
			for (auto& r : sink)
			{
				if (r.kind == dap::ResultKind::EvOutput && r.outputText.find("answer 42") != std::string::npos)
					sawOutput = true;
				if (r.kind == dap::ResultKind::EvExited || r.kind == dap::ResultKind::EvTerminated)
					exited = true;
			}
			return exited && sawOutput;
		});
		CHECK(sawOutput, "program output ('answer 42') arrived as output events");
		CHECK(exited, "exited/terminated event ends the session");
	}

	client.disconnect(true);
	client.stop();
	SDL_Quit();

	std::error_code ec;
	std::filesystem::remove(script, ec);

	if (gFailures == 0) { std::printf("daptest: all %d checks passed\n", gChecks); return 0; }
	std::fprintf(stderr, "daptest: %d/%d checks FAILED\n", gFailures, gChecks);
	return 1;
}
