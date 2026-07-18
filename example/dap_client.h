//	DAP client transport — spawns a debug adapter (debugpy, lldb-dap, …) via
//	SDL3's process API and speaks the Debug Adapter Protocol asynchronously.
//	Same architecture as LspClient: a single reader thread reads stdout and
//	pushes decoded results to a queue the UI thread drains via poll(); the UI
//	thread owns all writes (the reader only ever ENQUEUES — avoids a pipe
//	deadlock). The pure protocol logic lives in dap_protocol.{h,cpp}.
//
//	Copyright (c) 2026 Logan Brunet (ImGui-IDE). All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.

#pragma once

#include "dap_protocol.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct SDL_Process;

namespace dap {

// What a poll() entry is. Responses carry the request kind they answer (the
// pending map translates request_seq -> kind); events carry their own kind.
enum class ResultKind {
	// responses
	Initialize, Launch, SetBreakpoints, ConfigurationDone,
	Continue, Next, StepIn, StepOut, Pause,
	Threads, StackTrace, Scopes, Variables, Evaluate, Disconnect,
	// events
	EvInitialized, EvStopped, EvContinued, EvOutput, EvTerminated, EvExited,
	// transport
	AdapterGone,
};

struct DapResult {
	ResultKind  kind = ResultKind::EvOutput;
	int         id = 0;            // responses: request seq this answers
	bool        success = true;    // responses only
	int         requestContext = 0;   // caller-supplied tag from the request (e.g. variablesReference)
	// parsed payloads (filled per kind)
	StoppedInfo              stopped;       // EvStopped
	std::string              outputCategory, outputText;   // EvOutput
	int                      exitCode = -1; // EvExited
	std::vector<ThreadInfo>  threads;       // Threads
	std::vector<StackFrame>  frames;        // StackTrace
	std::vector<Scope>       scopes;        // Scopes
	std::vector<Variable>    variables;     // Variables
	std::string              evaluateResult;   // Evaluate
};

class DapClient {
public:
	DapClient() = default;
	~DapClient();
	DapClient(const DapClient&) = delete;
	DapClient& operator=(const DapClient&) = delete;

	// Spawn the adapter (argv form: {"python", "-m", "debugpy.adapter"}) and
	// fire the initialize request. Non-blocking; ready() flips true when the
	// initialize response arrives. False only if the process failed to spawn.
	bool start(const std::vector<std::string>& argv);
	void stop();   // force-kill + join; idempotent (send Disconnect first for grace)

	bool spawned() const { return mProcess != nullptr; }
	bool ready() const { return mReady.load(); }

	// Requests (no-ops returning 0 until spawned; initialize is internal).
	// `context` is echoed back on the matching DapResult (requestContext) so
	// the caller can tag e.g. which variablesReference a Variables reply is for.
	int launch(const std::string& adapterType, const std::string& program,
	           const std::string& cwd, bool stopOnEntry);
	int setBreakpoints(const std::string& sourcePath, const std::vector<int>& lines1Based);
	int configurationDone();
	int continueExec(int threadId);
	int next(int threadId);
	int stepIn(int threadId);
	int stepOut(int threadId);
	int pause(int threadId);
	int threads();
	int stackTrace(int threadId);
	int scopes(int frameId, int context = 0);
	int variables(int variablesReference);
	int evaluate(const std::string& expression, int frameId);
	int disconnect(bool terminateDebuggee);

	// UI thread, once per frame: flush queued writes + return arrivals.
	std::vector<DapResult> poll();

private:
	void readerLoop();
	void enqueueOut(const std::string& framed);
	void flushOut();
	void dispatch(const std::string& body);
	int  request(ResultKind kind, int seq, std::string framed, int context = 0);

	SDL_Process* mProcess = nullptr;
	void*        mStdin = nullptr;    // SDL_IOStream*
	void*        mStdout = nullptr;   // SDL_IOStream*
	std::thread  mReader;

	std::atomic<bool> mReady{false};
	std::atomic<bool> mShuttingDown{false};
	std::atomic<bool> mStopped{false};

	int mNextSeq = 2;         // 1 reserved for initialize
	int mInitializeSeq = 1;

	std::mutex               mOutMutex;
	std::vector<std::string> mOutQueue;

	struct Pending { ResultKind kind; int context; };
	std::mutex                        mInMutex;
	std::vector<DapResult>            mInQueue;
	std::unordered_map<int, Pending>  mPending;   // request seq -> kind/context (guarded by mInMutex)
};

} // namespace dap
