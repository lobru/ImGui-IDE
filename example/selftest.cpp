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
#include <vector>

#include "TextEditor.h"
#include "tsindex.h"

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

	// ── Backslash-continued #define carries preprocessor color to next line ──
	{
		const int kPre = static_cast<int>(TextEditor::Color::preprocessor);
		std::string cpp =
			"#define FOO(x) \\\n"        // line 0: directive, ends with backslash
			"    do { x; } while (0)\n"  // line 1: continuation
			"int after = 1;\n";          // line 2: back to normal code
		CHECK(colorAt(TextEditor::Language::Cpp(), cpp, 0, 0) == kPre,
			"#define line is preprocessor");
		CHECK(colorAt(TextEditor::Language::Cpp(), cpp, 1, 4) == kPre,
			"backslash-continued line is preprocessor");
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
		CHECK(encl("Foo", 1).empty(), "top-level type Foo has no enclosing (self-skip)");
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

	if (gFailures == 0) {
		std::printf("selftest: all %d checks passed\n", gChecks);
		return 0;
	}
	std::fprintf(stderr, "selftest: %d/%d checks FAILED\n", gFailures, gChecks);
	return 1;
}
