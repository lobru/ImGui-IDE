//	gh_parse_test.cpp - native unit test for the web IDE's GitHub parse layer
//	Part of ImGui-IDE (github.com/lobotomy-x/ImGuiColorTextEdit)
//
//	Copyright (c) 2024-2026 Johan A. Goossens, Logan Brunet. All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.
//
//	gh_parse.h is transport-free, so the response parsing, base64, and tree
//	model used by the wasm IDE shell are verified here, natively, in CI.

#include <cstdio>
#include <string>

#include "../hosts/web/gh_parse.h"

static int failures = 0;

#define CHECK(cond, ...) \
	do { \
		if (!(cond)) { \
			failures++; \
			std::printf("FAIL %s:%d: %s\n      ", __FILE__, __LINE__, #cond); \
			std::printf(__VA_ARGS__); \
			std::printf("\n"); \
		} \
	} while (0)

int main() {
	using namespace ghp;

	// ── base64 round trip, including binary + padding variants ───────
	for (const std::string& s : {std::string(""), std::string("a"), std::string("ab"),
	                             std::string("abc"), std::string("hello\nworld\0x", 13)}) {
		CHECK(base64Decode(base64Encode(s)) == s, "b64 roundtrip len %zu", s.size());
	}

	CHECK(base64Encode("Man") == "TWFu", "b64 known vector: %s", base64Encode("Man").c_str());
	CHECK(base64Decode("TWFu\nTWFu") == "ManMan", "b64 ignores newlines (GitHub wraps them)");

	// ── url encoding ─────────────────────────────────────────────────
	CHECK(urlEncode("a b+c/d") == "a%20b%2Bc%2Fd", "got %s", urlEncode("a b+c/d").c_str());

	// ── branches ─────────────────────────────────────────────────────
	auto branches = parseBranches(R"([{"name":"main"},{"name":"future"},{"bogus":1}])");
	CHECK(branches.size() == 2 && branches[0] == "main", "parsed %zu branches", branches.size());
	CHECK(parseBranches("not json").empty(), "malformed branches tolerated");

	// ── tree + model ─────────────────────────────────────────────────
	const char* treeJson = R"({
		"truncated": false,
		"tree": [
			{"path":"src","type":"tree","sha":"d1"},
			{"path":"README.md","type":"blob","sha":"b1","size":10},
			{"path":"src/main.cpp","type":"blob","sha":"b2","size":2048},
			{"path":"src/util","type":"tree","sha":"d2"},
			{"path":"src/util/x.h","type":"blob","sha":"b3","size":5}
		]})";
	bool truncated = true;
	auto entries = parseTree(treeJson, &truncated);
	CHECK(entries.size() == 5 && !truncated, "parsed %zu entries", entries.size());

	auto nodes = buildTreeModel(entries);
	CHECK(nodes.size() == 6, "node pool %zu (root + 5)", nodes.size());
	// root: dirs first => src before README.md
	CHECK(nodes[0].children.size() == 2, "root has %zu children", nodes[0].children.size());
	const TreeNode& first = nodes[(size_t) nodes[0].children[0]];
	CHECK(first.name == "src" && first.isDir, "first root child is %s", first.name.c_str());
	// src has main.cpp and util; util (dir) sorts first
	const TreeNode& src = first;
	CHECK(src.children.size() == 2, "src has %zu children", src.children.size());
	CHECK(nodes[(size_t) src.children[0]].name == "util", "src first child %s",
		nodes[(size_t) src.children[0]].name.c_str());
	// nested file resolved with full path
	const TreeNode& util = nodes[(size_t) src.children[0]];
	CHECK(util.children.size() == 1 && nodes[(size_t) util.children[0]].path == "src/util/x.h",
		"nested path %s", nodes[(size_t) util.children[0]].path.c_str());

	// ── put response / error ─────────────────────────────────────────
	CHECK(parsePutResponse(R"({"content":{"sha":"abc123"},"commit":{}})") == "abc123",
		"put sha parse");
	CHECK(parsePutResponse(R"({"message":"Conflict"})").empty(), "put conflict -> empty sha");
	CHECK(parseErrorMessage(R"({"message":"Bad credentials"})") == "Bad credentials",
		"error message parse");

	// ── search ───────────────────────────────────────────────────────
	auto hits = parseSearchResults(R"gh({
		"total_count": 1,
		"items": [{"path":"src/main.cpp",
		           "text_matches":[{"fragment":"int main()"}]}]})gh");
	CHECK(hits.size() == 1 && hits[0].path == "src/main.cpp" && hits[0].fragment == "int main()",
		"search hit parse");

	// ── language mapping ─────────────────────────────────────────────
	CHECK(languageForPath("a/b/editor.cpp") == TE_LANG_CPP, "cpp");
	CHECK(languageForPath("shader.FXH") == TE_LANG_HLSL, "case-insensitive ext");
	CHECK(languageForPath("script.lua") == TE_LANG_LUA, "lua");
	CHECK(languageForPath("Makefile") == TE_LANG_NONE, "no extension");

	if (failures == 0) {
		std::printf("gh_parse_test: all checks passed\n");
	}

	return failures == 0 ? 0 : 1;
}
