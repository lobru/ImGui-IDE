//	DAP client transport — implementation. SDL3 process + a reader thread.
//	Mirrors lsp_client.cpp: bounded pipe writes, idle-backoff reads, and a
//	one-shot "adapter gone" sentinel when the process dies.
//
//	Copyright (c) 2026 Logan Brunet (ImGui-IDE). All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.

#include "dap_client.h"
#include "lsp_protocol.h"   // parseFrame — DAP framing is identical

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_timer.h>

namespace dap {

static SDL_IOStream* asIO(void* p) { return reinterpret_cast<SDL_IOStream*>(p); }

DapClient::~DapClient()
{
	stop();
}

bool DapClient::start(const std::vector<std::string>& argv)
{
	if (mProcess || argv.empty())
		return mProcess != nullptr;
	std::vector<const char*> args;
	args.reserve(argv.size() + 1);
	for (auto& a : argv)
		args.push_back(a.c_str());
	args.push_back(nullptr);
	mProcess = SDL_CreateProcess(args.data(), /*pipe_stdio*/ true);
	if (!mProcess)
		return false;
	mStdin = SDL_GetProcessInput(mProcess);
	mStdout = SDL_GetProcessOutput(mProcess);
	if (!mStdin || !mStdout)
	{
		SDL_DestroyProcess(mProcess);
		mProcess = nullptr;
		return false;
	}
	mReady = false;
	mShuttingDown = false;
	mStopped = false;
	mNextSeq = 2;
	{
		std::lock_guard<std::mutex> lk(mInMutex);
		mPending.clear();
		mPending[mInitializeSeq] = {ResultKind::Initialize, 0};
	}
	mReader = std::thread([this] { readerLoop(); });
	enqueueOut(buildInitialize(mInitializeSeq));
	flushOut();
	return true;
}

void DapClient::stop()
{
	if (mStopped.exchange(true))
		return;
	mShuttingDown = true;
	if (mReader.joinable())
		mReader.join();
	if (mProcess)
	{
		SDL_KillProcess(mProcess, /*force*/ true);
		SDL_DestroyProcess(mProcess);
		mProcess = nullptr;
	}
	mStdin = nullptr;
	mStdout = nullptr;
	mReady = false;
}

void DapClient::enqueueOut(const std::string& framed)
{
	std::lock_guard<std::mutex> lk(mOutMutex);
	mOutQueue.push_back(framed);
}

void DapClient::flushOut()
{
	std::lock_guard<std::mutex> lk(mOutMutex);
	if (!mStdin)
	{
		mOutQueue.clear();
		return;
	}
	for (auto& msg : mOutQueue)
	{
		std::size_t off = 0;
		int budget = 4000;   // bounded so a back-pressured pipe can't hang the UI thread
		while (off < msg.size() && budget-- > 0)
		{
			std::size_t w = SDL_WriteIO(asIO(mStdin), msg.data() + off, msg.size() - off);
			if (w > 0) { off += w; continue; }
			if (SDL_GetIOStatus(asIO(mStdin)) == SDL_IO_STATUS_NOT_READY) { SDL_Delay(1); continue; }
			break;   // error / closed
		}
	}
	SDL_FlushIO(asIO(mStdin));
	mOutQueue.clear();
}

void DapClient::readerLoop()
{
	std::string buf;
	char chunk[8192];
	int idle = 1;
	while (true)
	{
		std::size_t r = SDL_ReadIO(asIO(mStdout), chunk, sizeof(chunk));
		if (r > 0)
		{
			buf.append(chunk, r);
			std::size_t pos = 0;
			std::string body;
			while (lsp::parseFrame(buf, pos, body))
				dispatch(body);
			if (pos > 0)
				buf.erase(0, pos);
			idle = 1;
			continue;
		}
		SDL_IOStatus st = SDL_GetIOStatus(asIO(mStdout));
		if (st == SDL_IO_STATUS_EOF || st == SDL_IO_STATUS_ERROR)
			break;
		if (mShuttingDown.load())
			break;
		SDL_Delay((Uint32) idle);
		if (idle < 8) ++idle;   // back off idle polling to cut CPU
	}
	// One-shot "adapter gone" sentinel so the UI can end the session cleanly.
	std::lock_guard<std::mutex> lk(mInMutex);
	DapResult s;
	s.kind = ResultKind::AdapterGone;
	mInQueue.push_back(std::move(s));
}

void DapClient::dispatch(const std::string& body)
{
	Incoming in = inspect(body);

	if (in.type == "response")
	{
		Pending p{ResultKind::Initialize, 0};
		bool known = false;
		{
			std::lock_guard<std::mutex> lk(mInMutex);
			auto it = mPending.find(in.requestSeq);
			if (it != mPending.end()) { p = it->second; mPending.erase(it); known = true; }
		}
		if (!known)
			return;
		DapResult res;
		res.kind = p.kind;
		res.id = in.requestSeq;
		res.success = in.success;
		res.requestContext = p.context;
		if (in.success)
		{
			switch (p.kind)
			{
				case ResultKind::Initialize:  mReady.store(true); break;
				case ResultKind::Threads:     res.threads = parseThreads(body); break;
				case ResultKind::StackTrace:  res.frames = parseStackTrace(body); break;
				case ResultKind::Scopes:      res.scopes = parseScopes(body); break;
				case ResultKind::Variables:   res.variables = parseVariables(body); break;
				case ResultKind::Evaluate:    res.evaluateResult = parseEvaluate(body); break;
				default: break;
			}
		}
		std::lock_guard<std::mutex> lk(mInMutex);
		mInQueue.push_back(std::move(res));
		return;
	}

	if (in.type == "event")
	{
		DapResult res;
		if (in.event == "initialized")      res.kind = ResultKind::EvInitialized;
		else if (in.event == "stopped")     { res.kind = ResultKind::EvStopped; parseStopped(body, res.stopped); }
		else if (in.event == "continued")   res.kind = ResultKind::EvContinued;
		else if (in.event == "output")      { res.kind = ResultKind::EvOutput;
		                                      if (!parseOutput(body, res.outputCategory, res.outputText)) return; }
		else if (in.event == "terminated")  res.kind = ResultKind::EvTerminated;
		else if (in.event == "exited")      { res.kind = ResultKind::EvExited; res.exitCode = parseExitCode(body); }
		else return;   // process/thread/module/… — not surfaced
		std::lock_guard<std::mutex> lk(mInMutex);
		mInQueue.push_back(std::move(res));
		return;
	}

	if (in.type == "request")
	{
		// Microsoft vsdbg gates every session behind a signed "handshake"
		// reverse-request; the secret ships only in authorized MS hosts, so we
		// can't answer it. Surface a distinct result so the UI explains the
		// license situation instead of a bare "adapter gone" after the refusal.
		if (in.command == "handshake")
		{
			std::lock_guard<std::mutex> lk(mInMutex);
			DapResult s;
			s.kind = ResultKind::LicenseHandshake;
			mInQueue.push_back(std::move(s));
		}
		// Reverse request (handshake, runInTerminal, startDebugging, …) we don't
		// support — refuse it so the adapter doesn't block. Reader only ENQUEUES.
		enqueueOut(buildErrorResponse(in.seq, in.command));
	}
}

// Register `seq` as pending (kind + caller tag), then send the framed bytes.
// Each public wrapper takes its seq from mNextSeq (UI thread only) and builds
// the frame with it, so the pending map and the wire always agree.
int DapClient::request(ResultKind kind, int seq, std::string framed, int context)
{
	if (!mProcess)
		return 0;
	{
		std::lock_guard<std::mutex> lk(mInMutex);
		mPending[seq] = {kind, context};
	}
	enqueueOut(std::move(framed));
	flushOut();
	return seq;
}

int DapClient::launch(const std::string& adapterType, const std::string& program,
                      const std::string& cwd, bool stopOnEntry,
                      const std::vector<std::pair<std::string, std::string>>& extraStrings,
                      const std::vector<std::string>& programArgs)
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::Launch, s, buildLaunch(s, adapterType, program, cwd, stopOnEntry, extraStrings, programArgs));
}

