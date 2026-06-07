//	LSP protocol layer — implementation. Pure: only nlohmann/json + std.

#include "lsp_protocol.h"

#include <cstdio>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace lsp {

// ── UTF offset conversion ────────────────────────────────────────────────────

uint32_t utf8ByteToUtf16(const std::string& line, uint32_t byteOffset)
{
	uint32_t units = 0;
	uint32_t i = 0;
	uint32_t n = (uint32_t) line.size();
	if (byteOffset > n) byteOffset = n;
	while (i < byteOffset)
	{
		unsigned char c = (unsigned char) line[i];
		uint32_t len = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
		if (i + len > byteOffset) break;   // don't split a multibyte char straddling the offset
		units += (len == 4) ? 2 : 1;       // astral chars are a surrogate pair (2 UTF-16 units)
		i += len;
	}
	return units;
}

uint32_t utf16ToUtf8Byte(const std::string& line, uint32_t utf16Offset)
{
	uint32_t units = 0;
	uint32_t i = 0;
	uint32_t n = (uint32_t) line.size();
	while (i < n && units < utf16Offset)
	{
		unsigned char c = (unsigned char) line[i];
		uint32_t len = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
		if (i + len > n) break;
		units += (len == 4) ? 2 : 1;
		i += len;
	}
	return i;
}

// ── framing ──────────────────────────────────────────────────────────────────

std::string frameMessage(const std::string& body)
{
	return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

bool parseFrame(const std::string& buf, std::size_t& pos, std::string& body)
{
	// Locate the header terminator.
	std::size_t headerEnd = buf.find("\r\n\r\n", pos);
	if (headerEnd == std::string::npos)
		return false;   // headers not fully arrived yet

	// Parse header lines for Content-Length (ignore any others, e.g. Content-Type).
	long long contentLen = -1;
	std::size_t lineStart = pos;
	while (lineStart < headerEnd)
	{
		std::size_t lineEnd = buf.find("\r\n", lineStart);
		if (lineEnd == std::string::npos || lineEnd > headerEnd) lineEnd = headerEnd;
		std::string h = buf.substr(lineStart, lineEnd - lineStart);
		std::size_t colon = h.find(':');
		if (colon != std::string::npos)
		{
			std::string key = h.substr(0, colon);
			std::string val = h.substr(colon + 1);
			// trim
			while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
			while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) val.pop_back();
			if (key == "Content-Length")
			{
				try { contentLen = std::stoll(val); } catch (...) { contentLen = -1; }
			}
		}
		lineStart = lineEnd + 2;
	}
	if (contentLen < 0)
		return false;   // malformed; caller can't recover this frame, but treat as partial

	std::size_t bodyStart = headerEnd + 4;
	if (buf.size() < bodyStart + (std::size_t) contentLen)
		return false;   // body not fully arrived yet

	body = buf.substr(bodyStart, (std::size_t) contentLen);
	pos = bodyStart + (std::size_t) contentLen;
	return true;
}

// ── path <-> uri ─────────────────────────────────────────────────────────────

static bool isUnreserved(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		   c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
}

std::string pathToUri(const std::string& absPath)
{
	std::string p = absPath;
	for (auto& c : p) if (c == '\\') c = '/';   // normalize separators
	// percent-encode everything except unreserved + '/'
	std::string enc;
	for (unsigned char c : p)
	{
		// ':' is kept unencoded so the drive letter reads as the conventional
		// file:///C:/… (clangd accepts both, but this is the canonical form).
		if (isUnreserved(c) || c == ':') enc += (char) c;
		else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); enc += b; }
	}
	// file:///C:/...  (leading slash before the drive)
	if (!enc.empty() && enc[0] != '/') enc = "/" + enc;
	return "file://" + enc;
}

std::string uriToPath(const std::string& uri)
{
	std::string s = uri;
	const std::string scheme = "file://";
	if (s.rfind(scheme, 0) == 0) s = s.substr(scheme.size());
	// percent-decode
	std::string dec;
	for (std::size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '%' && i + 2 < s.size())
		{
			auto hex = [](char h) -> int {
				if (h >= '0' && h <= '9') return h - '0';
				if (h >= 'a' && h <= 'f') return h - 'a' + 10;
				if (h >= 'A' && h <= 'F') return h - 'A' + 10;
				return -1;
			};
			int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
			if (hi >= 0 && lo >= 0) { dec += (char) ((hi << 4) | lo); i += 2; continue; }
		}
		dec += s[i];
	}
	// /C:/...  ->  C:/...
	if (dec.size() >= 3 && dec[0] == '/' && dec[2] == ':')
		dec = dec.substr(1);
	return dec;
}

// ── builders ─────────────────────────────────────────────────────────────────

static std::string rpc(const json& j) { return frameMessage(j.dump()); }

std::string buildInitialize(int id, const std::string& rootUri, long long processId)
{
	json j = {
		{"jsonrpc", "2.0"}, {"id", id}, {"method", "initialize"},
		{"params", {
			{"processId", processId},
			{"rootUri", rootUri},
			{"capabilities", {
				// clangd extension: prefer utf-8 so we can pass byte offsets directly.
				{"offsetEncoding", {"utf-8", "utf-16"}},
				{"textDocument", {
					{"synchronization", {{"didSave", false}, {"dynamicRegistration", false}}},
					{"completion", {{"completionItem", {{"snippetSupport", false}}}}},
					{"definition", {{"dynamicRegistration", false}}},
				}},
			}},
		}},
	};
	return rpc(j);
}

