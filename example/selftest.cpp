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
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "TextEditor.h"
#include "tsindex.h"
#include "lsp_protocol.h"
#include "nav_history.h"
#include "cppgen.h"
#include "plugin_registry.h"
#ifdef IMGUIIDE_PLUGIN_UNREAL
#include "unreal.h"
#include "unreal_plugin.h"
#endif
#ifdef IMGUIIDE_PLUGIN_UEVR
#include "BlueprintEditor.h"
#include "BlueprintLua.h"
#include "BlueprintLuaImport.h"
#include "BlueprintRegistryJson.h"
#include "blueprint_templates.h"
#endif

#include <filesystem>
#include <fstream>

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

int main(int argc, char** argv)
{
#ifdef IMGUIIDE_PLUGIN_UEVR
	// Dev helper: `selftest <sdk_dump_dir>` loads a real UEVR SDK dump through the
	// same importer the plugin uses and reports counts — a headless way to sanity
	// -check real dumps without launching the app. Exits without running the suite.
	if (argc > 1) {
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		size_t classesBefore = bp.GetRegistry().GetClasses().size();
		int added = BlueprintRegistryJson::LoadSdkDir(bp.GetRegistry(), argv[1], {});
		std::printf("LoadSdkDir(%s): +%d classes; registry now classes=%zu enums=%zu\n",
		            argv[1], added, bp.GetRegistry().GetClasses().size(), bp.GetRegistry().GetEnums().size());
		return classesBefore == bp.GetRegistry().GetClasses().size() ? 1 : 0;
	}
#else
	(void)argc;
	(void)argv;
#endif

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

	// ── Ctrl+L (selectCursorLines): per-cursor whole-line select, grows on repeat ──
	{
		TextEditor ed;
		ed.SetText("aaa\nbbb\nccc\nddd\neee\n");   // 5 content lines (+ trailing empty)

		// Two cursors on different lines (1 and 3). Ctrl+L must select BOTH lines,
		// not collapse to one.
		ed.SetCursor(1, 2);
		ed.AddCursor(3, 1);
		CHECK(ed.GetNumberOfCursors() == 2, "Ctrl+L setup: two cursors");
		ed.SelectCursorLines();
		CHECK(ed.GetNumberOfCursors() == 2, "Ctrl+L: keeps both cursors (no collapse)");
		auto s0 = ed.GetCursorSelection(0);
		auto s1 = ed.GetCursorSelection(1);
		// Each selection spans its own whole line (col 0 -> start of next line).
		CHECK(s0.start.line == 1 && s0.start.column == 0 && s0.end.line == 2 && s0.end.column == 0,
			"Ctrl+L: cursor on line 1 selects full line 1");
		CHECK(s1.start.line == 3 && s1.start.column == 0 && s1.end.line == 4 && s1.end.column == 0,
			"Ctrl+L: cursor on line 3 selects full line 3");
		// Single cursor + repeat-grow (no merge ambiguity): line 0 -> select line 0,
		// repeat -> grow to lines 0-1.
		TextEditor ed2;
		ed2.SetText("aaa\nbbb\nccc\nddd\neee\n");
		ed2.SetCursor(0, 1);
		ed2.SelectCursorLines();
		auto z = ed2.GetCursorSelection(0);
		CHECK(ed2.GetNumberOfCursors() == 1 && z.start.line == 0 && z.start.column == 0 && z.end.line == 1,
			"Ctrl+L single cursor: selects its whole line");
		ed2.SelectCursorLines();
		z = ed2.GetCursorSelection(0);
		CHECK(z.start.line == 0 && z.end.line == 2, "Ctrl+L repeat: selection grows down one line");
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

		// Unreal Engine container / core-type members.
		const auto* tarr = ts::stlMembers("TArray");
		CHECK(has(tarr, "Add") && has(tarr, "Num") && has(tarr, "Contains") && has(tarr, "RemoveAt"),
			"TArray members include Add/Num/Contains/RemoveAt");
		const auto* tmap = ts::stlMembers("TMap");
		CHECK(has(tmap, "Add") && has(tmap, "FindOrAdd") && has(tmap, "Contains") && has(tmap, "Num"),
			"TMap members include Add/FindOrAdd/Contains/Num");
		const auto* tset = ts::stlMembers("TSet");
		CHECK(has(tset, "Add") && has(tset, "Contains") && has(tset, "Num"),
			"TSet members include Add/Contains/Num");
		const auto* fstr = ts::stlMembers("FString");
		CHECK(has(fstr, "Len") && has(fstr, "Contains") && has(fstr, "Replace"),
			"FString members include Len/Contains/Replace");
		const auto* tshared = ts::stlMembers("TSharedPtr");
		CHECK(has(tshared, "Get") && has(tshared, "IsValid"), "TSharedPtr members include Get/IsValid");
	}

	// UE type resolves through the chain resolver: a declared "TMap<FString,
	// FString> m;" reduces to "TMap" so "m." completes the curated members.
	{
		std::string cpp =
			"void f() {\n"
			"    TMap<FString, FString> m;\n"
			"    TArray<int32> a;\n"
			"    /*U*/\n"
			"}\n";
		int row = 3, col = 4; // the /*U*/ line
		auto has = [](const std::vector<std::string>* v, const char* mm) {
			if (!v) return false;
			for (auto& s : *v) if (s == mm) return true;
			return false;
		};
		std::string mt = ts::resolveMemberChain(ts::Lang::Cpp, cpp, row, col, {"m"});
		CHECK(mt == "TMap", "chain: TMap<FString,FString> m -> TMap");
		CHECK(has(ts::stlMembers(mt), "FindOrAdd"), "chain: m. completes TMap members");
		std::string at = ts::resolveMemberChain(ts::Lang::Cpp, cpp, row, col, {"a"});
		CHECK(at == "TArray", "chain: TArray<int32> a -> TArray");
		CHECK(has(ts::stlMembers(at), "Add"), "chain: a. completes TArray members");
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

	// ── Chained member resolution (a.b.c -> type of c) ──
	{
		auto pos = [](const std::string& s, const std::string& marker) -> std::pair<int,int> {
			size_t off = s.find(marker);
			if (off == std::string::npos) return {-1, -1};
			int row = 0; size_t lineStart = 0;
			for (size_t i = 0; i < off; ++i) if (s[i] == '\n') { ++row; lineStart = i + 1; }
			return { row, (int)(off - lineStart) };
		};
		// Outer has an Inner field + an Inner pointer; Inner holds a std::vector and
		// a scalar. The chain must hop Outer -> Inner -> {vector, int}.
		std::string cpp =
			"struct Inner {\n"
			"    std::vector<int> items;\n"
			"    int count;\n"
			"};\n"
			"struct Outer {\n"
			"    Inner inner;\n"
			"    Inner* ptr;\n"
			"    Inner getInner() { return inner; }\n"   // inline method -> return type
			"};\n"
			"void use() {\n"
			"    Outer o;\n"
			"    /*M*/;\n"
			"}\n";
		auto pm = pos(cpp, "/*M*/"); int row = pm.first, col = pm.second;   // plain ints: capturing a structured binding is a C++20 ext (Apple clang -Werror)
		auto chain = [&](std::vector<std::string> segs) {
			return ts::resolveMemberChain(ts::Lang::Cpp, cpp, row, col, segs);
		};
		auto memHas = [](const std::vector<std::string>* v, const char* m) {
			if (!v) return false;
			for (auto& s : *v) if (s == m) return true;
			return false;
		};
		// Single-segment behaves exactly like resolveLocalType.
		CHECK(chain({"o"}) == "Outer", "chain: base receiver o -> Outer");
		// Two-hop field access.
		CHECK(chain({"o", "inner"}) == "Inner", "chain: o.inner -> Inner");
		// Pointer field reduces to the pointee type (so o.ptr-> completes Inner).
		CHECK(chain({"o", "ptr"}) == "Inner", "chain: o.ptr -> Inner (pointer reduced)");
		// Three-hop into a std::vector field — end to end into the STL table.
		CHECK(chain({"o", "inner", "items"}) == "vector", "chain: o.inner.items -> vector");
		CHECK(memHas(ts::stlMembers(chain({"o", "inner", "items"})), "push_back"),
			"chain end-to-end: o.inner.items. completes push_back");
		// Scalar member terminates resolution (int has no further members).
		CHECK(chain({"o", "inner", "count"}) == "int", "chain: o.inner.count -> int");
			CHECK(chain({"o", "getInner"}) == "Inner", "chain: o.getInner() -> Inner (same-doc inline method)");
			CHECK(chain({"o", "getInner", "items"}) == "vector", "chain: o.getInner().items -> vector (same-doc)");
		// A bad hop anywhere returns empty (no false member list).
		CHECK(chain({"o", "nope"}).empty(), "chain: unknown member -> empty");
		CHECK(chain({"o", "inner", "nope"}).empty(), "chain: unknown deep member -> empty");
		// Element access THROUGH a field-of-field: o.inner.items is vector<int>, so
		// .front() yields int (the element type rides along on the field's stored
		// "vector\x1fint", not just on a local base).
		CHECK(chain({"o", "inner", "items", "front"}) == "int",
			"chain: o.inner.items.front() -> int (element through nested fields)");
		// A non-accessor member on the container still bails (no false members).
		CHECK(chain({"o", "inner", "items", "nope"}).empty(),
			"chain: vector.nope is not resolvable (no false members)");

		// C# property/field chain: Outer.Inner.Value.
		std::string cs =
			"class Leaf { public int Value; }\n"
			"class Branch { public Leaf Leaf; }\n"
			"class Tree {\n"
			"    Branch branch;\n"
			"    void Walk() {\n"
			"        /*CM*/;\n"
			"    }\n"
			"}\n";
		auto [crow, ccol] = pos(cs, "/*CM*/");
		CHECK(ts::resolveMemberChain(ts::Lang::CSharp, cs, crow, ccol, {"this", "branch"}) == "Branch",
			"C# chain: this.branch -> Branch");
		CHECK(ts::resolveMemberChain(ts::Lang::CSharp, cs, crow, ccol, {"this", "branch", "Leaf"}) == "Leaf",
			"C# chain: this.branch.Leaf -> Leaf");
	}

	// ── Element-type access: v.front()/at()/value()/get() -> element type ──
	{
		auto pos = [](const std::string& s, const std::string& marker) -> std::pair<int,int> {
			size_t off = s.find(marker);
			if (off == std::string::npos) return {-1, -1};
			int row = 0; size_t lineStart = 0;
			for (size_t i = 0; i < off; ++i) if (s[i] == '\n') { ++row; lineStart = i + 1; }
			return { row, (int)(off - lineStart) };
		};
		std::string cpp =
			"struct Widget { Inner sub; int health; };\n"
			"struct Inner { int x; };\n"
			"struct Holder { std::vector<Widget> widgets; std::vector<Widget> makeAll(); };\n"
			"void use() {\n"
			"    std::vector<Widget> ws;\n"
			"    std::optional<Widget> ow;\n"
			"    std::shared_ptr<Widget> pw;\n"
			"    std::map<int, Widget> mw;\n"
			"    Holder h;\n"
			"    /*E*/;\n"
			"}\n";
		auto pe = pos(cpp, "/*E*/"); int row = pe.first, col = pe.second;   // plain ints (see above)
		auto el = [&](std::vector<std::string> segs) {
			return ts::resolveMemberChain(ts::Lang::Cpp, cpp, row, col, segs);
		};
		CHECK(el({"ws", "front"}) == "Widget", "element: vector<Widget>.front() -> Widget");
		CHECK(el({"ws", "at"}) == "Widget", "element: vector<Widget>.at() -> Widget");
		CHECK(el({"ws", "back"}) == "Widget", "element: vector<Widget>.back() -> Widget");
		CHECK(el({"ow", "value"}) == "Widget", "element: optional<Widget>.value() -> Widget");
		CHECK(el({"pw", "get"}) == "Widget", "element: shared_ptr<Widget>.get() -> Widget");
		// Subscript v[i] (editor emits "[]" sentinel) -> element type.
		CHECK(el({"ws", "[]"}) == "Widget", "element: vector<Widget>[i] -> Widget (subscript)");
		CHECK(el({"ws", "[]", "sub"}) == "Inner", "element chain: ws[i].sub -> Inner");
		// Full chain: ws.front().sub -> Inner (element type's member).
		CHECK(el({"ws", "front", "sub"}) == "Inner", "element chain: ws.front().sub -> Inner");
		CHECK(el({"ws", "front", "health"}) == "int", "element chain: ws.front().health -> int");
		// map's value is the SECOND arg: m.at()/m[k] -> Widget (the value), not the key.
		CHECK(el({"mw", "at"}) == "Widget", "element: map<int,Widget>.at() -> Widget (value, not key)");
		CHECK(el({"mw", "[]"}) == "Widget", "element: map<int,Widget>[k] -> Widget (value)");
		CHECK(el({"mw", "[]", "sub"}) == "Inner", "element chain: mw[k].sub -> Inner");
		// A non-accessor member on the container is a normal (failing) hop, not element.
		CHECK(el({"ws", "size"}).empty(), "element: vector.size() is not an element accessor");
		// Element access THROUGH a field-of-container (h.widgets is vector<Widget>):
		// h.widgets.front().sub -> Inner, h.widgets[i].health -> int. And through a
		// method returning a container: h.makeAll().front().sub -> Inner.
		CHECK(el({"h", "widgets", "front"}) == "Widget", "element through field: h.widgets.front() -> Widget");
		CHECK(el({"h", "widgets", "front", "sub"}) == "Inner", "element through field: h.widgets.front().sub -> Inner");
		CHECK(el({"h", "widgets", "[]", "health"}) == "int", "element through field: h.widgets[i].health -> int");
		CHECK(el({"h", "makeAll", "front", "sub"}) == "Inner", "element through method: h.makeAll().front().sub -> Inner");
	}

	// ── Cross-file member types (extractMemberTypes + index-backed chains) ──
	{
		// extractMemberTypes pulls every type's members + their declared types.
		std::string cpp =
			"struct Inner {\n"
			"    std::vector<int> items;\n"
			"    int count;\n"
			"    Inner* self;\n"
			"};\n"
			"struct Outer {\n"
			"    Inner inner;\n"
			"    Inner getInner();\n"            // method prototype -> return type
			"    Inner* makeInner() { return nullptr; }\n"  // inline method -> return type
			"};\n";
		auto mt = ts::extractMemberTypes(ts::Lang::Cpp, cpp);
		// items is a vector field — stored as "vector\x1f<elem>" so element access can
		// chain through it. Assert the type prefix (encoding-tolerant).
		CHECK(mt.count("Inner") && mt["Inner"]["items"].rfind("vector", 0) == 0,
			"extractMemberTypes: Inner.items -> vector (type prefix)");
		CHECK(mt["Inner"]["count"] == "int", "extractMemberTypes: Inner.count -> int");
		CHECK(mt["Inner"]["self"] == "Inner", "extractMemberTypes: pointer member reduced");
		CHECK(mt.count("Outer") && mt["Outer"]["inner"] == "Inner", "extractMemberTypes: Outer.inner -> Inner");
		CHECK(mt["Outer"]["getInner"] == "Inner", "extractMemberTypes: method prototype -> return type");
		CHECK(mt["Outer"]["makeInner"] == "Inner", "extractMemberTypes: inline method -> return type (ptr reduced)");

		// Index-backed chain: the CURRENT doc only declares `Outer o;` — Outer and
		// Inner are defined elsewhere (here, supplied via the index map). The chain
		// o.inner.items must still resolve to vector by hopping through the index.
		ts::MemberTypeMap index = {
			{"Outer", {{"inner", "Inner"}, {"getInner", "Inner"}}},   // getInner: method -> return type
			{"Inner", {{"items", "vector"}, {"count", "int"}}},
		};
		std::string doc =
			"void f() {\n"
			"    Outer o;\n"
			"    /*X*/;\n"
			"}\n";
		auto xp = doc.find("/*X*/");
		int xrow = 0; size_t ls = 0;
		for (size_t i = 0; i < xp; ++i) if (doc[i] == '\n') { ++xrow; ls = i + 1; }
		int xcol = (int)(xp - ls);
		CHECK(ts::resolveMemberChain(ts::Lang::Cpp, doc, xrow, xcol, {"o", "inner"}, &index) == "Inner",
			"cross-file chain: o.inner -> Inner via index");
		CHECK(ts::resolveMemberChain(ts::Lang::Cpp, doc, xrow, xcol, {"o", "inner", "items"}, &index) == "vector",
			"cross-file chain: o.inner.items -> vector via index");
		// Method-return hop: o.getInner().items -> Inner -> vector (getInner keyed by
		// name -> return type, exactly like a field).
		CHECK(ts::resolveMemberChain(ts::Lang::Cpp, doc, xrow, xcol, {"o", "getInner", "items"}, &index) == "vector",
			"method-return chain via index: o.getInner().items -> vector");
		// Without the index, the same chain can't be resolved (types not in doc).
		CHECK(ts::resolveMemberChain(ts::Lang::Cpp, doc, xrow, xcol, {"o", "inner"}).empty(),
			"cross-file chain: unresolved without index (no false members)");

		// Real composition: feed extractMemberTypes' OWN output (not a hand-built
		// map) back into resolveMemberChain — this mirrors what the project index
		// does (extract per file -> merge -> resolve), minus the file walk.
		std::string defs =
			"struct Inner { std::vector<int> items; int count; };\n"
			"struct Outer { Inner inner; };\n";
		auto produced = ts::extractMemberTypes(ts::Lang::Cpp, defs);
		CHECK(ts::resolveMemberChain(ts::Lang::Cpp, doc, xrow, xcol, {"o", "inner", "items"}, &produced) == "vector",
			"cross-file chain (produced map): o.inner.items -> vector");
		CHECK(ts::resolveMemberChain(ts::Lang::Cpp, doc, xrow, xcol, {"o", "inner", "count"}, &produced) == "int",
			"cross-file chain (produced map): o.inner.count -> int");
		// Element access THROUGH the index: o.inner.items.front() -> int, proving the
		// element type survives extract -> (cache-shaped) map -> resolve, cross-file.
		CHECK(ts::resolveMemberChain(ts::Lang::Cpp, doc, xrow, xcol, {"o", "inner", "items", "front"}, &produced) == "int",
			"cross-file element through field: o.inner.items.front() -> int (via index)");
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

		// Real-world Lua definition forms must all land in tsDefs (the index that
		// gates whether "Go to Definition" is offered — see goToDefinitionProjectWide).
		auto luaForms = ts::extractSymbols(ts::Lang::Lua,
			"function M.foo() end\n"
			"function obj:bar() end\n"
			"M.baz = function() end\n"
			"function globalFn() end\n");
		CHECK(hasDef(luaForms, "foo"),      "Lua: function M.foo() captured");
		CHECK(hasDef(luaForms, "bar"),      "Lua: method obj:bar() captured");
		CHECK(hasDef(luaForms, "baz"),      "Lua: M.baz = function() captured");
		CHECK(hasDef(luaForms, "globalFn"), "Lua: global function captured");

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
		a.memberTypes["Foo"]["bar"] = "Widget";
		a.memberTypes["Foo"]["count"] = "int";
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
			CHECK(it->second.memberTypes.count("Foo") &&
				  it->second.memberTypes["Foo"]["bar"] == "Widget" &&
				  it->second.memberTypes["Foo"]["count"] == "int",
				  "cache roundtrip: memberTypes preserved");
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

	// ── MarkChangedLines (diff → change markers, ported from the app) ──────
	{
		// Build the editor at the NEW text, then diff against the OLD text.
		auto marked = [](const std::string& oldText, const std::string& newText) {
			TextEditor ed;
			ed.SetText(newText);
			return ed.MarkChangedLines(oldText);
		};

		// Single changed middle line → one inclusive [1,1] range.
		{
			auto r = marked("a\nb\nc\n", "a\nB\nc\n");
			CHECK(r.size() == 1 && r[0].first == 1 && r[0].second == 1,
				"MarkChangedLines: one changed line -> [1,1]");
		}
		// Identical text → no ranges.
		{
			auto r = marked("a\nb\nc\n", "a\nb\nc\n");
			CHECK(r.empty(), "MarkChangedLines: identical text -> no ranges");
		}
		// Two consecutive changed lines → coalesced into a single [1,2] range.
		{
			auto r = marked("a\nb\nc\nd\n", "a\nB\nC\nd\n");
			CHECK(r.size() == 1 && r[0].first == 1 && r[0].second == 2,
				"MarkChangedLines: consecutive changes coalesce to [1,2]");
		}
		// An inserted line is marked at its position.
		{
			auto r = marked("a\nc\n", "a\nb\nc\n");
			CHECK(r.size() == 1 && r[0].first == 1 && r[0].second == 1,
				"MarkChangedLines: inserted line -> [1,1]");
		}
		// Pure deletion (new text strictly shorter, nothing new) → no marks.
		{
			auto r = marked("a\nb\nc\n", "a\nc\n");
			CHECK(r.empty(), "MarkChangedLines: pure deletion -> no ranges");
		}
		// Two separated changes → two distinct ranges.
		{
			auto r = marked("a\nb\nc\nd\ne\n", "A\nb\nc\nd\nE\n");
			CHECK(r.size() == 2 && r[0].first == 0 && r[0].second == 0 &&
				  r[1].first == 4 && r[1].second == 4,
				"MarkChangedLines: separated changes -> two ranges");
		}
	}

	// ── Merge3 (3-way / diff3 merge, ported from the app) ─────────────────
	{
		bool cf = false;
		// Only mine changed → take mine, no conflict.
		cf = false;
		CHECK(TextEditor::Merge3("a\nb\nc\n", "a\nB\nc\n", "a\nb\nc\n", cf) == "a\nB\nc\n" && !cf,
			"Merge3: only-mine change auto-merges");
		// Only theirs changed → take theirs.
		cf = false;
		CHECK(TextEditor::Merge3("a\nb\nc\n", "a\nb\nc\n", "a\nb\nC\n", cf) == "a\nb\nC\n" && !cf,
			"Merge3: only-theirs change auto-merges");
		// Non-overlapping changes on both sides → both applied, no conflict.
		cf = false;
		CHECK(TextEditor::Merge3("a\nb\nc\nd\ne\n", "a\nB\nc\nd\ne\n", "a\nb\nc\nD\ne\n", cf) ==
				  "a\nB\nc\nD\ne\n" && !cf,
			"Merge3: non-overlapping changes auto-merge");
		// Identical change on both sides → dedup, no conflict.
		cf = false;
		CHECK(TextEditor::Merge3("a\nb\nc\n", "a\nB\nc\n", "a\nB\nc\n", cf) == "a\nB\nc\n" && !cf,
			"Merge3: identical change on both sides dedups");
		// Overlapping different changes → git-style conflict markers.
		cf = false;
		std::string conflicted = TextEditor::Merge3("a\nb\nc\n", "a\nB\nc\n", "a\nX\nc\n", cf);
		CHECK(cf && conflicted.find("<<<<<<<") != std::string::npos &&
				  conflicted.find("=======") != std::string::npos &&
				  conflicted.find(">>>>>>>") != std::string::npos,
			"Merge3: overlapping changes produce a conflict");
		// No change anywhere → identity.
		cf = false;
		CHECK(TextEditor::Merge3("a\nb\nc\n", "a\nb\nc\n", "a\nb\nc\n", cf) == "a\nb\nc\n" && !cf,
			"Merge3: identical inputs -> identity");
	}

	// ── IndentGuideLevels (VSCode-style indent guides, ported into the widget) ──
	{
		// Tab indentation: guide count == number of leading tabs (independent of tab
		// size, since each tab advances to the next stop).
		TextEditor ed;
		ed.SetText(
			"a\n"     // 0: level 0
			"\tb\n"   // 1: level 1
			"\t\tc\n" // 2: level 2
			"\td\n"); // 3: level 1
		auto lv = ed.IndentGuideLevels();
		CHECK(lv.size() >= 4 && lv[0] == 0 && lv[1] == 1 && lv[2] == 2 && lv[3] == 1,
			"IndentGuides: tab indent -> level per line");

		// A blank line inherits the SHALLOWER of its nearest non-blank neighbours, so
		// guides run through it without overshooting the block.
		TextEditor ed2;
		ed2.SetText(
			"a\n"      // 0: level 0
			"\tb\n"    // 1: level 1
			"\n"       // 2: blank -> min(prev 1, next 2) = 1
			"\t\tc\n"); // 3: level 2
		auto lv2 = ed2.IndentGuideLevels();
		CHECK(lv2.size() >= 4 && lv2[2] == 1, "IndentGuides: blank inherits shallower neighbour");
	}

	// ── CaretColumnAtVisual (click -> caret midpoint snap; the cursor-X fix) ──
	{
		TextEditor ed;
		ed.SetText("abc\n");
		// A fractional visual column snaps to the NEARER glyph gap. The bug was that
		// the click path pre-floored the column (int), so the sub-cell offset was lost
		// and the caret could land off the click on tab-containing lines.
		CHECK(ed.CaretColumnAtVisual(0, 0.4f) == 0, "CaretColumn: left of midpoint -> 0");
		CHECK(ed.CaretColumnAtVisual(0, 0.6f) == 1, "CaretColumn: right of midpoint -> 1");
		CHECK(ed.CaretColumnAtVisual(0, 1.6f) == 2, "CaretColumn: 1.6 -> 2 (nearest gap)");
		CHECK(ed.CaretColumnAtVisual(0, 100.0f) == 3, "CaretColumn: past EOL clamps to maxColumn");

		// A leading tab: a click in its left edge stays at the tab start (col 0).
		TextEditor ed2;
		ed2.SetText("\tab\n");
		CHECK(ed2.CaretColumnAtVisual(0, 0.2f) == 0, "CaretColumn: left edge of leading tab -> 0");
	}

#ifdef IMGUIIDE_PLUGIN_UNREAL
	// ── Unreal include resolution (module-relative Go-to-File) ────────────
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		auto root = fs::temp_directory_path(ec) / "imguiide_ue_selftest";
		fs::remove_all(root, ec);

		auto mk = [&](const fs::path& p) {
			fs::create_directories(p.parent_path(), ec);
			std::ofstream f(p);
			f << "// test\n";
		};
		auto uproj = root / "FakeProj" / "FakeProj.uproject";
		mk(uproj);
		auto engine = root / "FakeEngine";
		mk(engine / "Engine" / "Build" / "BatchFiles" / "Build.bat");
		auto gameHdr = root / "FakeProj" / "Source" / "MyGame" / "Public" / "MyChar.h";
		mk(gameHdr);
		auto engHdr = engine / "Engine" / "Source" / "Runtime" / "Engine" / "Classes" /
			"GameFramework" / "Actor.h";
		mk(engHdr);
		auto genHdr = root / "FakeProj" / "Intermediate" / "Build" / "Win64" /
			"UnrealEditor" / "Inc" / "MyGame" / "UHT" / "MyChar.generated.h";
		mk(genHdr);

		CHECK(unreal::findUProject(gameHdr.parent_path()) == uproj,
			"UE: findUProject walks up from Source/");
		CHECK(unreal::targetName(uproj) == "FakeProjEditor", "UE: editor target name");
		CHECK(unreal::hasCppSource(uproj), "UE: Source/ detected");
		CHECK(unreal::resolveInclude(engine, uproj, "MyChar.h") == gameHdr,
			"UE: game module Public header resolves");
		CHECK(unreal::resolveInclude(engine, uproj, "GameFramework/Actor.h") == engHdr,
			"UE: engine module Classes header resolves");
		CHECK(unreal::resolveInclude(engine, uproj, "MyChar.generated.h") == genHdr,
			"UE: UHT generated header resolves");
		CHECK(unreal::resolveInclude(engine, uproj, "NoSuch/Header.h").empty(),
			"UE: unknown include stays empty");

		// Descriptor autocomplete: schema words + discovered module/plugin names.
		mk(root / "FakeProj" / "Plugins" / "MyPlug" / "MyPlug.uplugin");
		mk(engine / "Engine" / "Plugins" / "Runtime" / "FakePlug" / "FakePlug.uplugin");
		auto& words = unreal::descriptorWords(engine, uproj.parent_path());
		auto has = [&](const char* s) {
			for (auto& x : words) if (x == s) return true;
			return false;
		};
		CHECK(has("LoadingPhase") && has("PostEngineInit") && has("RuntimeAndProgram"),
			"UE: descriptor schema words present");
		CHECK(has("MyGame"), "UE: project module name discovered");
		CHECK(has("MyPlug"), "UE: project plugin name discovered");
		CHECK(has("FakePlug"), "UE: engine plugin name discovered");
		CHECK(has("Engine"), "UE: engine module name discovered");

		fs::remove_all(root, ec);
	}

	// ── Unreal C++ type + macro highlighting ──────────────────────────────
	{
		// The UE vocabulary is added by the Unreal plugin at registration (in the
		// app, Editor's ctor). Apply it here through the plugin so the shared C++
		// language matches a running editor. onRegister ignores the host, but the
		// signature needs one — a no-op stub suffices.
		struct StubHost : PluginHost {
			std::filesystem::path hostProjectRoot() const override { return {}; }
			void hostSetProjectRoot(const std::string&) override {}
			void hostOpenFile(const std::string&) override {}
			void hostOpenLuaTab(const std::string&) override {}
			std::string hostActiveText() const override { return {}; }
			std::string hostActiveSelection() const override { return {}; }
			void hostToast(const std::string&) override {}
			void hostError(const std::string&) override {}
			void hostSendToClaude(const std::string&) override {}
			void hostRunInDir(const std::string&, const std::filesystem::path&) override {}
			void hostRunProjectBuild() override {}
			std::filesystem::path hostExeDir() const override { return {}; }
			std::filesystem::path hostRepoRoot() const override { return {}; }
			bool hostGetFlag(const std::string&, bool d) const override { return d; }
			void hostSetFlag(const std::string&, bool) override {}
			bool hostPanInverted() const override { return false; }
			void hostMiddleMousePanScroll(int) override {}
			// mirror Editor's impl so this test exercises the real host-routed path
			void hostAugmentCppLanguage(const std::vector<std::string>& types,
			                            const std::vector<std::string>& keywords,
			                            bool (*isTypeLike)(const std::string&)) override
			{
				auto* cpp = TextEditor::Language::CppMutable();
				if (!cpp) return;
				for (auto& t : types) cpp->declarations.insert(t);
				for (auto& k : keywords) cpp->keywords.insert(k);
				if (isTypeLike) cpp->isTypeLike = isTypeLike;
			}
		} stub;
		UnrealPlugin().onRegister(stub);

		auto cpp = TextEditor::Language::Cpp();
		int typeCol = colorAt(cpp, "int i;\n", 0, 0);    // known base type
		int kwCol = colorAt(cpp, "return x;\n", 0, 0);   // known keyword
		int plainId = colorAt(cpp, "zqzq x;\n", 0, 0);   // unknown identifier (control)
		CHECK(typeCol != plainId, "UE C++: (sanity) type color differs from plain identifier");
		CHECK(colorAt(cpp, "int32 x;\n", 0, 0) == typeCol, "UE C++: int32 colored as a type");
		CHECK(colorAt(cpp, "uint8 x;\n", 0, 0) == typeCol, "UE C++: uint8 colored as a type");
		CHECK(colorAt(cpp, "FString s;\n", 0, 0) == typeCol, "UE C++: FString colored as a type");
		CHECK(colorAt(cpp, "TArray<int> a;\n", 0, 0) == typeCol, "UE C++: TArray colored as a type");
		CHECK(colorAt(cpp, "UCLASS()\n", 0, 0) == kwCol, "UE C++: UCLASS colored as a keyword");
		CHECK(colorAt(cpp, "UE_LOG(x);\n", 0, 0) == kwCol, "UE C++: UE_LOG colored as a keyword");

		// Both types in "TArray<FString>&" color (tokenizer splits at < > &).
		CHECK(colorAt(cpp, "TArray<FString>& r;\n", 0, 0) == typeCol, "UE C++: TArray in TArray<FString>&");
		CHECK(colorAt(cpp, "TArray<FString>& r;\n", 0, 7) == typeCol, "UE C++: FString inside <> colored");

		// Pattern fallback: UNLISTED F*/U*/A*/T* CamelCase types color as types.
		CHECK(colorAt(cpp, "AMyActor* a;\n", 0, 0) == typeCol, "UE C++: unlisted AMyActor colored (pattern)");
		CHECK(colorAt(cpp, "UMyComponent* c;\n", 0, 0) == typeCol, "UE C++: unlisted UMyComponent colored");
		CHECK(colorAt(cpp, "FMyStruct s;\n", 0, 0) == typeCol, "UE C++: unlisted FMyStruct colored");
		// All-caps macros / normal CamelCase are NOT miscolored by the pattern.
		CHECK(colorAt(cpp, "TESTING x;\n", 0, 0) == plainId, "UE C++: SCREAMING_CASE not treated as a type");
		CHECK(colorAt(cpp, "Total x;\n", 0, 0) == plainId, "UE C++: normal CamelCase (Total) not a type");
	}
#endif // IMGUIIDE_PLUGIN_UNREAL

	// ── Plugin registry: runtime enable/disable + persistence ─────────────
	// Config-independent (uses a private test plugin, not a compiled-in one), so
	// this runs in a core build too. Verifies the flag store loads/persists the
	// per-plugin enabled state and gates the hooks.
	{
		struct FlagHost : PluginHost {
			std::unordered_map<std::string, bool> flags;
			std::filesystem::path hostProjectRoot() const override { return {}; }
			void hostSetProjectRoot(const std::string&) override {}
			void hostOpenFile(const std::string&) override {}
			void hostOpenLuaTab(const std::string&) override {}
			std::string hostActiveText() const override { return {}; }
			std::string hostActiveSelection() const override { return {}; }
			void hostToast(const std::string&) override {}
			void hostError(const std::string&) override {}
			void hostSendToClaude(const std::string&) override {}
			void hostRunInDir(const std::string&, const std::filesystem::path&) override {}
			void hostRunProjectBuild() override {}
			std::filesystem::path hostExeDir() const override { return {}; }
			std::filesystem::path hostRepoRoot() const override { return {}; }
			bool hostPanInverted() const override { return false; }
			void hostMiddleMousePanScroll(int) override {}
			void hostAugmentCppLanguage(const std::vector<std::string>&, const std::vector<std::string>&,
			                            bool (*)(const std::string&)) override {}
			bool hostGetFlag(const std::string& k, bool d) const override
			{
				auto it = flags.find(k);
				return it == flags.end() ? d : it->second;
			}
			void hostSetFlag(const std::string& k, bool v) override { flags[k] = v; }
		};
		struct CountPlugin : EditorPlugin {
			int registers = 0, frames = 0;
			const char* id() const override { return "selftest.count"; }
			const char* displayName() const override { return "Selftest counter"; }
			void onRegister(PluginHost&) override { ++registers; }
			void onFrame(PluginHost&) override { ++frames; }
		};
		const std::string key = PluginRegistry::enabledKey("selftest.count");

		// Default (no saved flag): enabled → onRegister once, onFrame dispatches.
		{
			FlagHost host;
			PluginRegistry reg;
			auto up = std::make_unique<CountPlugin>();
			CountPlugin* raw = up.get();
			reg.add(std::move(up));
			reg.registerAll(host);
			CHECK(raw->enabled(), "plugin: default enabled after registerAll");
			CHECK(raw->registers == 1, "plugin: onRegister ran once when enabled");
			reg.frame(host);
			CHECK(raw->frames == 1, "plugin: enabled plugin gets onFrame");
		}

		// Persisted-off: registerAll skips onRegister and frame() is gated off;
		// enabling at runtime persists the flag and lazily runs onRegister once.
		{
			FlagHost host;
			host.flags[key] = false;
			PluginRegistry reg;
			auto up = std::make_unique<CountPlugin>();
			CountPlugin* raw = up.get();
			reg.add(std::move(up));
			reg.registerAll(host);
			CHECK(!raw->enabled(), "plugin: persisted-off flag disables at registerAll");
			CHECK(raw->registers == 0, "plugin: disabled plugin does not onRegister");
			reg.frame(host);
			CHECK(raw->frames == 0, "plugin: disabled plugin gets no onFrame");

			reg.setEnabled(host, *raw, true);
			CHECK(raw->enabled(), "plugin: setEnabled(true) enables");
			CHECK(host.flags[key], "plugin: enable persisted to flag store");
			CHECK(raw->registers == 1, "plugin: lazy onRegister on first enable");
			reg.frame(host);
			CHECK(raw->frames == 1, "plugin: re-enabled plugin resumes onFrame");

			reg.setEnabled(host, *raw, false);
			CHECK(!host.flags[key], "plugin: disable persisted to flag store");
			reg.setEnabled(host, *raw, true);
			CHECK(raw->registers == 1, "plugin: onRegister not repeated on re-enable");
		}
	}

	// ── C++ definition / declaration generation ───────────────────────────
	{
		using namespace cppgen;

		// Single member: normal method.
		{
			MemberDecl m = parseMemberDecl("int computeArea(const Rect& r, int scale = 1) const;");
			CHECK(m.valid, "cppgen: normal method parses");
			CHECK(m.returnType == "int", "cppgen: return type = int");
			CHECK(m.name == "computeArea", "cppgen: name = computeArea");
			CHECK(m.params == "const Rect& r, int scale", "cppgen: default arg stripped");
			CHECK(m.trailing == "const", "cppgen: const qualifier kept");
			CHECK(memberDefinition("Widget", m) == "int Widget::computeArea(const Rect& r, int scale) const\n{\n}\n",
			      "cppgen: definition stub for normal method");
		}

		// virtual + override are dropped; pure-virtual gets no body.
		{
			MemberDecl v = parseMemberDecl("virtual void draw() override;");
			CHECK(v.valid && v.returnType == "void" && v.trailing.empty(),
			      "cppgen: virtual/override stripped");
			CHECK(memberDefinition("Shape", v) == "void Shape::draw()\n{\n}\n",
			      "cppgen: virtual def has no virtual/override");
			MemberDecl pure = parseMemberDecl("virtual double area() const = 0;");
			CHECK(pure.isPureVirtual, "cppgen: = 0 detected");
			CHECK(memberDefinition("Shape", pure).empty(), "cppgen: pure virtual → no definition");
		}

		// Constructor / destructor: no return type.
		{
			MemberDecl ctor = parseMemberDecl("Widget(int w, int h);");
			CHECK(ctor.isCtorOrDtor && ctor.returnType.empty() && ctor.name == "Widget",
			      "cppgen: constructor parsed");
			CHECK(memberDefinition("Widget", ctor) == "Widget::Widget(int w, int h)\n{\n}\n",
			      "cppgen: constructor definition");
			MemberDecl dtor = parseMemberDecl("~Widget();");
			CHECK(dtor.name == "~Widget", "cppgen: destructor name keeps ~");
			CHECK(memberDefinition("Widget", dtor) == "Widget::~Widget()\n{\n}\n",
			      "cppgen: destructor definition");
			MemberDecl def = parseMemberDecl("Widget() = default;");
			CHECK(def.isDefaulted && memberDefinition("Widget", def).empty(),
			      "cppgen: = default → no definition");
		}

		// noexcept, ref-qualifier, templated return, operator.
		{
			MemberDecl m = parseMemberDecl("std::vector<int> values() const noexcept;");
			CHECK(m.returnType == "std::vector<int>" && m.trailing == "const noexcept",
			      "cppgen: templated return + const noexcept");
			MemberDecl op = parseMemberDecl("Widget& operator=(const Widget& o);");
			CHECK(op.name == "operator=" && op.returnType == "Widget&",
			      "cppgen: operator= parsed");
			CHECK(memberDefinition("Widget", op) == "Widget& Widget::operator=(const Widget& o)\n{\n}\n",
			      "cppgen: operator= definition");
		}

		// Inline body → excluded from generation.
		{
			MemberDecl inl = parseMemberDecl("int getX() const { return x; }");
			CHECK(inl.hasInlineBody && memberDefinition("P", inl).empty(),
			      "cppgen: inline-bodied member → no definition");
		}

		// Enclosing class + whole-class generation, skipping inline/data/pure.
		{
			std::string hdr =
			    "class Widget {\n"
			    "public:\n"
			    "    Widget();\n"
			    "    int area() const;\n"
			    "    int getX() const { return x; }\n"   // inline: skip
			    "    virtual void draw() = 0;\n"          // pure: skip
			    "private:\n"
			    "    int x;\n"                            // data member: skip
			    "};\n";
			CHECK(enclosingClass(hdr, 2) == "Widget", "cppgen: enclosingClass finds Widget");
			CHECK(enclosingClass(hdr, 0).empty() || enclosingClass(hdr, 8) == "Widget",
			      "cppgen: enclosingClass line inside body");
			std::string cls;
			std::string one = generateOneDefinition(hdr, 3, &cls);  // int area() const;
			CHECK(one == "int Widget::area() const\n{\n}\n" && cls == "Widget",
			      "cppgen: generateOneDefinition for area()");
			std::string all = generateClassDefinitions(hdr, 3, &cls);
			CHECK(all.find("Widget::Widget()") != std::string::npos, "cppgen: all-defs includes ctor");
			CHECK(all.find("int Widget::area() const") != std::string::npos, "cppgen: all-defs includes area");
			CHECK(all.find("getX") == std::string::npos, "cppgen: all-defs skips inline getX");
			CHECK(all.find("draw") == std::string::npos, "cppgen: all-defs skips pure draw");
			CHECK(all.find("int x") == std::string::npos, "cppgen: all-defs skips data member");
		}

		// Reverse: declaration from an out-of-line definition.
		{
			std::string decl = declarationFromDefinition("int Widget::computeArea(const Rect& r) const\n{\n    return 0;\n}");
			CHECK(decl == "int Widget::computeArea(const Rect& r) const;" || decl == "int computeArea(const Rect& r) const;",
			      "cppgen: declarationFromDefinition strips qualifier");
			CHECK(declarationFromDefinition("void freeFunc(int x) {}").empty(),
			      "cppgen: unqualified free function → no declaration");
		}
	}

	// ── Blueprint → UEVR Lua codegen (pure; no ImGui context needed) ───────
	// Gated on the UEVR plugin: the widgets it exercises ship only with it.
#ifdef IMGUIIDE_PLUGIN_UEVR
	{
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);

		// registry is populated with the UEVR API surface
		CHECK(bp.GetRegistry().FindClass("UEVR") != nullptr, "blueprintlua: UEVR class registered");
		CHECK(bp.GetRegistry().FindEvent("UEVR", "Pre Engine Tick") != nullptr,
		      "blueprintlua: Pre Engine Tick event registered");
		CHECK(bp.GetRegistry().FindFunction("UEVR_API", "Find UObject") != nullptr,
		      "blueprintlua: uevr.api Find UObject registered");

		// build a tiny graph: Pre Engine Tick -> Print, and generate a script
		auto tick = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(0, 0));
		auto print = bp.AddCallFunctionNode("UEVR_API", "Print", ImVec2(320, 0));
		CHECK(tick != 0 && print != 0, "blueprintlua: event + call nodes created");
		bp.AddLink(bp.FindPinID(tick, "", true), bp.FindPinID(print, "", false));

		std::string lua = BlueprintLua::GenerateScript(bp);
		CHECK(!lua.empty(), "blueprintlua: GenerateScript emits non-empty output");
		CHECK(lua.find("uevr.sdk.callbacks.on_pre_engine_tick") != std::string::npos,
		      "blueprintlua: event node becomes an on_pre_engine_tick callback");
		CHECK(lua.find("print(") != std::string::npos,
		      "blueprintlua: Print call node emits print(...)");

		// empty graph still generates a valid (non-crashing) script
		BlueprintEditor empty;
		BlueprintLua::SetupUEVRRegistry(empty);
		CHECK(BlueprintLua::GenerateScript(empty).size() >= 0, "blueprintlua: empty graph is safe");

		// curated Lua-API autocomplete word list
		const auto& api = BlueprintLua::LuaApiIdentifiers();
		auto hasWord = [&](const char* w) {
			for (auto& s : api) if (s == w) return true;
			return false;
		};
		CHECK(!api.empty() && hasWord("find_uobject") && hasWord("on_pre_engine_tick") && hasWord("uevr"),
		      "blueprintlua: LuaApiIdentifiers includes core UEVR tokens");
	}

	// ── Custom Lua node: create/pins/source/save-load round-trip ───────────
	{
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);

		auto node = bp.AddCustomLuaNode(ImVec2(0, 0));
		CHECK(node != 0, "customlua: node created");
		CHECK(bp.SetCustomLuaSource(node, "Out0 = 1 + 1"), "customlua: source set");

		auto outPin = bp.AddCustomLuaPin(node, true, "");
		CHECK(outPin != 0, "customlua: output pin added");
		CHECK(bp.GetPin(outPin) != nullptr && bp.GetPin(outPin)->name == "Out0",
		      "customlua: unnamed pin auto-named Out0");

		auto inPin = bp.AddCustomLuaPin(node, false, "MyInput");
		CHECK(inPin != 0 && bp.GetPin(inPin) != nullptr && bp.GetPin(inPin)->name == "MyInput",
		      "customlua: named input pin keeps its name");

		CHECK(bp.RemoveCustomLuaPin(node, inPin), "customlua: pin removed");
		CHECK(bp.GetPin(inPin) == nullptr, "customlua: removed pin no longer resolves");

		auto execIn = bp.FindPinID(node, "", false);
		CHECK(execIn != 0, "customlua: exec pins survive pin add/remove");

		std::string saved = bp.SaveToString();
		BlueprintEditor loaded;
		CHECK(loaded.LoadFromString(saved), "customlua: graph reloads");
		CHECK(loaded.GetNodeCount() == 1, "customlua: reloaded graph has exactly one node");
		auto* reloadedNode = loaded.GetNode(loaded.GetNodes()[0].id);
		CHECK(reloadedNode != nullptr && reloadedNode->customCode == "Out0 = 1 + 1",
		      "customlua: source text survives save/load");
		CHECK(reloadedNode != nullptr && reloadedNode->pins.size() == 3, // exec in, exec out, Out0
		      "customlua: dynamic pin count survives save/load");
	}

	// ── Custom Lua codegen: mixed with a CallFunction node ──────────────────
	{
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);

		auto tick = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(0, 0));
		auto lua = bp.AddCustomLuaNode(ImVec2(320, 0));
		bp.SetCustomLuaSource(lua, "print(\"hello\")");
		bp.AddLink(bp.FindPinID(tick, "", true), bp.FindPinID(lua, "", false));

		std::string script = BlueprintLua::GenerateScript(bp);
		CHECK(script.find("print(\"hello\")") != std::string::npos,
		      "customlua codegen: raw source appears verbatim in the generated script");
		CHECK(script.find("uevr.sdk.callbacks.on_pre_engine_tick") != std::string::npos,
		      "customlua codegen: still emits the surrounding event registration");
	}

	// ── Import: round-trips GenerateScript's own output ─────────────────────
	{
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		auto tick = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(0, 0));
		auto print = bp.AddCallFunctionNode("UEVR_API", "Print", ImVec2(320, 0));
		bp.AddLink(bp.FindPinID(tick, "", true), bp.FindPinID(print, "", false));
		std::string script = BlueprintLua::GenerateScript(bp);

		BlueprintEditor imported;
		BlueprintLua::SetupUEVRRegistry(imported);
		std::string error;
		CHECK(BlueprintLuaImport::ImportScript(imported, script, error),
		      "import: GenerateScript output imports without error");
		CHECK(imported.GetNodeCount() >= 2, "import: recognizes the event and the call as separate nodes");

		bool hasEventNode = false, hasCallNode = false;
		for (auto& n : imported.GetNodes()) {
			if (n.kind == BlueprintEditor::NodeKind::Event) hasEventNode = true;
			if (n.kind == BlueprintEditor::NodeKind::CallFunction && n.memberName == "Print") hasCallNode = true;
		}
		CHECK(hasEventNode, "import: event wrapper decomposed into a real Event node (not a fallback)");
		CHECK(hasCallNode, "import: bare call decomposed into a real CallFunction node (not a fallback)");

		// re-generating the imported graph should still mention the same callback + call
		std::string regenerated = BlueprintLua::GenerateScript(imported);
		CHECK(regenerated.find("uevr.sdk.callbacks.on_pre_engine_tick") != std::string::npos,
		      "import round-trip: regenerated script keeps the event registration");
	}

	// ── Import: never drops or crashes on unrecognizable input ──────────────
	{
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		std::string error;
		std::string weird =
			"uevr.sdk.callbacks.on_draw_ui(function()\n"
			"    local x = coroutine.wrap(function() return 1 end)()\n"
			"    SomeGlobal.deeply.nested[x]:call(1, 2, 3)\n"
			"end)\n";
		CHECK(BlueprintLuaImport::ImportScript(bp, weird, error),
		      "import: unrecognizable-but-valid Lua still succeeds (via fallback)");
		CHECK(bp.GetNodeCount() >= 1, "import: fallback still produces at least one node, nothing silently dropped");

		bool hasCustomLua = false;
		for (auto& n : bp.GetNodes()) {
			if (n.kind == BlueprintEditor::NodeKind::CustomLua && n.customCode.find("coroutine.wrap") != std::string::npos) {
				hasCustomLua = true;
			}
		}
		CHECK(hasCustomLua, "import: unrecognized body text preserved verbatim in a Custom Lua node");

		// totally empty input is handled gracefully too
		BlueprintEditor emptyImport;
		BlueprintLua::SetupUEVRRegistry(emptyImport);
		std::string emptyError;
		CHECK(BlueprintLuaImport::ImportScript(emptyImport, "", emptyError),
		      "import: empty source does not error");
	}

	// ── Blueprint templates: each builds a valid, generating graph ─────────
	// A wrong class/function/pin name would silently drop a link, so the content
	// spot-checks below (which need the links intact) are the real guard.
	{
		const auto& templates = BlueprintTemplates::All();
		CHECK(templates.size() >= 3, "templates: at least three built-in templates");
		for (auto& tmpl : templates) {
			BlueprintEditor bp;
			BlueprintLua::SetupUEVRRegistry(bp);
			tmpl.build(bp);
			CHECK(bp.GetNodeCount() >= 2, "template: builds at least two nodes");
			CHECK(!BlueprintLua::GenerateScript(bp).empty(), "template: generates a non-empty script");
		}

		BlueprintEditor hello;
		BlueprintLua::SetupUEVRRegistry(hello);
		templates[0].build(hello);
		std::string h = BlueprintLua::GenerateScript(hello);
		CHECK(h.find("on_pre_engine_tick") != std::string::npos && h.find("Hello from UEVR") != std::string::npos,
		      "template hello: registers the tick and prints the message");

		BlueprintEditor ui;
		BlueprintLua::SetupUEVRRegistry(ui);
		templates[1].build(ui);
		std::string u = BlueprintLua::GenerateScript(ui);
		CHECK(u.find("begin_window") != std::string::npos && u.find("end_window") != std::string::npos,
		      "template panel: emits begin_window + end_window");

		BlueprintEditor pawn;
		BlueprintLua::SetupUEVRRegistry(pawn);
		templates[2].build(pawn);
		std::string p = BlueprintLua::GenerateScript(pawn);
		CHECK(p.find("get_local_pawn") != std::string::npos && p.find("get_full_name") != std::string::npos,
		      "template log-pawn: data links carry pawn:get_full_name() into print");
	}

	// ── Registry JSON: data-driven API export/import round-trips ────────────
	{
		BlueprintEditor a;
		BlueprintLua::SetupUEVRRegistry(a);
		std::string js = BlueprintRegistryJson::Save(a.GetRegistry());
		CHECK(js.find("UEVR_API") != std::string::npos && js.find("on_pre_engine_tick") != std::string::npos,
		      "registry json: save emits class names + metadata");

		BlueprintEditor b;
		b.GetRegistry().Clear(); // start empty so counts compare exactly
		std::string error;
		CHECK(BlueprintRegistryJson::Load(b.GetRegistry(), js, error), "registry json: reloads without error");
		CHECK(a.GetRegistry().GetClasses().size() == b.GetRegistry().GetClasses().size(),
		      "registry json: class count round-trips");
		CHECK(a.GetRegistry().GetEnums().size() == b.GetRegistry().GetEnums().size(),
		      "registry json: enum count round-trips");

		const auto* pf = b.GetRegistry().FindFunction("UEVR_API", "Print");
		CHECK(pf != nullptr && pf->metadata.find("print(") != std::string::npos,
		      "registry json: function metadata (Lua template) round-trips");

		// The reloaded registry must still drive codegen identically.
		auto tick = b.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(0, 0));
		auto print = b.AddCallFunctionNode("UEVR_API", "Print", ImVec2(300, 0));
		b.AddLink(b.FindPinID(tick, "", true), b.FindPinID(print, "", false));
		std::string script = BlueprintLua::GenerateScript(b);
		CHECK(script.find("uevr.sdk.callbacks.on_pre_engine_tick") != std::string::npos && script.find("print(") != std::string::npos,
		      "registry json: reloaded registry generates the same script");

		BlueprintEditor c;
		std::string cerr;
		CHECK(!BlueprintRegistryJson::Load(c.GetRegistry(), "{ not valid", cerr) && !cerr.empty(),
		      "registry json: malformed input reports an error");
	}

	// ── SDK import: UEVR Class Browser reflection dump + enum .lua ──────────
	{
		// A trimmed slice of a real UEVR classes/*.json reflection dump.
		std::string dump = R"JSON({
		  "classes": [
		    {
		      "full_name": "Class /Script/IKRig.IKRetargetPinBoneController",
		      "name": "IKRetargetPinBoneController",
		      "super": "IKRetargetOpControllerBase",
		      "properties": [],
		      "functions": [
		        {
		          "name": "SetBonePair",
		          "flag_names": ["Final","Native","Public","BlueprintCallable"],
		          "params": [
		            {"name":"InBoneToCopyFrom","type":"NameProperty","flag_names":["ConstParm","Parm"]},
		            {"name":"InBoneToCopyTo","type":"NameProperty","flag_names":["ConstParm","Parm"]}
		          ]
		        },
		        {
		          "name": "GetSettings",
		          "flag_names": ["Final","Native","Public","BlueprintCallable","BlueprintPure"],
		          "params": [
		            {"name":"ReturnValue","type":"StructProperty","flag_names":["Parm","OutParm","ReturnParm"]}
		          ]
		        }
		      ]
		    }
		  ]
		})JSON";

		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		size_t before = bp.GetRegistry().GetClasses().size();
		std::string err;
		CHECK(BlueprintRegistryJson::Load(bp.GetRegistry(), dump, err), "sdk dump: UEVR reflection dump imports");
		CHECK(bp.GetRegistry().GetClasses().size() == before + 1, "sdk dump: adds the dumped class");

		const auto* cls = bp.GetRegistry().FindClass("IKRetargetPinBoneController");
		CHECK(cls != nullptr && cls->parentName == "IKRetargetOpControllerBase", "sdk dump: class parent (super) mapped");

		const auto* setPair = bp.GetRegistry().FindFunction("IKRetargetPinBoneController", "SetBonePair");
		int inCount = 0, outCount = 0;
		if (setPair)
			for (auto& p : setPair->parameters)
				(p.isOutput ? outCount : inCount)++;
		CHECK(setPair != nullptr && inCount == 2 && outCount == 0, "sdk dump: input params mapped, no phantom outputs");

		const auto* getSettings = bp.GetRegistry().FindFunction("IKRetargetPinBoneController", "GetSettings");
		bool hasReturn = false;
		if (getSettings)
			for (auto& p : getSettings->parameters)
				if (p.isOutput)
					hasReturn = true;
		CHECK(getSettings != nullptr && getSettings->isPure, "sdk dump: BlueprintPure -> pure node");
		CHECK(hasReturn, "sdk dump: ReturnParm -> Return Value output");

		CHECK(bp.AddCallFunctionNode("IKRetargetPinBoneController", "SetBonePair", ImVec2(0, 0)) != 0,
		      "sdk dump: imported function spawns a node");

		// Enum .lua (UE4SS annotation) parsing; the ---@class line must be ignored.
		std::string enumLua =
			"---@meta\n"
			"---@enum EAxis\n"
			"EAxis = {\n"
			"    X = 1,\n"
			"    Y = 2,\n"
			"    Z = 3,\n"
			"    EAxis_MAX = 4,\n"
			"}\n"
			"---@class Actor : Object\n"
			"Actor = {}\n";
		size_t enumsBefore = bp.GetRegistry().GetEnums().size();
		int added = BlueprintRegistryJson::LoadEnumLua(bp.GetRegistry(), enumLua);
		CHECK(added == 1, "sdk enum lua: exactly one enum parsed (class annotation ignored)");
		CHECK(bp.GetRegistry().GetEnums().size() == enumsBefore + 1, "sdk enum lua: enum added to registry");
		const auto* eax = bp.GetRegistry().FindEnum("EAxis");
		CHECK(eax != nullptr && eax->values.size() == 4 && eax->values[0] == "X",
		      "sdk enum lua: enum values captured in order");
	}

	// ── Variables: typed vars drive Get/Set codegen + round-trip ───────────
	{
		using PK = BlueprintEditor::PinKind;
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		CHECK(bp.AddVariable("Health", BlueprintEditor::PinType(PK::Float), "100"), "variables: add typed variable");
		CHECK(bp.GetVariables().size() == 1 && bp.GetVariables()[0].name == "Health", "variables: variable registered");

		auto tick = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(0, 0));
		auto setN = bp.AddVariableSetNode("Health", ImVec2(300, 0));
		CHECK(setN != 0, "variables: Set node created");
		bp.AddLink(bp.FindPinID(tick, "", true), bp.FindPinID(setN, "", false));
		CHECK(BlueprintLua::GenerateScript(bp).find("Health") != std::string::npos,
		      "variables: variable name appears in generated script");

		CHECK(bp.AddVariableGetNode("Health", ImVec2(0, 200)) != 0, "variables: Get node created");

		std::string saved = bp.SaveToString();
		BlueprintEditor reloaded;
		CHECK(reloaded.LoadFromString(saved) && reloaded.GetVariables().size() == 1 &&
		          reloaded.GetVariables()[0].name == "Health",
		      "variables: survive save/load");

		CHECK(bp.RemoveVariable("Health") && bp.GetVariables().empty(), "variables: remove");
	}

	// ── Make Struct: field inputs → { Key = value } table for struct args ──
	{
		using PK = BlueprintEditor::PinKind;
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		auto& reg = bp.GetRegistry();
		auto& s = reg.AddClass("FMyStruct", "", "test struct");
		s.AddProperty("X", BlueprintEditor::PinType(PK::Float), "");
		s.AddProperty("Name", BlueprintEditor::PinType(PK::String), "");
		reg.AddClass("Consumer", "", "").AddFunction("Use", "")
			.Static().In("S", BlueprintEditor::PinType(PK::Struct, "FMyStruct")).Metadata("use({0})");

		auto mk = bp.AddMakeStructNode("FMyStruct", ImVec2(0, 0));
		CHECK(mk != 0, "make struct: node created");
		CHECK(bp.SetPinDefaultValue(bp.FindPinID(mk, "X", false), "1.5"), "make struct: set X field");
		CHECK(bp.SetPinDefaultValue(bp.FindPinID(mk, "Name", false), "hi"), "make struct: set Name field");

		auto ev = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(-300, 0));
		auto call = bp.AddCallFunctionNode("Consumer", "Use", ImVec2(300, 0));
		bp.AddLink(bp.FindPinID(ev, "", true), bp.FindPinID(call, "", false));           // exec
		bp.AddLink(bp.FindPinID(mk, "FMyStruct", true), bp.FindPinID(call, "S", false));  // struct data
		std::string script = BlueprintLua::GenerateScript(bp);
		CHECK(script.find("X = 1.5") != std::string::npos, "make struct: numeric field in table");
		CHECK(script.find("Name = \"hi\"") != std::string::npos, "make struct: string field quoted in table");
		CHECK(script.find("use({") != std::string::npos, "make struct: table passed as the struct arg");
	}
#endif // IMGUIIDE_PLUGIN_UEVR

	if (gFailures == 0) {
		std::printf("selftest: all %d checks passed\n", gChecks);
		return 0;
	}
	std::fprintf(stderr, "selftest: %d/%d checks FAILED\n", gFailures, gChecks);
	return 1;
}