int DapClient::setBreakpoints(const std::string& sourcePath, const std::vector<int>& lines1Based)
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::SetBreakpoints, s, buildSetBreakpoints(s, sourcePath, lines1Based));
}

int DapClient::configurationDone()
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::ConfigurationDone, s, buildConfigurationDone(s));
}

int DapClient::continueExec(int threadId)
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::Continue, s, buildContinue(s, threadId));
}

int DapClient::next(int threadId)
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::Next, s, buildNext(s, threadId));
}

int DapClient::stepIn(int threadId)
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::StepIn, s, buildStepIn(s, threadId));
}

int DapClient::stepOut(int threadId)
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::StepOut, s, buildStepOut(s, threadId));
}

int DapClient::pause(int threadId)
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::Pause, s, buildPause(s, threadId));
}

int DapClient::threads()
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::Threads, s, buildThreads(s));
}

int DapClient::stackTrace(int threadId)
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::StackTrace, s, buildStackTrace(s, threadId));
}

int DapClient::scopes(int frameId, int context)
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::Scopes, s, buildScopes(s, frameId), context);
}

int DapClient::variables(int variablesReference)
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::Variables, s, buildVariables(s, variablesReference), variablesReference);
}

int DapClient::evaluate(const std::string& expression, int frameId)
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::Evaluate, s, buildEvaluate(s, expression, frameId));
}

int DapClient::disconnect(bool terminateDebuggee)
{
	if (!mProcess) return 0;
	int s = mNextSeq++;
	return request(ResultKind::Disconnect, s, buildDisconnect(s, terminateDebuggee));
}

std::vector<DapResult> DapClient::poll()
{
	flushOut();
	std::vector<DapResult> out;
	{
		std::lock_guard<std::mutex> lk(mInMutex);
		out.swap(mInQueue);
	}
	return out;
}

} // namespace dap
