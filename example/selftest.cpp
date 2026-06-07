//	TextEditor selftest — headless logic regression net.
//
//	Exercises the pure, non-UI code paths (colorizer, fold detection, language
//	mapping) with NO ImGui/SDL context. setText() already colorizes + rebuilds
//	folds synchronously, so a bare TextEditor instance is enough. Built as a
//	separate `selftest` executable (see example/CMakeLists.txt) and run in CI /
//	by hand: it sidesteps the GUI entirely, so it works even when example.exe is
//	locked by a running instance.
//
//	Exit code 0 = all pass, 1 = a failure (printed to stderr).

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "TextEditor.h"
#include "tsindex.h"
#include "lsp_protocol.h"
#include "nav_history.h"

static int gFailures = 0;
static int gChecks = 0;

#define CHECK(cond, msg) do { \
		++gChecks; \
		if (!(cond)) { ++gFailures; std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
	} while (0)

// Colorize `src` as `lang` and return the color of the glyph at (line,col).
static int colorAt(const TextEditor::Language* lang, const std::string& src, int line, int col)
{
	TextEditor ed;
	ed.SetLanguage(lang);
	ed.SetText(src);
	return ed.GetGlyphColorAt(line, col);
}

static size_t foldCount(const TextEditor::Language* lang, const std::string& src)
{
	TextEditor ed;
	ed.SetFoldingEnabled(true);
	ed.SetLanguage(lang);
	ed.SetText(src);
	return ed.GetFoldCount();
}

