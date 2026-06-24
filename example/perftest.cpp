//	TextEditor perftest — headless performance regression net.
//
//	Times the dominant non-UI hot paths (load+colorize, fold detection, color
//	lookups, serialization, cursor/coordinate normalization) on a large synthetic
//	document, with NO ImGui/SDL context — the same headless setup as selftest, so
//	it runs in CI and even while example.exe is open.
//
//	It PRINTS every timing (the real signal) and asserts only *generous* budgets so
//	a gross (≈order-of-magnitude) regression fails the build while ordinary
//	machine-to-machine variance does not. Tighten budgets once a baseline is known.
//
//	Exit code 0 = within budget, 1 = a budget blown (printed to stderr).

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "TextEditor.h"

static int gFailures = 0;

#define BUDGET(label, ms, limitMs) do { \
		std::printf("  %-42s %8.1f ms   (budget %.0f)\n", label, (ms), (double)(limitMs)); \
		if ((ms) > (limitMs)) { ++gFailures; std::fprintf(stderr, "PERF FAIL: %s took %.1f ms (> %.0f)\n", label, (ms), (double)(limitMs)); } \
	} while (0)

template <class F>
static double timeMs(F &&fn)
{
	auto a = std::chrono::steady_clock::now();
	fn();
	auto b = std::chrono::steady_clock::now();
	return std::chrono::duration<double, std::milli>(b - a).count();
}

// A large, syntactically varied C++ document: functions (brace folds), block /
// line comments, string + char literals, numbers, and a range of identifiers, so
// the colorizer and fold detector both do representative work.
static std::string makeBigCpp(int functions)
{
	std::string s;
	s.reserve((size_t)functions * 220);
	s += "// generated perf fixture\n#include <vector>\n#include <string>\n\nnamespace perf {\n\n";
	for (int i = 0; i < functions; ++i)
	{
		char buf[256];
		std::snprintf(buf, sizeof(buf),
			"/* block comment for fn %d spanning\n   two lines */\n"
			"int compute_%d(int a, int b) {\n"
			"    const char* name = \"item_%d\"; // trailing line comment\n"
			"    double scale = 3.14159 * %d + 0x%x;\n"
			"    for (int k = 0; k < a; ++k) {\n"
			"        b += (k * %d) - '\\n';\n"
			"    }\n"
			"    return b + (int)scale;\n"
			"}\n\n",
			i, i, i, i, (unsigned)(i * 7 + 1), i);
		s += buf;
	}
	s += "} // namespace perf\n";
	return s;
}

int main()
{
	const int kFunctions = 1200; // ~13k lines
	std::string big = makeBigCpp(kFunctions);
	int approxLines = 1;
	for (char c : big)
		if (c == '\n')
			++approxLines;

	std::printf("perftest: C++ fixture %d functions / ~%d lines / %zu KB\n",
		kFunctions, approxLines, big.size() / 1024);

	// 1) Load + colorize + fold (the open-a-big-file cost).
	TextEditor ed;
	ed.SetLanguage(TextEditor::Language::Cpp());
	ed.SetFoldingEnabled(true);
	double tLoad = timeMs([&] { ed.SetText(big); });
	BUDGET("SetText (load + colorize + fold)", tLoad, 2000.0);
	std::printf("    -> %d lines, %zu folds\n", ed.GetLineCount(), ed.GetFoldCount());

	// 2) Re-load identical text (warm colorize path).
	double tReload = timeMs([&] { ed.SetText(big); });
	BUDGET("SetText again (warm)", tReload, 2000.0);

	// 3) Per-glyph color lookups across the whole document (render-time cost).
	int lineCount = ed.GetLineCount();
	double tColor = timeMs([&] {
		long long sink = 0;
		for (int ln = 0; ln < lineCount; ++ln)
		{
			int len = (int)ed.GetLineText(ln).size();
			for (int col = 0; col < len; ++col)
				sink += ed.GetGlyphColorAt(ln, col);
		}
		// keep the compiler from eliding the sweep
		if (sink == -1)
			std::fprintf(stderr, "unreachable %lld\n", sink);
	});
	BUDGET("GetGlyphColorAt sweep (whole doc)", tColor, 1500.0);

	// 4) Serialize the whole document back out (GetText / save cost).
	double tGet = timeMs([&] {
		std::string out = ed.GetText();
		if (out.empty())
			std::fprintf(stderr, "unreachable empty\n");
	});
	BUDGET("GetText (serialize)", tGet, 500.0);

	// 5) Cursor placement across the doc — exercises normalizeCoordinate / tab math.
	double tCursor = timeMs([&] {
		for (int ln = 0; ln < lineCount; ++ln)
			ed.SetCursor(ln, ln % 40);
	});
	BUDGET("SetCursor sweep (normalizeCoordinate)", tCursor, 1000.0);

	// 6) MarkChangedLines — the diff run on every external/Claude reload. `ed` holds
	//    `big`; we diff a *previous* version against it.
	//    a) Localized edit (one char): the equal prefix/suffix trim should make this
	//       near-instant regardless of document size.
	std::string editedLocal = big;
	editedLocal[big.size() / 2] = (editedLocal[big.size() / 2] == 'x') ? 'y' : 'x';
	double tDiffLocal = timeMs([&] {
		auto r = ed.MarkChangedLines(editedLocal);
		if (r.size() > 1000000)
			std::fprintf(stderr, "unreachable %zu\n", r.size());
	});
	BUDGET("MarkChangedLines (localized 1-line change)", tDiffLocal, 300.0);

	//    b) A large differing block (rewrite the first ~quarter): trim can't help, so
	//       the bounded LCS runs (or the >3000-line wholesale-mark guard trips). Either
	//       way this is the worst realistic case.
	std::string editedBlock = big;
	for (size_t i = 0, q = big.size() / 4; i < q; ++i)
		if (editedBlock[i] == 'e')
			editedBlock[i] = '3';
	double tDiffBlock = timeMs([&] {
		auto r = ed.MarkChangedLines(editedBlock);
		if (r.size() > 1000000)
			std::fprintf(stderr, "unreachable %zu\n", r.size());
	});
	BUDGET("MarkChangedLines (large differing block)", tDiffBlock, 2000.0);
	// restore the markers to a clean state (the test mutated them)
	ed.ClearMarkers();

	if (gFailures == 0)
		std::printf("perftest: all within budget\n");
	else
		std::fprintf(stderr, "perftest: %d budget(s) blown\n", gFailures);
	return gFailures ? 1 : 0;
}
