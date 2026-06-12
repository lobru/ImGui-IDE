//	gh_parse.h - pure GitHub-API response parsing + small utilities
//	Part of ImGui-IDE (github.com/lobotomy-x/ImGuiColorTextEdit)
//
//	Copyright (c) 2024-2026 Johan A. Goossens, Logan Brunet. All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.
//
//	Everything in here is transport-free (no fetch, no emscripten) so it can
//	be unit-tested natively - see tests/gh_parse_test.cpp. The wasm shell
//	(ide_main.cpp) and the fetch transport (gh_client.h) sit on top.

#ifndef GH_PARSE_H
#define GH_PARSE_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "texteditor_embed.h"

namespace ghp {

//
//	models
//

struct TreeEntry {
	std::string path;  // repo-relative, e.g. "example/editor.cpp"
	std::string sha;   // blob/tree sha
	bool isDir = false;
	int64_t size = 0;  // blobs only
};

struct SearchHit {
	std::string path;
	std::string fragment; // first text-match fragment, may be empty
};

//
//	parsing (all tolerate malformed input by returning empty results)
//

//	GET /repos/{o}/{r}/branches -> branch names
inline std::vector<std::string> parseBranches(const std::string& body) {
	std::vector<std::string> out;
	auto j = nlohmann::json::parse(body, nullptr, false);

	if (j.is_array()) {
		for (auto& b : j) {
			if (b.contains("name") && b["name"].is_string()) {
				out.push_back(b["name"].get<std::string>());
			}
		}
	}

	return out;
}

//	GET /repos/{o}/{r}/git/trees/{ref}?recursive=1 -> entries (+truncated flag)
inline std::vector<TreeEntry> parseTree(const std::string& body, bool* truncated = nullptr) {
	std::vector<TreeEntry> out;
	auto j = nlohmann::json::parse(body, nullptr, false);

	if (truncated) {
		*truncated = j.is_object() && j.value("truncated", false);
	}

	if (j.is_object() && j.contains("tree") && j["tree"].is_array()) {
		for (auto& e : j["tree"]) {
			if (!e.contains("path") || !e["path"].is_string()) {
				continue;
			}

			TreeEntry t;
			t.path = e["path"].get<std::string>();
			t.sha = e.value("sha", "");
			t.isDir = e.value("type", "") == "tree";
			t.size = e.value("size", (int64_t) 0);
			out.push_back(std::move(t));
		}
	}

	return out;
}

//	PUT /repos/{o}/{r}/contents/{path} response -> new blob sha ("" on failure)
inline std::string parsePutResponse(const std::string& body) {
	auto j = nlohmann::json::parse(body, nullptr, false);

	if (j.is_object() && j.contains("content") && j["content"].is_object()) {
		return j["content"].value("sha", "");
	}

	return "";
}

//	GET /search/code response (text-match media type) -> hits
inline std::vector<SearchHit> parseSearchResults(const std::string& body) {
	std::vector<SearchHit> out;
	auto j = nlohmann::json::parse(body, nullptr, false);

	if (j.is_object() && j.contains("items") && j["items"].is_array()) {
		for (auto& it : j["items"]) {
			SearchHit h;
			h.path = it.value("path", "");

			if (it.contains("text_matches") && it["text_matches"].is_array() &&
				!it["text_matches"].empty()) {
				h.fragment = it["text_matches"][0].value("fragment", "");
			}

			if (!h.path.empty()) {
				out.push_back(std::move(h));
			}
		}
	}

	return out;
}

//	error responses: {"message": "..."} -> human-readable ("" if none)
inline std::string parseErrorMessage(const std::string& body) {
	auto j = nlohmann::json::parse(body, nullptr, false);

	if (j.is_object() && j.contains("message") && j["message"].is_string()) {
		return j["message"].get<std::string>();
	}

	return "";
}

//
//	encoding utilities
//

inline std::string base64Encode(const std::string& in) {
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((in.size() + 2) / 3 * 4);
	size_t i = 0;

	while (i + 2 < in.size()) {
		uint32_t v = (uint8_t) in[i] << 16 | (uint8_t) in[i + 1] << 8 | (uint8_t) in[i + 2];
		out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
		i += 3;
	}

	if (i + 1 == in.size()) {
		uint32_t v = (uint8_t) in[i] << 16;
		out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += "==";
	} else if (i + 2 == in.size()) {
		uint32_t v = (uint8_t) in[i] << 16 | (uint8_t) in[i + 1] << 8;
		out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
	}

	return out;
}

inline std::string base64Decode(const std::string& in) {
	auto val = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') { return c - 'A'; }
		if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
		if (c >= '0' && c <= '9') { return c - '0' + 52; }
		if (c == '+') { return 62; }
		if (c == '/') { return 63; }
		return -1; // padding, whitespace, anything else
	};

