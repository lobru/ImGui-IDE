//	LSP protocol layer — PURE (no SDL, no threads). JSON-RPC framing, message
//	builders/parsers, and UTF offset conversion. Selftest-covered; the transport
//	(process + threads) lives in lsp_client.{h,cpp}.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lsp {

// ── UTF offset conversion ────────────────────────────────────────────────────
// LSP positions count UTF-16 code units. The editor stores UTF-8 lines and works
// in byte / codepoint offsets, so conversions are explicit (a 4-byte UTF-8 char
// is one codepoint but TWO UTF-16 code units). Both clamp to the line length.
uint32_t utf8ByteToUtf16(const std::string& line, uint32_t byteOffset);
uint32_t utf16ToUtf8Byte(const std::string& line, uint32_t utf16Offset);

// ── JSON-RPC framing ─────────────────────────────────────────────────────────
// Wrap a JSON body in a "Content-Length: N\r\n\r\n<body>" frame.
std::string frameMessage(const std::string& body);
// Parse ONE complete frame from `buf` starting at `pos`. On success: returns true,
// sets `body`, advances `pos` past the frame. On a partial (incomplete) frame:
// returns false and leaves `pos` unchanged. Tolerates multiple/unknown headers.
bool parseFrame(const std::string& buf, std::size_t& pos, std::string& body);

// ── path <-> file URI (Windows aware, percent-decoding) ──────────────────────
std::string pathToUri(const std::string& absPath);
std::string uriToPath(const std::string& uri);

// ── message builders (return framed, ready-to-send bytes) ────────────────────
std::string buildInitialize(int id, const std::string& rootUri, long long processId);
std::string buildInitialized();
std::string buildDidOpen(const std::string& uri, const std::string& langId, int version, const std::string& text);
std::string buildDidChange(const std::string& uri, int version, const std::string& text);
std::string buildDidClose(const std::string& uri);
std::string buildCompletion(int id, const std::string& uri, int line, int character);
std::string buildDefinition(int id, const std::string& uri, int line, int character);
std::string buildHover(int id, const std::string& uri, int line, int character);
std::string buildShutdown(int id);
std::string buildExit();
// Reply to a server->client request we don't implement, so clangd doesn't stall.
std::string buildNullResponse(long long id);

// ── results ──────────────────────────────────────────────────────────────────
struct CompletionItem {
	std::string label;       // display text
	std::string insertText;  // text actually inserted (insertText / textEdit / label)
	std::string detail;      // type / signature
	int         kind = 0;    // LSP CompletionItemKind
	std::string sortText;
};
struct Location {
	std::string uri;
	int         line = 0;        // 0-based
	int         character = 0;   // in negotiated encoding
};

// Parse a response body (the full JSON-RPC message). Empty vector on error/empty.
std::vector<CompletionItem> parseCompletion(const std::string& body);
std::vector<Location>       parseDefinition(const std::string& body);
// Hover contents flattened to plain text (string / MarkupContent / MarkedString[]).
std::string                 parseHover(const std::string& body);
// Extract the negotiated offsetEncoding from an initialize result; defaults to
// "utf-16" (the LSP spec default) when the server omits it.
bool parseInitializeResult(const std::string& body, std::string& offsetEncoding);

// ── inbound routing ──────────────────────────────────────────────────────────
// A decoded incoming message: a response (hasId, no method), a notification
// (method, no id), or a server->client request (method AND id -> needs a reply).
struct Incoming {
	bool        hasId = false;
	long long   id = 0;
	std::string method;   // empty for responses
	bool        isError = false;
};
Incoming inspect(const std::string& body);

} // namespace lsp