std::string buildInitialized()
{
	return rpc({{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", json::object()}});
}

std::string buildDidOpen(const std::string& uri, const std::string& langId, int version, const std::string& text)
{
	return rpc({{"jsonrpc", "2.0"}, {"method", "textDocument/didOpen"}, {"params", {
		{"textDocument", {{"uri", uri}, {"languageId", langId}, {"version", version}, {"text", text}}}}}});
}

std::string buildDidChange(const std::string& uri, int version, const std::string& text)
{
	return rpc({{"jsonrpc", "2.0"}, {"method", "textDocument/didChange"}, {"params", {
		{"textDocument", {{"uri", uri}, {"version", version}}},
		{"contentChanges", json::array({ json{{"text", text}} })}}}});
}

std::string buildDidClose(const std::string& uri)
{
	return rpc({{"jsonrpc", "2.0"}, {"method", "textDocument/didClose"}, {"params", {
		{"textDocument", {{"uri", uri}}}}}});
}

std::string buildCompletion(int id, const std::string& uri, int line, int character)
{
	return rpc({{"jsonrpc", "2.0"}, {"id", id}, {"method", "textDocument/completion"}, {"params", {
		{"textDocument", {{"uri", uri}}},
		{"position", {{"line", line}, {"character", character}}}}}});
}

std::string buildDefinition(int id, const std::string& uri, int line, int character)
{
	return rpc({{"jsonrpc", "2.0"}, {"id", id}, {"method", "textDocument/definition"}, {"params", {
		{"textDocument", {{"uri", uri}}},
		{"position", {{"line", line}, {"character", character}}}}}});
}

std::string buildShutdown(int id)
{
	return rpc({{"jsonrpc", "2.0"}, {"id", id}, {"method", "shutdown"}});
}

std::string buildExit()
{
	return rpc({{"jsonrpc", "2.0"}, {"method", "exit"}});
}

std::string buildNullResponse(long long id)
{
	return rpc({{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}});
}

// ── parsers ──────────────────────────────────────────────────────────────────

static json parseBody(const std::string& body)
{
	return json::parse(body, nullptr, /*allow_exceptions*/ false);
}

std::vector<CompletionItem> parseCompletion(const std::string& body)
{
	std::vector<CompletionItem> out;
	json j = parseBody(body);
	if (j.is_discarded() || !j.contains("result")) return out;
	const json& result = j["result"];
	const json* items = nullptr;
	if (result.is_array()) items = &result;
	else if (result.is_object() && result.contains("items") && result["items"].is_array()) items = &result["items"];
	if (!items) return out;
	for (const auto& it : *items)
	{
		if (!it.is_object()) continue;
		CompletionItem ci;
		ci.label = it.value("label", std::string());
		ci.detail = it.value("detail", std::string());
		ci.kind = it.value("kind", 0);
		ci.sortText = it.value("sortText", std::string());
		// insertText preference: textEdit.newText -> insertText -> label (trimmed).
		if (it.contains("textEdit") && it["textEdit"].is_object())
			ci.insertText = it["textEdit"].value("newText", std::string());
		if (ci.insertText.empty()) ci.insertText = it.value("insertText", std::string());
		if (ci.insertText.empty())
		{
			ci.insertText = ci.label;
			while (!ci.insertText.empty() && ci.insertText.front() == ' ') ci.insertText.erase(ci.insertText.begin());
		}
		if (!ci.label.empty() || !ci.insertText.empty()) out.push_back(std::move(ci));
	}
	return out;
}

static void pushLoc(std::vector<Location>& out, const json& loc)
{
	if (!loc.is_object()) return;
	Location l;
	if (loc.contains("uri")) { l.uri = loc.value("uri", std::string());
		if (loc.contains("range") && loc["range"].is_object() && loc["range"].contains("start"))
		{ l.line = loc["range"]["start"].value("line", 0); l.character = loc["range"]["start"].value("character", 0); }
	}
	else if (loc.contains("targetUri")) { l.uri = loc.value("targetUri", std::string());
		const char* rk = loc.contains("targetSelectionRange") ? "targetSelectionRange" : "targetRange";
		if (loc.contains(rk) && loc[rk].is_object() && loc[rk].contains("start"))
		{ l.line = loc[rk]["start"].value("line", 0); l.character = loc[rk]["start"].value("character", 0); }
	}
	if (!l.uri.empty()) out.push_back(std::move(l));
}

std::vector<Location> parseDefinition(const std::string& body)
{
	std::vector<Location> out;
	json j = parseBody(body);
	if (j.is_discarded() || !j.contains("result")) return out;
	const json& result = j["result"];
	if (result.is_array()) for (const auto& e : result) pushLoc(out, e);
	else if (result.is_object()) pushLoc(out, result);
	return out;
}

bool parseInitializeResult(const std::string& body, std::string& offsetEncoding)
{
	offsetEncoding = "utf-16";   // LSP spec default when the server omits it
	json j = parseBody(body);
	if (j.is_discarded() || !j.contains("result") || !j["result"].is_object()) return false;
	const json& r = j["result"];
	if (r.contains("offsetEncoding") && r["offsetEncoding"].is_string())
		offsetEncoding = r["offsetEncoding"].get<std::string>();
	else if (r.contains("capabilities") && r["capabilities"].is_object() &&
			 r["capabilities"].contains("offsetEncoding") && r["capabilities"]["offsetEncoding"].is_string())
		offsetEncoding = r["capabilities"]["offsetEncoding"].get<std::string>();
	return true;
}

Incoming inspect(const std::string& body)
{
	Incoming in;
	json j = parseBody(body);
	if (j.is_discarded()) return in;
	if (j.contains("id") && (j["id"].is_number_integer() || j["id"].is_number_unsigned()))
	{
		in.hasId = true;
		in.id = j["id"].get<long long>();
	}
	if (j.contains("method") && j["method"].is_string())
		in.method = j["method"].get<std::string>();
	in.isError = j.contains("error");
	return in;
}

} // namespace lsp
