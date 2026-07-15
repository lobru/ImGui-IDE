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

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "TextEditor.h"
#include "git_url.h"
#include "notes.h"
#include "screenshot.h"
#include "tsindex.h"
#include "lsp_protocol.h"
#include "nav_history.h"
#include "cppgen.h"
#include "plugin_registry.h"
#ifdef IMGUIIDE_PLUGIN_UNREAL
#include "unreal.h"
#include "unreal_codegen.h"
#include "unreal_plugin.h"
#include "unreal_uasset.h"
#endif
#ifdef IMGUIIDE_PLUGIN_UEVR
#include "BlueprintEditor.h"
#include "BlueprintLua.h"
#include "BlueprintLuaImport.h"
#include "BlueprintRegistryJson.h"
#include "blueprint_snippets.h"
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
	// Dev helper: `selftest --dump-lua` prints a kitchen-sink generated script
	// (variables incl. lazy-init + tables, custom-lua functions, events with the
	// guard/pcall wrapper, For Each, Make Struct) so external Lua tools (stylua,
	// luacheck) can validate the generator's output syntax.
	if (argc > 1 && std::string(argv[1]) == "--dump-lua") {
		using PK = BlueprintEditor::PinKind;
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		bp.AddVariable("Items", BlueprintEditor::PinType(PK::Wildcard, "", true), "");
		bp.AddVariable("Pawn", BlueprintEditor::PinType(PK::Object, "UObject"), "");
		bp.AddVariable("Speed", BlueprintEditor::PinType(PK::Float), "1.5");

		auto& reg = bp.GetRegistry();
		auto& s = reg.AddClass("FTestStruct", "", "");
		s.AddProperty("X", BlueprintEditor::PinType(PK::Float), "");
		reg.AddClass("Consumer", "", "").AddFunction("Use", "")
			.Static().In("S", BlueprintEditor::PinType(PK::Struct, "FTestStruct")).Metadata("use({0})");

		auto tick = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(0, 0));
		auto fetch = bp.AddCallFunctionNode("UEVR_API", "Get Local Pawn", ImVec2(0, 150));
		auto init = bp.AddVariableSetIfUnsetNode("Pawn", ImVec2(250, 0));
		bp.AddLink(bp.FindPinID(tick, "", true), bp.FindPinID(init, "", false));
		bp.AddLink(bp.FindPinID(fetch, "Return Value", true), bp.FindPinID(init, "Pawn", false));

		auto getPawn = bp.AddVariableGetNode("Pawn", ImVec2(250, 150));
		auto valid = bp.AddFlowControlNode("Is Valid", ImVec2(500, 0));
		bp.AddLink(bp.FindPinID(init, "", true), bp.FindPinID(valid, "", false));
		bp.AddLink(bp.FindPinID(getPawn, "", true), bp.FindPinID(valid, "Object", false));

		auto add = bp.AddCallFunctionNode("LuaTable", "Array Add", ImVec2(750, 0));
		auto getItems = bp.AddVariableGetNode("Items", ImVec2(750, 150));
		bp.AddLink(bp.FindPinID(valid, "Is Valid", true), bp.FindPinID(add, "", false));
		bp.AddLink(bp.FindPinID(getItems, "", true), bp.FindPinID(add, "Array", false));
		bp.SetPinDefaultValue(bp.FindPinID(add, "Value", false), "tick");

		auto each = bp.AddFlowControlNode("For Each", ImVec2(1000, 0));
		auto getItems2 = bp.AddVariableGetNode("Items", ImVec2(1000, 200));
		bp.AddLink(bp.FindPinID(add, "", true), bp.FindPinID(each, "", false));
		bp.AddLink(bp.FindPinID(getItems2, "", true), bp.FindPinID(each, "Array", false));

		auto lua = bp.AddCustomLuaNode(ImVec2(1300, 0));
		bp.AddCustomLuaPin(lua, false, "Item");
		bp.AddCustomLuaPin(lua, true, "Out0");
		bp.SetCustomLuaSource(lua, "print(tostring({Item}))\nOut0 = {Item}");
		bp.AddLink(bp.FindPinID(each, "Loop Body", true), bp.FindPinID(lua, "", false));
		bp.AddLink(bp.FindPinID(each, "Element", true), bp.FindPinID(lua, "Item", false));

		auto mk = bp.AddMakeStructNode("FTestStruct", ImVec2(1300, 300));
		bp.SetPinDefaultValue(bp.FindPinID(mk, "X", false), "2.0");
		auto use = bp.AddCallFunctionNode("Consumer", "Use", ImVec2(1600, 0));
		bp.AddLink(bp.FindPinID(each, "Completed", true), bp.FindPinID(use, "", false));
		bp.AddLink(bp.FindPinID(mk, "FTestStruct", true), bp.FindPinID(use, "S", false));

		std::printf("%s\n", BlueprintLua::GenerateScript(bp).c_str());
		return 0;
	}

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

		// The fold scan was extracted into a pure text->ranges function so it can run
		// on a worker (Folder::computeFoldRanges). Prove it still folds correctly at a
		// size well past the async threshold (kAsyncFoldLines = 5000).
		{
			std::string big;
			const int blocks = 3000;              // 3 lines each -> 9000 lines
			big.reserve(blocks * 24);
			for (int i = 0; i < blocks; ++i)
				big += "void f() {\n    int x = 1;\n}\n";
			size_t n = foldCount(TextEditor::Language::Cpp(), big);
			CHECK(n == static_cast<size_t>(blocks),
				"folds: a 9000-line file folds every brace block (pure scan scales)");
		}
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

	// ── SelectAll reaches the true top-left even from a mid-line cursor ──
	{
		TextEditor ed;
		ed.SetText("hello world\nsecond line\nthird\n");
		ed.SetCursor(1, 3); // caret mid second line
		ed.SelectAll();
		auto sel = ed.GetMainCursorSelection();
		CHECK(sel.start.line == 0 && sel.start.column == 0,
		      "SelectAll anchors at line 0, column 0 (regression: kept the cursor's column)");
		CHECK(sel.end.line == 3 || sel.end.line == 2,
		      "SelectAll extends to the last line");
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

	// ── A backslash-continued #define is ONE directive: the whole macro, every
	//    continuation line included, stays in the preprocessor color. ──
	{
		const int kPre = static_cast<int>(TextEditor::Color::preprocessor);
		std::string cpp =
			"#define FOO(x) \\\n"        // 0: directive, ends with a backslash
			"    do { x; } while (0)\n"  // 1: continuation - still the same directive
			"int after = 1;\n";          // 2: back to normal code
		CHECK(colorAt(TextEditor::Language::Cpp(), cpp, 0, 0) == kPre,
			"#define directive line is preprocessor");
		CHECK(colorAt(TextEditor::Language::Cpp(), cpp, 1, 4) == kPre,
			"the backslash-continued line stays preprocessor (the WHOLE macro is one color)");
		CHECK(colorAt(TextEditor::Language::Cpp(), cpp, 1, 20) == kPre,
			"the end of a continuation line is preprocessor too");
		CHECK(colorAt(TextEditor::Language::Cpp(), cpp, 2, 0) != kPre,
			"the line after the last continuation is back to normal code");
	}

	// ── Multi-line (2+ backslash-continued) macro: EVERY line of it is preprocessor,
	//    and normal code resumes on the first line that isn't continued. ──
	{
		const int kPre = static_cast<int>(TextEditor::Color::preprocessor);
		std::string cpp =
			"#define FOO(x) \\\n"    // 0 directive
			"    do { \\\n"          // 1 continuation
			"        x; \\\n"        // 2 continuation
			"    } while (0)\n"      // 3 last continuation (no trailing backslash)
			"int after = 1;\n";      // 4 normal code again
		for (int line = 0; line <= 3; line++)
		{
			CHECK(colorAt(TextEditor::Language::Cpp(), cpp, line, 4) == kPre,
				"multiline macro: every line of the macro is preprocessor");
		}
		CHECK(colorAt(TextEditor::Language::Cpp(), cpp, 4, 0) ==
			  colorAt(TextEditor::Language::Cpp(), "int x;\n", 0, 0),
			"multiline macro: code after the macro colors like normal code (no state bleed)");
	}

	// ── A continuation line's own tokens must NOT win over the directive color:
	//    keywords, strings and comments inside a macro body stay preprocessor, and
	//    a '//' or '"' inside the body must not bleed past the end of the macro. ──
	{
		const int kPre = static_cast<int>(TextEditor::Color::preprocessor);
		const int kCmt = static_cast<int>(TextEditor::Color::comment);
		std::string body =
			"#define LOG(x) \\\n"                          // 0
			"    if (x) { printf(\"hi\"); } /* c */ \\\n"  // 1: col 4 'if', col 20 string, col 30 comment
			"    else { return 0; }\n"                     // 2 last continuation
			"int q = 1;\n";                                // 3 normal code
		CHECK(colorAt(TextEditor::Language::Cpp(), body, 1, 4) == kPre, "macro body: a keyword stays preprocessor");
		CHECK(colorAt(TextEditor::Language::Cpp(), body, 1, 20) == kPre, "macro body: a string literal stays preprocessor");
		CHECK(colorAt(TextEditor::Language::Cpp(), body, 1, 30) == kPre, "macro body: a comment stays preprocessor");
		CHECK(colorAt(TextEditor::Language::Cpp(), body, 2, 4) == kPre, "macro body: the last continuation line is preprocessor");
		CHECK(colorAt(TextEditor::Language::Cpp(), body, 3, 0) != kPre, "code after the macro is not preprocessor");
		CHECK(colorAt(TextEditor::Language::Cpp(), body, 3, 0) != kCmt, "a comment inside a macro body does not bleed past the macro");
	}

	// ── A directive with no trailing backslash ends at its own line ──
	{
		const int kPre = static_cast<int>(TextEditor::Color::preprocessor);
		std::string inc = "#include <vector>\nint x = 1;\n";
		CHECK(colorAt(TextEditor::Language::Cpp(), inc, 0, 0) == kPre, "#include is preprocessor");
		CHECK(colorAt(TextEditor::Language::Cpp(), inc, 0, 12) == kPre, "the whole #include line is preprocessor");
		CHECK(colorAt(TextEditor::Language::Cpp(), inc, 1, 0) != kPre, "the line after a non-continued directive is normal code");
	}

	// ── Off-thread document load (SetTextAsync): the worker-built document must be
	//    byte-identical to a synchronous SetText, colors and all. ──
	{
		std::string big;
		for (int i = 0; i < 4000; i++)
		{
			big += "int fn_" + std::to_string(i) + "(int a) { /* c */ return a + " + std::to_string(i) + "; } // tail\n";
		}

		TextEditor sync;
		sync.SetLanguage(TextEditor::Language::Cpp());
		sync.SetText(big);

		TextEditor async;
		async.SetLanguage(TextEditor::Language::Cpp());
		async.SetTextAsync(big);
		CHECK(async.IsLoading(), "SetTextAsync leaves the editor in a loading state");
		CHECK(async.IsReadOnlyEnabled(), "a loading document is read-only (no typing into a doc about to be replaced)");
		CHECK(async.GetLineCount() <= 1, "a loading document is empty until the worker lands");

		// no ImGui frame here, so poll the load ourselves (render() does this in the app)
		for (int spin = 0; spin < 20000 && !async.PollLoad(); spin++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		CHECK(!async.IsLoading(), "the async load completes");
		CHECK(!async.IsReadOnlyEnabled(), "read-only is released once the load lands");
		CHECK(async.GetLineCount() == sync.GetLineCount(), "async load produces the same line count as SetText");
		CHECK(async.GetText() == sync.GetText(), "async load produces the same text as SetText");
		CHECK(async.GetGlyphColorAt(0, 0) == sync.GetGlyphColorAt(0, 0), "async load colorizes ('int' declaration)");
		CHECK(async.GetGlyphColorAt(10, 16) == sync.GetGlyphColorAt(10, 16), "async load colorizes mid-document glyphs identically");
		CHECK(async.GetFoldCount() == sync.GetFoldCount(), "async load rebuilds the same folds");
	}

	// ── Sticky notes: a note must FOLLOW its line when the file changes, and be
	//    honestly marked orphaned when the line it was written on is gone. ──
	{
		std::vector<std::string> lines = {
			"#include <vector>",   // 0
			"",                    // 1
			"int compute(int a)",  // 2
			"{",                   // 3
			"    return a * 2;",   // 4
			"}",                   // 5
		};

		// a note that hasn't moved stays put
		notes::Note n;
		n.file = "src/a.cpp";
		n.line = 4;
		n.anchor = "    return a * 2;";
		n.text = "off by one?";
		notes::reanchor(n, lines);
		CHECK(n.line == 4 && !n.orphaned, "note on an unchanged line keeps its line");

		// insert 3 lines above it: the note must follow the CODE, not the number
		std::vector<std::string> shifted = {
			"#include <vector>", "#include <string>", "#include <map>", "",
			"int compute(int a)", "{", "    return a * 2;", "}",
		};
		notes::reanchor(n, shifted);
		CHECK(n.line == 6 && !n.orphaned, "note follows its line down when lines are inserted above");

		// delete lines above it: it must follow back up
		std::vector<std::string> pulled = { "int compute(int a)", "{", "    return a * 2;", "}" };
		notes::reanchor(n, pulled);
		CHECK(n.line == 2 && !n.orphaned, "note follows its line up when lines are removed above");

		// a pure re-indent must NOT orphan the note (anchors compare trimmed)
		std::vector<std::string> reindented = { "int compute(int a)", "{", "\t\treturn a * 2;", "}" };
		notes::reanchor(n, reindented);
		CHECK(n.line == 2 && !n.orphaned, "re-indenting a line does not orphan its note");

		// the line is gone entirely -> orphaned, not silently re-pointed at code
		std::vector<std::string> rewritten = { "int compute(int a)", "{", "    return a << 1;", "}" };
		notes::reanchor(n, rewritten);
		CHECK(n.orphaned, "a note whose line is deleted is marked orphaned");
		CHECK(n.line >= 0 && n.line < (int)rewritten.size(), "an orphaned note's line stays in range");

		// nearest match wins: a duplicate line elsewhere must not steal the note
		std::vector<std::string> dupes = {
			"    return a * 2;",   // 0 - a decoy far from the note
			"", "", "", "", "", "", "",
			"    return a * 2;",   // 8 - the one right next to where it was
			"",
		};
		notes::Note d;
		d.file = "src/a.cpp";
		d.line = 9;
		d.anchor = "    return a * 2;";
		d.text = "here";
		notes::reanchor(d, dupes);
		CHECK(d.line == 8, "re-anchoring picks the NEAREST matching line, not the first in the file");

		// reanchorFile only touches the named file
		std::vector<notes::Note> all = { n, d };
		all[0].file = "other.cpp";
		all[0].line = 99;
		notes::reanchorFile(all, "src/a.cpp", dupes);
		CHECK(all[0].line == 99, "reanchorFile leaves notes for other files alone");

		// JSON round trip preserves every field
		notes::Note full;
		full.file = "src/b.cpp";
		full.line = 12;
		full.text = "why is this \"quoted\" and \\escaped?";
		full.anchor = "auto x = 1;";
		full.commit = "abc1234";
		full.author = "Logan";
		full.epoch = 1780000000LL;
		full.resolved = true;

		auto round = notes::fromJson(notes::toJson({full}));
		CHECK(round.size() == 1, "notes JSON round trip keeps the note");
		CHECK(round[0].file == full.file && round[0].line == full.line, "round trip keeps file + line");
		CHECK(round[0].text == full.text, "round trip keeps text with quotes and backslashes");
		CHECK(round[0].commit == "abc1234" && round[0].author == "Logan", "round trip keeps the git stamp");
		CHECK(round[0].resolved, "round trip keeps resolved");

		// a corrupt / hand-mangled sidecar means "no notes", never a crash
		CHECK(notes::fromJson("{ not json at all").empty(), "a corrupt notes sidecar parses to zero notes");
		CHECK(notes::fromJson("[{\"line\":3}]").empty(), "a note with no file/text is dropped");

		// line shifts within a session
		std::vector<notes::Note> shiftMe = { full, full };
		shiftMe[0].line = 5;
		shiftMe[1].line = 20;
		notes::shiftLines(shiftMe, "src/b.cpp", 10, +3);
		CHECK(shiftMe[0].line == 5, "shiftLines leaves notes above the edit alone");
		CHECK(shiftMe[1].line == 23, "shiftLines pushes notes below the edit down");
	}

	// ── Git remote → web URL normalization ──
	{
		CHECK(gitRemoteToWebUrl("git@github.com:lobru/ImGui-IDE.git") == "https://github.com/lobru/ImGui-IDE",
			  "scp-style git@ remote → https, .git stripped");
		CHECK(gitRemoteToWebUrl("ssh://git@github.com/lobru/ImGui-IDE.git") == "https://github.com/lobru/ImGui-IDE",
			  "ssh:// remote → https, git@ authority stripped");
		CHECK(gitRemoteToWebUrl("https://github.com/lobru/ImGui-IDE.git") == "https://github.com/lobru/ImGui-IDE",
			  "https remote keeps host, .git stripped");
		CHECK(gitRemoteToWebUrl("http://example.com/a/b") == "https://example.com/a/b",
			  "http upgraded to https");
		CHECK(gitRemoteToWebUrl("git@gitlab.com:group/sub/proj.git") == "https://gitlab.com/group/sub/proj",
			  "nested path on scp-style remote");
		CHECK(gitRemoteToWebUrl("").empty(), "empty remote → empty");
		CHECK(gitRemoteToWebUrl("/local/path/repo").empty(), "a local path is not a web URL");
		CHECK(gitRemoteToWebUrl("file:///srv/git/x.git").empty(), "file:// is not a web URL");
	}

	// ── Screenshot PNG encoder. Hand-rolled (STORED deflate + CRC32 + Adler32), so
	//    it gets checked structurally rather than trusted. ──
	{
		const int W = 7, H = 5; // deliberately not a power of two / not stride-aligned
		std::vector<uint8_t> px((size_t)W * H * 4);
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++)
			{
				uint8_t *p = &px[((size_t)y * W + x) * 4];
				p[0] = (uint8_t)(x * 30);
				p[1] = (uint8_t)(y * 50);
				p[2] = 0x7F;
				p[3] = 0x11; // must come out opaque regardless
			}

		std::string path = "selftest-shot.png";
		CHECK(screenshot::writePng(path, px.data(), W, H, W * 4), "writePng writes a file");

		std::ifstream in(path, std::ios::binary);
		std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		in.close();
		CHECK(blob.size() > 60, "the PNG is non-trivial");

		auto be32 = [&](size_t at) {
			return ((uint32_t)(uint8_t)blob[at] << 24) | ((uint32_t)(uint8_t)blob[at + 1] << 16) |
				   ((uint32_t)(uint8_t)blob[at + 2] << 8) | (uint32_t)(uint8_t)blob[at + 3];
		};

		CHECK(blob.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0, "PNG signature");
		CHECK(blob.compare(12, 4, "IHDR") == 0, "first chunk is IHDR");
		CHECK(be32(16) == (uint32_t)W && be32(20) == (uint32_t)H, "IHDR carries the right width/height");
		CHECK((uint8_t)blob[24] == 8 && (uint8_t)blob[25] == 6, "IHDR says 8-bit RGBA");
		CHECK(blob.find("IDAT") != std::string::npos, "an IDAT chunk is present");
		CHECK(blob.compare(blob.size() - 8, 4, "IEND") == 0, "the file ends with IEND");

		// Walk the chunks and verify EVERY CRC — a bad CRC is the classic way a
		// hand-rolled PNG "works" in one viewer and is rejected by the next.
		auto crcOf = [](const uint8_t *d, size_t n) {
			uint32_t crc = 0xFFFFFFFFu;
			for (size_t i = 0; i < n; i++)
			{
				crc ^= d[i];
				for (int k = 0; k < 8; k++)
					crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
			}
			return crc ^ 0xFFFFFFFFu;
		};

		size_t at = 8;
		int chunks = 0;
		bool crcOk = true;
		while (at + 12 <= blob.size())
		{
			uint32_t len = be32(at);
			const uint8_t *typeAndData = (const uint8_t *)blob.data() + at + 4;
			uint32_t stored = be32(at + 8 + len);
			if (crcOf(typeAndData, len + 4) != stored)
				crcOk = false;
			chunks++;
			at += 12 + len;
		}
		CHECK(chunks == 3, "the PNG has exactly IHDR + IDAT + IEND");
		CHECK(crcOk, "every chunk CRC-32 validates");
		CHECK(at == blob.size(), "the chunk walk consumes the file exactly (no trailing garbage)");

		// BGRA -> RGBA swaps the right channels and leaves alpha alone
		uint8_t bgra[4] = {0x11, 0x22, 0x33, 0x44};
		screenshot::bgraToRgba(bgra, 4);
		CHECK(bgra[0] == 0x33 && bgra[1] == 0x22 && bgra[2] == 0x11 && bgra[3] == 0x44,
			  "bgraToRgba swaps R and B and leaves G/A untouched");

		std::remove(path.c_str());
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

		// ── Runtime symbol packs: a registered type completes like a compiled one,
		//    a pointer handed out before more registrations stays valid, and merging
		//    into an EXISTING compiled type dedups rather than duplicating. ──
		{
			CHECK(ts::registeredTypeCount() == 0, "registry starts empty");
			CHECK(ts::stlMembers("MyPackType") == nullptr, "an unregistered pack type has no members");

			ts::registerTypeMembers("MyPackType", {"alpha", "beta", "gamma"});
			const auto *mine = ts::stlMembers("MyPackType");
			CHECK(mine != nullptr && has(mine, "alpha") && has(mine, "gamma"),
				  "a registered pack type completes like a compiled one");
			CHECK(ts::registeredTypeCount() == 1, "registry counts the new type");

			// pointer stability: register more types, the earlier pointer must survive
			for (int i = 0; i < 200; i++)
				ts::registerTypeMembers("Filler" + std::to_string(i), {"x", "y"});
			CHECK(mine == ts::stlMembers("MyPackType"),
				  "a member-list pointer stays valid across later registrations (node-based map)");
			CHECK(has(ts::stlMembers("MyPackType"), "beta"), "and still resolves its members");

			// merging into a type: dedup, order-preserving, additive
			ts::registerTypeMembers("MyPackType", {"alpha", "delta"}); // alpha dup, delta new
			const auto *merged = ts::stlMembers("MyPackType");
			size_t alphas = 0;
			for (const auto &m : *merged)
				if (m == "alpha")
					alphas++;
			CHECK(alphas == 1, "re-registering a member does not duplicate it");
			CHECK(has(merged, "delta"), "re-registering adds the genuinely new member");

			// a pack merging into a COMPILED type augments it without losing the originals
			ts::registerTypeMembers("vector", {"custom_pack_method"});
			const auto *v2 = ts::stlMembers("vector");
			CHECK(has(v2, "push_back") && has(v2, "custom_pack_method"),
				  "a pack augments a compiled type (keeps push_back, adds the new one)");

			ts::registerTypeMembers("", {"nope"});
			ts::registerTypeMembers("Empty", {});
			CHECK(ts::stlMembers("Empty") == nullptr, "registering an empty member list is a no-op");
		}

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

		// ── .uplugin recognition: a standalone plugin repo (no .uproject) is still
		//    recognized as an Unreal project, with the engine taken from EngineVersion. ──
		{
			auto pluginRoot = root / "PluginRepo";
			auto uplugin = pluginRoot / "MyPlugin" / "MyPlugin.uplugin";   // nested <Name>/<Name>.uplugin
			fs::create_directories(uplugin.parent_path(), ec);
			std::ofstream pf(uplugin);
			pf << "{ \"FileVersion\": 3, \"EngineVersion\": \"5.4.0\", \"Modules\": [] }\n";
			pf.close();
			mk(pluginRoot / "MyPlugin" / "Source" / "MyPlugin" / "MyPlugin.cpp");

			// a plain findUProject sees nothing here…
			CHECK(unreal::findUProject(pluginRoot).empty(), "UE: no .uproject in a plugin-only repo");
			// …but findUProjectOrPlugin finds the .uplugin, even nested one level down
			CHECK(unreal::findUProjectOrPlugin(pluginRoot) == uplugin,
				"UE: findUProjectOrPlugin finds a nested .uplugin");
			CHECK(unreal::isPluginDescriptor(uplugin), "UE: .uplugin is a plugin descriptor");
			CHECK(!unreal::isPluginDescriptor(uproj), "UE: .uproject is NOT a plugin descriptor");
			CHECK(unreal::hasCppSource(uplugin), "UE: plugin Source/ detected");

			// EngineVersion "5.4.0" reduces to the "5.4" association the launcher registers
			std::string assoc;
			unreal::findEngineRoot(uplugin, assoc);
			CHECK(assoc == "5.4", "UE: .uplugin EngineVersion 5.4.0 -> association 5.4");

			// a real .uproject still wins when both are reachable (richer descriptor)
			CHECK(unreal::findUProjectOrPlugin(gameHdr.parent_path()) == uproj,
				"UE: a .uproject is preferred over a .uplugin");

			// ── Tolerant descriptor parsing: a real UE .uproject with a TRAILING COMMA
			//    (UE's own editor writes them) must still yield its EngineAssociation.
			//    This is the "ue source linking broken" bug: a strict parse discarded
			//    the file, so the engine was never found and includes wouldn't resolve. ──
			auto lenientProj = root / "Lenient" / "Lenient.uproject";
			fs::create_directories(lenientProj.parent_path(), ec);
			{
				std::ofstream lf(lenientProj);
				lf << "{\n"
				      "  \"FileVersion\": 3,\n"
				      "  \"EngineAssociation\": \"5.4\",\n"
				      "  \"Modules\": [\n"
				      "    { \"Name\": \"Lenient\", \"AdditionalDependencies\": [ \"Engine\", \"CoreUObject\", ] },\n" // trailing comma
				      "  ],\n"   // trailing comma
				      "}\n";
			}
			std::string lassoc;
			unreal::findEngineRoot(lenientProj, lassoc);
			CHECK(lassoc == "5.4",
				"UE: a .uproject with trailing commas still yields EngineAssociation (tolerant parse)");

			// the stripper itself: drops trailing commas + comments, keeps string content
			CHECK(unreal::stripJsonLeniencies("[1,2,]") == "[1,2]", "strip: trailing comma in array");
			CHECK(unreal::stripJsonLeniencies("{\"a\":1,}") == "{\"a\":1}", "strip: trailing comma in object");
			CHECK(unreal::stripJsonLeniencies("[\"a,\",]") == "[\"a,\"]",
				"strip: a comma INSIDE a string is preserved; only the trailing one drops");
			{
				std::string commented = "{ \"x\": 1 // note\n}";
				CHECK(unreal::stripJsonLeniencies(commented).find("//") == std::string::npos,
					"strip: line comment removed");
			}

			// ── availablePlugins: enumerate .uplugin names from project + engine trees ──
			{
				mk(root / "PluginRepo2" / "Plugins" / "AwesomeTool" / "AwesomeTool.uplugin");
				mk(root / "PluginRepo2" / "Plugins" / "Nested" / "Deep" / "DeepPlugin.uplugin");
				mk(engine / "Engine" / "Plugins" / "Runtime" / "GameplayAbilities" / "GameplayAbilities.uplugin");

				auto plugs = unreal::availablePlugins(engine, root / "PluginRepo2");
				auto hasP = [&](const char *n) {
					return std::find(plugs.begin(), plugs.end(), std::string(n)) != plugs.end();
				};
				CHECK(hasP("AwesomeTool"), "availablePlugins: finds a project plugin");
				CHECK(hasP("DeepPlugin"), "availablePlugins: finds a nested project plugin");
				CHECK(hasP("GameplayAbilities"), "availablePlugins: finds an engine plugin");
				// sorted + deduped
				CHECK(std::is_sorted(plugs.begin(), plugs.end()), "availablePlugins: result is sorted");
				// empty roots don't crash / add nothing
				CHECK(unreal::availablePlugins({}, {}).empty(), "availablePlugins: empty roots -> empty");
			}

			// ── availableModules: enumerate <Name>.Build.cs across Source + Plugins ──
			{
				auto proj = root / "ModProj";
				mk(proj / "Source" / "ModGame" / "ModGame.Build.cs");
				mk(proj / "Source" / "ModGameEditor" / "ModGameEditor.Build.cs");
				mk(proj / "Plugins" / "MyPlug" / "Source" / "MyPlugRuntime" / "MyPlugRuntime.Build.cs");
				mk(proj / "Source" / "ModGame" / "ModGame.cpp"); // not a .Build.cs → ignored

				auto mods = unreal::availableModules(proj);
				auto hasM = [&](const char *n) {
					return std::find(mods.begin(), mods.end(), std::string(n)) != mods.end();
				};
				CHECK(hasM("ModGame"), "availableModules: finds a project module");
				CHECK(hasM("ModGameEditor"), "availableModules: finds a second project module");
				CHECK(hasM("MyPlugRuntime"), "availableModules: finds a module inside a project plugin");
				CHECK(!hasM("ModGame.cpp") && !hasM("ModGame.Build"),
					"availableModules: the name is the module, not the filename");
				CHECK(std::is_sorted(mods.begin(), mods.end()), "availableModules: sorted");
				CHECK(unreal::availableModules({}).empty(), "availableModules: empty root -> empty");
			}
		}
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
			std::string hostActiveFilename() const override { return {}; }
			void hostToast(const std::string&) override {}
			void hostError(const std::string&) override {}
			void hostSendToClaude(const std::string&) override {}
			void hostSuppressAppShortcuts() override {}
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

	// ── UE class-wizard codegen ─────────────────────────────────────────────
	{
		using namespace unreal::codegen;
		ClassSpec actor{"MyActor", "AActor", "MyGame"};
		CHECK(prefixedClassName(actor) == "AMyActor", "ue codegen: A prefix from actor parent");
		std::string h = generateClassHeader(actor);
		CHECK(h.find("UCLASS()") != std::string::npos &&
		          h.find("class MYGAME_API AMyActor : public AActor") != std::string::npos &&
		          h.find("GENERATED_BODY()") != std::string::npos &&
		          h.find("#include \"MyActor.generated.h\"") != std::string::npos,
		      "ue codegen: actor header has UCLASS/API/GENERATED_BODY/.generated.h");
		std::string c = generateClassSource(actor);
		CHECK(c.find("AMyActor::AMyActor()") != std::string::npos &&
		          c.find("PrimaryActorTick.bCanEverTick = true") != std::string::npos &&
		          c.find("Super::Tick(DeltaTime)") != std::string::npos,
		      "ue codegen: actor source has ctor/tick boilerplate");

		ClassSpec comp{"MyComp", "UActorComponent", "MyGame"};
		CHECK(prefixedClassName(comp) == "UMyComp", "ue codegen: U prefix from component parent");
		CHECK(generateClassHeader(comp).find("TickComponent") != std::string::npos,
		      "ue codegen: component header has TickComponent");

		ClassSpec pawn{"MyPawn", "APawn", "MyGame"};
		CHECK(generateClassHeader(pawn).find("SetupPlayerInputComponent") != std::string::npos,
		      "ue codegen: pawn header binds input");

		ClassSpec obj{"MyData", "UObject", "MyGame"};
		CHECK(generateClassSource(obj).find("::MyData()") == std::string::npos,
		      "ue codegen: plain UObject source is boilerplate-free");
		CHECK(generateClassHeader(ClassSpec{"X", "NotAParent", "M"}).empty(),
		      "ue codegen: unknown parent rejected");

		// Verse scaffold
		CHECK(sanitizeVerseName("My Device!") == "my_device", "verse: name sanitized to snake_case");
		CHECK(sanitizeVerseName("9lives") == "device_9lives", "verse: leading digit prefixed");
		std::string v = generateVerseDevice("hud_timer");
		CHECK(v.find("hud_timer := class(creative_device):") != std::string::npos &&
		          v.find("OnBegin<override>()<suspends>:void=") != std::string::npos,
		      "verse: creative_device scaffold shape");
	}

	// ── UAsset package reader (synthetic versioned package) ────────────────
	{
		using namespace unreal::uasset;
		std::string pkg;
		auto putU32 = [&](uint32_t v) { pkg.append(reinterpret_cast<const char*>(&v), 4); };
		auto putU16 = [&](uint16_t v) { pkg.append(reinterpret_cast<const char*>(&v), 2); };
		auto putStr = [&](const std::string& s) { putU32(static_cast<uint32_t>(s.size() + 1)); pkg += s; pkg += '\0'; };

		putU32(0x9E2A83C1u);            // tag
		putU32(static_cast<uint32_t>(-7)); // legacy version (UE4-era)
		putU32(0);                      // legacy UE3
		putU32(522);                    // FileVersionUE4 (>= all our gates)
		putU32(0);                      // licensee
		putU32(0);                      // custom versions: none
		putU32(0);                      // TotalHeaderSize (unused by the parser)
		putStr("None");                 // folder
		putU32(0x00000001u);            // flags
		putU32(3);                      // NameCount
		size_t nameOffsetPos = pkg.size();
		putU32(0);                      // NameOffset (patched)
		putStr("");                     // LocalizationId (>=516) — serialized empty string
		putU32(0); putU32(0);           // gatherable text (>=459)
		putU32(0); putU32(0);           // exports
		putU32(1);                      // ImportCount
		size_t importOffsetPos = pkg.size();
		putU32(0);                      // ImportOffset (patched)

		uint32_t nameOffset = static_cast<uint32_t>(pkg.size());
		std::memcpy(&pkg[nameOffsetPos], &nameOffset, 4);
		for (const char* n : {"/Script/Engine", "StaticMesh", "SM_Rock"}) {
			putStr(n);
			putU16(0); putU16(0);       // name hashes (>=504)
		}

		uint32_t importOffset = static_cast<uint32_t>(pkg.size());
		std::memcpy(&pkg[importOffsetPos], &importOffset, 4);
		putU32(0); putU32(0);           // ClassPackage FName -> names[0]
		putU32(1); putU32(0);           // ClassName    FName -> names[1]
		putU32(0);                      // OuterIndex
		putU32(2); putU32(0);           // ObjectName   FName -> names[2]

		Summary s = parse(pkg.data(), pkg.size());
		CHECK(s.valid, "uasset: synthetic package parses");
		CHECK(s.fileVersionUE4 == 522 && !s.unversioned, "uasset: versions read");
		CHECK(s.names.size() == 3 && s.names[1] == "StaticMesh", "uasset: name table read");
		CHECK(s.imports.size() == 1 && s.imports[0].classPackage == "/Script/Engine" &&
		          s.imports[0].className == "StaticMesh" && s.imports[0].objectName == "SM_Rock",
		      "uasset: import map resolves FNames");
		CHECK(report(s, "test").find("SM_Rock") != std::string::npos, "uasset: report includes imports");

		Summary bad = parse("nope", 4);
		CHECK(!bad.valid && !bad.error.empty(), "uasset: non-package rejected with a reason");

		// analyzeBlueprint: a non-BP asset isn't flagged; a BP-shaped one is, with
		// its generated class + a best-effort parent.
		BlueprintSummary nb = analyzeBlueprint(s);
		CHECK(!nb.isBlueprint, "blueprint: a StaticMesh package is not a Blueprint");

		Summary bpSum;
		bpSum.valid = true;
		bpSum.names = {"BP_Door", "BP_Door_C", "Actor", "BlueprintGeneratedClass"};
		bpSum.imports.push_back(Import{"/Script/Engine", "Class", "Actor", 0});
		bpSum.imports.push_back(Import{"/Script/Engine", "BlueprintGeneratedClass", "BP_Door_C", 0});
		BlueprintSummary bp = analyzeBlueprint(bpSum);
		CHECK(bp.isBlueprint, "blueprint: BlueprintGeneratedClass import detected");
		CHECK(bp.generatedClass == "BP_Door_C", "blueprint: generated class is the *_C name");
		CHECK(bp.parentClass == "Actor", "blueprint: best-effort parent is the referenced native class");
		CHECK(report(bpSum, "bp").find("Blueprint asset") != std::string::npos,
		      "blueprint: report gains a Blueprint section");

		// JSON export: valid-shaped, carries the blueprint object + names/imports.
		std::string js = toJson(bpSum, "bp.uasset");
		CHECK(js.front() == '{' && js.find("\"blueprint\"") != std::string::npos &&
		          js.find("\"generatedClass\": \"BP_Door_C\"") != std::string::npos,
		      "json export: structured JSON with the blueprint object");
		CHECK(js.find("\"names\"") != std::string::npos && js.find("\"imports\"") != std::string::npos,
		      "json export: includes names + imports");

		// The plugin claims .uasset (binary) so opening one shows a report in a tab
		// instead of launching the external app — else inspection is unreachable.
		struct CapHost : PluginHost {
			std::string opened;
			std::filesystem::path hostProjectRoot() const override { return {}; }
			void hostSetProjectRoot(const std::string&) override {}
			void hostOpenFile(const std::string& p) override { opened = p; }
			void hostOpenLuaTab(const std::string&) override {}
			std::string hostActiveText() const override { return {}; }
			std::string hostActiveSelection() const override { return {}; }
			std::string hostActiveFilename() const override { return {}; }
			void hostToast(const std::string&) override {}
			void hostError(const std::string&) override {}
			void hostSendToClaude(const std::string&) override {}
			void hostSuppressAppShortcuts() override {}
			void hostRunInDir(const std::string&, const std::filesystem::path&) override {}
			void hostRunProjectBuild() override {}
			std::filesystem::path hostExeDir() const override { return {}; }
			std::filesystem::path hostRepoRoot() const override { return {}; }
			bool hostGetFlag(const std::string&, bool d) const override { return d; }
			void hostSetFlag(const std::string&, bool) override {}
			bool hostPanInverted() const override { return false; }
			void hostMiddleMousePanScroll(int) override {}
			void hostAugmentCppLanguage(const std::vector<std::string>&, const std::vector<std::string>&,
			                            bool (*)(const std::string&)) override {}
		} host;

		std::error_code wec;
		std::filesystem::path tmp = std::filesystem::temp_directory_path(wec) / "selftest_probe.uasset";
		{ std::ofstream(tmp, std::ios::binary).write(pkg.data(), (std::streamsize) pkg.size()); }
		UnrealPlugin plugin;
		CHECK(plugin.openFile(host, tmp), "uasset open: plugin claims a .uasset");
		CHECK(host.opened.find(".uasset.json") != std::string::npos,
		      "uasset open: it opens a JSON inspection tab (coloured/foldable), not the external app");
		host.opened.clear();
		CHECK(!plugin.openFile(host, "notes.txt") && host.opened.empty(),
		      "uasset open: non-asset files are left for the host");
		std::filesystem::remove(tmp, wec);
	}

	// ── Interactive descriptor editing (.uproject module/plugin adder) ──────
	{
		std::string err;
		std::string base = "{\n  \"FileVersion\": 3,\n  \"Modules\": [\n    {\n      \"Name\": \"Game\",\n"
		                   "      \"Type\": \"Runtime\",\n      \"LoadingPhase\": \"Default\"\n    }\n  ]\n}\n";

		std::string withPlugin = unreal::descriptorAddPlugin(base, "EnhancedInput", true, err);
		CHECK(!withPlugin.empty() && withPlugin.find("\"EnhancedInput\"") != std::string::npos &&
		          withPlugin.find("\"Plugins\"") != std::string::npos,
		      "descriptor: adds a Plugins array + the dependency");

		std::string withMod = unreal::descriptorAddModule(base, "GameEditor", "Editor", "PostEngineInit", err);
		CHECK(withMod.find("\"GameEditor\"") != std::string::npos &&
		          withMod.find("\"Type\": \"Editor\"") != std::string::npos &&
		          withMod.find("\"LoadingPhase\": \"PostEngineInit\"") != std::string::npos,
		      "descriptor: adds a module with Type + LoadingPhase from the lists");

		// idempotent: adding the same plugin twice doesn't duplicate it
		std::string twice = unreal::descriptorAddPlugin(withPlugin, "EnhancedInput", false, err);
		size_t first = twice.find("EnhancedInput");
		CHECK(first != std::string::npos && twice.find("EnhancedInput", first + 1) == std::string::npos,
		      "descriptor: re-adding a plugin updates in place (no duplicate)");

		CHECK(unreal::descriptorAddPlugin("not json", "X", true, err).empty() && !err.empty(),
		      "descriptor: invalid JSON is rejected with a reason");

		CHECK(!unreal::moduleTypes().empty() && !unreal::loadingPhases().empty() &&
		          unreal::loadingPhases()[0] == "Default",
		      "descriptor: Type + LoadingPhase picklists populated");
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
			std::string hostActiveFilename() const override { return {}; }
			void hostToast(const std::string&) override {}
			void hostError(const std::string&) override {}
			void hostSendToClaude(const std::string&) override {}
			void hostSuppressAppShortcuts() override {}
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
			// A pure virtual gets NO out-of-line definition, but is now surfaced as a
			// marker comment so it isn't silently "lost" from the class.
			CHECK(all.find("void Widget::draw()") == std::string::npos,
			      "cppgen: all-defs generates no body for pure draw");
			CHECK(all.find("draw()") != std::string::npos && all.find("pure virtual") != std::string::npos,
			      "cppgen: pure virtual draw is surfaced as a marker comment, not dropped");
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

		BlueprintEditor hmd;
		BlueprintLua::SetupUEVRRegistry(hmd);
		templates[3].build(hmd);
		std::string h3 = BlueprintLua::GenerateScript(hmd);
		CHECK(h3.find("is_hmd_active") != std::string::npos && h3.find("if ") != std::string::npos,
		      "template hmd-gate: Branch on is_hmd_active generates an if");

		BlueprintEditor fl;
		BlueprintLua::SetupUEVRRegistry(fl);
		templates[4].build(fl);
		std::string f4 = BlueprintLua::GenerateScript(fl);
		CHECK(f4.find("tostring") != std::string::npos && f4.find("dt=") != std::string::npos,
		      "template log-dt: builds a string from the tick's Delta Seconds");
	}

	// ── Snippets: INSERT a wired cluster (unlike templates, no clear) ───────
	{
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		auto ev = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(0, 0));
		size_t before = bp.GetNodeCount();
		CHECK(!BlueprintSnippets::All().empty(), "snippets: at least one snippet");
		BlueprintSnippets::All()[0].insert(bp); // For Loop (print index)
		CHECK(bp.GetNodeCount() > before + 1, "snippets: For Loop inserts multiple nodes");
		CHECK(bp.GetNode(ev) != nullptr, "snippets: existing graph preserved (no clear)");

		BlueprintEditor::ID loopId = 0;
		for (auto& n : bp.GetNodes())
			if (n.kind == BlueprintEditor::NodeKind::FlowControl && n.memberName == "For Loop")
				loopId = n.id;
		CHECK(loopId != 0, "snippets: For Loop flow node present");
		bp.AddLink(bp.FindPinID(ev, "", true), bp.FindPinID(loopId, "", false));
		CHECK(BlueprintLua::GenerateScript(bp).find("for ") != std::string::npos,
		      "snippets: wired For Loop generates a for-loop");
	}

	// ── Table snippets + table UI template ──────────────────────────────────
	{
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		auto& snippets = BlueprintSnippets::All();
		CHECK(snippets.size() >= 7, "snippets: table/map/guarded snippets registered");

		auto ev = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(-400, 0));
		snippets[1].insert(bp); // "For Each over table"
		CHECK(bp.GetVariables().size() == 1 && bp.GetVariables()[0].name == "Items",
		      "snippet table: creates the Items variable");

		BlueprintEditor::ID addNode = 0;
		for (auto& n : bp.GetNodes())
			if (n.memberName == "Array Add")
				addNode = n.id;
		CHECK(addNode != 0, "snippet table: Array Add present");
		bp.AddLink(bp.FindPinID(ev, "", true), bp.FindPinID(addNode, "", false));
		std::string script = BlueprintLua::GenerateScript(bp);
		CHECK(script.find("table.insert(Items, \"hello\")") != std::string::npos &&
		          script.find("in ipairs((Items) or {})") != std::string::npos,
		      "snippet table: append + iterate codegen");

		BlueprintEditor bp2;
		BlueprintLua::SetupUEVRRegistry(bp2);
		auto ev2 = bp2.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(-400, 0));
		snippets[3].insert(bp2); // "Guarded pawn fetch"
		BlueprintEditor::ID initNode = 0;
		for (auto& n : bp2.GetNodes())
			if (n.kind == BlueprintEditor::NodeKind::VariableSet)
				initNode = n.id;
		CHECK(initNode != 0, "snippet guarded: Set If Unset present");
		bp2.AddLink(bp2.FindPinID(ev2, "", true), bp2.FindPinID(initNode, "", false));
		std::string s2 = BlueprintLua::GenerateScript(bp2);
		CHECK(s2.find("Pawn = Pawn or (") != std::string::npos && s2.find("~= nil then") != std::string::npos,
		      "snippet guarded: lazy init + Is Valid gate codegen");

		BlueprintEditor tpl;
		BlueprintLua::SetupUEVRRegistry(tpl);
		BlueprintTemplates::All()[5].build(tpl); // "List a table in a UI panel"
		std::string t = BlueprintLua::GenerateScript(tpl);
		CHECK(t.find("begin_window") != std::string::npos && t.find("in ipairs(") != std::string::npos &&
		          t.find("imgui.text(") != std::string::npos && t.find("end_window") != std::string::npos,
		      "template table-ui: window + iteration + per-row text");
	}

	// ── Live SDK dump: the bridge's SDKDUMP JSON loads into an index ────────
	{
		// The exact shape kSdkDumpChunk emits (leaf class + super + properties).
		std::string json = R"JSON({"classes":[{"name":"BP_PlayerPawn_C","super":"Character",
			"full_name":"BlueprintGeneratedClass /Game/BP_PlayerPawn.BP_PlayerPawn_C",
			"functions":[],"properties":[{"name":"Health","type":"FloatProperty"},
			{"name":"Inventory","type":"ArrayProperty"}]}]})JSON";
		BlueprintEditor::TypeRegistry index;
		std::string error;
		CHECK(BlueprintRegistryJson::Load(index, json, error), "live sdk: dump JSON loads");
		const auto* cls = index.FindClass("BP_PlayerPawn_C");
		CHECK(cls != nullptr && cls->parentName == "Character" && cls->properties.size() == 2,
		      "live sdk: class + properties indexed");
		CHECK(cls->properties[0].type.kind == BlueprintEditor::PinKind::Float,
		      "live sdk: property types map to pin kinds");
	}

	// ── Script safety + new codegen semantics ───────────────────────────────
	{
		using PK = BlueprintEditor::PinKind;
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);

		// table-typed variable declares an empty table
		bp.AddVariable("Items", BlueprintEditor::PinType(PK::Wildcard, "", true), "");

		auto tick = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(0, 0));
		auto print = bp.AddCallFunctionNode("UEVR_API", "Print", ImVec2(300, 0));
		bp.AddLink(bp.FindPinID(tick, "", true), bp.FindPinID(print, "", false));
		std::string script = BlueprintLua::GenerateScript(bp);

		CHECK(script.find("__bp_gen = (__bp_gen or 0) + 1") != std::string::npos &&
		          script.find("if __gen ~= __bp_gen then return end") != std::string::npos,
		      "codegen: generation guard emitted (BP reset support)");
		CHECK(script.find("pcall(function()") != std::string::npos &&
		          script.find("if not __ok then print(") != std::string::npos,
		      "codegen: callbacks are pcall-wrapped");
		CHECK(script.find("local Items = {}") != std::string::npos,
		      "codegen: table variable declares an empty table");

		// Set If Unset emits the lazy-init idiom and survives save/load
		bp.AddVariable("Pawn", BlueprintEditor::PinType(PK::Object, "UObject"), "");
		auto lazy = bp.AddVariableSetIfUnsetNode("Pawn", ImVec2(300, 200));
		CHECK(lazy != 0, "codegen: Set If Unset node created");
		auto pawnFn = bp.AddCallFunctionNode("UEVR_API", "Get Local Pawn", ImVec2(0, 200));
		bp.AddLink(bp.FindPinID(print, "", true), bp.FindPinID(lazy, "", false));
		bp.AddLink(bp.FindPinID(pawnFn, "Return Value", true), bp.FindPinID(lazy, "Pawn", false));
		script = BlueprintLua::GenerateScript(bp);
		CHECK(script.find("Pawn = Pawn or (") != std::string::npos,
		      "codegen: Set If Unset emits X = X or (value)");

		BlueprintEditor reloaded;
		CHECK(reloaded.LoadFromString(bp.SaveToString()), "codegen: graph with markers reloads");
		CHECK(BlueprintLua::GenerateScript(reloaded).find("Pawn = Pawn or (") != std::string::npos,
		      "codegen: the Set-If-Unset marker survives save/load");
	}

	// ── For Each flow + CustomLua-as-local-function ─────────────────────────
	{
		using PK = BlueprintEditor::PinKind;
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		bp.AddVariable("Items", BlueprintEditor::PinType(PK::Wildcard, "", true), "");

		auto tick = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(0, 0));
		auto get = bp.AddVariableGetNode("Items", ImVec2(0, 150));
		auto each = bp.AddFlowControlNode("For Each", ImVec2(300, 0));
		CHECK(each != 0, "foreach: flow node exists");
		auto lua = bp.AddCustomLuaNode(ImVec2(600, 0));
		bp.AddCustomLuaPin(lua, false, "Item");
		bp.SetCustomLuaSource(lua, "print(tostring({Item}))");

		bp.AddLink(bp.FindPinID(tick, "", true), bp.FindPinID(each, "", false));
		bp.AddLink(bp.FindPinID(get, "", true), bp.FindPinID(each, "Array", false));
		bp.AddLink(bp.FindPinID(each, "Loop Body", true), bp.FindPinID(lua, "", false));
		bp.AddLink(bp.FindPinID(each, "Element", true), bp.FindPinID(lua, "Item", false));

		std::string script = BlueprintLua::GenerateScript(bp);
		CHECK(script.find("in ipairs((Items) or {})") != std::string::npos, "foreach: the table variable actually wires into the loop");
		CHECK(script.find("local function bpLua") != std::string::npos,
		      "customlua: node compiled as a local function");
		CHECK(script.find("print(tostring(Item))") != std::string::npos,
		      "customlua: body references its parameter, not an inline expression");
		CHECK(script.find("bpLua" + std::to_string(lua) + "(elem") != std::string::npos,
		      "customlua: call site passes the loop element");
	}

	// ── SDK side index: ExposeClass copies one class into the live registry ─
	{
		BlueprintEditor::TypeRegistry sideIndex;
		auto& cls = sideIndex.AddClass("BP_Door_C", "AActor", "a dumped game class");
		cls.AddFunction("OpenDoor", "Game").Metadata("{target}[\"OpenDoor\"]({target})");
		cls.AddProperty("bIsOpen", BlueprintEditor::PinType(BlueprintEditor::PinKind::Boolean), "Game");

		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		size_t before = bp.GetRegistry().GetClasses().size();
		CHECK(BlueprintRegistryJson::ExposeClass(bp.GetRegistry(), sideIndex, "BP_Door_C"),
		      "sdk expose: class copied into the live registry");
		CHECK(bp.GetRegistry().GetClasses().size() == before + 1 &&
		          bp.GetRegistry().FindFunction("BP_Door_C", "OpenDoor") != nullptr,
		      "sdk expose: functions arrive with the class");
		CHECK(!BlueprintRegistryJson::ExposeClass(bp.GetRegistry(), sideIndex, "BP_Door_C"),
		      "sdk expose: double-expose rejected");
		CHECK(!BlueprintRegistryJson::ExposeClass(bp.GetRegistry(), sideIndex, "Missing"),
		      "sdk expose: unknown class rejected");
		CHECK(bp.AddCallFunctionNode("BP_Door_C", "OpenDoor", ImVec2(0, 0)) != 0,
		      "sdk expose: exposed function spawns a node");
	}

	// ── Class/Object pin interop + api_fast nodes (UEVR API audit) ──────────
	{
		using PK = BlueprintEditor::PinKind;
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);

		// The reported bug: Get Class -> Get Objects Matching. Get Class now returns a
		// UClass; its output must connect to the node's class/Target pin, NOT only the
		// bool "Allow Default".
		auto pawn = bp.AddCallFunctionNode("UEVR_API", "Get Local Pawn", ImVec2(0, 0));
		auto getClass = bp.AddCallFunctionNode("UObject", "Get Class", ImVec2(250, 0));
		bp.AddLink(bp.FindPinID(pawn, "Return Value", true), bp.FindPinID(getClass, "Target", false));

		auto matching = bp.AddCallFunctionNode("UClass", "Get Objects Matching", ImVec2(500, 0));
		BlueprintEditor::ID classOut = bp.FindPinID(getClass, "Return Value", true);
		BlueprintEditor::ID targetPin = bp.FindPinID(matching, "Target", false);
		BlueprintEditor::ID allowPin = bp.FindPinID(matching, "Allow Default", false);
		CHECK(targetPin != 0 && allowPin != 0, "classinterop: node pins found");

		CHECK(bp.CanConnectPins(classOut, targetPin),
		      "classinterop: a UClass output connects to the class/Target pin");
		// strong-match auto-connect must pick Target (a real class match), not the bool
		CHECK(bp.AutoConnectForTest(classOut, matching) == targetPin,
		      "classinterop: auto-connect lands on Target, not the bool Allow Default");

		// truthiness still lets a class (or anything) reach a real bool when intended
		auto branch = bp.AddFlowControlNode("Branch", ImVec2(500, 300));
		CHECK(bp.CanConnectPins(classOut, bp.FindPinID(branch, "Condition", false)),
		      "classinterop: class->bool still allowed for explicit nil-checks");

		// api_fast family present and generating uevr.api_fast.* calls
		auto tick = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(-400, -300));
		auto findCls = bp.AddCallFunctionNode("UEVR_API", "Find Class (Fast)", ImVec2(-400, -150));
		CHECK(findCls != 0, "fast: Find Class (Fast) exists");
		auto objs = bp.AddCallFunctionNode("UEVR_API", "Get Objects By Class (Fast)", ImVec2(-150, -300));
		CHECK(objs != 0, "fast: Get Objects By Class (Fast) exists");
		bp.SetPinDefaultValue(bp.FindPinID(objs, "Class Name", false), "Actor");
		auto each = bp.AddFlowControlNode("For Each", ImVec2(150, -300));
		bp.AddLink(bp.FindPinID(tick, "", true), bp.FindPinID(each, "", false));
		bp.AddLink(bp.FindPinID(objs, "Return Value", true), bp.FindPinID(each, "Array", false));
		std::string script = BlueprintLua::GenerateScript(bp);
		CHECK(script.find("uevr.api_fast.get_objects_by_class(\"Actor\", false)") != std::string::npos,
		      "fast: generates a uevr.api_fast call with the short name");

		auto getProp = bp.AddCallFunctionNode("UEVR_API", "Get Property (Fast)", ImVec2(150, -150));
		CHECK(getProp != 0 && bp.FindPinID(getProp, "Object", false) != 0 &&
		          bp.FindPinID(getProp, "Name", false) != 0,
		      "fast: generic Get Property (Fast) takes object + name, any-typed return");
	}

	// ── SDK contextual exposure: Cast node + aux registry (no palette flood) ─
	{
		using PK = BlueprintEditor::PinKind;

		// a "dumped" game class living ONLY in the side index
		BlueprintEditor::TypeRegistry sdk;
		auto &door = sdk.AddClass("BP_Door_C", "AActor", "a dumped game class");
		door.AddFunction("OpenDoor", "Game").Metadata("{target}:call(\"OpenDoor\")");
		door.AddProperty("bIsOpen", BlueprintEditor::PinType(PK::Boolean), "Game");

		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		bp.SetAuxRegistry(&sdk);

		// the dumped class must NOT be in the live registry/palette yet
		CHECK(bp.GetRegistry().FindClass("BP_Door_C") == nullptr,
		      "sdk contextual: imported class is not dumped into the global palette");

		// Cast establishes the class: output pin is typed BP_Door_C, class lazily copied
		auto pawn = bp.AddCallFunctionNode("UEVR_API", "Get Local Pawn", ImVec2(0, 0));
		auto cast = bp.AddCastNode("BP_Door_C", ImVec2(250, 0));
		CHECK(cast != 0, "sdk contextual: Cast node created");
		CHECK(bp.GetRegistry().FindClass("BP_Door_C") != nullptr,
		      "sdk contextual: Cast lazily copies the class into the live registry");
		BlueprintEditor::ID castOut = bp.FindPinID(cast, "As BP_Door_C", true);
		CHECK(castOut != 0 && bp.GetPin(castOut)->type.subtype == "BP_Door_C",
		      "sdk contextual: Cast output is typed as the target class");

		// now a member call on the cast output generates + guards correctly
		bp.AddLink(bp.FindPinID(pawn, "Return Value", true), bp.FindPinID(cast, "Object", false));
		auto open = bp.AddCallFunctionNode("BP_Door_C", "OpenDoor", ImVec2(500, 0));
		CHECK(open != 0, "sdk contextual: a node for the exposed class's function spawns");
		auto tick = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(-300, 0));
		bp.AddLink(bp.FindPinID(tick, "", true), bp.FindPinID(open, "", false));
		bp.AddLink(castOut, bp.FindPinID(open, "Target", false));
		std::string script = BlueprintLua::GenerateScript(bp);
		CHECK(script.find(":is_a(\"BP_Door_C\")") != std::string::npos,
		      "sdk contextual: Cast emits an is_a-guarded downcast");

		// classLike interop: the typed cast output still connects where a UObject is wanted
		CHECK(bp.CanConnectPins(castOut, bp.FindPinID(open, "Target", false)),
		      "sdk contextual: typed object connects to the member's Target");
	}

	// ── UE prefix folding (AActor<->Actor) + no palette flood (the reported bug) ─
	{
		// A dumped class uses the UNPREFIXED reflection name ("Actor"); built-in
		// api_fast returns the C++-prefixed "AActor". They must interoperate, and the
		// dumped class must NOT appear in the flat palette.
		BlueprintEditor::TypeRegistry idx;
		auto &act = idx.AddClass("Actor", "Object", "dumped actor");
		act.AddFunction("SetActorTickEnabled", "").In("bEnabled", BlueprintEditor::PinType(BlueprintEditor::PinKind::Boolean))
			.Metadata("{target}:call(\"SetActorTickEnabled\", {0})");

		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		bp.SetAuxRegistry(&idx);

		CHECK(bp.GetRegistry().IsChildOf("AActor", "Actor"), "naming: AActor is-a Actor (prefix folded)");
		CHECK(bp.GetRegistry().IsChildOf("Actor", "AActor"), "naming: Actor is-a AActor (symmetric fold)");

		auto spawn = bp.AddCallFunctionNode("UEVR_API", "Spawn Actor (Fast)", ImVec2(0, 0)); // returns Object "AActor"
		CHECK(spawn != 0, "naming: Spawn Actor (Fast) exists");
		bp.ensureClassAvailable("Actor"); // as a contextual pick / Cast would
		auto tick = bp.AddCallFunctionNode("Actor", "SetActorTickEnabled", ImVec2(300, 0));
		CHECK(tick != 0, "naming: a node for the dumped Actor method spawns");

		BlueprintEditor::ID ret = bp.FindPinID(spawn, "Return Value", true);
		BlueprintEditor::ID tgt = bp.FindPinID(tick, "Target", false);
		BlueprintEditor::ID bEnabled = bp.FindPinID(tick, "bEnabled", false);
		CHECK(ret != 0 && tgt != 0 && bEnabled != 0, "naming: pins found");
		CHECK(bp.CanConnectPins(ret, tgt),
		      "naming: AActor Return Value connects to the Actor method's Target (was: only the bool)");
		CHECK(bp.AutoConnectForTest(ret, tick) == tgt,
		      "naming: auto-connect lands on Target, not bEnabled");

		const auto *actorCls = bp.GetRegistry().FindClass("Actor");
		CHECK(actorCls != nullptr && actorCls->paletteHidden,
		      "flood: a contextually-added SDK class is hidden from the flat palette");
	}

	// ── Is Valid flow node + Lua-truthiness Boolean inputs ──────────────────
	{
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);

		auto tick = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(0, 0));
		auto pawn = bp.AddCallFunctionNode("UEVR_API", "Get Local Pawn", ImVec2(0, 150));
		auto valid = bp.AddFlowControlNode("Is Valid", ImVec2(300, 0));
		auto print = bp.AddCallFunctionNode("UEVR_API", "Print", ImVec2(600, 0));
		CHECK(valid != 0, "isvalid: flow node exists");

		bp.AddLink(bp.FindPinID(tick, "", true), bp.FindPinID(valid, "", false));
		CHECK(bp.AddLink(bp.FindPinID(pawn, "Return Value", true), bp.FindPinID(valid, "Object", false)),
		      "isvalid: object wires into the check");
		bp.AddLink(bp.FindPinID(valid, "Is Valid", true), bp.FindPinID(print, "", false));

		std::string script = BlueprintLua::GenerateScript(bp);
		CHECK(script.find("~= nil then") != std::string::npos, "isvalid: emits a nil check");

		// Lua truthiness: an OBJECT output connects straight into a Branch Condition
		auto branch = bp.AddFlowControlNode("Branch", ImVec2(300, 300));
		CHECK(bp.AddLink(bp.FindPinID(pawn, "Return Value", true), bp.FindPinID(branch, "Condition", false)),
		      "truthiness: object output accepted by a Boolean condition pin");
		auto len = bp.AddCallFunctionNode("LuaTable", "Array Length", ImVec2(0, 450));
		CHECK(bp.AddLink(bp.FindPinID(len, "Length", true), bp.FindPinID(branch, "Condition", false)),
		      "truthiness: integer accepted by a Boolean condition pin (replaces link)");
		// but exec pins still never cross into data
		CHECK(!bp.AddLink(bp.FindPinID(tick, "", true), bp.FindPinID(branch, "Condition", false)),
		      "truthiness: exec into Boolean still rejected");
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

		// rename updates the variable AND the Get node that references it
		auto getN2 = bp.AddVariableGetNode("Health", ImVec2(0, 400));
		CHECK(getN2 != 0, "variables: Get node for rename test");
		CHECK(bp.RenameVariable("Health", "HP"), "variables: rename");
		CHECK(bp.GetVariables()[0].name == "HP", "variables: name updated in list");
		CHECK(bp.GetNode(getN2) && bp.GetNode(getN2)->memberName == "HP", "variables: rename propagates to Get node");
		CHECK(!bp.RenameVariable("HP", ""), "variables: empty rename rejected");
		CHECK(!bp.RenameVariable("HP", "HP"), "variables: no-op rename rejected");
		CHECK(bp.RemoveVariable("HP") && bp.GetVariables().empty(), "variables: remove");
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

	// ── Expanded registry: datatypes + table/array nodes ───────────────────
	{
		BlueprintEditor bp;
		BlueprintLua::SetupUEVRRegistry(bp);
		auto& reg = bp.GetRegistry();
		CHECK(reg.FindClass("LuaTable") && reg.FindFunction("LuaTable", "Array Get") &&
		          reg.FindFunction("LuaTable", "Array Length") && reg.FindFunction("LuaTable", "Map Set"),
		      "registry: LuaTable array/map nodes present");
		CHECK(reg.FindClass("Vector3f") && reg.FindFunction("Vector3f", "Dot (Vector3f)"), "registry: Vector3f nodes present");
		CHECK(reg.FindClass("Quaternionf") && reg.FindFunction("Quaternionf", "Slerp (Quaternion)"), "registry: Quaternionf nodes present");
		CHECK(reg.FindClass("Transformf") && reg.FindClass("Matrix4x4f") && reg.FindClass("StructObject"),
		      "registry: Transform/Matrix/StructObject classes present");
		CHECK(reg.FindClass("UObjectHook") && reg.FindFunction("UObjectHook", "Get First Object By Class"),
		      "registry: UObjectHook present");
		CHECK(reg.FindFunction("IConsoleVariable", "Get Int") && reg.FindFunction("UEVR_API", "Get Console Manager"),
		      "registry: console API present");
		CHECK(reg.FindFunction("UClass", "Get Class Default Object") && reg.FindFunction("UObject", "Get Address"),
		      "registry: reflection additions on existing classes (not shadowed by duplicates)");

		// codegen spot-check: Array Length (Integer) -> To String -> Print emits #(...)
		auto ev = bp.AddEventNode("UEVR", "Pre Engine Tick", ImVec2(0, 0));
		auto len = bp.AddCallFunctionNode("LuaTable", "Array Length", ImVec2(150, 0));
		auto ts = bp.AddCallFunctionNode("LuaString", "To String", ImVec2(300, 0));
		auto print = bp.AddCallFunctionNode("UEVR_API", "Print", ImVec2(450, 0));
		bp.AddLink(bp.FindPinID(ev, "", true), bp.FindPinID(print, "", false));
		bp.AddLink(bp.FindPinID(len, "Length", true), bp.FindPinID(ts, "Value", false));
		bp.AddLink(bp.FindPinID(ts, "Return Value", true), bp.FindPinID(print, "Message", false));
		CHECK(BlueprintLua::GenerateScript(bp).find("#(") != std::string::npos, "table: Array Length emits #(...) in codegen");
	}
#endif // IMGUIIDE_PLUGIN_UEVR

	if (gFailures == 0) {
		std::printf("selftest: all %d checks passed\n", gChecks);
		return 0;
	}
	std::fprintf(stderr, "selftest: %d/%d checks FAILED\n", gFailures, gChecks);
	return 1;
}
