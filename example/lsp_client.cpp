//	LSP client transport — implementation. SDL3 process + a reader thread.

#include "lsp_client.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_timer.h>

namespace lsp {

static SDL_IOStream* asIO(void* p) { return reinterpret_cast<SDL_IOStream*>(p); }

LspClient::~LspClient()
{
	stop();
}

bool LspClient::start(const std::string& serverPath, const std::string& rootUri)
{
	if (mProcess)
		return true;
	const char* args[] = { serverPath.c_str(), nullptr };
	mProcess = SDL_CreateProcess(args, /*pipe_stdio*/ true);
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
	mReader = std::thread([this] { readerLoop(); });
	// Fire initialize; its reply (in dispatch) queues 'initialized' + flips ready.
	enqueueOut(buildInitialize(mInitializeId, rootUri, 0));
	flushOut();
	return true;
}

void LspClient::stop()
{
	if (mStopped.exchange(true))
		return;
	if (mProcess)
	{
		// Graceful: ask the server to shut down + exit, flush, then let stdout EOF
		// (or mShuttingDown) end the reader. A force-kill backs this up in teardown.
		enqueueOut(buildShutdown(mNextId++));
		enqueueOut(buildExit());
		flushOut();
	}
	teardown();
}

void LspClient::teardown()
{
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
}

void LspClient::enqueueOut(const std::string& framed)
{
	std::lock_guard<std::mutex> lk(mOutMutex);
	mOutQueue.push_back(framed);
}

void LspClient::flushOut()
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

void LspClient::readerLoop()
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
			while (parseFrame(buf, pos, body))
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
	// Deliver a one-shot "server gone" sentinel so the UI can react (and re-detect).
	std::lock_guard<std::mutex> lk(mInMutex);
	LspResult s;
	s.serverGone = true;
	mInQueue.push_back(std::move(s));
}

void LspClient::dispatch(const std::string& body)
{
	Incoming in = inspect(body);

	if (in.hasId && in.method.empty())
	{
		// Response to one of our requests.
		if ((int) in.id == mInitializeId)
		{
			std::string enc;
			parseInitializeResult(body, enc);
			mEncoding = enc;                 // published before mReady (release) below
			enqueueOut(buildInitialized());  // reader only ENQUEUES; UI flushes
			mReady.store(true);
			return;
		}
		ResultKind kind = ResultKind::Completion;
		bool known = false;
		{
			std::lock_guard<std::mutex> lk(mInMutex);
			auto it = mPending.find((int) in.id);
			if (it != mPending.end()) { kind = it->second; mPending.erase(it); known = true; }
		}
		if (!known)
			return;
		LspResult res;
		res.id = (int) in.id;
		res.kind = kind;
		if (!in.isError)
		{
			if (kind == ResultKind::Completion)      res.completionItems = parseCompletion(body);
			else if (kind == ResultKind::Definition) res.locations = parseDefinition(body);
			else                                     res.hoverText = parseHover(body);
		}
		std::lock_guard<std::mutex> lk(mInMutex);
		mInQueue.push_back(std::move(res));
		return;
	}

	if (in.hasId && !in.method.empty())
	{
		// Server->client request we don't implement — reply null so clangd
		// doesn't block waiting on us (registerCapability, workDoneProgress/create…).
		enqueueOut(buildNullResponse(in.id));
		return;
	}
	// Notification (diagnostics, progress, log) — ignored in this slice.
}

void LspClient::didOpen(const std::string& uri, const std::string& langId, const std::string& text)
{
	if (!mReady.load())
		return;
	{
		std::lock_guard<std::mutex> lk(mDocMutex);
		mDocVersions[uri] = 1;
		mOpenDocs.insert(uri);
	}
	enqueueOut(buildDidOpen(uri, langId, 1, text));
	flushOut();
}

void LspClient::didChange(const std::string& uri, const std::string& text)
{
	int v;
	{
		std::lock_guard<std::mutex> lk(mDocMutex);
		auto it = mDocVersions.find(uri);
		if (it == mDocVersions.end())
			return;   // never opened — caller should didOpen first
		v = ++it->second;
	}
	enqueueOut(buildDidChange(uri, v, text));
	flushOut();
}

void LspClient::didClose(const std::string& uri)
{
	{
		std::lock_guard<std::mutex> lk(mDocMutex);
		if (!mOpenDocs.count(uri))
			return;
		mDocVersions.erase(uri);
		mOpenDocs.erase(uri);
	}
	enqueueOut(buildDidClose(uri));
	flushOut();
}

bool LspClient::isOpen(const std::string& uri)
{
	std::lock_guard<std::mutex> lk(mDocMutex);
	return mOpenDocs.count(uri) != 0;
}

int LspClient::requestCompletion(const std::string& uri, int line, int character)
{
	if (!mReady.load())
		return 0;
	int id = mNextId++;
	{
		std::lock_guard<std::mutex> lk(mInMutex);
		mPending[id] = ResultKind::Completion;
	}
	enqueueOut(buildCompletion(id, uri, line, character));
	flushOut();
	return id;
}

int LspClient::requestDefinition(const std::string& uri, int line, int character)
{
	if (!mReady.load())
		return 0;
	int id = mNextId++;
	{
		std::lock_guard<std::mutex> lk(mInMutex);
		mPending[id] = ResultKind::Definition;
	}
	enqueueOut(buildDefinition(id, uri, line, character));
	flushOut();
	return id;
}

int LspClient::requestHover(const std::string& uri, int line, int character)
{
	if (!mReady.load())
		return 0;
	int id = mNextId++;
	{
		std::lock_guard<std::mutex> lk(mInMutex);
		mPending[id] = ResultKind::Hover;
	}
	enqueueOut(buildHover(id, uri, line, character));
	flushOut();
	return id;
}

std::vector<LspResult> LspClient::poll()
{
	flushOut();
	std::vector<LspResult> out;
	{
		std::lock_guard<std::mutex> lk(mInMutex);
		out.swap(mInQueue);
	}
	return out;
}

} // namespace lsp