	std::string out;
	out.reserve(in.size() / 4 * 3);
	uint32_t acc = 0;
	int bits = 0;

	for (char c : in) {
		int v = val(c);

		if (v < 0) {
			continue;
		}

		acc = acc << 6 | (uint32_t) v;
		bits += 6;

		if (bits >= 8) {
			bits -= 8;
			out += (char) ((acc >> bits) & 0xFF);
		}
	}

	return out;
}

inline std::string urlEncode(const std::string& in) {
	static const char* hex = "0123456789ABCDEF";
	std::string out;

	for (unsigned char c : in) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~') {
			out += (char) c;
		} else {
			out += '%'; out += hex[c >> 4]; out += hex[c & 15];
		}
	}

	return out;
}

//
//	extension -> editor language
//

inline te_language languageForPath(const std::string& path) {
	auto dot = path.find_last_of('.');

	if (dot == std::string::npos) {
		return TE_LANG_NONE;
	}

	std::string ext = path.substr(dot + 1);

	for (auto& c : ext) {
		c = (char) ((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
	}

	if (ext == "c") { return TE_LANG_C; }
	if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "h" || ext == "hpp" || ext == "inl") { return TE_LANG_CPP; }
	if (ext == "cs") { return TE_LANG_CSHARP; }
	if (ext == "as") { return TE_LANG_ANGELSCRIPT; }
	if (ext == "lua") { return TE_LANG_LUA; }
	if (ext == "py") { return TE_LANG_PYTHON; }
	if (ext == "glsl" || ext == "vert" || ext == "frag" || ext == "comp") { return TE_LANG_GLSL; }
	if (ext == "hlsl" || ext == "fx" || ext == "fxh" || ext == "usf") { return TE_LANG_HLSL; }
	if (ext == "json") { return TE_LANG_JSON; }
	if (ext == "md" || ext == "markdown") { return TE_LANG_MARKDOWN; }
	if (ext == "sql") { return TE_LANG_SQL; }
	if (ext == "ini" || ext == "cfg" || ext == "toml") { return TE_LANG_INI; }
	return TE_LANG_NONE;
}

//
//	tree model: flat sorted entries -> renderable hierarchy
//

struct TreeNode {
	std::string name;          // last path component
	std::string path;          // full repo path
	std::string sha;
	bool isDir = false;
	std::vector<int> children; // indices into the node pool
};

//	Build a node pool from the flat recursive tree listing. Node 0 is the
//	synthetic root. Children are sorted directories-first, then by name.
inline std::vector<TreeNode> buildTreeModel(std::vector<TreeEntry> entries) {
	std::vector<TreeNode> nodes;
	nodes.push_back(TreeNode{"", "", "", true, {}});

	// parents come before children when sorted by path
	std::sort(entries.begin(), entries.end(),
		[](const TreeEntry& a, const TreeEntry& b) { return a.path < b.path; });

	// path -> node index for directories
	std::unordered_map<std::string, int> dirIndex;
	dirIndex[""] = 0;

	for (auto& e : entries) {
		auto slash = e.path.find_last_of('/');
		std::string parent = slash == std::string::npos ? "" : e.path.substr(0, slash);
		std::string name = slash == std::string::npos ? e.path : e.path.substr(slash + 1);
		auto p = dirIndex.find(parent);

		if (p == dirIndex.end()) {
			continue; // orphan (parent filtered/truncated) - skip
		}

		nodes.push_back(TreeNode{name, e.path, e.sha, e.isDir, {}});
		int idx = (int) nodes.size() - 1;
		nodes[(size_t) p->second].children.push_back(idx);

		if (e.isDir) {
			dirIndex[e.path] = idx;
		}
	}

	// directories first, then case-insensitive by name
	for (auto& n : nodes) {
		std::sort(n.children.begin(), n.children.end(), [&](int a, int b) {
			const TreeNode& A = nodes[(size_t) a];
			const TreeNode& B = nodes[(size_t) b];

			if (A.isDir != B.isDir) {
				return A.isDir;
			}

			std::string an = A.name, bn = B.name;

			for (auto& c : an) { c = (char) std::tolower((unsigned char) c); }
			for (auto& c : bn) { c = (char) std::tolower((unsigned char) c); }

			return an < bn;
		});
	}

	return nodes;
}

} // namespace ghp

#endif // GH_PARSE_H
