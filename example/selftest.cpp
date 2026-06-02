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

	if (gFailures == 0) {
		std::printf("selftest: all %d checks passed\n", gChecks);
		return 0;
	}
	std::fprintf(stderr, "selftest: %d/%d checks FAILED\n", gFailures, gChecks);
	return 1;
}
