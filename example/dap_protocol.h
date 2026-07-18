//	DAP protocol layer — PURE (no SDL, no threads). Debug Adapter Protocol
//	message builders/parsers. DAP uses the same Content-Length framing as LSP
//	(lsp::frameMessage / lsp::parseFrame are reused), but its own message shape:
//	{seq, type:"request"/"response"/"event", command/event, arguments/body}.
//	Selftest-covered; the transport (process + threads) lives in dap_client.
//
//	Copyright (c) 2026 Logan Brunet (ImGui-IDE). All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.

#pragma once

#include <string>
#include <vector>

namespace dap {

// ── message builders (return framed, ready-to-send bytes) ────────────────────
// All line numbers cross this API 1-BASED — the initialize request advertises
// linesStartAt1/columnsStartAt1 (the DAP default); the editor converts.
std::string buildInitialize(int seq);
// `adapterType` fills the launch config's "type" ("python" for debugpy);
// empty omits it (lldb-dap and friends don't need one).
std::string buildLaunch(int seq, const std::string& adapterType,
                        const std::string& program, const std::string& cwd,
                        bool stopOnEntry);
std::string buildSetBreakpoints(int seq, const std::string& sourcePath, const std::vector<int>& lines);
std::string buildConfigurationDone(int seq);
std::string buildContinue(int seq, int threadId);
std::string buildNext(int seq, int threadId);        // step over
std::string buildStepIn(int seq, int threadId);
std::string buildStepOut(int seq, int threadId);
std::string buildPause(int seq, int threadId);
std::string buildThreads(int seq);
std::string buildStackTrace(int seq, int threadId);
std::string buildScopes(int seq, int frameId);
std::string buildVariables(int seq, int variablesReference);
std::string buildEvaluate(int seq, const std::string& expression, int frameId);
std::string buildDisconnect(int seq, bool terminateDebuggee);
// Reply to an adapter->client (reverse) request we don't implement.
std::string buildErrorResponse(int requestSeq, const std::string& command);

// ── results ──────────────────────────────────────────────────────────────────
struct ThreadInfo {
	int         id = 0;
	std::string name;
};
struct StackFrame {
	int         id = 0;          // frameId for scopes/evaluate
	std::string name;
	std::string sourcePath;      // "" when the frame has no source (library code)
	int         line = 0;        // 1-based (as on the wire)
};
struct Scope {
	std::string name;
	int         variablesReference = 0;
	bool        expensive = false;
};
struct Variable {
	std::string name;
	std::string value;
	std::string type;
	int         variablesReference = 0;   // > 0 → has expandable children
};
struct StoppedInfo {
	std::string reason;          // "breakpoint", "step", "exception", …
	std::string description;
	int         threadId = 0;
	bool        allThreadsStopped = false;
};

// ── inbound routing ──────────────────────────────────────────────────────────
// A decoded incoming message header: a response to one of our requests, an
// event, or a reverse request (adapter -> client, needs a reply).
struct Incoming {
	std::string type;         // "response" / "event" / "request"
	std::string command;      // response+request: the command name
	std::string event;        // event: the event name
	int         requestSeq = 0;   // response: which request it answers
	int         seq = 0;          // the message's own seq (reverse-request replies need it)
	bool        success = true;   // response only
};
Incoming inspect(const std::string& body);

// Parse the bodies (each takes the full JSON message; empty/default on error).
bool                     parseStopped(const std::string& body, StoppedInfo& out);
// output event → (category, text); returns false if not parseable.
bool                     parseOutput(const std::string& body, std::string& category, std::string& text);
std::vector<ThreadInfo>  parseThreads(const std::string& body);
std::vector<StackFrame>  parseStackTrace(const std::string& body);
std::vector<Scope>       parseScopes(const std::string& body);
std::vector<Variable>    parseVariables(const std::string& body);
// evaluate response → result string ("" on failure).
std::string              parseEvaluate(const std::string& body);
// exited event → exit code (-1 if absent).
int                      parseExitCode(const std::string& body);

} // namespace dap