int main()
{
	const int kComment = static_cast<int>(TextEditor::Color::comment);

	// ── Lua comments (regression: block comment opener vs single-line "--") ──
	{
		// A multi-line block comment: every line inside, including the closing
		// ]], must be comment-colored.
		std::string lua =
			"local x = 1\n"
			"--[[ this is\n"
			"a block comment ]]\n"
			"local y = 2\n";
		// line 1 ("--[[ this is") col 0 → comment
		CHECK(colorAt(TextEditor::Language::Lua(), lua, 1, 0) == kComment,
			"Lua: --[[ opens a block comment");
		// line 2 inside the block → comment
		CHECK(colorAt(TextEditor::Language::Lua(), lua, 2, 0) == kComment,
			"Lua: line inside --[[ ]] block is comment");
		// line 0 (code before the comment) → NOT comment
		CHECK(colorAt(TextEditor::Language::Lua(), lua, 0, 0) != kComment,
			"Lua: code before block comment is not comment");
		// line 3 (code after ]]) → NOT comment
		CHECK(colorAt(TextEditor::Language::Lua(), lua, 3, 0) != kComment,
			"Lua: code after ]] is not comment");

		// A plain single-line -- comment still works.
		std::string luaLine = "local z = 3 -- trailing\n";
		CHECK(colorAt(TextEditor::Language::Lua(), luaLine, 0, 12) == kComment,
			"Lua: -- single-line comment");
		CHECK(colorAt(TextEditor::Language::Lua(), luaLine, 0, 0) != kComment,
			"Lua: code before -- is not comment");
	}

	// ── C++ block comment sanity (ensure the reorder didn't break C-style) ──
	{
		std::string cpp =
			"int a = 0;\n"
			"/* block\n"
			"   here */\n"
			"int b = 1;\n";
		CHECK(colorAt(TextEditor::Language::Cpp(), cpp, 1, 0) == kComment,
			"C++: /* opens a block comment");
		CHECK(colorAt(TextEditor::Language::Cpp(), cpp, 2, 0) == kComment,
			"C++: line inside /* */ is comment");
		CHECK(colorAt(TextEditor::Language::Cpp(), cpp, 3, 0) != kComment,
			"C++: code after */ is not comment");
		std::string cline = "int c = 2; // tail\n";
		CHECK(colorAt(TextEditor::Language::Cpp(), cline, 0, 11) == kComment,
			"C++: // single-line comment");
	}

	// ── Folding: INI sections + Lua blocks ──
	{
		std::string ini =
			"[alpha]\n"
			"a=1\n"
			"b=2\n"
			"[beta]\n"
			"c=3\n";
		// Two [section] headers → at least the first folds down to before [beta].
		CHECK(foldCount(TextEditor::Language::Ini(), ini) >= 1,
			"INI: [section] produces a fold");

		std::string luaFn =
			"function f()\n"
			"  return 1\n"
			"end\n";
		CHECK(foldCount(TextEditor::Language::Lua(), luaFn) >= 1,
			"Lua: function ... end produces a fold");
	}

	// ── Comment toggles: single-line uses "--", block uses "--[[ ]]" ──
	{
		// Ctrl+/ on a Lua line should prefix "--", NOT the block opener "--[[".
		TextEditor ed;
		ed.SetLanguage(TextEditor::Language::Lua());
		ed.SetText("local x = 1\n");
		ed.SelectAll();
		ed.ToggleComments();
		std::string out = ed.GetText();
		CHECK(out.rfind("--", 0) == 0, "Lua single-line toggle prefixes --");
		CHECK(out.rfind("--[[", 0) != 0, "Lua single-line toggle does NOT use --[[");
		// Toggling again removes it.
		ed.SelectAll();
		ed.ToggleComments();
		CHECK(ed.GetText().rfind("--", 0) != 0, "Lua single-line toggle is reversible");
	}

	// ── Comment toggle leaves blank lines alone ──
	{
		TextEditor ed;
		ed.SetLanguage(TextEditor::Language::Lua());
		ed.SetText("a = 1\n\nb = 2\n");   // line 1 is blank
		ed.SelectAll();
		ed.ToggleComments();
		// The blank middle line must NOT have gained a "--".
		// GetText lines: "--a = 1", "", "--b = 2"
		std::string out = ed.GetText();
		// crude: the empty line stays empty (no "--\n--" run)
		CHECK(out.find("--\n") == std::string::npos, "toggle does not comment blank lines");
	}

	// ── Folding collapses / restores visible lines (regression net for the
	//    "folding no longer flags the document content-changed" perf fix). ──
	{
		TextEditor ed;
		ed.SetFoldingEnabled(true);
		ed.SetLanguage(TextEditor::Language::Cpp());
		ed.SetText(
			"void f()\n"
			"{\n"
			"    int x = 1;\n"
			"    int y = 2;\n"
			"    int z = 3;\n"
			"}\n");
		int full = ed.getVisualLineCount();
		ed.FoldAll();
		int folded = ed.getVisualLineCount();
		CHECK(folded < full, "FoldAll hides body lines (visible count drops)");
		ed.UnfoldAll();
		CHECK(ed.getVisualLineCount() == full, "UnfoldAll restores all lines");
	}

	// ── C# #region / #endregion folding ──
	{
		std::string cs =
			"#region Helpers\n"
			"int Add(int a, int b) { return a + b; }\n"
			"int Sub(int a, int b) { return a - b; }\n"
			"#endregion\n";
		CHECK(foldCount(TextEditor::Language::Cs(), cs) >= 1,
			"C#: #region/#endregion produces a fold");
	}

	// ── Backslash-continued #define: directive line is preprocessor, but the
	//    continued macro BODY is highlighted as code (not blobbed preprocessor) ──
	{
		const int kPre = static_cast<int>(TextEditor::Color::preprocessor);
		const int kKeyword = static_cast<int>(TextEditor::Color::keyword);
		std::string cpp =
			"#define FOO(x) \\\n"        // line 0: directive, ends with backslash
			"    do { x; } while (0)\n"  // line 1: continuation — col 4 = 'do' keyword
			"int after = 1;\n";          // line 2: back to normal code
		CHECK(colorAt(TextEditor::Language::Cpp(), cpp, 0, 0) == kPre,
			"#define directive line is preprocessor");
		CHECK(colorAt(TextEditor::Language::Cpp(), cpp, 1, 4) == kKeyword,
			"backslash-continued macro body is highlighted as code (do = keyword)");
		CHECK(colorAt(TextEditor::Language::Cpp(), cpp, 2, 0) != kPre,
			"line after the continuation is back to normal code");
	}

	// ── Tree-sitter symbol extraction (go-to-def/decl foundation) ──
	{
		std::string cpp =
			"namespace ns {\n"                              // 0
			"class Foo {\n"                                 // 1
			"public:\n"                                     // 2
			"    int width;\n"                              // 3  field
			"    int bar(int x);\n"                         // 4  method decl
			"    class Inner {\n"                           // 5  nested type
			"        void m();\n"                           // 6  method in nested
			"    };\n"                                       // 7
			"};\n"                                           // 8
			"}\n"                                            // 9
			"int ns::Foo::bar(int x) { return baz(x); }\n"  // 10 out-of-line def
			"void baz() {}\n";                              // 11 top-level
		auto syms = ts::extractSymbols(ts::Lang::Cpp, cpp);

		std::fprintf(stderr, "[ts] available=%d  symbols=%zu\n", (int) ts::available(), syms.size());
		for (auto& s : syms)
			std::fprintf(stderr, "[ts]   %-10s kind=%d def=%d encl=%-6s @%d:%d\n",
				s.name.c_str(), (int) s.kind, (int) s.isDefinition,
				s.enclosingType.empty() ? "-" : s.enclosingType.c_str(), s.line, s.column);

		// enclosingType of the symbol matching name(+line); "?" if not found.
		auto encl = [&](const char* name, int line) -> std::string {
			for (auto& s : syms) if (s.name == name && (line < 0 || s.line == line)) return s.enclosingType;
			return "?";
		};
		bool foo = false, baz = false, inner = false, barDecl = false, barDef = false;
		for (auto& s : syms) {
			if (s.name == "Foo")   foo = true;
			if (s.name == "baz")   baz = true;
			if (s.name == "Inner") inner = true;
			if (s.name == "bar" && s.line == 4)  barDecl = true;
			if (s.name == "bar" && s.line == 10) barDef = true;
		}
		CHECK(ts::available(), "tree-sitter C++ grammar available");
		CHECK(foo && baz && inner, "tree-sitter finds Foo, baz, Inner");
		CHECK(barDecl, "tree-sitter finds the in-class bar declaration");
		CHECK(barDef, "tree-sitter finds the out-of-line bar definition");
		CHECK(encl("width", 3) == "Foo", "field width enclosed by Foo");
		CHECK(encl("bar", 4) == "Foo", "in-class bar enclosed by Foo");
		CHECK(encl("bar", 10) == "Foo", "out-of-line bar enclosed by Foo (qualified)");
		CHECK(encl("m", 6) == "Inner", "nested m enclosed by tightest type Inner");
		CHECK(encl("baz", 11).empty(), "top-level baz has no enclosing type");
		CHECK(encl("Inner", 5) == "Foo", "nested type Inner's enclosing is outer Foo (self-skip)");
		// A class directly in a namespace reports that namespace as its enclosing
		// scope, so `ns::` member completion can list the namespace's types.
		CHECK(encl("Foo", 1) == "ns", "type Foo enclosed by its namespace ns");
	}

	// ── Tree-sitter C# symbol extraction ──
	{
		std::string cs =
			"namespace App {\n"
			"  class Widget {\n"
			"    public int Width;\n"
			"    public int Area() { return Width * Width; }\n"
			"  }\n"
			"  interface IThing { void Do(); }\n"
			"}\n";
		auto syms = ts::extractSymbols(ts::Lang::CSharp, cs);
		std::fprintf(stderr, "[ts:cs] symbols=%zu\n", syms.size());
		for (auto& s : syms)
			std::fprintf(stderr, "[ts:cs]   %-10s kind=%d def=%d encl=%-6s @%d:%d\n",
				s.name.c_str(), (int) s.kind, (int) s.isDefinition,
				s.enclosingType.empty() ? "-" : s.enclosingType.c_str(), s.line, s.column);

		auto encl = [&](const char* name) -> std::string {
			for (auto& s : syms) if (s.name == name) return s.enclosingType;
			return "?";
		};
		bool widget = false, area = false, ithing = false, width = false;
		for (auto& s : syms) {
			if (s.name == "Widget") widget = true;
			if (s.name == "Area")   area = true;
			if (s.name == "IThing") ithing = true;
			if (s.name == "Width")  width = true;
		}
		CHECK(ts::langForExtension(".cs") == ts::Lang::CSharp, "C# extension maps to the C# grammar");
		CHECK(widget && area && ithing, "tree-sitter finds Widget, Area, IThing");
		CHECK(width, "C# query (augmented) finds the Width field");   // fails without the field augmentation
		CHECK(encl("Area") == "Widget", "C# method Area enclosed by Widget");
		CHECK(encl("Width") == "Widget", "C# field Width enclosed by Widget");
	}

	// ── Curated STL member table ──
	{
		auto has = [](const std::vector<std::string>* v, const char* m) {
			if (!v) return false;
			for (auto& s : *v) if (s == m) return true;
			return false;
		};
		const auto* vec = ts::stlMembers("vector");
		CHECK(vec != nullptr, "stlMembers knows vector");
		CHECK(has(vec, "push_back") && has(vec, "size") && has(vec, "emplace_back"),
			"vector members include push_back/size/emplace_back");
		const auto* str = ts::stlMembers("string");
		CHECK(has(str, "substr") && has(str, "c_str") && has(str, "length"),
			"string members include substr/c_str/length");
		const auto* mp = ts::stlMembers("unordered_map");
		CHECK(has(mp, "find") && has(mp, "emplace") && has(mp, "contains"),
			"unordered_map members include find/emplace/contains");
		const auto* opt = ts::stlMembers("optional");
		CHECK(has(opt, "value") && has(opt, "has_value"), "optional members include value/has_value");
		CHECK(ts::stlMembers("NotAStlType") == nullptr, "unknown type -> no STL members");
	}

	// ── Scope-aware type resolver (the proof that member completion works) ──
	{
		// Byte (row,col) of a marker's first occurrence — avoids hardcoded columns.
		auto pos = [](const std::string& s, const std::string& marker) -> std::pair<int,int> {
			size_t off = s.find(marker);
			if (off == std::string::npos) return {-1, -1};
			int row = 0; size_t lineStart = 0;
			for (size_t i = 0; i < off; ++i) if (s[i] == '\n') { ++row; lineStart = i + 1; }
			return { row, (int)(off - lineStart) };
		};

		// Markers (/*U1*/ etc.) mark cursor sites that are AFTER the relevant
		// declarations, so declaration-before-use is satisfied.
		std::string cpp =
			"struct Widget {\n"
			"    int x;\n"
			"    float speed;\n"
			"    void update(Widget* other) {\n"
			"        int counter = 0;\n"
			"        Widget w;\n"
			"        auto p = new Widget();\n"
			"        auto q = Widget();\n"
			"        std::vector<int> items;\n"
			"        /*U1*/;\n"
			"        {\n"
			"            float counter;\n"
			"            /*U2*/;\n"
			"        }\n"
			"    }\n"
			"};\n"
			"void Widget::update2(Widget* w2) {\n"
			"    /*U3*/;\n"
			"}\n";

		auto rc = [&](const char* marker, const std::string& recv) -> std::string {
			auto [row, col] = pos(cpp, marker);
			return ts::resolveLocalType(ts::Lang::Cpp, cpp, row, col, recv);
		};

		CHECK(rc("/*U1*/", "w")     == "Widget", "C++ local Widget w -> Widget");
		CHECK(rc("/*U1*/", "p")     == "Widget", "C++ auto p = new Widget() -> Widget");
		CHECK(rc("/*U1*/", "q")     == "Widget", "C++ auto q = Widget() -> Widget");
		CHECK(rc("/*U1*/", "items") == "vector", "C++ std::vector<int> items -> vector");
		CHECK(rc("/*U1*/", "other") == "Widget", "C++ parameter other -> Widget");
		CHECK(rc("/*U1*/", "speed") == "float",  "C++ class field speed -> float");
		CHECK(rc("/*U1*/", "counter") == "int",  "C++ outer counter -> int");
		CHECK(rc("/*U1*/", "this")  == "Widget", "C++ this (in-class) -> Widget");
		// The scope-shadowing proof: inner float counter beats outer int counter.
		CHECK(rc("/*U2*/", "counter") == "float", "C++ scope shadow: inner counter -> float");
		// Out-of-line definition: this resolves to the enclosing class.
		CHECK(rc("/*U3*/", "this")  == "Widget", "C++ this (out-of-line) -> Widget");
		CHECK(rc("/*U3*/", "w2")    == "Widget", "C++ out-of-line parameter w2 -> Widget");
		// Negatives.
		CHECK(rc("/*U1*/", "3")          == "", "C++ numeric receiver -> empty");
		CHECK(rc("/*U1*/", "undeclared") == "", "C++ undeclared receiver -> empty");
		// Resolver -> STL table linkage (end to end for the std::vector case): the
		// user's headline ask, "use even something like an std::vector and get proper
		// members". `items` resolves to vector, whose member list carries push_back/
		// size — exactly what the editor's member-completion popup shows for `items.`.
		auto memHas = [](const std::vector<std::string>* v, const char* m) {
			if (!v) return false;
			for (auto& s : *v) if (s == m) return true;
			return false;
		};
		const auto* itemsMembers = ts::stlMembers(rc("/*U1*/", "items"));
		CHECK(memHas(itemsMembers, "push_back") && memHas(itemsMembers, "size"),
			"end-to-end: std::vector receiver -> push_back/size in the completion set");

		std::string cs =
			"class Widget {\n"
			"    public int X;\n"
			"    void Run(Widget other) {\n"
			"        var w = new Widget();\n"
			"        /*C1*/;\n"
			"    }\n"
			"}\n";
		auto rcs = [&](const char* marker, const std::string& recv) -> std::string {
			auto [row, col] = pos(cs, marker);
			return ts::resolveLocalType(ts::Lang::CSharp, cs, row, col, recv);
		};
		CHECK(rcs("/*C1*/", "w")     == "Widget", "C# var w = new Widget() -> Widget");
		CHECK(rcs("/*C1*/", "other") == "Widget", "C# parameter other -> Widget");
		CHECK(rcs("/*C1*/", "this")  == "Widget", "C# this -> Widget");
		CHECK(rcs("/*C1*/", "X")     == "int",    "C# field X -> int");
	}

	// ── Additional language grammars (Python, JavaScript) ──
	{
		CHECK(ts::langForExtension(".py") == ts::Lang::Python,      ".py -> Python grammar");
		CHECK(ts::langForExtension(".js") == ts::Lang::JavaScript,  ".js -> JavaScript grammar");
		CHECK(ts::langForExtension(".jsx") == ts::Lang::JavaScript, ".jsx -> JavaScript grammar");

		std::string py =
			"class Animal:\n"
			"    def speak(self):\n"
			"        return 1\n"
			"def helper(x):\n"
			"    return x\n";
		auto pys = ts::extractSymbols(ts::Lang::Python, py);
		std::fprintf(stderr, "[ts:py] symbols=%zu\n", pys.size());
		bool animal = false, helper = false;
		for (auto& s : pys) {
			if (s.name == "Animal" && s.isDefinition) animal = true;
			if (s.name == "helper" && s.isDefinition) helper = true;
		}
		CHECK(animal, "Python: class Animal found");
		CHECK(helper, "Python: function helper found");

		std::string js =
			"class Foo {\n"
			"  bar() { return 1; }\n"
			"}\n"
			"function baz() { return 2; }\n";
		auto jss = ts::extractSymbols(ts::Lang::JavaScript, js);
		std::fprintf(stderr, "[ts:js] symbols=%zu\n", jss.size());
		bool foo = false, baz = false;
		for (auto& s : jss) {
			if (s.name == "Foo" && s.isDefinition) foo = true;
			if (s.name == "baz" && s.isDefinition) baz = true;
		}
		CHECK(foo, "JS: class Foo found");
		CHECK(baz, "JS: function baz found");

		CHECK(ts::langForExtension(".lua") == ts::Lang::Lua, ".lua -> Lua grammar");
		CHECK(ts::langForExtension(".go")  == ts::Lang::Go,  ".go -> Go grammar");
		CHECK(ts::langForExtension(".rs")  == ts::Lang::Rust, ".rs -> Rust grammar");

		auto hasDef = [](const std::vector<ts::Symbol>& v, const char* nm) {
			for (auto& s : v) if (s.isDefinition && s.name == nm) return true;
			return false;
		};

		std::string lua =
			"local function helper()\n"
			"    return 1\n"
			"end\n";
		auto luas = ts::extractSymbols(ts::Lang::Lua, lua);
		std::fprintf(stderr, "[ts:lua] symbols=%zu\n", luas.size());
		CHECK(hasDef(luas, "helper"), "Lua: function helper found");

		std::string go =
			"package main\n"
			"type Widget struct { X int }\n"
			"func Run() int { return 1 }\n";
		auto gos = ts::extractSymbols(ts::Lang::Go, go);
		std::fprintf(stderr, "[ts:go] symbols=%zu\n", gos.size());
		CHECK(hasDef(gos, "Run"),    "Go: func Run found");
		CHECK(hasDef(gos, "Widget"), "Go: type Widget found");

		std::string rust =
			"struct Widget { x: i32 }\n"
			"fn run() -> i32 { 1 }\n";
		auto rusts = ts::extractSymbols(ts::Lang::Rust, rust);
		std::fprintf(stderr, "[ts:rust] symbols=%zu\n", rusts.size());
		CHECK(hasDef(rusts, "run"),    "Rust: fn run found");
		CHECK(hasDef(rusts, "Widget"), "Rust: struct Widget found");
	}

	// ── On-disk symbol cache roundtrip ──
	{
		std::unordered_map<std::string, ts::FileSyms> in;
		ts::FileSyms a;
		a.mtime = 123456789LL;
		a.size = 4096ULL;
		ts::Symbol s1; s1.name = "Foo"; s1.kind = ts::Kind::Class;  s1.line = 10; s1.column = 2; s1.isDefinition = true;  s1.enclosingType = "ns";
		ts::Symbol s2; s2.name = "bar"; s2.kind = ts::Kind::Method; s2.line = 12; s2.column = 4; s2.isDefinition = false; s2.enclosingType = "Foo";
		a.symbols = {s1, s2};
		in["a/b/foo.cpp"] = a;

		const char* path = "selftest_cache.idx";
		CHECK(ts::writeIndexCache(path, in), "writeIndexCache succeeds");
		std::unordered_map<std::string, ts::FileSyms> out;
		CHECK(ts::readIndexCache(path, out), "readIndexCache succeeds");
		CHECK(out.size() == 1, "cache roundtrip: one file entry");
		auto it = out.find("a/b/foo.cpp");
		CHECK(it != out.end(), "cache roundtrip: file key preserved");
		if (it != out.end()) {
			CHECK(it->second.mtime == 123456789LL && it->second.size == 4096ULL, "cache roundtrip: mtime/size");
			CHECK(it->second.symbols.size() == 2, "cache roundtrip: two symbols");
			if (it->second.symbols.size() == 2) {
				auto& r1 = it->second.symbols[0];
				CHECK(r1.name == "Foo" && r1.kind == ts::Kind::Class && r1.line == 10 &&
					  r1.column == 2 && r1.isDefinition && r1.enclosingType == "ns",
					  "cache roundtrip: symbol fields preserved");
				CHECK(!it->second.symbols[1].isDefinition, "cache roundtrip: isDefinition=false preserved");
			}
		}
		CHECK(!ts::readIndexCache("definitely_missing_cache_file.idx", out),
			  "readIndexCache fails on a missing file");
	}

	// ── LSP protocol layer (pure: framing, UTF-16 offsets, URIs, parsers) ──
	{
		// Content-Length framing roundtrip.
		std::string framed = lsp::frameMessage("{\"x\":1}");
		CHECK(framed.rfind("Content-Length: 7\r\n\r\n", 0) == 0, "frameMessage builds Content-Length header");
		std::size_t pos = 0;
		std::string body;
		CHECK(lsp::parseFrame(framed, pos, body) && body == "{\"x\":1}", "parseFrame roundtrips one frame");
		CHECK(pos == framed.size(), "parseFrame advances past the frame");
		// Partial frame -> false, pos unchanged.
		std::string partial = "Content-Length: 20\r\n\r\n{\"a\":1}";
		std::size_t ppos = 0;
		CHECK(!lsp::parseFrame(partial, ppos, body) && ppos == 0, "parseFrame returns false on a partial body");
		// Extra header line tolerated.
		std::string twoHdr = "Content-Type: x\r\nContent-Length: 3\r\n\r\nabc";
		std::size_t tpos = 0;
		CHECK(lsp::parseFrame(twoHdr, tpos, body) && body == "abc", "parseFrame tolerates extra headers");

		// UTF-16 offset conversion. "aé𝄞b": a=1B, é=2B(1u16), 𝄞=4B(2u16), b=1B.
		std::string s = "a\xC3\xA9\xF0\x9D\x84\x9E" "b";
		CHECK(lsp::utf8ByteToUtf16(s, 0) == 0, "utf16: byte 0 -> 0");
		CHECK(lsp::utf8ByteToUtf16(s, 1) == 1, "utf16: after ASCII -> 1");
		CHECK(lsp::utf8ByteToUtf16(s, 3) == 2, "utf16: after 2-byte char -> 2 units");
		CHECK(lsp::utf8ByteToUtf16(s, 7) == 4, "utf16: after astral char -> 4 units (surrogate pair)");
		CHECK(lsp::utf16ToUtf8Byte(s, 4) == 7, "utf16->byte inverse at astral boundary");
		CHECK(lsp::utf16ToUtf8Byte(s, 2) == 3, "utf16->byte inverse after 2-byte char");

		// path <-> uri roundtrip incl. spaces (percent-encoded) and drive letter.
		std::string uri = lsp::pathToUri("C:\\a b\\foo.cpp");
		CHECK(uri == "file:///C:/a%20b/foo.cpp", "pathToUri encodes spaces + drive form");
		CHECK(lsp::uriToPath(uri) == "C:/a b/foo.cpp", "uriToPath decodes back");
		CHECK(lsp::uriToPath("file:///c%3A/x/y.cpp") == "c:/x/y.cpp", "uriToPath decodes %3A colon");

		// Builders produce inspectable framed messages.
		std::string init = lsp::buildInitialize(1, "file:///C:/p", 4321);
		std::size_t ipos = 0; std::string ibody;
		CHECK(lsp::parseFrame(init, ipos, ibody), "buildInitialize is a valid frame");
		lsp::Incoming inq = lsp::inspect(ibody);
		CHECK(inq.hasId && inq.id == 1 && inq.method == "initialize", "inspect reads id + method");

		// Response parsers on canned clangd-shaped bodies.
		std::string compResp =
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"isIncomplete\":false,\"items\":["
			"{\"label\":\"push_back\",\"kind\":2,\"detail\":\"void\"},"
			"{\"label\":\" size\",\"insertText\":\"size\",\"kind\":2}]}}";
		auto comp = lsp::parseCompletion(compResp);
		CHECK(comp.size() == 2, "parseCompletion: two items");
		CHECK(!comp.empty() && comp[0].label == "push_back" && comp[0].insertText == "push_back",
			"parseCompletion: label/insertText");
		std::string defResp =
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":[{\"uri\":\"file:///C:/x.h\","
			"\"range\":{\"start\":{\"line\":41,\"character\":6},\"end\":{\"line\":41,\"character\":9}}}]}";
		auto defs = lsp::parseDefinition(defResp);
		CHECK(defs.size() == 1 && defs[0].line == 41 && defs[0].character == 6, "parseDefinition: location");
		CHECK(defs.size() == 1 && lsp::uriToPath(defs[0].uri) == "C:/x.h", "parseDefinition: uri");

		// Encoding negotiation: present -> utf-8; absent -> spec default utf-16.
		std::string enc;
		CHECK(lsp::parseInitializeResult("{\"result\":{\"offsetEncoding\":\"utf-8\",\"capabilities\":{}}}", enc) && enc == "utf-8",
			"parseInitializeResult reads utf-8");
		CHECK(lsp::parseInitializeResult("{\"result\":{\"capabilities\":{}}}", enc) && enc == "utf-16",
			"parseInitializeResult defaults to utf-16 when absent");

		// Hover: MarkupContent, plain string, MarkedString[], null.
		CHECK(lsp::parseHover("{\"result\":{\"contents\":{\"kind\":\"markdown\",\"value\":\"int x\"}}}") == "int x",
			"parseHover: MarkupContent value");
		CHECK(lsp::parseHover("{\"result\":{\"contents\":\"plain text\"}}") == "plain text",
			"parseHover: string contents");
		CHECK(lsp::parseHover("{\"result\":{\"contents\":[\"a\",{\"value\":\"b\"}]}}") == "a\nb",
			"parseHover: MarkedString array joined");
		CHECK(lsp::parseHover("{\"result\":null}").empty(), "parseHover: null result -> empty");
	}

	// parsePublishDiagnostics — server-pushed notification.
	{
		std::string uri; std::vector<lsp::Diagnostic> diags;
		const char* notif =
			"{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{"
			"\"uri\":\"file:///C%3A/x.cpp\",\"diagnostics\":["
			"{\"range\":{\"start\":{\"line\":4,\"character\":2},\"end\":{\"line\":4,\"character\":9}},"
			"\"severity\":1,\"message\":\"expected ';'\"},"
			"{\"range\":{\"start\":{\"line\":7,\"character\":0},\"end\":{\"line\":7,\"character\":5}},"
			"\"severity\":2,\"message\":\"unused variable\"}]}}";
		bool ok = lsp::parsePublishDiagnostics(notif, uri, diags);
		CHECK(ok, "parsePublishDiagnostics: recognized notification");
		CHECK(lsp::uriToPath(uri) == "C:/x.cpp", "parsePublishDiagnostics: uri decoded");
		CHECK(diags.size() == 2, "parsePublishDiagnostics: two diagnostics");
		CHECK(diags.size() == 2 && diags[0].line == 4 && diags[0].character == 2 &&
			diags[0].endChar == 9 && diags[0].severity == 1 && diags[0].message == "expected ';'",
			"parsePublishDiagnostics: first diagnostic fields");
		CHECK(diags.size() == 2 && diags[1].line == 7 && diags[1].severity == 2,
			"parsePublishDiagnostics: second diagnostic severity/line");
		// A non-diagnostics notification is rejected.
		std::string u2; std::vector<lsp::Diagnostic> d2;
		CHECK(!lsp::parsePublishDiagnostics("{\"method\":\"window/logMessage\",\"params\":{}}", u2, d2),
			"parsePublishDiagnostics: ignores other notifications");
		// Empty diagnostics array (server says "file is now clean").
		std::string u3; std::vector<lsp::Diagnostic> d3;
		CHECK(lsp::parsePublishDiagnostics(
			"{\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":\"file:///c%3A/y.cpp\",\"diagnostics\":[]}}",
			u3, d3) && d3.empty(), "parsePublishDiagnostics: empty array clears");
	}

	// NavHistory — back/forward stack (go-to-def "return to position").
	{
		auto L = [](const char* f, int ln) { NavLocation l; l.file = f; l.line = ln; return l; };
		NavHistory h;
		CHECK(!h.canBack() && !h.canForward(), "NavHistory: empty initially");

		// Jump A(10) -> def B(40): record origin A, land on B.
		h.record(L("a.cpp", 10));
		CHECK(h.canBack() && !h.canForward() && h.backDepth() == 1, "NavHistory: record pushes back, clears forward");

		// Back from B -> A. Forward stack now holds B.
		NavLocation out;
		CHECK(h.back(L("b.cpp", 40), out) && out.file == "a.cpp" && out.line == 10,
			"NavHistory: back returns origin");
		CHECK(!h.canBack() && h.canForward(), "NavHistory: back moved entry to forward");

		// Forward from A -> B.
		CHECK(h.forward(L("a.cpp", 10), out) && out.file == "b.cpp" && out.line == 40,
			"NavHistory: forward returns where we came from");
		CHECK(h.canBack() && !h.canForward(), "NavHistory: forward moved entry back");

		// A new record after a back branches history (forward is discarded).
		h.clear();
		h.record(L("a.cpp", 1));
		h.record(L("b.cpp", 2));        // back: [a1, b2]
		CHECK(h.back(L("c.cpp", 3), out) && out.line == 2, "NavHistory: back pops latest origin");
		CHECK(h.canForward(), "NavHistory: forward available after back");
		h.record(L("c.cpp", 3));        // new jump -> forward cleared
		CHECK(!h.canForward(), "NavHistory: new record clears forward (branch)");

		// Dedup: recording the same spot as the back top doesn't grow the stack.
		h.clear();
		h.record(L("a.cpp", 5));
		h.record(L("a.cpp", 5));
		CHECK(h.backDepth() == 1, "NavHistory: dedups consecutive same-spot record");

		// Invalid (empty-file) locations are ignored.
		h.clear();
		h.record(NavLocation{});
		CHECK(!h.canBack(), "NavHistory: ignores invalid origin");
		CHECK(!h.back(NavLocation{}, out), "NavHistory: back on empty returns false");
	}

	if (gFailures == 0) {
		std::printf("selftest: all %d checks passed\n", gChecks);
		return 0;
	}
	std::fprintf(stderr, "selftest: %d/%d checks FAILED\n", gFailures, gChecks);
	return 1;
}
