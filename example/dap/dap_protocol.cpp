//	DAP protocol layer — implementation. Reuses LSP's Content-Length framing.
//
//	Copyright (c) 2026 Logan Brunet (ImGui-IDE). All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.

#include "dap_protocol.h"
#include "lsp_protocol.h"   // frameMessage — DAP framing is identical

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace dap {

namespace {

std::string frameRequest(int seq, const char* command, json arguments)
{
	json m = {
		{"seq", seq},
		{"type", "request"},
		{"command", command},
	};
	if (!arguments.is_null())
		m["arguments"] = std::move(arguments);
	return lsp::frameMessage(m.dump());
}

// One thread-targeted request ("continue", "next", …) differs only by name.
std::string frameThreadRequest(int seq, const char* command, int threadId)
{
	return frameRequest(seq, command, json{{"threadId", threadId}});
}

} // namespace

std::string buildInitialize(int seq)
{
	return frameRequest(seq, "initialize", json{
		{"clientID", "imgui-ide"},
		{"clientName", "ImGui-IDE"},
		{"adapterID", "imgui-ide"},
		{"linesStartAt1", true},
		{"columnsStartAt1", true},
		{"pathFormat", "path"},
		{"supportsVariableType", true},
		{"locale", "en-US"},
	});
}

std::string buildLaunch(int seq, const std::string& adapterType,
                        const std::string& program, const std::string& cwd,
                        bool stopOnEntry,
                        const std::vector<std::pair<std::string, std::string>>& extraStrings,
                        const std::vector<std::string>& programArgs)
{
	json args = {
		{"name", "ImGui-IDE launch"},
		{"request", "launch"},
		{"program", program},
		{"stopOnEntry", stopOnEntry},
		// debugpy: run inside the adapter, output arrives as "output" events.
		{"console", "internalConsole"},
		{"justMyCode", true},
	};
	if (!adapterType.empty())
		args["type"] = adapterType;
	if (!cwd.empty())
		args["cwd"] = cwd;
	if (!programArgs.empty())
		args["args"] = programArgs;
	for (auto& [k, v] : extraStrings)
		args[k] = v;
	return frameRequest(seq, "launch", std::move(args));
}

std::string buildSetBreakpoints(int seq, const std::string& sourcePath, const std::vector<int>& lines)
{
	json bps = json::array();
	for (int l : lines)
		bps.push_back(json{{"line", l}});
	return frameRequest(seq, "setBreakpoints", json{
		{"source", {{"path", sourcePath}}},
		{"breakpoints", std::move(bps)},
	});
}

std::string buildConfigurationDone(int seq) { return frameRequest(seq, "configurationDone", json()); }
std::string buildContinue(int seq, int threadId) { return frameThreadRequest(seq, "continue", threadId); }
std::string buildNext(int seq, int threadId) { return frameThreadRequest(seq, "next", threadId); }
std::string buildStepIn(int seq, int threadId) { return frameThreadRequest(seq, "stepIn", threadId); }
std::string buildStepOut(int seq, int threadId) { return frameThreadRequest(seq, "stepOut", threadId); }
std::string buildPause(int seq, int threadId) { return frameThreadRequest(seq, "pause", threadId); }
std::string buildThreads(int seq) { return frameRequest(seq, "threads", json()); }

std::string buildStackTrace(int seq, int threadId)
{
	return frameRequest(seq, "stackTrace", json{{"threadId", threadId}, {"startFrame", 0}, {"levels", 64}});
}

std::string buildScopes(int seq, int frameId)
{
	return frameRequest(seq, "scopes", json{{"frameId", frameId}});
}

std::string buildVariables(int seq, int variablesReference)
{
	return frameRequest(seq, "variables", json{{"variablesReference", variablesReference}});
}

std::string buildEvaluate(int seq, const std::string& expression, int frameId)
{
	json args = {{"expression", expression}, {"context", "repl"}};
	if (frameId > 0)
		args["frameId"] = frameId;
	return frameRequest(seq, "evaluate", std::move(args));
}

std::string buildDisconnect(int seq, bool terminateDebuggee)
{
	return frameRequest(seq, "disconnect", json{{"terminateDebuggee", terminateDebuggee}});
}

std::string buildErrorResponse(int requestSeq, const std::string& command)
{
	json m = {
		{"seq", 0},
		{"type", "response"},
		{"request_seq", requestSeq},
		{"command", command},
		{"success", false},
		{"message", "not supported"},
	};
	return lsp::frameMessage(m.dump());
}

Incoming inspect(const std::string& body)
{
	Incoming in;
	json m = json::parse(body, nullptr, /*allow_exceptions*/ false);
	if (!m.is_object())
		return in;
	in.type = m.value("type", "");
	in.command = m.value("command", "");
	in.event = m.value("event", "");
	in.requestSeq = m.value("request_seq", 0);
	in.seq = m.value("seq", 0);
	in.success = m.value("success", true);
	return in;
}

bool parseStopped(const std::string& body, StoppedInfo& out)
{
	json m = json::parse(body, nullptr, false);
	if (!m.is_object() || !m.contains("body") || !m["body"].is_object())
		return false;
	const json& b = m["body"];
	out.reason = b.value("reason", "");
	out.description = b.value("description", "");
	out.threadId = b.value("threadId", 0);
	out.allThreadsStopped = b.value("allThreadsStopped", false);
	return true;
}

bool parseOutput(const std::string& body, std::string& category, std::string& text)
{
	json m = json::parse(body, nullptr, false);
	if (!m.is_object() || !m.contains("body") || !m["body"].is_object())
		return false;
	const json& b = m["body"];
	category = b.value("category", "console");
	text = b.value("output", "");
	return !text.empty();
}

std::vector<ThreadInfo> parseThreads(const std::string& body)
{
	std::vector<ThreadInfo> out;
	json m = json::parse(body, nullptr, false);
	if (!m.is_object() || !m.contains("body"))
		return out;
	for (const auto& t : m["body"].value("threads", json::array()))
	{
		if (!t.is_object())
			continue;
		ThreadInfo ti;
		ti.id = t.value("id", 0);
		ti.name = t.value("name", "");
		out.push_back(std::move(ti));
	}
	return out;
}

std::vector<StackFrame> parseStackTrace(const std::string& body)
{
	std::vector<StackFrame> out;
	json m = json::parse(body, nullptr, false);
	if (!m.is_object() || !m.contains("body"))
		return out;
	for (const auto& f : m["body"].value("stackFrames", json::array()))
	{
		if (!f.is_object())
			continue;
		StackFrame sf;
		sf.id = f.value("id", 0);
		sf.name = f.value("name", "");
		sf.line = f.value("line", 0);
		if (f.contains("source") && f["source"].is_object())
			sf.sourcePath = f["source"].value("path", "");
		out.push_back(std::move(sf));
	}
	return out;
}

std::vector<Scope> parseScopes(const std::string& body)
{
	std::vector<Scope> out;
	json m = json::parse(body, nullptr, false);
	if (!m.is_object() || !m.contains("body"))
		return out;
	for (const auto& s : m["body"].value("scopes", json::array()))
	{
		if (!s.is_object())
			continue;
		Scope sc;
		sc.name = s.value("name", "");
		sc.variablesReference = s.value("variablesReference", 0);
		sc.expensive = s.value("expensive", false);
		out.push_back(std::move(sc));
	}
	return out;
}

std::vector<Variable> parseVariables(const std::string& body)
{
	std::vector<Variable> out;
	json m = json::parse(body, nullptr, false);
	if (!m.is_object() || !m.contains("body"))
		return out;
	for (const auto& v : m["body"].value("variables", json::array()))
	{
		if (!v.is_object())
			continue;
		Variable var;
		var.name = v.value("name", "");
		var.value = v.value("value", "");
		var.type = v.value("type", "");
		var.variablesReference = v.value("variablesReference", 0);
		out.push_back(std::move(var));
	}
	return out;
}

std::string parseEvaluate(const std::string& body)
{
	json m = json::parse(body, nullptr, false);
	if (!m.is_object() || !m.contains("body") || !m["body"].is_object())
		return {};
	return m["body"].value("result", "");
}

int parseExitCode(const std::string& body)
{
	json m = json::parse(body, nullptr, false);
	if (!m.is_object() || !m.contains("body") || !m["body"].is_object())
		return -1;
	return m["body"].value("exitCode", -1);
}

} // namespace dap
