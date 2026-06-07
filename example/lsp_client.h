//	LSP client transport — spawns a language server (clangd) via SDL3's process
//	API and speaks JSON-RPC asynchronously. A single reader thread reads stdout
//	and pushes decoded results to a queue the UI thread drains via poll(); the UI
//	thread owns all writes (the reader never writes — avoids a pipe deadlock).
//	The pure protocol logic lives in lsp_protocol.{h,cpp}.

#pragma once

#include "lsp_protocol.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SDL_Process;

namespace lsp {

enum class ResultKind { Completion, Definition, Hover };

struct LspResult {
	int                         id = 0;
	ResultKind                  kind = ResultKind::Completion;
	std::vector<CompletionItem> completionItems;
	std::vector<Location>       locations;
	std::string                 hoverText;
	bool                        serverGone = false;   // EOF/crash sentinel (delivered once)
};

class LspClient {
public:
	LspClient() = default;
	~LspClient();
	LspClient(const LspClient&) = delete;
	LspClient& operator=(const LspClient&) = delete;

	// Spawn `serverPath` rooted at `rootUri`. Non-blocking: the initialize
	// handshake runs asynchronously — ready() flips true once it completes.
	// Returns false only if the process failed to spawn.
	bool start(const std::string& serverPath, const std::string& rootUri);
	void stop();                                   // graceful then force; idempotent

	bool spawned() const { return mProcess != nullptr; }
	bool ready() const { return mReady.load(); }   // initialize handshake done
	const std::string& offsetEncoding() const { return mEncoding; }

	// Document lifecycle (no-ops until ready()). Versions tracked per uri.
	void didOpen(const std::string& uri, const std::string& langId, const std::string& text);
	void didChange(const std::string& uri, const std::string& text);
	void didClose(const std::string& uri);
	bool isOpen(const std::string& uri);

	// Fire a request; returns its id (correlate with poll() results) or 0 if not ready.
	int requestCompletion(const std::string& uri, int line, int character);
	int requestDefinition(const std::string& uri, int line, int character);
	int requestHover(const std::string& uri, int line, int character);

	// UI thread, once per frame: flush queued writes + return results that arrived.
	std::vector<LspResult> poll();

private:
	void readerLoop();
	void enqueueOut(const std::string& framed);
	void flushOut();
	void dispatch(const std::string& body);
	void teardown();   // join reader + kill/destroy process (idempotent)

	SDL_Process* mProcess = nullptr;
	void*        mStdin = nullptr;    // SDL_IOStream* (process stdin)
	void*        mStdout = nullptr;   // SDL_IOStream* (process stdout)
	std::thread  mReader;

	std::atomic<bool> mReady{false};
	std::atomic<bool> mShuttingDown{false};
	std::atomic<bool> mStopped{false};

	int         mNextId = 2;        // 1 reserved for initialize
	int         mInitializeId = 1;
	std::string mEncoding = "utf-16";

	std::mutex               mOutMutex;
	std::vector<std::string> mOutQueue;

	std::mutex                       mInMutex;
	std::vector<LspResult>           mInQueue;
	std::unordered_map<int, ResultKind> mPending;   // request id -> kind (guarded by mInMutex)

	std::mutex                       mDocMutex;
	std::unordered_map<std::string, int> mDocVersions;
	std::unordered_set<std::string>  mOpenDocs;
};

} // namespace lsp
