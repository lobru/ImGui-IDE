//	TextEditor - A syntax highlighting text editor for Dear ImGui.
//	Copyright (c) 2024-2026 Johan A. Goossens. All rights reserved.
//	Copyright (c) 2026 Logan Brunet (ImGui-IDE). All rights reserved.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


//
//	Include files
//

#include <cmath>
#include <fstream>
#include <limits>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "imgui.h"

// TE_NO_IMGUI_INTERNAL: build against the PUBLIC Dear ImGui API only, for
// hosts that expose ImGui through a function table covering just the public
// surface (e.g. ReShade addons). Costs, all graceful: no IME positioning,
// no scrollbar minimap, no autocomplete-popup z-order nudge. Each guarded
// site below names its fallback.
#ifndef TE_NO_IMGUI_INTERNAL
#include "imgui_internal.h"
#endif

#include "TextEditor.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

static 	std::vector<int> visibleLineMap;

//
//	TextEditor::TextEditor
//
static inline std::string trim_left(const std::string& s, size_t& index)
{
	size_t i = 0;
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
	index = i;
	return s.substr(i);
}

static inline bool starts_with(const std::string& s, const char* p)
{
	const size_t n = std::strlen(p);
	return s.size() >= n && std::memcmp(s.data(), p, n) == 0;
}

static inline bool starts_with_trimmed(const std::string& s, const char* p, size_t& index)
{
	return starts_with(trim_left(s, index), p);
}

static inline bool is_blank(const std::string& s)
{
	for (char c : s)
	{
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			return false;
	}
	return true;
}

static inline int indent_level(const std::string& s)
{
	int level = 0;
	for (char c : s)
	{
		if (c == ' ')
			level++;
		else if (c == '\t')
			level += 4;
		else
			break;
	}
	return level;
}

static inline bool starts_with_any_trimmed(const std::string& s, std::initializer_list<const char*> prefixes, size_t& index)
{
	std::string t = trim_left(s, index);
	for (auto p : prefixes)
	{
		const size_t n = std::strlen(p);
		if (t.size() >= n && std::memcmp(t.data(), p, n) == 0)
			return true;
	}
	return false;
}



TextEditor::TextEditor()
{
	SetPalette(defaultPalette);

}


//
//	TextEditor::setText
//

void TextEditor::setText(const std::string_view& text)
{
	// load text into document and reset subsystems
	document.setText(text);
	transactions.reset();
	// transactions.reset() zeroes the undo index, so the indent-guide cache key
	// (undoIndex, lineCount, tabSize) can COLLIDE with the previous content when a
	// reload has the same line count — invalidate explicitly.
	indentGuideCacheLines = -1;
	bracketeer.reset();
	colorizer.updateEntireDocument(document, language);
	if (language)
	{
		document.setLanguage(language);
	}
	cursors.clearAll();
	clearMarkers();
	makeCursorVisible();
	foldRanges.rebuildFoldRanges(document);
	showMatchingBracketsChanged = false;
	languageChanged = false;

}


//
//	TextEditor::render
//

void TextEditor::render(const char* title, const ImVec2& size, bool border)
{
	// get current transaction version
	auto transActionVersion = transactions.getVersion();

	// update color palette (if required)
	if (paletteAlpha != ImGui::GetStyle().Alpha)
	{
		updatePalette();
	}

	// get font information and determine horizontal offsets for line numbers, decorations and text
	font = ImGui::GetFont();
	fontSize = ImGui::GetFontSize();
	glyphSize = ImVec2(ImGui::CalcTextSize("#").x, ImGui::GetTextLineHeightWithSpacing() * lineSpacing);
	// Detect a proportional (non-fixed-pitch) font: if a narrow and a wide glyph
	// advance differently, switch the column↔pixel mapping to measured advances.
	// For a monospace font this stays false and all layout uses the fast
	// column*cell arithmetic exactly as before.
	proportional = std::fabs(ImGui::CalcTextSize("i").x - ImGui::CalcTextSize("W").x) > 0.5f;
	lineNumberLeftOffset = leftMargin * glyphSize.x;


	if (showLineNumbers)
	{
		int digits = static_cast<int>(std::log10(document.lineCount() + 1) + 1.0f);
		lineNumberRightOffset = lineNumberLeftOffset + digits * glyphSize.x;
		decorationOffset = lineNumberRightOffset + decorationMargin * glyphSize.x;

	}
	else
	{
		lineNumberRightOffset = lineNumberLeftOffset;
		decorationOffset = lineNumberLeftOffset;
	}

	if (decoratorWidth > 0.0f)
	{
		textOffset = decorationOffset + decoratorWidth + decorationMargin * glyphSize.x;

	}
	else if (decoratorWidth < 0.0f)
	{
		textOffset = decorationOffset + (-decoratorWidth + decorationMargin) * glyphSize.x;

	}
	else
	{
		textOffset = decorationOffset + textMargin * glyphSize.x;
	}

	// get current position and total/visible editor size
	auto pos = ImGui::GetCursorScreenPos();
	// scrollable content height = number of visible lines (folded lines collapse)
	const int totalLines = std::max(getVisualLineCount(), 1);
	// Right-edge gutter so glyphs never kiss the window border. Applied to:
	//   - totalSize.x : lets horizontal scroll reveal the trailing space.
	//   - visibleWidth: makes the last-visible-column calc stop short of the edge.
	//   - text clip rect: visually clips anything that overshoots.
	const float rightTextPadding = glyphSize.x * 1.5f;
	auto totalSize = ImVec2(
		textOffset + document.getMaxColumn() * glyphSize.x + cursorWidth + rightTextPadding,
		totalLines * glyphSize.y
	);

	// estimate visible size before BeginChild for scroll adjustment calculations below
	// (uses last frame's scrollbar state which is close enough for one-frame adjustments)
	auto region = ImGui::GetContentRegionAvail();
	// public-API replica of ImGui::CalcItemSize(size, region.x, region.y)
	// (imgui_internal): 0 = use default, negative = available minus |value|
	auto visibleSize = size;
	if (visibleSize.x == 0.0f) visibleSize.x = region.x;
	else if (visibleSize.x < 0.0f) visibleSize.x = std::max(4.0f, region.x + visibleSize.x);
	if (visibleSize.y == 0.0f) visibleSize.y = region.y;
	else if (visibleSize.y < 0.0f) visibleSize.y = std::max(4.0f, region.y + visibleSize.y);
	float scrollbarSize = ImGui::GetStyle().ScrollbarSize;
	verticalScrollBarSize = (totalSize.y > visibleSize.y) ? scrollbarSize : 0.0f;
	horizontalScrollBarSize = (totalSize.x > visibleSize.x) ? scrollbarSize : 0.0f;
	visibleWidth = visibleSize.x - textOffset - verticalScrollBarSize - rightTextPadding;
	visibleHeight = visibleSize.y - horizontalScrollBarSize;
	visibleLines = std::max((int)ceil(visibleHeight / glyphSize.y), 0);

	// Word wrap overrides the measurement: rows replace lines as the vertical
	// unit, and there is no horizontal scroll (content width == view width).
	if (wordWrap)
	{
		float avail = (wrapWidthPx > 0.0f)
			? wrapWidthPx
			: (visibleSize.x - textOffset - scrollbarSize - rightTextPadding);
		wrapColumns = std::max(1, static_cast<int>(avail / glyphSize.x));
		buildWrapRows();
		totalSize.x = visibleSize.x;
		totalSize.y = std::max(static_cast<int>(wrapRows.size()), 1) * glyphSize.y;
		verticalScrollBarSize = (totalSize.y > visibleSize.y) ? scrollbarSize : 0.0f;
		horizontalScrollBarSize = 0.0f;
		visibleWidth = visibleSize.x - textOffset - verticalScrollBarSize - rightTextPadding;
		visibleHeight = visibleSize.y;
		visibleLines = std::max((int)ceil(visibleHeight / glyphSize.y), 0);
	}

	// determine scrolling requirements (uses last frame's firstVisibleLine/Column stored in member vars)
	float scrollX = -1.0f;
	float scrollY = -1.0f;

	// ensure cursor is visible (if requested) — use visual indices for folding-aware scroll.
	// Only scroll when the cursor is strictly outside the viewport; otherwise leaving
	// the view alone matches what the user just clicked or arrowed onto.
	if (ensureCursorIsVisible)
	{
		auto cursor = cursors.getCurrent().getInteractiveEnd();
		// In wrap mode the vertical unit is the wrapped row; no horizontal scroll.
		int cursorVI = wordWrap ? wrapRowOfCoordinate(cursor) : lineToVisualIndex(cursor.line);
		int firstVI  = static_cast<int>(std::floor(ImGui::GetScrollY() / glyphSize.y));
		int lastVI   = firstVI + std::max(visibleLines - 1, 0);
		if (!wordWrap)
		{
			firstVI = lineToVisualIndex(firstVisibleLine);
			lastVI  = lineToVisualIndex(lastVisibleLine);
		}

		if (cursorVI < firstVI)
		{
			scrollY = std::max(0.0f, static_cast<float>(cursorVI) * glyphSize.y);
		}
		else if (cursorVI > lastVI)
		{
			scrollY = std::max(0.0f, (cursorVI + 1.0f) * glyphSize.y - visibleHeight);
		}

		if (!wordWrap)
		{
			if (cursor.column < firstVisibleColumn)
			{
				scrollX = std::max(0.0f, static_cast<float>(cursor.column) * glyphSize.x);
			}
			else if (cursor.column > lastVisibleColumn)
			{
				scrollX = std::max(0.0f, (cursor.column + 1.0f) * glyphSize.x - visibleWidth);
			}
		}

		ensureCursorIsVisible = false;
	}

	// scroll to specified line (if required)
	if (scrollToLineNumber >= 0)
	{
		scrollToLineNumber = std::min(scrollToLineNumber, document.lineCount());
		scrollX = 0.0f;

		// In wrap mode translate the target line to its first wrapped row.
		int unit = scrollToLineNumber;
		if (wordWrap)
			unit = wrapRowOfCoordinate(Coordinate(scrollToLineNumber, 0));

		switch (scrollToAlignment)
		{
		case Scroll::alignTop:
			scrollY = std::max(0.0f, static_cast<float>(unit) * glyphSize.y);
			break;

		case Scroll::alignMiddle:
			scrollY = std::max(0.0f, static_cast<float>(unit - visibleLines / 2) * glyphSize.y);
			break;

		case Scroll::alignBottom:
			scrollY = std::max(0.0f, static_cast<float>(unit - (visibleLines - 1)) * glyphSize.y);
			break;
		}

		scrollToLineNumber = -1;
	}

	// set scroll (if required)
	if (scrollX >= 0.0f || scrollY >= 0.0f)
	{
		ImGui::SetNextWindowScroll(ImVec2(scrollX, scrollY));
	}

	// ensure editor has focus (if required)
	if (focusOnEditor)
	{
		ImGui::SetNextWindowFocus();
		focusOnEditor = false;
	}

	// start a new child window
	// this must be done before we handle keyboard and mouse interactions to ensure correct Dear ImGui context
	ImGui::SetNextWindowContentSize(totalSize);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(palette.get(Color::background)));
	ImGui::BeginChild(title, size, (border ? ImGuiChildFlags_Borders : 0), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs);
	lastRenderOrigin = ImGui::GetCursorScreenPos();

	// compute visible range from child window's own scroll (correct source)
	bool documentChanged = document.isUpdated();
	{
		firstVisibleColumn = std::max(static_cast<int>(std::floor(ImGui::GetScrollX() / glyphSize.x)), 0);
		lastVisibleColumn = static_cast<int>(std::floor((ImGui::GetScrollX() + visibleWidth) / glyphSize.x));

		int firstVI = std::max((int)std::floor(ImGui::GetScrollY() / glyphSize.y), 0);
		int lastVI = (int)std::floor((ImGui::GetScrollY() + visibleHeight) / glyphSize.y);
		int maxVI = std::max(getVisualLineCount() - 1, 0);
		firstVI = std::clamp(firstVI, 0, maxVI);
		lastVI = std::clamp(lastVI, 0, maxVI);
		firstVisibleLine = visualIndexToLine(firstVI);
		lastVisibleLine = visualIndexToLine(lastVI);

		visibleColumns = std::max(static_cast<int>(std::ceil(visibleWidth / glyphSize.x)), 0);
	}

	// handle keyboard and mouse inputs
	handleKeyboardInputs();
	handleMouseInteractions();

	// ensure cursors are up to date (sort and merge if required)
	if (cursors.anyHasUpdate())
	{
		cursors.update();
	}

	// recolorize entire document and reset brackets (if required)
	if (showMatchingBracketsChanged || languageChanged)
	{
		colorizer.updateEntireDocument(document, language);
		if (language)
		{
			document.setLanguage(language);
		}
		bracketeer.reset();
	}

	// An edit recolors the touched lines right away (cheap, keeps coloring live),
	// but the two O(document) passes below — fold-range reparse and bracket rescan
	// — are DEBOUNCED. Running them on every keystroke reparses the whole file each
	// frame; in a large document that drops the frame rate hard (240->30). Run them
	// only once typing has settled (or immediately on a language / setting change).
	if (documentChanged) { structureDirty = true; structureDirtyTime = ImGui::GetTime(); }
	bool structureForce   = languageChanged || showMatchingBracketsChanged;
	bool structureSettled = structureDirty && (ImGui::GetTime() - structureDirtyTime) > 0.15;

	if (language && documentChanged)
	{
		// recolorize updated lines (incremental — only the changed lines)
		colorizer.updateChangedLines(document, language);
	}

	if (foldRanges.foldingEnabled && (structureForce || structureSettled))
	{
		foldRanges.rebuildFoldRanges(document);
	}

	if (language && showMatchingBrackets && (structureForce || structureSettled))
	{
		// rebuild bracket list
		bracketeer.update(document);
	}

	if (structureSettled) structureDirty = false;

	// reset changed states
	showMatchingBracketsChanged = false;
	languageChanged = false;

	// render editor parts. Word-wrap uses a single combined pass that draws
	// line numbers + selection + glyphs + carets from the wrapped-row model;
	// the line-based decoration passes (markers, brackets, minimap, fold
	// margin) are skipped in wrap mode since their Y math assumes 1 line = 1
	// row. (Documented v1 limitation.)
	if (wordWrap)
	{
		// v1: line-based decoration passes (fold margin, markers, brackets,
		// minimap) are skipped — their Y math assumes 1 line = 1 row.
		renderTextWrapped();
		renderPanScrollIndicator();
	}
	else
	{
		renderSelections();
		renderMarkers();
		renderMatchingBrackets();
		renderIndentGuides();    // language-agnostic indent guides (VSCode-style) when folding is on
		renderMargin();          // <-- moved up so click-to-fold takes effect same frame
		renderLineNumbers();
		renderText();
		renderCursors();
		renderDecorations();
		renderScrollbarMiniMap();
		renderPanScrollIndicator();
	}

	if (ImGui::BeginPopup("LineNumberContextMenu", ImGuiWindowFlags_NoMove))
	{
		lineNumberContextMenuCallback(contextMenuLine);
		ImGui::EndPopup();
	}

	if (ImGui::BeginPopup("TextContextMenu", ImGuiWindowFlags_NoMove))
	{
		textContextMenuCallback(contextMenuLine, contextMenuColumn);
		ImGui::EndPopup();
	}

	// render find/replace popup
	renderFindReplace(pos, visibleSize.x - verticalScrollBarSize);

	// render autocomplete popup
	if (autocomplete.render(*this, document, cursors, language, textOffset, glyphSize))
	{
		// user picked a suggestion so insert it
		auto start = autocomplete.getStart();
		auto end = document.findWordEnd(start, true);
		auto replacement = autocomplete.getReplacement();
		replaceSectionText(start, end, replacement);
	}

	// handle change tracking if there is a change callback in place
	if (delayedChangeCallback)
	{
		if (delayedChangeDetected)
		{
			if (std::chrono::system_clock::now() > delayedChangeReportTime)
			{
				delayedChangeCallback();
				delayedChangeDetected = false;
			}

		}
		else if (transactions.getVersion() != transActionVersion)
		{
			delayedChangeDetected = true;
			delayedChangeReportTime = std::chrono::system_clock::now() + delayedChangeDelay;
		}
	}

	ImGui::EndChild();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
}


//
//	TextEditor::renderSelections
//

void TextEditor::renderSelections()
{
	auto drawList = ImGui::GetWindowDrawList();
	ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();

	// Clip selection drawing to the text area so it can't bleed across the
	// line-number / fold-arrow gutter when the document is scrolled
	// horizontally. The gutter is fixed at `windowPos.x .. windowPos.x + textOffset`
	// in screen space.
	const float gutterRightX = ImGui::GetWindowPos().x + textOffset;
	const float windowRightX = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x;
	ImVec2 clipMin(gutterRightX, ImGui::GetWindowPos().y);
	ImVec2 clipMax(windowRightX, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y);
	drawList->PushClipRect(clipMin, clipMax, true);

	for (auto& cursor : cursors)
	{
		if (!cursor.hasSelection())
			continue;

		auto start = cursor.getSelectionStart();
		auto end = cursor.getSelectionEnd();

		if (end.line < firstVisibleLine || start.line > lastVisibleLine)
			continue;

		int firstLine = std::max(start.line, firstVisibleLine);
		int lastLine = std::min(end.line, lastVisibleLine);

		// First pass: figure out if the mouse is over ANY line of this selection.
		// Otherwise hovered/non-hovered status diverged per-line, making the top
		// lines look dimmer than the bottom ones when the user hovered the middle.
//		bool selectionHovered = false;
	/*
		for (int line = firstLine; line <= lastLine && !selectionHovered; ++line)
		{
			if (!document[line].visible) continue;
			int vi = lineToVisualIndex(line);
			auto x = cursorScreenPos.x + textOffset;
			auto left = x + columnToX(line, (line == start.line ? start.column : 0));
			auto right = x + columnToX(line, (line == end.line ? end.column : document[line].maxColumn));
			if (left < gutterRightX) left = gutterRightX;
			auto y = cursorScreenPos.y + vi * glyphSize.y;
			if (ImGui::IsMouseHoveringRect(ImVec2(left, y), ImVec2(right, y + glyphSize.y)))
			{
				selectionHovered = true;
			}
		}
	*/
		for (int line = firstLine; line <= lastLine; ++line)
		{
			if (!document[line].visible) continue;
			int vi = lineToVisualIndex(line);
			auto x = cursorScreenPos.x + textOffset;
			auto left = x + columnToX(line, (line == start.line ? start.column : 0));
			auto right = x + columnToX(line, (line == end.line ? end.column : document[line].maxColumn));
			if (left < gutterRightX) left = gutterRightX;
			auto y = cursorScreenPos.y + vi * glyphSize.y;


			bool hovered = ImGui::IsMouseHoveringRect(ImVec2(left, y), ImVec2(right, y + glyphSize.y));
			ImU32 bgCol = IM_COL32(80, 80, 80, 180);/*selectionHovered ? IM_COL32(100, 100, 110, 200) : IM_COL32(80, 80, 80, 140);*/
			drawList->AddRectFilled(ImVec2(left, y), ImVec2(right, y + glyphSize.y), bgCol);
			// Drag-start detection moved to handleMouseInteractions so it can
			// be gated on a real ImGui drag (not just an immediate down-click);
			// this keeps a small click from being mistaken for a move/copy.
			(void)hovered;

		}
	}

	drawList->PopClipRect();
}

void TextEditor::renderCursors()
{
	// Read-only docs don't need a blinking caret. (Picked up from upstream
	// Pascal Thomet's fix.) Stops the I-beam from showing in diff / history
	// / "view source" style windows.
	if (readOnly) return;

	cursorAnimationTimer = std::fmod(cursorAnimationTimer + ImGui::GetIO().DeltaTime, 1.0f);

	if (!ImGui::IsWindowFocused())
		return;

	ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();

	if (!ImGui::GetIO().ConfigInputTextCursorBlink || cursorAnimationTimer < 0.5f)
	{
		auto drawList = ImGui::GetWindowDrawList();

		for (auto& cursor : cursors)
		{
			auto pos = cursor.getInteractiveEnd();
			if (pos.line < firstVisibleLine || pos.line > lastVisibleLine)
				continue;

			int vi = lineToVisualIndex(pos.line);
			auto x = cursorScreenPos.x + textOffset + columnToX(pos.line, pos.column) - 1;
			auto y = cursorScreenPos.y + vi * glyphSize.y;
			drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + cursorWidth, y + glyphSize.y), palette.get(Color::cursor));
		}
	}

	// IME setup unchanged...
	// (needs ImGuiContext internals; without them the OS IME composition
	// window may appear at a default position - typing is unaffected)
#ifndef TE_NO_IMGUI_INTERNAL
	if (!readOnly)
	{
		auto context = ImGui::GetCurrentContext();
		context->PlatformImeData.WantVisible = true;
		context->PlatformImeData.WantTextInput = true;
		context->PlatformImeData.InputPos = ImVec2(cursorScreenPos.x - 1.0f, cursorScreenPos.y - context->FontSize);
		context->PlatformImeData.InputLineHeight = context->FontSize;
		context->PlatformImeData.ViewportId = ImGui::GetCurrentWindow()->Viewport->ID;
	}
#endif
}

//
//	TextEditor::renderMarkers
//

void TextEditor::renderMarkers()
{
	if (markers.size())
	{
		auto drawList = ImGui::GetWindowDrawList();
		ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();

		for (int line = firstVisibleLine; line <= lastVisibleLine; line++)
		{
			if (!document[line].visible) continue;
			if (document[line].marker)
			{
				auto& marker = markers[document[line].marker - 1];
				int vi = lineToVisualIndex(line);
				auto y = cursorScreenPos.y + vi * glyphSize.y;

				if (((marker.lineNumberColor >> IM_COL32_A_SHIFT) & 0xFF) != 0)
				{
					auto left = cursorScreenPos.x + lineNumberLeftOffset;
					auto right = cursorScreenPos.x + lineNumberRightOffset;
					auto start = ImVec2(left, y);
					auto end = ImVec2(right, y + glyphSize.y);
					drawList->AddRectFilled(start, end, marker.lineNumberColor);

					if (marker.lineNumberTooltip.size() && ImGui::IsMouseHoveringRect(start, end))
					{
						ImGui::PushStyleColor(ImGuiCol_PopupBg, marker.lineNumberColor);
						ImGui::BeginTooltip();
						ImGui::TextUnformatted(marker.lineNumberTooltip.c_str());
						ImGui::EndTooltip();
						ImGui::PopStyleColor();
					}
				}

				if (((marker.textColor >> IM_COL32_A_SHIFT) & 0xFF) != 0)
				{
					auto left = cursorScreenPos.x + textOffset;
					auto right = left + lastVisibleColumn * glyphSize.x;
					auto start = ImVec2(left, y);
					auto end = ImVec2(right, y + glyphSize.y);
					drawList->AddRectFilled(start, end, marker.textColor);

					if (marker.textTooltip.size() && ImGui::IsMouseHoveringRect(start, end))
					{
						ImGui::PushStyleColor(ImGuiCol_PopupBg, marker.textColor);
						ImGui::BeginTooltip();
						ImGui::TextUnformatted(marker.textTooltip.c_str());
						ImGui::EndTooltip();
						ImGui::PopStyleColor();
					}
				}
			}
		}
	}
}


//
//	TextEditor::renderMatchingBrackets
//

void TextEditor::renderMatchingBrackets()
{
	if (showMatchingBrackets)
	{
		if (bracketeer.size())
		{
			auto drawList = ImGui::GetWindowDrawList();
			ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();

			// Passive per-bracket scope guides — only when folding is OFF. With folding
			// ON, renderIndentGuides() draws language-agnostic indent guides instead, so
			// brace-less languages (Lua, Python) get guides too and we avoid doubling.
			if (!foldRanges.foldingEnabled)
			for (auto& bracket : bracketeer)
			{
				if ((bracket.end.line - bracket.start.line) > 1 &&
					bracket.start.line <= lastVisibleLine &&
					bracket.end.line > firstVisibleLine)
				{

					int startVI = lineToVisualIndex(bracket.start.line);
					int endVI = lineToVisualIndex(bracket.end.line);
					// Skip when the body of the bracket pair is entirely folded
					// away. Otherwise we'd draw a zero- or one-pixel vertical
					// stub between the visible `{` and `}` rows, which shows
					// up as a stray `|` on what looks like an empty line.
					if (startVI + 1 >= endVI) continue;
					int guideLine = bracket.start.column <= bracket.end.column ? bracket.start.line : bracket.end.line;
					auto lineX = cursorScreenPos.x + textOffset + columnToX(guideLine, std::min(bracket.start.column, bracket.end.column));
					auto startY = cursorScreenPos.y + (startVI + 1) * glyphSize.y;
					auto endY = cursorScreenPos.y + endVI * glyphSize.y;
					drawList->AddLine(ImVec2(lineX, startY), ImVec2(lineX, endY), palette.get(Color::whitespace), 1.0f);
				}
			}

			// render active bracket pair
			auto active = bracketeer.getEnclosingBrackets(cursors.getMain().getInteractiveEnd());

			if (active != bracketeer.end() &&
				active->start.line <= lastVisibleLine &&
				active->end.line > firstVisibleLine)
			{

				int startVI = lineToVisualIndex(active->start.line);
				int endVI = lineToVisualIndex(active->end.line);
				auto x1 = cursorScreenPos.x + textOffset + columnToX(active->start.line, active->start.column);
				auto y1 = cursorScreenPos.y + startVI * glyphSize.y;
				auto w1 = columnToX(active->start.line, active->start.column + 1) - columnToX(active->start.line, active->start.column);
				drawList->AddRectFilled(ImVec2(x1, y1), ImVec2(x1 + w1, y1 + glyphSize.y), palette.get(Color::matchingBracketBackground));

				auto x2 = cursorScreenPos.x + textOffset + columnToX(active->end.line, active->end.column);
				auto y2 = cursorScreenPos.y + endVI * glyphSize.y;
				auto w2 = columnToX(active->end.line, active->end.column + 1) - columnToX(active->end.line, active->end.column);
				drawList->AddRectFilled(ImVec2(x2, y2), ImVec2(x2 + w2, y2 + glyphSize.y), palette.get(Color::matchingBracketBackground));

				if (active->end.line - active->start.line > 1 && startVI + 1 < endVI)
				{
					auto lineX = std::min(x1, x2);
					drawList->AddLine(ImVec2(lineX, y1 + glyphSize.y), ImVec2(lineX, y2), palette.get(Color::matchingBracketActive), 1.0f);
				}
			}
		}
	}
}


//
//	TextEditor::IndentGuideLevels
//
//	Per-line indent-guide count: the number of vertical indentation guides to draw
//	on each line (one per indent step to the left of the line's content). Blank
//	lines inherit the shallower of their nearest non-blank neighbours so guides run
//	continuously through them without overshooting a block. Pure logic (no ImGui
//	context) → unit-testable headless.

std::vector<int> TextEditor::IndentGuideLevels() const
{
	int ts = document.getTabSize();
	if (ts <= 0)
		ts = 4;
	int n = document.lineCount();
	std::vector<int> raw(n, -1); // own indent level for non-blank lines, -1 for blank
	for (int i = 0; i < n; ++i)
	{
		const Line& gl = document[i];
		int col = 0;
		bool blank = true;
		for (size_t k = 0; k < gl.size(); ++k)
		{
			auto cp = gl[k].codepoint;
			if (cp == ' ')
				col += 1;
			else if (cp == '\t')
				col += ts - (col % ts);
			else
			{
				blank = false;
				break;
			}
		}
		if (!blank)
			raw[i] = col / ts; // full indent steps before the content
	}

	// Nearest non-blank level to each side (two linear passes, so a run of blank
	// lines stays O(n) rather than O(n^2)).
	std::vector<int> prevLvl(n, -1), nextLvl(n, -1);
	int last = -1;
	for (int i = 0; i < n; ++i)
	{
		prevLvl[i] = last;
		if (raw[i] >= 0)
			last = raw[i];
	}
	last = -1;
	for (int i = n - 1; i >= 0; --i)
	{
		nextLvl[i] = last;
		if (raw[i] >= 0)
			last = raw[i];
	}

	std::vector<int> levels(n, 0);
	for (int i = 0; i < n; ++i)
	{
		if (raw[i] >= 0)
		{
			levels[i] = raw[i];
			continue;
		}
		int prev = prevLvl[i], next = nextLvl[i];
		if (prev < 0 && next < 0)
			levels[i] = 0;
		else if (prev < 0)
			levels[i] = next;
		else if (next < 0)
			levels[i] = prev;
		else
			levels[i] = (std::min)(prev, next);
	}
	return levels;
}


//
//	TextEditor::renderIndentGuides
//
//	Draw a faint vertical guide at each indentation step for every visible line
//	(VSCode/Sublime style), independent of language so brace-less code (Lua, Python)
//	shows block structure too. Only active when folding is enabled.

void TextEditor::renderIndentGuides()
{
	if (!foldRanges.foldingEnabled || document.lineCount() == 0)
		return;

	int ts = document.getTabSize();
	if (ts <= 0)
		ts = 4;
	// Cached full-doc scan: recompute only when the document (undo version / line
	// count) or tab size changed — per-frame O(lines) was a large-file frame killer.
	size_t ver = transactions.getUndoIndex();
	if (indentGuideCacheVersion != ver || indentGuideCacheLines != document.lineCount() ||
		indentGuideCacheTab != ts)
	{
		indentGuideCache = IndentGuideLevels();
		indentGuideCacheVersion = ver;
		indentGuideCacheLines = document.lineCount();
		indentGuideCacheTab = ts;
	}
	const auto& levels = indentGuideCache;

	auto drawList = ImGui::GetWindowDrawList();
	ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
	ImU32 color = palette.get(Color::whitespace);

	// Clip to the text area so guides never bleed under the fixed line-number gutter
	// when scrolled horizontally (mirrors renderText's clip).
	const float gutterRightX = ImGui::GetWindowPos().x + textOffset;
	const float windowRightX = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x - glyphSize.x * 1.5f - verticalScrollBarSize;
	drawList->PushClipRect(
		ImVec2(gutterRightX, ImGui::GetWindowPos().y),
		ImVec2(windowRightX, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y),
		true);

	int firstVisibleIndex = lineToVisualIndex(firstVisibleLine);
	int lastVisibleIndex = lineToVisualIndex(lastVisibleLine);
	for (int vi = firstVisibleIndex; vi <= lastVisibleIndex; ++vi)
	{
		int line = visualIndexToLine(vi);
		if (line < 0 || line >= (int) levels.size())
			continue;
		int depth = levels[line];
		float y0 = cursorScreenPos.y + vi * glyphSize.y;
		float y1 = y0 + glyphSize.y;
		for (int k = 0; k < depth; ++k)
		{
			float x = cursorScreenPos.x + textOffset + columnToX(line, k * ts);
			drawList->AddLine(ImVec2(x, y0), ImVec2(x, y1), color, 1.0f);
		}
	}

	drawList->PopClipRect();
}


//
//	TextEditor::buildWrapRows — 1:many visual-row model for word wrap.
//
//	Each VISIBLE document line (folding-aware) is split into one or more rows of
//	at most `wrapColumns` columns, breaking at the last space before the limit
//	when possible (word wrap) and hard-breaking otherwise. Rows store column
//	ranges [startColumn, endColumn); the renderer/mouse/cursor map positions by
//	column membership, so the same model drives all three consistently.
//

void TextEditor::buildWrapRows()
{
	wrapRows.clear();
	int cols = wrapColumns > 0 ? wrapColumns : 1;
	int tab = document.getTabSize();
	int lineCount = document.lineCount();

	for (int line = 0; line < lineCount; ++line)
	{
		if (!document[line].visible) continue;       // respect folding
		auto& ln = document[line];
		int sz = static_cast<int>(ln.size());
		if (sz == 0) { wrapRows.push_back({ line, 0, 0 }); continue; }

		int col = 0;
		int rowStartCol = 0;
		int lastSpaceCol = -1;                        // column boundary just after a space
		for (int idx = 0; idx < sz; ++idx)
		{
			auto cp = ln[idx].codepoint;
			int adv = (cp == '\t') ? (tab - (col % tab)) : 1;
			if (col + adv - rowStartCol > cols && col > rowStartCol)
			{
				int breakCol = (lastSpaceCol > rowStartCol) ? lastSpaceCol : col;
				wrapRows.push_back({ line, rowStartCol, breakCol });
				rowStartCol = breakCol;
				lastSpaceCol = -1;
			}
			col += adv;
			if (cp == ' ' || cp == '\t') lastSpaceCol = col;
		}
		wrapRows.push_back({ line, rowStartCol, col });
	}
	if (wrapRows.empty()) wrapRows.push_back({ 0, 0, 0 });
}

// Visual-row index containing a document coordinate (the row whose column range
// covers it; the line's last row when the column is at/after the line end).
int TextEditor::wrapRowOfCoordinate(const Coordinate& c) const
{
	int fallback = 0;
	for (int r = 0; r < static_cast<int>(wrapRows.size()); ++r)
	{
		if (wrapRows[r].line != c.line) continue;
		fallback = r;
		bool lastRowOfLine = (r + 1 >= static_cast<int>(wrapRows.size()))
			|| wrapRows[r + 1].line != c.line;
		if (c.column >= wrapRows[r].startColumn &&
			(c.column < wrapRows[r].endColumn || lastRowOfLine))
			return r;
	}
	return fallback;
}


//
//	TextEditor::renderText
//

float TextEditor::glyphAdvanceX(ImWchar codepoint, int column) const
{
	int tabSize = document.getTabSize();
	if (codepoint == '\t')
	{
		int next = ((column / tabSize) + 1) * tabSize;
		// Tab width in the proportional world is measured in space-advances so it
		// stays visually consistent with the rest of the (variable) text.
		float space = proportional ? ImGui::CalcTextSize(" ").x : glyphSize.x;
		return (next - column) * space;
	}
	if (!proportional)
		return glyphSize.x;
	char buf[5];
	size_t n = CodePoint::write(buf, codepoint);
	buf[n] = '\0';
	return ImGui::CalcTextSize(buf).x;
}

float TextEditor::columnToX(int line, int column) const
{
	if (!proportional)
		return column * glyphSize.x;
	if (line < 0 || line >= document.lineCount())
		return column * glyphSize.x;
	// Walk glyphs accumulating real advances until we reach `column`. Columns
	// past end-of-line continue at the space advance (so the caret can sit in
	// virtual space, matching the monospace behaviour).
	const Line& gl = document[line];
	float x = 0.0f;
	int col = 0;
	for (size_t i = 0; i < gl.size() && col < column; ++i)
	{
		x += glyphAdvanceX(gl[i].codepoint, col);
		col += (gl[i].codepoint == '\t') ? (document.getTabSize() - (col % document.getTabSize())) : 1;
	}
	if (col < column)
		x += (column - col) * ImGui::CalcTextSize(" ").x;   // virtual trailing space
	return x;
}

int TextEditor::xToColumn(int line, float x) const
{
	if (!proportional)
		return std::max(0, static_cast<int>(x / glyphSize.x));
	if (x <= 0.0f || line < 0 || line >= document.lineCount())
		return std::max(0, static_cast<int>(x / glyphSize.x));
	// Advance glyph by glyph; pick the column whose cell the pixel falls in
	// (rounding at the glyph's horizontal midpoint, like a normal text caret).
	const Line& gl = document[line];
	float acc = 0.0f;
	int col = 0;
	for (size_t i = 0; i < gl.size(); ++i)
	{
		float adv = glyphAdvanceX(gl[i].codepoint, col);
		if (x < acc + adv * 0.5f)
			return col;
		acc += adv;
		col += (gl[i].codepoint == '\t') ? (document.getTabSize() - (col % document.getTabSize())) : 1;
	}
	// Past end of line → virtual space columns.
	float space = ImGui::CalcTextSize(" ").x;
	if (space > 0.0f) col += static_cast<int>((x - acc) / space + 0.5f);
	return col;
}

void TextEditor::renderText()
{
	if (wordWrap) { renderTextWrapped(); return; }

	auto drawList = ImGui::GetWindowDrawList();
	ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
	auto tabSize = document.getTabSize();
	auto firstRenderableColumn = (firstVisibleColumn / tabSize) * tabSize;

	int firstVisibleIndex = lineToVisualIndex(firstVisibleLine);
	int lastVisibleIndex = lineToVisualIndex(lastVisibleLine);

	ImVec2 lineScreenPos = cursorScreenPos + ImVec2(textOffset, firstVisibleIndex * glyphSize.y);

	// Clip text drawing to the text area only. When horizontally scrolled past
	// a tab boundary, `firstRenderableColumn` is rounded down, so we'd draw
	// 1-3 columns of glyphs to the LEFT of the text area — which renders
	// behind the (fixed) line-number gutter. Clip prevents the bleed.
	// Reserve a glyph-and-a-half on the right so trailing characters don't
	// kiss the document window's edge.
	const float gutterRightXText = ImGui::GetWindowPos().x + textOffset;
	const float windowRightXText = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x
		- glyphSize.x * 1.5f - verticalScrollBarSize;
	drawList->PushClipRect(
		ImVec2(gutterRightXText, ImGui::GetWindowPos().y),
		ImVec2(windowRightXText, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y),
		true);

	for (int vi = firstVisibleIndex; vi <= lastVisibleIndex; ++vi)
	{
		int lineIndex = visualIndexToLine(vi);
		if (lineIndex < 0 || lineIndex >= document.lineCount())
			continue;

		auto& line = document[lineIndex];
		if (!line.visible)
			continue;

		auto column = firstRenderableColumn;
		auto index = document.getIndex(line, column);
		auto lineSize = line.size();

		while (index < lineSize && column <= lastVisibleColumn)
		{
			auto& glyph = line[index];
			auto codepoint = glyph.codepoint;
			// columnToX == column*glyphSize.x in monospace mode (identical), but
			// sums real advances when the font is proportional so glyphs sit flush.
			ImVec2 glyphPos{ lineScreenPos.x + columnToX(lineIndex, column), lineScreenPos.y };

			if (codepoint == '\t')
			{
				if (showTabs)
				{
					const auto x1 = glyphPos.x + glyphSize.x * 0.3f;
					const auto y = glyphPos.y + fontSize * 0.5f;
					const auto x2 = glyphPos.x + glyphSize.x;

					ImVec2 p1(x1, y);
					ImVec2 p2(x2, y);
					ImVec2 p3(x2 - fontSize * 0.16f, y - fontSize * 0.16f);
					ImVec2 p4(x2 - fontSize * 0.16f, y + fontSize * 0.16f);

					drawList->AddLine(p1, p2, palette.get(Color::whitespace));
					drawList->AddLine(p2, p3, palette.get(Color::whitespace));
					drawList->AddLine(p2, p4, palette.get(Color::whitespace));
				}
			}
			else if (codepoint == ' ')
			{
				if (showSpaces)
				{
					const auto x = glyphPos.x + glyphSize.x * 0.5f;
					const auto y = glyphPos.y + fontSize * 0.5f;
					drawList->AddCircleFilled(ImVec2(x, y), 1.5f, palette.get(Color::whitespace), 4);
				}
			}
			else
			{
				font->RenderChar(drawList, fontSize, glyphPos, palette.get(glyph.color), codepoint);
			}

			index++;
			column += (codepoint == '\t') ? tabSize - (column % tabSize) : 1;
		}

		// draw VSCode/Sublime-style fold preview on folded fold-start lines.
		// Skip the whole scan when nothing is folded (hiddenLineCount == 0) —
		// otherwise this is O(visibleLines * totalFolds) every frame.
		if (foldRanges.foldingEnabled && foldRanges.hiddenLineCount > 0)
		{
			for (auto& fr : foldRanges)
			{
				if (fr.start.line == lineIndex && fr.folded)
				{
					const char* preview = nullptr;
					switch (fr.type)
					{
					case Comment:       preview = fr.docComment ? " ///..." : " /*...*/"; break;
					case IfDef:         preview = " #if...";  break;
					case Region:        preview = " //...";   break;
					case PragmaRegion:  preview = " //...";   break;
					case Indent:        preview = " ...";     break;
					case Braces:        preview = " ...";   break;
					default:            preview = " ...";     break;
					}
					float px = cursorScreenPos.x + textOffset + (line.maxColumn + 1) * glyphSize.x;
					float py = lineScreenPos.y;
					float tw = ImGui::CalcTextSize(preview).x;
					ImVec2 rmin(px, py + 2.0f);
					ImVec2 rmax(px + tw + 6.0f, py + glyphSize.y - 2.0f);
					bool hovered = ImGui::IsMouseHoveringRect(rmin, rmax);
					ImU32 bgCol = hovered ? IM_COL32(100, 100, 110, 200) : IM_COL32(80, 80, 80, 140);
					ImU32 fgCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
					drawList->AddRectFilled(rmin, rmax, bgCol, 3.0f);
					drawList->AddText(ImVec2(px + 3.0f, py), fgCol, preview);

					if (hovered)
					{
						// single click → select the folded region; double click → also unfold.
						if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						{
							cursors.setCursor(fr.start,
											  Coordinate(fr.end.line, document[fr.end.line].maxColumn));
							fr.folded = false;
							// visibility-only — see toggleFold(): no setUpdated(true).
							foldRanges.updateVisibility(document);
						}
						else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
						{
							cursors.setCursor(fr.start,
											  Coordinate(fr.end.line, document[fr.end.line].maxColumn));
						}
					}
					break;
				}
			}
		}

		lineScreenPos.y += glyphSize.y;
	}

	drawList->PopClipRect();
}


//
//	TextEditor::renderTextWrapped — word-wrap render path.
//
//	Draws line numbers, selection, glyphs and carets straight from `wrapRows`
//	(built in render()). Each row occupies one screen line at y = row*glyphSize.y;
//	a glyph at document column C in row [start,end) is drawn at x =
//	textOffset + (C - start)*glyphSize.x. The same column→x mapping is used by
//	the mouse hit-test and cursor movement so all three agree.
//

void TextEditor::renderTextWrapped()
{
	auto drawList = ImGui::GetWindowDrawList();
	ImVec2 origin = ImGui::GetCursorScreenPos();
	int rowCount = static_cast<int>(wrapRows.size());
	if (rowCount == 0) return;

	float scrollY = ImGui::GetScrollY();
	int firstRow = std::clamp(static_cast<int>(std::floor(scrollY / glyphSize.y)), 0, rowCount - 1);
	int lastRow  = std::clamp(static_cast<int>(std::floor((scrollY + visibleHeight) / glyphSize.y)), 0, rowCount - 1);

	const float textLeft = origin.x + textOffset;
	const float winLeft  = ImGui::GetWindowPos().x;
	const float winTop   = ImGui::GetWindowPos().y;
	const float winRight = winLeft + ImGui::GetWindowSize().x - glyphSize.x * 1.5f - verticalScrollBarSize;
	const float winBot   = winTop + ImGui::GetWindowSize().y;

	// Whole-line gutter selections end at column 0 of the next line; highlight the
	// selected line, not the empty one below it (matches renderLineNumbers).
	auto& gutterCursor = cursors.getCurrent();
	auto cursorLine = gutterCursor.getInteractiveEnd().line;
	if (gutterCursor.hasSelection())
	{
		auto selS = gutterCursor.getSelectionStart();
		auto selE = gutterCursor.getSelectionEnd();
		if (selE.column == 0 && selE.line > selS.line)
			cursorLine = selE.line - 1;
	}

	// Line numbers + selection backgrounds first (under the glyphs).
	for (int r = firstRow; r <= lastRow; ++r)
	{
		const WrapRow& wr = wrapRows[r];
		float y = origin.y + r * glyphSize.y;
		bool firstRowOfLine = (r == 0) || wrapRows[r - 1].line != wr.line;

		// Line number on the line's first row only.
		if (showLineNumbers && firstRowOfLine)
		{
			auto number = std::to_string(wr.line + 1);
			auto width = static_cast<int>(number.size()) * glyphSize.x;
			auto fg = (wr.line == cursorLine) ? Color::currentLineNumber : Color::lineNumber;
			drawList->AddText(ImVec2(winLeft + lineNumberRightOffset - width, y),
				palette.get(fg), number.c_str());
		}

		// Selection shading: intersect each cursor's selection with this row's
		// column range, then with the line span.
		for (auto& cursor : cursors)
		{
			if (!cursor.hasSelection()) continue;
			auto s = cursor.getSelectionStart();
			auto e = cursor.getSelectionEnd();
			if (wr.line < s.line || wr.line > e.line) continue;
			int selStart = (wr.line == s.line) ? s.column : 0;
			int selEnd   = (wr.line == e.line) ? e.column : wr.endColumn;
			int a = std::max(selStart, wr.startColumn);
			int b = std::min(selEnd,   wr.endColumn);
			// Whole-line (interior) selection extends a bit past the last glyph.
			if (wr.line != e.line && b == wr.endColumn) b += 1;
			if (b <= a && !(wr.line != s.line && wr.line != e.line)) continue;
			float left  = textLeft + (a - wr.startColumn) * glyphSize.x;
			float right = textLeft + (b - wr.startColumn) * glyphSize.x;
			if (right <= left) right = left + glyphSize.x * 0.4f;
			drawList->AddRectFilled(ImVec2(left, y), ImVec2(right, y + glyphSize.y),
				IM_COL32(80, 80, 110, 150));
		}
	}

	// Glyphs.
	drawList->PushClipRect(ImVec2(textLeft, winTop), ImVec2(winRight, winBot), true);
	for (int r = firstRow; r <= lastRow; ++r)
	{
		const WrapRow& wr = wrapRows[r];
		auto& line = document[wr.line];
		float y = origin.y + r * glyphSize.y;

		int column = wr.startColumn;
		size_t index = document.getIndex(line, column);
		size_t lineSize = line.size();
		while (index < lineSize && column < wr.endColumn)
		{
			auto& glyph = line[index];
			auto codepoint = glyph.codepoint;
			ImVec2 glyphPos{ textLeft + (column - wr.startColumn) * glyphSize.x, y };
			if (codepoint == '\t')
			{
				if (showTabs)
				{
					const auto x1 = glyphPos.x + glyphSize.x * 0.3f;
					const auto ym = glyphPos.y + fontSize * 0.5f;
					const auto x2 = glyphPos.x + glyphSize.x;
					drawList->AddLine(ImVec2(x1, ym), ImVec2(x2, ym), palette.get(Color::whitespace));
				}
			}
			else if (codepoint == ' ')
			{
				if (showSpaces)
					drawList->AddCircleFilled(ImVec2(glyphPos.x + glyphSize.x * 0.5f,
						glyphPos.y + fontSize * 0.5f), 1.5f, palette.get(Color::whitespace), 4);
			}
			else
			{
				font->RenderChar(drawList, fontSize, glyphPos, palette.get(glyph.color), codepoint);
			}
			int tab = document.getTabSize();
			column += (codepoint == '\t') ? tab - (column % tab) : 1;
			++index;
		}
	}
	drawList->PopClipRect();

	// Carets.
	if (!readOnly && ImGui::IsWindowFocused() &&
		(!ImGui::GetIO().ConfigInputTextCursorBlink || cursorAnimationTimer < 0.5f))
	{
		for (auto& cursor : cursors)
		{
			auto pos = cursor.getInteractiveEnd();
			int r = wrapRowOfCoordinate(pos);
			if (r < firstRow || r > lastRow) continue;
			float x = textLeft + (pos.column - wrapRows[r].startColumn) * glyphSize.x - 1;
			float y = origin.y + r * glyphSize.y;
			drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + cursorWidth, y + glyphSize.y),
				palette.get(Color::cursor));
		}
	}
}


//
//	TextEditor::renderCursors
//



//
//	TextEditor::renderMargin
//

void TextEditor::renderMargin()
{
	if ((decoratorWidth != 0.0f && decoratorCallback) || showLineNumbers)
	{
		if (ImGui::GetScrollX() > 0.0f)
		{
			ImGui::GetWindowDrawList()->AddRectFilled(
				ImGui::GetWindowPos(),
				ImGui::GetWindowPos() + ImVec2(textOffset, ImGui::GetWindowSize().y),
				palette.get(Color::background));
		}
	}

	// folding arrows in the decoration area
	if (!foldRanges.foldingEnabled || foldRanges.empty())
		return;

	auto drawList = ImGui::GetWindowDrawList();
	ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();




	// Arrow centered in the decoration column, nudged a hair right so it's not
	// flush against the line-number gutter. Size scales with font (looks
	// sharper at high DPI than fixed 3/4-pixel triangles).
	const float arrowCenterX = ImGui::GetWindowPos().x + decorationOffset + glyphSize.x * 0.6f;
	const float arrowHalf = std::max(3.0f, glyphSize.y * 0.22f);
	for (auto& fr : foldRanges)
	{
		int line = fr.start.line;
		if (line < 0 || line >= document.lineCount() || !document[line].visible || line == fr.end.line)
			continue;
		// Cull off-screen fold arrows — only the visible range matters, and a
		// large file can have thousands of folds (was drawing them all/frame).
		if (line < firstVisibleLine || line > lastVisibleLine)
			continue;
		auto vi = lineToVisualIndex(line);
		float y = cursorScreenPos.y + vi * glyphSize.y + glyphSize.y * 0.5f;
		ImVec2 center(arrowCenterX, y);

		ImVec2 hmin(arrowCenterX - glyphSize.x * 0.75f, y - glyphSize.y * 0.5f);
		ImVec2 hmax(arrowCenterX + glyphSize.x * 0.75f, y + glyphSize.y * 0.5f);
		bool hovered = ImGui::IsMouseHoveringRect(hmin, hmax);

		ImU32 col = hovered
			? ImGui::GetColorU32(ImGuiCol_ButtonHovered)
			: ImGui::GetColorU32(ImGuiCol_TextDisabled);

		// Use AddTriangleFilled — drawList has AA on by default for filled
		// primitives in modern ImGui, so this avoids the jagged look of the
		// previous fixed-pixel triangles. Geometry centred on `center`.
		if (fr.folded)
		{
			// pointing right ▶
			ImVec2 p1(center.x - arrowHalf * 0.5f, center.y - arrowHalf);
			ImVec2 p2(center.x + arrowHalf * 0.8f, center.y);
			ImVec2 p3(center.x - arrowHalf * 0.5f, center.y + arrowHalf);
			drawList->AddTriangleFilled(p1, p2, p3, col);
		}
		else
		{
			// pointing down ▼
			ImVec2 p1(center.x - arrowHalf, center.y - arrowHalf * 0.5f);
			ImVec2 p2(center.x + arrowHalf, center.y - arrowHalf * 0.5f);
			ImVec2 p3(center.x, center.y + arrowHalf * 0.8f);
			drawList->AddTriangleFilled(p1, p2, p3, col);
		}

		if (hovered)
		{
			// click → toggle, double click → select the whole fold region
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				// select start..end then leave it folded/open as it was
				cursors.setCursor(fr.start, Coordinate(fr.end.line, document[fr.end.line].maxColumn));
			}
			else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				foldRanges.toggleFold(line, document);
			}
		}
	}

}

//
//	TextEditor::renderLineNumbers
//

void TextEditor::renderLineNumbers()
{
	if (!showLineNumbers)
		return;

	auto drawList = ImGui::GetWindowDrawList();
	auto cursorScreenPos = ImGui::GetCursorScreenPos();
	// A whole-line gutter selection ends at column 0 of the NEXT line, so the raw
	// caret line would highlight the line BELOW the one the user selected. Treat a
	// column-0 multi-line selection end as the line above it.
	auto& gutterCursor = cursors.getCurrent();
	auto cursorLine = gutterCursor.getInteractiveEnd().line;
	if (gutterCursor.hasSelection())
	{
		auto selS = gutterCursor.getSelectionStart();
		auto selE = gutterCursor.getSelectionEnd();
		if (selE.column == 0 && selE.line > selS.line)
			cursorLine = selE.line - 1;
	}
	auto position = ImVec2(ImGui::GetWindowPos().x + lineNumberRightOffset, cursorScreenPos.y);

	int firstVisibleIndex = lineToVisualIndex(firstVisibleLine);
	int lastVisibleIndex = lineToVisualIndex(lastVisibleLine);

	int visualRow = firstVisibleIndex;
	for (int vi = firstVisibleIndex; vi <= lastVisibleIndex; ++vi)
	{
		int line = visualIndexToLine(vi);
		if (line < 0 || line >= document.lineCount())
			continue;

		auto width = static_cast<int>(std::log10(line + 1) + 1.0f) * glyphSize.x;
		auto foreground = (line == cursorLine) ? Color::currentLineNumber : Color::lineNumber;
		auto number = std::to_string(line + 1);

		drawList->AddText(
			position + ImVec2(-width, visualRow * glyphSize.y),
			palette.get(foreground),
			number.c_str()
		);

		++visualRow;
	}

}
//
//	TextEditor::renderDecorations
//

void TextEditor::renderDecorations()
{
	if (decoratorWidth == 0.0f || !decoratorCallback)
		return;

	auto cursorScreenPos = ImGui::GetCursorScreenPos();
	auto position = ImVec2(
		ImGui::GetWindowPos().x + decorationOffset,
		cursorScreenPos.y + glyphSize.y * (visibleLineMap.empty() ? firstVisibleLine : 0)
	);

	auto widthInPixels = (decoratorWidth < 0.0f) ? -decoratorWidth * glyphSize.x : decoratorWidth;
	Decorator decorator{ 0, widthInPixels, glyphSize.y, glyphSize, nullptr, this, 0 };

	int firstVisibleIndex = lineToVisualIndex(firstVisibleLine);
	int lastVisibleIndex = lineToVisualIndex(lastVisibleLine);

	for (int vi = firstVisibleIndex; vi <= lastVisibleIndex; ++vi)
	{
		int line = visualIndexToLine(vi);
		decorator.line = line;
		decorator.userData = document.getUserData(line);

		ImGui::SetCursorScreenPos(position + ImVec2(0.0f, (vi - firstVisibleIndex) * glyphSize.y));
		ImGui::PushID(line);
		decoratorCallback(decorator);
		ImGui::PopID();
	}

	ImGui::SetCursorScreenPos(cursorScreenPos);
}


//
//	TextEditor::renderScrollbarMiniMap
//

void TextEditor::renderScrollbarMiniMap()
{
#ifdef TE_NO_IMGUI_INTERNAL
	// needs the scrollbar rect from Dear ImGui internals; the minimap is
	// simply disabled on public-API-only builds
#else
	// based on https://github.com/ocornut/imgui/issues/3114
	// messing with Dear ImGui internals
	if (showScrollbarMiniMap)
	{
		auto window = ImGui::GetCurrentWindow();

		if (window->ScrollbarY)
		{
			auto drawList = ImGui::GetWindowDrawList();
			auto rect = ImGui::GetWindowScrollbarRect(window, ImGuiAxis_Y);
			auto lineHeight = rect.GetHeight() / static_cast<float>(document.size());
			auto offset = (rect.Max.x - rect.Min.x) * 0.3f;
			auto left = rect.Min.x + offset;
			auto right = rect.Max.x - offset;

			drawList->PushClipRect(rect.Min, rect.Max, false);

			// render cursor locations
			for (auto& cursor : cursors)
			{
				auto begin = cursor.getSelectionStart();
				auto end = cursor.getSelectionEnd();

				auto ly1 = std::round(rect.Min.y + begin.line * lineHeight);
				auto ly2 = std::round(rect.Min.y + (end.line + 1) * lineHeight);

				drawList->AddRectFilled(ImVec2(left, ly1), ImVec2(right, ly2), palette.get(Color::selection));
			}

			// render marker locations
			if (markers.size())
			{
				for (size_t line = 0; line < document.size(); line++)
				{
					if (document[line].marker)
					{
						auto color = markers[document[line].marker - 1].textColor;

						if (!color)
						{
							color = markers[document[line].marker - 1].lineNumberColor;
						}

						auto ly = std::round(rect.Min.y + line * lineHeight);
						drawList->AddRectFilled(ImVec2(left, ly), ImVec2(right, ly + lineHeight), color);
					}
				}
			}

			drawList->PopClipRect();
		}
	}
#endif // TE_NO_IMGUI_INTERNAL
}


//
//	TextEditor::renderPanScrollIndicator
//

void TextEditor::renderPanScrollIndicator()
{
	if (!showPanScrollIndicator || (!panning && !scrolling))
		return;

	auto drawList = ImGui::GetWindowDrawList();
	// Anchor at the click point (captured on middle-press), not the window
	// centre, so the indicator appears where the user actually clicked.
	auto center = panScrollAnchor;
	center.x = std::round(center.x);
	center.y = std::round(center.y);

	// Simple but clearly visible indicator: a faint backing disc (so it reads
	// against any code colour), a crisp ring + centre dot, and 4 small arrows.
	// (Previous version at radius 10 / 55% alpha was effectively invisible.)
	const ImGuiStyle& style = ImGui::GetStyle();
	ImVec4 textC = style.Colors[ImGuiCol_Text];
	textC.w = 0.9f;
	ImU32 col = ImGui::ColorConvertFloat4ToU32(textC);

	constexpr float radius      = 12.0f;
	constexpr float armLen      = 6.0f;    // arrow tail extension past the ring
	constexpr float tipLen      = 4.5f;    // arrow tip leg length
	constexpr float tipSpread   = 3.0f;    // arrow tip half-width

	// Backing disc — low-alpha dark fill keeps the indicator legible without
	// really "covering" content (it's tiny and translucent).
	drawList->AddCircleFilled(center, radius + 4.0f, IM_COL32(20, 20, 24, 80));
	drawList->AddCircle(center, radius, col, 0, 2.0f);
	drawList->AddCircleFilled(center, 2.0f, col);

	// All four arrows whenever the indicator is active — middle-mouse pan
	// moves both axes, so showing only L/R was misleading.
	// Compute drag direction relative to the anchor. Used to bump the
	// matching arrow's alpha — same cue Windows' middle-click pan cursor
	// uses to show which way you're nudging.
	ImVec2 mouse = ImGui::GetMousePos();
	float dx = mouse.x - center.x;
	float dy = mouse.y - center.y;
	float dragMag = std::sqrt(dx * dx + dy * dy);
	bool dragging = dragMag > 4.0f;

	auto drawArrow = [&](float dirX, float dirY) {
		// Brighten the arrow when the user is pulling that way.
		ImU32 c = col;
		if (dragging) {
			float dot = dirX * (dx / dragMag) + dirY * (dy / dragMag);
			if (dot > 0.35f) {
				ImVec4 hi = style.Colors[ImGuiCol_Text];
				hi.w = 0.95f;
				c = ImGui::ColorConvertFloat4ToU32(hi);
			}
		}
		float baseX = center.x + dirX * radius;
		float baseY = center.y + dirY * radius;
		float tipX  = baseX + dirX * armLen;
		float tipY  = baseY + dirY * armLen;
		// Two short legs forming a chevron at the tip.
		float pX = -dirY * tipSpread;
		float pY =  dirX * tipSpread;
		drawList->AddLine(ImVec2(baseX, baseY), ImVec2(tipX, tipY), c, 1.5f);
		drawList->AddLine(ImVec2(tipX, tipY),
			ImVec2(tipX - dirX * tipLen + pX, tipY - dirY * tipLen + pY), c, 1.5f);
		drawList->AddLine(ImVec2(tipX, tipY),
			ImVec2(tipX - dirX * tipLen - pX, tipY - dirY * tipLen - pY), c, 1.5f);
	};
	drawArrow(-1.0f,  0.0f);   // ←
	drawArrow( 1.0f,  0.0f);   // →
	drawArrow( 0.0f, -1.0f);   // ↑
	drawArrow( 0.0f,  1.0f);   // ↓
}


//
//	TextEditor::handleKeyboardInputs
//

void TextEditor::SetKeyChordOverride(const std::string& action, const std::string& chord)
{
	if (chord.empty()) keyChordOverrides.erase(action);
	else               keyChordOverrides[action] = chord;
}

// Map a glyph key token to the word ImGui::GetKeyName returns ("=" -> "Equal").
// Must mirror the host app's normalizer so overrides round-trip; without it,
// punctuation chords (Ctrl+], Ctrl+/) never match.
static std::string teNormalizeKeyToken(const std::string& tok)
{
	if (tok == "=")  return "Equal";
	if (tok == "-")  return "Minus";
	if (tok == "\\") return "Backslash";
	if (tok == "/")  return "Slash";
	if (tok == "[")  return "LeftBracket";
	if (tok == "]")  return "RightBracket";
	if (tok == ";")  return "Semicolon";
	if (tok == "'")  return "Apostrophe";
	if (tok == ",")  return "Comma";
	if (tok == ".")  return "Period";
	if (tok == "`")  return "GraveAccent";
	if (tok == "+")  return "KeypadAdd";
	return tok;
}

// Test a single-combo chord string ("Ctrl+Shift+U") against the live key state:
// exact modifier set + the named key pressed this frame. Multi-stroke chords
// (containing a space) are not matched here. Mirrors the host app's matcher so
// chords round-trip with what the Settings recorder produces.
static bool teChordPressed(const std::string& chord)
{
	if (chord.empty() || chord.find(' ') != std::string::npos) return false;
	bool needCtrl = false, needShift = false, needAlt = false, needSuper = false;
	std::string keyName;
	size_t pos = 0;
	while (pos < chord.size())
	{
		size_t plus = chord.find('+', pos);
		std::string tok = (plus == std::string::npos) ? chord.substr(pos) : chord.substr(pos, plus - pos);
		if      (tok == "Ctrl")  needCtrl  = true;
		else if (tok == "Shift") needShift = true;
		else if (tok == "Alt")   needAlt   = true;
		else if (tok == "Super") needSuper = true;
		else if (!tok.empty())   keyName   = tok;
		if (plus == std::string::npos) break;
		pos = plus + 1;
		if (pos < chord.size() && chord[pos] == '+') { keyName = "+"; break; }
	}
	if (keyName.empty()) return false;
	keyName = teNormalizeKeyToken(keyName);
	ImGuiIO& io = ImGui::GetIO();
	if (io.KeyCtrl != needCtrl || io.KeyShift != needShift ||
	    io.KeyAlt != needAlt || io.KeySuper != needSuper)
		return false;
	for (ImGuiKey k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k = (ImGuiKey)(k + 1))
	{
		const char* n = ImGui::GetKeyName(k);
		if (n && n[0] && keyName == n)
			return ImGui::IsKeyPressed(k, false);
	}
	return false;
}

bool TextEditor::tryKeyChordOverrides()
{
	if (keyChordOverrides.empty()) { keyChordPending.clear(); return false; }

	// Age out / cancel a half-entered two-stroke prefix (mirrors the host app).
	if (!keyChordPending.empty()) {
		keyChordPendingAge += ImGui::GetIO().DeltaTime;
		if (keyChordPendingAge > 1.2f || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			keyChordPending.clear();
	}

	// Match a chord that may be single ("Ctrl+U") or two-stroke ("Ctrl+K Ctrl+U").
	// For two-stroke: the first combo arms keyChordPending (the awaited second
	// combo) and returns false; the second combo completes it.
	auto matches = [&](const std::string& chord) -> bool {
		auto sp = chord.find(' ');
		if (sp == std::string::npos) return teChordPressed(chord);
		std::string first = chord.substr(0, sp), second = chord.substr(sp + 1);
		if (!keyChordPending.empty() && keyChordPending == second)
			return teChordPressed(second) ? (keyChordPending.clear(), true) : false;
		if (teChordPressed(first)) { keyChordPending = second; keyChordPendingAge = 0.0f; }
		return false;
	};

	// Map action id -> the action to run. Checked before the default keyboard
	// chain; first matching override wins and suppresses default handling.
	for (auto& [action, chord] : keyChordOverrides)
	{
		if (!matches(chord)) continue;
		if      (action == "undo")           { if (!readOnly) undo(); }
		else if (action == "redo")           { if (!readOnly) redo(); }
		else if (action == "cut")            { if (!readOnly) cut(); }
		else if (action == "copy")           { copy(); }
		else if (action == "paste")          { if (!readOnly) paste(); }
		else if (action == "selectAll")      { selectAll(); }
		else if (action == "addNextOccurrence") { if (cursors.currentCursorHasSelection()) addNextOccurrence(); }
		else if (action == "toggleComments") { if (!readOnly && language) toggleComments(); }
		else if (action == "indent")         { if (!readOnly) indentLines(); }
		else if (action == "deindent")       { if (!readOnly) deindentLines(); }
		else if (action == "selectAllOccurrences") { selectAllOccurrences(); }
		else if (action == "moveLineUp")     { if (!readOnly) moveUpLines(); }
		else if (action == "moveLineDown")   { if (!readOnly) moveDownLines(); }
		else if (action == "find")           { openFindReplace(); }
		else if (action == "findNext")       { findNext(); }
		else if (action == "findAll")        { findAll(); }
		else if (action == "upperCase")      { if (!readOnly) selectionToUpperCase(); }
		else if (action == "lowerCase")      { if (!readOnly) selectionToLowerCase(); }
		else if (action == "foldAll")        { if (foldRanges.foldingEnabled) FoldAll(); }
		else if (action == "unfoldAll")      { if (foldRanges.foldingEnabled) UnfoldAll(); }
		else if (action == "foldCurrent")    { if (foldRanges.foldingEnabled) FoldCurrent(); }
		else if (action == "unfoldCurrent")  { if (foldRanges.foldingEnabled) UnfoldCurrent(); }
		else continue;   // unknown id — ignore, keep scanning
		return true;
	}
	return false;
}

void TextEditor::handleKeyboardInputs()
{

	// Only intercept keyboard when THIS editor's child window has focus AND no
	// other ImGui input widget (e.g. the Find/Replace textbox or the file
	// dialog's name field) is currently capturing text. Otherwise typing in
	// those widgets would also fire handleCharacter() and either steal focus
	// back or insert into the document instead.
	bool editorIsFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	bool otherWidgetActive = ImGui::IsAnyItemActive();
	if (editorIsFocused && !otherWidgetActive)
	{
		auto& io = ImGui::GetIO();
		io.WantCaptureKeyboard = true;
		io.WantTextInput = true;
		static std::vector<ImGuiKey> shortcutKeysQueue{};

		// Host-app keybinding overrides take precedence over the built-in chords.
		// If one fires this frame, skip default handling so the rebinding wins.
		if (tryKeyChordOverrides()) return;

		// get state of modifier keys
		auto shift = ImGui::IsKeyDown(ImGuiMod_Shift);
		auto ctrl = ImGui::IsKeyDown(ImGuiMod_Ctrl);
		auto alt = ImGui::IsKeyDown(ImGuiMod_Alt);
		if (!ctrl)
		{
			shortcutKeysQueue.clear();
		}
		auto isNoModifiers = !ctrl && !shift && !alt;
		auto isShortcut = ctrl && !shift && !alt;
		auto isShiftShortcut = ctrl && shift && !alt;
		auto isOptionalShiftShortcut = ctrl && !alt;
		auto isAltOnly = !ctrl && !shift && alt;
		auto isShiftOnly = !ctrl && shift && !alt;
		auto isOptionalShift = !ctrl && !alt;
		//auto isOptionalAlt = !ctrl && !shift;

#if __APPLE__
		// Dear ImGui switches the Cmd(Super) and Ctrl keys on MacOS
		auto super = ImGui::IsKeyDown(ImGuiMod_Super);
		auto isCtrlShift = !ctrl && shift && !alt && super;
		auto isOptionalAltShift = !ctrl;
#else
		auto isOptionalCtrlShift = !alt;
		(void)isOptionalCtrlShift;  // currently unused on non-Apple builds
#endif

		// ── Chord shortcut handling ──────────────────────────────────────
		// shortcutKeysQueue tracks the prefix of a multi-key shortcut that
		// requires Ctrl held throughout (VSCode-style "Ctrl+K Ctrl+U" etc.).
		// The queue is cleared whenever Ctrl is released (above), so chords
		// must be typed without letting go of Ctrl.
		if (isShortcut && !shortcutKeysQueue.empty())
		{
			// A chord prefix is already queued — interpret the second key.
			if (shortcutKeysQueue.front() == ImGuiKey_K)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_U)) { selectionToUpperCase();    shortcutKeysQueue.clear(); return; }
				else if (ImGui::IsKeyPressed(ImGuiKey_L)) { selectionToLowerCase();    shortcutKeysQueue.clear(); return; }
				else if (ImGui::IsKeyPressed(ImGuiKey_0)) { foldRanges.foldAll(document);   shortcutKeysQueue.clear(); return; }
				else if (ImGui::IsKeyPressed(ImGuiKey_J)) { foldRanges.unfoldAll(document); shortcutKeysQueue.clear(); return; }
				// Any other key cancels the chord and falls through to normal handling.
				if (ImGui::GetIO().InputQueueCharacters.Size > 0 ||
					ImGui::IsKeyPressed(ImGuiKey_Escape))
				{
					shortcutKeysQueue.clear();
				}
			}
			// Don't dispatch more handlers while we're inside a pending chord.
			return;
		}
		// Open a Ctrl+K chord prefix. Other Ctrl+K combinations (e.g. Ctrl+Shift+K
		// for removeSelectedLines) still work because they require Shift, not bare Ctrl.
		if (isShortcut && ImGui::IsKeyPressed(ImGuiKey_K))
		{
			shortcutKeysQueue.push_back(ImGuiKey_K);
			return;
		}

		// ignore specific keys when autocomplete is active, they will be handled later
		if (autocomplete.isActive() && autocomplete.isSpecialKeyPressed())
		{
			if (autocomplete.hasSuggestions())
			{
				return;

			}
			else
			{
				// this is the exception, cancel autocomplete when special keys are used without any suggestions
				autocomplete.cancel();
			}
		}

		// cursor movements and selections
		if (isOptionalShift && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { moveUp(1, shift); }
		else if (isOptionalShift && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { moveDown(1, shift); }

#if __APPLE__
		else if (isCtrlShift && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { shrinkSelectionsToCurlyBrackets(); }
		else if (isCtrlShift && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { growSelectionsToCurlyBrackets(); }
		// On macOS: Cmd (Super) = word jump, Alt = subword jump, Shift = select.
		// This keeps the (select, wordMode, subWord) param layout identical to
		// the Windows/Linux branch below.
		else if (isOptionalAltShift && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { moveLeft(shift, super, alt); }
		else if (isOptionalAltShift && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { moveRight(shift, super, alt); }
#else
		// Windows/Linux: Ctrl = word jump, Alt = subword jump, Shift = select — so
		// Alt+Shift+arrow is subword-select (Sublime-style). Ctrl+Alt+arrow is NOT
		// consumed here: it's left for the host app's nav back/forward. Bracket
		// grow/shrink moved to Ctrl+Shift+[ / ] so it no longer steals subword-select.
		else if (ctrl && shift && !alt && ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) { shrinkSelectionsToCurlyBrackets(); }
		else if (ctrl && shift && !alt && ImGui::IsKeyPressed(ImGuiKey_RightBracket)) { growSelectionsToCurlyBrackets(); }
		else if (!(ctrl && alt) && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { moveLeft(shift, ctrl, alt); }
		else if (!(ctrl && alt) && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { moveRight(shift, ctrl, alt); }
#endif

		else if (isOptionalShift && ImGui::IsKeyPressed(ImGuiKey_PageUp)) { moveUp(visibleLines - 2, shift); }
		else if (isOptionalShift && ImGui::IsKeyPressed(ImGuiKey_PageDown)) { moveDown(visibleLines - 2, shift); }
		else if (isOptionalShiftShortcut && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { moveToTop(shift); }
		else if (isOptionalShiftShortcut && ImGui::IsKeyPressed(ImGuiKey_Home)) { moveToTop(shift); }
		else if (isOptionalShiftShortcut && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { moveToBottom(shift); }
		else if (isOptionalShiftShortcut && ImGui::IsKeyPressed(ImGuiKey_End)) { moveToBottom(shift); }
		else if (isOptionalShift && ImGui::IsKeyPressed(ImGuiKey_Home)) { moveToStartOfLine(shift); }
		else if (isOptionalShift && ImGui::IsKeyPressed(ImGuiKey_End)) { moveToEndOfLine(shift); }
		else if (isShortcut && ImGui::IsKeyPressed(ImGuiKey_A)) { selectAll(); }
		// Ctrl+L: select the current line for EVERY cursor (extends each selection
		// down to the next line on repeat — like VSCode). Multi-cursor aware.
		else if (isShortcut && ImGui::IsKeyPressed(ImGuiKey_L))
		{
			selectCursorLines();
		}

		else if (isShortcut && ImGui::IsKeyPressed(ImGuiKey_D) && cursors.currentCursorHasSelection()) { addNextOccurrence(); }

		// clipboard operations
		else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X)) { cut(); }
		//else if (isShiftOnly && ImGui::IsKeyPressed(ImGuiKey_Delete)) { cut(); }
		else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C)) { copy(); }
		//else if (isShortcut && ImGui::IsKeyPressed(ImGuiKey_Insert)) { copy(); }

		else if (!readOnly && ctrl && ImGui::IsKeyPressed(ImGuiKey_V)) { paste(); }
		else if (!readOnly && isShiftOnly && ImGui::IsKeyPressed(ImGuiKey_Insert)) { paste(); }
		else if (!readOnly && ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_Z)) { undo(); }
		else if (!readOnly && ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_Z)) { redo(); }
		else if (!readOnly && ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) { redo(); }

		// remove text
		else if (!readOnly  && ImGui::IsKeyPressed(ImGuiKey_Delete)) { handleDelete(shift); }
		else if (!readOnly && ImGui::IsKeyPressed(ImGuiKey_Backspace)) { handleBackspace(ctrl); }
		else if (!readOnly && isShiftShortcut && ImGui::IsKeyPressed(ImGuiKey_K)) { removeSelectedLines(); }

		// text manipulation
		else if (!readOnly && isShortcut && ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) { deindentLines(); }
		else if (!readOnly && isShortcut && ImGui::IsKeyPressed(ImGuiKey_RightBracket)) { indentLines(); }
		else if (!readOnly && isAltOnly && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { moveUpLines(); }
		else if (!readOnly && isAltOnly && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { moveDownLines(); }
		else if (!readOnly && language && ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_Slash)) { toggleCommentsShift(); }
		else if (!readOnly && language && ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_Slash)) { toggleComments(); }

		// find/replace support
		else if (isShortcut && ImGui::IsKeyPressed(ImGuiKey_F))
		{
			if (autocomplete.isActive())
			{
				autocomplete.cancel();
				findCancelledAutocomplete = true;
			}

			openFindReplace();
		}
		else if (isShortcut && ImGui::IsKeyPressed(ImGuiKey_H))
		{
			openFindReplace();
		}
		else if (isShiftShortcut && ImGui::IsKeyPressed(ImGuiKey_F)) { findAll(); }
		// F3 (or Ctrl+G as a legacy fallback) advances find. The Ctrl+G slot
		// is owned by the host app's "Go to Line" dialog; keep it here only
		// when no app-level handler is going to claim it.
		else if (ImGui::IsKeyPressed(ImGuiKey_F3)) { findNext(); }

		// folding shortcuts (VSCode-style)
		// Ctrl+K Ctrl+0 = fold all, Ctrl+K Ctrl+J = unfold all
		// Ctrl+Shift+[ = fold current, Ctrl+Shift+] = unfold current
		else if (foldRanges.foldingEnabled && isShiftShortcut && ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
		{
			FoldCurrent();
		}
		else if (foldRanges.foldingEnabled && isShiftShortcut && ImGui::IsKeyPressed(ImGuiKey_RightBracket))
		{
			UnfoldCurrent();
		}
		else if (foldRanges.foldingEnabled && isShortcut && ImGui::IsKeyPressed(ImGuiKey_0))
		{
			FoldAll();
		}
		else if (foldRanges.foldingEnabled && isShortcut && ImGui::IsKeyPressed(ImGuiKey_J))
		{
			UnfoldAll();
		}

		// autocomplete support
		else if (!readOnly && ImGui::IsKeyChordPressed(autocomplete.getTriggerShortcut()))
		{
			// don't activate if we have multiple cursors active
			if (cursors.hasMultiple())
			{
				// TODO: inform user

			}
			else
			{
				if (autocomplete.startShortcut(cursors))
				{
					makeCursorVisible();
				}
			}
		}

		// change insert mode
		else if (isNoModifiers && ImGui::IsKeyPressed(ImGuiKey_Insert)) { overwrite = !overwrite; }

		// handle new line
		else if (!readOnly && isNoModifiers && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) { handleCharacter('\n'); }
		else if (!readOnly && isShortcut && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) { insertLineBelow(); }
		else if (!readOnly && isShiftShortcut && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) { insertLineAbove(); }

		// handle tabs
		else if (!readOnly && isOptionalShift && ImGui::IsKeyPressed(ImGuiKey_Tab))
		{
			if (shift)
			{
				// Shift-Tab always deindents — current line(s) when no selection,
				// selected line range when there is one (deindentLines handles both).
				deindentLines();
			}
			else if (cursors.anyHasSelection())
			{
				indentLines();
			}
			else
			{
				handleCharacter('\t');
			}
		}

		// handle escape key
		else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			if (autocomplete.isActive())
			{
				autocomplete.cancel();

			}
			else if (findReplaceVisible)
			{
				closeFindReplace();

			}
			else if (cursors.hasMultiple())
			{
				cursors.clearAdditional();
			}
		}

		// handle regular text
		if (!io.InputQueueCharacters.empty())
		{
			// ignore Ctrl inputs, but need to allow Alt+Ctrl as some keyboards (e.g. German) use AltGR (which is Alt+Ctrl) to input certain characters
			if (!(io.KeyCtrl && !io.KeyAlt) && !readOnly)
			{
				for (auto i = 0; i < io.InputQueueCharacters.size(); i++)
				{
					auto character = io.InputQueueCharacters[i];

					if (character == '\n' || character >= 32)
					{
						handleCharacter(character);
					}
				}
			}

			io.InputQueueCharacters.resize(0);
		}
	}
}



//
//	TextEditor::handleMouseInteractions
//

void TextEditor::handleMouseInteractions()
{
	// handle middle mouse button modes
	panning &= panMode && ImGui::IsMouseDown(ImGuiMouseButton_Middle);
	auto absoluteMousePos = ImGui::GetMousePos() - ImGui::GetWindowPos();

	// Pan/scroll acceleration: scroll speed grows superlinearly with how far the
	// cursor sits from the middle-click anchor ("scroll cursor"). Near the anchor
	// the multiplier is ~1 (precise); far out it climbs quadratically (fast),
	// capped so it can't run away. Reference distance ~8 text lines.
	auto panAccel = [&](float distPx) -> float {
		if (panScrollAccelGain <= 0.0f) return 1.0f;
		float ref = glyphSize.y * 8.0f;
		float t = (ref > 0.0f) ? (distPx / ref) : 0.0f;
		float a = 1.0f + t * t * panScrollAccelGain * 0.25f;   // 0.25: user 1.0 == old 0.25
		return (a > 16.0f) ? 16.0f : a;
	};

	if (panning && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
	{
		// handle middle mouse button panning
		auto windowSize = ImGui::GetWindowSize();
		auto mouseDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
		float dragFactor = ImGui::GetIO().DeltaTime * 15.0f;
		ImVec2 autoPanMargin(glyphSize.x * 4.0f, glyphSize.y * 2.0f);

		if (absoluteMousePos.x < textOffset + autoPanMargin.x)
		{
			mouseDelta.x = (absoluteMousePos.x - (textOffset + autoPanMargin.x)) * dragFactor;
		}
		else if (absoluteMousePos.x > windowSize.x - verticalScrollBarSize - autoPanMargin.x)
		{
			mouseDelta.x = (absoluteMousePos.x - (windowSize.x - verticalScrollBarSize - autoPanMargin.x)) * dragFactor * 0.5f;
		}

		if (absoluteMousePos.y < autoPanMargin.y)
		{
			mouseDelta.y = (absoluteMousePos.y - autoPanMargin.y) * dragFactor;
		}
		else if (absoluteMousePos.y > windowSize.y - horizontalScrollBarSize - autoPanMargin.y)
		{
			mouseDelta.y = (absoluteMousePos.y - (windowSize.y - horizontalScrollBarSize - autoPanMargin.y)) * dragFactor;
		}

		float panSign = panInverted ? -1.0f : 1.0f;
		// Acceleration (vertical-only) scales with distance from the anchor.
		ImVec2 aRel = ImGui::GetMousePos() - panScrollAnchor;
		float accel = panAccel(std::sqrt(aRel.x * aRel.x + aRel.y * aRel.y));
		// Axis SNAP: re-decide every frame (so direction can change mid-drag) but
		// snap to ONE axis so there's no diagonal drift. The decision runs on a
		// smoothed velocity (panVelEMA) and uses hysteresis with a strong vertical
		// bias, so it doesn't flip-flop near the boundary: vertical is the default
		// and is sticky (need |dx| > 3|dy| to flip to horizontal), horizontal drops
		// back as soon as the motion is no longer clearly sideways (|dx| < 1.5|dy|),
		// and horizontal only engages when there's real horizontal scroll room.
		panVelEMA.x = panVelEMA.x * 0.7f + mouseDelta.x * 0.3f;
		panVelEMA.y = panVelEMA.y * 0.7f + mouseDelta.y * 0.3f;
		float vAx = std::fabs(panVelEMA.x), vAy = std::fabs(panVelEMA.y);
		bool canHoriz = ImGui::GetScrollMaxX() > glyphSize.x * 4.0f;
		if (panSnapAxis == 1) {                       // currently horizontal
			if (!canHoriz || vAx < vAy * 1.5f) panSnapAxis = 2;
		} else {                                      // currently vertical (default)
			if (canHoriz && vAx > vAy * 3.0f) panSnapAxis = 1;
		}
		if (panSnapAxis == 1)
			ImGui::SetScrollX(ImGui::GetScrollX() - panSign * mouseDelta.x * 0.35f);
		else
			ImGui::SetScrollY(ImGui::GetScrollY() - panSign * mouseDelta.y * accel);
		ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
	}
	else if (scrolling)
	{
		// handle middle mouse button scrolling. Horizontal gets a larger
		// deadzone + damping so vertical scrolling is the easy default.
		float deadzone  = glyphSize.x;
		float deadzoneX = glyphSize.x * 3.0f;
		auto offset = scrollStart - absoluteMousePos;
		offset.x = (offset.x < 0.0f) ? std::min(offset.x + deadzoneX, 0.0f) : std::max(offset.x - deadzoneX, 0.0f);
		offset.y = (offset.y < 0.0f) ? std::min(offset.y + deadzone,  0.0f) : std::max(offset.y - deadzone,  0.0f);

		float panSign = panInverted ? -1.0f : 1.0f;
		// Axis SNAP, same hysteresis as pan. `offset` (post-deadzone) is already the
		// stable anchor-relative vector, so no smoothing is needed — move the cursor
		// to a different side of the anchor to change direction mid-scroll.
		{
			float oAx = std::fabs(offset.x), oAy = std::fabs(offset.y);
			bool canHoriz = ImGui::GetScrollMaxX() > glyphSize.x * 4.0f;
			if (panSnapAxis == 1) {
				if (!canHoriz || oAx < oAy * 1.5f) panSnapAxis = 2;
			} else {
				if (canHoriz && oAx > oAy * 3.0f) panSnapAxis = 1;
			}
		}
		// Acceleration (vertical-only) from the px distance, before time scaling.
		float accel = panAccel(std::sqrt(offset.x * offset.x + offset.y * offset.y));
		float scrollFactor = ImGui::GetIO().DeltaTime * 5.0f;
		if (panSnapAxis == 1)
			ImGui::SetScrollX(ImGui::GetScrollX() - panSign * offset.x * scrollFactor * 0.35f);
		else
			ImGui::SetScrollY(ImGui::GetScrollY() - panSign * offset.y * scrollFactor * accel);

		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
			ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
			ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{

			scrolling = false;
		}
	}
	else if (ImGui::IsWindowHovered())
	{
		auto io = ImGui::GetIO();
		auto mousePos = ImGui::GetMousePos() - ImGui::GetCursorScreenPos();

		bool overLineNumbers = showLineNumbers &&
			(absoluteMousePos.x > lineNumberLeftOffset) &&
			(absoluteMousePos.x < lineNumberRightOffset);

		bool overDecorator = (decoratorWidth != 0.0f) &&
			(absoluteMousePos.x >= decorationOffset) &&
			(absoluteMousePos.x < textOffset);

		// Symmetric Y bound — without this, the mouse over the horizontal
		// scrollbar (below the text area but inside the text column) still
		// passes the X check and we'd swap to the I-beam cursor / start a
		// drag-select. Ported from upstream Pascal Thomet's fix.
		bool overText = mousePos.x - ImGui::GetScrollX() > textOffset
		             && mousePos.y - ImGui::GetScrollY() >= 0
		             && mousePos.y - ImGui::GetScrollY() < visibleHeight;

		// Map mouse Y to visible line index. GetCursorScreenPos after BeginChild
		// returns the *content* origin (above the viewport when scrolled), so
		// mousePos.y already includes the scroll — don't add scrollY again.
		int visibleIndex = std::max(
			static_cast<int>(std::floor(mousePos.y / glyphSize.y)),
			0
		);

		// Compute glyph/cursor coordinates in document space.
		Coordinate glyphCoordinate;
		Coordinate cursorCoordinate;

		if (wordWrap && !wrapRows.empty())
		{
			// In wrap mode the visible index IS the wrapped-row index. Map it to
			// the source line + a column offset into that row, so clicks land on
			// the glyph under the cursor regardless of how the line was wrapped.
			int row = std::clamp(visibleIndex, 0, static_cast<int>(wrapRows.size()) - 1);
			int realLine = wrapRows[row].line;
			// Fractional column (see the non-wrap branch): single tab-aware midpoint snap.
			float colInRow = std::max(0.0f, (mousePos.x - textOffset) / glyphSize.x);
			float column = static_cast<float>(wrapRows[row].startColumn) + colInRow;
			document.normalizeCoordinate(1.0f * realLine, column,
				glyphCoordinate, cursorCoordinate);
		}
		else
		{
			int realLine = visualIndexToLine(visibleIndex);
			// Pass the FRACTIONAL visual column so normalizeCoordinate does ONE tab-aware
			// midpoint snap from the true click position. Pre-flooring via int xToColumn
			// dropped the sub-cell offset, which on tab-containing lines (wide cells)
			// biased the caret off the click — the roadmap's "cursor X mis-alignment".
			// Proportional still needs the measured walk; monospace is an exact x/glyphSize.x.
			float vcol = proportional
				? static_cast<float>(xToColumn(realLine, mousePos.x - textOffset))
				: (mousePos.x - textOffset) / glyphSize.x;
			document.normalizeCoordinate(
				1.0f * realLine,
				std::max(0.0f, vcol),
				glyphCoordinate,
				cursorCoordinate
			);
		}

		// show text cursor if required
		if (ImGui::IsWindowFocused() && overText)
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
		}
		// Promote a pending click-in-selection to a real drag once the user
		// actually moves the mouse (ImGui::IsMouseDragging with a meaningful
		// threshold). Stops a quick click from being mistaken for a move.
		if (pendingClickInSelection && !draggingSelection &&
			ImGui::IsMouseDragging(ImGuiMouseButton_Left, /*lock_threshold*/ 6.0f))
		{
			auto& cur = cursors.getCurrent();
			if (cur.hasSelection())
			{
				draggedText = GetCursorText(cursors.getCurrentIndex());
				dragSelectionStart = cur.getSelectionStart();
				dragSelectionEnd = cur.getSelectionEnd();
				draggingSelection = true;
				pendingClickInSelection = false;
			}
		}

		if (draggingSelection)
		{
			io.WantCaptureMouse = true;
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			// Floating preview of the dragged text near the cursor.
			if (!draggedText.empty())
			{
				bool willCopy = ImGui::IsKeyDown(ImGuiMod_Ctrl);
				std::string preview = draggedText;
				if (preview.size() > 60) preview = preview.substr(0, 60) + "…";
				// Strip newlines in the preview so the tooltip stays one-line.
				for (auto& c : preview) if (c == '\n' || c == '\r') c = ' ';
				ImGui::SetTooltip("%s %s", willCopy ? "Copy:" : "Move:", preview.c_str());
			}

			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				bool copy = ImGui::IsKeyDown(ImGuiMod_Ctrl);
				// Drop position is the current mouse-mapped cursor coordinate.
				// Reject drops that land inside the source selection (would
				// otherwise self-destruct on move).
				bool inSource = !(cursorCoordinate < dragSelectionStart || dragSelectionEnd < cursorCoordinate);
				if (!inSource && !draggedText.empty() && !readOnly)
				{
					auto transaction = startTransaction();
					if (!copy)
					{
						// MOVE: delete the source range first, then adjust the
						// drop coordinate if it lay after the deletion (the
						// deletion shifts everything past it leftward/upward).
						Coordinate adjustedDrop = cursorCoordinate;
						deleteText(transaction, dragSelectionStart, dragSelectionEnd);
						if (dragSelectionEnd < adjustedDrop ||
							(dragSelectionEnd.line == adjustedDrop.line && dragSelectionEnd.column <= adjustedDrop.column))
						{
							// Drop was past the deleted range; recompute it.
							if (adjustedDrop.line == dragSelectionEnd.line)
							{
								adjustedDrop.column -= (dragSelectionEnd.column - dragSelectionStart.column);
								adjustedDrop.line = dragSelectionStart.line;
							}
							else
							{
								adjustedDrop.line -= (dragSelectionEnd.line - dragSelectionStart.line);
							}
						}
						auto end = insertText(transaction, adjustedDrop, draggedText);
						cursors.setCursor(adjustedDrop, end);
					}
					else
					{
						// COPY: just insert at the drop position.
						auto end = insertText(transaction, cursorCoordinate, draggedText);
						cursors.setCursor(cursorCoordinate, end);
					}
					endTransaction(transaction);
				}
				draggingSelection = false;
				draggedText.clear();
			}
		}
#if !__APPLE__
		// Mid-drag axis switch: hold Alt during an active left-drag to convert a
		// normal selection into a column/vertical one (and release Alt to go back),
		// so the choice isn't locked in before the drag starts. (Skipped on macOS
		// where Alt is the multi-cursor modifier.)
		if (selectingText && !draggingSelection && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
		{
			bool altDown = ImGui::IsKeyDown(ImGuiMod_Alt);
			if (altDown && !columnSelecting)
			{
				columnSelecting = true;
				columnAnchor = cursors.getCurrent().getInteractiveStart();  // anchor = drag start
			}
			else if (!altDown && columnSelecting)
			{
				columnSelecting = false;
				cursors.setCursor(columnAnchor);   // collapse the box back to one cursor
			}
		}
#endif
		// --- Column / box selection (Alt+drag) -----------------------------
		// One vertical anchor column → one cursor per intermediate line, each
		// selecting from the anchor column to the current mouse column. VSCode
		// / Sublime style. The mode is exited on mouse release further down.
		if (columnSelecting && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
		{
			io.WantCaptureMouse = true;
			int loLine = std::min(columnAnchor.line, cursorCoordinate.line);
			int hiLine = std::max(columnAnchor.line, cursorCoordinate.line);
			int anchorCol = columnAnchor.column;
			int dragCol = cursorCoordinate.column;
			int selLo = std::min(anchorCol, dragCol);
			int selHi = std::max(anchorCol, dragCol);

			// VS Code-style rectangle: select [selLo, selHi] on EVERY line in the
			// range, letting selHi (and selLo) extend past the end of short lines
			// into virtual space rather than dropping or clipping them — so the box
			// has a straight right edge. Every cursor gets the SAME columns, so they
			// can never become mismatched "stub" cursors; Cursors::update() only
			// merges *overlapping* selections (different lines never overlap) and
			// doesn't re-normalize coordinates, so the virtual columns are stable.
			bool first = true;
			for (int ln = loLine; ln <= hiLine; ++ln)
			{
				if (ln < 0 || ln >= document.lineCount()) continue;
				if (!document[ln].visible) continue;
				Coordinate a(ln, selLo), b(ln, selHi);
				if (first) { cursors.setCursor(a, b); first = false; }
				else { cursors.addCursor(a, b); }
			}
			if (first) cursors.setCursor(columnAnchor);
			makeCursorVisible();
		}
		// --- Dragging selection with left mouse button -------------------
		// Only honour drags that ACTUALLY started on the text or line-number
		// gutter — otherwise dragging the horizontal scrollbar (which lives
		// inside the same child window) would happily extend the selection.
		else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !draggingSelection && selectingText)
		{
			io.WantCaptureMouse = true;
			// If the mouse moved at all while we had a pending click on an
			// existing selection, that's no longer a deselect-click — fall
			// through to the renderSelections hover handler which will pick
			// it up as a drag of the selected text.
			pendingClickInSelection = false;
			if (overLineNumbers)
			{
				auto& cursor = cursors.getCurrent();
				auto start = Coordinate(cursorCoordinate.line, 0);
				auto end = document.getDown(start);
				cursor.update(cursor.getInteractiveEnd() < cursor.getInteractiveStart() ? start : end);
			}
			else
			{
				cursors.updateCurrentCursor(cursorCoordinate);
			}

			makeCursorVisible();
		}
		// --- End of drag: commit deferred click if we never moved --------
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			columnSelecting = false;
			selectingText = false;
			if (pendingClickInSelection && !draggingSelection)
			{
				// User clicked inside a selection and didn't drag — treat it
				// as a normal click that places the cursor.
				cursors.setCursor(pendingClickCoord);
				makeCursorVisible();
			}
			pendingClickInSelection = false;
		}
		// --- Middle click: pan/scroll mode -------------------------------
		else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
		{
			// Anchor the on-screen indicator at the exact click point (screen
			// space) for both modes, so it appears where the user pressed.
			panScrollAnchor = ImGui::GetMousePos();
			panSnapAxis = 2;                    // start vertical-biased
			panVelEMA = ImVec2(0.0f, 0.0f);     // clear smoothed velocity
			if (panMode)
			{
				panning = true;
			}
			else
			{
				scrolling = true;
				scrollStart = absoluteMousePos;
			}
		}
		// --- Right click: context menus ---------------------------------
		else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			if (overLineNumbers && lineNumberContextMenuCallback)
			{
				contextMenuLine = glyphCoordinate.line;
				ImGui::OpenPopup("LineNumberContextMenu");
			}
			else if (overText && textContextMenuCallback)
			{
				contextMenuLine = glyphCoordinate.line;
				contextMenuColumn = glyphCoordinate.column;
				ImGui::OpenPopup("TextContextMenu");
			}
		}
		// --- Left click: folding, selection, multi-cursor ----------------
		else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			auto click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
			auto doubleClick = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
			auto now = static_cast<float>(ImGui::GetTime());
			auto tripleClick = click && !doubleClick &&
				(lastClickTime != -1.0f && (now - lastClickTime) < io.MouseDoubleClickTime);

			if (click || doubleClick || tripleClick)
			{
				lastClickTime = tripleClick ? -1.0f : now;
			}

			// Remember whether this click started a text / gutter selection
			// drag. Drags that originated on the h-scrollbar etc. won't pass
			// this gate and won't extend the selection.
			selectingText = overText || overLineNumbers;


			// 2) Triple click: select line
			if (tripleClick)
			{
				if (overText)
				{
					auto start = document.getStartOfLine(cursorCoordinate);
					auto end = document.getDown(start);
					cursors.updateCurrentCursor(start, end);
				}
			}
			// 3) Double click: word / bracket selection
			if ((doubleClick || tripleClick))
			{


				if (overText || overDecorator)
				{
					auto codepoint = document.getCodePoint(glyphCoordinate);
					bool handled = false;

					if (CodePoint::isBracketOpener(codepoint))
					{
						auto brackets = bracketeer.getEnclosingBrackets(document.getRight(glyphCoordinate));

						if (brackets != bracketeer.end())
						{
							if (ImGui::IsKeyDown(ImGuiMod_Shift))
							{
								cursors.setCursor(brackets->start, document.getRight(brackets->end));
							}
							else
							{
								cursors.setCursor(document.getRight(brackets->start), brackets->end);
							}

							handled = true;
						}
					}
					else if (CodePoint::isBracketCloser(codepoint))
					{
						auto brackets = bracketeer.getEnclosingBrackets(glyphCoordinate);

						if (brackets != bracketeer.end())
						{
							cursors.setCursor(brackets->start, document.getRight(brackets->end));
							handled = true;
						}
					}

					if (!handled && !document.isEndOfLine(glyphCoordinate))
					{
						auto start = document.findWordStart(glyphCoordinate);
						auto end = document.findWordEnd(glyphCoordinate);
						cursors.updateCurrentCursor(start, end);
					}
				}
			}
			// 4) Single click: line numbers / text / multi-cursor
			else if (click)
			{
				auto extendCursor =
					ImGui::IsKeyDown(ImGuiMod_Shift);

#if __APPLE__
				auto addCursor = ImGui::IsKeyDown(ImGuiMod_Alt);
#else
				auto addCursor = ImGui::IsKeyDown(ImGuiMod_Ctrl);
#endif

				if (overLineNumbers)
				{
					auto start = Coordinate(cursorCoordinate.line, 0);
					auto end = document.getDown(start);

					if (extendCursor)
					{
						auto& cursor = cursors.getCurrent();
						cursor.update(cursor.getInteractiveEnd() < cursor.getInteractiveStart() ? start : end);
						autocomplete.cancel();
					}
					else if (addCursor)
					{
						cursors.addCursor(start, end);
						autocomplete.cancel();
					}
					else
					{
						cursors.setCursor(start, end);
					}

					makeCursorVisible();
				}
				else if (overText)
				{
#if !__APPLE__
					// Alt+click (without Ctrl) opens a column / box selection.
					// On macOS Alt is already used for multi-cursor add, so we
					// keep that path and rely on Alt+drag without click — users
					// can still column-select by holding Alt before drag.
					bool startColumn = ImGui::IsKeyDown(ImGuiMod_Alt) &&
						!ImGui::IsKeyDown(ImGuiMod_Ctrl);
#else
					bool startColumn = false;
#endif

					if (startColumn)
					{
						columnSelecting = true;
						columnAnchor = cursorCoordinate;
						cursors.setCursor(cursorCoordinate);
						autocomplete.cancel();
					}
					else if (extendCursor)
					{
						cursors.updateCurrentCursor(cursorCoordinate);
						autocomplete.cancel();
					}
					else if (addCursor)
					{
						cursors.addCursor(cursorCoordinate);
						autocomplete.cancel();
					}
					else
					{
						// If the click landed inside an existing selection,
						// don't move the cursor yet — wait for either a drag
						// (the selection drag-and-drop kicks in) or a release
						// without movement (then we commit a plain click).
						auto& cur = cursors.getCurrent();
						bool insideSel = false;
						if (cur.hasSelection())
						{
							auto s = cur.getSelectionStart();
							auto e = cur.getSelectionEnd();
							auto le = [](Coordinate a, Coordinate b)
								{
									if (a.line != b.line) return a.line < b.line;
									return a.column <= b.column;
								};
							insideSel = le(s, cursorCoordinate) && le(cursorCoordinate, e);
						}
						if (insideSel)
						{
							pendingClickInSelection = true;
							pendingClickCoord = cursorCoordinate;
						}
						else
						{
							cursors.setCursor(cursorCoordinate);
						}
					}

					makeCursorVisible();
				}
			}
		}
	}
}


//
//	TextEditor::selectAll
//

void TextEditor::selectAll()
{
	moveToTop(false);
	moveToBottom(true);
}


//
//	TextEditor::selectLine
//

void TextEditor::selectLine(int line)
{
	// Select the whole line's content (column 0 → end of line). Previously this
	// stopped one column short (maxColumn - 1), leaving the last glyph
	// unselected and the caret one column before the line end.
	Coordinate start{ line, 0 };
	moveTo(start, false);
	moveTo(Coordinate(start.line, document[line].maxColumn), true);
}


//
//	TextEditor::selectCursorLines
//

void TextEditor::selectCursorLines()
{
	// Ctrl+L: expand EVERY cursor's selection to cover its whole line(s) — full
	// lines including the trailing newline (anchor at column 0, active at the next
	// line's start) so a repeat press extends each selection down one more line,
	// VSCode-style. Unlike selectLine(int) this preserves all cursors instead of
	// collapsing to one.
	int last = document.lineCount() - 1;
	for (auto& cursor : cursors)
	{
		int startLine = cursor.getSelectionStart().line;
		int endLineExclusive = cursor.getSelectionEnd().line + 1;
		Coordinate ns(startLine, 0);
		Coordinate ne = (endLineExclusive > last)
			? Coordinate(last, document[last].maxColumn)   // last line: no phantom trailing line
			: Coordinate(endLineExclusive, 0);
		cursor.update(ns, ne);
	}
	cursors.update();        // merge cursors that now overlap (e.g. two on one line)
	makeCursorVisible();
}


void TextEditor::selectWord(int line, int column)
{
	Coordinate start{ line, column };
	moveTo(document.findWordStart(start, true), false);
	moveTo(document.findWordEnd(start), true);
}

//
//	TextEditor::selectLines
//

void TextEditor::selectLines(int startLine, int endLine)
{
	Coordinate start{ startLine, 0 };
	moveTo(start, false);
	moveTo(document.getDown(start, endLine - startLine + 1), true);
}


//
//	TextEditor::selectRegion
//

void TextEditor::selectRegion(int startLine, int startColumn, int endLine, int endColumn)
{
	auto start = document.normalizeCoordinate(Coordinate(startLine, startColumn));
	auto end = document.normalizeCoordinate(Coordinate(endLine, endColumn));

	if (end < start)
	{
		std::swap(start, end);
	}

	cursors.setCursor(start, end);
}


//
//	TextEditor::selectToBrackets
//

void TextEditor::selectToBrackets(bool includeBrackets)
{
	if (!showMatchingBrackets)
	{
		bracketeer.update(document);
	}

	for (auto& cursor : cursors)
	{
		auto bracket = bracketeer.getEnclosingBrackets(cursor.getSelectionStart());

		if (bracket != bracketeer.end())
		{
			if (includeBrackets)
			{
				cursor.update(bracket->start, document.getRight(bracket->end));

			}
			else
			{
				cursor.update(document.getRight(bracket->start), bracket->end);
			}
		}
	}
}


//
//	TextEditor::growSelectionsToCurlyBrackets
//

void TextEditor::growSelectionsToCurlyBrackets()
{
	if (!showMatchingBrackets)
	{
		bracketeer.update(document);
	}

	for (auto& cursor : cursors)
	{
		auto start = cursor.getSelectionStart();
		auto end = cursor.getSelectionEnd();
		auto startCodePoint = document.getCodePoint(document.getLeft(start));
		auto endCodePoint = document.getCodePoint(end);

		if (startCodePoint == CodePoint::openCurlyBracket && endCodePoint == CodePoint::closeCurlyBracket)
		{
			cursor.update(document.getLeft(start), document.getRight(end));

		}
		else
		{
			auto bracket = bracketeer.getEnclosingCurlyBrackets(start, end);

			if (bracket != bracketeer.end())
			{
				cursor.update(document.getRight(bracket->start), bracket->end);
			}
		}
	}
}


//
//	TextEditor::shrinkSelectionsToCurlyBrackets
//

void TextEditor::shrinkSelectionsToCurlyBrackets()
{
	if (!showMatchingBrackets)
	{
		bracketeer.update(document);
	}

	for (auto& cursor : cursors)
	{
		if (cursor.hasSelection())
		{
			auto start = cursor.getSelectionStart();
			auto end = cursor.getSelectionEnd();
			auto startCodePoint = document.getCodePoint(start);
			auto endCodePoint = document.getCodePoint(document.getLeft(end));

			if (startCodePoint == CodePoint::openCurlyBracket && endCodePoint == CodePoint::closeCurlyBracket)
			{
				cursor.update(document.getRight(start), document.getLeft(end));

			}
			else
			{
				auto bracket = bracketeer.getInnerCurlyBrackets(start, end);

				if (bracket != bracketeer.end())
				{
					cursor.update(bracket->start, document.getRight(bracket->end));
				}
			}
		}
	}
}


//
//	TextEditor::cut
//

void TextEditor::cut()
{
	// copy selections to clipboard and remove them
	copy();
	auto transaction = startTransaction();
	deleteTextFromAllCursors(transaction);
	cursors.getCurrent().resetToStart();
	endTransaction(transaction);
}


//
//	TextEditor::copy
//

void TextEditor::copy() const
{
	// copy all selections and put them on the clipboard
	// empty cursors copy the entire line
	std::string text;

	if (cursors.anyHasSelection())
	{
		for (auto& cursor : cursors)
		{
			if (text.size())
			{
				text += "\n";
			}

			if (cursor.hasSelection())
			{
				text += document.getSectionText(cursor.getSelectionStart(), cursor.getSelectionEnd());

			}
			else
			{
				text += document.getLineText(cursor.getSelectionStart().line);
			}
		}

	}
	else
	{
		for (auto& cursor : cursors)
		{
			text += document.getLineText(cursor.getSelectionStart().line) + "\n";
		}
	}

	ImGui::SetClipboardText(text.c_str());
}


//
//	TextEditor::paste
//

void TextEditor::paste()
{
	// ignore non-text clipboard content
	auto clipboard = ImGui::GetClipboardText();

	if (clipboard)
	{
		auto transaction = startTransaction();
		insertTextIntoAllCursors(transaction, clipboard);
		endTransaction(transaction);
	}
}


//
//	TextEditor::undo
//

void TextEditor::undo()
{
	if (transactions.canUndo())
	{
		transactions.undo(document, cursors);
		makeCursorVisible();
	}
}


//
//	TextEditor::redo
//

void TextEditor::redo()
{
	if (transactions.canRedo())
	{
		transactions.redo(document, cursors);
		makeCursorVisible();
	}
}


//
//	TextEditor::getCursor
//

void TextEditor::getCursor(int& line, int& column, size_t cursor) const
{
	cursor = std::min(cursor, cursors.size() - 1);
	auto pos = cursors[cursor].getInteractiveEnd();
	line = pos.line;
	column = pos.column;
}


//
//	TextEditor::getCursor
//

void TextEditor::getCursor(int& startLine, int& startColumn, int& endLine, int& endColumn, size_t cursor) const
{
	cursor = std::min(cursor, cursors.size() - 1);
	auto start = cursors[cursor].getSelectionStart();
	auto end = cursors[cursor].getSelectionEnd();
	startLine = start.line;
	startColumn = start.column;
	endLine = end.line;
	endColumn = end.column;
}


//
//	TextEditor::getCursorText
//

std::string TextEditor::getCursorText(size_t cursor) const
{
	cursor = std::min(cursor, cursors.size() - 1);
	return document.getSectionText(cursors[cursor].getSelectionStart(), cursors[cursor].getSelectionEnd());
}


//
//	TextEditor::GetWordAtScreenPos
//

std::string TextEditor::GetWordAtScreenPos(const ImVec2& screenPos) const
{
	// convert screen position to local coordinates using the origin saved during last Render()
	auto local = screenPos - lastRenderOrigin;

	// convert to text coordinates
	Coordinate glyphCoordinate;
	Coordinate cursorCoordinate;
	if (wordWrap && !wrapRows.empty())
	{
		int row = std::clamp(static_cast<int>(local.y / glyphSize.y), 0,
			static_cast<int>(wrapRows.size()) - 1);
		int column = wrapRows[row].startColumn +
			std::max(0, static_cast<int>((local.x - textOffset) / glyphSize.x));
		document.normalizeCoordinate(1.0f * wrapRows[row].line, 1.0f * column,
			glyphCoordinate, cursorCoordinate);
	}
	else
	{
		document.normalizeCoordinate(local.y / glyphSize.y, (local.x - textOffset) / glyphSize.x, glyphCoordinate, cursorCoordinate);
	}

	// Find word boundaries and extract text
	auto start = document.findWordStart(glyphCoordinate);
	auto end = document.findWordEnd(glyphCoordinate);

	return document.getSectionText(start, end);
}

bool TextEditor::GetBytePosAtScreenPos(const ImVec2& screenPos, int& line, int& byteOffset) const
{
	auto local = screenPos - lastRenderOrigin;
	Coordinate g, c;
	if (wordWrap && !wrapRows.empty())
	{
		int row = std::clamp(static_cast<int>(local.y / glyphSize.y), 0, static_cast<int>(wrapRows.size()) - 1);
		int column = wrapRows[row].startColumn + std::max(0, static_cast<int>((local.x - textOffset) / glyphSize.x));
		document.normalizeCoordinate(1.0f * wrapRows[row].line, 1.0f * column, g, c);
	}
	else
	{
		document.normalizeCoordinate(local.y / glyphSize.y, (local.x - textOffset) / glyphSize.x, g, c);
	}
	line = g.line;
	byteOffset = 0;
	if (line < 0 || line >= document.lineCount()) return false;
	size_t cp = document.getIndex(document[line], g.column);   // visible column -> codepoint
	std::string ln = document.getLineText(line);
	size_t b = 0;
	for (size_t i = 0; i < cp && b < ln.size(); ++i)
	{
		++b;
		while (b < ln.size() && (((unsigned char) ln[b]) & 0xC0) == 0x80) ++b;
	}
	byteOffset = static_cast<int>(b);
	return true;
}


//
//	TextEditor::makeCursorVisible
//

void TextEditor::makeCursorVisible()
{
	ensureCursorIsVisible = true;
	scrollToLineNumber = -1;
}


//
//	TextEditor::scrollToLine
//

void TextEditor::scrollToLine(int line, Scroll alignment)
{
	ensureCursorIsVisible = false;
	scrollToLineNumber = line;
	scrollToAlignment = alignment;
}


//
//	TextEditor::addMarker
//

void TextEditor::addMarker(int line, ImU32 lineNumberColor, ImU32 textColor, const std::string_view& lineNumberTooltip, const std::string_view& textTooltip)
{
	if (line >= 0 && line < document.lineCount())
	{
		markers.emplace_back(lineNumberColor, textColor, lineNumberTooltip, textTooltip);
		document[line].marker = markers.size();
	}
}


//
//	TextEditor::clearMarkers
//

void TextEditor::clearMarkers()
{
	for (auto& line : document)
	{
		line.marker = 0;
	}

	markers.clear();
}


//
//	TextEditor::MarkChangedLines
//
//	Diff `previousText` against the current document text and add a purple change
//	marker on every differing line, returning those lines as coalesced inclusive
//	0-based ranges. An equal prefix/suffix trim bounds the LCS so a localized edit
//	in a huge file is cheap; a still-huge differing window marks wholesale.

std::vector<std::pair<int, int>> TextEditor::MarkChangedLines(const std::string& previousText)
{
	clearMarkers();
	std::vector<std::pair<int, int>> ranges;

	auto split = [](const std::string& s) {
		std::vector<std::string> v;
		std::string cur;
		for (char c : s)
		{
			if (c == '\r')
				continue;
			if (c == '\n')
			{
				v.push_back(cur);
				cur.clear();
			}
			else
				cur += c;
		}
		v.push_back(cur);
		return v;
	};
	std::vector<std::string> a = split(previousText), b = split(document.getText());
	size_t N = a.size(), M = b.size();
	if (M == 0)
		return ranges;

	// Trim the equal prefix / suffix — external edits are usually localized.
	size_t p = 0;
	while (p < N && p < M && a[p] == b[p])
		++p;
	size_t ea = N, eb = M;
	while (ea > p && eb > p && a[ea - 1] == b[eb - 1])
	{
		--ea;
		--eb;
	}

	size_t n = ea - p, m = eb - p; // sizes of the differing windows
	if (m == 0)
		return ranges; // pure deletion — nothing to mark in the current text

	const ImU32 gutter = IM_COL32(170, 130, 250, 255);
	const ImU32 bg = IM_COL32(120, 90, 200, 38);
	std::vector<int> changed; // 0-based lines marked (ascending)

	if (n > 3000 || m > 3000)
	{
		// window still huge → mark the whole window
		for (size_t k = p; k < eb; ++k)
		{
			addMarker((int)k, gutter, bg, "Changed externally", "Changed on disk by an external tool");
			changed.push_back((int)k);
		}
	}
	else
	{
		// LCS over the differing window only.
		std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
		for (size_t i = n; i-- > 0;)
			for (size_t j = m; j-- > 0;)
				dp[i][j] = (a[p + i] == b[p + j]) ? dp[i + 1][j + 1] + 1
												  : (std::max)(dp[i + 1][j], dp[i][j + 1]);

		std::vector<char> common(m, 0);
		for (size_t i = 0, j = 0; i < n && j < m;)
		{
			if (a[p + i] == b[p + j])
			{
				common[j] = 1;
				++i;
				++j;
			}
			else if (dp[i + 1][j] >= dp[i][j + 1])
				++i;
			else
				++j;
		}

		for (size_t k = 0; k < m; ++k)
		{
			if (!common[k])
			{
				addMarker((int)(p + k), gutter, bg, "Changed externally", "Changed on disk by an external tool");
				changed.push_back((int)(p + k));
			}
		}
	}

	// Coalesce consecutive changed lines into inclusive ranges.
	for (int ln : changed)
	{
		if (!ranges.empty() && ln == ranges.back().second + 1)
			ranges.back().second = ln;
		else
			ranges.emplace_back(ln, ln);
	}
	return ranges;
}


//
//	TextEditor::Merge3  (three-way line merge / diff3)
//

static std::vector<std::string> mergeSplitLines(const std::string& s)
{
	std::vector<std::string> v;
	std::string cur;
	for (char c : s)
	{
		if (c == '\r')
			continue;
		if (c == '\n')
		{
			v.push_back(cur);
			cur.clear();
		}
		else
			cur += c;
	}
	v.push_back(cur);
	return v;
}

static std::string mergeJoinLines(const std::vector<std::string>& v)
{
	std::string r;
	for (size_t k = 0; k < v.size(); ++k)
	{
		r += v[k];
		if (k + 1 < v.size())
			r += '\n';
	}
	return r;
}

// LCS line-match pairs (baseIdx, otherIdx), increasing in both.
static std::vector<std::pair<int, int>> mergeLcs(const std::vector<std::string>& O, const std::vector<std::string>& X)
{
	int n = (int)O.size(), m = (int)X.size();
	std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
	for (int i = n; i-- > 0;)
		for (int j = m; j-- > 0;)
			dp[i][j] = (O[i] == X[j]) ? dp[i + 1][j + 1] + 1 : (std::max)(dp[i + 1][j], dp[i][j + 1]);
	std::vector<std::pair<int, int>> pairs;
	for (int i = 0, j = 0; i < n && j < m;)
	{
		if (O[i] == X[j])
		{
			pairs.push_back({i, j});
			++i;
			++j;
		}
		else if (dp[i + 1][j] >= dp[i][j + 1])
			++i;
		else
			++j;
	}
	return pairs;
}

// Coarse fallback for very large files: 2-way prefix/suffix trim of mine vs
// theirs, wrapping the differing window in conflict markers.
static std::string mergeCoarse(const std::vector<std::string>& A, const std::vector<std::string>& B, bool& conflict)
{
	size_t n = A.size(), m = B.size();
	size_t p = 0;
	while (p < n && p < m && A[p] == B[p])
		++p;
	size_t ea = n, eb = m;
	while (ea > p && eb > p && A[ea - 1] == B[eb - 1])
	{
		--ea;
		--eb;
	}
	std::vector<std::string> out;
	for (size_t k = 0; k < p; ++k)
		out.push_back(A[k]);
	if (ea > p || eb > p)
	{
		conflict = true;
		out.push_back("<<<<<<< mine (your unsaved edits)");
		for (size_t k = p; k < ea; ++k)
			out.push_back(A[k]);
		out.push_back("=======");
		for (size_t k = p; k < eb; ++k)
			out.push_back(B[k]);
		out.push_back(">>>>>>> external (on disk)");
	}
	for (size_t k = ea; k < n; ++k)
		out.push_back(A[k]);
	return mergeJoinLines(out);
}

// diff3: O=base, A=mine, B=theirs. Auto-merges non-overlapping changes; wraps
// overlapping (both-changed) regions in git-style conflict markers.
std::string TextEditor::Merge3(const std::string& baseS, const std::string& mineS,
							   const std::string& theirsS, bool& conflict)
{
	conflict = false;
	auto O = mergeSplitLines(baseS), A = mergeSplitLines(mineS), B = mergeSplitLines(theirsS);
	if (O.size() > 4000 || A.size() > 4000 || B.size() > 4000)
		return mergeCoarse(A, B, conflict);

	auto ma = mergeLcs(O, A);
	auto mb = mergeLcs(O, B);
	std::unordered_map<int, int> oa, ob;
	for (auto& pr : ma)
		oa[pr.first] = pr.second;
	for (auto& pr : mb)
		ob[pr.first] = pr.second;
	std::vector<int> sync; // base lines common to BOTH sides (sync points)
	for (auto& pr : ma)
		if (ob.count(pr.first))
			sync.push_back(pr.first);

	std::vector<std::string> out;
	auto pushRange = [&](const std::vector<std::string>& X, int lo, int hi) { for (int k = lo; k < hi; ++k) out.push_back(X[k]); };
	auto sameAsBase = [&](const std::vector<std::string>& X, int xLo, int xHi, int oLo, int oHi) {
		if (xHi - xLo != oHi - oLo)
			return false;
		for (int k = 0; k < xHi - xLo; ++k)
			if (X[xLo + k] != O[oLo + k])
				return false;
		return true;
	};
	auto emitChunk = [&](int oLo, int oHi, int aLo, int aHi, int bLo, int bHi) {
		bool aSame = sameAsBase(A, aLo, aHi, oLo, oHi);
		bool bSame = sameAsBase(B, bLo, bHi, oLo, oHi);
		if (aSame && bSame)
		{
			pushRange(O, oLo, oHi);
			return;
		}
		if (aSame)
		{
			pushRange(B, bLo, bHi);
			return;
		} // only theirs changed
		if (bSame)
		{
			pushRange(A, aLo, aHi);
			return;
		} // only mine changed
		// both changed — identical change? take one. else conflict.
		bool abEqual = (aHi - aLo == bHi - bLo);
		if (abEqual)
			for (int k = 0; k < aHi - aLo; ++k)
				if (A[aLo + k] != B[bLo + k])
				{
					abEqual = false;
					break;
				}
		if (abEqual)
		{
			pushRange(A, aLo, aHi);
			return;
		}
		conflict = true;
		out.push_back("<<<<<<< mine (your unsaved edits)");
		pushRange(A, aLo, aHi);
		out.push_back("=======");
		pushRange(B, bLo, bHi);
		out.push_back(">>>>>>> external (on disk)");
	};

	int prevO = -1, prevA = -1, prevB = -1;
	for (int s : sync)
	{
		emitChunk(prevO + 1, s, prevA + 1, oa[s], prevB + 1, ob[s]);
		out.push_back(O[s]); // the agreed-upon sync line
		prevO = s;
		prevA = oa[s];
		prevB = ob[s];
	}
	emitChunk(prevO + 1, (int)O.size(), prevA + 1, (int)A.size(), prevB + 1, (int)B.size());
	return mergeJoinLines(out);
}


//
//	TextEditor::moveUp
//

void TextEditor::moveUp(int lines, bool select)
{
	// Word wrap: move by wrapped row, preserving the column offset within the row.
	if (wordWrap && !wrapRows.empty())
	{
		for (auto& cursor : cursors)
		{
			Coordinate cur = cursor.getInteractiveEnd();
			int r = wrapRowOfCoordinate(cur);
			int colOff = cur.column - wrapRows[r].startColumn;
			int tr = std::max(0, r - lines);
			int tcol = std::min(wrapRows[tr].startColumn + colOff, wrapRows[tr].endColumn);
			cursor.update(document.normalizeCoordinate(Coordinate(wrapRows[tr].line, tcol)), select);
		}
		makeCursorVisible();
		return;
	}
	for (auto& cursor : cursors)
	{
		auto pos = document.getUp(cursor.getInteractiveEnd(), lines);
		// skip any folded (invisible) lines
		if (foldRanges.foldingEnabled)
		{
			int guard = document.lineCount();
			while (pos.line > 0 && !document[pos.line].visible && --guard > 0)
				pos = document.getUp(pos);
		}
		cursor.update(pos, select);
	}
	makeCursorVisible();
}


//
//	TextEditor::moveDown
//

void TextEditor::moveDown(int lines, bool select)
{
	if (wordWrap && !wrapRows.empty())
	{
		int maxRow = static_cast<int>(wrapRows.size()) - 1;
		for (auto& cursor : cursors)
		{
			Coordinate cur = cursor.getInteractiveEnd();
			int r = wrapRowOfCoordinate(cur);
			int colOff = cur.column - wrapRows[r].startColumn;
			int tr = std::min(maxRow, r + lines);
			int tcol = std::min(wrapRows[tr].startColumn + colOff, wrapRows[tr].endColumn);
			cursor.update(document.normalizeCoordinate(Coordinate(wrapRows[tr].line, tcol)), select);
		}
		makeCursorVisible();
		return;
	}
	for (auto& cursor : cursors)
	{
		auto pos = document.getDown(cursor.getInteractiveEnd(), lines);
		// skip any folded (invisible) lines
		if (foldRanges.foldingEnabled)
		{
			int guard = document.lineCount();
			while (pos.line < document.lineCount() - 1 && !document[pos.line].visible && --guard > 0)
				pos = document.getDown(pos);
		}
		cursor.update(pos, select);
	}
	makeCursorVisible();
}


//
//	TextEditor::moveLeft
//

void TextEditor::moveLeft(bool select, bool wordMode, bool subWord)
{
	for (auto& cursor : cursors)
	{
		if (cursor.hasSelection() && !select && !wordMode && !subWord)
		{
			cursor.resetToStart();
		}
		else if (subWord)
		{
			cursor.update(document.findSubWordLeft(cursor.getInteractiveEnd()), select);
		}
		else
		{
			cursor.update(document.getLeft(cursor.getInteractiveEnd(), wordMode), select);
		}
	}

	makeCursorVisible();
}


//
//	TextEditor::moveRight
//

void TextEditor::moveRight(bool select, bool wordMode, bool subWord)
{
	for (auto& cursor : cursors)
	{
		if (cursor.hasSelection() && !select && !wordMode && !subWord)
		{
			cursor.resetToEnd();
		}
		else if (subWord)
		{
			cursor.update(document.findSubWordRight(cursor.getInteractiveEnd()), select);
		}
		else
		{
			cursor.update(document.getRight(cursor.getInteractiveEnd(), wordMode), select);
		}
	}

	makeCursorVisible();
}


//
//	TextEditor::moveToTop
//

void TextEditor::moveToTop(bool select)
{
	auto& cursor = cursors.getCurrent();
	int firstVisible = 0;
	for (int i = 0; i < document.lineCount(); ++i)
	{
		if (document[i].visible) { firstVisible = i; break; }
	}
	moveTo(Coordinate(firstVisible, cursor.getInteractiveStart().column), select);
}


//
//	TextEditor::moveToBottom
//

void TextEditor::moveToBottom(bool select)
{
	int lastVisible = std::max(document.lineCount() - 1, 0);
	for (int i = document.lineCount() - 1; i >= 0; --i)
	{
		if (document[i].visible) { lastVisible = i; break; }
	}
	int col = (lastVisible < document.lineCount()) ? document[lastVisible].maxColumn : 0;
	moveTo(Coordinate(lastVisible, col), select);
}


//
//	TextEditor::moveToStartOfLine
//

void TextEditor::moveToStartOfLine(bool select)
{
	auto& cursor = cursors.getCurrent();
	moveTo(Coordinate(cursor.getInteractiveEnd().line, 0), select);
}


//
//	TextEditor::moveToEndOfLine
//

void TextEditor::moveToEndOfLine(bool select)
{
	auto& cursor = cursors.getCurrent();
	int line = cursor.getInteractiveEnd().line;
	int col = document[line].maxColumn;
	moveTo(Coordinate(line, col), select);
}


//
//	TextEditor::moveTo
//

void TextEditor::moveTo(Coordinate coordinate, bool select)
{
	cursors.clearAdditional();
	cursors.updateCurrentCursor(coordinate, select);
	makeCursorVisible();
}


//
//	TextEditor::handleCharacter
//

void TextEditor::handleCharacter(ImWchar character)
{
	auto transaction = startTransaction(false);

	auto opener = character;
	auto isPaired = !overwrite && completePairedGlyphs && CodePoint::isPairOpener(opener);
	auto closer = CodePoint::toPairCloser(opener);

	// ignore input if it was the closing character for a pair that was automatically inserted
	if (completePairCloser)
	{
		if (completePairCloser == character && completePairLocation == cursors.getCurrent().getSelectionEnd())
		{
			completePairCloser = 0;
			moveRight(false, false);
			return;
		}

		completePairCloser = 0;
	}

	if (cursors.anyHasSelection() && isPaired)
	{
		// encapsulate the current selections with the requested pairs
		for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
		{
			if (cursor->hasSelection())
			{
				auto start = cursor->getSelectionStart();
				auto end = cursor->getSelectionEnd();

				// insert the closing glyph
				char utf8[4];
				auto end1 = insertText(transaction, end, std::string_view(utf8, CodePoint::write(utf8, closer)));
				cursors.adjustForInsert(cursor, start, end1);

				// insert the opening glyph
				auto end2 = insertText(transaction, start, std::string_view(utf8, CodePoint::write(utf8, opener)));
				cursors.adjustForInsert(cursor, start, end2);

				// update old selection
				cursor->update(Coordinate(start.line, start.column + 1), Coordinate(end.line, end.column + 1));
			}
		}

	}
	else if (isPaired)
	{
		// insert the requested pair
		char utf8[8];
		auto size = CodePoint::write(utf8, opener);
		size += CodePoint::write(utf8 + size, closer);
		std::string_view pair(utf8, size);

		for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
		{
			auto start = cursor->getSelectionStart();
			auto end = insertText(transaction, start, pair);
			cursors.adjustForInsert(cursor, start, end);
			cursor->update(Coordinate(start.line, start.column + 1), false);
		}

		// remember the closer
		completePairCloser = closer;
		completePairLocation = cursors.getCurrent().getSelectionEnd();

	}
	else if (!overwrite && autoIndent && character == '\n')
	{
		// handle auto indent case
		autoIndentAllCursors(transaction);

	}
	else
	{
		// handle overwrite by deleting next glyph before insert
		if (overwrite)
		{
			for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
			{
				if (!cursor->hasSelection())
				{
					auto start = cursor->getSelectionStart();

					if (start != document.getEndOfLine(start))
					{
						auto end = document.getRight(start);
						deleteText(transaction, start, end);
						cursors.adjustForDelete(cursor, start, end);
					}
				}
			}
		}

		// just insert a regular character
		char utf8[4];
		insertTextIntoAllCursors(transaction, std::string_view(utf8, CodePoint::write(utf8, character)));
	}

	endTransaction(transaction);

	// Trigger autocomplete on word chars (identifier completion) AND on member-
	// access operators '.', '->', '::' (member completion — the app callback
	// resolves the receiver's type and lists its members). For '>' / ':' check
	// the preceding char so a stray comparison / label / ternary doesn't pop it.
	bool memberOp = (character == '.');
	if (!memberOp && (character == '>' || character == ':'))
	{
		auto pe = cursors.getMain().getSelectionEnd();
		if (pe.column >= 2)
		{
			std::string before = document.getSectionText(
				Coordinate(pe.line, pe.column - 2), Coordinate(pe.line, pe.column - 1));
			memberOp = (character == '>' && before == "-") || (character == ':' && before == ":");
		}
	}
	if (CodePoint::isWord(character) || memberOp)
	{
		if (autocomplete.startTyping(cursors))
		{
			makeCursorVisible();
		}
	}
}


//
//	TextEditor::handleBackspace
//

void TextEditor::handleBackspace(bool wordMode)
{
	auto transaction = startTransaction(false);

	// remove selections or characters to the left of the cursor
	for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
	{
		auto start = cursor->hasSelection() ? cursor->getSelectionStart() : document.getLeft(cursor->getSelectionStart(), wordMode);
		auto end = cursor->getSelectionEnd();
		deleteText(transaction, start, end);
		cursor->update(start, false);
		cursors.adjustForDelete(cursor, start, end);
	}

	endTransaction(transaction);
}


//
//	TextEditor::handleDelete
//

void TextEditor::handleDelete(bool wordMode)
{
	auto transaction = startTransaction(false);

	// remove selections or characters to the right of the cursor
	for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
	{
		auto start = cursor->getSelectionStart();
		auto end = cursor->hasSelection() ? cursor->getSelectionEnd() : document.getRight(cursor->getSelectionEnd(), wordMode);
		deleteText(transaction, start, end);
		cursor->update(start, false);
		cursors.adjustForDelete(cursor, start, end);
	}

	endTransaction(transaction);
}


//
//	TextEditor::removeSelectedLines
//

void TextEditor::removeSelectedLines()
{
	auto transaction = startTransaction();

	for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
	{
		auto start = document.getStartOfLine(cursor->getSelectionStart());
		auto end = cursor->getSelectionEnd();
		end = (end.column == 0) ? end : document.getNextLine(end);
		deleteText(transaction, start, end);
		cursor->update(start, false);
		cursors.adjustForDelete(cursor, start, end);
	}

	endTransaction(transaction);
}


//
//	TextEditor::insertLineAbove
//

void TextEditor::insertLineAbove()
{
	auto transaction = startTransaction();

	for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
	{
		auto start = document.getStartOfLine(cursor->getSelectionStart());
		auto end = insertText(transaction, start, "\n");
		cursor->update(start, false);
		cursors.adjustForInsert(cursor, start, end);
	}

	endTransaction(transaction);
}


//
//	TextEditor::insertLineBelow
//

void TextEditor::insertLineBelow()
{
	auto transaction = startTransaction();

	for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
	{
		auto start = cursor->getSelectionEnd();
		start = (start.column == 0) ? start : document.getNextLine(start);
		auto end = insertText(transaction, start, "\n");
		cursor->update(start, false);
		cursors.adjustForInsert(cursor, start, end);
	}

	endTransaction(transaction);
}


//
//	TextEditor::indentLines
//

void TextEditor::indentLines()
{
	auto transaction = startTransaction();

	// Multi-cursor (e.g. column / box selection): insert a tab at the LEFT
	// edge of each cursor's selection rather than at column 0 of the line.
	// That matches VSCode/Sublime column-mode behaviour and lets the user
	// indent only the boxed region instead of nuking the whole line's indent.
	if (cursors.hasMultiple())
	{
		for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
		{
			auto pos = cursor->hasSelection()
				? cursor->getSelectionStart()
				: cursor->getInteractiveEnd();
			auto insertEnd = insertText(transaction, pos, "\t");
			cursors.adjustForInsert(cursor, pos, insertEnd);
			cursor->adjustForInsert(pos, insertEnd);
		}
		endTransaction(transaction);
		return;
	}

	// Single cursor — existing line-indent behaviour: tab at column 0 of every
	// line touched by the selection (skipping a trailing line if the selection
	// ended at column 0 of it).
	for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
	{
		auto cursorStart = cursor->getSelectionStart();
		auto cursorEnd = cursor->getSelectionEnd();

		// process all lines in this cursor
		for (auto line = cursorStart.line; line <= cursorEnd.line; line++)
		{
			if (Coordinate(line, 0) != cursorEnd && document[line].size())
			{
				auto insertStart = Coordinate(line, 0);
				auto insertEnd = insertText(transaction, insertStart, "\t");
				cursors.adjustForInsert(cursor, insertStart, insertEnd);
			}
		}

		auto tabSize = document.getTabSize();
		cursorStart.column += cursorStart.column ? tabSize : 0;
		cursorEnd.column += cursorEnd.column ? tabSize : 0;
		cursor->update(cursorStart, cursorEnd);
	}

	endTransaction(transaction);
}


//
//	TextEditor::deindentLines
//

void TextEditor::deindentLines()
{
	auto transaction = startTransaction();

	// process all cursors
	for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
	{

		auto cursorStart = cursor->getInteractiveStart();
		auto cursorEnd = cursor->getInteractiveEnd();
		auto tabSize = document.getTabSize();



		if (cursor->hasSelection())
		{
			cursorStart = cursor->getSelectionStart();
			cursorEnd = cursor->getSelectionEnd();
			if (cursorEnd < cursorStart)
				std::swap(cursorStart, cursorEnd);
		}
		else
		{
			auto pos = cursor->getInteractiveEnd();
			cursorStart = Coordinate(pos.line, 0);
			cursorEnd = Coordinate(pos.line, document[pos.line].maxColumn);
		}


		// If a multi-line selection ends exactly at column 0 of `cursorEnd.line`,
		// the user never actually selected anything on that line — don't deindent it.
		int lastLine = cursorEnd.line;
		if (cursorStart.line != cursorEnd.line && cursorEnd.column == 0)
		{
			lastLine = cursorEnd.line - 1;
		}
		for (auto line = cursorStart.line; line <= lastLine; line++)
		{
			// determine how many whitespaces are available at the start with a max of 4 columns
			int column = 0;
			size_t index = 0;

			while (column < 4 && index < document[line].size() && std::isblank(document[line][index].codepoint))
			{
				column += document[line][index].codepoint == '\t' ? tabSize - (column % tabSize) : 1;
				index++;
			}

			// delete that whitespace (if required)
			Coordinate deleteStart{ line, 0 };
			Coordinate deleteEnd{ line, document.getColumn(line, index) };

			if (deleteEnd != deleteStart)
			{
				deleteText(transaction, deleteStart, deleteEnd);
				cursors.adjustForDelete(cursor, deleteStart, deleteEnd);
				// adjustForDelete above only shifts *other* cursors; manually
				// shift the cursor we're processing so Tab → Shift-Tab is a
				// no-op for both selections and bare carets.
				cursor->adjustForDelete(deleteStart, deleteEnd);
			}
		}
	}

	endTransaction(transaction);
}


//
//	Widget::moveUpLines
//

void TextEditor::moveUpLines()
{
	// don't move up if first line is in one of the cursors
	if (cursors[0].getSelectionStart().line != 0)
	{
		auto transaction = startTransaction();

		for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
		{
			auto start = cursor->getSelectionStart();
			auto end = cursor->getSelectionEnd();

			// delete existing lines
			auto deleteStart = document.getStartOfLine(start);
			auto deleteEnd = (end.column == 0) ? end : document.getNextLine(end);
			auto text = document.getSectionText(deleteStart, deleteEnd);
			deleteText(transaction, deleteStart, deleteEnd);
			cursors.adjustForDelete(cursor, deleteStart, deleteEnd);

			// insert text one line up
			auto insertStart = document.getUp(deleteStart);
			auto insertEnd = insertText(transaction, insertStart, text);
			cursors.adjustForInsert(cursor, insertStart, insertEnd);

			// update cursor
			cursor->update(start - Coordinate(1, 0), end - Coordinate(1, 0));
		}

		endTransaction(transaction);
	}
}


//
//	TextEditor::moveDownLines
//

void TextEditor::moveDownLines()
{
	// don't move up if last line is in one of the cursors
	if (!document.isLastLine(cursors[cursors.size() - 1].getSelectionStart().line))
	{
		auto transaction = startTransaction();

		for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
		{
			auto start = cursor->getSelectionStart();
			auto end = cursor->getSelectionEnd();

			// delete existing lines
			auto deleteStart = document.getStartOfLine(start);
			auto deleteEnd = (end.column == 0) ? end : document.getNextLine(end);
			auto text = document.getSectionText(deleteStart, deleteEnd);
			deleteText(transaction, deleteStart, deleteEnd);
			cursors.adjustForDelete(cursor, deleteStart, deleteEnd);

			// insert text one line down
			auto insertStart = document.getDown(deleteStart);
			auto insertEnd = insertText(transaction, insertStart, text);
			cursors.adjustForInsert(cursor, insertStart, insertEnd);

			// update cursor
			cursor->update(start + Coordinate(1, 0), end + Coordinate(1, 0));
		}

		endTransaction(transaction);
	}
}


//
//	TextEditor::toggleComments
//
void TextEditor::toggleComments()
{
	if (!language)
		return;

	// Single-line toggle uses the LINE comment marker ("--", "//", "#") when the
	// language has one; only fall back to the block opener for languages with no
	// line comment. Previously this always used commentStart, so Lua (commentStart
	// "--[[") prefixed every line with "--[[" instead of "--".
	const std::string& cs = language->singleLineComment.empty()
		? language->commentStart : language->singleLineComment;
	if (cs.empty())
		return;

	auto& cursor = cursors.getCurrent();

	Coordinate selStart = cursor.getInteractiveStart();
	Coordinate selEnd = cursor.getInteractiveEnd();

	if (cursor.hasSelection())
	{
		selStart = cursor.getSelectionStart();
		selEnd = cursor.getSelectionEnd();
		if (selEnd < selStart)
			std::swap(selStart, selEnd);
	}
	else
	{
		auto pos = cursor.getInteractiveEnd();
		selStart = Coordinate(pos.line, 0);
		selEnd = Coordinate(pos.line, document[pos.line].maxColumn);
	}

	int firstLine = selStart.line;
	int lastLine = selEnd.line;

	// First pass: determine if all lines are already commented
	bool allCommented = true;

	for (int line = firstLine; line <= lastLine; ++line)
	{
		std::string text = document.getLineText(line);

		// Skip leading whitespace
		size_t i = 0;
		while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
			++i;

		// Blank / whitespace-only lines don't carry a comment marker — ignore
		// them when deciding whether the block is fully commented (and below
		// when applying), so commenting a selection doesn't litter "--" on empty
		// lines and uncommenting isn't blocked by them.
		if (i >= text.size())
			continue;

		if (i + cs.size() > text.size() ||
			text.compare(i, cs.size(), cs) != 0)
		{
			allCommented = false;
			break;
		}
	}

	// Second pass: apply comment/uncomment in a SINGLE transaction so the
	// whole toggle is one undo step (not one per character per line).
	// `cursors.adjustFor{Insert,Delete}` only touches cursors *after* the
	// iterator we pass — so to also shift the selection that triggered the
	// toggle we update every cursor by hand below.
	auto transaction = startTransaction();
	for (int line = firstLine; line <= lastLine; ++line)
	{
		std::string text = document.getLineText(line);

		size_t i = 0;
		while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
			++i;

		if (i >= text.size())
			continue;   // blank line — leave it alone

		Coordinate start(line, (int)i);

		if (allCommented)
		{
			// Uncomment: remove commentStart if present
			if (i + cs.size() <= text.size() &&
				text.compare(i, cs.size(), cs) == 0)
			{
				Coordinate endPos(line, (int)(i + cs.size()));
				deleteText(transaction, start, endPos);
				for (auto cur = cursors.begin(); cur < cursors.end(); ++cur)
				{
					cur->adjustForDelete(start, endPos);
				}
			}
		}
		else
		{
			// Comment: insert commentStart at first non‑whitespace
			auto endInsert = insertText(transaction, start, cs);
			for (auto cur = cursors.begin(); cur < cursors.end(); ++cur)
			{
				cur->adjustForInsert(start, endInsert);
			}
		}
	}
	endTransaction(transaction);

	makeCursorVisible();
}

void TextEditor::toggleCommentsShift()
{
	if (!language)
		return;

	const std::string& cs = language->commentStart;
	const std::string& ce = language->commentEnd;

	// Shift‑variant only makes sense for block‑style comments
	if (cs.empty() || ce.empty())
		return;

	auto& cursor = cursors.getCurrent();
	if (!cursor.hasSelection())
		return;

	auto selStart = cursor.getSelectionStart();
	auto selEnd = cursor.getSelectionEnd();
	if (selEnd < selStart)
		std::swap(selStart, selEnd);

	std::string selected = document.getSectionText(selStart, selEnd);

	bool isWrapped =
		selected.size() >= cs.size() + ce.size() &&
		selected.compare(0, cs.size(), cs) == 0 &&
		selected.compare(selected.size() - ce.size(), ce.size(), ce) == 0;

	if (isWrapped)
	{
		// UNWRAP: remove leading cs and trailing ce
		std::string unwrapped = selected.substr(
			cs.size(),
			selected.size() - cs.size() - ce.size()
		);

		replaceSectionText(selStart, selEnd, unwrapped);

		Coordinate newEnd = selStart;
		newEnd.column += (int)unwrapped.size();
		cursor.update(selStart, newEnd);
	}
	else
	{
		// WRAP: cs + selected + ce
		std::string wrapped = cs + selected + ce;
		replaceSectionText(selStart, selEnd, wrapped);

		Coordinate newStart = selStart;
		Coordinate newEnd = selStart;
		newStart.column += (int)cs.size();
		newEnd.column += (int)(cs.size() + selected.size());

		cursor.update(selStart, newEnd);

	}

	makeCursorVisible();
}


//
//	TextEditor::filterSelections
//

void TextEditor::filterSelections(std::function<std::string(std::string_view)> filter)
{
	auto transaction = startTransaction();

	// process all cursors
	for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
	{
		auto start = cursor->getSelectionStart();
		auto end = cursor->getSelectionEnd();

		// process all lines in this cursor
		for (auto line = start.line; line <= end.line; line++)
		{
			if (Coordinate(line, 0) != end && document[line].size())
			{
				// get original text and run it through filter
				auto before = document.getSectionText(start, end);
				std::string after = filter(before);

				// update selection if anything changed
				if (after != before)
				{
					deleteText(transaction, start, end);
					cursors.adjustForDelete(cursor, start, end);
					auto newEnd = insertText(transaction, start, after);
					cursor->update(start, newEnd);
					cursors.adjustForInsert(cursor, start, newEnd);
				}
			}
		}
	}

	endTransaction(transaction);
}


//
//	TextEditor::selectionToLowerCase
//

void TextEditor::selectionToLowerCase()
{
	FilterSelections([](const std::string_view& text)
					 {
						 std::string result;
						 auto end = text.end();
						 auto i = text.begin();
						 char utf8[4];

						 while (i < end)
						 {
							 ImWchar codepoint;
							 i = CodePoint::read(i, end, &codepoint);
							 result.append(utf8, CodePoint::write(utf8, CodePoint::toLower(codepoint)));
						 }

						 return result;
					 });
}


//
//	TextEditor::selectionToUpperCase
//

void TextEditor::selectionToUpperCase()
{
	FilterSelections([](const std::string_view& text)
					 {
						 std::string result;
						 auto end = text.end();
						 auto i = text.begin();
						 char utf8[4];

						 while (i < end)
						 {
							 ImWchar codepoint;
							 i = CodePoint::read(i, end, &codepoint);
							 result.append(utf8, CodePoint::write(utf8, CodePoint::toUpper(codepoint)));
						 }

						 return result;
					 });
}



void TextEditor::Folder::rebuildFoldRanges(Document& document)
{
	// Save which folds were folded before we clear
	std::unordered_set<int> previouslyFolded;
	for (const auto& fr : *this)
	{
		if (fr.folded)
		{
			previouslyFolded.insert(fr.start.line);
		}
	}
	clear();

	const int lineCount = document.lineCount();
	if (lineCount <= 0)
		return;

	// Indent-based folding only applies to languages whose blocks are
	// expressed by indentation (Python). Brace-based languages (C, C++, C#,
	// JS, GLSL, HLSL, JSON, …) build folds from { } and would otherwise
	// produce a duplicate fold per code block.
	bool useIndentFolding = false;
	bool useLuaFolding = false;
	bool useIniFolding = false;
	bool useTagFolding = false;   // XML/XAML/HTML element nesting
	if (auto lang = document.getLanguage())
	{
		const std::string& name = lang->name;
		useIndentFolding = (name == "Python");
		useLuaFolding = (name == "Lua");
		useIniFolding = (name == "INI");
		useTagFolding = (name == "XML" || name == "XAML" || name == "HTML");
	}

	// --- XML/XAML/HTML element folds: <Tag ...> … </Tag> ---
	// Stack of (tagName, openCoordinate). Self-closing <Tag/>, declarations
	// (<?xml?>, <!-- -->, <!DOCTYPE>) and HTML void elements don't push.
	struct XmlTag { std::string name; Coordinate start; };
	std::vector<XmlTag> tagStack;

	// --- Lua keyword blocks (function/if/do/repeat … end/until) ---
	// Stack of opener positions; each closer pops and produces a fold.
	std::vector<Coordinate> luaStack;

	// --- INI sections: fold each [header] down to the line before the next
	// header (or EOF). `iniHeaderLine` is the line of the open section, -1 when
	// none is open yet. We emit the fold when the next header arrives / at EOF.
	int iniHeaderLine = -1;

	// --- Braces ---
	std::vector<Coordinate> braceStack;

	// --- #if / #endif ---
	struct IfBlock { Coordinate start; };
	std::vector<IfBlock> ifStack;

	// --- // region / // endregion ---
	struct RegionBlock { Coordinate start; };
	std::vector<RegionBlock> regionStack;

	// --- #pragma region / #pragma endregion ---
	struct PragmaBlock { Coordinate start; };
	std::vector<PragmaBlock> pragmaStack;

	// --- Multiline comments ---
	Coordinate commentStartPoint{};
	bool inComment = false;
	std::string commentStart, commentEnd;
	if (auto lang = document.getLanguage())
	{
		commentStart = lang->commentStart;
		commentEnd = lang->commentEnd;
	}

	// --- Indentation blocks ---
	struct IndentBlock { int indent; Coordinate start; };
	std::vector<IndentBlock> indentStack;

	// --- Consecutive single-line-comment blocks (docstrings, banners) ---
	// Track a run of lines whose first non-whitespace token is the language's
	// single-line comment marker (`//`, `#`, `--`, …). Any run of 3+ becomes a
	// Comment fold so docstring-style blocks can be collapsed.
	std::string lineComment, lineCommentAlt;
	if (auto lang = document.getLanguage())
	{
		lineComment = lang->singleLineComment;
		lineCommentAlt = lang->singleLineCommentAlt;
	}
	int  lineCommentBlockStart = -1;        // first line of current run, or -1
	int  lineCommentBlockCol = 0;          // column where the // sits on start line
	bool lineCommentBlockTriple = false;    // run's start line is a `///` doc comment
	auto flushLineCommentBlock = [&](int endLine)
		{
			if (lineCommentBlockStart < 0) return;
			int runLength = endLine - lineCommentBlockStart + 1;
			if (runLength >= 3)
			{
				FoldRange fr(
					Coordinate(lineCommentBlockStart, lineCommentBlockCol),
					Coordinate(endLine, 0),
					Comment);
				fr.docComment = lineCommentBlockTriple;
				push_back(fr);
			}
			lineCommentBlockStart = -1;
			lineCommentBlockTriple = false;
		};

	for (int line = 0; line < lineCount; ++line)
	{
		const std::string text = document.getLineText(line);

		// Braces { }
		for (int it = 0; it < static_cast<int>(text.size()); ++it)
		{
			char c = text[it];
			if (c == '{')
			{
				braceStack.push_back(Coordinate(line, it));
			}
			else if (c == '}')
			{
				if (!braceStack.empty())
				{
					Coordinate start = braceStack.back();
					braceStack.pop_back();
					if (start.line < line)
					{
						push_back(FoldRange(start, Coordinate(line, it), Braces));
					}
				}
			}
		}

		// XML / XAML / HTML element folds. Scan every '<' on the line: an
		// opening tag pushes, a matching </close> pops and emits a fold spanning
		// the two lines. Self-closing <Tag/>, processing instructions <?…?>,
		// comments <!-- … -->, and declarations <!…> never push.
		if (useTagFolding)
		{
			size_t p = 0;
			while ((p = text.find('<', p)) != std::string::npos)
			{
				if (p + 1 >= text.size()) break;
				char c1 = text[p + 1];
				if (c1 == '?' || c1 == '!')   // <?xml?>, <!-- -->, <!DOCTYPE>
				{
					p += 1;
					continue;
				}
				bool closing = (c1 == '/');
				size_t nameStart = p + (closing ? 2 : 1);
				size_t q = nameStart;
				while (q < text.size() &&
				       (std::isalnum((unsigned char) text[q]) || text[q] == '_' ||
				        text[q] == '-' || text[q] == ':' || text[q] == '.'))
					++q;
				std::string tagName = text.substr(nameStart, q - nameStart);
				if (tagName.empty()) { p = nameStart; continue; }

				// Find this tag's terminating '>' to detect self-closing "/>".
				size_t gt = text.find('>', q);
				bool selfClose = (gt != std::string::npos && gt > 0 && text[gt - 1] == '/');

				if (closing)
				{
					// Pop until we match (tolerates unbalanced/void tags).
					for (size_t k = tagStack.size(); k-- > 0; )
					{
						if (tagStack[k].name == tagName)
						{
							Coordinate start = tagStack[k].start;
							tagStack.resize(k);
							if (start.line < line)
								push_back(FoldRange(start, Coordinate(line, (int) p), Region));
							break;
						}
					}
				}
				else if (!selfClose)
				{
					tagStack.push_back({ tagName, Coordinate(line, (int) p) });
				}
				p = (gt != std::string::npos) ? gt + 1 : q;
			}
		}

		// #if / #ifdef / #ifndef ... #endif
		size_t idx = 0;
		if (starts_with_any_trimmed(text, { "#if", "#ifdef", "#ifndef" }, idx))
		{
			ifStack.push_back({ Coordinate(line, static_cast<int>(idx)) });
		}
		else if (starts_with_trimmed(text, "#endif", idx))
		{
			if (!ifStack.empty())
			{
				Coordinate start = ifStack.back().start;
				ifStack.pop_back();
				if (start.line < line)
				{
					push_back(FoldRange(start, Coordinate(line, static_cast<int>(idx)), IfDef));
				}
			}
		}

		// // region / // endregion (JS/TS)  and  #region / #endregion (C#/F#/VB/PS).
		// A given file uses one style, so sharing regionStack is safe; #endregion is
		// tested before #region patterns can't alias it ("#e..." vs "#r...").
		idx = 0;
		if (starts_with_trimmed(text, "// region", idx) || starts_with_trimmed(text, "#region", idx))
		{
			regionStack.push_back({ Coordinate(line, static_cast<int>(idx)) });
		}
		else if (starts_with_trimmed(text, "// endregion", idx) || starts_with_trimmed(text, "#endregion", idx))
		{
			if (!regionStack.empty())
			{
				Coordinate start = regionStack.back().start;
				regionStack.pop_back();
				if (start.line < line)
				{
					push_back(FoldRange(start, Coordinate(line, static_cast<int>(idx)), Region));
				}
			}
		}

		// #pragma region / #pragma endregion
		idx = 0;
		if (starts_with_trimmed(text, "#pragma region", idx))
		{
			pragmaStack.push_back({ Coordinate(line, static_cast<int>(idx)) });
		}
		else if (starts_with_trimmed(text, "#pragma endregion", idx))
		{
			if (!pragmaStack.empty())
			{
				Coordinate start = pragmaStack.back().start;
				pragmaStack.pop_back();
				if (start.line < line)
				{
					push_back(FoldRange(start, Coordinate(line, static_cast<int>(idx)), PragmaRegion));
				}
			}
		}

		// Multiline comments /* */
		if (!commentStart.empty() && !commentEnd.empty())
		{
			if (!inComment)
			{
				size_t pos = text.find(commentStart);
				if (pos != std::string::npos)
				{
					inComment = true;
					commentStartPoint = Coordinate(line, static_cast<int>(pos));
					size_t endPos = text.find(commentEnd, pos + commentStart.size());
					if (endPos != std::string::npos)
					{
						inComment = false;
						if (commentStartPoint.line < line)
						{
							push_back(FoldRange(commentStartPoint, Coordinate(line, static_cast<int>(endPos)), Comment));
						}
					}
				}
			}
			else
			{
				size_t endPos = text.find(commentEnd);
				if (endPos != std::string::npos)
				{
					inComment = false;
					if (commentStartPoint.line < line)
					{
						push_back(FoldRange(commentStartPoint, Coordinate(line, static_cast<int>(endPos)), Comment));
					}
				}
			}
		}

		// Consecutive `//`-style comment blocks
		if (!lineComment.empty())
		{
			size_t idx2 = 0;
			bool isLineComment =
				starts_with_trimmed(text, lineComment.c_str(), idx2) ||
				(!lineCommentAlt.empty() && starts_with_trimmed(text, lineCommentAlt.c_str(), idx2));
			if (isLineComment)
			{
				if (lineCommentBlockStart < 0)
				{
					lineCommentBlockStart = line;
					lineCommentBlockCol = static_cast<int>(idx2);
					// A `///` doc-comment run gets its own preview. Detected by three
					// slashes at the marker column (works for // langs; harmless else).
					lineCommentBlockTriple = (text.compare(idx2, 3, "///") == 0);
				}
			}
			else
			{
				// Non-comment line ends the run. Blank lines also break it.
				flushLineCommentBlock(line - 1);
			}
		}

		// Indentation-based folding (Python and similar).
		// Each block is opened by a header line (e.g. `def foo():`) whose next
		// non-blank line has a greater indent. The block ends on the last line
		// whose indent is still >= the body's indent. We track the previous
		// non-blank line + its indent and create folds when indent decreases.
		if (useIndentFolding && !is_blank(text))
		{
			int indent = indent_level(text);

			// Close any blocks whose body has now ended (current indent below them)
			while (!indentStack.empty() && indent < indentStack.back().indent)
			{
				IndentBlock blk = indentStack.back();
				indentStack.pop_back();
				// header line was at blk.start.line - 1 conceptually, body spans
				// blk.start.line..(line-1)
				int headerLine = blk.start.line - 1;
				int lastBodyLine = line - 1;
				if (headerLine >= 0 && headerLine < lastBodyLine)
				{
					push_back(FoldRange(
						Coordinate(headerLine, 0),
						Coordinate(lastBodyLine, blk.indent),
						Indent));
				}
			}

			// Open a new block when indent strictly increases.
			// `indent > top.indent` (or empty stack but indent > 0) — never for
			// top-level lines, which avoids the per-line spurious folds.
			int topIndent = indentStack.empty() ? -1 : indentStack.back().indent;
			if (indent > topIndent && indent > 0)
			{
				indentStack.push_back({ indent, Coordinate(line, indent) });
			}
		}

		// Lua keyword-delimited block folding. `function`, `if`, `do` (covers
		// for/while/standalone do), and `repeat` open a block; `end` and
		// `until` close one. One-liners (`if x then y end`) net out on the same
		// line and produce no fold (start.line == line is skipped).
		if (useLuaFolding)
		{
			// Strip a trailing line comment so keywords inside `-- ...` don't count.
			std::string scan = text;
			size_t cpos = scan.find("--");
			if (cpos != std::string::npos) scan.resize(cpos);

			size_t i = 0;
			while (i < scan.size())
			{
				char c = scan[i];
				if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
				{
					size_t s = i;
					while (i < scan.size() &&
						(std::isalnum(static_cast<unsigned char>(scan[i])) || scan[i] == '_'))
						++i;
					std::string w = scan.substr(s, i - s);
					if (w == "function" || w == "if" || w == "do" || w == "repeat")
					{
						luaStack.push_back(Coordinate(line, static_cast<int>(s)));
					}
					else if (w == "end" || w == "until")
					{
						if (!luaStack.empty())
						{
							Coordinate start = luaStack.back();
							luaStack.pop_back();
							if (start.line < line)
								push_back(FoldRange(start, Coordinate(line, static_cast<int>(s)), Indent));
						}
					}
				}
				else
				{
					++i;
				}
			}
		}

		// INI section folding — a header line is one whose first non-whitespace
		// character is '['. Each header closes the previous section's fold
		// (header .. line-1) and opens a new one.
		if (useIniFolding)
		{
			size_t s2 = 0;
			while (s2 < text.size() && (text[s2] == ' ' || text[s2] == '\t')) ++s2;
			if (s2 < text.size() && text[s2] == '[')
			{
				if (iniHeaderLine >= 0 && iniHeaderLine < line - 1)
					push_back(FoldRange(Coordinate(iniHeaderLine, 0),
					                    Coordinate(line - 1, 0), Region));
				iniHeaderLine = line;
			}
		}
	}

	// Close a still-open INI section at EOF.
	if (useIniFolding && iniHeaderLine >= 0 && iniHeaderLine < lineCount - 1)
		push_back(FoldRange(Coordinate(iniHeaderLine, 0),
		                    Coordinate(lineCount - 1, 0), Region));

	// Close any line-comment run still open at EOF
	flushLineCommentBlock(lineCount - 1);

	// Close remaining open indent blocks at EOF
	if (useIndentFolding)
	{
		for (auto& blk : indentStack)
		{
			int headerLine = blk.start.line - 1;
			int lastBodyLine = lineCount - 1;
			if (headerLine >= 0 && headerLine < lastBodyLine)
			{
				push_back(FoldRange(
					Coordinate(headerLine, 0),
					Coordinate(lastBodyLine, blk.indent),
					Indent));
			}
		}
	}

	// Close open multiline comment at EOF
	if (inComment && commentStartPoint.line >= 0 && commentStartPoint.line < lineCount - 1)
	{
		push_back(FoldRange(commentStartPoint, Coordinate(lineCount - 1, 0), Comment));
	}

	// Deduplicate by start line — keep the outermost (largest end.line)
	// so each line shows at most one fold arrow.
	{
		std::unordered_map<int, size_t> bestByStart;
		for (size_t i = 0; i < size(); ++i)
		{
			const auto& fr = (*this)[i];
			auto it = bestByStart.find(fr.start.line);
			if (it == bestByStart.end() || (*this)[it->second].end.line < fr.end.line)
			{
				bestByStart[fr.start.line] = i;
			}
		}
		std::vector<FoldRange> kept;
		kept.reserve(bestByStart.size());
		for (auto& kv : bestByStart) kept.push_back((*this)[kv.second]);
		static_cast<std::vector<FoldRange>&>(*this) = std::move(kept);
	}

	// Restore previously folded state
	for (auto& fr : *this)
	{
		if (previouslyFolded.count(fr.start.line))
		{
			fr.folded = true;
		}
	}

	updateVisibility(document);
}








void TextEditor::Folder::updateVisibility(Document& document)
{
	const int lineCount = document.lineCount();
	if (lineCount == 0) return;

	// Reset all lines to visible with foldLevel 0
	for (int i = 0; i < lineCount; ++i)
	{
		document[i].visible = true;
		document[i].foldLevel = 0;
	}

	// For each folded range, hide inner lines (nesting increments foldLevel).
	// Indent folds include the last body line, so hide [start+1, end] inclusive.
	// Brace / region / comment folds have their closer on end.line (still visible).
	for (const auto& fr : *this)
	{
		if (!fr.folded) continue;
		int startL = fr.start.line;
		int endL = fr.end.line;
		if (startL >= endL || startL < 0 || endL >= lineCount) continue;

		int hideEnd = (fr.type == Indent) ? endL + 1 : endL;
		for (int i = startL + 1; i < hideEnd; ++i)
		{
			if (i >= 0 && i < lineCount)
			{
				document[i].foldLevel++;
				document[i].visible = (document[i].foldLevel <= 0);
			}
		}
	}

	// Rebuild the visual<->document line maps + hidden count in one pass. These
	// keep the per-frame visual-index mapping O(1) even while a fold is collapsed
	// (this runs only on fold/document changes, not per frame). Without them,
	// folding a block in a large file dropped FPS hard because every mapping call
	// went O(lines).
	visibleToDoc.clear();
	visibleToDoc.reserve(lineCount);
	docToVisible.assign(lineCount, 0);
	int hidden = 0;
	int lastVI = 0;
	for (int i = 0; i < lineCount; ++i)
	{
		if (document[i].visible)
		{
			lastVI = static_cast<int>(visibleToDoc.size());
			docToVisible[i] = lastVI;
			visibleToDoc.push_back(i);
		}
		else
		{
			docToVisible[i] = lastVI;   // folded line snaps to nearest visible before it
			++hidden;
		}
	}
	hiddenLineCount = hidden;
}

void TextEditor::Folder::toggleFold(int line, Document& document)
{
	for (auto& fr : *this)
	{
		if (fr.start.line == line)
		{
			fr.folded = !fr.folded;

			// A fold is a VISIBILITY change only — updateVisibility() refreshes the
			// O(1) line maps and the immediate-mode UI redraws next frame on its
			// own. Do NOT call document.setUpdated(true): that flags the document as
			// content-changed, which makes render() run a full rebuildFoldRanges()
			// parse AND a full bracketeer rescan every single toggle (the big
			// fold-latency bug). Real edits set the document's updated flag
			// internally, so they still rebuild correctly.
			updateVisibility(document);
			break;
		}
	}
}


bool TextEditor::Folder::unfoldContaining(int line, Document& document)
{
	if (!foldingEnabled) return false;
	if (line < 0 || line >= document.lineCount()) return false;
	bool changed = false;
	// Any edit anywhere in the fold's range (start..end, inclusive) unfolds it.
	// VSCode behaviour: touching the folded region reveals it.
	for (auto& fr : *this)
	{
		if (fr.folded && fr.start.line <= line && line <= fr.end.line)
		{
			fr.folded = false;
			foldedStartLines.erase(fr.start.line);
			changed = true;
		}
	}
	if (changed)
	{
		// visibility-only change — see toggleFold(): no document.setUpdated(true).
		updateVisibility(document);
	}
	return changed;
}


void TextEditor::Folder::foldAll(Document& document)
{
	if (!foldingEnabled) return;
	for (auto& fr : *this)
	{
		fr.folded = true;
		foldedStartLines.insert(fr.start.line);
	}
	// visibility-only change — see toggleFold(): no document.setUpdated(true).
	updateVisibility(document);
}


void TextEditor::Folder::unfoldAll(Document& document)
{
	if (!foldingEnabled) return;
	for (auto& fr : *this)
	{
		fr.folded = false;
	}
	foldedStartLines.clear();
	// visibility-only change — see toggleFold(): no document.setUpdated(true).
	updateVisibility(document);
}


bool TextEditor::Folder::toggleCurrent(int line, Document& document, int forceFolded)
{
	if (!foldingEnabled) return false;
	if (line < 0 || line >= document.lineCount()) return false;

	// Pick the innermost (smallest span) fold whose start..end covers this line.
	FoldRange* best = nullptr;
	int bestSpan = std::numeric_limits<int>::max();
	for (auto& fr : *this)
	{
		if (fr.start.line <= line && line <= fr.end.line)
		{
			int span = fr.end.line - fr.start.line;
			if (span < bestSpan) { best = &fr; bestSpan = span; }
		}
	}
	if (!best) return false;

	bool target = (forceFolded == 0) ? !best->folded : (forceFolded > 0);
	if (best->folded == target) return false;
	best->folded = target;
	if (target) foldedStartLines.insert(best->start.line);
	else        foldedStartLines.erase(best->start.line);
	// visibility-only change — see toggleFold(): no document.setUpdated(true).
	updateVisibility(document);
	return true;
}

//
//	TextEditor::stripTrailingWhitespaces
//

void TextEditor::stripTrailingWhitespaces()
{
	auto transaction = startTransaction();

	// process all the lines
	for (int i = 0; i < document.lineCount(); i++)
	{
		auto& line = document[i];
		size_t lineSize = line.size();
		size_t whitespace = std::numeric_limits<std::size_t>::max();
		bool done = false;

		// look for first non-whitespace glyph at the end of the line
		if (lineSize)
		{
			for (auto index = lineSize - 1; !done; index--)
			{
				if (CodePoint::isWhiteSpace(line[index].codepoint))
				{
					whitespace = index;

					if (index == 0)
					{
						done = true;
					}

				}
				else
				{
					done = true;
				}
			}
		}

		// remove whitespaces (if required)
		if (whitespace != std::numeric_limits<std::size_t>::max())
		{
			auto start = Coordinate(i, document.getColumn(line, whitespace));
			auto end = Coordinate(i, document.getColumn(line, lineSize));
			deleteText(transaction, start, end);
		}
	}

	// update cursor if transaction wasn't empty
	if (endTransaction(transaction))
	{
		cursors.setCursor(document.normalizeCoordinate(cursors.getCurrent().getSelectionEnd()));
	}
}


//
//	TextEditor::filterLines
//

void TextEditor::filterLines(std::function<std::string(std::string_view)> filter)
{
	auto transaction = startTransaction();

	// process all the lines
	for (int i = 0; i < document.lineCount(); i++)
	{
		// get original text and run it through filter
		auto before = document.getLineText(i);
		std::string after = filter(before);

		// update line if anything changed
		if (after != before)
		{
			auto start = Coordinate(i, 0);
			auto end = document.getEndOfLine(start);
			deleteText(transaction, start, end);
			insertText(transaction, start, after);
		}
	}

	// update cursor if transaction wasn't empty
	if (endTransaction(transaction))
	{
		cursors.setCursor(document.normalizeCoordinate(cursors.getCurrent().getSelectionEnd()));
	}
}


//
//	TextEditor::tabsToSpaces
//

void TextEditor::tabsToSpaces()
{
	filterLines([this](const std::string_view& input)
				{
					auto tabSize = static_cast<size_t>(document.getTabSize());
					std::string output;
					auto end = input.end();
					auto i = input.begin();
					size_t pos = 0;

					while (i < end)
					{
						char utf8[4];
						ImWchar codepoint;
						i = CodePoint::read(i, end, &codepoint);

						if (codepoint == '\t')
						{
							auto spaces = tabSize - (pos % tabSize);
							output.append(spaces, ' ');
							pos += spaces;

						}
						else
						{
							output.append(utf8, CodePoint::write(utf8, codepoint));
							pos++;
						}
					}

					return output;
				});
}


//
//	TextEditor::spacesToTabs
//

void TextEditor::spacesToTabs()
{
	FilterLines([this](const std::string_view& input)
				{
					auto tabSize = static_cast<size_t>(document.getTabSize());
					std::string output;
					auto end = input.end();
					auto i = input.begin();
					size_t pos = 0;
					size_t spaces = 0;

					while (i < end)
					{
						char utf8[4];
						ImWchar codepoint;
						i = CodePoint::read(i, end, &codepoint);

						if (codepoint == ' ')
						{
							spaces++;

						}
						else
						{
							while (spaces)
							{
								auto spacesUntilNextTab = tabSize - (pos % tabSize);

								if (spacesUntilNextTab == 1)
								{
									output += ' ';
									pos++;
									spaces--;

								}
								else if (spaces >= spacesUntilNextTab)
								{
									output += '\t';
									pos += spacesUntilNextTab;
									spaces -= spacesUntilNextTab;

								}
								else if (codepoint != '\t')
									while (spaces)
									{
										output += ' ';
										pos++;
										spaces--;
									}

								else
								{
									spaces = 0;
								}
							}

							if (codepoint == '\t')
							{
								output += '\t';
								pos += tabSize - (pos % tabSize);

							}
							else
							{
								output.append(utf8, CodePoint::write(utf8, codepoint));
								pos++;
							}
						}
					}

					return output;
				});
}


//
//	TextEditor::startTransaction
//

std::shared_ptr<TextEditor::Transaction> TextEditor::startTransaction(bool cancelsAutoComplete)
{
	if (cancelsAutoComplete)
	{
		autocomplete.cancel();
	}

	std::shared_ptr<Transaction> transaction = Transactions::create();
	transaction->setBeforeState(cursors);
	return transaction;
}


//
//	TextEditor::endTransaction
//

bool TextEditor::endTransaction(std::shared_ptr<Transaction> transaction)
{
	if (transaction->actions() > 0)
	{
		cursors.update();
		transaction->setAfterState(cursors);
		transactions.add(transaction);
		std::vector<Change> changes;

		if (transactionCallback)
		{
			for (auto& action : *transaction)
			{
				auto& change = changes.emplace_back();
				change.insert = action.type == Action::Type::insertText;

				change.startLine = static_cast<int>(action.start.line);
				change.startColumn = static_cast<int>(action.start.column);
				change.startIndex = static_cast<int>(document.getIndex(action.start));

				change.startLine = static_cast<int>(action.end.line);
				change.startColumn = static_cast<int>(action.end.column);
				change.startIndex = static_cast<int>(document.getIndex(action.end));

				change.text = action.text;
			}

			transactionCallback(changes);
		}

		return true;

	}
	else
	{
		return false;
	}
}


//
//	TextEditor::insertTextIntoAllCursors
//

void TextEditor::insertTextIntoAllCursors(std::shared_ptr<Transaction> transaction, const std::string_view& text)
{
	// delete any selection content first
	deleteTextFromAllCursors(transaction);

	// insert the text
	for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
	{
		auto start = cursor->getSelectionStart();
		auto end = insertText(transaction, start, text);
		cursor->update(end, false);
		cursors.adjustForInsert(cursor, start, end);
	}
}


//
//	TextEditor::deleteTextFromAllCursors
//

void TextEditor::deleteTextFromAllCursors(std::shared_ptr<Transaction> transaction)
{
	for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
	{
		if (cursor->hasSelection())
		{
			auto start = cursor->getSelectionStart();
			auto end = cursor->getSelectionEnd();
			deleteText(transaction, start, end);
			cursor->update(start, false);
			cursors.adjustForDelete(cursor, start, end);
		}
	}
}


//
//	TextEditor::autoIndentAllCursors
//

void TextEditor::autoIndentAllCursors(std::shared_ptr<Transaction> transaction)
{
	for (auto cursor = cursors.begin(); cursor < cursors.end(); cursor++)
	{
		auto start = cursor->getSelectionStart();

		// delete any selections
		if (cursor->hasSelection())
		{
			auto end = cursor->getSelectionEnd();
			deleteText(transaction, start, end);
			cursors.adjustForDelete(cursor, start, end);
		}

		// get previous and next character
		auto index = document.getIndex(start);
		auto& line = document[start.line];
		ImWchar previousChar = index > 0 ? line[index - 1].codepoint : 0;
		ImWchar nextChar = index < line.size() ? line[index].codepoint : 0;

		// remove extra whitespaces if required
		if (CodePoint::isWhiteSpace(nextChar))
		{
			while (index < line.size() && CodePoint::isWhiteSpace(line[index].codepoint))
			{
				index++;
			}

			auto end = Coordinate(start.line, document.getColumn(start.line, index));
			deleteText(transaction, start, end);
			cursors.adjustForDelete(cursor, start, end);
		}

		// determine whitespace at start of current line
		std::string whitespace;

		for (size_t i = 0; i < line.size() && CodePoint::isWhiteSpace(line[i].codepoint); i++)
		{
			char utf8[4];
			whitespace.append(utf8, CodePoint::write(utf8, line[i].codepoint));
		}

		// determine text to insert
		std::string insert = "\n" + whitespace;
		auto newCursorIndex = static_cast<int>(whitespace.size());

		// handle special cases
		if (previousChar == CodePoint::openCurlyBracket || previousChar == CodePoint::openSquareBracket)
		{
			// add to an existing block
			insert += "\t";
			newCursorIndex++;

			if ((previousChar == CodePoint::openCurlyBracket && nextChar == CodePoint::closeCurlyBracket) ||
				(previousChar == CodePoint::openSquareBracket && nextChar == CodePoint::closeSquareBracket))
			{

				// open a new block
				insert += "\n" + whitespace;
			}
		}

		// insert new text
		auto end = insertText(transaction, start, insert);
		cursors.adjustForInsert(cursor, start, end);

		// set new cursor location
		cursor->update(Coordinate(start.line + 1, document.getColumn(start.line + 1, newCursorIndex)), false);
	}
}


//
//	TextEditor::insertText
//

TextEditor::Coordinate TextEditor::insertText(std::shared_ptr<Transaction> transaction, Coordinate start, const std::string_view& text)
{
	// update document, add transaction and return coordinate of end of insert
	// this function does not touch the cursors
	// VSCode-like behaviour: any edit that lands in a folded region unfolds it.
	if (foldRanges.foldingEnabled)
	{
		foldRanges.unfoldContaining(start.line, document);
	}
	auto end = document.insertText(start, text);
	transaction->addInsert(start, end, text);
	makeCursorVisible();
	return end;
}


//
//	TextEditor::deleteText
//

void TextEditor::deleteText(std::shared_ptr<Transaction> transaction, Coordinate start, Coordinate end)
{
	// update document and add transaction
	// this function does not touch the cursors
	// VSCode-like behaviour: unfold any fold whose range overlaps the deletion.
	if (foldRanges.foldingEnabled)
	{
		for (int ln = start.line; ln <= end.line && ln < document.lineCount(); ++ln)
		{
			if (ln >= 0) foldRanges.unfoldContaining(ln, document);
		}
	}
	auto text = document.getSectionText(start, end);
	document.deleteText(start, end);
	transaction->addDelete(start, end, text);
	makeCursorVisible();
}


//
//	TextEditor::updatePalette
//

void TextEditor::updatePalette()
{
	// Update palette with the current alpha from style
	paletteAlpha = ImGui::GetStyle().Alpha;

	for (size_t i = 0; i < static_cast<size_t>(Color::count); i++)
	{
		auto color = ImGui::ColorConvertU32ToFloat4(paletteBase[i]);
		color.w *= paletteAlpha;
		palette[i] = ImGui::ColorConvertFloat4ToU32(color);
	}
}


//
//	Color palettes
//

const TextEditor::Palette& TextEditor::GetDarkPalette()
{
	const static Palette p = { {
		IM_COL32(224, 224, 224, 255),	// text
		IM_COL32(197, 134, 192, 255),	// keyword
		IM_COL32(90, 179, 155, 255),	// declaration
		IM_COL32(181, 206, 168, 255),	// number
		IM_COL32(206, 145, 120, 255),	// string
		IM_COL32(255, 255, 153, 255),	// punctuation
		IM_COL32(64, 192, 128, 255),	// preprocessor
		IM_COL32(156, 220, 254, 255),	// identifier
		IM_COL32(79, 193, 255, 255),	// known identifier
		IM_COL32(106, 153,  85, 255),	// comment
		IM_COL32(30,  30,  30, 255),	// background
		IM_COL32(224, 224, 224, 255),	// cursor
		IM_COL32(32,  96, 160, 255),	// selection
		IM_COL32(80,  80,  80, 255),	// whitespace
		IM_COL32(70,  70,  70, 255),	// matchingBracketBackground
		IM_COL32(140, 140, 140, 255),	// matchingBracketActive
		IM_COL32(246, 222,  36, 255),	// matchingBracketLevel1
		IM_COL32(66, 120, 198, 255),	// matchingBracketLevel2
		IM_COL32(213,  96, 213, 255),	// matchingBracketLevel3
		IM_COL32(198,   8,  32, 255),	// matchingBracketError
		IM_COL32(128, 128, 144, 255),	// line number
		IM_COL32(224, 224, 240, 255),	// current line number
	} };

	return p;
}

const TextEditor::Palette& TextEditor::GetLightPalette()
{
	const static Palette p = { {
		IM_COL32(64,  64,  64, 255),	// text
		IM_COL32(170,  0, 220, 255),	// keyword
		IM_COL32(65,   0, 255, 255),	// declaration
		IM_COL32(40, 140,  90, 255),	// number
		IM_COL32(160,  32,  32, 255),	// string
		IM_COL32(0,   0,   0, 255),	// punctuation
		IM_COL32(96,  96,  64, 255),	// preprocessor
		IM_COL32(64,  64,  64, 255),	// identifier
		IM_COL32(16,  96,  96, 255),	// known identifier
		IM_COL32(35, 135,   5, 255),	// comment
		IM_COL32(255, 255, 255, 255),	// background
		IM_COL32(0,   0,   0, 255),	// cursor
		IM_COL32(0,   0,  96,  64),	// selection
		IM_COL32(144, 144, 144, 144),	// whitespace
		IM_COL32(180, 180, 180, 144),	// matchingBracketBackground
		IM_COL32(72,  72,  72, 255),	// matchingBracketActive
		IM_COL32(70,   0, 250, 255),	// matchingBracketLevel1
		IM_COL32(80, 160,  70, 255),	// matchingBracketLevel2
		IM_COL32(120,  60, 25, 255),	// matchingBracketLevel3
		IM_COL32(198,   8,  32, 255),	// matchingBracketError
		IM_COL32(0,  80,  80, 255),	// line number
		IM_COL32(0,   0,   0, 255),	// current line number
	} };

	return p;
}

TextEditor::Palette TextEditor::defaultPalette = TextEditor::GetDarkPalette();


//
//	TextEditor::Cursor::adjustCoordinateForInsert
//

TextEditor::Coordinate TextEditor::Cursor::adjustCoordinateForInsert(Coordinate coordinate, Coordinate insertStart, Coordinate insertEnd)
{
	if (coordinate.line == insertStart.line)
	{
		coordinate.column += insertEnd.column - insertStart.column;
	}

	coordinate.line += insertEnd.line - insertStart.line;
	return coordinate;
}


//
//	TextEditor::Cursor::adjustForInsert
//

void TextEditor::Cursor::adjustForInsert(Coordinate insertStart, Coordinate insertEnd)
{
	start = adjustCoordinateForInsert(start, insertStart, insertEnd);
	end = adjustCoordinateForInsert(end, insertStart, insertEnd);
}


//
//	TextEditor::Cursor::adjustCoordinateForDelete
//

TextEditor::Coordinate TextEditor::Cursor::adjustCoordinateForDelete(Coordinate coordinate, Coordinate deleteStart, Coordinate deleteEnd)
{
	// Cases:
	//  1. Coordinate is strictly before the deletion → unchanged.
	//  2. Coordinate is inside the deletion → snap to deleteStart.
	//  3. Coordinate is on the SAME line as deleteEnd, AT or AFTER deleteEnd
	//     → land on deleteStart.line, column shifted relative to the merge point.
	//  4. Coordinate is on a later line → shift line up by the deleted line count.
	auto cmp = [](Coordinate a, Coordinate b)
		{
			if (a.line != b.line) return a.line < b.line;
			return a.column < b.column;
		};

	// (1) before the deletion
	if (cmp(coordinate, deleteStart)) return coordinate;

	// (2) inside the deletion (including exactly at deleteEnd we treat as "just past")
	if (cmp(coordinate, deleteEnd)) return deleteStart;

	// (3) same line as deleteEnd, at or past it
	if (coordinate.line == deleteEnd.line)
	{
		coordinate.line = deleteStart.line;
		coordinate.column = deleteStart.column + (coordinate.column - deleteEnd.column);
		return coordinate;
	}

	// (4) wholly after deletion on a later line
	coordinate.line -= (deleteEnd.line - deleteStart.line);
	return coordinate;
}


//
//	TextEditor::Cursor::adjustForDelete
//

void TextEditor::Cursor::adjustForDelete(Coordinate deleteStart, Coordinate deleteEnd)
{
	start = adjustCoordinateForDelete(start, deleteStart, deleteEnd);
	end = adjustCoordinateForDelete(end, deleteStart, deleteEnd);
}


//
//	TextEditor::Cursors::reset
//

void TextEditor::Cursors::reset()
{
	clear();
	main = 0;
	current = 0;
}


//
//	TextEditor::Cursors::setCursor
//

void TextEditor::Cursors::setCursor(Coordinate cursorStart, Coordinate cursorEnd)
{
	reset();
	emplace_back(cursorStart, cursorEnd);
	front().setMain(true);
	front().setCurrent(true);
}


//
//	TextEditor::Cursors::addCursor
//

void TextEditor::Cursors::addCursor(Coordinate start, Coordinate end)
{
	// Defensive: if `current` is somehow out of range (e.g. left over from a
	// just-cleared cursors vector or after a move-assign in update()), don't
	// dereference it via at() — that's where the iterator-incompatible
	// debug assert was tripping on Ctrl+click.
	if (current < size()) at(current).setCurrent(false);
	emplace_back(start, end);
	back().setCurrent(true);
	current = size() - 1;
}


//
//	TextEditor::Cursors::anyHasSelection
//

bool TextEditor::Cursors::anyHasSelection() const
{
	for (auto cursor = begin(); cursor < end(); cursor++)
	{
		if (cursor->hasSelection())
		{
			return true;
		}
	}

	return false;
}


//
//	TextEditor::Cursors::allHaveSelection
//

bool TextEditor::Cursors::allHaveSelection() const
{
	for (auto cursor = begin(); cursor < end(); cursor++)
	{
		if (!cursor->hasSelection())
		{
			return false;
		}
	}

	return true;
}


//
//	TextEditor::Cursors::anyHasUpdate
//

bool TextEditor::Cursors::anyHasUpdate() const
{
	for (auto cursor = begin(); cursor < end(); cursor++)
	{
		if (cursor->isUpdated())
		{
			return true;
		}
	}

	return false;
}


//
//	TextEditor::Cursors::clearAll
//

void TextEditor::Cursors::clearAll()
{
	reset();
	emplace_back(Coordinate(0, 0));
	front().setMain(true);
	front().setCurrent(true);
}


//
//	TextEditor::Cursors::clearAdditional
//

void TextEditor::Cursors::clearAdditional(bool reset)
{
	for (auto cursor = begin(); cursor < end();)
	{
		if (cursor->isMain())
		{
			cursor++;

		}
		else
		{
			cursor = erase(cursor);
		}
	}

	main = 0;
	current = 0;
	front().setCurrent(true);

	if (reset)
	{
		front().resetToEnd();
	}
}


//
//	TextEditor::Cursors::clearUpdated
//

void TextEditor::Cursors::clearUpdated()
{
	for (auto cursor = begin(); cursor < end(); cursor++)
	{
		cursor->setUpdated(false);
	}
}


//
//	TextEditor::Cursors::update
//

void TextEditor::Cursors::update()
{
	// reset update flags
	clearUpdated();

	if (!hasMultiple()) return;

	// Sort by selection start.
	std::sort(begin(), end(), [](Cursor& a, Cursor& b)
			  {
				  return a.getSelectionStart() < b.getSelectionStart();
			  });

	// Build the merged list in a fresh vector — iterating + erasing the
	// underlying vector in-place with reverse iterators was the source of
	// "iterators incompatible" debug-asserts on Ctrl+click + multi-cursor.
	std::vector<Cursor> merged;
	merged.reserve(size());
	for (size_t i = 0; i < size(); ++i)
	{
		Cursor& c = at(i);
		if (merged.empty())
		{
			merged.push_back(c);
			continue;
		}
		Cursor& prev = merged.back();
		// `prev` ends at or past where `c` starts → overlap, fold c into prev.
		if (!(prev.getSelectionEnd() < c.getSelectionStart()))
		{
			if (c.getSelectionEnd() > prev.getSelectionEnd())
			{
				// Extend prev to cover c, preserving prev's "interactive" direction.
				if (prev.getInteractiveEnd() < prev.getInteractiveStart())
				{
					prev.update(c.getSelectionEnd(), prev.getSelectionStart());
				}
				else
				{
					prev.update(prev.getSelectionStart(), c.getSelectionEnd());
				}
			}
			if (c.isMain())    prev.setMain(true);
			if (c.isCurrent()) prev.setCurrent(true);
		}
		else
		{
			merged.push_back(c);
		}
	}

	// Replace the cursors vector with the merged result.
	static_cast<std::vector<Cursor>&>(*this) = std::move(merged);

	// Recompute main / current indices into the new vector.
	main = 0;
	current = 0;
	for (size_t c = 0; c < size(); ++c)
	{
		if (at(c).isMain())    main = c;
		if (at(c).isCurrent()) current = c;
	}
}


//
//	TextEditor::Cursors::adjustForInsert
//

void TextEditor::Cursors::adjustForInsert(iterator start, Coordinate insertStart, Coordinate insertEnd)
{
	for (auto cursor = start + 1; cursor < end(); cursor++)
	{
		cursor->adjustForInsert(insertStart, insertEnd);
	}
}


//
//	TextEditor::Cursors::adjustForDelete
//

void TextEditor::Cursors::adjustForDelete(iterator start, Coordinate deleteStart, Coordinate deleteEnd)
{
	for (auto cursor = start + 1; cursor < end(); cursor++)
	{
		cursor->adjustForDelete(deleteStart, deleteEnd);
	}
}


//
//	TextEditor::Document::setText
//

void TextEditor::Document::setText(const std::string_view& text)
{
	// reset document
	clearDocument();
	appendLine();
	updated = true;

	// process UTF-8 and generate lines of glyphs
	auto end = text.end();
	auto i = CodePoint::skipBOM(text.begin(), end);

	while (i < end)
	{
		ImWchar character;
		i = CodePoint::read(i, end, &character);

		if (character == '\n')
		{
			appendLine();

		}
		else if (insertSpacesOnTabs && character == '\t')
		{
			auto spaces = ((back().size() / tabSize) + 1) * tabSize - back().size();

			for (size_t s = 0; s < spaces; s++)
			{
				back().emplace_back(Glyph(' ', Color::text));
			}

		}
		else if (character != '\r')
		{
			back().emplace_back(Glyph(character, Color::text));
		}
	}

	// update maximum column counts
	updateMaximumColumn(0, lineCount() - 1);
}


//
//	TextEditor::Document::setText
//

void TextEditor::Document::setText(const std::vector<std::string_view>& text)
{
	// reset document
	clearDocument();
	updated = true;

	if (text.size())
	{
		// process input UTF-8 and generate lines of glyphs
		for (auto& line : text)
		{
			appendLine();
			auto i = line.begin();
			auto end = line.end();

			while (i < end)
			{
				ImWchar character;
				i = CodePoint::read(i, end, &character);

				if (insertSpacesOnTabs && character == '\t')
				{
					auto spaces = ((back().size() / tabSize) + 1) * tabSize - back().size();

					for (size_t s = 0; s < spaces; s++)
					{
						back().emplace_back(Glyph(' ', Color::text));
					}

				}
				else if (character != '\r')
				{
					back().emplace_back(Glyph(character, Color::text));
				}
			}
		}

	}
	else
	{
		appendLine();
	}

	// update maximum column counts
	updateMaximumColumn(0, lineCount() - 1);

}


//
//	TextEditor::Document::insertText
//

TextEditor::Coordinate TextEditor::Document::insertText(Coordinate start, const std::string_view& text)
{
	auto line = begin() + start.line;
	auto index = getIndex(start);
	auto lineNo = start.line;

	// process input as UTF-8
	auto endOfText = text.end();
	auto i = text.begin();

	// process all codepoints
	while (i < endOfText)
	{
		ImWchar character;
		i = CodePoint::read(i, endOfText, &character);

		if (character == '\n')
		{
			// split line
			insertLine(lineNo + 1);
			line = begin() + lineNo;
			auto nextLine = begin() + ++lineNo;

			for (auto j = line->begin() + index; j < line->end(); j++)
			{
				nextLine->push_back(*j);
			}

			line->erase(line->begin() + index, line->end());
			line = nextLine;
			index = 0;

		}
		else if (insertSpacesOnTabs && character == '\t')
		{
			auto spaces = ((index / tabSize) + 1) * tabSize - index;

			for (size_t s = 0; s < spaces; s++)
			{
				line->insert(line->begin() + (index++), Glyph(' ', Color::text));
			}

		}
		else if (character != '\r')
		{
			// insert next glyph
			line->insert(line->begin() + (index++), Glyph(character, Color::text));
		}
	}

	// determine end of insert
	auto end = Coordinate(lineNo, getColumn(static_cast<int>(line - begin()), index));

	// mark affected lines for colorization
	for (auto j = start.line; j <= end.line; j++)
	{
		at(j).colorize = true;
	}

	// update maximum column counts
	updateMaximumColumn(start.line, end.line);

	updated = true;
	return end;
}


//
//	TextEditor::Document::deleteText
//

void TextEditor::Document::deleteText(Coordinate start, Coordinate end)
{
	auto& startLine = at(start.line);
	auto& endLine = at(end.line);
	auto startIndex = getIndex(start);
	auto endIndex = getIndex(end);

	// see if start and end are on the same line
	if (start.line == end.line)
	{
		startLine.erase(startLine.begin() + startIndex, startLine.begin() + endIndex);

		// start and end are on different lines
	}
	else
	{
		// remove end of first line
		startLine.erase(startLine.begin() + startIndex, startLine.end());

		// remove start of last line
		endLine.erase(endLine.begin(), endLine.begin() + endIndex);

		// join lines
		startLine.insert(startLine.end(), endLine.begin(), endLine.end());

		// delete lines
		deleteLines(start.line + 1, end.line);
	}

	// remove marker
	startLine.marker = 0;

	// mark line and document as changed
	at(start.line).colorize = true;
	updated = true;

	// update maximum column counts
	updateMaximumColumn(start.line, start.line);
}


//
//	TextEditor::Document::getText
//

std::string TextEditor::Document::getText() const
{
	// process all glyphs and generate UTF-8 output
	std::string text;
	char utf8[4];

	for (auto line = begin(); line < end(); line++)
	{
		for (auto glyph = line->begin(); glyph < line->end(); glyph++)
		{
			text.append(std::string_view(utf8, CodePoint::write(utf8, glyph->codepoint)));
		}

		if (line < end() - 1)
		{
			text += "\n";
		}
	}

	return text;
}


//
//	TextEditor::Document::getLineText
//

std::string TextEditor::Document::getLineText(int line) const
{
	return getSectionText(Coordinate(line, 0), Coordinate(line, at(line).maxColumn));
}


//
//	TextEditor::Document::getSectionText
//

std::string TextEditor::Document::getSectionText(Coordinate start, Coordinate end) const
{
	std::string section;

	auto lineNo = start.line;
	auto index = getIndex(start);
	auto endIndex = getIndex(end);
	char utf8[4];

	while (lineNo < end.line || index < endIndex)
	{
		auto& line = at(lineNo);

		if (index < line.size())
		{
			section.append(std::string_view(utf8, CodePoint::write(utf8, line[index].codepoint)));
			index++;

		}
		else
		{
			section += '\n';
			lineNo++;
			index = 0;
		}
	}

	return section;
}


//
//	TextEditor::Document::getCodePoint
//

ImWchar TextEditor::Document::getCodePoint(Coordinate location) const
{
	auto index = getIndex(location);

	if (index < at(location.line).size())
	{
		return at(location.line)[index].codepoint;

	}
	else
	{
		return IM_UNICODE_CODEPOINT_INVALID;
	}
}

//
//	TextEditor::Document::getColor
//

TextEditor::Color TextEditor::Document::getColor(Coordinate location)  const
{
	auto index = getIndex(location);

	if (index < at(location.line).size())
	{
		return at(location.line)[index].color;

	}
	else
	{
		return Color::text;
	}
}


//
//	TextEditor::Document::updateMaximumColumn
//

void TextEditor::Document::updateMaximumColumn(int first, int last)
{
	// process specified lines
	for (auto line = begin() + first; line <= begin() + last; line++)
	{
		// determine the maximum column number for this line
		int column = 0;

		for (auto glyph = line->begin(); glyph < line->end(); glyph++)
		{
			column = (glyph->codepoint == '\t') ? ((column / tabSize) + 1) * tabSize : column + 1;
		}

		line->maxColumn = column;
	}

	// determine maximum column number in document
	maxColumn = 0;

	for (auto line = begin(); line < end(); line++)
	{
		maxColumn = std::max(maxColumn, line->maxColumn);
	}
}


//
//	TextEditor::Document::getIndex
//

size_t TextEditor::Document::getIndex(const Line& line, int column) const
{
	// convert a column reference to a glyph index for a specified line (taking tabs into account)
	auto end = line.end();
	size_t index = 0;
	auto leftCol = 0;
	auto rightCol = 0;

	for (auto glyph = line.begin(); rightCol < column && glyph < end; glyph++)
	{
		leftCol = rightCol;
		rightCol = (glyph->codepoint == '\t') ? ((rightCol / tabSize) + 1) * tabSize : rightCol + 1;
		index++;
	}

	if (rightCol - leftCol <= 1)
	{
		return index;

	}
	else
	{
		auto leftDiff = column - leftCol;
		auto rightDiff = rightCol - column;
		return leftDiff <= rightDiff ? index - 1 : index;
	}
}


//
//	TextEditor::Document::getColumn
//

int TextEditor::Document::getColumn(const Line& line, size_t index) const
{
	// convert a glyph index to a column reference for the specified line (taking tabs into account)
	auto end = line.begin() + index;
	int column = 0;

	for (auto glyph = line.begin(); glyph < end; glyph++)
	{
		column = (glyph->codepoint == '\t') ? ((column / tabSize) + 1) * tabSize : column + 1;
	}

	return column;
}


//
//	TextEditor::Document::getUp
//

TextEditor::Coordinate TextEditor::Document::getUp(Coordinate from, int lines) const
{
	return normalizeCoordinate(Coordinate(from.line - lines, from.column));
}


//
//	TextEditor::Document::getDown
//

TextEditor::Coordinate TextEditor::Document::getDown(Coordinate from, int lines) const
{
	return normalizeCoordinate(Coordinate(from.line + lines, from.column));
}


//
//	TextEditor::Document::getLeft
//

TextEditor::Coordinate TextEditor::Document::getLeft(Coordinate from, bool wordMode) const
{
	if (wordMode)
	{
		// first move left by one glyph
		from = getLeft(from);

		// now skip all whitespaces
		from = findPreviousNonWhiteSpace(from, false);

		// find the start of the current word
		return findWordStart(from);

	}
	else
	{
		// calculate coordinate of previous glyph (could be on previous line)
		auto index = getIndex(from);

		if (index == 0)
		{
			return (from.line > 0) ? Coordinate(from.line - 1, at(from.line - 1).maxColumn) : from;

		}
		else
		{
			return Coordinate(from.line, getColumn(from.line, index - 1));
		}
	}
}


//
//	TextEditor::Document::getRight
//

TextEditor::Coordinate TextEditor::Document::getRight(Coordinate from, int wordMode) const
{
	if (wordMode == 1)
	{
		// first move right by one glyph
		from = getRight(from);

		// now skip all whitespaces
		from = findNextNonWhiteSpace(from, false);

		// find the end of the current word
		auto index = getIndex(from);
		return findWordEnd(Coordinate(from.line, getColumn(from.line, index)));

	}
	else
	{
		// calculate coordinate of next glyph (could be on next line)
		auto index = getIndex(from);

		if (index == at(from.line).size())
		{
			return (from.line < lineCount() - 1) ? Coordinate(from.line + 1, 0) : from;

		}
		else
		{
			if (wordMode == 2)
			{

			}
			return Coordinate(from.line, getColumn(from.line, index + 1));
		}
	}
}


//
//	TextEditor::Document::getTop
//

TextEditor::Coordinate TextEditor::Document::getTop() const
{
	return Coordinate(0, 0);
}


//
//	TextEditor::Document::getBottom
//

TextEditor::Coordinate TextEditor::Document::getBottom() const
{
	auto lastLine = lineCount() - 1;
	return Coordinate(lastLine, at(lastLine).maxColumn);
}


//
//	TextEditor::Document::getStartOfLine
//

TextEditor::Coordinate TextEditor::Document::getStartOfLine(Coordinate from) const
{
	return Coordinate(from.line, 0);
}


//
//	TextEditor::Document::getEndOfLine
//

TextEditor::Coordinate TextEditor::Document::getEndOfLine(Coordinate from) const
{
	return Coordinate(from.line, at(from.line).maxColumn);
}


//
//	TextEditor::Document::findWordStart
//

TextEditor::Coordinate TextEditor::Document::findWordStart(Coordinate from, bool wordOnly) const
{
	auto& line = at(from.line);
	auto lineSize = line.size();

	if (from.column == 0 || lineSize == 0)
	{
		return from;

	}
	else
	{
		auto index = getIndex(from);
		auto firstCharacter = line[index - 1].codepoint;

		if (!wordOnly && CodePoint::isWhiteSpace(firstCharacter))
		{
			while (index > 0 && CodePoint::isWhiteSpace(line[index - 1].codepoint))
			{
				index--;
			}

		}
		else if (CodePoint::isWord(firstCharacter))
		{
			while (index > 0 && CodePoint::isWord(line[index - 1].codepoint))
			{
				index--;
			}

		}
		else
		{
			while (!wordOnly && index > 0 && !CodePoint::isWord(line[index - 1].codepoint) && !CodePoint::isWhiteSpace(line[index - 1].codepoint))
			{
				index--;
			}
		}

		return Coordinate(from.line, getColumn(line, index));
	}
}

//
//	TextEditor::Document::findSubWordLeft
//

TextEditor::Coordinate TextEditor::Document::findSubWordLeft(Coordinate from) const
{
	// A subword boundary is either:
	//   - a transition from a lowercase letter to an uppercase letter (camelCase),
	//   - a run of separators (snake_case underscores, dashes, etc.), or
	//   - the start of a word after whitespace / non-word characters.
	if (from.line < 0 || from.line >= lineCount()) return from;
	auto& line = at(from.line);
	auto index = getIndex(from);

	if (index == 0)
	{
		// at column 0: wrap to end of previous line
		if (from.line > 0)
		{
			return Coordinate(from.line - 1, at(from.line - 1).maxColumn);
		}
		return from;
	}

	auto ch = line[index - 1].codepoint;

	// Skip a run of whitespace
	if (CodePoint::isWhiteSpace(ch))
	{
		while (index > 0 && CodePoint::isWhiteSpace(line[index - 1].codepoint)) --index;
		return Coordinate(from.line, getColumn(line, index));
	}

	// Skip a run of separators (e.g. multiple underscores)
	if (CodePoint::isSeparator(ch))
	{
		while (index > 0 && CodePoint::isSeparator(line[index - 1].codepoint)) --index;
		return Coordinate(from.line, getColumn(line, index));
	}

	// Word characters: walk back, but stop at a separator boundary or a
	// lowercase→UPPERCASE transition (camelCase boundary).
	if (CodePoint::isWord(ch))
	{
		--index;
		while (index > 0)
		{
			auto prev = line[index - 1].codepoint;
			auto curr = line[index].codepoint;
			if (!CodePoint::isWord(prev))         break;
			if (CodePoint::isSeparator(prev))     break;
			if (CodePoint::isLowerToUpper(prev, curr)) break;
			--index;
		}
		return Coordinate(from.line, getColumn(line, index));
	}

	// Otherwise just step back one glyph
	return Coordinate(from.line, getColumn(line, index - 1));
}


//
//	TextEditor::Document::findSubWordRight
//

TextEditor::Coordinate TextEditor::Document::findSubWordRight(Coordinate from) const
{
	if (from.line < 0 || from.line >= lineCount()) return from;
	auto& line = at(from.line);
	auto index = getIndex(from);
	auto size = line.size();

	if (index >= size)
	{
		if (from.line < lineCount() - 1) return Coordinate(from.line + 1, 0);
		return from;
	}

	auto ch = line[index].codepoint;

	if (CodePoint::isWhiteSpace(ch))
	{
		while (index < size && CodePoint::isWhiteSpace(line[index].codepoint)) ++index;
		return Coordinate(from.line, getColumn(line, index));
	}

	if (CodePoint::isSeparator(ch))
	{
		while (index < size && CodePoint::isSeparator(line[index].codepoint)) ++index;
		return Coordinate(from.line, getColumn(line, index));
	}

	if (CodePoint::isWord(ch))
	{
		++index;
		while (index < size)
		{
			auto prev = line[index - 1].codepoint;
			auto curr = line[index].codepoint;
			if (!CodePoint::isWord(curr))             break;
			if (CodePoint::isSeparator(curr))         break;
			if (CodePoint::isLowerToUpper(prev, curr)) break;
			++index;
		}
		return Coordinate(from.line, getColumn(line, index));
	}

	return Coordinate(from.line, getColumn(line, index + 1));
}

//
//	TextEditor::Document::findWordEnd
//

TextEditor::Coordinate TextEditor::Document::findWordEnd(Coordinate from, bool wordOnly) const
{
	auto& line = at(from.line);
	auto index = getIndex(from);
	auto size = line.size();

	if (index >= size)
	{
		return from;

	}
	else
	{
		auto firstCharacter = line[index].codepoint;

		if (!wordOnly && CodePoint::isWhiteSpace(firstCharacter))
		{
			while (index < size && CodePoint::isWhiteSpace(line[index].codepoint))
			{
				index++;
			}

		}
		else if (CodePoint::isWord(firstCharacter))
		{
			while (index < size && CodePoint::isWord(line[index].codepoint))
			{
				index++;
			}

		}
		else
		{
			while (!wordOnly && index < size && !CodePoint::isWord(line[index].codepoint) && !CodePoint::isWhiteSpace(line[index].codepoint))
			{
				index++;
			}
		}
	}

	return Coordinate(from.line, getColumn(line, index));
}


//
//	TextEditor::Document::findText
//

bool TextEditor::Document::findText(Coordinate from, const std::string_view& text, bool caseSensitive, bool wholeWord, Coordinate& start, Coordinate& end) const
{
	// convert input string to vector of codepoints
	std::vector<ImWchar> search;
	auto endOfText = text.end();
	auto i = text.begin();

	while (i < endOfText)
	{
		ImWchar character;
		i = CodePoint::read(i, endOfText, &character);
		search.emplace_back(caseSensitive ? character : CodePoint::toLower(character));
	}

	// search document
	auto startLine = from.line;
	auto startIndex = getIndex(from);
	auto searchLine = startLine;
	auto searchIndex = startIndex;

	do
	{
		auto line = searchLine;
		auto index = searchIndex;
		auto lineSize = at(line).size();
		bool done = false;
		size_t j = 0;

		while (!done && j < search.size())
		{
			if (search[j] == '\n')
			{
				if (index == lineSize)
				{
					if (line == lineCount() - 1)
					{
						done = true;

					}
					else
					{
						line++;
						index = 0;
						lineSize = at(line).size();
						j++;
					}

				}
				else
				{
					done = true;
				}

			}
			else
			{
				if (index == lineSize)
				{
					done = true;

				}
				else
				{
					auto ch = at(line)[index].codepoint;

					if (!caseSensitive)
					{
						ch = CodePoint::toLower(ch);
					}

					if (ch == search[j])
					{
						index++;
						j++;

					}
					else
					{
						done = true;
					}
				}
			}
		}

		if (j == search.size())
		{
			start = Coordinate(searchLine, getColumn(searchLine, searchIndex));
			end = Coordinate(line, getColumn(line, index));

			if (!wholeWord || isWholeWord(start, end))
			{
				return true;
			}
		}

		if (searchIndex == at(searchLine).size())
		{
			searchLine = (searchLine == lineCount() - 1) ? 0 : searchLine + 1;
			searchIndex = 0;

		}
		else
		{
			searchIndex++;
		}

	} while (searchLine != startLine || searchIndex != startIndex);

	return false;
}


//
//	TextEditor::Document::setUserData
//

void TextEditor::Document::setUserData(int line, void* data)
{
	if (line >= 0 && line < lineCount())
	{
		at(static_cast<size_t>(line)).userData = data;
	}
}


//
//	TextEditor::Document::getUserData
//

void* TextEditor::Document::getUserData(int line) const
{
	if (line >= 0 && line < lineCount())
	{
		return at(static_cast<size_t>(line)).userData;

	}
	else
	{
		return nullptr;
	}
}

//
//	TextEditor::Document::iterateUserData
//

void TextEditor::Document::iterateUserData(std::function<void(int line, void* data)> callback) const
{
	for (size_t i = 0; i < size(); i++)
	{
		callback(static_cast<int>(i), at(i).userData);
	}
}


//
//	TextEditor::Document::iterateIdentifiers
//

static inline bool isIdentifier(TextEditor::Color color)
{
	return
		color == TextEditor::Color::identifier ||
		color == TextEditor::Color::knownIdentifier;
}

void TextEditor::Document::iterateIdentifiers(std::function<void(const std::string&)> callback) const
{
	for (size_t i = 0; i < size(); i++)
	{
		auto p = at(i).begin();
		auto end = at(i).end();
		char utf8[4];

		while (p < end)
		{
			if (isIdentifier(p->color))
			{
				std::string identifier;

				while (p < end && isIdentifier(p->color))
				{
					identifier.append(std::string_view(utf8, CodePoint::write(utf8, p->codepoint)));
					p++;
				}

				callback(identifier);

			}
			else
			{
				p++;
			}
		}
	}
}


//
//	TextEditor::Document::isWholeWord
//

bool TextEditor::Document::isWholeWord(Coordinate start, Coordinate end) const
{
	if (start.line != end.line || end.column - start.column < 1)
	{
		return false;

	}
	else
	{
		auto wordStart = findWordStart(Coordinate(start.line, start.column + 1));
		auto wordEnd = findWordEnd(Coordinate(end.line, end.column - 1));
		return start == wordStart && end == wordEnd;
	}
}


//
//	TextEditor::Document::findPreviousNonWhiteSpace
//

TextEditor::Coordinate TextEditor::Document::findPreviousNonWhiteSpace(Coordinate from, bool includeEndOfLine) const
{
	bool done = false;

	while (!done)
	{
		auto& line = at(from.line);
		auto index = getIndex(from);

		while (!done && index > 0)
		{
			index--;

			if (!CodePoint::isWhiteSpace(line[index].codepoint))
			{
				from.column = getColumn(line, index);
				done = true;
			}
		}

		if (!done)
		{
			if (from.line == 0 || !includeEndOfLine)
			{
				from.column = 0;
				done = true;

			}
			else
			{
				from.line--;
				from.column = at(from.line).maxColumn;
			}
		}
	}

	return from;
}


//
//	TextEditor::Document::findNextNonWhiteSpace
//

TextEditor::Coordinate TextEditor::Document::findNextNonWhiteSpace(Coordinate from, bool includeEndOfLine) const
{
	bool done = false;

	while (!done)
	{
		auto& line = at(from.line);
		auto index = getIndex(from);

		while (!done && index < line.size())
		{
			if (CodePoint::isWhiteSpace(line[index].codepoint))
			{
				index++;

			}
			else
			{
				from.column = getColumn(line, index);
				done = true;
			}
		}

		if (!done)
		{
			if (from.line == lineCount() || !includeEndOfLine)
			{
				from.column = line.maxColumn;
				done = true;

			}
			else
			{
				from.line++;
				from.column = 0;
			}
		}
	}

	return from;
}


//
//	TextEditor::Document::normalizeCoordinate
//

TextEditor::Coordinate TextEditor::Document::normalizeCoordinate(Coordinate coordinate) const
{
	if (coordinate.line < 0)
	{
		return Coordinate(0, 0);

	}
	else if (coordinate.line >= lineCount())
	{
		return Coordinate(lineCount() - 1, at(size() - 1).maxColumn);

	}
	else if (coordinate.column < 0)
	{
		return Coordinate(coordinate.line, 0);

	}
	else if (coordinate.column > at(coordinate.line).maxColumn)
	{
		return Coordinate(coordinate.line, at(coordinate.line).maxColumn);

	}
	else
	{
		// determine column numbers left and right of provided coordinate
		auto& line = at(coordinate.line);
		auto end = line.end();
		auto leftCol = 0;
		auto rightCol = 0;

		for (auto glyph = line.begin(); rightCol < coordinate.column && glyph < end; glyph++)
		{
			leftCol = rightCol;
			rightCol = (glyph->codepoint == '\t') ? ((rightCol / tabSize) + 1) * tabSize : rightCol + 1;
		}

		auto leftDiff = coordinate.column - leftCol;
		auto rightDiff = rightCol - coordinate.column;
		return Coordinate(coordinate.line, leftDiff <= rightDiff ? leftCol : rightCol);
	}
}


//
//	TextEditor::Document::normalizeCoordinate
//

void TextEditor::Document::normalizeCoordinate(float line, float column, Coordinate& glyphCoordinate, Coordinate& cursorCoordinate) const
{
	// normalize coordinates by clamping them to the document and line range
	// the returned glyphCoordinate addresses the glyph pointed to by the line and column parameters
	// the returned cursorCoordinate returns the closest cursor position (which can be at the start or the end of the glyph)
	if (line < 0.0f)
	{
		glyphCoordinate = Coordinate(0, 0);
		cursorCoordinate = glyphCoordinate;

	}
	else if (line >= static_cast<float>(lineCount()))
	{
		glyphCoordinate = Coordinate(lineCount() - 1, at(lineCount() - 1).maxColumn);
		cursorCoordinate = glyphCoordinate;

	}
	else
	{
		auto lineNo = static_cast<int>(line);

		if (column < 0.0f)
		{
			glyphCoordinate = Coordinate(lineNo, 0);
			cursorCoordinate = glyphCoordinate;

		}
		else if (column >= static_cast<float>(at(lineNo).maxColumn))
		{
			glyphCoordinate = Coordinate(lineNo, at(lineNo).maxColumn);
			cursorCoordinate = glyphCoordinate;

		}
		else
		{
			// determine column numbers left and right of provided coordinate
			auto leftCol = 0;
			auto rightCol = 0;
			auto end = at(lineNo).end();

			for (auto glyph = at(lineNo).begin(); rightCol < column && glyph < end; glyph++)
			{
				leftCol = rightCol;
				rightCol = (glyph->codepoint == '\t') ? ((rightCol / tabSize) + 1) * tabSize : rightCol + 1;
			}

			auto leftDiff = column - static_cast<float>(leftCol);
			auto rightDiff = static_cast<float>(rightCol) - column;

			glyphCoordinate = Coordinate(lineNo, leftCol);
			cursorCoordinate = Coordinate(lineNo, leftDiff <= rightDiff ? leftCol : rightCol);
		}
	}
}


//
//	TextEditor::Document::appendLine
//

void TextEditor::Document::appendLine()
{
	auto& line = emplace_back();

	if (insertor)
	{
		line.userData = insertor(static_cast<int>(size() - 1));
	}
}


//
//	TextEditor::Document::insertLine
//

void TextEditor::Document::insertLine(int offsset)
{
	auto line = insert(begin() + offsset, Line());

	if (insertor)
	{
		line->userData = insertor(offsset);
	}
}


//
//	TextEditor::Document::deleteLines
//

void TextEditor::Document::deleteLines(int start, int end)
{
	if (deletor)
	{
		for (auto i = start; i <= end; i++)
		{
			deletor(i, at(i).userData);
		}
	}

	erase(begin() + start, begin() + end + 1);
}


//
//	TextEditor::Document::clearDocument
//

void TextEditor::Document::clearDocument()
{
	if (deletor)
	{
		for (auto i = 0; i <= lineCount(); i++)
		{
			deletor(i, at(i).userData);
		}
	}

	clear();
}


//
//	TextEditor::Transactions::reset
//

void TextEditor::Transactions::reset()
{
	clear();
	undoIndex = 0;
	version = 0;
}


//
//	TextEditor::Transactions::add
//

void TextEditor::Transactions::add(std::shared_ptr<Transaction> transaction)
{
	resize(undoIndex);
	push_back(transaction);
	undoIndex++;
	version++;
}


//
//	TextEditor::Transactions::undo
//

void TextEditor::Transactions::undo(Document& document, Cursors& cursors)
{
	auto transaction = at(--undoIndex);

	for (auto action = transaction->rbegin(); action < transaction->rend(); action++)
	{
		if (action->type == Action::Type::insertText)
		{
			document.deleteText(action->start, action->end);

		}
		else
		{
			document.insertText(action->start, action->text);
		}
	}

	cursors = transaction->getBeforeState();
	version++;
}


//
//	TextEditor::Transactions::redo
//

void TextEditor::Transactions::redo(Document& document, Cursors& cursors)
{
	auto transaction = at(undoIndex++);

	for (auto action = transaction->begin(); action < transaction->end(); action++)
	{
		if (action->type == Action::Type::insertText)
		{
			document.insertText(action->start, action->text);

		}
		else
		{
			document.deleteText(action->start, action->end);
		}
	}

	cursors = transaction->getAfterState();
	version++;
}


//
//	TextEditor::Colorizer::update
//

TextEditor::State TextEditor::Colorizer::update(Line& line, const Language* language)
{
	auto state = line.state;

	// A line whose last non-whitespace glyph is a backslash continues onto the
	// next line (C/C++ line splicing) — used to carry a #define etc. across lines.
	auto lineEndsInBackslash = [](Line& ln) -> bool {
		for (auto it = ln.end(); it != ln.begin(); ) {
			--it;
			if (CodePoint::isWhiteSpace(it->codepoint)) continue;
			return it->codepoint == static_cast<ImWchar>('\\');
		}
		return false;
	};

	// process all glyphs on this line
	auto nonWhiteSpace = false;
	auto glyph = line.begin();

	while (glyph < line.end())
	{
		if (state == State::inText)
		{
			// special handling for preprocessor lines
			if (!nonWhiteSpace && language->preprocess && glyph->codepoint != language->preprocess && !CodePoint::isWhiteSpace(glyph->codepoint))
			{
				nonWhiteSpace = true;
			}

			// start parsing glyphs
			auto start = glyph;

			// mark whitespace characters
			if (CodePoint::isWhiteSpace(glyph->codepoint))
			{
				(glyph++)->color = Color::whitespace;

				// Block comment must be tried BEFORE the single-line comment when its
			// opener begins with the single-line marker (Lua: "--[[" starts with
			// "--"), otherwise "--[[" is mis-read as a "--" line comment and the
			// block never opens.
			}
			else if (language->commentStart.size()
				&& language->singleLineComment.size()
				&& language->commentStart.rfind(language->singleLineComment, 0) == 0
				&& matches(glyph, line.end(), language->commentStart))
			{
				state = State::inComment;
				auto size = language->commentStart.size();
				setColor(glyph, glyph + size, Color::comment);
				glyph += size;

				// handle single line comments
			}
			else if (language->singleLineComment.size() && matches(glyph, line.end(), language->singleLineComment))
			{
				setColor(glyph, line.end(), Color::comment);
				glyph = line.end();

			}
			else if (language->singleLineCommentAlt.size() && matches(glyph, line.end(), language->singleLineCommentAlt))
			{
				setColor(glyph, line.end(), Color::comment);
				glyph = line.end();

				// are we starting a multiline comment
			}
			else if (language->commentStart.size() && matches(glyph, line.end(), language->commentStart))
			{
				state = State::inComment;
				auto size = language->commentStart.size();
				setColor(glyph, glyph + size, Color::comment);
				glyph += size;

				// are we starting a special string
			}
			else if (language->otherStringStart.size() && matches(glyph, line.end(), language->otherStringStart))
			{
				state = State::inOtherString;
				auto size = language->otherStringStart.size();
				setColor(glyph, glyph + size, Color::string);
				glyph += size;

			}
			else if (language->otherStringAltStart.size() && matches(glyph, line.end(), language->otherStringAltStart))
			{
				state = State::inOtherStringAlt;
				auto size = language->otherStringAltStart.size();
				setColor(glyph, glyph + size, Color::string);
				glyph += size;

				// are we starting a single quoted string
			}
			else if (language->hasSingleQuotedStrings && glyph->codepoint == CodePoint::singleQuote)
			{
				state = State::inSingleQuotedString;
				(glyph++)->color = Color::string;

				// are we starting a double quoted string
			}
			else if (language->hasDoubleQuotedStrings && glyph->codepoint == CodePoint::doubleQuote)
			{
				state = State::inDoubleQuotedString;
				(glyph++)->color = Color::string;

				// is this a preprocessor line
			}
			else if (language->preprocess && !nonWhiteSpace && glyph->codepoint == language->preprocess)
			{
				setColor(line.begin(), line.end(), Color::preprocessor);
				glyph = line.end();
				// A trailing '\' splices the next line onto this directive, but we
				// deliberately do NOT carry the flat preprocessor color across it:
				// multi-line macro BODIES (e.g. a big #define) should be syntax-
				// highlighted as code, not blobbed one color. So the continuation
				// lines fall through to normal tokenizing (state stays inText).

				// handle custom tokenizer (if we have one)
			}
			else if (language->customTokenizer)
			{
				Color color;
				Iterator tokenStart(&*glyph);
				Iterator lineEnd(line.data() + line.size());
				Iterator tokenEnd = language->customTokenizer(tokenStart, lineEnd, color);

				if (tokenEnd != tokenStart)
				{
					auto size = tokenEnd - tokenStart;
					setColor(glyph, glyph + size, color);
					glyph += size;
				}
			}

			if (glyph == start)
			{
				// nothing worked so far so it's time to do some tokenizing
				Color color;
				Iterator lineEnd(line.data() + line.size());
				Iterator tokenStart(&*glyph);
				Iterator tokenEnd;

				// do we have an identifier
				if (language->getIdentifier && (tokenEnd = language->getIdentifier(tokenStart, lineEnd)) != tokenStart)
				{
					// determine identifier text and color color
					auto size = tokenEnd - tokenStart;
					std::string identifier;
					color = Color::identifier;

					for (auto i = tokenStart; i < tokenEnd; i++)
					{
						ImWchar codepoint = *i;

						if (!language->caseSensitive)
						{
							codepoint = CodePoint::toLower(codepoint);
						}

						char utf8[4];
						identifier.append(utf8, CodePoint::write(utf8, codepoint));
					}

					if (language->keywords.find(identifier) != language->keywords.end())
					{
						color = Color::keyword;

					}
					else if (language->declarations.find(identifier) != language->declarations.end())
					{
						color = Color::declaration;

					}
					else if (language->identifiers.find(identifier) != language->identifiers.end())
					{
						color = Color::knownIdentifier;
					}
					else if (language->isTypeLike && language->isTypeLike(identifier))
					{
						// Convention-based type (e.g. an unlisted Unreal F*/U*/A*/T* type).
						color = Color::declaration;
					}

					// colorize identifier and move on
					setColor(glyph, glyph + size, color);
					glyph += size;

					// do we have a number
				}
				else if (language->getNumber && (tokenEnd = language->getNumber(tokenStart, lineEnd)) != tokenStart)
				{
					auto size = tokenEnd - tokenStart;
					setColor(glyph, glyph + size, Color::number);
					glyph += size;

					// is this punctuation
				}
				else if (language->isPunctuation && language->isPunctuation(glyph->codepoint))
				{
					(glyph++)->color = Color::punctuation;

				}
				else
				{
					// I guess we don't know what this character is
					(glyph++)->color = Color::text;
				}
			}

		}
		else if (state == State::inComment)
		{
			// stay in comment state until we see the end sequence
			if (matches(glyph, line.end(), language->commentEnd))
			{
				auto size = language->commentEnd.size();
				setColor(glyph, glyph + size, Color::comment);
				glyph += size;
				state = State::inText;

			}
			else
			{
				(glyph++)->color = Color::comment;
			}

		}
		else if (state == State::inPreprocessor)
		{
			// Continuation line of a backslash-continued directive: the whole line
			// belongs to the directive. Stay here while lines keep ending in '\',
			// then fall back to normal text.
			setColor(line.begin(), line.end(), Color::preprocessor);
			glyph = line.end();
			if (!lineEndsInBackslash(line)) state = State::inText;
		}
		else if (state == State::inOtherString)
		{
			// stay in otherString state until we see the end sequence
			// skip escaped characters
			if (glyph->codepoint == language->stringEscape)
			{
				(glyph++)->color = Color::string;

				if (glyph < line.end())
				{
					(glyph++)->color = Color::string;
				}

			}
			else if (matches(glyph, line.end(), language->otherStringEnd))
			{
				auto size = language->otherStringEnd.size();
				setColor(glyph, glyph + size, Color::string);
				glyph += size;
				state = State::inText;

			}
			else
			{
				(glyph++)->color = Color::comment;
			}

		}
		else if (state == State::inOtherStringAlt)
		{
			// stay in otherStringAlt state until we see the end sequence
			// skip escaped characters
			if (glyph->codepoint == language->stringEscape)
			{
				(glyph++)->color = Color::string;

				if (glyph < line.end())
				{
					(glyph++)->color = Color::string;
				}

			}
			else if (matches(glyph, line.end(), language->otherStringAltEnd))
			{
				auto size = language->otherStringAltEnd.size();
				setColor(glyph, glyph + size, Color::string);
				glyph += size;
				state = State::inText;

			}
			else
			{
				(glyph++)->color = Color::comment;
			}

		}
		else if (state == State::inSingleQuotedString)
		{
			// stay in single quote state until we see an end
			// skip escaped characters
			if (glyph->codepoint == language->stringEscape)
			{
				(glyph++)->color = Color::string;

				if (glyph < line.end())
				{
					(glyph++)->color = Color::string;
				}

			}
			else if (glyph->codepoint == CodePoint::singleQuote)
			{
				(glyph++)->color = Color::string;
				state = State::inText;

			}
			else
			{
				(glyph++)->color = Color::string;
			}

		}
		else if (state == State::inDoubleQuotedString)
		{
			// stay in double quote state until we see an end
			// skip escaped characters
			if (glyph->codepoint == language->stringEscape)
			{
				(glyph++)->color = Color::string;

				if (glyph < line.end())
				{
					(glyph++)->color = Color::string;
				}

			}
			else if (glyph->codepoint == CodePoint::doubleQuote)
			{
				(glyph++)->color = Color::string;
				state = State::inText;

			}
			else
			{
				(glyph++)->color = Color::string;
			}
		}
	}

	line.colorize = false;
	return state;
}


//
//	TextEditor::Colorizer::updateEntireDocument
//

void TextEditor::Colorizer::updateEntireDocument(Document& document, const Language* language)
{
	if (language)
	{
		for (auto line = document.begin(); line < document.end(); line++)
		{
			auto state = update(*line, language);
			auto next = line + 1;

			if (next < document.end())
			{
				next->state = state;
			}
		}

	}
	else
	{
		for (auto line = document.begin(); line < document.end(); line++)
		{
			for (auto glyph = line->begin(); glyph < line->end(); glyph++)
			{
				glyph->color = Color::text;
			}

			line->state = State::inText;
			line->colorize = false;
		}
	}
}

int TextEditor::screenYToDocumentLine(float y) const
{
	ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
	int vi = static_cast<int>(std::floor((y - cursorScreenPos.y) / glyphSize.y));
	if (vi < 0) return 0;

	if (visibleLineMap.empty())
	{
		return std::clamp(vi, 0, document.lineCount() - 1);
	}
	else
	{
		int maxIndex = static_cast<int>(visibleLineMap.size()) - 1;
		vi = std::clamp(vi, 0, maxIndex);
		return visibleLineMap[vi];
	}
}

//
//	TextEditor::Colorizer::updateChangedLines
//

void TextEditor::Colorizer::updateChangedLines(Document& document, const Language* language)
{
	for (auto line = document.begin(); line < document.end(); line++)
	{
		if (line->colorize)
		{
			auto state = update(*line, language);
			auto next = line + 1;

			if (next < document.end() && next->state != state)
			{
				next->state = state;
				next->colorize = true;
			}
		}
	}
}
const TextEditor::FoldRange* TextEditor::GetFoldRangeStartingAt(int line)
{
	for (auto& fr : foldRanges)
	{
		if (fr.start.line == line)
		{
			return &fr;
		}
	}
	return nullptr;
}


//
//	TextEditor::Colorizer::matches
//

bool TextEditor::Colorizer::matches(Line::iterator start, Line::iterator end, const std::string_view& text)
{
	// see if text at iterators matches provided UTF-8 string
	auto i = text.begin();

	while (i < text.end())
	{
		if (start == end)
		{
			return false;
		}

		ImWchar codepoint;
		i = CodePoint::read(i, text.end(), &codepoint);

		if ((start++)->codepoint != codepoint)
		{
			return false;
		}
	}

	return true;
}


//
//	TextEditor::Bracketeer::reset
//

void TextEditor::Bracketeer::reset()
{
	clear();
}


//
//	TextEditor::Bracketeer::update
//

void TextEditor::Bracketeer::update(Document& document)
{
	Color bracketColors[] = {
		Color::matchingBracketLevel1,
		Color::matchingBracketLevel2,
		Color::matchingBracketLevel3
	};

	reset();
	std::vector<size_t> levels;
	int level = 0;

	// process all the glyphs
	for (int line = 0; line < document.lineCount(); line++)
	{
		for (size_t index = 0; index < document[line].size(); index++)
		{
			auto& glyph = document[line][index];

			// handle a "bracket opener" that is not in a comment, string or preprocessor statement
			if (isBracketCandidate(glyph) && CodePoint::isBracketOpener(glyph.codepoint))
			{
				// start a new level
				levels.emplace_back(size());
				emplace_back(glyph.codepoint, Coordinate(line, document.getColumn(line, index)), static_cast<ImWchar>(0), Coordinate::invalid(), level);
				glyph.color = bracketColors[level % 3];
				level++;

				// handle a "bracket closer" that is not in a comment, string or preprocessor statement
			}
			else if (isBracketCandidate(glyph) && CodePoint::isBracketCloser(glyph.codepoint))
			{
				if (levels.size())
				{
					auto& lastBracket = at(levels.back());
					levels.pop_back();
					level--;

					if (lastBracket.startChar == CodePoint::toPairOpener(glyph.codepoint))
					{
						// handle matching bracket
						glyph.color = bracketColors[level % 3];
						lastBracket.endChar = glyph.codepoint;
						lastBracket.end = Coordinate(line, document.getColumn(line, index));

					}
					else
					{
						// no matching bracket, mark brackets as errors
						glyph.color = Color::matchingBracketError;
						document[lastBracket.start.line][document.getIndex(lastBracket.start)].color = Color::matchingBracketError;
						pop_back();
					}

					// this is a closer without an opener
				}
				else
				{
					glyph.color = Color::matchingBracketError;
				}
			}
		}
	}

	// handle levels left open and mark them as errors
	if (levels.size())
	{
		for (auto i = levels.rbegin(); i < levels.rend(); i++)
		{
			auto& start = at(*i).start;
			document[start.line][document.getIndex(start)].color = Color::matchingBracketError;
			erase(begin() + *i);
		}
	}
}


//
//	TextEditor::Bracketeer::getEnclosingBrackets
//

TextEditor::Bracketeer::iterator TextEditor::Bracketeer::getEnclosingBrackets(Coordinate location)
{
	iterator brackets = end();
	bool done = false;

	for (auto i = begin(); !done && i < end(); i++)
	{
		// brackets are sorted so no need to go past specified location
		if (i->isAfter(location))
		{
			done = true;
		}

		else if (i->isAround(location))
		{
			// this could be what we're looking for
			brackets = i;
		}
	}

	return brackets;
}


//
//	TextEditor::Bracketeer::getEnclosingCurlyBrackets
//

TextEditor::Bracketeer::iterator TextEditor::Bracketeer::getEnclosingCurlyBrackets(Coordinate first, Coordinate last)
{
	iterator brackets = end();
	bool done = false;

	for (auto i = begin(); !done && i < end(); i++)
	{
		// brackets are sorted so no need to go past specified location
		if (i->isAfter(first))
		{
			done = true;
		}

		else if (i->isAround(first) && i->isAround(last) && i->startChar == CodePoint::openCurlyBracket)
		{
			// this could be what we're looking for
			brackets = i;
		}
	}

	return brackets;
}


//
//	TextEditor::Bracketeer::getInnerCurlyBrackets
//

TextEditor::Bracketeer::iterator TextEditor::Bracketeer::getInnerCurlyBrackets(Coordinate first, Coordinate last)
{
	iterator brackets = end();
	auto outer = getEnclosingCurlyBrackets(first, last);

	if (outer != end())
	{
		bool done = false;

		for (auto i = outer + 1; i < end() && !done; i++)
		{
			if (i->level <= outer->level)
			{
				done = true;

			}
			else if (
				i->level == outer->level + 1 &&
				i->startChar == CodePoint::openCurlyBracket &&
				i->start > first &&
				i->end < last)
			{

				brackets = i;
				done = true;
			}
		}
	}

	return brackets;
}


//
//	latchButton
//

static bool latchButton(const char* label, bool* value, const ImVec2& size)
{
	auto changed = false;
	ImVec4* colors = ImGui::GetStyle().Colors;

	if (*value)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, colors[ImGuiCol_ButtonActive]);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[ImGuiCol_ButtonActive]);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors[ImGuiCol_TableBorderLight]);

	}
	else
	{
		ImGui::PushStyleColor(ImGuiCol_Button, colors[ImGuiCol_TableBorderLight]);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[ImGuiCol_TableBorderLight]);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors[ImGuiCol_ButtonActive]);
	}

	ImGui::Button(label, size);

	if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
	{
		*value = !*value;
		changed = true;
	}

	ImGui::PopStyleColor(3);
	return changed;
}


//
//	inputString
//

static bool inputString(const char* label, std::string* value, ImGuiInputTextFlags flags = ImGuiInputTextFlags_None)
{
	flags |=
		ImGuiInputTextFlags_NoUndoRedo |
		ImGuiInputTextFlags_CallbackResize;

	return ImGui::InputText(label, (char*)value->c_str(), value->capacity() + 1, flags, [](ImGuiInputTextCallbackData* data)
							{
								if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
								{
									std::string* value = (std::string*)data->UserData;
									value->resize(data->BufTextLen);
									data->Buf = (char*)value->c_str();
								}

								return 0;
							}, value);
}


//
//	TextEditor::renderFindReplace
//

void TextEditor::recomputeFindMatches()
{
	// Cache key: term + flags + document revision. Recompute only when it
	// changes so we scan the document at most once per edit/term change, not
	// every frame the find bar is open.
	std::string key = findText + "\x1f"
		+ (caseSensitiveFind ? "1" : "0") + (wholeWordFind ? "1" : "0")
		+ "\x1f" + std::to_string(transactions.getUndoIndex());
	if (key == findCountCacheKey) return;
	findCountCacheKey = key;
	findMatchCount = 0;
	findMatchCurrent = 0;
	if (findText.empty()) return;

	Coordinate cursor = cursors.getCurrent().getInteractiveEnd();
	Coordinate from{ 0, 0 }, s, e;
	bool first = true;
	while (document.findText(from, findText, caseSensitiveFind, wholeWordFind, s, e))
	{
		// findText wraps to the top when it runs off the end; stop once a hit
		// comes at/before a prior one (same guard as selectAllOccurrencesOf).
		auto less = [](Coordinate a, Coordinate b){
			return a.line != b.line ? a.line < b.line : a.column < b.column; };
		if (!first && !less(from, e)) break;   // no forward progress → wrapped
		first = false;
		++findMatchCount;
		// The current match is the first one at or after the cursor.
		if (findMatchCurrent == 0 && !less(s, cursor)) findMatchCurrent = findMatchCount;
		from = e;
		if (findMatchCount > 100000) break;   // pathological safety bound
	}
	// Cursor past the last match → treat the last as current (wrap semantics).
	if (findMatchCurrent == 0 && findMatchCount > 0) findMatchCurrent = findMatchCount;
}

void TextEditor::renderFindReplace(ImVec2 pos, float width)
{
	// render find/replace window (if required)
	if (findReplaceVisible)
	{
		// save current screen position
		auto currentScreenPosition = ImGui::GetCursorScreenPos();

		// calculate sizes
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
		auto& style = ImGui::GetStyle();
		auto fieldWidth = 250.0f;

		auto button1Width = ImGui::CalcTextSize(findButtonLabel.c_str()).x + style.ItemSpacing.x * 2.0f;
		auto button2Width = ImGui::CalcTextSize(findAllButtonLabel.c_str()).x + style.ItemSpacing.x * 2.0f;
		auto optionWidth = ImGui::CalcTextSize("Aa").x + style.ItemSpacing.x * 2.0f;

		if (!readOnly)
		{
			button1Width = std::max(button1Width, ImGui::CalcTextSize(replaceButtonLabel.c_str()).x + style.ItemSpacing.x * 2.0f);
			button2Width = std::max(button2Width, ImGui::CalcTextSize(replaceAllButtonLabel.c_str()).x + style.ItemSpacing.x * 2.0f);
		}

		auto windowHeight =
			style.ChildBorderSize * 2.0f +
			style.WindowPadding.y * 2.0f +
			ImGui::GetFrameHeight() +
			(readOnly ? 0.0f : (style.ItemSpacing.y + ImGui::GetFrameHeight()));

		// Reserve room for the "999 of 999" match-count readout between the field
		// and the buttons.
		auto countWidth = ImGui::CalcTextSize("9999 of 9999").x + style.ItemSpacing.x;
		auto windowWidth =
			style.ChildBorderSize * 2.0f +
			style.WindowPadding.x * 2.0f +
			fieldWidth + style.ItemSpacing.x +
			countWidth + style.ItemSpacing.x +
			button1Width + style.ItemSpacing.x +
			button2Width + style.ItemSpacing.x +
			optionWidth * 3.0f + style.ItemSpacing.x * 2.0f;

		// Clamp the widget to the available editor width so a narrow window can't
		// push it off the left edge; absorb the overflow by shrinking the search
		// field (down to a usable floor) rather than overflowing.
		float maxWidth = width - style.ItemSpacing.x * 2.0f;
		if (windowWidth > maxWidth)
		{
			float over = windowWidth - maxWidth;
			fieldWidth = std::max(80.0f, fieldWidth - over);
			windowWidth = std::max(maxWidth, /* hard floor */ 120.0f);
		}

		// create window
		ImGui::SetNextWindowPos(ImVec2(
			pos.x + width - windowWidth - style.ItemSpacing.x,
			pos.y + style.ItemSpacing.y * 2.0f));

		ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight));
		ImGui::SetNextWindowBgAlpha(0.75f);

		ImGui::BeginChild("find-replace", ImVec2(windowWidth, windowHeight), ImGuiChildFlags_Borders);
		ImGui::SetNextItemWidth(fieldWidth);

		if (focusOnFind)
		{
			ImGui::SetKeyboardFocusHere();
			focusOnFind = false;

		}
		else if (findCancelledAutocomplete)
		{
			ImGui::SetKeyboardFocusHere();
			findCancelledAutocomplete = false;
		}

		if (inputString("###find", &findText, ImGuiInputTextFlags_AutoSelectAll))
		{
			if (findText.size())
			{
				selectFirstOccurrenceOf(findText, caseSensitiveFind, wholeWordFind);

			}
			else
			{
				cursors.clearAll();
			}
		}

		if (ImGui::IsItemDeactivated())
		{
			if (ImGui::IsKeyPressed(ImGuiKey_Escape))
			{
				closeFindReplace();
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
			{
				// Enter inside the find textbox advances to the next match (Shift+Enter
				// the previous) and re-focuses the input so the user can keep walking
				// occurrences instead of typing newlines into the editor.
				if (!findText.empty())
				{
					if (ImGui::IsKeyDown(ImGuiMod_Shift)) findPrevious();
					else findNext();
				}
				focusOnFind = true;
			}
		}

		// VSCode-style live match count next to the field: "3 of 12" / "No
		// results". Recomputed only when term/flags/document change.
		recomputeFindMatches();
		ImGui::SameLine();
		if (findText.empty())
			ImGui::TextDisabled("   ");
		else if (findMatchCount == 0)
			ImGui::TextDisabled("No results");
		else
			ImGui::TextDisabled("%d of %d", findMatchCurrent, findMatchCount);

		bool disableFindButtons = !findText.size();

		if (disableFindButtons)
		{
			ImGui::BeginDisabled();
		}

		// Prev / next match step buttons (Shift+Enter / Enter also do this).
		ImGui::SameLine();
		if (ImGui::Button("<##findprev", ImVec2(optionWidth, 0.0f)))
		{
			findPrevious();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous match (Shift+Enter)");
		ImGui::SameLine();
		if (ImGui::Button(">##findnext", ImVec2(optionWidth, 0.0f)))
		{
			findNext();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next match (Enter)");

		ImGui::SameLine();

		if (ImGui::Button(findButtonLabel.c_str(), ImVec2(button1Width, 0.0f)))
		{
			find();
		}

		ImGui::SameLine();

		if (ImGui::Button(findAllButtonLabel.c_str(), ImVec2(button2Width, 0.0f)))
		{
			findAll();
		}

		if (disableFindButtons)
		{
			ImGui::EndDisabled();
		}

		ImGui::SameLine();

		if (latchButton("Aa", &caseSensitiveFind, ImVec2(optionWidth, 0.0f)))
		{
			find();
		}

		ImGui::SameLine();

		if (latchButton("[]", &wholeWordFind, ImVec2(optionWidth, 0.0f)))
		{
			find();
		}

		ImGui::SameLine();

		// "In selection" — captures the current selection as the scope when latched;
		// findAll/replaceAll then only touch matches inside it (VSCode Alt+L).
		{
			bool before = findInSelection;
			if (latchButton("\xe2\x8a\x82", &findInSelection, ImVec2(optionWidth, 0.0f))) // ⊂
			{
				findInSelection = before; // toggleFindInSelection flips it (+ captures scope)
				toggleFindInSelection();
				find();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Find in selection");
		}

		ImGui::SameLine();

		if (ImGui::Button("x", ImVec2(optionWidth, 0.0f)))
		{
			closeFindReplace();
		}

		if (!readOnly)
		{
			ImGui::SetNextItemWidth(fieldWidth);
			inputString("###replace", &replaceText);
			ImGui::SameLine();
			if (focusOnReplace)
			{
				ImGui::SetKeyboardFocusHere();
				focusOnReplace = false;

			}
			bool disableReplaceButtons = !findText.size() || !replaceText.size();

			if (disableReplaceButtons)
			{
				ImGui::BeginDisabled();
			}

			if (ImGui::Button(replaceButtonLabel.c_str(), ImVec2(button1Width, 0.0f)))
			{
				replace();
			}

			ImGui::SameLine();

			if (ImGui::Button(replaceAllButtonLabel.c_str(), ImVec2(button2Width, 0.0f)))
			{
				replaceAll();
			}

			if (disableReplaceButtons)
			{
				ImGui::EndDisabled();
			}
		}

		ImGui::EndChild();
		ImGui::PopStyleVar();
		ImGui::SetCursorScreenPos(currentScreenPosition);
	}
}


//
//	TextEditor::selectFirstOccurrenceOf
//

void TextEditor::goToFirstOccurrenceOf(const std::string_view& text, bool caseSensitive, bool select)
{
	Coordinate start, end;

	if (document.findText(Coordinate(0, 0), text, caseSensitive, true, start, end))
	{
		scrollToLine(start.line, Scroll::alignMiddle);
		cursors.setCursor(start, select ? end : start);
		makeCursorVisible();


	}
	else
	{
		cursors.clearAdditional(true);
	}
}


//
//	TextEditor::goToDefinitionOf
//
//	Walks every whole-word occurrence of `text` and scores each one against a
//	"looks like a definition" heuristic. Picks the highest-scoring match (ties
//	broken by document order). No real parser — works lexically.
//

void TextEditor::goToDefinitionOf(const std::string_view& text, bool caseSensitive)
{
	struct Hit { Coordinate start, end; int score; };
	std::vector<Hit> hits;
	Coordinate from{ 0, 0 }, s, e;
	auto coordLess = [](Coordinate a, Coordinate b)
		{
			if (a.line != b.line) return a.line < b.line;
			return a.column < b.column;
		};
	while (document.findText(from, text, caseSensitive, /*wholeWord*/ true, s, e))
	{
		// `findText` wraps to the start of the document when it falls off the
		// end. Bail as soon as it returns a hit that's before our advancing
		// search cursor — otherwise we'd loop forever once we've seen them all.
		if (!hits.empty() && coordLess(s, from)) break;
		int score = 0;
		auto line = document.getLineText(s.line);

		// Skip past the match and look at what follows on the same line.
		size_t after = std::min(static_cast<size_t>(e.column), line.size());
		// Skip whitespace after the word
		while (after < line.size() && (line[after] == ' ' || line[after] == '\t')) ++after;

		bool inLineComment = false;
		// Cheap line-comment detection: `//` (or `#` for Python-ish) earlier on this line.
		if (auto pos1 = line.find("//"); pos1 != std::string::npos && pos1 < static_cast<size_t>(s.column)) inLineComment = true;

		if (inLineComment)                                                  score -= 10;
		// Definitions usually open a body with `{` somewhere on the same line.
		if (line.find('{', static_cast<size_t>(e.column)) != std::string::npos) score += 10;
		// Python: `def foo(...):` — ends with `:`
		while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
		if (!line.empty() && line.back() == ':')                            score += 5;
		// Forward declarations / calls end in `;` — penalise.
		if (!line.empty() && line.back() == ';')                            score -= 8;
		// Bonus when the match is preceded by a type keyword (best-effort).
		if (s.column > 0)
		{
			std::string pre = line.substr(0, static_cast<size_t>(s.column));
			while (!pre.empty() && (pre.back() == ' ' || pre.back() == '\t')) pre.pop_back();
			static const char* preWords[] = { "void","int","float","double","bool","char","auto","class","struct","def","function","fn" };
			for (auto* w : preWords)
			{
				auto wl = std::strlen(w);
				if (pre.size() >= wl && pre.compare(pre.size() - wl, wl, w) == 0) { score += 3; break; }
			}
		}

		hits.push_back({ s, e, score });
		from = e;
	}

	if (hits.empty()) { cursors.clearAdditional(true); return; }

	// Pick the highest-scoring hit; on tie, first one wins.
	auto best = std::max_element(hits.begin(), hits.end(),
								 [](const Hit& a, const Hit& b) { return a.score < b.score; });

	scrollToLine(best->start.line, Scroll::alignMiddle);
	cursors.setCursor(best->start, best->end);
	makeCursorVisible();
}

void TextEditor::selectFirstOccurrenceOf(const std::string_view& text, bool caseSensitive, bool wholeWord)
{
	Coordinate start, end;

	if (document.findText(Coordinate(0, 0), text, caseSensitive, wholeWord, start, end))
	{
		cursors.setCursor(start, end);
		makeCursorVisible();

	}
	else
	{
		cursors.clearAdditional(true);
	}
}


//
//	TextEditor::selectNextOccurrenceOf
//

void TextEditor::selectNextOccurrenceOf(const std::string_view& text, bool caseSensitive, bool wholeWord)
{
	Coordinate start, end;

	if (document.findText(cursors.getCurrent().getSelectionEnd(), text, caseSensitive, wholeWord, start, end))
	{
		cursors.setCursor(start, end);
		makeCursorVisible();

	}
	else
	{
		cursors.clearAdditional(true);
	}
}


//
//	TextEditor::selectAllOccurrencesOf
//

void TextEditor::selectAllOccurrencesOf(const std::string_view& text, bool caseSensitive, bool wholeWord)
{
	Coordinate start, end;

	if (document.findText(Coordinate(0, 0), text, caseSensitive, wholeWord, start, end))
	{
		cursors.setCursor(start, end);
		bool done = false;

		while (!done)
		{
			Coordinate nextStart, nextEnd;
			document.findText(cursors.getCurrent().getSelectionEnd(), text, caseSensitive, wholeWord, nextStart, nextEnd);

			if (nextStart == start && nextEnd == end)
			{
				done = true;

			}
			else
			{
				cursors.addCursor(nextStart, nextEnd);
			}
		}

		makeCursorVisible();

	}
	else
	{
		cursors.clearAdditional(true);
	}
}


//
//	TextEditor::selectAllOccurrencesOfInRange
//
//	Like selectAllOccurrencesOf but only within [rangeStart, rangeEnd) — the
//	"find in selection" scope. A match whose START is at/after rangeEnd is out.

void TextEditor::selectAllOccurrencesOfInRange(const std::string_view& text, bool caseSensitive,
	bool wholeWord, Coordinate rangeStart, Coordinate rangeEnd)
{
	Coordinate start, end;
	bool any = false;

	if (document.findText(rangeStart, text, caseSensitive, wholeWord, start, end))
	{
		// findText wraps around the document; a first hit past the scope means
		// there is nothing inside it.
		if (!(start < rangeEnd))
		{
			cursors.clearAdditional(true);
			return;
		}
		cursors.setCursor(start, end);
		any = true;

		Coordinate firstStart = start, firstEnd = end;
		bool done = false;
		while (!done)
		{
			Coordinate nextStart, nextEnd;
			document.findText(cursors.getCurrent().getSelectionEnd(), text, caseSensitive, wholeWord, nextStart, nextEnd);
			if ((nextStart == firstStart && nextEnd == firstEnd) || !(nextStart < rangeEnd))
				done = true; // wrapped back to the first hit, or ran past the scope
			else
				cursors.addCursor(nextStart, nextEnd);
		}
		makeCursorVisible();
	}
	if (!any)
		cursors.clearAdditional(true);
}


//
//	TextEditor::toggleFindInSelection
//

void TextEditor::toggleFindInSelection()
{
	findInSelection = !findInSelection;
	if (findInSelection)
	{
		// Capture the current selection as the scope. With no (or a caret-only)
		// selection, scope the whole document so the toggle is still meaningful.
		auto& c = cursors.getCurrent();
		if (c.hasSelection())
		{
			findScopeStart = c.getSelectionStart();
			findScopeEnd = c.getSelectionEnd();
		}
		else
		{
			findScopeStart = Coordinate(0, 0);
			findScopeEnd = Coordinate(document.lineCount(), 0);
		}
	}
}


//
//	TextEditor::addNextOccurrence
//

void TextEditor::addNextOccurrence()
{

	auto cursor = cursors.getCurrent();
	auto text = document.getSectionText(cursor.getSelectionStart(), cursor.getSelectionEnd());
	Coordinate start, end;

	if (document.findText(cursor.getSelectionEnd(), text, true, false, start, end))
	{
		cursors.addCursor(start, end);
	}
}


//
//	TextEditor::addNextOccurrences
//

void TextEditor::selectAllOccurrences()
{
	auto cursor = cursors.getCurrent();
	auto text = document.getSectionText(cursor.getSelectionStart(), cursor.getSelectionEnd());
	selectAllOccurrencesOf(text, true, false);
}


//
//	TextEditor::replaceTextInCurrentCursor
//

void TextEditor::replaceTextInCurrentCursor(const std::string_view& text)
{
	auto transaction = startTransaction();

	// first delete old text
	auto cursor = cursors.getCurrentAsIterator();
	auto start = cursor->getSelectionStart();
	auto end = cursor->getSelectionEnd();
	deleteText(transaction, start, end);
	cursors.adjustForDelete(cursor, start, end);

	// now insert new text
	Coordinate newEnd = insertText(transaction, start, text);
	cursor->update(newEnd, false);
	cursors.adjustForInsert(cursor, start, newEnd);

	endTransaction(transaction);
}


//
//	TextEditor::replaceTextInAllCursors
//

void TextEditor::replaceTextInAllCursors(const std::string_view& text)
{
	auto transaction = startTransaction();
	insertTextIntoAllCursors(transaction, text);
	endTransaction(transaction);
}


//
//	TextEditor::replaceSectionText
//

void TextEditor::replaceSectionText(const Coordinate& start, const Coordinate& end, const std::string_view& text)
{
	auto transaction = startTransaction();
	deleteText(transaction, start, end);
	auto newEnd = insertText(transaction, start, text);
	cursors.clearAdditional();
	cursors.getMain().update(newEnd, newEnd);
	endTransaction(transaction);
}


//
//	TextEditor::openFindReplace
//

void TextEditor::openFindReplace()
{
	// get main cursor location
	auto cursor = cursors.getMain();

	// see if we have a current selection that's on one line
	if (cursor.hasSelection())
	{
		if (cursor.getSelectionStart().line == cursor.getSelectionEnd().line)
		{
			// use it as the default search
			findText = document.getSectionText(cursor.getSelectionStart(), cursor.getSelectionEnd());
		}

	}
	else
	{
		// if cursor is inside "real" word, use that as the default
		auto start = document.findWordStart(cursor.getSelectionStart(), true);
		auto end = document.findWordEnd(cursor.getSelectionStart(), true);

		if (start != end)
		{
			findText = document.getSectionText(start, end);
		}
	}

	findReplaceVisible = true;
	focusOnFind = true;
}


//
//	TextEditor::closeFindReplace
//

void TextEditor::closeFindReplace()
{
	findReplaceVisible = false;
	focusOnEditor = true;
	focusOnFind = false;
}


//
//	TextEditor::find
//

void TextEditor::find()
{
	if (findText.size())
	{
		selectNextOccurrenceOf(findText, caseSensitiveFind, wholeWordFind);
		focusOnEditor = true;
		focusOnFind = false;
	}
}


//
//	TextEditor::findNext
//

void TextEditor::findNext()
{
	if (findText.size())
	{
		selectNextOccurrenceOf(findText, caseSensitiveFind, wholeWordFind);
		focusOnEditor = true;
		focusOnFind = false;
	}
}


//
//	TextEditor::findPrevious
//

void TextEditor::selectPreviousOccurrenceOf(const std::string_view& text, bool caseSensitive, bool wholeWord)
{
	if (text.empty()) return;
	auto less = [](Coordinate a, Coordinate b) {
		return a.line != b.line ? a.line < b.line : a.column < b.column; };
	// document.findText only searches forward (wrapping). To go backward, scan all
	// matches once and keep the last one strictly before the current selection;
	// if none precede it, wrap to the final match.
	Coordinate target = cursors.getCurrent().getSelectionStart();
	Coordinate from{ 0, 0 }, s, e, bestS, bestE, lastS, lastE;
	bool haveBest = false, haveLast = false, first = true;
	int n = 0;
	while (document.findText(from, text, caseSensitive, wholeWord, s, e))
	{
		if (!first && !less(from, e)) break;   // no forward progress → wrapped
		first = false;
		lastS = s; lastE = e; haveLast = true;
		if (less(s, target)) { bestS = s; bestE = e; haveBest = true; }
		from = e;
		if (++n > 100000) break;               // pathological safety bound
	}
	Coordinate rs, re;
	if (haveBest) { rs = bestS; re = bestE; }
	else if (haveLast) { rs = lastS; re = lastE; }   // none before cursor → wrap to last
	else return;
	scrollToLine(rs.line, Scroll::alignMiddle);
	cursors.setCursor(rs, re);
	makeCursorVisible();
}

void TextEditor::findPrevious()
{
	if (findText.size())
	{
		selectPreviousOccurrenceOf(findText, caseSensitiveFind, wholeWordFind);
		focusOnEditor = true;
		focusOnFind = false;
	}
}


//
//	TextEditor::findAll
//

void TextEditor::findAll()
{
	if (findText.size())
	{
		if (findInSelection)
			selectAllOccurrencesOfInRange(findText, caseSensitiveFind, wholeWordFind, findScopeStart, findScopeEnd);
		else
			selectAllOccurrencesOf(findText, caseSensitiveFind, wholeWordFind);
		focusOnEditor = true;
		focusOnFind = false;
	}
}


//
//	TextEditor::replace
//

void TextEditor::replace()
{
	if (findText.size())
	{
		if (!cursors.anyHasSelection())
		{
			selectNextOccurrenceOf(findText, caseSensitiveFind, wholeWordFind);
		}

		replaceTextInCurrentCursor(replaceText);
		selectNextOccurrenceOf(findText, caseSensitiveFind, wholeWordFind);
		focusOnEditor = true;
		focusOnFind = false;
	}
}


//
//	TextEditor::replaceAll
//

void TextEditor::replaceAll()
{
	if (findText.size())
	{
		if (findInSelection)
			selectAllOccurrencesOfInRange(findText, caseSensitiveFind, wholeWordFind, findScopeStart, findScopeEnd);
		else
			selectAllOccurrencesOf(findText, caseSensitiveFind, wholeWordFind);
		replaceTextInAllCursors(replaceText);
		focusOnEditor = true;
		focusOnFind = false;
	}
}



//
//	TextEditor::setAutoCompleteConfig
//


void TextEditor::Autocomplete::setConfig(const AutoCompleteConfig* config)
{
	if (config)
	{
		configuration = *config;
		configured = true;

	}
	else
	{
		configured = false;
	}

	active = false;
}


//
//	TextEditor::Autocomplete::startTyping
//

bool TextEditor::Autocomplete::startTyping(Cursors& cursors)
{
	if (!active && !requestActivation && configured && configuration.triggerOnTyping)
	{
		triggeredManually = false;
		start(cursors);
		return true;

	}
	else
	{
		return false;
	}
}


//
//	TextEditor::Autocomplete::startShortcut
//

bool TextEditor::Autocomplete::startShortcut(Cursors& cursors)
{
	if (!active && !requestActivation && configured && configuration.triggerOnShortcut)
	{
		triggeredManually = true;
		start(cursors);
		return true;

	}
	else
	{
		return false;
	}
}


//
//	TextEditor::Autocomplete::cancel
//

void TextEditor::Autocomplete::cancel()
{
	if (active)
	{
		requestDeactivation = true;
	}
}


//
//	renderSuggestion
//

static bool renderSuggestion(const std::string_view& suggestion, const std::string_view& searchTerm, float width, bool selected)
{
	// custom widget to render an autocomplete suggestion in the style of Visual Studio Code
	auto glyphPos = ImGui::GetCursorScreenPos();
	auto size = ImVec2(width, ImGui::GetFrameHeightWithSpacing());
	auto clicked = ImGui::InvisibleButton("suggestion", size);

	auto drawList = ImGui::GetWindowDrawList();
	auto font = ImGui::GetFont();
	auto fontSize = ImGui::GetFontSize();
	auto glyphWidth = ImGui::CalcTextSize("#").x;

	// highlight selected item
	if (selected)
	{
		drawList->AddRectFilled(glyphPos, glyphPos + size, ImGui::GetColorU32(ImGuiCol_Header));
	}

	// process all UTF-8 glyphs in suggestion
	glyphPos += ImGui::GetStyle().FramePadding;
	auto suggestionEnd = suggestion.end();
	auto searchTermEnd = searchTerm.end();
	auto i = TextEditor::CodePoint::skipBOM(suggestion.begin(), suggestionEnd);
	auto j = TextEditor::CodePoint::skipBOM(searchTerm.begin(), searchTermEnd);

	while (i < suggestionEnd)
	{
		// get next glyph from suggestion
		ImWchar codepoint;
		i = TextEditor::CodePoint::read(i, suggestionEnd, &codepoint);

		// highlight glyph in suggestion that match search term
		auto color = ImGui::GetColorU32(ImGuiCol_Text);

		if (j < searchTermEnd)
		{
			ImWchar searchCodePoint;
			auto next = TextEditor::CodePoint::read(j, searchTermEnd, &searchCodePoint);

			if (TextEditor::CodePoint::toLower(searchCodePoint) == TextEditor::CodePoint::toLower(codepoint))
			{
				color = ImGui::GetColorU32(ImGuiCol_TextLink);
				j = next;
			}
		}

		// render the glyph
		font->RenderChar(drawList, fontSize, glyphPos, color, codepoint);
		glyphPos.x += glyphWidth;
	}

	return clicked;
}


//
//	TextEditor::Autocomplete::render
//

bool TextEditor::Autocomplete::render(const TextEditor& editor, Document& document, Cursors& cursors, const Language* language, float textOffset, ImVec2 glyphSize)
{
	// see if we need to activate autocomplete mode
	if (requestActivation)
	{
		// apply popup delay
		if (std::chrono::system_clock::now() > activationTime)
		{
			// reset activation flag
			requestActivation = false;

			// capture locations
			startLocation = document.findWordStart(currentLocation, true);

			// update the autocomplete state
			updateState(document, language);

			// handle cases where autocomplete request is ignored
			if (state.inComment && !configuration.triggerInComments)
			{
				return false;
			}

			if (state.inString && !configuration.triggerInStrings)
			{
				return false;
			}

			// get initial list of suggestions from the app
			refreshSuggestions();

			// show autocomplete popup window
			ImGui::OpenPopup("AutoCompleteContextMenu");
			active = true;
		}
	}

	// only continue if autocomplete is active
	if (!active)
	{
		return false;
	}

	// see if cursor moved since last time
	auto newLocation = cursors.getMain().getSelectionEnd();

	if (newLocation != currentLocation)
	{
		// see if we need to deactivate autocomplete because cursor is on new line
		if (newLocation.line != currentLocation.line)
		{
			requestDeactivation = true;

		}
		else
		{
			// see if cursor moved away from current word
			auto newStart = document.findWordStart(newLocation, true);

			if (newStart == startLocation)
			{
				currentLocation = newLocation;

				// we deactivate autocomplete if the current location is the start
				if (currentLocation == startLocation)
				{
					requestDeactivation = true;

				}
				else
				{
					updateState(document, language);
					refreshSuggestions();
				}

			}
			else
			{
				// Cursor left the word being completed → close the popup.
				// (Comment added via the in-editor reply-to-Claude loop — you sent
				// "[TextEditor.cpp:8660] if you see this add a comment" and it worked.)
				requestDeactivation = true;
			}
		}
	}

	// open popup window
	bool result = false;
	auto cursorScreenPos = ImGui::GetCursorScreenPos();

	// Anchor at the VISUAL cursor position: fold-collapsed row (lineToVisualIndex)
	// and measured x (columnToX — handles proportional fonts; == column*glyph.x in
	// monospace). Raw line*glyph.y put the popup N-hidden-rows below the caret when
	// folds were collapsed above it — over whatever window sat under the editor.
	ImGui::SetNextWindowPos(ImVec2(
		cursorScreenPos.x + textOffset + editor.columnToX(currentLocation.line, currentLocation.column),
		cursorScreenPos.y + (editor.lineToVisualIndex(currentLocation.line) + 1) * glyphSize.y));

	auto suggestions = state.suggestions.size();
	auto visibleSuggestions = (suggestions == 0) ? 1 : std::min(static_cast<size_t>(10), suggestions);
	auto& style = ImGui::GetStyle();
	auto height = ImGui::GetFrameHeightWithSpacing() * visibleSuggestions + style.WindowPadding.y * 2.0f;
	ImGui::SetNextWindowSize(ImVec2(suggestionWidth, height));

	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoNav;

	if (ImGui::BeginPopup("AutoCompleteContextMenu", flags))
	{
		// z-order nudge needs internals; without it the popup still shows,
		// it just isn't forced above sibling floating windows on first frame
#ifndef TE_NO_IMGUI_INTERNAL
		if (ImGui::IsWindowAppearing())
		{
			ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
		}
#endif

		// deactivate popup (if requested)
		if (requestDeactivation)
		{
			ImGui::CloseCurrentPopup();
			requestDeactivation = false;
			active = false;

		}
		else
		{
			// do we have any suggestions
			if (suggestions)
			{
				auto items = state.suggestions.size();

				// apply arrow keys to selected suggestion
				if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
				{
					if (currentSelection == 0)
					{
						currentSelection = items - 1;
					}
					else
					{
						currentSelection--;
					}

				}
				else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
				{
					if (currentSelection == items - 1)
					{
						currentSelection = 0;

					}
					else
					{
						currentSelection++;
					}

					// use selected suggestion if user hit tab of return
				}
				else if (ImGui::IsKeyPressed(ImGuiKey_Tab) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
				{
					requestDeactivation = true;
					result = true;

				}
				else if (configuration.autoInsertSingleSuggestions && triggeredManually && state.suggestions.size() == 1)
				{
					requestDeactivation = true;
					result = true;
				}

				// render suggestions
				for (size_t i = 0; i < items; i++)
				{
					// ensure unique ID
					ImGui::PushID(static_cast<int>(i));

					// scroll list to selected item (if required)
					auto selected = i == currentSelection;

					if (selected)
					{
						ImGui::SetScrollHereY(1.0f);
					}

					if (renderSuggestion(state.suggestions[i].c_str(), state.searchTerm, ImGui::GetContentRegionAvail().x, selected))
					{
						// user clicked on a suggestion, use it
						currentSelection = i;
						requestDeactivation = true;
						result = true;
					}

					ImGui::PopID();
				}

			}
			else
			{
				ImGui::TextUnformatted(configuration.noSuggestionsLabel.c_str());
			}
		}

		ImGui::EndPopup();

	}
	else
	{
		requestDeactivation = false;
		active = false;
	}

	return result;
}


//
//	TextEditor::Autocomplete::setSuggestions
//

void TextEditor::Autocomplete::setSuggestions(const std::vector<std::string>& suggestions)
{
	state.suggestions = suggestions;
	currentSelection = 0;
}


//
//	TextEditor::Autocomplete::isSpecialKeyPressed
//

bool TextEditor::Autocomplete::isSpecialKeyPressed() const
{
	for (auto key :{ ImGuiKey_Tab, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_UpArrow, ImGuiKey_DownArrow })
	{
		if (ImGui::IsKeyPressed(key))
		{
			return true;
		}
	}

	return false;
}


//
//	TextEditor::Autocomplete::start
//

void TextEditor::Autocomplete::start(Cursors& cursors)
{
	// request start of autocomplete mode (can't be done here as the Dear ImGui context might not be right)
	requestActivation = true;
	currentLocation = cursors.getMain().getSelectionEnd();
	activationTime = std::chrono::system_clock::now() + configuration.triggerDelay;
}


//
//	TextEditor::Autocomplete::updateState
//

void TextEditor::Autocomplete::updateState(Document& document, const Language* language)
{
	state.searchTerm = document.getSectionText(startLocation, currentLocation);

	if (currentLocation.column == 0)
	{
		state.inIdentifier = false;
		state.inNumber = false;

		auto lineState = document[currentLocation.line].state;
		state.inComment = lineState == State::inComment;

		state.inString =
			lineState == State::inDoubleQuotedString ||
			lineState == State::inSingleQuotedString||
			lineState == State::inOtherString ||
			lineState == State::inOtherStringAlt;

	}
	else
	{
		auto color = document.getColor(Coordinate(currentLocation.line, currentLocation.column - 1));
		state.inIdentifier = color == Color::identifier || color == Color::knownIdentifier;
		state.inNumber = color == Color::number;
		state.inComment = color == Color::comment;
		state.inString = color == Color::string;
	}

	state.line = currentLocation.line;
	state.searchTermStartColumn = startLocation.column;
	state.searchTermStartIndex = document.getIndex(startLocation);
	state.searchTermEndColumn = currentLocation.column;
	state.searchTermEndIndex = document.getIndex(currentLocation);

	state.language = language;
	state.userData = configuration.userData;
}


//
//	TextEditor::Autocomplete::refreshSuggestions
//

void TextEditor::Autocomplete::refreshSuggestions()
{
	// populate suggestion list through callback (or clear it if there is none)
	if (configuration.callback)
	{
		configuration.callback(state);

	}
	else
	{
		state.suggestions.clear();
	}

	currentSelection = 0;
}


//
//	TextEditor::Trie::insert
//

void TextEditor::Trie::insert(const std::string_view& word)
{
	auto node = root.get();
	auto end = word.end();
	auto i = TextEditor::CodePoint::skipBOM(word.begin(), end);

	while (i < end)
	{
		ImWchar codepoint;
		i = TextEditor::CodePoint::read(i, end, &codepoint);

		if (node->children.find(codepoint) == node->children.end())
		{
			node->children[codepoint] = std::make_unique<Node>();
		}

		node = node->children[codepoint].get();
	}

	node->word = word;
}


//
//	TextEditor::Trie::contains
//

bool TextEditor::Trie::contains(const std::string_view& word) const
{
	if (!root) return false;
	auto node = root.get();
	auto end = word.end();
	auto i = TextEditor::CodePoint::skipBOM(word.begin(), end);
	while (i < end)
	{
		ImWchar codepoint;
		i = TextEditor::CodePoint::read(i, end, &codepoint);
		auto it = node->children.find(codepoint);
		if (it == node->children.end()) return false;
		node = it->second.get();
	}
	// `word` was stored if the node carries a non-empty matching string.
	return node && !node->word.empty();
}


//
//	TextEditor::Trie::findSuggestions
//

void TextEditor::Trie::findSuggestions(std::vector<std::string>& suggestions, const std::string_view& searchTerm, size_t limit, size_t maxSkippedLetters)
{
	// clear result vector
	maxSkip = maxSkippedLetters;
	suggestions.clear();

	// don't even try if search term is empty
	if (searchTerm.size() != 0)
	{
		// convert search term into vector of code blocks
		searchCodepoints.clear();
		auto end = searchTerm.end();
		auto i = TextEditor::CodePoint::skipBOM(searchTerm.begin(), end);

		while (i < end)
		{
			ImWchar codepoint;
			i = TextEditor::CodePoint::read(i, end, &codepoint);
			searchCodepoints.emplace_back(codepoint);
		}

		// recursively evaluate nodes
		candidates.clear();
		evaluateNode(root.get(), 0, 0, maxSkip);

		// did we find anything?
		if (candidates.size())
		{
			// sort candidates by cost
			std::sort(candidates.begin(), candidates.end());

			// remove duplicates which are caused by mutiple paths based on skips
			auto last = std::unique(candidates.begin(), candidates.end());
			candidates.erase(last, candidates.end());

			// populate suggestions (applying limit)
			auto size = std::min(static_cast<size_t>(limit), candidates.size());

			for (size_t j = 0; j < size; j++)
			{
				suggestions.emplace_back(candidates[j].node->word);
			}
		}
	}
}


//
//	TextEditor::Trie::evaluateNode
//

void TextEditor::Trie::evaluateNode(const Node* node, size_t index, size_t cost, size_t skip)
{
	// see if that is one of our children (check both lower and uppercase matches)
	ImWchar codepointLower = TextEditor::CodePoint::toLower(searchCodepoints[index]);
	Node* childLower = nullptr;

	if (node->children.find(codepointLower) != node->children.end())
	{
		// codepoint found, is this the last one in our searchTerm?
		childLower = node->children.at(codepointLower).get();

		if (index == searchCodepoints.size() - 1)
		{
			// yes, add candidate words to results
			addCandidates(childLower, cost);

		}
		else
		{
			// no, try to find the rest
			evaluateNode(childLower, index + 1, cost, maxSkip);
		}
	}

	ImWchar codepointUpper = TextEditor::CodePoint::toUpper(searchCodepoints[index]);
	Node* childUpper = nullptr;

	if (node->children.find(codepointUpper) != node->children.end())
	{
		// codepoint found, is this the last one in our searchTerm?
		childUpper = node->children.at(codepointUpper).get();

		if (index == searchCodepoints.size() - 1)
		{
			// yes, add candidate words to results
			addCandidates(childUpper, cost);

		}
		else
		{
			// no, try to find the rest
			evaluateNode(childUpper, index + 1, cost, maxSkip);
		}
	}

	// also try children to support detection of missing letters (if we haven't skipped too many entries yet)
	if (skip)
	{
		for (auto const& [key, value] : node->children)
		{
			auto next = value.get();

			if (next != childLower && next != childUpper)
			{
				evaluateNode(next, index, cost + 1, skip - 1);
			}
		}
	}
}


//
//	TextEditor::Trie::addCandidates
//

void TextEditor::Trie::addCandidates(const Node* node, size_t cost)
{
	if (node->word.size())
	{
		candidates.emplace_back(node, cost);
	}

	for (auto const& [key, value] : node->children)
	{
		addCandidates(value.get(), cost + 1);
	}
}


//
//	Range table types
//

template <typename T>
struct Range
{
	T low;
	T high;
	T stride;
};

using Range16 = Range<ImWchar16>;
using Range32 = Range<ImWchar32>;

template <typename T>
struct CaseRange
{
	T low;
	T high;
	int32_t toUpper;
	int32_t toLower;
};

using CaseRange16 = CaseRange<char16_t>;
using CaseRange32 = CaseRange<char32_t>;


//
//	letters16
//

static Range16 letters16[] = {
	{0x0041, 0x005a, 0x0001}, {0x0061, 0x007a, 0x0001}, {0x00aa, 0x00b5, 0x000b}, {0x00ba, 0x00c0, 0x0006},
	{0x00c1, 0x00d6, 0x0001}, {0x00d8, 0x00f6, 0x0001}, {0x00f8, 0x02c1, 0x0001}, {0x02c6, 0x02d1, 0x0001},
	{0x02e0, 0x02e4, 0x0001}, {0x02ec, 0x02ee, 0x0002}, {0x0370, 0x0374, 0x0001}, {0x0376, 0x0377, 0x0001},
	{0x037a, 0x037d, 0x0001}, {0x037f, 0x0386, 0x0007}, {0x0388, 0x038a, 0x0001}, {0x038c, 0x038e, 0x0002},
	{0x038f, 0x03a1, 0x0001}, {0x03a3, 0x03f5, 0x0001}, {0x03f7, 0x0481, 0x0001}, {0x048a, 0x052f, 0x0001},
	{0x0531, 0x0556, 0x0001}, {0x0559, 0x0560, 0x0007}, {0x0561, 0x0588, 0x0001}, {0x05d0, 0x05ea, 0x0001},
	{0x05ef, 0x05f2, 0x0001}, {0x0620, 0x064a, 0x0001}, {0x066e, 0x066f, 0x0001}, {0x0671, 0x06d3, 0x0001},
	{0x06d5, 0x06e5, 0x0010}, {0x06e6, 0x06ee, 0x0008}, {0x06ef, 0x06fa, 0x000b}, {0x06fb, 0x06fc, 0x0001},
	{0x06ff, 0x0710, 0x0011}, {0x0712, 0x072f, 0x0001}, {0x074d, 0x07a5, 0x0001}, {0x07b1, 0x07ca, 0x0019},
	{0x07cb, 0x07ea, 0x0001}, {0x07f4, 0x07f5, 0x0001}, {0x07fa, 0x0800, 0x0006}, {0x0801, 0x0815, 0x0001},
	{0x081a, 0x0824, 0x000a}, {0x0828, 0x0840, 0x0018}, {0x0841, 0x0858, 0x0001}, {0x0860, 0x086a, 0x0001},
	{0x0870, 0x0887, 0x0001}, {0x0889, 0x088f, 0x0001}, {0x08a0, 0x08c9, 0x0001}, {0x0904, 0x0939, 0x0001},
	{0x093d, 0x0950, 0x0013}, {0x0958, 0x0961, 0x0001}, {0x0971, 0x0980, 0x0001}, {0x0985, 0x098c, 0x0001},
	{0x098f, 0x0990, 0x0001}, {0x0993, 0x09a8, 0x0001}, {0x09aa, 0x09b0, 0x0001}, {0x09b2, 0x09b6, 0x0004},
	{0x09b7, 0x09b9, 0x0001}, {0x09bd, 0x09ce, 0x0011}, {0x09dc, 0x09dd, 0x0001}, {0x09df, 0x09e1, 0x0001},
	{0x09f0, 0x09f1, 0x0001}, {0x09fc, 0x0a05, 0x0009}, {0x0a06, 0x0a0a, 0x0001}, {0x0a0f, 0x0a10, 0x0001},
	{0x0a13, 0x0a28, 0x0001}, {0x0a2a, 0x0a30, 0x0001}, {0x0a32, 0x0a33, 0x0001}, {0x0a35, 0x0a36, 0x0001},
	{0x0a38, 0x0a39, 0x0001}, {0x0a59, 0x0a5c, 0x0001}, {0x0a5e, 0x0a72, 0x0014}, {0x0a73, 0x0a74, 0x0001},
	{0x0a85, 0x0a8d, 0x0001}, {0x0a8f, 0x0a91, 0x0001}, {0x0a93, 0x0aa8, 0x0001}, {0x0aaa, 0x0ab0, 0x0001},
	{0x0ab2, 0x0ab3, 0x0001}, {0x0ab5, 0x0ab9, 0x0001}, {0x0abd, 0x0ad0, 0x0013}, {0x0ae0, 0x0ae1, 0x0001},
	{0x0af9, 0x0b05, 0x000c}, {0x0b06, 0x0b0c, 0x0001}, {0x0b0f, 0x0b10, 0x0001}, {0x0b13, 0x0b28, 0x0001},
	{0x0b2a, 0x0b30, 0x0001}, {0x0b32, 0x0b33, 0x0001}, {0x0b35, 0x0b39, 0x0001}, {0x0b3d, 0x0b5c, 0x001f},
	{0x0b5d, 0x0b5f, 0x0002}, {0x0b60, 0x0b61, 0x0001}, {0x0b71, 0x0b83, 0x0012}, {0x0b85, 0x0b8a, 0x0001},
	{0x0b8e, 0x0b90, 0x0001}, {0x0b92, 0x0b95, 0x0001}, {0x0b99, 0x0b9a, 0x0001}, {0x0b9c, 0x0b9e, 0x0002},
	{0x0b9f, 0x0ba3, 0x0004}, {0x0ba4, 0x0ba8, 0x0004}, {0x0ba9, 0x0baa, 0x0001}, {0x0bae, 0x0bb9, 0x0001},
	{0x0bd0, 0x0c05, 0x0035}, {0x0c06, 0x0c0c, 0x0001}, {0x0c0e, 0x0c10, 0x0001}, {0x0c12, 0x0c28, 0x0001},
	{0x0c2a, 0x0c39, 0x0001}, {0x0c3d, 0x0c58, 0x001b}, {0x0c59, 0x0c5a, 0x0001}, {0x0c5c, 0x0c5d, 0x0001},
	{0x0c60, 0x0c61, 0x0001}, {0x0c80, 0x0c85, 0x0005}, {0x0c86, 0x0c8c, 0x0001}, {0x0c8e, 0x0c90, 0x0001},
	{0x0c92, 0x0ca8, 0x0001}, {0x0caa, 0x0cb3, 0x0001}, {0x0cb5, 0x0cb9, 0x0001}, {0x0cbd, 0x0cdc, 0x001f},
	{0x0cdd, 0x0cde, 0x0001}, {0x0ce0, 0x0ce1, 0x0001}, {0x0cf1, 0x0cf2, 0x0001}, {0x0d04, 0x0d0c, 0x0001},
	{0x0d0e, 0x0d10, 0x0001}, {0x0d12, 0x0d3a, 0x0001}, {0x0d3d, 0x0d4e, 0x0011}, {0x0d54, 0x0d56, 0x0001},
	{0x0d5f, 0x0d61, 0x0001}, {0x0d7a, 0x0d7f, 0x0001}, {0x0d85, 0x0d96, 0x0001}, {0x0d9a, 0x0db1, 0x0001},
	{0x0db3, 0x0dbb, 0x0001}, {0x0dbd, 0x0dc0, 0x0003}, {0x0dc1, 0x0dc6, 0x0001}, {0x0e01, 0x0e30, 0x0001},
	{0x0e32, 0x0e33, 0x0001}, {0x0e40, 0x0e46, 0x0001}, {0x0e81, 0x0e82, 0x0001}, {0x0e84, 0x0e86, 0x0002},
	{0x0e87, 0x0e8a, 0x0001}, {0x0e8c, 0x0ea3, 0x0001}, {0x0ea5, 0x0ea7, 0x0002}, {0x0ea8, 0x0eb0, 0x0001},
	{0x0eb2, 0x0eb3, 0x0001}, {0x0ebd, 0x0ec0, 0x0003}, {0x0ec1, 0x0ec4, 0x0001}, {0x0ec6, 0x0edc, 0x0016},
	{0x0edd, 0x0edf, 0x0001}, {0x0f00, 0x0f40, 0x0040}, {0x0f41, 0x0f47, 0x0001}, {0x0f49, 0x0f6c, 0x0001},
	{0x0f88, 0x0f8c, 0x0001}, {0x1000, 0x102a, 0x0001}, {0x103f, 0x1050, 0x0011}, {0x1051, 0x1055, 0x0001},
	{0x105a, 0x105d, 0x0001}, {0x1061, 0x1065, 0x0004}, {0x1066, 0x106e, 0x0008}, {0x106f, 0x1070, 0x0001},
	{0x1075, 0x1081, 0x0001}, {0x108e, 0x10a0, 0x0012}, {0x10a1, 0x10c5, 0x0001}, {0x10c7, 0x10cd, 0x0006},
	{0x10d0, 0x10fa, 0x0001}, {0x10fc, 0x1248, 0x0001}, {0x124a, 0x124d, 0x0001}, {0x1250, 0x1256, 0x0001},
	{0x1258, 0x125a, 0x0002}, {0x125b, 0x125d, 0x0001}, {0x1260, 0x1288, 0x0001}, {0x128a, 0x128d, 0x0001},
	{0x1290, 0x12b0, 0x0001}, {0x12b2, 0x12b5, 0x0001}, {0x12b8, 0x12be, 0x0001}, {0x12c0, 0x12c2, 0x0002},
	{0x12c3, 0x12c5, 0x0001}, {0x12c8, 0x12d6, 0x0001}, {0x12d8, 0x1310, 0x0001}, {0x1312, 0x1315, 0x0001},
	{0x1318, 0x135a, 0x0001}, {0x1380, 0x138f, 0x0001}, {0x13a0, 0x13f5, 0x0001}, {0x13f8, 0x13fd, 0x0001},
	{0x1401, 0x166c, 0x0001}, {0x166f, 0x167f, 0x0001}, {0x1681, 0x169a, 0x0001}, {0x16a0, 0x16ea, 0x0001},
	{0x16f1, 0x16f8, 0x0001}, {0x1700, 0x1711, 0x0001}, {0x171f, 0x1731, 0x0001}, {0x1740, 0x1751, 0x0001},
	{0x1760, 0x176c, 0x0001}, {0x176e, 0x1770, 0x0001}, {0x1780, 0x17b3, 0x0001}, {0x17d7, 0x17dc, 0x0005},
	{0x1820, 0x1878, 0x0001}, {0x1880, 0x1884, 0x0001}, {0x1887, 0x18a8, 0x0001}, {0x18aa, 0x18b0, 0x0006},
	{0x18b1, 0x18f5, 0x0001}, {0x1900, 0x191e, 0x0001}, {0x1950, 0x196d, 0x0001}, {0x1970, 0x1974, 0x0001},
	{0x1980, 0x19ab, 0x0001}, {0x19b0, 0x19c9, 0x0001}, {0x1a00, 0x1a16, 0x0001}, {0x1a20, 0x1a54, 0x0001},
	{0x1aa7, 0x1b05, 0x005e}, {0x1b06, 0x1b33, 0x0001}, {0x1b45, 0x1b4c, 0x0001}, {0x1b83, 0x1ba0, 0x0001},
	{0x1bae, 0x1baf, 0x0001}, {0x1bba, 0x1be5, 0x0001}, {0x1c00, 0x1c23, 0x0001}, {0x1c4d, 0x1c4f, 0x0001},
	{0x1c5a, 0x1c7d, 0x0001}, {0x1c80, 0x1c8a, 0x0001}, {0x1c90, 0x1cba, 0x0001}, {0x1cbd, 0x1cbf, 0x0001},
	{0x1ce9, 0x1cec, 0x0001}, {0x1cee, 0x1cf3, 0x0001}, {0x1cf5, 0x1cf6, 0x0001}, {0x1cfa, 0x1d00, 0x0006},
	{0x1d01, 0x1dbf, 0x0001}, {0x1e00, 0x1f15, 0x0001}, {0x1f18, 0x1f1d, 0x0001}, {0x1f20, 0x1f45, 0x0001},
	{0x1f48, 0x1f4d, 0x0001}, {0x1f50, 0x1f57, 0x0001}, {0x1f59, 0x1f5f, 0x0002}, {0x1f60, 0x1f7d, 0x0001},
	{0x1f80, 0x1fb4, 0x0001}, {0x1fb6, 0x1fbc, 0x0001}, {0x1fbe, 0x1fc2, 0x0004}, {0x1fc3, 0x1fc4, 0x0001},
	{0x1fc6, 0x1fcc, 0x0001}, {0x1fd0, 0x1fd3, 0x0001}, {0x1fd6, 0x1fdb, 0x0001}, {0x1fe0, 0x1fec, 0x0001},
	{0x1ff2, 0x1ff4, 0x0001}, {0x1ff6, 0x1ffc, 0x0001}, {0x2071, 0x207f, 0x000e}, {0x2090, 0x209c, 0x0001},
	{0x2102, 0x2107, 0x0005}, {0x210a, 0x2113, 0x0001}, {0x2115, 0x2119, 0x0004}, {0x211a, 0x211d, 0x0001},
	{0x2124, 0x212a, 0x0002}, {0x212b, 0x212d, 0x0001}, {0x212f, 0x2139, 0x0001}, {0x213c, 0x213f, 0x0001},
	{0x2145, 0x2149, 0x0001}, {0x214e, 0x2183, 0x0035}, {0x2184, 0x2c00, 0x0a7c}, {0x2c01, 0x2ce4, 0x0001},
	{0x2ceb, 0x2cee, 0x0001}, {0x2cf2, 0x2cf3, 0x0001}, {0x2d00, 0x2d25, 0x0001}, {0x2d27, 0x2d2d, 0x0006},
	{0x2d30, 0x2d67, 0x0001}, {0x2d6f, 0x2d80, 0x0011}, {0x2d81, 0x2d96, 0x0001}, {0x2da0, 0x2da6, 0x0001},
	{0x2da8, 0x2dae, 0x0001}, {0x2db0, 0x2db6, 0x0001}, {0x2db8, 0x2dbe, 0x0001}, {0x2dc0, 0x2dc6, 0x0001},
	{0x2dc8, 0x2dce, 0x0001}, {0x2dd0, 0x2dd6, 0x0001}, {0x2dd8, 0x2dde, 0x0001}, {0x2e2f, 0x3005, 0x01d6},
	{0x3006, 0x3031, 0x002b}, {0x3032, 0x3035, 0x0001}, {0x303b, 0x303c, 0x0001}, {0x3041, 0x3096, 0x0001},
	{0x309d, 0x309f, 0x0001}, {0x30a1, 0x30fa, 0x0001}, {0x30fc, 0x30ff, 0x0001}, {0x3105, 0x312f, 0x0001},
	{0x3131, 0x318e, 0x0001}, {0x31a0, 0x31bf, 0x0001}, {0x31f0, 0x31ff, 0x0001}, {0x3400, 0x4dbf, 0x19bf},
	{0x4e00, 0x9fff, 0x51ff}, {0xa000, 0xa48c, 0x0001}, {0xa4d0, 0xa4fd, 0x0001}, {0xa500, 0xa60c, 0x0001},
	{0xa610, 0xa61f, 0x0001}, {0xa62a, 0xa62b, 0x0001}, {0xa640, 0xa66e, 0x0001}, {0xa67f, 0xa69d, 0x0001},
	{0xa6a0, 0xa6e5, 0x0001}, {0xa717, 0xa71f, 0x0001}, {0xa722, 0xa788, 0x0001}, {0xa78b, 0xa7dc, 0x0001},
	{0xa7f1, 0xa801, 0x0001}, {0xa803, 0xa805, 0x0001}, {0xa807, 0xa80a, 0x0001}, {0xa80c, 0xa822, 0x0001},
	{0xa840, 0xa873, 0x0001}, {0xa882, 0xa8b3, 0x0001}, {0xa8f2, 0xa8f7, 0x0001}, {0xa8fb, 0xa8fd, 0x0002},
	{0xa8fe, 0xa90a, 0x000c}, {0xa90b, 0xa925, 0x0001}, {0xa930, 0xa946, 0x0001}, {0xa960, 0xa97c, 0x0001},
	{0xa984, 0xa9b2, 0x0001}, {0xa9cf, 0xa9e0, 0x0011}, {0xa9e1, 0xa9e4, 0x0001}, {0xa9e6, 0xa9ef, 0x0001},
	{0xa9fa, 0xa9fe, 0x0001}, {0xaa00, 0xaa28, 0x0001}, {0xaa40, 0xaa42, 0x0001}, {0xaa44, 0xaa4b, 0x0001},
	{0xaa60, 0xaa76, 0x0001}, {0xaa7a, 0xaa7e, 0x0004}, {0xaa7f, 0xaaaf, 0x0001}, {0xaab1, 0xaab5, 0x0004},
	{0xaab6, 0xaab9, 0x0003}, {0xaaba, 0xaabd, 0x0001}, {0xaac0, 0xaac2, 0x0002}, {0xaadb, 0xaadd, 0x0001},
	{0xaae0, 0xaaea, 0x0001}, {0xaaf2, 0xaaf4, 0x0001}, {0xab01, 0xab06, 0x0001}, {0xab09, 0xab0e, 0x0001},
	{0xab11, 0xab16, 0x0001}, {0xab20, 0xab26, 0x0001}, {0xab28, 0xab2e, 0x0001}, {0xab30, 0xab5a, 0x0001},
	{0xab5c, 0xab69, 0x0001}, {0xab70, 0xabe2, 0x0001}, {0xac00, 0xd7a3, 0x2ba3}, {0xd7b0, 0xd7c6, 0x0001},
	{0xd7cb, 0xd7fb, 0x0001}, {0xf900, 0xfa6d, 0x0001}, {0xfa70, 0xfad9, 0x0001}, {0xfb00, 0xfb06, 0x0001},
	{0xfb13, 0xfb17, 0x0001}, {0xfb1d, 0xfb1f, 0x0002}, {0xfb20, 0xfb28, 0x0001}, {0xfb2a, 0xfb36, 0x0001},
	{0xfb38, 0xfb3c, 0x0001}, {0xfb3e, 0xfb40, 0x0002}, {0xfb41, 0xfb43, 0x0002}, {0xfb44, 0xfb46, 0x0002},
	{0xfb47, 0xfbb1, 0x0001}, {0xfbd3, 0xfd3d, 0x0001}, {0xfd50, 0xfd8f, 0x0001}, {0xfd92, 0xfdc7, 0x0001},
	{0xfdf0, 0xfdfb, 0x0001}, {0xfe70, 0xfe74, 0x0001}, {0xfe76, 0xfefc, 0x0001}, {0xff21, 0xff3a, 0x0001},
	{0xff41, 0xff5a, 0x0001}, {0xff66, 0xffbe, 0x0001}, {0xffc2, 0xffc7, 0x0001}, {0xffca, 0xffcf, 0x0001},
	{0xffd2, 0xffd7, 0x0001}, {0xffda, 0xffdc, 0x0001}
};


//
//	letters32
//

#if defined(IMGUI_USE_WCHAR32)

static Range32 letters32[] = {
	{0x10000, 0x1000b, 0x0001}, {0x1000d, 0x10026, 0x0001}, {0x10028, 0x1003a, 0x0001}, {0x1003c, 0x1003d, 0x0001},
	{0x1003f, 0x1004d, 0x0001}, {0x10050, 0x1005d, 0x0001}, {0x10080, 0x100fa, 0x0001}, {0x10280, 0x1029c, 0x0001},
	{0x102a0, 0x102d0, 0x0001}, {0x10300, 0x1031f, 0x0001}, {0x1032d, 0x10340, 0x0001}, {0x10342, 0x10349, 0x0001},
	{0x10350, 0x10375, 0x0001}, {0x10380, 0x1039d, 0x0001}, {0x103a0, 0x103c3, 0x0001}, {0x103c8, 0x103cf, 0x0001},
	{0x10400, 0x1049d, 0x0001}, {0x104b0, 0x104d3, 0x0001}, {0x104d8, 0x104fb, 0x0001}, {0x10500, 0x10527, 0x0001},
	{0x10530, 0x10563, 0x0001}, {0x10570, 0x1057a, 0x0001}, {0x1057c, 0x1058a, 0x0001}, {0x1058c, 0x10592, 0x0001},
	{0x10594, 0x10595, 0x0001}, {0x10597, 0x105a1, 0x0001}, {0x105a3, 0x105b1, 0x0001}, {0x105b3, 0x105b9, 0x0001},
	{0x105bb, 0x105bc, 0x0001}, {0x105c0, 0x105f3, 0x0001}, {0x10600, 0x10736, 0x0001}, {0x10740, 0x10755, 0x0001},
	{0x10760, 0x10767, 0x0001}, {0x10780, 0x10785, 0x0001}, {0x10787, 0x107b0, 0x0001}, {0x107b2, 0x107ba, 0x0001},
	{0x10800, 0x10805, 0x0001}, {0x10808, 0x1080a, 0x0002}, {0x1080b, 0x10835, 0x0001}, {0x10837, 0x10838, 0x0001},
	{0x1083c, 0x1083f, 0x0003}, {0x10840, 0x10855, 0x0001}, {0x10860, 0x10876, 0x0001}, {0x10880, 0x1089e, 0x0001},
	{0x108e0, 0x108f2, 0x0001}, {0x108f4, 0x108f5, 0x0001}, {0x10900, 0x10915, 0x0001}, {0x10920, 0x10939, 0x0001},
	{0x10940, 0x10959, 0x0001}, {0x10980, 0x109b7, 0x0001}, {0x109be, 0x109bf, 0x0001}, {0x10a00, 0x10a10, 0x0010},
	{0x10a11, 0x10a13, 0x0001}, {0x10a15, 0x10a17, 0x0001}, {0x10a19, 0x10a35, 0x0001}, {0x10a60, 0x10a7c, 0x0001},
	{0x10a80, 0x10a9c, 0x0001}, {0x10ac0, 0x10ac7, 0x0001}, {0x10ac9, 0x10ae4, 0x0001}, {0x10b00, 0x10b35, 0x0001},
	{0x10b40, 0x10b55, 0x0001}, {0x10b60, 0x10b72, 0x0001}, {0x10b80, 0x10b91, 0x0001}, {0x10c00, 0x10c48, 0x0001},
	{0x10c80, 0x10cb2, 0x0001}, {0x10cc0, 0x10cf2, 0x0001}, {0x10d00, 0x10d23, 0x0001}, {0x10d4a, 0x10d65, 0x0001},
	{0x10d6f, 0x10d85, 0x0001}, {0x10e80, 0x10ea9, 0x0001}, {0x10eb0, 0x10eb1, 0x0001}, {0x10ec2, 0x10ec7, 0x0001},
	{0x10f00, 0x10f1c, 0x0001}, {0x10f27, 0x10f30, 0x0009}, {0x10f31, 0x10f45, 0x0001}, {0x10f70, 0x10f81, 0x0001},
	{0x10fb0, 0x10fc4, 0x0001}, {0x10fe0, 0x10ff6, 0x0001}, {0x11003, 0x11037, 0x0001}, {0x11071, 0x11072, 0x0001},
	{0x11075, 0x11083, 0x000e}, {0x11084, 0x110af, 0x0001}, {0x110d0, 0x110e8, 0x0001}, {0x11103, 0x11126, 0x0001},
	{0x11144, 0x11147, 0x0003}, {0x11150, 0x11172, 0x0001}, {0x11176, 0x11183, 0x000d}, {0x11184, 0x111b2, 0x0001},
	{0x111c1, 0x111c4, 0x0001}, {0x111da, 0x111dc, 0x0002}, {0x11200, 0x11211, 0x0001}, {0x11213, 0x1122b, 0x0001},
	{0x1123f, 0x11240, 0x0001}, {0x11280, 0x11286, 0x0001}, {0x11288, 0x1128a, 0x0002}, {0x1128b, 0x1128d, 0x0001},
	{0x1128f, 0x1129d, 0x0001}, {0x1129f, 0x112a8, 0x0001}, {0x112b0, 0x112de, 0x0001}, {0x11305, 0x1130c, 0x0001},
	{0x1130f, 0x11310, 0x0001}, {0x11313, 0x11328, 0x0001}, {0x1132a, 0x11330, 0x0001}, {0x11332, 0x11333, 0x0001},
	{0x11335, 0x11339, 0x0001}, {0x1133d, 0x11350, 0x0013}, {0x1135d, 0x11361, 0x0001}, {0x11380, 0x11389, 0x0001},
	{0x1138b, 0x1138e, 0x0003}, {0x11390, 0x113b5, 0x0001}, {0x113b7, 0x113d1, 0x001a}, {0x113d3, 0x11400, 0x002d},
	{0x11401, 0x11434, 0x0001}, {0x11447, 0x1144a, 0x0001}, {0x1145f, 0x11461, 0x0001}, {0x11480, 0x114af, 0x0001},
	{0x114c4, 0x114c5, 0x0001}, {0x114c7, 0x11580, 0x00b9}, {0x11581, 0x115ae, 0x0001}, {0x115d8, 0x115db, 0x0001},
	{0x11600, 0x1162f, 0x0001}, {0x11644, 0x11680, 0x003c}, {0x11681, 0x116aa, 0x0001}, {0x116b8, 0x11700, 0x0048},
	{0x11701, 0x1171a, 0x0001}, {0x11740, 0x11746, 0x0001}, {0x11800, 0x1182b, 0x0001}, {0x118a0, 0x118df, 0x0001},
	{0x118ff, 0x11906, 0x0001}, {0x11909, 0x1190c, 0x0003}, {0x1190d, 0x11913, 0x0001}, {0x11915, 0x11916, 0x0001},
	{0x11918, 0x1192f, 0x0001}, {0x1193f, 0x11941, 0x0002}, {0x119a0, 0x119a7, 0x0001}, {0x119aa, 0x119d0, 0x0001},
	{0x119e1, 0x119e3, 0x0002}, {0x11a00, 0x11a0b, 0x000b}, {0x11a0c, 0x11a32, 0x0001}, {0x11a3a, 0x11a50, 0x0016},
	{0x11a5c, 0x11a89, 0x0001}, {0x11a9d, 0x11ab0, 0x0013}, {0x11ab1, 0x11af8, 0x0001}, {0x11bc0, 0x11be0, 0x0001},
	{0x11c00, 0x11c08, 0x0001}, {0x11c0a, 0x11c2e, 0x0001}, {0x11c40, 0x11c72, 0x0032}, {0x11c73, 0x11c8f, 0x0001},
	{0x11d00, 0x11d06, 0x0001}, {0x11d08, 0x11d09, 0x0001}, {0x11d0b, 0x11d30, 0x0001}, {0x11d46, 0x11d60, 0x001a},
	{0x11d61, 0x11d65, 0x0001}, {0x11d67, 0x11d68, 0x0001}, {0x11d6a, 0x11d89, 0x0001}, {0x11d98, 0x11db0, 0x0018},
	{0x11db1, 0x11ddb, 0x0001}, {0x11ee0, 0x11ef2, 0x0001}, {0x11f02, 0x11f04, 0x0002}, {0x11f05, 0x11f10, 0x0001},
	{0x11f12, 0x11f33, 0x0001}, {0x11fb0, 0x12000, 0x0050}, {0x12001, 0x12399, 0x0001}, {0x12480, 0x12543, 0x0001},
	{0x12f90, 0x12ff0, 0x0001}, {0x13000, 0x1342f, 0x0001}, {0x13441, 0x13446, 0x0001}, {0x13460, 0x143fa, 0x0001},
	{0x14400, 0x14646, 0x0001}, {0x16100, 0x1611d, 0x0001}, {0x16800, 0x16a38, 0x0001}, {0x16a40, 0x16a5e, 0x0001},
	{0x16a70, 0x16abe, 0x0001}, {0x16ad0, 0x16aed, 0x0001}, {0x16b00, 0x16b2f, 0x0001}, {0x16b40, 0x16b43, 0x0001},
	{0x16b63, 0x16b77, 0x0001}, {0x16b7d, 0x16b8f, 0x0001}, {0x16d40, 0x16d6c, 0x0001}, {0x16e40, 0x16e7f, 0x0001},
	{0x16ea0, 0x16eb8, 0x0001}, {0x16ebb, 0x16ed3, 0x0001}, {0x16f00, 0x16f4a, 0x0001}, {0x16f50, 0x16f93, 0x0043},
	{0x16f94, 0x16f9f, 0x0001}, {0x16fe0, 0x16fe1, 0x0001}, {0x16fe3, 0x16ff2, 0x000f}, {0x16ff3, 0x17000, 0x000d},
	{0x187ff, 0x18cd5, 0x0001}, {0x18cff, 0x18d00, 0x0001}, {0x18d1e, 0x18d80, 0x0062}, {0x18d81, 0x18df2, 0x0001},
	{0x1aff0, 0x1aff3, 0x0001}, {0x1aff5, 0x1affb, 0x0001}, {0x1affd, 0x1affe, 0x0001}, {0x1b000, 0x1b122, 0x0001},
	{0x1b132, 0x1b150, 0x001e}, {0x1b151, 0x1b152, 0x0001}, {0x1b155, 0x1b164, 0x000f}, {0x1b165, 0x1b167, 0x0001},
	{0x1b170, 0x1b2fb, 0x0001}, {0x1bc00, 0x1bc6a, 0x0001}, {0x1bc70, 0x1bc7c, 0x0001}, {0x1bc80, 0x1bc88, 0x0001},
	{0x1bc90, 0x1bc99, 0x0001}, {0x1d400, 0x1d454, 0x0001}, {0x1d456, 0x1d49c, 0x0001}, {0x1d49e, 0x1d49f, 0x0001},
	{0x1d4a2, 0x1d4a5, 0x0003}, {0x1d4a6, 0x1d4a9, 0x0003}, {0x1d4aa, 0x1d4ac, 0x0001}, {0x1d4ae, 0x1d4b9, 0x0001},
	{0x1d4bb, 0x1d4bd, 0x0002}, {0x1d4be, 0x1d4c3, 0x0001}, {0x1d4c5, 0x1d505, 0x0001}, {0x1d507, 0x1d50a, 0x0001},
	{0x1d50d, 0x1d514, 0x0001}, {0x1d516, 0x1d51c, 0x0001}, {0x1d51e, 0x1d539, 0x0001}, {0x1d53b, 0x1d53e, 0x0001},
	{0x1d540, 0x1d544, 0x0001}, {0x1d546, 0x1d54a, 0x0004}, {0x1d54b, 0x1d550, 0x0001}, {0x1d552, 0x1d6a5, 0x0001},
	{0x1d6a8, 0x1d6c0, 0x0001}, {0x1d6c2, 0x1d6da, 0x0001}, {0x1d6dc, 0x1d6fa, 0x0001}, {0x1d6fc, 0x1d714, 0x0001},
	{0x1d716, 0x1d734, 0x0001}, {0x1d736, 0x1d74e, 0x0001}, {0x1d750, 0x1d76e, 0x0001}, {0x1d770, 0x1d788, 0x0001},
	{0x1d78a, 0x1d7a8, 0x0001}, {0x1d7aa, 0x1d7c2, 0x0001}, {0x1d7c4, 0x1d7cb, 0x0001}, {0x1df00, 0x1df1e, 0x0001},
	{0x1df25, 0x1df2a, 0x0001}, {0x1e030, 0x1e06d, 0x0001}, {0x1e100, 0x1e12c, 0x0001}, {0x1e137, 0x1e13d, 0x0001},
	{0x1e14e, 0x1e290, 0x0142}, {0x1e291, 0x1e2ad, 0x0001}, {0x1e2c0, 0x1e2eb, 0x0001}, {0x1e4d0, 0x1e4eb, 0x0001},
	{0x1e5d0, 0x1e5ed, 0x0001}, {0x1e5f0, 0x1e6c0, 0x00d0}, {0x1e6c1, 0x1e6de, 0x0001}, {0x1e6e0, 0x1e6e2, 0x0001},
	{0x1e6e4, 0x1e6e5, 0x0001}, {0x1e6e7, 0x1e6ed, 0x0001}, {0x1e6f0, 0x1e6f4, 0x0001}, {0x1e6fe, 0x1e6ff, 0x0001},
	{0x1e7e0, 0x1e7e6, 0x0001}, {0x1e7e8, 0x1e7eb, 0x0001}, {0x1e7ed, 0x1e7ee, 0x0001}, {0x1e7f0, 0x1e7fe, 0x0001},
	{0x1e800, 0x1e8c4, 0x0001}, {0x1e900, 0x1e943, 0x0001}, {0x1e94b, 0x1ee00, 0x04b5}, {0x1ee01, 0x1ee03, 0x0001},
	{0x1ee05, 0x1ee1f, 0x0001}, {0x1ee21, 0x1ee22, 0x0001}, {0x1ee24, 0x1ee27, 0x0003}, {0x1ee29, 0x1ee32, 0x0001},
	{0x1ee34, 0x1ee37, 0x0001}, {0x1ee39, 0x1ee3b, 0x0002}, {0x1ee42, 0x1ee47, 0x0005}, {0x1ee49, 0x1ee4d, 0x0002},
	{0x1ee4e, 0x1ee4f, 0x0001}, {0x1ee51, 0x1ee52, 0x0001}, {0x1ee54, 0x1ee57, 0x0003}, {0x1ee59, 0x1ee61, 0x0002},
	{0x1ee62, 0x1ee64, 0x0002}, {0x1ee67, 0x1ee6a, 0x0001}, {0x1ee6c, 0x1ee72, 0x0001}, {0x1ee74, 0x1ee77, 0x0001},
	{0x1ee79, 0x1ee7c, 0x0001}, {0x1ee7e, 0x1ee80, 0x0002}, {0x1ee81, 0x1ee89, 0x0001}, {0x1ee8b, 0x1ee9b, 0x0001},
	{0x1eea1, 0x1eea3, 0x0001}, {0x1eea5, 0x1eea9, 0x0001}, {0x1eeab, 0x1eebb, 0x0001}, {0x20000, 0x2a6df, 0xa6df},
	{0x2a700, 0x2b73f, 0x103f}, {0x2b740, 0x2b81d, 0x00dd}, {0x2b820, 0x2cead, 0x168d}, {0x2ceb0, 0x2ebe0, 0x1d30},
	{0x2ebf0, 0x2ee5d, 0x026d}, {0x2f800, 0x2fa1d, 0x0001}, {0x30000, 0x3134a, 0x134a}, {0x31350, 0x323af, 0x105f},
	{0x323b0, 0x33479, 0x10c9}
};

#endif


//
//	lower16
//

static Range16 lower16[] = {
	{0x0061, 0x007a, 0x0001}, {0x00b5, 0x00df, 0x002a}, {0x00e0, 0x00f6, 0x0001}, {0x00f8, 0x00ff, 0x0001},
	{0x0101, 0x0137, 0x0002}, {0x0138, 0x0148, 0x0002}, {0x0149, 0x0177, 0x0002}, {0x017a, 0x017e, 0x0002},
	{0x017f, 0x0180, 0x0001}, {0x0183, 0x0185, 0x0002}, {0x0188, 0x018c, 0x0004}, {0x018d, 0x0192, 0x0005},
	{0x0195, 0x0199, 0x0004}, {0x019a, 0x019b, 0x0001}, {0x019e, 0x01a1, 0x0003}, {0x01a3, 0x01a5, 0x0002},
	{0x01a8, 0x01aa, 0x0002}, {0x01ab, 0x01ad, 0x0002}, {0x01b0, 0x01b4, 0x0004}, {0x01b6, 0x01b9, 0x0003},
	{0x01ba, 0x01bd, 0x0003}, {0x01be, 0x01bf, 0x0001}, {0x01c6, 0x01cc, 0x0003}, {0x01ce, 0x01dc, 0x0002},
	{0x01dd, 0x01ef, 0x0002}, {0x01f0, 0x01f3, 0x0003}, {0x01f5, 0x01f9, 0x0004}, {0x01fb, 0x0233, 0x0002},
	{0x0234, 0x0239, 0x0001}, {0x023c, 0x023f, 0x0003}, {0x0240, 0x0242, 0x0002}, {0x0247, 0x024f, 0x0002},
	{0x0250, 0x0293, 0x0001}, {0x0296, 0x02af, 0x0001}, {0x0371, 0x0373, 0x0002}, {0x0377, 0x037b, 0x0004},
	{0x037c, 0x037d, 0x0001}, {0x0390, 0x03ac, 0x001c}, {0x03ad, 0x03ce, 0x0001}, {0x03d0, 0x03d1, 0x0001},
	{0x03d5, 0x03d7, 0x0001}, {0x03d9, 0x03ef, 0x0002}, {0x03f0, 0x03f3, 0x0001}, {0x03f5, 0x03fb, 0x0003},
	{0x03fc, 0x0430, 0x0034}, {0x0431, 0x045f, 0x0001}, {0x0461, 0x0481, 0x0002}, {0x048b, 0x04bf, 0x0002},
	{0x04c2, 0x04ce, 0x0002}, {0x04cf, 0x052f, 0x0002}, {0x0560, 0x0588, 0x0001}, {0x10d0, 0x10fa, 0x0001},
	{0x10fd, 0x10ff, 0x0001}, {0x13f8, 0x13fd, 0x0001}, {0x1c80, 0x1c88, 0x0001}, {0x1c8a, 0x1d00, 0x0076},
	{0x1d01, 0x1d2b, 0x0001}, {0x1d6b, 0x1d77, 0x0001}, {0x1d79, 0x1d9a, 0x0001}, {0x1e01, 0x1e95, 0x0002},
	{0x1e96, 0x1e9d, 0x0001}, {0x1e9f, 0x1eff, 0x0002}, {0x1f00, 0x1f07, 0x0001}, {0x1f10, 0x1f15, 0x0001},
	{0x1f20, 0x1f27, 0x0001}, {0x1f30, 0x1f37, 0x0001}, {0x1f40, 0x1f45, 0x0001}, {0x1f50, 0x1f57, 0x0001},
	{0x1f60, 0x1f67, 0x0001}, {0x1f70, 0x1f7d, 0x0001}, {0x1f80, 0x1f87, 0x0001}, {0x1f90, 0x1f97, 0x0001},
	{0x1fa0, 0x1fa7, 0x0001}, {0x1fb0, 0x1fb4, 0x0001}, {0x1fb6, 0x1fb7, 0x0001}, {0x1fbe, 0x1fc2, 0x0004},
	{0x1fc3, 0x1fc4, 0x0001}, {0x1fc6, 0x1fc7, 0x0001}, {0x1fd0, 0x1fd3, 0x0001}, {0x1fd6, 0x1fd7, 0x0001},
	{0x1fe0, 0x1fe7, 0x0001}, {0x1ff2, 0x1ff4, 0x0001}, {0x1ff6, 0x1ff7, 0x0001}, {0x210a, 0x210e, 0x0004},
	{0x210f, 0x2113, 0x0004}, {0x212f, 0x2139, 0x0005}, {0x213c, 0x213d, 0x0001}, {0x2146, 0x2149, 0x0001},
	{0x214e, 0x2184, 0x0036}, {0x2c30, 0x2c5f, 0x0001}, {0x2c61, 0x2c65, 0x0004}, {0x2c66, 0x2c6c, 0x0002},
	{0x2c71, 0x2c73, 0x0002}, {0x2c74, 0x2c76, 0x0002}, {0x2c77, 0x2c7b, 0x0001}, {0x2c81, 0x2ce3, 0x0002},
	{0x2ce4, 0x2cec, 0x0008}, {0x2cee, 0x2cf3, 0x0005}, {0x2d00, 0x2d25, 0x0001}, {0x2d27, 0x2d2d, 0x0006},
	{0xa641, 0xa66d, 0x0002}, {0xa681, 0xa69b, 0x0002}, {0xa723, 0xa72f, 0x0002}, {0xa730, 0xa731, 0x0001},
	{0xa733, 0xa771, 0x0002}, {0xa772, 0xa778, 0x0001}, {0xa77a, 0xa77c, 0x0002}, {0xa77f, 0xa787, 0x0002},
	{0xa78c, 0xa78e, 0x0002}, {0xa791, 0xa793, 0x0002}, {0xa794, 0xa795, 0x0001}, {0xa797, 0xa7a9, 0x0002},
	{0xa7af, 0xa7b5, 0x0006}, {0xa7b7, 0xa7c3, 0x0002}, {0xa7c8, 0xa7ca, 0x0002}, {0xa7cd, 0xa7db, 0x0002},
	{0xa7f6, 0xa7fa, 0x0004}, {0xab30, 0xab5a, 0x0001}, {0xab60, 0xab68, 0x0001}, {0xab70, 0xabbf, 0x0001},
	{0xfb00, 0xfb06, 0x0001}, {0xfb13, 0xfb17, 0x0001}, {0xff41, 0xff5a, 0x0001}
};


//
//	lower32
//

#if defined(IMGUI_USE_WCHAR32)

static Range32 lower32[] = {
	{0x10428, 0x1044f, 0x0001}, {0x104d8, 0x104fb, 0x0001}, {0x10597, 0x105a1, 0x0001}, {0x105a3, 0x105b1, 0x0001},
	{0x105b3, 0x105b9, 0x0001}, {0x105bb, 0x105bc, 0x0001}, {0x10cc0, 0x10cf2, 0x0001}, {0x10d70, 0x10d85, 0x0001},
	{0x118c0, 0x118df, 0x0001}, {0x16e60, 0x16e7f, 0x0001}, {0x16ebb, 0x16ed3, 0x0001}, {0x1d41a, 0x1d433, 0x0001},
	{0x1d44e, 0x1d454, 0x0001}, {0x1d456, 0x1d467, 0x0001}, {0x1d482, 0x1d49b, 0x0001}, {0x1d4b6, 0x1d4b9, 0x0001},
	{0x1d4bb, 0x1d4bd, 0x0002}, {0x1d4be, 0x1d4c3, 0x0001}, {0x1d4c5, 0x1d4cf, 0x0001}, {0x1d4ea, 0x1d503, 0x0001},
	{0x1d51e, 0x1d537, 0x0001}, {0x1d552, 0x1d56b, 0x0001}, {0x1d586, 0x1d59f, 0x0001}, {0x1d5ba, 0x1d5d3, 0x0001},
	{0x1d5ee, 0x1d607, 0x0001}, {0x1d622, 0x1d63b, 0x0001}, {0x1d656, 0x1d66f, 0x0001}, {0x1d68a, 0x1d6a5, 0x0001},
	{0x1d6c2, 0x1d6da, 0x0001}, {0x1d6dc, 0x1d6e1, 0x0001}, {0x1d6fc, 0x1d714, 0x0001}, {0x1d716, 0x1d71b, 0x0001},
	{0x1d736, 0x1d74e, 0x0001}, {0x1d750, 0x1d755, 0x0001}, {0x1d770, 0x1d788, 0x0001}, {0x1d78a, 0x1d78f, 0x0001},
	{0x1d7aa, 0x1d7c2, 0x0001}, {0x1d7c4, 0x1d7c9, 0x0001}, {0x1d7cb, 0x1df00, 0x0735}, {0x1df01, 0x1df09, 0x0001},
	{0x1df0b, 0x1df1e, 0x0001}, {0x1df25, 0x1df2a, 0x0001}, {0x1e922, 0x1e943, 0x0001}
};

#endif


//
//	upper16
//

static Range16 upper16[] = {
	{0x0041, 0x005a, 0x0001}, {0x00c0, 0x00d6, 0x0001}, {0x00d8, 0x00de, 0x0001}, {0x0100, 0x0136, 0x0002},
	{0x0139, 0x0147, 0x0002}, {0x014a, 0x0178, 0x0002}, {0x0179, 0x017d, 0x0002}, {0x0181, 0x0182, 0x0001},
	{0x0184, 0x0186, 0x0002}, {0x0187, 0x0189, 0x0002}, {0x018a, 0x018b, 0x0001}, {0x018e, 0x0191, 0x0001},
	{0x0193, 0x0194, 0x0001}, {0x0196, 0x0198, 0x0001}, {0x019c, 0x019d, 0x0001}, {0x019f, 0x01a0, 0x0001},
	{0x01a2, 0x01a6, 0x0002}, {0x01a7, 0x01a9, 0x0002}, {0x01ac, 0x01ae, 0x0002}, {0x01af, 0x01b1, 0x0002},
	{0x01b2, 0x01b3, 0x0001}, {0x01b5, 0x01b7, 0x0002}, {0x01b8, 0x01bc, 0x0004}, {0x01c4, 0x01cd, 0x0003},
	{0x01cf, 0x01db, 0x0002}, {0x01de, 0x01ee, 0x0002}, {0x01f1, 0x01f4, 0x0003}, {0x01f6, 0x01f8, 0x0001},
	{0x01fa, 0x0232, 0x0002}, {0x023a, 0x023b, 0x0001}, {0x023d, 0x023e, 0x0001}, {0x0241, 0x0243, 0x0002},
	{0x0244, 0x0246, 0x0001}, {0x0248, 0x024e, 0x0002}, {0x0370, 0x0372, 0x0002}, {0x0376, 0x037f, 0x0009},
	{0x0386, 0x0388, 0x0002}, {0x0389, 0x038a, 0x0001}, {0x038c, 0x038e, 0x0002}, {0x038f, 0x0391, 0x0002},
	{0x0392, 0x03a1, 0x0001}, {0x03a3, 0x03ab, 0x0001}, {0x03cf, 0x03d2, 0x0003}, {0x03d3, 0x03d4, 0x0001},
	{0x03d8, 0x03ee, 0x0002}, {0x03f4, 0x03f7, 0x0003}, {0x03f9, 0x03fa, 0x0001}, {0x03fd, 0x042f, 0x0001},
	{0x0460, 0x0480, 0x0002}, {0x048a, 0x04c0, 0x0002}, {0x04c1, 0x04cd, 0x0002}, {0x04d0, 0x052e, 0x0002},
	{0x0531, 0x0556, 0x0001}, {0x10a0, 0x10c5, 0x0001}, {0x10c7, 0x10cd, 0x0006}, {0x13a0, 0x13f5, 0x0001},
	{0x1c89, 0x1c90, 0x0007}, {0x1c91, 0x1cba, 0x0001}, {0x1cbd, 0x1cbf, 0x0001}, {0x1e00, 0x1e94, 0x0002},
	{0x1e9e, 0x1efe, 0x0002}, {0x1f08, 0x1f0f, 0x0001}, {0x1f18, 0x1f1d, 0x0001}, {0x1f28, 0x1f2f, 0x0001},
	{0x1f38, 0x1f3f, 0x0001}, {0x1f48, 0x1f4d, 0x0001}, {0x1f59, 0x1f5f, 0x0002}, {0x1f68, 0x1f6f, 0x0001},
	{0x1fb8, 0x1fbb, 0x0001}, {0x1fc8, 0x1fcb, 0x0001}, {0x1fd8, 0x1fdb, 0x0001}, {0x1fe8, 0x1fec, 0x0001},
	{0x1ff8, 0x1ffb, 0x0001}, {0x2102, 0x2107, 0x0005}, {0x210b, 0x210d, 0x0001}, {0x2110, 0x2112, 0x0001},
	{0x2115, 0x2119, 0x0004}, {0x211a, 0x211d, 0x0001}, {0x2124, 0x212a, 0x0002}, {0x212b, 0x212d, 0x0001},
	{0x2130, 0x2133, 0x0001}, {0x213e, 0x213f, 0x0001}, {0x2145, 0x2183, 0x003e}, {0x2c00, 0x2c2f, 0x0001},
	{0x2c60, 0x2c62, 0x0002}, {0x2c63, 0x2c64, 0x0001}, {0x2c67, 0x2c6d, 0x0002}, {0x2c6e, 0x2c70, 0x0001},
	{0x2c72, 0x2c75, 0x0003}, {0x2c7e, 0x2c80, 0x0001}, {0x2c82, 0x2ce2, 0x0002}, {0x2ceb, 0x2ced, 0x0002},
	{0x2cf2, 0xa640, 0x794e}, {0xa642, 0xa66c, 0x0002}, {0xa680, 0xa69a, 0x0002}, {0xa722, 0xa72e, 0x0002},
	{0xa732, 0xa76e, 0x0002}, {0xa779, 0xa77d, 0x0002}, {0xa77e, 0xa786, 0x0002}, {0xa78b, 0xa78d, 0x0002},
	{0xa790, 0xa792, 0x0002}, {0xa796, 0xa7aa, 0x0002}, {0xa7ab, 0xa7ae, 0x0001}, {0xa7b0, 0xa7b4, 0x0001},
	{0xa7b6, 0xa7c4, 0x0002}, {0xa7c5, 0xa7c7, 0x0001}, {0xa7c9, 0xa7cb, 0x0002}, {0xa7cc, 0xa7dc, 0x0002},
	{0xa7f5, 0xff21, 0x572c}, {0xff22, 0xff3a, 0x0001}
};


//
//	upper32
//

#if defined(IMGUI_USE_WCHAR32)

static Range32 upper32[] = {
	{0x10400, 0x10427, 0x0001}, {0x104b0, 0x104d3, 0x0001}, {0x10570, 0x1057a, 0x0001}, {0x1057c, 0x1058a, 0x0001},
	{0x1058c, 0x10592, 0x0001}, {0x10594, 0x10595, 0x0001}, {0x10c80, 0x10cb2, 0x0001}, {0x10d50, 0x10d65, 0x0001},
	{0x118a0, 0x118bf, 0x0001}, {0x16e40, 0x16e5f, 0x0001}, {0x16ea0, 0x16eb8, 0x0001}, {0x1d400, 0x1d419, 0x0001},
	{0x1d434, 0x1d44d, 0x0001}, {0x1d468, 0x1d481, 0x0001}, {0x1d49c, 0x1d49e, 0x0002}, {0x1d49f, 0x1d4a5, 0x0003},
	{0x1d4a6, 0x1d4a9, 0x0003}, {0x1d4aa, 0x1d4ac, 0x0001}, {0x1d4ae, 0x1d4b5, 0x0001}, {0x1d4d0, 0x1d4e9, 0x0001},
	{0x1d504, 0x1d505, 0x0001}, {0x1d507, 0x1d50a, 0x0001}, {0x1d50d, 0x1d514, 0x0001}, {0x1d516, 0x1d51c, 0x0001},
	{0x1d538, 0x1d539, 0x0001}, {0x1d53b, 0x1d53e, 0x0001}, {0x1d540, 0x1d544, 0x0001}, {0x1d546, 0x1d54a, 0x0004},
	{0x1d54b, 0x1d550, 0x0001}, {0x1d56c, 0x1d585, 0x0001}, {0x1d5a0, 0x1d5b9, 0x0001}, {0x1d5d4, 0x1d5ed, 0x0001},
	{0x1d608, 0x1d621, 0x0001}, {0x1d63c, 0x1d655, 0x0001}, {0x1d670, 0x1d689, 0x0001}, {0x1d6a8, 0x1d6c0, 0x0001},
	{0x1d6e2, 0x1d6fa, 0x0001}, {0x1d71c, 0x1d734, 0x0001}, {0x1d756, 0x1d76e, 0x0001}, {0x1d790, 0x1d7a8, 0x0001},
	{0x1d7ca, 0x1e900, 0x1136}, {0x1e901, 0x1e921, 0x0001}
};

#endif


//
//	numbers16
//

static Range16 numbers16[] = {
	{0x0030, 0x0039, 0x0001}, {0x0660, 0x0669, 0x0001}, {0x06f0, 0x06f9, 0x0001}, {0x07c0, 0x07c9, 0x0001},
	{0x0966, 0x096f, 0x0001}, {0x09e6, 0x09ef, 0x0001}, {0x0a66, 0x0a6f, 0x0001}, {0x0ae6, 0x0aef, 0x0001},
	{0x0b66, 0x0b6f, 0x0001}, {0x0be6, 0x0bef, 0x0001}, {0x0c66, 0x0c6f, 0x0001}, {0x0ce6, 0x0cef, 0x0001},
	{0x0d66, 0x0d6f, 0x0001}, {0x0de6, 0x0def, 0x0001}, {0x0e50, 0x0e59, 0x0001}, {0x0ed0, 0x0ed9, 0x0001},
	{0x0f20, 0x0f29, 0x0001}, {0x1040, 0x1049, 0x0001}, {0x1090, 0x1099, 0x0001}, {0x17e0, 0x17e9, 0x0001},
	{0x1810, 0x1819, 0x0001}, {0x1946, 0x194f, 0x0001}, {0x19d0, 0x19d9, 0x0001}, {0x1a80, 0x1a89, 0x0001},
	{0x1a90, 0x1a99, 0x0001}, {0x1b50, 0x1b59, 0x0001}, {0x1bb0, 0x1bb9, 0x0001}, {0x1c40, 0x1c49, 0x0001},
	{0x1c50, 0x1c59, 0x0001}, {0xa620, 0xa629, 0x0001}, {0xa8d0, 0xa8d9, 0x0001}, {0xa900, 0xa909, 0x0001},
	{0xa9d0, 0xa9d9, 0x0001}, {0xa9f0, 0xa9f9, 0x0001}, {0xaa50, 0xaa59, 0x0001}, {0xabf0, 0xabf9, 0x0001},
	{0xff10, 0xff19, 0x0001}
};


//
//	numbers32
//

#if defined(IMGUI_USE_WCHAR32)

static Range32 numbers32[] = {
	{0x104a0, 0x104a9, 0x0001}, {0x10d30, 0x10d39, 0x0001}, {0x10d40, 0x10d49, 0x0001}, {0x11066, 0x1106f, 0x0001},
	{0x110f0, 0x110f9, 0x0001}, {0x11136, 0x1113f, 0x0001}, {0x111d0, 0x111d9, 0x0001}, {0x112f0, 0x112f9, 0x0001},
	{0x11450, 0x11459, 0x0001}, {0x114d0, 0x114d9, 0x0001}, {0x11650, 0x11659, 0x0001}, {0x116c0, 0x116c9, 0x0001},
	{0x116d0, 0x116e3, 0x0001}, {0x11730, 0x11739, 0x0001}, {0x118e0, 0x118e9, 0x0001}, {0x11950, 0x11959, 0x0001},
	{0x11bf0, 0x11bf9, 0x0001}, {0x11c50, 0x11c59, 0x0001}, {0x11d50, 0x11d59, 0x0001}, {0x11da0, 0x11da9, 0x0001},
	{0x11de0, 0x11de9, 0x0001}, {0x11f50, 0x11f59, 0x0001}, {0x16130, 0x16139, 0x0001}, {0x16a60, 0x16a69, 0x0001},
	{0x16ac0, 0x16ac9, 0x0001}, {0x16b50, 0x16b59, 0x0001}, {0x16d70, 0x16d79, 0x0001}, {0x1ccf0, 0x1ccf9, 0x0001},
	{0x1d7ce, 0x1d7ff, 0x0001}, {0x1e140, 0x1e149, 0x0001}, {0x1e2f0, 0x1e2f9, 0x0001}, {0x1e4f0, 0x1e4f9, 0x0001},
	{0x1e5f1, 0x1e5fa, 0x0001}, {0x1e950, 0x1e959, 0x0001}, {0x1fbf0, 0x1fbf9, 0x0001}
};

#endif


//
//	whitespace16
//

static Range16 whitespace16[] = {
	{0x0009, 0x000d, 0x0001}, {0x0020, 0x0085, 0x0065}, {0x00a0, 0x1680, 0x15e0}, {0x2000, 0x200a, 0x0001},
	{0x2028, 0x2029, 0x0001}, {0x202f, 0x205f, 0x0030}, {0x3000, 0x3000, 0x0001}
};


//
//	xidStart16
//

static Range16 xidStart16[] = {
	{0x0041, 0x005a, 0x0001}, {0x0061, 0x007a, 0x0001}, {0x00aa, 0x00b5, 0x000b}, {0x00ba, 0x00c0, 0x0006},
	{0x00c1, 0x00d6, 0x0001}, {0x00d8, 0x00f6, 0x0001}, {0x00f8, 0x02c1, 0x0001}, {0x02c6, 0x02d1, 0x0001},
	{0x02e0, 0x02e4, 0x0001}, {0x02ec, 0x02ee, 0x0002}, {0x0370, 0x0374, 0x0001}, {0x0376, 0x0377, 0x0001},
	{0x037b, 0x037d, 0x0001}, {0x037f, 0x0386, 0x0007}, {0x0388, 0x038a, 0x0001}, {0x038c, 0x038e, 0x0002},
	{0x038f, 0x03a1, 0x0001}, {0x03a3, 0x03f5, 0x0001}, {0x03f7, 0x0481, 0x0001}, {0x048a, 0x052f, 0x0001},
	{0x0531, 0x0556, 0x0001}, {0x0559, 0x0560, 0x0007}, {0x0561, 0x0588, 0x0001}, {0x05d0, 0x05ea, 0x0001},
	{0x05ef, 0x05f2, 0x0001}, {0x0620, 0x064a, 0x0001}, {0x066e, 0x066f, 0x0001}, {0x0671, 0x06d3, 0x0001},
	{0x06d5, 0x06e5, 0x0010}, {0x06e6, 0x06ee, 0x0008}, {0x06ef, 0x06fa, 0x000b}, {0x06fb, 0x06fc, 0x0001},
	{0x06ff, 0x0710, 0x0011}, {0x0712, 0x072f, 0x0001}, {0x074d, 0x07a5, 0x0001}, {0x07b1, 0x07ca, 0x0019},
	{0x07cb, 0x07ea, 0x0001}, {0x07f4, 0x07f5, 0x0001}, {0x07fa, 0x0800, 0x0006}, {0x0801, 0x0815, 0x0001},
	{0x081a, 0x0824, 0x000a}, {0x0828, 0x0840, 0x0018}, {0x0841, 0x0858, 0x0001}, {0x0860, 0x086a, 0x0001},
	{0x0870, 0x0887, 0x0001}, {0x0889, 0x088f, 0x0001}, {0x08a0, 0x08c9, 0x0001}, {0x0904, 0x0939, 0x0001},
	{0x093d, 0x0950, 0x0013}, {0x0958, 0x0961, 0x0001}, {0x0971, 0x0980, 0x0001}, {0x0985, 0x098c, 0x0001},
	{0x098f, 0x0990, 0x0001}, {0x0993, 0x09a8, 0x0001}, {0x09aa, 0x09b0, 0x0001}, {0x09b2, 0x09b6, 0x0004},
	{0x09b7, 0x09b9, 0x0001}, {0x09bd, 0x09ce, 0x0011}, {0x09dc, 0x09dd, 0x0001}, {0x09df, 0x09e1, 0x0001},
	{0x09f0, 0x09f1, 0x0001}, {0x09fc, 0x0a05, 0x0009}, {0x0a06, 0x0a0a, 0x0001}, {0x0a0f, 0x0a10, 0x0001},
	{0x0a13, 0x0a28, 0x0001}, {0x0a2a, 0x0a30, 0x0001}, {0x0a32, 0x0a33, 0x0001}, {0x0a35, 0x0a36, 0x0001},
	{0x0a38, 0x0a39, 0x0001}, {0x0a59, 0x0a5c, 0x0001}, {0x0a5e, 0x0a72, 0x0014}, {0x0a73, 0x0a74, 0x0001},
	{0x0a85, 0x0a8d, 0x0001}, {0x0a8f, 0x0a91, 0x0001}, {0x0a93, 0x0aa8, 0x0001}, {0x0aaa, 0x0ab0, 0x0001},
	{0x0ab2, 0x0ab3, 0x0001}, {0x0ab5, 0x0ab9, 0x0001}, {0x0abd, 0x0ad0, 0x0013}, {0x0ae0, 0x0ae1, 0x0001},
	{0x0af9, 0x0b05, 0x000c}, {0x0b06, 0x0b0c, 0x0001}, {0x0b0f, 0x0b10, 0x0001}, {0x0b13, 0x0b28, 0x0001},
	{0x0b2a, 0x0b30, 0x0001}, {0x0b32, 0x0b33, 0x0001}, {0x0b35, 0x0b39, 0x0001}, {0x0b3d, 0x0b5c, 0x001f},
	{0x0b5d, 0x0b5f, 0x0002}, {0x0b60, 0x0b61, 0x0001}, {0x0b71, 0x0b83, 0x0012}, {0x0b85, 0x0b8a, 0x0001},
	{0x0b8e, 0x0b90, 0x0001}, {0x0b92, 0x0b95, 0x0001}, {0x0b99, 0x0b9a, 0x0001}, {0x0b9c, 0x0b9e, 0x0002},
	{0x0b9f, 0x0ba3, 0x0004}, {0x0ba4, 0x0ba8, 0x0004}, {0x0ba9, 0x0baa, 0x0001}, {0x0bae, 0x0bb9, 0x0001},
	{0x0bd0, 0x0c05, 0x0035}, {0x0c06, 0x0c0c, 0x0001}, {0x0c0e, 0x0c10, 0x0001}, {0x0c12, 0x0c28, 0x0001},
	{0x0c2a, 0x0c39, 0x0001}, {0x0c3d, 0x0c58, 0x001b}, {0x0c59, 0x0c5a, 0x0001}, {0x0c5c, 0x0c5d, 0x0001},
	{0x0c60, 0x0c61, 0x0001}, {0x0c80, 0x0c85, 0x0005}, {0x0c86, 0x0c8c, 0x0001}, {0x0c8e, 0x0c90, 0x0001},
	{0x0c92, 0x0ca8, 0x0001}, {0x0caa, 0x0cb3, 0x0001}, {0x0cb5, 0x0cb9, 0x0001}, {0x0cbd, 0x0cdc, 0x001f},
	{0x0cdd, 0x0cde, 0x0001}, {0x0ce0, 0x0ce1, 0x0001}, {0x0cf1, 0x0cf2, 0x0001}, {0x0d04, 0x0d0c, 0x0001},
	{0x0d0e, 0x0d10, 0x0001}, {0x0d12, 0x0d3a, 0x0001}, {0x0d3d, 0x0d4e, 0x0011}, {0x0d54, 0x0d56, 0x0001},
	{0x0d5f, 0x0d61, 0x0001}, {0x0d7a, 0x0d7f, 0x0001}, {0x0d85, 0x0d96, 0x0001}, {0x0d9a, 0x0db1, 0x0001},
	{0x0db3, 0x0dbb, 0x0001}, {0x0dbd, 0x0dc0, 0x0003}, {0x0dc1, 0x0dc6, 0x0001}, {0x0e01, 0x0e30, 0x0001},
	{0x0e32, 0x0e40, 0x000e}, {0x0e41, 0x0e46, 0x0001}, {0x0e81, 0x0e82, 0x0001}, {0x0e84, 0x0e86, 0x0002},
	{0x0e87, 0x0e8a, 0x0001}, {0x0e8c, 0x0ea3, 0x0001}, {0x0ea5, 0x0ea7, 0x0002}, {0x0ea8, 0x0eb0, 0x0001},
	{0x0eb2, 0x0ebd, 0x000b}, {0x0ec0, 0x0ec4, 0x0001}, {0x0ec6, 0x0edc, 0x0016}, {0x0edd, 0x0edf, 0x0001},
	{0x0f00, 0x0f40, 0x0040}, {0x0f41, 0x0f47, 0x0001}, {0x0f49, 0x0f6c, 0x0001}, {0x0f88, 0x0f8c, 0x0001},
	{0x1000, 0x102a, 0x0001}, {0x103f, 0x1050, 0x0011}, {0x1051, 0x1055, 0x0001}, {0x105a, 0x105d, 0x0001},
	{0x1061, 0x1065, 0x0004}, {0x1066, 0x106e, 0x0008}, {0x106f, 0x1070, 0x0001}, {0x1075, 0x1081, 0x0001},
	{0x108e, 0x10a0, 0x0012}, {0x10a1, 0x10c5, 0x0001}, {0x10c7, 0x10cd, 0x0006}, {0x10d0, 0x10fa, 0x0001},
	{0x10fc, 0x1248, 0x0001}, {0x124a, 0x124d, 0x0001}, {0x1250, 0x1256, 0x0001}, {0x1258, 0x125a, 0x0002},
	{0x125b, 0x125d, 0x0001}, {0x1260, 0x1288, 0x0001}, {0x128a, 0x128d, 0x0001}, {0x1290, 0x12b0, 0x0001},
	{0x12b2, 0x12b5, 0x0001}, {0x12b8, 0x12be, 0x0001}, {0x12c0, 0x12c2, 0x0002}, {0x12c3, 0x12c5, 0x0001},
	{0x12c8, 0x12d6, 0x0001}, {0x12d8, 0x1310, 0x0001}, {0x1312, 0x1315, 0x0001}, {0x1318, 0x135a, 0x0001},
	{0x1380, 0x138f, 0x0001}, {0x13a0, 0x13f5, 0x0001}, {0x13f8, 0x13fd, 0x0001}, {0x1401, 0x166c, 0x0001},
	{0x166f, 0x167f, 0x0001}, {0x1681, 0x169a, 0x0001}, {0x16a0, 0x16ea, 0x0001}, {0x16ee, 0x16f8, 0x0001},
	{0x1700, 0x1711, 0x0001}, {0x171f, 0x1731, 0x0001}, {0x1740, 0x1751, 0x0001}, {0x1760, 0x176c, 0x0001},
	{0x176e, 0x1770, 0x0001}, {0x1780, 0x17b3, 0x0001}, {0x17d7, 0x17dc, 0x0005}, {0x1820, 0x1878, 0x0001},
	{0x1880, 0x18a8, 0x0001}, {0x18aa, 0x18b0, 0x0006}, {0x18b1, 0x18f5, 0x0001}, {0x1900, 0x191e, 0x0001},
	{0x1950, 0x196d, 0x0001}, {0x1970, 0x1974, 0x0001}, {0x1980, 0x19ab, 0x0001}, {0x19b0, 0x19c9, 0x0001},
	{0x1a00, 0x1a16, 0x0001}, {0x1a20, 0x1a54, 0x0001}, {0x1aa7, 0x1b05, 0x005e}, {0x1b06, 0x1b33, 0x0001},
	{0x1b45, 0x1b4c, 0x0001}, {0x1b83, 0x1ba0, 0x0001}, {0x1bae, 0x1baf, 0x0001}, {0x1bba, 0x1be5, 0x0001},
	{0x1c00, 0x1c23, 0x0001}, {0x1c4d, 0x1c4f, 0x0001}, {0x1c5a, 0x1c7d, 0x0001}, {0x1c80, 0x1c8a, 0x0001},
	{0x1c90, 0x1cba, 0x0001}, {0x1cbd, 0x1cbf, 0x0001}, {0x1ce9, 0x1cec, 0x0001}, {0x1cee, 0x1cf3, 0x0001},
	{0x1cf5, 0x1cf6, 0x0001}, {0x1cfa, 0x1d00, 0x0006}, {0x1d01, 0x1dbf, 0x0001}, {0x1e00, 0x1f15, 0x0001},
	{0x1f18, 0x1f1d, 0x0001}, {0x1f20, 0x1f45, 0x0001}, {0x1f48, 0x1f4d, 0x0001}, {0x1f50, 0x1f57, 0x0001},
	{0x1f59, 0x1f5f, 0x0002}, {0x1f60, 0x1f7d, 0x0001}, {0x1f80, 0x1fb4, 0x0001}, {0x1fb6, 0x1fbc, 0x0001},
	{0x1fbe, 0x1fc2, 0x0004}, {0x1fc3, 0x1fc4, 0x0001}, {0x1fc6, 0x1fcc, 0x0001}, {0x1fd0, 0x1fd3, 0x0001},
	{0x1fd6, 0x1fdb, 0x0001}, {0x1fe0, 0x1fec, 0x0001}, {0x1ff2, 0x1ff4, 0x0001}, {0x1ff6, 0x1ffc, 0x0001},
	{0x2071, 0x207f, 0x000e}, {0x2090, 0x209c, 0x0001}, {0x2102, 0x2107, 0x0005}, {0x210a, 0x2113, 0x0001},
	{0x2115, 0x2118, 0x0003}, {0x2119, 0x211d, 0x0001}, {0x2124, 0x212a, 0x0002}, {0x212b, 0x2139, 0x0001},
	{0x213c, 0x213f, 0x0001}, {0x2145, 0x2149, 0x0001}, {0x214e, 0x2160, 0x0012}, {0x2161, 0x2188, 0x0001},
	{0x2c00, 0x2ce4, 0x0001}, {0x2ceb, 0x2cee, 0x0001}, {0x2cf2, 0x2cf3, 0x0001}, {0x2d00, 0x2d25, 0x0001},
	{0x2d27, 0x2d2d, 0x0006}, {0x2d30, 0x2d67, 0x0001}, {0x2d6f, 0x2d80, 0x0011}, {0x2d81, 0x2d96, 0x0001},
	{0x2da0, 0x2da6, 0x0001}, {0x2da8, 0x2dae, 0x0001}, {0x2db0, 0x2db6, 0x0001}, {0x2db8, 0x2dbe, 0x0001},
	{0x2dc0, 0x2dc6, 0x0001}, {0x2dc8, 0x2dce, 0x0001}, {0x2dd0, 0x2dd6, 0x0001}, {0x2dd8, 0x2dde, 0x0001},
	{0x3005, 0x3007, 0x0001}, {0x3021, 0x3029, 0x0001}, {0x3031, 0x3035, 0x0001}, {0x3038, 0x303c, 0x0001},
	{0x3041, 0x3096, 0x0001}, {0x309d, 0x309f, 0x0001}, {0x30a1, 0x30fa, 0x0001}, {0x30fc, 0x30ff, 0x0001},
	{0x3105, 0x312f, 0x0001}, {0x3131, 0x318e, 0x0001}, {0x31a0, 0x31bf, 0x0001}, {0x31f0, 0x31ff, 0x0001},
	{0x3400, 0x4dbf, 0x0001}, {0x4e00, 0xa48c, 0x0001}, {0xa4d0, 0xa4fd, 0x0001}, {0xa500, 0xa60c, 0x0001},
	{0xa610, 0xa61f, 0x0001}, {0xa62a, 0xa62b, 0x0001}, {0xa640, 0xa66e, 0x0001}, {0xa67f, 0xa69d, 0x0001},
	{0xa6a0, 0xa6ef, 0x0001}, {0xa717, 0xa71f, 0x0001}, {0xa722, 0xa788, 0x0001}, {0xa78b, 0xa7dc, 0x0001},
	{0xa7f1, 0xa801, 0x0001}, {0xa803, 0xa805, 0x0001}, {0xa807, 0xa80a, 0x0001}, {0xa80c, 0xa822, 0x0001},
	{0xa840, 0xa873, 0x0001}, {0xa882, 0xa8b3, 0x0001}, {0xa8f2, 0xa8f7, 0x0001}, {0xa8fb, 0xa8fd, 0x0002},
	{0xa8fe, 0xa90a, 0x000c}, {0xa90b, 0xa925, 0x0001}, {0xa930, 0xa946, 0x0001}, {0xa960, 0xa97c, 0x0001},
	{0xa984, 0xa9b2, 0x0001}, {0xa9cf, 0xa9e0, 0x0011}, {0xa9e1, 0xa9e4, 0x0001}, {0xa9e6, 0xa9ef, 0x0001},
	{0xa9fa, 0xa9fe, 0x0001}, {0xaa00, 0xaa28, 0x0001}, {0xaa40, 0xaa42, 0x0001}, {0xaa44, 0xaa4b, 0x0001},
	{0xaa60, 0xaa76, 0x0001}, {0xaa7a, 0xaa7e, 0x0004}, {0xaa7f, 0xaaaf, 0x0001}, {0xaab1, 0xaab5, 0x0004},
	{0xaab6, 0xaab9, 0x0003}, {0xaaba, 0xaabd, 0x0001}, {0xaac0, 0xaac2, 0x0002}, {0xaadb, 0xaadd, 0x0001},
	{0xaae0, 0xaaea, 0x0001}, {0xaaf2, 0xaaf4, 0x0001}, {0xab01, 0xab06, 0x0001}, {0xab09, 0xab0e, 0x0001},
	{0xab11, 0xab16, 0x0001}, {0xab20, 0xab26, 0x0001}, {0xab28, 0xab2e, 0x0001}, {0xab30, 0xab5a, 0x0001},
	{0xab5c, 0xab69, 0x0001}, {0xab70, 0xabe2, 0x0001}, {0xac00, 0xd7a3, 0x0001}, {0xd7b0, 0xd7c6, 0x0001},
	{0xd7cb, 0xd7fb, 0x0001}, {0xf900, 0xfa6d, 0x0001}, {0xfa70, 0xfad9, 0x0001}, {0xfb00, 0xfb06, 0x0001},
	{0xfb13, 0xfb17, 0x0001}, {0xfb1d, 0xfb1f, 0x0002}, {0xfb20, 0xfb28, 0x0001}, {0xfb2a, 0xfb36, 0x0001},
	{0xfb38, 0xfb3c, 0x0001}, {0xfb3e, 0xfb40, 0x0002}, {0xfb41, 0xfb43, 0x0002}, {0xfb44, 0xfb46, 0x0002},
	{0xfb47, 0xfbb1, 0x0001}, {0xfbd3, 0xfc5d, 0x0001}, {0xfc64, 0xfd3d, 0x0001}, {0xfd50, 0xfd8f, 0x0001},
	{0xfd92, 0xfdc7, 0x0001}, {0xfdf0, 0xfdf9, 0x0001}, {0xfe71, 0xfe73, 0x0002}, {0xfe77, 0xfe7f, 0x0002},
	{0xfe80, 0xfefc, 0x0001}, {0xff21, 0xff3a, 0x0001}, {0xff41, 0xff5a, 0x0001}, {0xff66, 0xff9d, 0x0001},
	{0xffa0, 0xffbe, 0x0001}, {0xffc2, 0xffc7, 0x0001}, {0xffca, 0xffcf, 0x0001}, {0xffd2, 0xffd7, 0x0001},
	{0xffda, 0xffdc, 0x0001}
};


//
//	xidStart32
//

#if defined(IMGUI_USE_WCHAR32)

static Range32 xidStart32[] = {
	{0x10000, 0x1000b, 0x0001}, {0x1000d, 0x10026, 0x0001}, {0x10028, 0x1003a, 0x0001}, {0x1003c, 0x1003d, 0x0001},
	{0x1003f, 0x1004d, 0x0001}, {0x10050, 0x1005d, 0x0001}, {0x10080, 0x100fa, 0x0001}, {0x10140, 0x10174, 0x0001},
	{0x10280, 0x1029c, 0x0001}, {0x102a0, 0x102d0, 0x0001}, {0x10300, 0x1031f, 0x0001}, {0x1032d, 0x1034a, 0x0001},
	{0x10350, 0x10375, 0x0001}, {0x10380, 0x1039d, 0x0001}, {0x103a0, 0x103c3, 0x0001}, {0x103c8, 0x103cf, 0x0001},
	{0x103d1, 0x103d5, 0x0001}, {0x10400, 0x1049d, 0x0001}, {0x104b0, 0x104d3, 0x0001}, {0x104d8, 0x104fb, 0x0001},
	{0x10500, 0x10527, 0x0001}, {0x10530, 0x10563, 0x0001}, {0x10570, 0x1057a, 0x0001}, {0x1057c, 0x1058a, 0x0001},
	{0x1058c, 0x10592, 0x0001}, {0x10594, 0x10595, 0x0001}, {0x10597, 0x105a1, 0x0001}, {0x105a3, 0x105b1, 0x0001},
	{0x105b3, 0x105b9, 0x0001}, {0x105bb, 0x105bc, 0x0001}, {0x105c0, 0x105f3, 0x0001}, {0x10600, 0x10736, 0x0001},
	{0x10740, 0x10755, 0x0001}, {0x10760, 0x10767, 0x0001}, {0x10780, 0x10785, 0x0001}, {0x10787, 0x107b0, 0x0001},
	{0x107b2, 0x107ba, 0x0001}, {0x10800, 0x10805, 0x0001}, {0x10808, 0x1080a, 0x0002}, {0x1080b, 0x10835, 0x0001},
	{0x10837, 0x10838, 0x0001}, {0x1083c, 0x1083f, 0x0003}, {0x10840, 0x10855, 0x0001}, {0x10860, 0x10876, 0x0001},
	{0x10880, 0x1089e, 0x0001}, {0x108e0, 0x108f2, 0x0001}, {0x108f4, 0x108f5, 0x0001}, {0x10900, 0x10915, 0x0001},
	{0x10920, 0x10939, 0x0001}, {0x10940, 0x10959, 0x0001}, {0x10980, 0x109b7, 0x0001}, {0x109be, 0x109bf, 0x0001},
	{0x10a00, 0x10a10, 0x0010}, {0x10a11, 0x10a13, 0x0001}, {0x10a15, 0x10a17, 0x0001}, {0x10a19, 0x10a35, 0x0001},
	{0x10a60, 0x10a7c, 0x0001}, {0x10a80, 0x10a9c, 0x0001}, {0x10ac0, 0x10ac7, 0x0001}, {0x10ac9, 0x10ae4, 0x0001},
	{0x10b00, 0x10b35, 0x0001}, {0x10b40, 0x10b55, 0x0001}, {0x10b60, 0x10b72, 0x0001}, {0x10b80, 0x10b91, 0x0001},
	{0x10c00, 0x10c48, 0x0001}, {0x10c80, 0x10cb2, 0x0001}, {0x10cc0, 0x10cf2, 0x0001}, {0x10d00, 0x10d23, 0x0001},
	{0x10d4a, 0x10d65, 0x0001}, {0x10d6f, 0x10d85, 0x0001}, {0x10e80, 0x10ea9, 0x0001}, {0x10eb0, 0x10eb1, 0x0001},
	{0x10ec2, 0x10ec7, 0x0001}, {0x10f00, 0x10f1c, 0x0001}, {0x10f27, 0x10f30, 0x0009}, {0x10f31, 0x10f45, 0x0001},
	{0x10f70, 0x10f81, 0x0001}, {0x10fb0, 0x10fc4, 0x0001}, {0x10fe0, 0x10ff6, 0x0001}, {0x11003, 0x11037, 0x0001},
	{0x11071, 0x11072, 0x0001}, {0x11075, 0x11083, 0x000e}, {0x11084, 0x110af, 0x0001}, {0x110d0, 0x110e8, 0x0001},
	{0x11103, 0x11126, 0x0001}, {0x11144, 0x11147, 0x0003}, {0x11150, 0x11172, 0x0001}, {0x11176, 0x11183, 0x000d},
	{0x11184, 0x111b2, 0x0001}, {0x111c1, 0x111c4, 0x0001}, {0x111da, 0x111dc, 0x0002}, {0x11200, 0x11211, 0x0001},
	{0x11213, 0x1122b, 0x0001}, {0x1123f, 0x11240, 0x0001}, {0x11280, 0x11286, 0x0001}, {0x11288, 0x1128a, 0x0002},
	{0x1128b, 0x1128d, 0x0001}, {0x1128f, 0x1129d, 0x0001}, {0x1129f, 0x112a8, 0x0001}, {0x112b0, 0x112de, 0x0001},
	{0x11305, 0x1130c, 0x0001}, {0x1130f, 0x11310, 0x0001}, {0x11313, 0x11328, 0x0001}, {0x1132a, 0x11330, 0x0001},
	{0x11332, 0x11333, 0x0001}, {0x11335, 0x11339, 0x0001}, {0x1133d, 0x11350, 0x0013}, {0x1135d, 0x11361, 0x0001},
	{0x11380, 0x11389, 0x0001}, {0x1138b, 0x1138e, 0x0003}, {0x11390, 0x113b5, 0x0001}, {0x113b7, 0x113d1, 0x001a},
	{0x113d3, 0x11400, 0x002d}, {0x11401, 0x11434, 0x0001}, {0x11447, 0x1144a, 0x0001}, {0x1145f, 0x11461, 0x0001},
	{0x11480, 0x114af, 0x0001}, {0x114c4, 0x114c5, 0x0001}, {0x114c7, 0x11580, 0x00b9}, {0x11581, 0x115ae, 0x0001},
	{0x115d8, 0x115db, 0x0001}, {0x11600, 0x1162f, 0x0001}, {0x11644, 0x11680, 0x003c}, {0x11681, 0x116aa, 0x0001},
	{0x116b8, 0x11700, 0x0048}, {0x11701, 0x1171a, 0x0001}, {0x11740, 0x11746, 0x0001}, {0x11800, 0x1182b, 0x0001},
	{0x118a0, 0x118df, 0x0001}, {0x118ff, 0x11906, 0x0001}, {0x11909, 0x1190c, 0x0003}, {0x1190d, 0x11913, 0x0001},
	{0x11915, 0x11916, 0x0001}, {0x11918, 0x1192f, 0x0001}, {0x1193f, 0x11941, 0x0002}, {0x119a0, 0x119a7, 0x0001},
	{0x119aa, 0x119d0, 0x0001}, {0x119e1, 0x119e3, 0x0002}, {0x11a00, 0x11a0b, 0x000b}, {0x11a0c, 0x11a32, 0x0001},
	{0x11a3a, 0x11a50, 0x0016}, {0x11a5c, 0x11a89, 0x0001}, {0x11a9d, 0x11ab0, 0x0013}, {0x11ab1, 0x11af8, 0x0001},
	{0x11bc0, 0x11be0, 0x0001}, {0x11c00, 0x11c08, 0x0001}, {0x11c0a, 0x11c2e, 0x0001}, {0x11c40, 0x11c72, 0x0032},
	{0x11c73, 0x11c8f, 0x0001}, {0x11d00, 0x11d06, 0x0001}, {0x11d08, 0x11d09, 0x0001}, {0x11d0b, 0x11d30, 0x0001},
	{0x11d46, 0x11d60, 0x001a}, {0x11d61, 0x11d65, 0x0001}, {0x11d67, 0x11d68, 0x0001}, {0x11d6a, 0x11d89, 0x0001},
	{0x11d98, 0x11db0, 0x0018}, {0x11db1, 0x11ddb, 0x0001}, {0x11ee0, 0x11ef2, 0x0001}, {0x11f02, 0x11f04, 0x0002},
	{0x11f05, 0x11f10, 0x0001}, {0x11f12, 0x11f33, 0x0001}, {0x11fb0, 0x12000, 0x0050}, {0x12001, 0x12399, 0x0001},
	{0x12400, 0x1246e, 0x0001}, {0x12480, 0x12543, 0x0001}, {0x12f90, 0x12ff0, 0x0001}, {0x13000, 0x1342f, 0x0001},
	{0x13441, 0x13446, 0x0001}, {0x13460, 0x143fa, 0x0001}, {0x14400, 0x14646, 0x0001}, {0x16100, 0x1611d, 0x0001},
	{0x16800, 0x16a38, 0x0001}, {0x16a40, 0x16a5e, 0x0001}, {0x16a70, 0x16abe, 0x0001}, {0x16ad0, 0x16aed, 0x0001},
	{0x16b00, 0x16b2f, 0x0001}, {0x16b40, 0x16b43, 0x0001}, {0x16b63, 0x16b77, 0x0001}, {0x16b7d, 0x16b8f, 0x0001},
	{0x16d40, 0x16d6c, 0x0001}, {0x16e40, 0x16e7f, 0x0001}, {0x16ea0, 0x16eb8, 0x0001}, {0x16ebb, 0x16ed3, 0x0001},
	{0x16f00, 0x16f4a, 0x0001}, {0x16f50, 0x16f93, 0x0043}, {0x16f94, 0x16f9f, 0x0001}, {0x16fe0, 0x16fe1, 0x0001},
	{0x16fe3, 0x16ff2, 0x000f}, {0x16ff3, 0x16ff6, 0x0001}, {0x17000, 0x18cd5, 0x0001}, {0x18cff, 0x18d1e, 0x0001},
	{0x18d80, 0x18df2, 0x0001}, {0x1aff0, 0x1aff3, 0x0001}, {0x1aff5, 0x1affb, 0x0001}, {0x1affd, 0x1affe, 0x0001},
	{0x1b000, 0x1b122, 0x0001}, {0x1b132, 0x1b150, 0x001e}, {0x1b151, 0x1b152, 0x0001}, {0x1b155, 0x1b164, 0x000f},
	{0x1b165, 0x1b167, 0x0001}, {0x1b170, 0x1b2fb, 0x0001}, {0x1bc00, 0x1bc6a, 0x0001}, {0x1bc70, 0x1bc7c, 0x0001},
	{0x1bc80, 0x1bc88, 0x0001}, {0x1bc90, 0x1bc99, 0x0001}, {0x1d400, 0x1d454, 0x0001}, {0x1d456, 0x1d49c, 0x0001},
	{0x1d49e, 0x1d49f, 0x0001}, {0x1d4a2, 0x1d4a5, 0x0003}, {0x1d4a6, 0x1d4a9, 0x0003}, {0x1d4aa, 0x1d4ac, 0x0001},
	{0x1d4ae, 0x1d4b9, 0x0001}, {0x1d4bb, 0x1d4bd, 0x0002}, {0x1d4be, 0x1d4c3, 0x0001}, {0x1d4c5, 0x1d505, 0x0001},
	{0x1d507, 0x1d50a, 0x0001}, {0x1d50d, 0x1d514, 0x0001}, {0x1d516, 0x1d51c, 0x0001}, {0x1d51e, 0x1d539, 0x0001},
	{0x1d53b, 0x1d53e, 0x0001}, {0x1d540, 0x1d544, 0x0001}, {0x1d546, 0x1d54a, 0x0004}, {0x1d54b, 0x1d550, 0x0001},
	{0x1d552, 0x1d6a5, 0x0001}, {0x1d6a8, 0x1d6c0, 0x0001}, {0x1d6c2, 0x1d6da, 0x0001}, {0x1d6dc, 0x1d6fa, 0x0001},
	{0x1d6fc, 0x1d714, 0x0001}, {0x1d716, 0x1d734, 0x0001}, {0x1d736, 0x1d74e, 0x0001}, {0x1d750, 0x1d76e, 0x0001},
	{0x1d770, 0x1d788, 0x0001}, {0x1d78a, 0x1d7a8, 0x0001}, {0x1d7aa, 0x1d7c2, 0x0001}, {0x1d7c4, 0x1d7cb, 0x0001},
	{0x1df00, 0x1df1e, 0x0001}, {0x1df25, 0x1df2a, 0x0001}, {0x1e030, 0x1e06d, 0x0001}, {0x1e100, 0x1e12c, 0x0001},
	{0x1e137, 0x1e13d, 0x0001}, {0x1e14e, 0x1e290, 0x0142}, {0x1e291, 0x1e2ad, 0x0001}, {0x1e2c0, 0x1e2eb, 0x0001},
	{0x1e4d0, 0x1e4eb, 0x0001}, {0x1e5d0, 0x1e5ed, 0x0001}, {0x1e5f0, 0x1e6c0, 0x00d0}, {0x1e6c1, 0x1e6de, 0x0001},
	{0x1e6e0, 0x1e6e2, 0x0001}, {0x1e6e4, 0x1e6e5, 0x0001}, {0x1e6e7, 0x1e6ed, 0x0001}, {0x1e6f0, 0x1e6f4, 0x0001},
	{0x1e6fe, 0x1e6ff, 0x0001}, {0x1e7e0, 0x1e7e6, 0x0001}, {0x1e7e8, 0x1e7eb, 0x0001}, {0x1e7ed, 0x1e7ee, 0x0001},
	{0x1e7f0, 0x1e7fe, 0x0001}, {0x1e800, 0x1e8c4, 0x0001}, {0x1e900, 0x1e943, 0x0001}, {0x1e94b, 0x1ee00, 0x04b5},
	{0x1ee01, 0x1ee03, 0x0001}, {0x1ee05, 0x1ee1f, 0x0001}, {0x1ee21, 0x1ee22, 0x0001}, {0x1ee24, 0x1ee27, 0x0003},
	{0x1ee29, 0x1ee32, 0x0001}, {0x1ee34, 0x1ee37, 0x0001}, {0x1ee39, 0x1ee3b, 0x0002}, {0x1ee42, 0x1ee47, 0x0005},
	{0x1ee49, 0x1ee4d, 0x0002}, {0x1ee4e, 0x1ee4f, 0x0001}, {0x1ee51, 0x1ee52, 0x0001}, {0x1ee54, 0x1ee57, 0x0003},
	{0x1ee59, 0x1ee61, 0x0002}, {0x1ee62, 0x1ee64, 0x0002}, {0x1ee67, 0x1ee6a, 0x0001}, {0x1ee6c, 0x1ee72, 0x0001},
	{0x1ee74, 0x1ee77, 0x0001}, {0x1ee79, 0x1ee7c, 0x0001}, {0x1ee7e, 0x1ee80, 0x0002}, {0x1ee81, 0x1ee89, 0x0001},
	{0x1ee8b, 0x1ee9b, 0x0001}, {0x1eea1, 0x1eea3, 0x0001}, {0x1eea5, 0x1eea9, 0x0001}, {0x1eeab, 0x1eebb, 0x0001},
	{0x20000, 0x2a6df, 0x0001}, {0x2a700, 0x2b81d, 0x0001}, {0x2b820, 0x2cead, 0x0001}, {0x2ceb0, 0x2ebe0, 0x0001},
	{0x2ebf0, 0x2ee5d, 0x0001}, {0x2f800, 0x2fa1d, 0x0001}, {0x30000, 0x3134a, 0x0001}, {0x31350, 0x33479, 0x0001}
};

#endif


//
//	xidContinue16
//

static Range16 xidContinue16[] = {
	{0x0030, 0x0039, 0x0001}, {0x0041, 0x005a, 0x0001}, {0x005f, 0x0061, 0x0002}, {0x0062, 0x007a, 0x0001},
	{0x00aa, 0x00b5, 0x000b}, {0x00b7, 0x00ba, 0x0003}, {0x00c0, 0x00d6, 0x0001}, {0x00d8, 0x00f6, 0x0001},
	{0x00f8, 0x02c1, 0x0001}, {0x02c6, 0x02d1, 0x0001}, {0x02e0, 0x02e4, 0x0001}, {0x02ec, 0x02ee, 0x0002},
	{0x0300, 0x0374, 0x0001}, {0x0376, 0x0377, 0x0001}, {0x037b, 0x037d, 0x0001}, {0x037f, 0x0386, 0x0007},
	{0x0387, 0x038a, 0x0001}, {0x038c, 0x038e, 0x0002}, {0x038f, 0x03a1, 0x0001}, {0x03a3, 0x03f5, 0x0001},
	{0x03f7, 0x0481, 0x0001}, {0x0483, 0x0487, 0x0001}, {0x048a, 0x052f, 0x0001}, {0x0531, 0x0556, 0x0001},
	{0x0559, 0x0560, 0x0007}, {0x0561, 0x0588, 0x0001}, {0x0591, 0x05bd, 0x0001}, {0x05bf, 0x05c1, 0x0002},
	{0x05c2, 0x05c4, 0x0002}, {0x05c5, 0x05c7, 0x0002}, {0x05d0, 0x05ea, 0x0001}, {0x05ef, 0x05f2, 0x0001},
	{0x0610, 0x061a, 0x0001}, {0x0620, 0x0669, 0x0001}, {0x066e, 0x06d3, 0x0001}, {0x06d5, 0x06dc, 0x0001},
	{0x06df, 0x06e8, 0x0001}, {0x06ea, 0x06fc, 0x0001}, {0x06ff, 0x0710, 0x0011}, {0x0711, 0x074a, 0x0001},
	{0x074d, 0x07b1, 0x0001}, {0x07c0, 0x07f5, 0x0001}, {0x07fa, 0x0800, 0x0003}, {0x0801, 0x082d, 0x0001},
	{0x0840, 0x085b, 0x0001}, {0x0860, 0x086a, 0x0001}, {0x0870, 0x0887, 0x0001}, {0x0889, 0x088f, 0x0001},
	{0x0897, 0x08e1, 0x0001}, {0x08e3, 0x0963, 0x0001}, {0x0966, 0x096f, 0x0001}, {0x0971, 0x0983, 0x0001},
	{0x0985, 0x098c, 0x0001}, {0x098f, 0x0990, 0x0001}, {0x0993, 0x09a8, 0x0001}, {0x09aa, 0x09b0, 0x0001},
	{0x09b2, 0x09b6, 0x0004}, {0x09b7, 0x09b9, 0x0001}, {0x09bc, 0x09c4, 0x0001}, {0x09c7, 0x09c8, 0x0001},
	{0x09cb, 0x09ce, 0x0001}, {0x09d7, 0x09dc, 0x0005}, {0x09dd, 0x09df, 0x0002}, {0x09e0, 0x09e3, 0x0001},
	{0x09e6, 0x09f1, 0x0001}, {0x09fc, 0x09fe, 0x0002}, {0x0a01, 0x0a03, 0x0001}, {0x0a05, 0x0a0a, 0x0001},
	{0x0a0f, 0x0a10, 0x0001}, {0x0a13, 0x0a28, 0x0001}, {0x0a2a, 0x0a30, 0x0001}, {0x0a32, 0x0a33, 0x0001},
	{0x0a35, 0x0a36, 0x0001}, {0x0a38, 0x0a39, 0x0001}, {0x0a3c, 0x0a3e, 0x0002}, {0x0a3f, 0x0a42, 0x0001},
	{0x0a47, 0x0a48, 0x0001}, {0x0a4b, 0x0a4d, 0x0001}, {0x0a51, 0x0a59, 0x0008}, {0x0a5a, 0x0a5c, 0x0001},
	{0x0a5e, 0x0a66, 0x0008}, {0x0a67, 0x0a75, 0x0001}, {0x0a81, 0x0a83, 0x0001}, {0x0a85, 0x0a8d, 0x0001},
	{0x0a8f, 0x0a91, 0x0001}, {0x0a93, 0x0aa8, 0x0001}, {0x0aaa, 0x0ab0, 0x0001}, {0x0ab2, 0x0ab3, 0x0001},
	{0x0ab5, 0x0ab9, 0x0001}, {0x0abc, 0x0ac5, 0x0001}, {0x0ac7, 0x0ac9, 0x0001}, {0x0acb, 0x0acd, 0x0001},
	{0x0ad0, 0x0ae0, 0x0010}, {0x0ae1, 0x0ae3, 0x0001}, {0x0ae6, 0x0aef, 0x0001}, {0x0af9, 0x0aff, 0x0001},
	{0x0b01, 0x0b03, 0x0001}, {0x0b05, 0x0b0c, 0x0001}, {0x0b0f, 0x0b10, 0x0001}, {0x0b13, 0x0b28, 0x0001},
	{0x0b2a, 0x0b30, 0x0001}, {0x0b32, 0x0b33, 0x0001}, {0x0b35, 0x0b39, 0x0001}, {0x0b3c, 0x0b44, 0x0001},
	{0x0b47, 0x0b48, 0x0001}, {0x0b4b, 0x0b4d, 0x0001}, {0x0b55, 0x0b57, 0x0001}, {0x0b5c, 0x0b5d, 0x0001},
	{0x0b5f, 0x0b63, 0x0001}, {0x0b66, 0x0b6f, 0x0001}, {0x0b71, 0x0b82, 0x0011}, {0x0b83, 0x0b85, 0x0002},
	{0x0b86, 0x0b8a, 0x0001}, {0x0b8e, 0x0b90, 0x0001}, {0x0b92, 0x0b95, 0x0001}, {0x0b99, 0x0b9a, 0x0001},
	{0x0b9c, 0x0b9e, 0x0002}, {0x0b9f, 0x0ba3, 0x0004}, {0x0ba4, 0x0ba8, 0x0004}, {0x0ba9, 0x0baa, 0x0001},
	{0x0bae, 0x0bb9, 0x0001}, {0x0bbe, 0x0bc2, 0x0001}, {0x0bc6, 0x0bc8, 0x0001}, {0x0bca, 0x0bcd, 0x0001},
	{0x0bd0, 0x0bd7, 0x0007}, {0x0be6, 0x0bef, 0x0001}, {0x0c00, 0x0c0c, 0x0001}, {0x0c0e, 0x0c10, 0x0001},
	{0x0c12, 0x0c28, 0x0001}, {0x0c2a, 0x0c39, 0x0001}, {0x0c3c, 0x0c44, 0x0001}, {0x0c46, 0x0c48, 0x0001},
	{0x0c4a, 0x0c4d, 0x0001}, {0x0c55, 0x0c56, 0x0001}, {0x0c58, 0x0c5a, 0x0001}, {0x0c5c, 0x0c5d, 0x0001},
	{0x0c60, 0x0c63, 0x0001}, {0x0c66, 0x0c6f, 0x0001}, {0x0c80, 0x0c83, 0x0001}, {0x0c85, 0x0c8c, 0x0001},
	{0x0c8e, 0x0c90, 0x0001}, {0x0c92, 0x0ca8, 0x0001}, {0x0caa, 0x0cb3, 0x0001}, {0x0cb5, 0x0cb9, 0x0001},
	{0x0cbc, 0x0cc4, 0x0001}, {0x0cc6, 0x0cc8, 0x0001}, {0x0cca, 0x0ccd, 0x0001}, {0x0cd5, 0x0cd6, 0x0001},
	{0x0cdc, 0x0cde, 0x0001}, {0x0ce0, 0x0ce3, 0x0001}, {0x0ce6, 0x0cef, 0x0001}, {0x0cf1, 0x0cf3, 0x0001},
	{0x0d00, 0x0d0c, 0x0001}, {0x0d0e, 0x0d10, 0x0001}, {0x0d12, 0x0d44, 0x0001}, {0x0d46, 0x0d48, 0x0001},
	{0x0d4a, 0x0d4e, 0x0001}, {0x0d54, 0x0d57, 0x0001}, {0x0d5f, 0x0d63, 0x0001}, {0x0d66, 0x0d6f, 0x0001},
	{0x0d7a, 0x0d7f, 0x0001}, {0x0d81, 0x0d83, 0x0001}, {0x0d85, 0x0d96, 0x0001}, {0x0d9a, 0x0db1, 0x0001},
	{0x0db3, 0x0dbb, 0x0001}, {0x0dbd, 0x0dc0, 0x0003}, {0x0dc1, 0x0dc6, 0x0001}, {0x0dca, 0x0dcf, 0x0005},
	{0x0dd0, 0x0dd4, 0x0001}, {0x0dd6, 0x0dd8, 0x0002}, {0x0dd9, 0x0ddf, 0x0001}, {0x0de6, 0x0def, 0x0001},
	{0x0df2, 0x0df3, 0x0001}, {0x0e01, 0x0e3a, 0x0001}, {0x0e40, 0x0e4e, 0x0001}, {0x0e50, 0x0e59, 0x0001},
	{0x0e81, 0x0e82, 0x0001}, {0x0e84, 0x0e86, 0x0002}, {0x0e87, 0x0e8a, 0x0001}, {0x0e8c, 0x0ea3, 0x0001},
	{0x0ea5, 0x0ea7, 0x0002}, {0x0ea8, 0x0ebd, 0x0001}, {0x0ec0, 0x0ec4, 0x0001}, {0x0ec6, 0x0ec8, 0x0002},
	{0x0ec9, 0x0ece, 0x0001}, {0x0ed0, 0x0ed9, 0x0001}, {0x0edc, 0x0edf, 0x0001}, {0x0f00, 0x0f18, 0x0018},
	{0x0f19, 0x0f20, 0x0007}, {0x0f21, 0x0f29, 0x0001}, {0x0f35, 0x0f39, 0x0002}, {0x0f3e, 0x0f47, 0x0001},
	{0x0f49, 0x0f6c, 0x0001}, {0x0f71, 0x0f84, 0x0001}, {0x0f86, 0x0f97, 0x0001}, {0x0f99, 0x0fbc, 0x0001},
	{0x0fc6, 0x1000, 0x003a}, {0x1001, 0x1049, 0x0001}, {0x1050, 0x109d, 0x0001}, {0x10a0, 0x10c5, 0x0001},
	{0x10c7, 0x10cd, 0x0006}, {0x10d0, 0x10fa, 0x0001}, {0x10fc, 0x1248, 0x0001}, {0x124a, 0x124d, 0x0001},
	{0x1250, 0x1256, 0x0001}, {0x1258, 0x125a, 0x0002}, {0x125b, 0x125d, 0x0001}, {0x1260, 0x1288, 0x0001},
	{0x128a, 0x128d, 0x0001}, {0x1290, 0x12b0, 0x0001}, {0x12b2, 0x12b5, 0x0001}, {0x12b8, 0x12be, 0x0001},
	{0x12c0, 0x12c2, 0x0002}, {0x12c3, 0x12c5, 0x0001}, {0x12c8, 0x12d6, 0x0001}, {0x12d8, 0x1310, 0x0001},
	{0x1312, 0x1315, 0x0001}, {0x1318, 0x135a, 0x0001}, {0x135d, 0x135f, 0x0001}, {0x1369, 0x1371, 0x0001},
	{0x1380, 0x138f, 0x0001}, {0x13a0, 0x13f5, 0x0001}, {0x13f8, 0x13fd, 0x0001}, {0x1401, 0x166c, 0x0001},
	{0x166f, 0x167f, 0x0001}, {0x1681, 0x169a, 0x0001}, {0x16a0, 0x16ea, 0x0001}, {0x16ee, 0x16f8, 0x0001},
	{0x1700, 0x1715, 0x0001}, {0x171f, 0x1734, 0x0001}, {0x1740, 0x1753, 0x0001}, {0x1760, 0x176c, 0x0001},
	{0x176e, 0x1770, 0x0001}, {0x1772, 0x1773, 0x0001}, {0x1780, 0x17d3, 0x0001}, {0x17d7, 0x17dc, 0x0005},
	{0x17dd, 0x17e0, 0x0003}, {0x17e1, 0x17e9, 0x0001}, {0x180b, 0x180d, 0x0001}, {0x180f, 0x1819, 0x0001},
	{0x1820, 0x1878, 0x0001}, {0x1880, 0x18aa, 0x0001}, {0x18b0, 0x18f5, 0x0001}, {0x1900, 0x191e, 0x0001},
	{0x1920, 0x192b, 0x0001}, {0x1930, 0x193b, 0x0001}, {0x1946, 0x196d, 0x0001}, {0x1970, 0x1974, 0x0001},
	{0x1980, 0x19ab, 0x0001}, {0x19b0, 0x19c9, 0x0001}, {0x19d0, 0x19da, 0x0001}, {0x1a00, 0x1a1b, 0x0001},
	{0x1a20, 0x1a5e, 0x0001}, {0x1a60, 0x1a7c, 0x0001}, {0x1a7f, 0x1a89, 0x0001}, {0x1a90, 0x1a99, 0x0001},
	{0x1aa7, 0x1ab0, 0x0009}, {0x1ab1, 0x1abd, 0x0001}, {0x1abf, 0x1add, 0x0001}, {0x1ae0, 0x1aeb, 0x0001},
	{0x1b00, 0x1b4c, 0x0001}, {0x1b50, 0x1b59, 0x0001}, {0x1b6b, 0x1b73, 0x0001}, {0x1b80, 0x1bf3, 0x0001},
	{0x1c00, 0x1c37, 0x0001}, {0x1c40, 0x1c49, 0x0001}, {0x1c4d, 0x1c7d, 0x0001}, {0x1c80, 0x1c8a, 0x0001},
	{0x1c90, 0x1cba, 0x0001}, {0x1cbd, 0x1cbf, 0x0001}, {0x1cd0, 0x1cd2, 0x0001}, {0x1cd4, 0x1cfa, 0x0001},
	{0x1d00, 0x1f15, 0x0001}, {0x1f18, 0x1f1d, 0x0001}, {0x1f20, 0x1f45, 0x0001}, {0x1f48, 0x1f4d, 0x0001},
	{0x1f50, 0x1f57, 0x0001}, {0x1f59, 0x1f5f, 0x0002}, {0x1f60, 0x1f7d, 0x0001}, {0x1f80, 0x1fb4, 0x0001},
	{0x1fb6, 0x1fbc, 0x0001}, {0x1fbe, 0x1fc2, 0x0004}, {0x1fc3, 0x1fc4, 0x0001}, {0x1fc6, 0x1fcc, 0x0001},
	{0x1fd0, 0x1fd3, 0x0001}, {0x1fd6, 0x1fdb, 0x0001}, {0x1fe0, 0x1fec, 0x0001}, {0x1ff2, 0x1ff4, 0x0001},
	{0x1ff6, 0x1ffc, 0x0001}, {0x200c, 0x200d, 0x0001}, {0x203f, 0x2040, 0x0001}, {0x2054, 0x2071, 0x001d},
	{0x207f, 0x2090, 0x0011}, {0x2091, 0x209c, 0x0001}, {0x20d0, 0x20dc, 0x0001}, {0x20e1, 0x20e5, 0x0004},
	{0x20e6, 0x20f0, 0x0001}, {0x2102, 0x2107, 0x0005}, {0x210a, 0x2113, 0x0001}, {0x2115, 0x2118, 0x0003},
	{0x2119, 0x211d, 0x0001}, {0x2124, 0x212a, 0x0002}, {0x212b, 0x2139, 0x0001}, {0x213c, 0x213f, 0x0001},
	{0x2145, 0x2149, 0x0001}, {0x214e, 0x2160, 0x0012}, {0x2161, 0x2188, 0x0001}, {0x2c00, 0x2ce4, 0x0001},
	{0x2ceb, 0x2cf3, 0x0001}, {0x2d00, 0x2d25, 0x0001}, {0x2d27, 0x2d2d, 0x0006}, {0x2d30, 0x2d67, 0x0001},
	{0x2d6f, 0x2d7f, 0x0010}, {0x2d80, 0x2d96, 0x0001}, {0x2da0, 0x2da6, 0x0001}, {0x2da8, 0x2dae, 0x0001},
	{0x2db0, 0x2db6, 0x0001}, {0x2db8, 0x2dbe, 0x0001}, {0x2dc0, 0x2dc6, 0x0001}, {0x2dc8, 0x2dce, 0x0001},
	{0x2dd0, 0x2dd6, 0x0001}, {0x2dd8, 0x2dde, 0x0001}, {0x2de0, 0x2dff, 0x0001}, {0x3005, 0x3007, 0x0001},
	{0x3021, 0x302f, 0x0001}, {0x3031, 0x3035, 0x0001}, {0x3038, 0x303c, 0x0001}, {0x3041, 0x3096, 0x0001},
	{0x3099, 0x309a, 0x0001}, {0x309d, 0x309f, 0x0001}, {0x30a1, 0x30ff, 0x0001}, {0x3105, 0x312f, 0x0001},
	{0x3131, 0x318e, 0x0001}, {0x31a0, 0x31bf, 0x0001}, {0x31f0, 0x31ff, 0x0001}, {0x3400, 0x4dbf, 0x0001},
	{0x4e00, 0xa48c, 0x0001}, {0xa4d0, 0xa4fd, 0x0001}, {0xa500, 0xa60c, 0x0001}, {0xa610, 0xa62b, 0x0001},
	{0xa640, 0xa66f, 0x0001}, {0xa674, 0xa67d, 0x0001}, {0xa67f, 0xa6f1, 0x0001}, {0xa717, 0xa71f, 0x0001},
	{0xa722, 0xa788, 0x0001}, {0xa78b, 0xa7dc, 0x0001}, {0xa7f1, 0xa827, 0x0001}, {0xa82c, 0xa840, 0x0014},
	{0xa841, 0xa873, 0x0001}, {0xa880, 0xa8c5, 0x0001}, {0xa8d0, 0xa8d9, 0x0001}, {0xa8e0, 0xa8f7, 0x0001},
	{0xa8fb, 0xa8fd, 0x0002}, {0xa8fe, 0xa92d, 0x0001}, {0xa930, 0xa953, 0x0001}, {0xa960, 0xa97c, 0x0001},
	{0xa980, 0xa9c0, 0x0001}, {0xa9cf, 0xa9d9, 0x0001}, {0xa9e0, 0xa9fe, 0x0001}, {0xaa00, 0xaa36, 0x0001},
	{0xaa40, 0xaa4d, 0x0001}, {0xaa50, 0xaa59, 0x0001}, {0xaa60, 0xaa76, 0x0001}, {0xaa7a, 0xaac2, 0x0001},
	{0xaadb, 0xaadd, 0x0001}, {0xaae0, 0xaaef, 0x0001}, {0xaaf2, 0xaaf6, 0x0001}, {0xab01, 0xab06, 0x0001},
	{0xab09, 0xab0e, 0x0001}, {0xab11, 0xab16, 0x0001}, {0xab20, 0xab26, 0x0001}, {0xab28, 0xab2e, 0x0001},
	{0xab30, 0xab5a, 0x0001}, {0xab5c, 0xab69, 0x0001}, {0xab70, 0xabea, 0x0001}, {0xabec, 0xabed, 0x0001},
	{0xabf0, 0xabf9, 0x0001}, {0xac00, 0xd7a3, 0x0001}, {0xd7b0, 0xd7c6, 0x0001}, {0xd7cb, 0xd7fb, 0x0001},
	{0xf900, 0xfa6d, 0x0001}, {0xfa70, 0xfad9, 0x0001}, {0xfb00, 0xfb06, 0x0001}, {0xfb13, 0xfb17, 0x0001},
	{0xfb1d, 0xfb28, 0x0001}, {0xfb2a, 0xfb36, 0x0001}, {0xfb38, 0xfb3c, 0x0001}, {0xfb3e, 0xfb40, 0x0002},
	{0xfb41, 0xfb43, 0x0002}, {0xfb44, 0xfb46, 0x0002}, {0xfb47, 0xfbb1, 0x0001}, {0xfbd3, 0xfc5d, 0x0001},
	{0xfc64, 0xfd3d, 0x0001}, {0xfd50, 0xfd8f, 0x0001}, {0xfd92, 0xfdc7, 0x0001}, {0xfdf0, 0xfdf9, 0x0001},
	{0xfe00, 0xfe0f, 0x0001}, {0xfe20, 0xfe2f, 0x0001}, {0xfe33, 0xfe34, 0x0001}, {0xfe4d, 0xfe4f, 0x0001},
	{0xfe71, 0xfe73, 0x0002}, {0xfe77, 0xfe7f, 0x0002}, {0xfe80, 0xfefc, 0x0001}, {0xff10, 0xff19, 0x0001},
	{0xff21, 0xff3a, 0x0001}, {0xff3f, 0xff41, 0x0002}, {0xff42, 0xff5a, 0x0001}, {0xff65, 0xffbe, 0x0001},
	{0xffc2, 0xffc7, 0x0001}, {0xffca, 0xffcf, 0x0001}, {0xffd2, 0xffd7, 0x0001}, {0xffda, 0xffdc, 0x0001}
};


//
//	xidContinue32
//

#if defined(IMGUI_USE_WCHAR32)

static Range32 xidContinue32[] = {
	{0x10000, 0x1000b, 0x0001}, {0x1000d, 0x10026, 0x0001}, {0x10028, 0x1003a, 0x0001}, {0x1003c, 0x1003d, 0x0001},
	{0x1003f, 0x1004d, 0x0001}, {0x10050, 0x1005d, 0x0001}, {0x10080, 0x100fa, 0x0001}, {0x10140, 0x10174, 0x0001},
	{0x101fd, 0x10280, 0x0083}, {0x10281, 0x1029c, 0x0001}, {0x102a0, 0x102d0, 0x0001}, {0x102e0, 0x10300, 0x0020},
	{0x10301, 0x1031f, 0x0001}, {0x1032d, 0x1034a, 0x0001}, {0x10350, 0x1037a, 0x0001}, {0x10380, 0x1039d, 0x0001},
	{0x103a0, 0x103c3, 0x0001}, {0x103c8, 0x103cf, 0x0001}, {0x103d1, 0x103d5, 0x0001}, {0x10400, 0x1049d, 0x0001},
	{0x104a0, 0x104a9, 0x0001}, {0x104b0, 0x104d3, 0x0001}, {0x104d8, 0x104fb, 0x0001}, {0x10500, 0x10527, 0x0001},
	{0x10530, 0x10563, 0x0001}, {0x10570, 0x1057a, 0x0001}, {0x1057c, 0x1058a, 0x0001}, {0x1058c, 0x10592, 0x0001},
	{0x10594, 0x10595, 0x0001}, {0x10597, 0x105a1, 0x0001}, {0x105a3, 0x105b1, 0x0001}, {0x105b3, 0x105b9, 0x0001},
	{0x105bb, 0x105bc, 0x0001}, {0x105c0, 0x105f3, 0x0001}, {0x10600, 0x10736, 0x0001}, {0x10740, 0x10755, 0x0001},
	{0x10760, 0x10767, 0x0001}, {0x10780, 0x10785, 0x0001}, {0x10787, 0x107b0, 0x0001}, {0x107b2, 0x107ba, 0x0001},
	{0x10800, 0x10805, 0x0001}, {0x10808, 0x1080a, 0x0002}, {0x1080b, 0x10835, 0x0001}, {0x10837, 0x10838, 0x0001},
	{0x1083c, 0x1083f, 0x0003}, {0x10840, 0x10855, 0x0001}, {0x10860, 0x10876, 0x0001}, {0x10880, 0x1089e, 0x0001},
	{0x108e0, 0x108f2, 0x0001}, {0x108f4, 0x108f5, 0x0001}, {0x10900, 0x10915, 0x0001}, {0x10920, 0x10939, 0x0001},
	{0x10940, 0x10959, 0x0001}, {0x10980, 0x109b7, 0x0001}, {0x109be, 0x109bf, 0x0001}, {0x10a00, 0x10a03, 0x0001},
	{0x10a05, 0x10a06, 0x0001}, {0x10a0c, 0x10a13, 0x0001}, {0x10a15, 0x10a17, 0x0001}, {0x10a19, 0x10a35, 0x0001},
	{0x10a38, 0x10a3a, 0x0001}, {0x10a3f, 0x10a60, 0x0021}, {0x10a61, 0x10a7c, 0x0001}, {0x10a80, 0x10a9c, 0x0001},
	{0x10ac0, 0x10ac7, 0x0001}, {0x10ac9, 0x10ae6, 0x0001}, {0x10b00, 0x10b35, 0x0001}, {0x10b40, 0x10b55, 0x0001},
	{0x10b60, 0x10b72, 0x0001}, {0x10b80, 0x10b91, 0x0001}, {0x10c00, 0x10c48, 0x0001}, {0x10c80, 0x10cb2, 0x0001},
	{0x10cc0, 0x10cf2, 0x0001}, {0x10d00, 0x10d27, 0x0001}, {0x10d30, 0x10d39, 0x0001}, {0x10d40, 0x10d65, 0x0001},
	{0x10d69, 0x10d6d, 0x0001}, {0x10d6f, 0x10d85, 0x0001}, {0x10e80, 0x10ea9, 0x0001}, {0x10eab, 0x10eac, 0x0001},
	{0x10eb0, 0x10eb1, 0x0001}, {0x10ec2, 0x10ec7, 0x0001}, {0x10efa, 0x10f1c, 0x0001}, {0x10f27, 0x10f30, 0x0009},
	{0x10f31, 0x10f50, 0x0001}, {0x10f70, 0x10f85, 0x0001}, {0x10fb0, 0x10fc4, 0x0001}, {0x10fe0, 0x10ff6, 0x0001},
	{0x11000, 0x11046, 0x0001}, {0x11066, 0x11075, 0x0001}, {0x1107f, 0x110ba, 0x0001}, {0x110c2, 0x110d0, 0x000e},
	{0x110d1, 0x110e8, 0x0001}, {0x110f0, 0x110f9, 0x0001}, {0x11100, 0x11134, 0x0001}, {0x11136, 0x1113f, 0x0001},
	{0x11144, 0x11147, 0x0001}, {0x11150, 0x11173, 0x0001}, {0x11176, 0x11180, 0x000a}, {0x11181, 0x111c4, 0x0001},
	{0x111c9, 0x111cc, 0x0001}, {0x111ce, 0x111da, 0x0001}, {0x111dc, 0x11200, 0x0024}, {0x11201, 0x11211, 0x0001},
	{0x11213, 0x11237, 0x0001}, {0x1123e, 0x11241, 0x0001}, {0x11280, 0x11286, 0x0001}, {0x11288, 0x1128a, 0x0002},
	{0x1128b, 0x1128d, 0x0001}, {0x1128f, 0x1129d, 0x0001}, {0x1129f, 0x112a8, 0x0001}, {0x112b0, 0x112ea, 0x0001},
	{0x112f0, 0x112f9, 0x0001}, {0x11300, 0x11303, 0x0001}, {0x11305, 0x1130c, 0x0001}, {0x1130f, 0x11310, 0x0001},
	{0x11313, 0x11328, 0x0001}, {0x1132a, 0x11330, 0x0001}, {0x11332, 0x11333, 0x0001}, {0x11335, 0x11339, 0x0001},
	{0x1133b, 0x11344, 0x0001}, {0x11347, 0x11348, 0x0001}, {0x1134b, 0x1134d, 0x0001}, {0x11350, 0x11357, 0x0007},
	{0x1135d, 0x11363, 0x0001}, {0x11366, 0x1136c, 0x0001}, {0x11370, 0x11374, 0x0001}, {0x11380, 0x11389, 0x0001},
	{0x1138b, 0x1138e, 0x0003}, {0x11390, 0x113b5, 0x0001}, {0x113b7, 0x113c0, 0x0001}, {0x113c2, 0x113c5, 0x0003},
	{0x113c7, 0x113ca, 0x0001}, {0x113cc, 0x113d3, 0x0001}, {0x113e1, 0x113e2, 0x0001}, {0x11400, 0x1144a, 0x0001},
	{0x11450, 0x11459, 0x0001}, {0x1145e, 0x11461, 0x0001}, {0x11480, 0x114c5, 0x0001}, {0x114c7, 0x114d0, 0x0009},
	{0x114d1, 0x114d9, 0x0001}, {0x11580, 0x115b5, 0x0001}, {0x115b8, 0x115c0, 0x0001}, {0x115d8, 0x115dd, 0x0001},
	{0x11600, 0x11640, 0x0001}, {0x11644, 0x11650, 0x000c}, {0x11651, 0x11659, 0x0001}, {0x11680, 0x116b8, 0x0001},
	{0x116c0, 0x116c9, 0x0001}, {0x116d0, 0x116e3, 0x0001}, {0x11700, 0x1171a, 0x0001}, {0x1171d, 0x1172b, 0x0001},
	{0x11730, 0x11739, 0x0001}, {0x11740, 0x11746, 0x0001}, {0x11800, 0x1183a, 0x0001}, {0x118a0, 0x118e9, 0x0001},
	{0x118ff, 0x11906, 0x0001}, {0x11909, 0x1190c, 0x0003}, {0x1190d, 0x11913, 0x0001}, {0x11915, 0x11916, 0x0001},
	{0x11918, 0x11935, 0x0001}, {0x11937, 0x11938, 0x0001}, {0x1193b, 0x11943, 0x0001}, {0x11950, 0x11959, 0x0001},
	{0x119a0, 0x119a7, 0x0001}, {0x119aa, 0x119d7, 0x0001}, {0x119da, 0x119e1, 0x0001}, {0x119e3, 0x119e4, 0x0001},
	{0x11a00, 0x11a3e, 0x0001}, {0x11a47, 0x11a50, 0x0009}, {0x11a51, 0x11a99, 0x0001}, {0x11a9d, 0x11ab0, 0x0013},
	{0x11ab1, 0x11af8, 0x0001}, {0x11b60, 0x11b67, 0x0001}, {0x11bc0, 0x11be0, 0x0001}, {0x11bf0, 0x11bf9, 0x0001},
	{0x11c00, 0x11c08, 0x0001}, {0x11c0a, 0x11c36, 0x0001}, {0x11c38, 0x11c40, 0x0001}, {0x11c50, 0x11c59, 0x0001},
	{0x11c72, 0x11c8f, 0x0001}, {0x11c92, 0x11ca7, 0x0001}, {0x11ca9, 0x11cb6, 0x0001}, {0x11d00, 0x11d06, 0x0001},
	{0x11d08, 0x11d09, 0x0001}, {0x11d0b, 0x11d36, 0x0001}, {0x11d3a, 0x11d3c, 0x0002}, {0x11d3d, 0x11d3f, 0x0002},
	{0x11d40, 0x11d47, 0x0001}, {0x11d50, 0x11d59, 0x0001}, {0x11d60, 0x11d65, 0x0001}, {0x11d67, 0x11d68, 0x0001},
	{0x11d6a, 0x11d8e, 0x0001}, {0x11d90, 0x11d91, 0x0001}, {0x11d93, 0x11d98, 0x0001}, {0x11da0, 0x11da9, 0x0001},
	{0x11db0, 0x11ddb, 0x0001}, {0x11de0, 0x11de9, 0x0001}, {0x11ee0, 0x11ef6, 0x0001}, {0x11f00, 0x11f10, 0x0001},
	{0x11f12, 0x11f3a, 0x0001}, {0x11f3e, 0x11f42, 0x0001}, {0x11f50, 0x11f5a, 0x0001}, {0x11fb0, 0x12000, 0x0050},
	{0x12001, 0x12399, 0x0001}, {0x12400, 0x1246e, 0x0001}, {0x12480, 0x12543, 0x0001}, {0x12f90, 0x12ff0, 0x0001},
	{0x13000, 0x1342f, 0x0001}, {0x13440, 0x13455, 0x0001}, {0x13460, 0x143fa, 0x0001}, {0x14400, 0x14646, 0x0001},
	{0x16100, 0x16139, 0x0001}, {0x16800, 0x16a38, 0x0001}, {0x16a40, 0x16a5e, 0x0001}, {0x16a60, 0x16a69, 0x0001},
	{0x16a70, 0x16abe, 0x0001}, {0x16ac0, 0x16ac9, 0x0001}, {0x16ad0, 0x16aed, 0x0001}, {0x16af0, 0x16af4, 0x0001},
	{0x16b00, 0x16b36, 0x0001}, {0x16b40, 0x16b43, 0x0001}, {0x16b50, 0x16b59, 0x0001}, {0x16b63, 0x16b77, 0x0001},
	{0x16b7d, 0x16b8f, 0x0001}, {0x16d40, 0x16d6c, 0x0001}, {0x16d70, 0x16d79, 0x0001}, {0x16e40, 0x16e7f, 0x0001},
	{0x16ea0, 0x16eb8, 0x0001}, {0x16ebb, 0x16ed3, 0x0001}, {0x16f00, 0x16f4a, 0x0001}, {0x16f4f, 0x16f87, 0x0001},
	{0x16f8f, 0x16f9f, 0x0001}, {0x16fe0, 0x16fe1, 0x0001}, {0x16fe3, 0x16fe4, 0x0001}, {0x16ff0, 0x16ff6, 0x0001},
	{0x17000, 0x18cd5, 0x0001}, {0x18cff, 0x18d1e, 0x0001}, {0x18d80, 0x18df2, 0x0001}, {0x1aff0, 0x1aff3, 0x0001},
	{0x1aff5, 0x1affb, 0x0001}, {0x1affd, 0x1affe, 0x0001}, {0x1b000, 0x1b122, 0x0001}, {0x1b132, 0x1b150, 0x001e},
	{0x1b151, 0x1b152, 0x0001}, {0x1b155, 0x1b164, 0x000f}, {0x1b165, 0x1b167, 0x0001}, {0x1b170, 0x1b2fb, 0x0001},
	{0x1bc00, 0x1bc6a, 0x0001}, {0x1bc70, 0x1bc7c, 0x0001}, {0x1bc80, 0x1bc88, 0x0001}, {0x1bc90, 0x1bc99, 0x0001},
	{0x1bc9d, 0x1bc9e, 0x0001}, {0x1ccf0, 0x1ccf9, 0x0001}, {0x1cf00, 0x1cf2d, 0x0001}, {0x1cf30, 0x1cf46, 0x0001},
	{0x1d165, 0x1d169, 0x0001}, {0x1d16d, 0x1d172, 0x0001}, {0x1d17b, 0x1d182, 0x0001}, {0x1d185, 0x1d18b, 0x0001},
	{0x1d1aa, 0x1d1ad, 0x0001}, {0x1d242, 0x1d244, 0x0001}, {0x1d400, 0x1d454, 0x0001}, {0x1d456, 0x1d49c, 0x0001},
	{0x1d49e, 0x1d49f, 0x0001}, {0x1d4a2, 0x1d4a5, 0x0003}, {0x1d4a6, 0x1d4a9, 0x0003}, {0x1d4aa, 0x1d4ac, 0x0001},
	{0x1d4ae, 0x1d4b9, 0x0001}, {0x1d4bb, 0x1d4bd, 0x0002}, {0x1d4be, 0x1d4c3, 0x0001}, {0x1d4c5, 0x1d505, 0x0001},
	{0x1d507, 0x1d50a, 0x0001}, {0x1d50d, 0x1d514, 0x0001}, {0x1d516, 0x1d51c, 0x0001}, {0x1d51e, 0x1d539, 0x0001},
	{0x1d53b, 0x1d53e, 0x0001}, {0x1d540, 0x1d544, 0x0001}, {0x1d546, 0x1d54a, 0x0004}, {0x1d54b, 0x1d550, 0x0001},
	{0x1d552, 0x1d6a5, 0x0001}, {0x1d6a8, 0x1d6c0, 0x0001}, {0x1d6c2, 0x1d6da, 0x0001}, {0x1d6dc, 0x1d6fa, 0x0001},
	{0x1d6fc, 0x1d714, 0x0001}, {0x1d716, 0x1d734, 0x0001}, {0x1d736, 0x1d74e, 0x0001}, {0x1d750, 0x1d76e, 0x0001},
	{0x1d770, 0x1d788, 0x0001}, {0x1d78a, 0x1d7a8, 0x0001}, {0x1d7aa, 0x1d7c2, 0x0001}, {0x1d7c4, 0x1d7cb, 0x0001},
	{0x1d7ce, 0x1d7ff, 0x0001}, {0x1da00, 0x1da36, 0x0001}, {0x1da3b, 0x1da6c, 0x0001}, {0x1da75, 0x1da84, 0x000f},
	{0x1da9b, 0x1da9f, 0x0001}, {0x1daa1, 0x1daaf, 0x0001}, {0x1df00, 0x1df1e, 0x0001}, {0x1df25, 0x1df2a, 0x0001},
	{0x1e000, 0x1e006, 0x0001}, {0x1e008, 0x1e018, 0x0001}, {0x1e01b, 0x1e021, 0x0001}, {0x1e023, 0x1e024, 0x0001},
	{0x1e026, 0x1e02a, 0x0001}, {0x1e030, 0x1e06d, 0x0001}, {0x1e08f, 0x1e100, 0x0071}, {0x1e101, 0x1e12c, 0x0001},
	{0x1e130, 0x1e13d, 0x0001}, {0x1e140, 0x1e149, 0x0001}, {0x1e14e, 0x1e290, 0x0142}, {0x1e291, 0x1e2ae, 0x0001},
	{0x1e2c0, 0x1e2f9, 0x0001}, {0x1e4d0, 0x1e4f9, 0x0001}, {0x1e5d0, 0x1e5fa, 0x0001}, {0x1e6c0, 0x1e6de, 0x0001},
	{0x1e6e0, 0x1e6f5, 0x0001}, {0x1e6fe, 0x1e6ff, 0x0001}, {0x1e7e0, 0x1e7e6, 0x0001}, {0x1e7e8, 0x1e7eb, 0x0001},
	{0x1e7ed, 0x1e7ee, 0x0001}, {0x1e7f0, 0x1e7fe, 0x0001}, {0x1e800, 0x1e8c4, 0x0001}, {0x1e8d0, 0x1e8d6, 0x0001},
	{0x1e900, 0x1e94b, 0x0001}, {0x1e950, 0x1e959, 0x0001}, {0x1ee00, 0x1ee03, 0x0001}, {0x1ee05, 0x1ee1f, 0x0001},
	{0x1ee21, 0x1ee22, 0x0001}, {0x1ee24, 0x1ee27, 0x0003}, {0x1ee29, 0x1ee32, 0x0001}, {0x1ee34, 0x1ee37, 0x0001},
	{0x1ee39, 0x1ee3b, 0x0002}, {0x1ee42, 0x1ee47, 0x0005}, {0x1ee49, 0x1ee4d, 0x0002}, {0x1ee4e, 0x1ee4f, 0x0001},
	{0x1ee51, 0x1ee52, 0x0001}, {0x1ee54, 0x1ee57, 0x0003}, {0x1ee59, 0x1ee61, 0x0002}, {0x1ee62, 0x1ee64, 0x0002},
	{0x1ee67, 0x1ee6a, 0x0001}, {0x1ee6c, 0x1ee72, 0x0001}, {0x1ee74, 0x1ee77, 0x0001}, {0x1ee79, 0x1ee7c, 0x0001},
	{0x1ee7e, 0x1ee80, 0x0002}, {0x1ee81, 0x1ee89, 0x0001}, {0x1ee8b, 0x1ee9b, 0x0001}, {0x1eea1, 0x1eea3, 0x0001},
	{0x1eea5, 0x1eea9, 0x0001}, {0x1eeab, 0x1eebb, 0x0001}, {0x1fbf0, 0x1fbf9, 0x0001}, {0x20000, 0x2a6df, 0x0001},
	{0x2a700, 0x2b81d, 0x0001}, {0x2b820, 0x2cead, 0x0001}, {0x2ceb0, 0x2ebe0, 0x0001}, {0x2ebf0, 0x2ee5d, 0x0001},
	{0x2f800, 0x2fa1d, 0x0001}, {0x30000, 0x3134a, 0x0001}, {0x31350, 0x33479, 0x0001}, {0xe0100, 0xe01ef, 0x0001}
};

#endif


//
//	case16
//

static CaseRange16 case16[] = {
	{0x0041, 0x005a,      0,     32}, {0x0061, 0x007a,    -32,      0}, {0x00b5, 0x00b5,    743,      0},
	{0x00c0, 0x00d6,      0,     32}, {0x00d8, 0x00de,      0,     32}, {0x00e0, 0x00f6,    -32,      0},
	{0x00f8, 0x00fe,    -32,      0}, {0x00ff, 0x00ff,    121,      0}, {0x0100, 0x012f,  65535,  65535},
	{0x0130, 0x0130,      0,   -199}, {0x0131, 0x0131,   -232,      0}, {0x0132, 0x0177,  65535,  65535},
	{0x0178, 0x0178,      0,   -121}, {0x0179, 0x017e,  65535,  65535}, {0x017f, 0x017f,   -300,      0},
	{0x0180, 0x0180,    195,      0}, {0x0181, 0x0181,      0,    210}, {0x0182, 0x0185,  65535,  65535},
	{0x0186, 0x0186,      0,    206}, {0x0187, 0x0188,  65535,  65535}, {0x0189, 0x018a,      0,    205},
	{0x018b, 0x018c,  65535,  65535}, {0x018e, 0x018e,      0,     79}, {0x018f, 0x018f,      0,    202},
	{0x0190, 0x0190,      0,    203}, {0x0191, 0x0192,  65535,  65535}, {0x0193, 0x0193,      0,    205},
	{0x0194, 0x0194,      0,    207}, {0x0195, 0x0195,     97,      0}, {0x0196, 0x0196,      0,    211},
	{0x0197, 0x0197,      0,    209}, {0x0198, 0x0199,  65535,  65535}, {0x019a, 0x019a,    163,      0},
	{0x019b, 0x019b,  42561,      0}, {0x019c, 0x019c,      0,    211}, {0x019d, 0x019d,      0,    213},
	{0x019e, 0x019e,    130,      0}, {0x019f, 0x019f,      0,    214}, {0x01a0, 0x01a5,  65535,  65535},
	{0x01a6, 0x01a6,      0,    218}, {0x01a7, 0x01a8,  65535,  65535}, {0x01a9, 0x01a9,      0,    218},
	{0x01ac, 0x01ad,  65535,  65535}, {0x01ae, 0x01ae,      0,    218}, {0x01af, 0x01b0,  65535,  65535},
	{0x01b1, 0x01b2,      0,    217}, {0x01b3, 0x01b6,  65535,  65535}, {0x01b7, 0x01b7,      0,    219},
	{0x01b8, 0x01bd,  65535,  65535}, {0x01bf, 0x01bf,     56,      0}, {0x01c4, 0x01c4,      0,      2},
	{0x01c6, 0x01c6,     -2,      0}, {0x01c7, 0x01c7,      0,      2}, {0x01c9, 0x01c9,     -2,      0},
	{0x01ca, 0x01ca,      0,      2}, {0x01cc, 0x01cc,     -2,      0}, {0x01cd, 0x01dc,  65535,  65535},
	{0x01dd, 0x01dd,    -79,      0}, {0x01de, 0x01ef,  65535,  65535}, {0x01f1, 0x01f1,      0,      2},
	{0x01f3, 0x01f3,     -2,      0}, {0x01f4, 0x01f5,  65535,  65535}, {0x01f6, 0x01f6,      0,    -97},
	{0x01f7, 0x01f7,      0,    -56}, {0x01f8, 0x021f,  65535,  65535}, {0x0220, 0x0220,      0,   -130},
	{0x0222, 0x0233,  65535,  65535}, {0x023a, 0x023a,      0,  10795}, {0x023b, 0x023c,  65535,  65535},
	{0x023d, 0x023d,      0,   -163}, {0x023e, 0x023e,      0,  10792}, {0x023f, 0x0240,  10815,      0},
	{0x0241, 0x0242,  65535,  65535}, {0x0243, 0x0243,      0,   -195}, {0x0244, 0x0244,      0,     69},
	{0x0245, 0x0245,      0,     71}, {0x0246, 0x024f,  65535,  65535}, {0x0250, 0x0250,  10783,      0},
	{0x0251, 0x0251,  10780,      0}, {0x0252, 0x0252,  10782,      0}, {0x0253, 0x0253,   -210,      0},
	{0x0254, 0x0254,   -206,      0}, {0x0256, 0x0257,   -205,      0}, {0x0259, 0x0259,   -202,      0},
	{0x025b, 0x025b,   -203,      0}, {0x025c, 0x025c,  42319,      0}, {0x0260, 0x0260,   -205,      0},
	{0x0261, 0x0261,  42315,      0}, {0x0263, 0x0263,   -207,      0}, {0x0264, 0x0264,  42343,      0},
	{0x0265, 0x0265,  42280,      0}, {0x0266, 0x0266,  42308,      0}, {0x0268, 0x0268,   -209,      0},
	{0x0269, 0x0269,   -211,      0}, {0x026a, 0x026a,  42308,      0}, {0x026b, 0x026b,  10743,      0},
	{0x026c, 0x026c,  42305,      0}, {0x026f, 0x026f,   -211,      0}, {0x0271, 0x0271,  10749,      0},
	{0x0272, 0x0272,   -213,      0}, {0x0275, 0x0275,   -214,      0}, {0x027d, 0x027d,  10727,      0},
	{0x0280, 0x0280,   -218,      0}, {0x0282, 0x0282,  42307,      0}, {0x0283, 0x0283,   -218,      0},
	{0x0287, 0x0287,  42282,      0}, {0x0288, 0x0288,   -218,      0}, {0x0289, 0x0289,    -69,      0},
	{0x028a, 0x028b,   -217,      0}, {0x028c, 0x028c,    -71,      0}, {0x0292, 0x0292,   -219,      0},
	{0x029d, 0x029d,  42261,      0}, {0x029e, 0x029e,  42258,      0}, {0x0370, 0x0377,  65535,  65535},
	{0x037b, 0x037d,    130,      0}, {0x037f, 0x037f,      0,    116}, {0x0386, 0x0386,      0,     38},
	{0x0388, 0x038a,      0,     37}, {0x038c, 0x038c,      0,     64}, {0x038e, 0x038f,      0,     63},
	{0x0391, 0x03a1,      0,     32}, {0x03a3, 0x03ab,      0,     32}, {0x03ac, 0x03ac,    -38,      0},
	{0x03ad, 0x03af,    -37,      0}, {0x03b1, 0x03c1,    -32,      0}, {0x03c2, 0x03c2,    -31,      0},
	{0x03c3, 0x03cb,    -32,      0}, {0x03cc, 0x03cc,    -64,      0}, {0x03cd, 0x03ce,    -63,      0},
	{0x03cf, 0x03cf,      0,      8}, {0x03d0, 0x03d0,    -62,      0}, {0x03d1, 0x03d1,    -57,      0},
	{0x03d5, 0x03d5,    -47,      0}, {0x03d6, 0x03d6,    -54,      0}, {0x03d7, 0x03d7,     -8,      0},
	{0x03d8, 0x03ef,  65535,  65535}, {0x03f0, 0x03f0,    -86,      0}, {0x03f1, 0x03f1,    -80,      0},
	{0x03f2, 0x03f2,      7,      0}, {0x03f3, 0x03f3,   -116,      0}, {0x03f4, 0x03f4,      0,    -60},
	{0x03f5, 0x03f5,    -96,      0}, {0x03f7, 0x03f8,  65535,  65535}, {0x03f9, 0x03f9,      0,     -7},
	{0x03fa, 0x03fb,  65535,  65535}, {0x03fd, 0x03ff,      0,   -130}, {0x0400, 0x040f,      0,     80},
	{0x0410, 0x042f,      0,     32}, {0x0430, 0x044f,    -32,      0}, {0x0450, 0x045f,    -80,      0},
	{0x0460, 0x04bf,  65535,  65535}, {0x04c0, 0x04c0,      0,     15}, {0x04c1, 0x04ce,  65535,  65535},
	{0x04cf, 0x04cf,    -15,      0}, {0x04d0, 0x052f,  65535,  65535}, {0x0531, 0x0556,      0,     48},
	{0x0561, 0x0586,    -48,      0}, {0x10a0, 0x10c5,      0,   7264}, {0x10c7, 0x10c7,      0,   7264},
	{0x10cd, 0x10cd,      0,   7264}, {0x10d0, 0x10fa,   3008,      0}, {0x10fd, 0x10ff,   3008,      0},
	{0x13a0, 0x13ef,      0,  38864}, {0x13f0, 0x13f5,      0,      8}, {0x13f8, 0x13fd,     -8,      0},
	{0x1c80, 0x1c80,  -6254,      0}, {0x1c81, 0x1c81,  -6253,      0}, {0x1c82, 0x1c82,  -6244,      0},
	{0x1c83, 0x1c84,  -6242,      0}, {0x1c85, 0x1c85,  -6243,      0}, {0x1c86, 0x1c86,  -6236,      0},
	{0x1c87, 0x1c87,  -6181,      0}, {0x1c88, 0x1c88,  35266,      0}, {0x1c89, 0x1c8a,  65535,  65535},
	{0x1c90, 0x1cba,      0,  -3008}, {0x1cbd, 0x1cbf,      0,  -3008}, {0x1d79, 0x1d79,  35332,      0},
	{0x1d7d, 0x1d7d,   3814,      0}, {0x1d8e, 0x1d8e,  35384,      0}, {0x1e00, 0x1e95,  65535,  65535},
	{0x1e9b, 0x1e9b,    -59,      0}, {0x1e9e, 0x1e9e,      0,  -7615}, {0x1ea0, 0x1eff,  65535,  65535},
	{0x1f00, 0x1f07,      8,      0}, {0x1f08, 0x1f0f,      0,     -8}, {0x1f10, 0x1f15,      8,      0},
	{0x1f18, 0x1f1d,      0,     -8}, {0x1f20, 0x1f27,      8,      0}, {0x1f28, 0x1f2f,      0,     -8},
	{0x1f30, 0x1f37,      8,      0}, {0x1f38, 0x1f3f,      0,     -8}, {0x1f40, 0x1f45,      8,      0},
	{0x1f48, 0x1f4d,      0,     -8}, {0x1f51, 0x1f51,      8,      0}, {0x1f53, 0x1f53,      8,      0},
	{0x1f55, 0x1f55,      8,      0}, {0x1f57, 0x1f57,      8,      0}, {0x1f59, 0x1f59,      0,     -8},
	{0x1f5b, 0x1f5b,      0,     -8}, {0x1f5d, 0x1f5d,      0,     -8}, {0x1f5f, 0x1f5f,      0,     -8},
	{0x1f60, 0x1f67,      8,      0}, {0x1f68, 0x1f6f,      0,     -8}, {0x1f70, 0x1f71,     74,      0},
	{0x1f72, 0x1f75,     86,      0}, {0x1f76, 0x1f77,    100,      0}, {0x1f78, 0x1f79,    128,      0},
	{0x1f7a, 0x1f7b,    112,      0}, {0x1f7c, 0x1f7d,    126,      0}, {0x1f80, 0x1f87,      8,      0},
	{0x1f90, 0x1f97,      8,      0}, {0x1fa0, 0x1fa7,      8,      0}, {0x1fb0, 0x1fb1,      8,      0},
	{0x1fb3, 0x1fb3,      9,      0}, {0x1fb8, 0x1fb9,      0,     -8}, {0x1fba, 0x1fbb,      0,    -74},
	{0x1fbe, 0x1fbe,  -7205,      0}, {0x1fc3, 0x1fc3,      9,      0}, {0x1fc8, 0x1fcb,      0,    -86},
	{0x1fd0, 0x1fd1,      8,      0}, {0x1fd8, 0x1fd9,      0,     -8}, {0x1fda, 0x1fdb,      0,   -100},
	{0x1fe0, 0x1fe1,      8,      0}, {0x1fe5, 0x1fe5,      7,      0}, {0x1fe8, 0x1fe9,      0,     -8},
	{0x1fea, 0x1feb,      0,   -112}, {0x1fec, 0x1fec,      0,     -7}, {0x1ff3, 0x1ff3,      9,      0},
	{0x1ff8, 0x1ff9,      0,   -128}, {0x1ffa, 0x1ffb,      0,   -126}, {0x2126, 0x2126,      0,  -7517},
	{0x212a, 0x212a,      0,  -8383}, {0x212b, 0x212b,      0,  -8262}, {0x2132, 0x2132,      0,     28},
	{0x214e, 0x214e,    -28,      0}, {0x2183, 0x2184,  65535,  65535}, {0x2c00, 0x2c2f,      0,     48},
	{0x2c30, 0x2c5f,    -48,      0}, {0x2c60, 0x2c61,  65535,  65535}, {0x2c62, 0x2c62,      0, -10743},
	{0x2c63, 0x2c63,      0,  -3814}, {0x2c64, 0x2c64,      0, -10727}, {0x2c65, 0x2c65, -10795,      0},
	{0x2c66, 0x2c66, -10792,      0}, {0x2c67, 0x2c6c,  65535,  65535}, {0x2c6d, 0x2c6d,      0, -10780},
	{0x2c6e, 0x2c6e,      0, -10749}, {0x2c6f, 0x2c6f,      0, -10783}, {0x2c70, 0x2c70,      0, -10782},
	{0x2c72, 0x2c76,  65535,  65535}, {0x2c7e, 0x2c7f,      0, -10815}, {0x2c80, 0x2cf3,  65535,  65535},
	{0x2d00, 0x2d25,  -7264,      0}, {0x2d27, 0x2d27,  -7264,      0}, {0x2d2d, 0x2d2d,  -7264,      0},
	{0xa640, 0xa77c,  65535,  65535}, {0xa77d, 0xa77d,      0, -35332}, {0xa77e, 0xa78c,  65535,  65535},
	{0xa78d, 0xa78d,      0, -42280}, {0xa790, 0xa793,  65535,  65535}, {0xa794, 0xa794,     48,      0},
	{0xa796, 0xa7a9,  65535,  65535}, {0xa7aa, 0xa7aa,      0, -42308}, {0xa7ab, 0xa7ab,      0, -42319},
	{0xa7ac, 0xa7ac,      0, -42315}, {0xa7ad, 0xa7ad,      0, -42305}, {0xa7ae, 0xa7ae,      0, -42308},
	{0xa7b0, 0xa7b0,      0, -42258}, {0xa7b1, 0xa7b1,      0, -42282}, {0xa7b2, 0xa7b2,      0, -42261},
	{0xa7b3, 0xa7b3,      0,    928}, {0xa7b4, 0xa7c3,  65535,  65535}, {0xa7c4, 0xa7c4,      0,    -48},
	{0xa7c5, 0xa7c5,      0, -42307}, {0xa7c6, 0xa7c6,      0, -35384}, {0xa7c7, 0xa7ca,  65535,  65535},
	{0xa7cb, 0xa7cb,      0, -42343}, {0xa7cc, 0xa7db,  65535,  65535}, {0xa7dc, 0xa7dc,      0, -42561},
	{0xa7f5, 0xa7f6,  65535,  65535}, {0xab53, 0xab53,   -928,      0}, {0xab70, 0xabbf, -38864,      0},
	{0xff21, 0xff3a,      0,     32}, {0xff41, 0xff5a,    -32,      0}
};


//
//	case32
//

#if defined(IMGUI_USE_WCHAR32)

static CaseRange32 case32[] = {
	{0x10400, 0x10427,   0, 40}, {0x10428, 0x1044f, -40,  0}, {0x104b0, 0x104d3,   0, 40}, {0x104d8, 0x104fb, -40,  0},
	{0x10570, 0x1057a,   0, 39}, {0x1057c, 0x1058a,   0, 39}, {0x1058c, 0x10592,   0, 39}, {0x10594, 0x10595,   0, 39},
	{0x10597, 0x105a1, -39,  0}, {0x105a3, 0x105b1, -39,  0}, {0x105b3, 0x105b9, -39,  0}, {0x105bb, 0x105bc, -39,  0},
	{0x10c80, 0x10cb2,   0, 64}, {0x10cc0, 0x10cf2, -64,  0}, {0x10d50, 0x10d65,   0, 32}, {0x10d70, 0x10d85, -32,  0},
	{0x118a0, 0x118bf,   0, 32}, {0x118c0, 0x118df, -32,  0}, {0x16e40, 0x16e5f,   0, 32}, {0x16e60, 0x16e7f, -32,  0},
	{0x16ea0, 0x16eb8,   0, 27}, {0x16ebb, 0x16ed3, -27,  0}, {0x1e900, 0x1e921,   0, 34}, {0x1e922, 0x1e943, -34,  0}
};

#endif


//
//	rangeContains
//

template <typename T, typename C>
bool rangeContains(const T& table, C codepoint)
{
	auto low = std::begin(table);
	auto high = std::end(table);

	while (low <= high)
	{
		auto mid = low + (high - low) / 2;

		if (codepoint >= mid->low && codepoint <= mid->high)
		{
			return (mid->stride == 1) || ((codepoint - mid->low) % mid->stride == 0);

		}
		else if (codepoint < mid->low)
		{
			high = mid - 1;

		}
		else
		{
			low = mid + 1;
		}
	}

	return false;
}


//
//	caseRangeFind
//

template <typename T, typename C>
const CaseRange<C>* caseRangeFind(const T& table, C codepoint)
{
	auto low = std::begin(table);
	auto high = std::end(table);

	while (low <= high)
	{
		auto mid = low + (high - low) / 2;

		if (codepoint >= mid->low && codepoint <= mid->high)
		{
			return mid;

		}
		else if (codepoint < mid->low)
		{
			high = mid - 1;

		}
		else
		{
			low = mid + 1;
		}
	}

	return nullptr;
}


//
//	caseRangeToUpper
//

template <typename T, typename C>
C caseRangeToUpper(const T& table, C codepoint)
{
	auto caseRange = caseRangeFind(table, codepoint);

	if (!caseRange || caseRange->toUpper == 0)
	{
		return codepoint;

	}
	else if (caseRange->toUpper == 0xffff)
	{
		return codepoint & ~0x1;
	}

	else
	{
		return static_cast<C>(static_cast<int32_t>(codepoint) + caseRange->toUpper);
	}
}


//
//	caseRangeToLower
//

template <typename T, typename C>
C caseRangeToLower(const T& table, C codepoint)
{
	auto caseRange = caseRangeFind(table, codepoint);

	if (!caseRange || caseRange->toLower == 0)
	{
		return codepoint;

	}
	else if (caseRange->toLower == 0xffff)
	{
		return codepoint | 0x1;
	}

	else
	{
		return static_cast<C>(static_cast<int32_t>(codepoint) + caseRange->toLower);
	}
}


//
//	Internal type conversions because "char" is signed
//

static inline ImWchar uch(char c)
{
	return static_cast<ImWchar>(c);
}

static inline char sch(ImWchar i)
{
	return static_cast<char>(i);
}


//
//	skipBOM
//

std::string_view::const_iterator TextEditor::CodePoint::skipBOM(std::string_view::const_iterator i, std::string_view::const_iterator end)
{
	// skip Byte Order Mark (BOM) just in case there is one

	// Note: the standard states that:
	// Use of a BOM is neither required nor recommended for UTF-8
	static constexpr char bom1 = static_cast<char>(0xEF);
	static constexpr char bom2 = static_cast<char>(0xBB);
	static constexpr char bom3 = static_cast<char>(0xBF);
	return ((end - i) >= 3 && i[0] == bom1 && i[1] == bom2 && i[2] == bom3) ? i + 3 : i;
}


//
//	TextEditor::CodePoint::read
//

std::string_view::const_iterator TextEditor::CodePoint::read(std::string_view::const_iterator i, std::string_view::const_iterator end, ImWchar* codepoint)
{
	// parse a UTF-8 sequence into a unicode codepoint
	if (i < end && (uch(*i) & 0x80) == 0)
	{
		*codepoint = uch(*i);
		i++;

	}
	else if (i + 1 < end && (uch(*i) & 0xE0) == 0xC0)
	{
		*codepoint = ((uch(*i) & 0x1f) << 6) | (uch(*(i + 1)) & 0x3f);
		i += 2;

	}
	else if (i + 2 < end && (uch(*i) & 0xF0) == 0xE0)
	{
		*codepoint = ((uch(*i) & 0x0f) << 12) | ((uch(*(i + 1)) & 0x3f) << 6) | (uch(*(i + 2)) & 0x3f);
		i += 3;

	}
	else if (i + 3 < end && (uch(*i) & 0xF8) == 0xF0)
	{
#if defined(IMGUI_USE_WCHAR32)
		*codepoint = ((uch(*i) & 0x07) << 18) | ((uch(*(i + 1)) & 0x3f) << 12) | ((uch(*(i + 2)) & 0x3f) << 6) | (uch(*(i + 3)) & 0x3f);
#else
		* codepoint = IM_UNICODE_CODEPOINT_INVALID;
#endif
		i += 4;

	}
	else
	{
		*codepoint = IM_UNICODE_CODEPOINT_INVALID;
		i++;
	}

	return i;
}


//
//	TextEditor::CodePoint::write
//

size_t TextEditor::CodePoint::write(char* start, ImWchar codepoint)
{
	// generate UTF-8 sequence from a unicode codepoint
	auto i = start;

	if (codepoint < 0x80)
	{
		*i++ = sch(codepoint);

	}
	else if (codepoint < 0x800)
	{
		*i++ = sch(0xc0 | ((codepoint >> 6) & 0x1f));
		*i++ = sch(0x80 | (codepoint & 0x3f));

#if defined(IMGUI_USE_WCHAR32)
	}
	else if (codepoint < 0x10000)
	{
		*i++ = sch(0xe0 | ((codepoint >> 12) & 0x0f));
		*i++ = sch(0x80 | ((codepoint >> 6) & 0x3f));
		*i++ = sch(0x80 | (codepoint & 0x3f));

	}
	else if (codepoint >= 0x110000)
	{
		codepoint = IM_UNICODE_CODEPOINT_INVALID;
		*i++ = sch(0xe0 | ((codepoint >> 12) & 0x0f));
		*i++ = sch(0x80 | ((codepoint >> 6) & 0x3f));
		*i++ = sch(0x80 | (codepoint & 0x3f));

	}
	else
	{
		*i++ = sch(0xf0 | ((codepoint >> 18) & 0x07));
		*i++ = sch(0x80 | ((codepoint >> 12) & 0x3f));
		*i++ = sch(0x80 | ((codepoint >> 6) & 0x3f));
		*i++ = sch(0x80 | (codepoint & 0x3f));

#else
	}
	else
	{
		*i++ = sch(0xe0 | ((codepoint >> 12) & 0x0f));
		*i++ = sch(0x80 | ((codepoint >> 6) & 0x3f));
		*i++ = sch(0x80 | (codepoint & 0x3f));
#endif
	}

	return i - start;
}


//
//	TextEditor::CodePoint::isLetter
//

bool TextEditor::CodePoint::isLetter(ImWchar codepoint)
{
	if (codepoint < 0x7f)
	{
		return static_cast<unsigned>((codepoint | 32) - 'a') < 26;

#if defined(IMGUI_USE_WCHAR32)
	}
	else if (codepoint >= 0x10000)
	{
		return rangeContains(letters32, static_cast<ImWchar32>(codepoint));
#endif

	}
	else
	{
		return rangeContains(letters16, static_cast<ImWchar16>(codepoint));
	}
}


//
//	TextEditor::CodePoint::isNumber
//

bool TextEditor::CodePoint::isNumber(ImWchar codepoint)
{
	if (codepoint < 0x7f)
	{
		return static_cast<unsigned>(codepoint - '0') < 10;

#if defined(IMGUI_USE_WCHAR32)
	}
	else if (codepoint >= 0x10000)
	{
		return rangeContains(numbers32, static_cast<ImWchar32>(codepoint));
#endif

	}
	else
	{
		return rangeContains(numbers16, static_cast<ImWchar16>(codepoint));
	}
}


//
//	TextEditor::CodePoint::isXidStart
//

bool TextEditor::CodePoint::isXidStart(ImWchar codepoint)
{
	if (codepoint < 0x7f)
	{
		return codepoint == '_' || static_cast<unsigned>((codepoint | 32) - 'a') < 26;

#if defined(IMGUI_USE_WCHAR32)
	}
	else if (codepoint >= 0x10000)
	{
		return rangeContains(xidStart32, static_cast<ImWchar32>(codepoint));
#endif

	}
	else
	{
		return rangeContains(xidStart16, static_cast<ImWchar16>(codepoint));
	}
}


//
//	TextEditor::CodePoint::isWhiteSpace
//

bool TextEditor::CodePoint::isWhiteSpace(ImWchar codepoint)
{
	if (codepoint < 0x7f)
	{
		return codepoint == ' ' || static_cast<unsigned>(codepoint - '\t') < 5;

#if defined(IMGUI_USE_WCHAR32)
	}
	else if (codepoint >= 0x10000)
	{
		return false;
#endif

	}
	else
	{
		return rangeContains(whitespace16, static_cast<ImWchar16>(codepoint));
	}
}


//
//	TextEditor::CodePoint::isWord
//

bool TextEditor::CodePoint::isWord(ImWchar codepoint)
{
	if (codepoint < 0x7f)
	{
		return
			(static_cast<unsigned>((codepoint | 32) - 'a') < 26) ||
			(static_cast<unsigned>(codepoint - '0') < 10) ||
			codepoint == '_';

#if defined(IMGUI_USE_WCHAR32)
	}
	else if (codepoint >= 0x10000)
	{
		return
			rangeContains(letters32, static_cast<ImWchar32>(codepoint)) ||
			rangeContains(numbers32, static_cast<ImWchar32>(codepoint)) ||
			codepoint == '_';
#endif

	}
	else
	{
		return
			rangeContains(letters16, static_cast<ImWchar16>(codepoint)) ||
			rangeContains(numbers16, static_cast<ImWchar16>(codepoint)) ||
			codepoint == '_';
	}
}


//
//	TextEditor::CodePoint::isXidContinue
//

bool TextEditor::CodePoint::isXidContinue(ImWchar codepoint)
{
	if (codepoint < 0x7f)
	{
		return codepoint == '_' || (static_cast<unsigned>((codepoint | 32) - 'a') < 26) || (static_cast<unsigned>(codepoint - '0') < 10);

#if defined(IMGUI_USE_WCHAR32)
	}
	else if (codepoint >= 0x10000)
	{
		return rangeContains(xidContinue32, static_cast<ImWchar16>(codepoint));
#endif

	}
	else
	{
		return rangeContains(xidContinue16, static_cast<ImWchar16>(codepoint));
	}
}


//
//	TextEditor::CodePoint::isLower
//

bool TextEditor::CodePoint::isLower(ImWchar codepoint)
{
	if (codepoint < 0x7f)
	{
		return static_cast<unsigned>(codepoint - 'a') < 26;

#if defined(IMGUI_USE_WCHAR32)
	}
	else if (codepoint >= 0x10000)
	{
		return rangeContains(lower32, static_cast<ImWchar32>(codepoint));
#endif

	}
	else
	{
		return rangeContains(lower16, static_cast<ImWchar16>(codepoint));
	}
}


//
//	TextEditor::CodePoint::isUpper
//

bool TextEditor::CodePoint::isUpper(ImWchar codepoint)
{
	if (codepoint < 0x7f)
	{
		return static_cast<unsigned>(codepoint - 'A') < 26;

#if defined(IMGUI_USE_WCHAR32)
	}
	else if (codepoint >= 0x10000)
	{
		return rangeContains(upper32, static_cast<ImWchar32>(codepoint));
#endif

	}
	else
	{
		return rangeContains(upper16, static_cast<ImWchar16>(codepoint));
	}
}


//
//	TextEditor::CodePoint::toUpper
//

ImWchar TextEditor::CodePoint::toUpper(ImWchar codepoint)
{
	if (codepoint < 0x7f)
	{
		return (static_cast<unsigned>(codepoint - 'a') < 26) ? codepoint & 0x5f : codepoint;

#if defined(IMGUI_USE_WCHAR32)
	}
	else if (codepoint >= 0x10000)
	{
		return caseRangeToUpper(case32, static_cast<char32_t>(codepoint));
#endif

	}
	else
	{
		return caseRangeToUpper(case16, static_cast<char16_t>(codepoint));
	}
}


//
//	TextEditor::CodePoint::toLower
//

ImWchar TextEditor::CodePoint::toLower(ImWchar codepoint)
{
	if (codepoint < 0x7f)
	{
		return (static_cast<unsigned>(codepoint - 'A') < 26) ? codepoint | 32 : codepoint;

#if defined(IMGUI_USE_WCHAR32)
	}
	else if (codepoint >= 0x10000)
	{
		return caseRangeToLower(case32, static_cast<char32_t>(codepoint));
#endif

	}
	else
	{
		return caseRangeToLower(case16, static_cast<char16_t>(codepoint));
	}
}


//
//	getCStyleIdentifier
//

static TextEditor::Iterator getCStyleIdentifier(TextEditor::Iterator start, TextEditor::Iterator end)
{
	if (start < end && TextEditor::CodePoint::isXidStart(*start))
	{
		start++;

		while (start < end && TextEditor::CodePoint::isXidContinue(*start))
		{
			start++;
		}
	}

	return start;
}


//
//	getCStyleNumber
//

static TextEditor::Iterator getCStyleNumber(TextEditor::Iterator start, TextEditor::Iterator end)
{
	TextEditor::Iterator i = start;
	TextEditor::Iterator marker;


	{
		ImWchar yych;
		unsigned int yyaccept = 0;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '.': goto yy3;
		case '0': goto yy4;
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy6;
		default:
			if (i >= end) goto yy82;
			goto yy1;
		}
	yy1:
		++i;
	yy2:
		{ return start; }
	yy3:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy8;
		default: goto yy2;
		}
	yy4:
		yyaccept = 0;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 0x00: goto yy5;
		case 'B':
		case 'b': goto yy16;
		case 'X':
		case 'x': goto yy20;
		default: goto yy13;
		}
	yy5:
		{ return i; }
	yy6:
		yyaccept = 1;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '.': goto yy10;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy6;
		case 'E':
		case 'e': goto yy17;
		case 'L': goto yy22;
		case 'U':
		case 'u': goto yy23;
		case 'l': goto yy24;
		default: goto yy7;
		}
	yy7:
		{ return i; }
	yy8:
		yyaccept = 2;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy8;
		case 'E':
		case 'e': goto yy25;
		case 'F':
		case 'L':
		case 'f':
		case 'l': goto yy26;
		default: goto yy9;
		}
	yy9:
		{ return i; }
	yy10:
		yyaccept = 3;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy8;
		case 'E':
		case 'e': goto yy27;
		case 'F':
		case 'L':
		case 'f':
		case 'l': goto yy28;
		default: goto yy11;
		}
	yy11:
		{ return i; }
	yy12:
		yyaccept = 0;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
	yy13:
		switch (yych)
		{
		case '.': goto yy10;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7': goto yy12;
		case '8':
		case '9': goto yy14;
		case 'E':
		case 'e': goto yy17;
		case 'L': goto yy18;
		case 'U':
		case 'u': goto yy19;
		case 'l': goto yy21;
		default: goto yy5;
		}
	yy14:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '.': goto yy10;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy14;
		case 'E':
		case 'e': goto yy17;
		default: goto yy15;
		}
	yy15:
		i = marker;
		switch (yyaccept)
		{
		case 0: goto yy5;
		case 1: goto yy7;
		case 2: goto yy9;
		case 3: goto yy11;
		default: goto yy40;
		}
	yy16:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1': goto yy29;
		default: goto yy15;
		}
	yy17:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '+':
		case '-': goto yy31;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy32;
		default: goto yy15;
		}
	yy18:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy34;
		case 'U':
		case 'u': goto yy35;
		default: goto yy5;
		}
	yy19:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy36;
		case 'l': goto yy37;
		default: goto yy5;
		}
	yy20:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '.': goto yy38;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f': goto yy39;
		default: goto yy15;
		}
	yy21:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy35;
		case 'l': goto yy34;
		default: goto yy5;
		}
	yy22:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy41;
		case 'U':
		case 'u': goto yy42;
		default: goto yy7;
		}
	yy23:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy43;
		case 'l': goto yy44;
		default: goto yy7;
		}
	yy24:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy42;
		case 'l': goto yy41;
		default: goto yy7;
		}
	yy25:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '+':
		case '-': goto yy45;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy46;
		default: goto yy15;
		}
	yy26:
		++i;
		goto yy9;
	yy27:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '+':
		case '-': goto yy47;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy48;
		default: goto yy15;
		}
	yy28:
		++i;
		goto yy11;
	yy29:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1': goto yy29;
		case 'L': goto yy49;
		case 'U':
		case 'u': goto yy50;
		case 'l': goto yy51;
		default: goto yy30;
		}
	yy30:
		{ return i; }
	yy31:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy32;
		default: goto yy15;
		}
	yy32:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy32;
		case 'F':
		case 'L':
		case 'f':
		case 'l': goto yy52;
		default: goto yy33;
		}
	yy33:
		{ return i; }
	yy34:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy35;
		default: goto yy5;
		}
	yy35:
		++i;
		goto yy5;
	yy36:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy35;
		default: goto yy5;
		}
	yy37:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'l': goto yy35;
		default: goto yy5;
		}
	yy38:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 0x00:
		case 'P':
		case 'p': goto yy15;
		default: goto yy54;
		}
	yy39:
		yyaccept = 4;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '.': goto yy55;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f': goto yy39;
		case 'L': goto yy56;
		case 'P':
		case 'p': goto yy57;
		case 'U':
		case 'u': goto yy58;
		case 'l': goto yy59;
		default: goto yy40;
		}
	yy40:
		{ return i; }
	yy41:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy42;
		default: goto yy7;
		}
	yy42:
		++i;
		goto yy7;
	yy43:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy42;
		default: goto yy7;
		}
	yy44:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'l': goto yy42;
		default: goto yy7;
		}
	yy45:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy46;
		default: goto yy15;
		}
	yy46:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy46;
		case 'F':
		case 'L':
		case 'f':
		case 'l': goto yy26;
		default: goto yy9;
		}
	yy47:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy48;
		default: goto yy15;
		}
	yy48:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy48;
		case 'F':
		case 'L':
		case 'f':
		case 'l': goto yy28;
		default: goto yy11;
		}
	yy49:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy60;
		case 'U':
		case 'u': goto yy61;
		default: goto yy30;
		}
	yy50:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy62;
		case 'l': goto yy63;
		default: goto yy30;
		}
	yy51:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy61;
		case 'l': goto yy60;
		default: goto yy30;
		}
	yy52:
		++i;
		goto yy33;
	yy53:
		++i;
		yych = i < end ? *i : 0;
	yy54:
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f': goto yy53;
		case 'P':
		case 'p': goto yy64;
		default: goto yy15;
		}
	yy55:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 0x00: goto yy15;
		case 'P':
		case 'p': goto yy65;
		default: goto yy54;
		}
	yy56:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy66;
		case 'U':
		case 'u': goto yy67;
		default: goto yy40;
		}
	yy57:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '+':
		case '-': goto yy68;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy69;
		default: goto yy15;
		}
	yy58:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy71;
		case 'l': goto yy72;
		default: goto yy40;
		}
	yy59:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy67;
		case 'l': goto yy66;
		default: goto yy40;
		}
	yy60:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy61;
		default: goto yy30;
		}
	yy61:
		++i;
		goto yy30;
	yy62:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy61;
		default: goto yy30;
		}
	yy63:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'l': goto yy61;
		default: goto yy30;
		}
	yy64:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '+':
		case '-': goto yy73;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy74;
		default: goto yy15;
		}
	yy65:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '+':
		case '-': goto yy76;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy77;
		default: goto yy15;
		}
	yy66:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy67;
		default: goto yy40;
		}
	yy67:
		++i;
		goto yy40;
	yy68:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy69;
		default: goto yy15;
		}
	yy69:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy69;
		case 'F':
		case 'L':
		case 'f':
		case 'l': goto yy79;
		default: goto yy70;
		}
	yy70:
		{ return i; }
	yy71:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy67;
		default: goto yy40;
		}
	yy72:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'l': goto yy67;
		default: goto yy40;
		}
	yy73:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy74;
		default: goto yy15;
		}
	yy74:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy74;
		case 'F':
		case 'L':
		case 'f':
		case 'l': goto yy80;
		default: goto yy75;
		}
	yy75:
		{ return i; }
	yy76:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy77;
		default: goto yy15;
		}
	yy77:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy77;
		case 'F':
		case 'L':
		case 'f':
		case 'l': goto yy81;
		default: goto yy78;
		}
	yy78:
		{ return i; }
	yy79:
		++i;
		goto yy70;
	yy80:
		++i;
		goto yy75;
	yy81:
		++i;
		goto yy78;
	yy82:
		{ return start; }
	}

}


//
//	isCStylePunctuation
//

static bool isCStylePunctuation(ImWchar character)
{
	static bool punctuation[128] = {
		false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
		false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
		false,  true, false, false, false,  true,  true, false,  true,  true,  true,  true,  true,  true,  true,  true,
		false, false, false, false, false, false, false, false, false, false,  true,  true,  true,  true,  true,  true,
		false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
		false, false, false, false, false, false, false, false, false, false, false,  true, false,  true,  true, false,
		false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
		false, false, false, false, false, false, false, false, false, false, false,  true,  true,  true,  true, false,
	};

	return character < 127 ? punctuation[character] : false;
}


//
//	TextEditor::Language::C
//

const TextEditor::Language* TextEditor::Language::C()
{
	static bool initialized = false;
	static TextEditor::Language language;

	if (!initialized)
	{
		language.name = "C";
		language.preprocess = '#';
		language.singleLineComment = "//";
		language.commentStart = "/*";
		language.commentEnd = "*/";
		language.hasSingleQuotedStrings = true;
		language.hasDoubleQuotedStrings = true;
		language.stringEscape = '\\';

		static const char* const keywords[] = {
			"break", "case", "continue", "default", "do", "else", "for", "goto", "if", "return", "sizeof",
			"switch", "while", "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
			"_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local"
		};

		static const char* const declarations[] = {
			"auto", "char", "const", "double", "enum", "extern", "float", "inline", "int", "long", "register",
			"restrict", "short", "signed", "static", "struct", "typedef", "union", "unsigned", "void", "volatile"
		};

		for (auto& keyword : keywords) { language.keywords.insert(keyword); }
		for (auto& declaration : declarations) { language.declarations.insert(declaration); }

		language.isPunctuation = isCStylePunctuation;
		language.getIdentifier = getCStyleIdentifier;
		language.getNumber = getCStyleNumber;
		initialized = true;
	}

	return &language;
}


//
//	TextEditor::Language::Cpp
//

// Unreal / COM-style type-name convention: a prefix in {F,U,A,T,E,I,S} followed by
// an uppercase letter, and CamelCase (has a lowercase char — so SCREAMING_CASE macros
// like FILE/TRUE aren't miscolored). Covers the thousands of UE types that can't be
// enumerated (FMyStruct, UMyComponent, AMyActor, TMyTemplate, EMyEnum, IMyInterface).
static bool cppUnrealTypeLike(const std::string& id)
{
	if (id.size() < 3)
		return false;
	char c0 = id[0], c1 = id[1];
	if (c0 != 'F' && c0 != 'U' && c0 != 'A' && c0 != 'T' && c0 != 'E' && c0 != 'I' && c0 != 'S')
		return false;
	if (c1 < 'A' || c1 > 'Z')
		return false;
	for (char c : id)
		if (c >= 'a' && c <= 'z')
			return true; // CamelCase → a type, not an all-caps macro
	return false;
}

const TextEditor::Language* TextEditor::Language::Cpp()
{
	static bool initialized = false;
	static TextEditor::Language language;

	if (!initialized)
	{
		language.name = "C++";
		language.preprocess = '#';
		language.singleLineComment = "//";
		language.commentStart = "/*";
		language.commentEnd = "*/";
		language.hasSingleQuotedStrings = true;
		language.hasDoubleQuotedStrings = true;
		language.stringEscape = '\\';

		static const char* const keywords[] = {
			"alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit", "atomic_noexcept",
			"bitand", "bitor", "break", "case", "catch", "compl", "const_cast", "continue", "default", "delete",
			"do", "dynamic_cast", "else", "explicit", "export", "extern", "false", "for", "goto", "if", "import",
			"new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "reinterpret_cast", "requires",
			"return", "sizeof", "static_assert", "static_cast", "switch", "synchronized", "this", "thread_local",
			"throw", "true", "try", "while", "xor", "xor_eq"
		};

		static const char* const declarations[] = {
			"auto", "bool", "char", "char16_t", "char32_t", "class", "concept", "const", "constexpr", "decltype",
			"double", "explicit", "export", "extern", "enum", "extern", "float", "friend", "inline", "int", "long",
			"module", "mutable", "namespace", "private", "protected", "public", "register", "restrict", "short",
			"signed", "static", "struct", "template", "typedef", "typeid", "typename", "union", "using", "unsigned",
			"virtual", "void", "volatile", "wchar_t"
		};

		// Unreal Engine fundamental types, containers, smart pointers, math and
		// string types — colored as types (declarations). UE aliases int32/uint8/
		// FString/TArray/... that the base C++ set doesn't know, so "int32 X;" went
		// uncolored. Harmless in non-UE C++ (these names simply don't appear there).
		static const char* const ueTypes[] = {
			"int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64",
			"TCHAR", "ANSICHAR", "WIDECHAR", "UTF8CHAR", "UCS2CHAR", "CHAR8", "CHAR16", "CHAR32",
			"SIZE_T", "SSIZE_T", "PTRINT", "UPTRINT",
			"FString", "FName", "FText", "FStringView", "FAnsiStringView",
			"TArray", "TArrayView", "TMap", "TMultiMap", "TSet", "TQueue", "TStaticArray",
			"TSharedPtr", "TSharedRef", "TWeakPtr", "TUniquePtr", "TWeakObjectPtr", "TStrongObjectPtr",
			"TObjectPtr", "TSoftObjectPtr", "TSoftClassPtr", "TSubclassOf", "TScriptInterface",
			"TOptional", "TFunction", "TUniqueFunction", "TTuple", "TPair", "TVariant", "TEnumAsByte",
			"FVector", "FVector2D", "FVector4", "FRotator", "FQuat", "FTransform", "FMatrix",
			"FColor", "FLinearColor", "FIntPoint", "FIntVector", "FBox", "FBox2D", "FGuid",
			"FDateTime", "FTimespan", "FDelegateHandle"
		};
		// UE reflection + logging + assertion macros — colored as keywords.
		static const char* const ueMacros[] = {
			"UCLASS", "UPROPERTY", "UFUNCTION", "USTRUCT", "UENUM", "UINTERFACE", "UDELEGATE", "UPARAM",
			"UMETA", "GENERATED_BODY", "GENERATED_UCLASS_BODY", "GENERATED_USTRUCT_BODY",
			"DECLARE_DYNAMIC_MULTICAST_DELEGATE", "DECLARE_MULTICAST_DELEGATE", "DECLARE_DELEGATE",
			"UE_LOG", "UE_LOGFMT", "UE_CLOG", "checkf", "checkNoEntry", "checkNoReentry", "checkSlow",
			"ensureMsgf", "ensureAlways", "verifyf", "unimplemented",
			"FORCEINLINE", "FORCENOINLINE", "UE_DEPRECATED", "UE_NODISCARD",
			"LOCTEXT", "NSLOCTEXT", "TEXT", "INVTEXT", "PURE_VIRTUAL"
		};

		for (auto& keyword : keywords) { language.keywords.insert(keyword); }
		for (auto& declaration : declarations) { language.declarations.insert(declaration); }
		for (auto& t : ueTypes) { language.declarations.insert(t); }
		for (auto& m : ueMacros) { language.keywords.insert(m); }
		language.isTypeLike = cppUnrealTypeLike; // catch unlisted F*/U*/A*/T*/E*/I*/S* types

		language.isPunctuation = isCStylePunctuation;
		language.getIdentifier = getCStyleIdentifier;
		language.getNumber = getCStyleNumber;
		initialized = true;
	}

	return &language;
}


//
//	getCsStyleNumber
//

static TextEditor::Iterator getCsStyleNumber(TextEditor::Iterator start, TextEditor::Iterator end)
{
	TextEditor::Iterator i = start;
	TextEditor::Iterator marker;


	{
		ImWchar yych;
		unsigned int yyaccept = 0;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '.': goto yy3;
		case '0': goto yy4;
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy6;
		default:
			if (i >= end) goto yy49;
			goto yy1;
		}
	yy1:
		++i;
	yy2:
		{ return start; }
	yy3:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy8;
		default: goto yy2;
		}
	yy4:
		yyaccept = 0;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 0x00: goto yy5;
		case 'B':
		case 'b': goto yy12;
		case 'X':
		case 'x': goto yy15;
		default: goto yy7;
		}
	yy5:
		{ return i; }
	yy6:
		yyaccept = 0;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
	yy7:
		switch (yych)
		{
		case '.': goto yy10;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '_': goto yy6;
		case 'L': goto yy13;
		case 'U':
		case 'u': goto yy14;
		case 'l': goto yy16;
		default: goto yy5;
		}
	yy8:
		yyaccept = 1;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '_': goto yy8;
		case 'E':
		case 'e': goto yy17;
		case 'F':
		case 'L':
		case 'f':
		case 'l': goto yy18;
		default: goto yy9;
		}
	yy9:
		{ return i; }
	yy10:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy19;
		default: goto yy11;
		}
	yy11:
		i = marker;
		switch (yyaccept)
		{
		case 0: goto yy5;
		case 1: goto yy9;
		default: goto yy20;
		}
	yy12:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1': goto yy21;
		default: goto yy11;
		}
	yy13:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy23;
		case 'U':
		case 'u': goto yy24;
		default: goto yy5;
		}
	yy14:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy25;
		case 'l': goto yy26;
		default: goto yy5;
		}
	yy15:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f': goto yy27;
		default: goto yy11;
		}
	yy16:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy24;
		case 'l': goto yy23;
		default: goto yy5;
		}
	yy17:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '+':
		case '-': goto yy29;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '_': goto yy30;
		default: goto yy11;
		}
	yy18:
		++i;
		goto yy9;
	yy19:
		yyaccept = 2;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '_': goto yy19;
		case 'E':
		case 'e': goto yy31;
		case 'F':
		case 'L':
		case 'f':
		case 'l': goto yy32;
		default: goto yy20;
		}
	yy20:
		{ return i; }
	yy21:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '_': goto yy21;
		case 'L': goto yy33;
		case 'U':
		case 'u': goto yy34;
		case 'l': goto yy35;
		default: goto yy22;
		}
	yy22:
		{ return i; }
	yy23:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy24;
		default: goto yy5;
		}
	yy24:
		++i;
		goto yy5;
	yy25:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy24;
		default: goto yy5;
		}
	yy26:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'l': goto yy24;
		default: goto yy5;
		}
	yy27:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case '_':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f': goto yy27;
		case 'L': goto yy36;
		case 'U':
		case 'u': goto yy37;
		case 'l': goto yy38;
		default: goto yy28;
		}
	yy28:
		{ return i; }
	yy29:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '_': goto yy30;
		default: goto yy11;
		}
	yy30:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '_': goto yy30;
		case 'F':
		case 'L':
		case 'f':
		case 'l': goto yy18;
		default: goto yy9;
		}
	yy31:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '+':
		case '-': goto yy39;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '_': goto yy40;
		default: goto yy11;
		}
	yy32:
		++i;
		goto yy20;
	yy33:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy41;
		case 'U':
		case 'u': goto yy42;
		default: goto yy22;
		}
	yy34:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy43;
		case 'l': goto yy44;
		default: goto yy22;
		}
	yy35:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy42;
		case 'l': goto yy41;
		default: goto yy22;
		}
	yy36:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy45;
		case 'U':
		case 'u': goto yy46;
		default: goto yy28;
		}
	yy37:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy47;
		case 'l': goto yy48;
		default: goto yy28;
		}
	yy38:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy46;
		case 'l': goto yy45;
		default: goto yy28;
		}
	yy39:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '_': goto yy40;
		default: goto yy11;
		}
	yy40:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '_': goto yy40;
		case 'F':
		case 'L':
		case 'f':
		case 'l': goto yy32;
		default: goto yy20;
		}
	yy41:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy42;
		default: goto yy22;
		}
	yy42:
		++i;
		goto yy22;
	yy43:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy42;
		default: goto yy22;
		}
	yy44:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'l': goto yy42;
		default: goto yy22;
		}
	yy45:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'U':
		case 'u': goto yy46;
		default: goto yy28;
		}
	yy46:
		++i;
		goto yy28;
	yy47:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'L': goto yy46;
		default: goto yy28;
		}
	yy48:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'l': goto yy46;
		default: goto yy28;
		}
	yy49:
		{ return start; }
	}

}


//
//	TextEditor::Language::Cs
//

const TextEditor::Language* TextEditor::Language::Cs()
{
	static bool initialized = false;
	static TextEditor::Language language;

	if (!initialized)
	{
		language.name = "C#";
		language.preprocess = '#';
		language.singleLineComment = "//";
		language.commentStart = "/*";
		language.commentEnd = "*/";
		language.hasSingleQuotedStrings = true;
		language.hasDoubleQuotedStrings = true;
		language.stringEscape = '\\';

		static const char* const keywords[] = {
			"abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class", "const", "continue",
			"decimal", "default", "delegate", "do", "double", "else", "enum", "event", "explicit", "extern", "false", "finally",
			"fixed", "float", "for", "foreach", "goto", "if", "implicit", "in", "in (generic modifier)", "int", "interface",
			"internal", "is", "lock", "long", "namespace", "new", "null", "object", "operator", "out", "override", "params",
			"private", "protected", "public", "readonly", "ref", "return", "sbyte", "sealed", "short", "sizeof", "stackalloc",
			"static", "string", "struct", "switch", "this", "throw", "true", "try", "typeof", "uint", "ulong", "unchecked",
			"unsafe", "ushort", "using", "using static", "void", "volatile", "while"
		};

		for (auto& keyword : keywords) { language.keywords.insert(keyword); }

		language.isPunctuation = isCStylePunctuation;
		language.getIdentifier = getCStyleIdentifier;
		language.getNumber = getCsStyleNumber;
		initialized = true;
	}

	return &language;
}


//
//	TextEditor::Language::AngelScript
//

const TextEditor::Language* TextEditor::Language::AngelScript()
{
	static bool initialized = false;
	static TextEditor::Language language;

	if (!initialized)
	{
		language.name = "AngelScript";
		language.preprocess = '#';
		language.singleLineComment = "//";
		language.commentStart = "/*";
		language.commentEnd = "*/";
		language.hasSingleQuotedStrings = true;
		language.hasDoubleQuotedStrings = true;
		language.stringEscape = '\\';

		static const char* const keywords[] = {
			"and", "abstract", "auto", "bool", "break", "case", "cast", "class", "const", "continue", "default",
			"do", "double", "else", "enum", "false", "final", "float", "for", "from", "funcdef", "function", "get",
			"if", "import", "in", "inout", "int", "interface", "int8", "int16", "int32", "int64", "is", "mixin",
			"namespace", "not", "null", "or", "out", "override", "private", "protected", "return", "set", "shared",
			"super", "switch", "this ", "true", "typedef", "uint", "uint8", "uint16", "uint32", "uint64", "void",
			"while", "xor"
		};

		for (auto& keyword : keywords) { language.keywords.insert(keyword); }

		language.isPunctuation = isCStylePunctuation;
		language.getIdentifier = getCStyleIdentifier;
		language.getNumber = getCStyleNumber;
		initialized = true;
	}

	return &language;
}


//
//	getLuaStyleNumber
//

static TextEditor::Iterator getLuaStyleNumber(TextEditor::Iterator start, TextEditor::Iterator end)
{
	TextEditor::Iterator i = start;
	TextEditor::Iterator marker;


	{
		ImWchar yych;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '.': goto yy3;
		case '0': goto yy5;
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy6;
		case 'E':
		case 'e': goto yy8;
		default:
			if (i >= end) goto yy1;
			goto yy2;
		}
	yy1:
		{ return i; }
	yy2:
		++i;
		{ return start; }
	yy3:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy3;
		case 'E':
		case 'e': goto yy8;
		default: goto yy4;
		}
	yy4:
		{ return i; }
	yy5:
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 0x00: goto yy1;
		case 'X':
		case 'x': goto yy9;
		default: goto yy7;
		}
	yy6:
		++i;
		yych = i < end ? *i : 0;
	yy7:
		switch (yych)
		{
		case '.': goto yy3;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy6;
		case 'E':
		case 'e': goto yy8;
		default: goto yy1;
		}
	yy8:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 0x00: goto yy4;
		case '+':
		case '-': goto yy11;
		default: goto yy12;
		}
	yy9:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f': goto yy13;
		default: goto yy10;
		}
	yy10:
		i = marker;
		goto yy1;
	yy11:
		++i;
		yych = i < end ? *i : 0;
	yy12:
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy11;
		default: goto yy4;
		}
	yy13:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '.': goto yy15;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f': goto yy13;
		case 'P':
		case 'p': goto yy16;
		default: goto yy14;
		}
	yy14:
		{ return i; }
	yy15:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f': goto yy15;
		case 'P':
		case 'p': goto yy16;
		default: goto yy14;
		}
	yy16:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 0x00: goto yy14;
		case '+':
		case '-': goto yy17;
		default: goto yy18;
		}
	yy17:
		++i;
		yych = i < end ? *i : 0;
	yy18:
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f': goto yy17;
		default: goto yy14;
		}
	}

}


//
//	isLuaStylePunctuation
//	[]{}!%#^&*()-+=~|<>?:/;,.
//

static bool isLuaStylePunctuation(ImWchar character)
{
	static bool punctuation[128] = {
		false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
		false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
		false,  true, false,  true, false,  true,  true, false,  true,  true,  true,  true,  true,  true,  true,  true,
		false, false, false, false, false, false, false, false, false, false,  true,  true,  true,  true,  true,  true,
		false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
		false, false, false, false, false, false, false, false, false, false, false,  true, false,  true,  true, false,
		false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
		false, false, false, false, false, false, false, false, false, false, false,  true,  true,  true,  true, false,
	};

	return character < 127 ? punctuation[character] : false;
}


//
//	TextEditor::Language::Lua
//

const TextEditor::Language* TextEditor::Language::Lua()
{
	static bool initialized = false;
	static TextEditor::Language language;

	if (!initialized)
	{
		language.name = "Lua";
		language.singleLineComment = "--";
		language.commentStart = "--[[";
		language.commentEnd = "]]";   // Lua block comments close on ]] (not --]])
		language.hasSingleQuotedStrings = true;
		language.hasDoubleQuotedStrings = true;
		language.otherStringStart = "[[";
		language.otherStringEnd = "]]";
		language.stringEscape = '\\';

		static const char* const keywords[] = {
			"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local", "nil",
			"not", "or", "repeat", "return", "then", "true", "until", "while"
		};

		for (auto& keyword : keywords) { language.keywords.insert(keyword); }

		language.isPunctuation = isLuaStylePunctuation;
		language.getIdentifier = getCStyleIdentifier;
		language.getNumber = getLuaStyleNumber;
		initialized = true;
	}

	return &language;
}


//
//	getPythonStyleNumber
//

static TextEditor::Iterator getPythonStyleNumber(TextEditor::Iterator start, TextEditor::Iterator end)
{
	TextEditor::Iterator i = start;
	TextEditor::Iterator marker;


	{
		ImWchar yych;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0': goto yy2;
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy4;
		default:
			if (i >= end) goto yy18;
			goto yy1;
		}
	yy1:
		++i;
		{ return start; }
	yy2:
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 0x00: goto yy3;
		case 'B':
		case 'b': goto yy9;
		case 'O':
		case 'o': goto yy11;
		case 'X':
		case 'x': goto yy12;
		default: goto yy5;
		}
	yy3:
		{
			return i;
		}
	yy4:
		++i;
		marker = i;
		yych = i < end ? *i : 0;
	yy5:
		switch (yych)
		{
		case '+':
		case '-':
		case 'E':
		case 'e': goto yy6;
		case '.': goto yy8;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '_': goto yy4;
		case 'J':
		case 'j': goto yy10;
		default: goto yy3;
		}
	yy6:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy13;
		default: goto yy7;
		}
	yy7:
		i = marker;
		goto yy3;
	yy8:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy14;
		default: goto yy7;
		}
	yy9:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '_': goto yy15;
		default: goto yy7;
		}
	yy10:
		++i;
		goto yy3;
	yy11:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '_': goto yy16;
		default: goto yy7;
		}
	yy12:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case '_':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f': goto yy17;
		default: goto yy7;
		}
	yy13:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '_': goto yy13;
		case 'J':
		case 'j': goto yy10;
		default: goto yy3;
		}
	yy14:
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '+':
		case '-':
		case 'E':
		case 'e': goto yy6;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '_': goto yy14;
		case 'J':
		case 'j': goto yy10;
		default: goto yy3;
		}
	yy15:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '_': goto yy15;
		default: goto yy3;
		}
	yy16:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '_': goto yy16;
		default: goto yy3;
		}
	yy17:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case '_':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f': goto yy17;
		default: goto yy3;
		}
	yy18:
		{ return start; }
	}

}


//
//	TextEditor::Language::Python
//

const TextEditor::Language* TextEditor::Language::Python()
{
	static bool initialized = false;
	static TextEditor::Language language;

	if (!initialized)
	{
		language.name = "Python";
		language.singleLineComment = "#";
		language.hasSingleQuotedStrings = true;
		language.hasDoubleQuotedStrings = true;
		language.otherStringStart = "\"\"\"";
		language.otherStringEnd = "\"\"\"";
		language.otherStringAltStart = "'''";
		language.otherStringAltEnd = "'''";
		language.stringEscape = '\\';

		static const char* const keywords[] = {
			"False", "await", "else", "import", "pass", "None", "break", "except", "in", "raise", "True",
			"class", "finally", "is", "return", "and", "continue", "for", "lambda", "try", "as", "def",
			"from", "nonlocal", "while", "assert", "del", "global", "not", "with", "async", "elif",
			"if", "or", "yield"
		};

		for (auto& keyword : keywords) { language.keywords.insert(keyword); }

		language.isPunctuation = isCStylePunctuation;
		language.getIdentifier = getCStyleIdentifier;
		language.getNumber = getPythonStyleNumber;
		initialized = true;
	}

	return &language;
}


//
//	TextEditor::Language::Glsl
//

const TextEditor::Language* TextEditor::Language::Glsl()
{
	static bool initialized = false;
	static TextEditor::Language language;

	if (!initialized)
	{
		language.name = "GLSL";
		language.preprocess = '#';
		language.singleLineComment = "//";
		language.commentStart = "/*";
		language.commentEnd = "*/";
		language.hasSingleQuotedStrings = true;
		language.hasDoubleQuotedStrings = true;
		language.stringEscape = '\\';

		// source: https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html
		static const char* const keywords[] = {
			"atomic_uint", "attribute", "bool", "break", "buffer", "bvec2", "bvec3", "bvec4", "case", "centroid",
			"coherent", "const", "continue", "default", "discard", "dmat2", "dmat2x2", "dmat2x3", "dmat2x4", "dmat3",
			"dmat3x2", "dmat3x3", "dmat3x4", "dmat4", "dmat4x2", "dmat4x3", "dmat4x4", "do", "double", "dvec2", "dvec3",
			"dvec4", "else", "false", "flat", "float", "for", "highp", "if", "iimage1D", "iimage1DArray", "iimage2D",
			"iimage2DArray", "iimage2DMS", "iimage2DMSArray", "iimage2DRect", "iimage3D", "iimageBuffer", "iimageCube",
			"iimageCubeArray", "image1D", "image1DArray", "image2D", "image2DArray", "image2DMS", "image2DMSArray",
			"image2DRect", "image3D", "imageBuffer", "imageCube", "imageCubeArray", "in", "inout", "int", "invariant",
			"isampler1D", "isampler1DArray", "isampler2D", "isampler2DArray", "isampler2DMS", "isampler2DMSArray",
			"isampler2DRect", "isampler3D", "isamplerBuffer", "isamplerCube", "isamplerCubeArray", "ivec2", "ivec3",
			"ivec4", "layout", "lowp", "mat2", "mat2x2", "mat2x3", "mat2x4", "mat3", "mat3x2", "mat3x3", "mat3x4",
			"mat4", "mat4x2", "mat4x3", "mat4x4", "mediump", "noperspective", "out", "patch", "precise", "precision",
			"readonly", "restrict", "return", "sample", "sampler1D", "sampler1DArray", "sampler1DArrayShadow",
			"sampler1DShadow", "sampler2D", "sampler2DArray", "sampler2DArrayShadow", "sampler2DMS", "sampler2DMSArray",
			"sampler2DRect", "sampler2DRectShadow", "sampler2DShadow", "sampler3D", "samplerBuffer", "samplerCube",
			"samplerCubeArray", "samplerCubeArrayShadow", "samplerCubeShadow", "shared", "smooth", "struct", "subroutine",
			"switch", "true", "uimage1D", "uimage1DArray", "uimage2D", "uimage2DArray", "uimage2DMS", "uimage2DMSArray",
			"uimage2DRect", "uimage3D", "uimageBuffer", "uimageCube", "uimageCubeArray", "uint", "uniform", "usampler1D",
			"usampler1DArray", "usampler2D", "usampler2DArray", "usampler2DMS", "usampler2DMSArray", "usampler2DRect",
			"usampler3D", "usamplerBuffer", "usamplerCube", "usamplerCubeArray", "uvec2", "uvec3", "uvec4", "varying",
			"vec2", "vec3", "vec4", "void", "volatile", "while", "writeonly"
		};

		for (auto& keyword : keywords) { language.keywords.insert(keyword); }

		language.isPunctuation = isCStylePunctuation;
		language.getIdentifier = getCStyleIdentifier;
		language.getNumber = getCStyleNumber;
		initialized = true;
	}

	return &language;
}


//
//	TextEditor::Language::Hlsl
//

const TextEditor::Language* TextEditor::Language::Hlsl()
{
	static bool initialized = false;
	static TextEditor::Language language;

	if (!initialized)
	{
		language.name = "HLSL";
		language.preprocess = '#';
		language.singleLineComment = "//";
		language.commentStart = "/*";
		language.commentEnd = "*/";
		language.hasSingleQuotedStrings = true;
		language.hasDoubleQuotedStrings = true;
		language.stringEscape = '\\';

		static const char* const keywords[] = {
			"AppendStructuredBuffer", "asm", "asm_fragment", "BlendState", "bool", "break", "Buffer",
			"ByteAddressBuffer", "case", "cbuffer", "centroid", "class", "column_major", "compile",
			"compile_fragment", "CompileShader", "const", "continue", "ComputeShader", "ConsumeStructuredBuffer",
			"default", "DepthStencilState", "DepthStencilView", "discard", "do", "double", "DomainShader", "dword",
			"else", "export", "extern", "false", "float", "for", "fxgroup", "GeometryShader", "groupshared", "half",
			"Hullshader", "if", "in", "inline", "inout", "InputPatch", "int", "interface", "line", "lineadj",
			"linear", "LineStream", "matrix", "min16float", "min10float", "min16int", "min12int", "min16uint",
			"namespace", "nointerpolation", "noperspective", "NULL", "out", "OutputPatch", "packoffset",
			"pass", "pixelfragment", "PixelShader", "point", "PointStream", "precise", "RasterizerState",
			"RenderTargetView", "return", "register", "row_major", "RWBuffer", "RWByteAddressBuffer",
			"RWStructuredBuffer", "RWTexture1D", "RWTexture1DArray", "RWTexture2D", "RWTexture2DArray",
			"RWTexture3D", "sample", "sampler", "SamplerState", "SamplerComparisonState", "shared",
			"snorm", "stateblock", "stateblock_state", "static", "string", "struct", "switch", "StructuredBuffer",
			"tbuffer", "technique", "technique10", "technique11", "texture", "Texture1D", "Texture1DArray",
			"Texture2D", "Texture2DArray", "Texture2DMS", "Texture2DMSArray", "Texture3D", "TextureCube",
			"TextureCubeArray", "true", "typedef", "triangle", "triangleadj", "TriangleStream", "uint",
			"uniform", "unorm", "unsigned", "vector", "vertexfragment", "VertexShader", "void", "volatile", "while",
			"bool1", "bool2", "bool3", "bool4", "double1", "double2", "double3", "double4", "float1", "float2",
			"float3", "float4", "int1", "int2", "int3", "int4", "in", "out", "inout", "uint1", "uint2", "uint3",
			"uint4", "dword1", "dword2", "dword3", "dword4", "half1", "half2", "half3", "half4", "float1x1",
			"float2x1", "float3x1", "float4x1", "float1x2", "float2x2", "float3x2", "float4x2",
			"float1x3", "float2x3", "float3x3", "float4x3", "float1x4", "float2x4", "float3x4", "float4x4",
			"half1x1", "half2x1", "half3x1", "half4x1", "half1x2", "half2x2", "half3x2", "half4x2",
			"half1x3", "half2x3", "half3x3", "half4x3", "half1x4", "half2x4", "half3x4", "half4x4",
		};

		for (auto& keyword : keywords) { language.keywords.insert(keyword); }

		language.isPunctuation = isCStylePunctuation;
		language.getIdentifier = getCStyleIdentifier;
		language.getNumber = getCStyleNumber;
		initialized = true;
	}

	return &language;
}


//
//	tokenizeJson
//

static TextEditor::Iterator tokenizeJson(TextEditor::Iterator start, TextEditor::Iterator end, TextEditor::Color& color)
{
	TextEditor::Iterator i = start;
	TextEditor::Iterator marker;


	{
		ImWchar yych;
		unsigned int yyaccept = 0;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case ',':
		case ':':
		case '[':
		case ']':
		case '{':
		case '}': goto yy3;
		case '-': goto yy4;
		case '0': goto yy5;
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy7;
		case 'f': goto yy8;
		case 'n': goto yy9;
		case 't': goto yy10;
		default:
			if (i >= end) goto yy24;
			goto yy1;
		}
	yy1:
		++i;
	yy2:
		{ return start; }
	yy3:
		++i;
		{
			color = TextEditor::Color::punctuation;
			return i;
		}
	yy4:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0': goto yy5;
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy7;
		default: goto yy2;
		}
	yy5:
		yyaccept = 0;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '.': goto yy11;
		case 'E':
		case 'e': goto yy13;
		default: goto yy6;
		}
	yy6:
		{
			color = TextEditor::Color::number;
			return i;
		}
	yy7:
		yyaccept = 0;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '.': goto yy11;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy7;
		case 'E':
		case 'e': goto yy13;
		default: goto yy6;
		}
	yy8:
		yyaccept = 1;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'a': goto yy14;
		default: goto yy2;
		}
	yy9:
		yyaccept = 1;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'u': goto yy15;
		default: goto yy2;
		}
	yy10:
		yyaccept = 1;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'r': goto yy16;
		default: goto yy2;
		}
	yy11:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy17;
		default: goto yy12;
		}
	yy12:
		i = marker;
		if (yyaccept == 0) goto yy6;
		else goto yy2;
	yy13:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '+':
		case '-': goto yy18;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy19;
		default: goto yy12;
		}
	yy14:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'l': goto yy20;
		default: goto yy12;
		}
	yy15:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'l': goto yy21;
		default: goto yy12;
		}
	yy16:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'u': goto yy22;
		default: goto yy12;
		}
	yy17:
		yyaccept = 0;
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy17;
		case 'E':
		case 'e': goto yy13;
		default: goto yy6;
		}
	yy18:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy19;
		default: goto yy12;
		}
	yy19:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy19;
		default: goto yy6;
		}
	yy20:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 's': goto yy22;
		default: goto yy12;
		}
	yy21:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'l': goto yy23;
		default: goto yy12;
		}
	yy22:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'e': goto yy23;
		default: goto yy12;
		}
	yy23:
		++i;
		{
			color = TextEditor::Color::identifier;
			return i;
		}
	yy24:
		{ return start; }
	}

}


//
//	TextEditor::Language::Json
//

const TextEditor::Language* TextEditor::Language::Json()
{
	static bool initialized = false;
	static TextEditor::Language language;

	if (!initialized)
	{
		language.name = "JSON";
		language.hasDoubleQuotedStrings = true;
		language.stringEscape = '\\';

		static const char* const keywords[] = {
			"false", "null", "true"
		};

		for (auto& keyword : keywords) { language.keywords.insert(keyword); }

		language.customTokenizer = tokenizeJson;
		initialized = true;
	}

	return &language;
}


//
//	tokenizeMarkdown
//

static TextEditor::Iterator tokenizeMarkdown(TextEditor::Iterator start, TextEditor::Iterator end, TextEditor::Color& color)
{
	TextEditor::Iterator i = start;
	TextEditor::Iterator marker;


	{
		ImWchar yych;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '!': goto yy3;
		case '#': goto yy4;
		case '*': goto yy6;
		case '+': goto yy7;
		case '-': goto yy8;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy10;
		case ':':
		case '|': goto yy11;
		case '<': goto yy12;
		case '[': goto yy13;
		case '`': goto yy14;
		case '~': goto yy15;
		default:
			if (i >= end) goto yy37;
			goto yy1;
		}
	yy1:
		++i;
	yy2:
		{ return start; }
	yy3:
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '[': goto yy16;
		default: goto yy2;
		}
	yy4:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '\n': goto yy5;
		default:
			if (i >= end) goto yy5;
			goto yy4;
		}
	yy5:
		{
			color = TextEditor::Color::declaration;
			return i;
		}
	yy6:
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case ' ': goto yy11;
		case '*': goto yy20;
		default:
			if (i >= end) goto yy2;
			goto yy18;
		}
	yy7:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case ' ': goto yy11;
		default: goto yy2;
		}
	yy8:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case ' ': goto yy11;
		default: goto yy9;
		}
	yy9:
		{
			color = TextEditor::Color::punctuation;
			return i;
		}
	yy10:
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '.': goto yy21;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy22;
		default: goto yy2;
		}
	yy11:
		++i;
		goto yy9;
	yy12:
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case 'G':
		case 'H':
		case 'I':
		case 'J':
		case 'K':
		case 'L':
		case 'M':
		case 'N':
		case 'O':
		case 'P':
		case 'Q':
		case 'R':
		case 'S':
		case 'T':
		case 'U':
		case 'V':
		case 'W':
		case 'X':
		case 'Y':
		case 'Z':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f':
		case 'g':
		case 'h':
		case 'i':
		case 'j':
		case 'k':
		case 'l':
		case 'm':
		case 'n':
		case 'o':
		case 'p':
		case 'q':
		case 'r':
		case 's':
		case 't':
		case 'u':
		case 'v':
		case 'w':
		case 'x':
		case 'y':
		case 'z': goto yy23;
		default: goto yy2;
		}
	yy13:
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		if (yych <= 0x00)
		{
			if (i >= end) goto yy2;
			goto yy16;
		}
		goto yy17;
	yy14:
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		if (yych <= 0x00)
		{
			if (i >= end) goto yy2;
			goto yy25;
		}
		goto yy26;
	yy15:
		++i;
		marker = i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '~': goto yy28;
		default: goto yy2;
		}
	yy16:
		++i;
		yych = i < end ? *i : 0;
	yy17:
		switch (yych)
		{
		case ']': goto yy24;
		default:
			if (i >= end) goto yy19;
			goto yy16;
		}
	yy18:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case ' ': goto yy19;
		case '*': goto yy29;
		default:
			if (i >= end) goto yy19;
			goto yy18;
		}
	yy19:
		i = marker;
		goto yy2;
	yy20:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '*': goto yy19;
		default:
			if (i >= end) goto yy19;
			goto yy30;
		}
	yy21:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case ' ': goto yy11;
		default: goto yy19;
		}
	yy22:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '.': goto yy21;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9': goto yy22;
		default: goto yy19;
		}
	yy23:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '>': goto yy31;
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case 'G':
		case 'H':
		case 'I':
		case 'J':
		case 'K':
		case 'L':
		case 'M':
		case 'N':
		case 'O':
		case 'P':
		case 'Q':
		case 'R':
		case 'S':
		case 'T':
		case 'U':
		case 'V':
		case 'W':
		case 'X':
		case 'Y':
		case 'Z':
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f':
		case 'g':
		case 'h':
		case 'i':
		case 'j':
		case 'k':
		case 'l':
		case 'm':
		case 'n':
		case 'o':
		case 'p':
		case 'q':
		case 'r':
		case 's':
		case 't':
		case 'u':
		case 'v':
		case 'w':
		case 'x':
		case 'y':
		case 'z': goto yy23;
		default: goto yy19;
		}
	yy24:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '(': goto yy32;
		default: goto yy19;
		}
	yy25:
		++i;
		yych = i < end ? *i : 0;
	yy26:
		switch (yych)
		{
		case '`': goto yy27;
		default:
			if (i >= end) goto yy19;
			goto yy25;
		}
	yy27:
		++i;
		{
			color = TextEditor::Color::string;
			return i;
		}
	yy28:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '~': goto yy19;
		default:
			if (i >= end) goto yy19;
			goto yy33;
		}
	yy29:
		++i;
		{
			color = TextEditor::Color::number;
			return i;
		}
	yy30:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '*': goto yy34;
		default:
			if (i >= end) goto yy19;
			goto yy30;
		}
	yy31:
		++i;
		{
			color = TextEditor::Color::keyword;
			return i;
		}
	yy32:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case ')': goto yy35;
		default:
			if (i >= end) goto yy19;
			goto yy32;
		}
	yy33:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '~': goto yy36;
		default:
			if (i >= end) goto yy19;
			goto yy33;
		}
	yy34:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '*': goto yy29;
		default: goto yy19;
		}
	yy35:
		++i;
		{
			color = TextEditor::Color::identifier;
			return i;
		}
	yy36:
		++i;
		yych = i < end ? *i : 0;
		switch (yych)
		{
		case '~': goto yy29;
		default: goto yy19;
		}
	yy37:
		{ return start; }
	}

}


//
//	TextEditor::Language::Markdown
//

const TextEditor::Language* TextEditor::Language::Markdown()
{
	static bool initialized = false;
	static TextEditor::Language language;

	if (!initialized)
	{
		language.name = "Markdown";
		language.commentStart = "<!--";
		language.commentEnd = "-->";

		language.customTokenizer = tokenizeMarkdown;
		initialized = true;
	}

	return &language;
}


//
//	tokenizeIni — hand-rolled (not re2c). INI has no fixed keyword set; the
//	structure is what matters:
//	  [section]     → the whole bracketed header is a declaration
//	  key = value   → the key (left of '=') is a known identifier
//	Comments (; and #) and the '=' punctuation are handled by the language's
//	singleLineComment / isPunctuation, so this only needs to catch headers
//	and keys.
//

static TextEditor::Iterator tokenizeIni(TextEditor::Iterator start, TextEditor::Iterator end, TextEditor::Color& color)
{
	TextEditor::Iterator i = start;
	if (i >= end) return start;
	ImWchar c = *i;

	// [section header] — consume through the closing ']' (or line end).
	if (c == '[')
	{
		++i;
		while (i < end && *i != ']') ++i;
		if (i < end) ++i;             // include the ']'
		color = TextEditor::Color::declaration;
		return i;
	}

	// key followed (after optional spaces) by '=' → highlight the key name.
	auto isKeyChar = [](ImWchar ch) {
		return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
		       (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-';
	};
	if (isKeyChar(c) && !(c >= '0' && c <= '9'))
	{
		TextEditor::Iterator j = i;
		while (j < end && isKeyChar(*j)) ++j;
		TextEditor::Iterator k = j;
		while (k < end && (*k == ' ' || *k == '\t')) ++k;
		if (k < end && *k == '=')
		{
			color = TextEditor::Color::knownIdentifier;
			return j;                 // colour only the key, not the spaces/'='
		}
	}

	return start;                     // nothing special — let the default path run
}


//
//	TextEditor::Language::Ini
//

const TextEditor::Language* TextEditor::Language::Ini()
{
	static bool initialized = false;
	static TextEditor::Language language;

	if (!initialized)
	{
		language.name = "INI";
		language.caseSensitive = false;
		// Both ';' and '#' are used for INI comments in the wild.
		language.singleLineComment = ";";
		language.singleLineCommentAlt = "#";
		language.hasDoubleQuotedStrings = true;
		language.customTokenizer = tokenizeIni;
		language.isPunctuation = [](ImWchar c) {
			return c == '=' || c == ':' || c == '[' || c == ']';
		};
		language.extensions = { ".ini", ".cfg", ".conf" };
		initialized = true;
	}

	return &language;
}


//
//	TextEditor::Language::Sql
//

const TextEditor::Language* TextEditor::Language::Sql()
{
	static bool initialized = false;
	static TextEditor::Language language;

	if (!initialized)
	{
		language.name = "SQL";
		language.caseSensitive = false;
		language.singleLineComment = "--";
		language.commentStart = "/*";
		language.commentEnd = "*/";
		language.hasSingleQuotedStrings = true;
		language.hasDoubleQuotedStrings = true;
		language.stringEscape = '\\';

		static const char* const keywords[] = {
			"abs", "absent", "acos", "all", "allocate", "alter", "and", "any", "any_value", "are", "array", "array_agg",
			"array_max_cardinality", "as", "asensitive", "asin", "asymmetric", "at", "atan", "atomic", "authorization",
			"avg", "begin", "begin_frame", "begin_partition", "between", "bigint", "binary", "blob", "boolean", "both",
			"btrim", "by", "call", "called", "cardinality", "cascaded", "case", "cast", "ceil", "ceiling", "char",
			"character", "character_length", "char_length", "check", "classifier", "clob", "close", "coalesce", "collate",
			"collect", "column", "commit", "condition", "connect", "constraint", "contains", "convert", "copy", "corr",
			"corresponding", "cos", "cosh", "count", "covar_pop", "covar_samp", "create", "cross", "cube", "cume_dist",
			"current", "current_catalog", "current_date", "current_default_transform_group", "current_path", "current_role",
			"current_row", "current_schema", "current_time", "current_timestamp", "current_transform_group_for_type",
			"current_user", "cursor", "cycle", "date", "day", "deallocate", "dec", "decfloat", "decimal", "declare",
			"default", "define", "delete", "dense_rank", "deref", "describe", "deterministic", "disconnect", "distinct",
			"double", "drop", "dynamic", "each", "element", "else", "empty", "end", "end-exec", "end_frame", "end_partition",
			"equals", "escape", "every", "except", "exec", "execute", "exists", "exp", "external", "extract", "false", "fetch",
			"filter", "first_value", "float", "floor", "for", "foreign", "frame_row", "free", "from", "full", "function",
			"fusion", "get", "global", "grant", "greatest", "group", "grouping", "groups", "having", "hold", "hour",
			"identity", "in", "indicator", "initial", "inner", "inout", "insensitive", "insert", "int", "integer",
			"intersect", "intersection", "interval", "into", "is", "join", "json", "json_array", "json_arrayagg",
			"json_exists", "json_object", "json_objectagg", "json_query", "json_scalar", "json_serialize", "json_table",
			"json_table_primitive", "json_value", "lag", "language", "large", "last_value", "lateral", "lead", "leading",
			"least", "left", "like", "like_regex", "limit", "listagg", "ln", "local", "localtime", "localtimestamp", "log", "log10",
			"lower", "lpad", "ltrim", "match", "matches", "match_number", "match_recognize", "max", "member", "merge", "method",
			"min", "minute", "mod", "modifies", "module", "month", "multiset", "national", "natural", "nchar", "nclob", "new",
			"no", "none", "normalize", "not", "nth_value", "ntile", "null", "nullif", "numeric", "occurrences_regex",
			"octet_length", "of", "offset", "old", "omit", "on", "one", "only", "open", "or", "order", "out", "outer", "over",
			"overlaps", "overlay", "parameter", "partition", "pattern", "per", "percent", "percentile_cont", "percentile_disc",
			"percent_rank", "period", "portion", "position", "position_regex", "power", "precedes", "precision", "prepare", "primary",
			"procedure", "ptf", "range", "rank", "reads", "real", "recursive", "ref", "references", "referencing", "regr_avgx",
			"regr_avgy", "regr_count", "regr_intercept", "regr_r2", "regr_slope", "regr_sxx", "regr_sxy", "regr_syy", "release",
			"result", "return", "returns", "revoke", "right", "rollback", "rollup", "row", "rows", "row_number", "rpad", "running",
			"savepoint", "scope", "scroll", "search", "second", "seek", "select", "sensitive", "session_user", "set", "show", "similar",
			"sin", "sinh", "skip", "smallint", "some", "specific", "specifictype", "sql", "sqlexception", "sqlstate", "sqlwarning", "sqrt",
			"start", "static", "stddev_pop", "stddev_samp", "submultiset", "subset", "substring", "substring_regex", "succeeds",
			"sum", "symmetric", "system", "system_time", "system_user", "table", "tablesample", "tan", "tanh", "then", "time",
			"timestamp", "timezone_hour", "timezone_minute", "to", "trailing", "translate", "translate_regex", "translation",
			"treat", "trigger", "trim", "trim_array", "true", "truncate", "uescape", "union", "unique", "unknown", "unnest",
			"update", "upper", "user", "using", "value", "values", "value_of", "varbinary", "varchar", "varying", "var_pop",
			"var_samp", "versioning", "when", "whenever", "where", "width_bucket", "window", "with", "within", "without", "year"
		};

		for (auto& keyword : keywords) { language.keywords.insert(keyword); }

		language.isPunctuation = isCStylePunctuation;
		language.getIdentifier = getCStyleIdentifier;
		language.getNumber = getCStyleNumber;
		initialized = true;
	}

	return &language;
}


//
//	TextEditor::Language::FromFile
//	Parse a simple key=value language definition file at runtime.
//
//	Keys: name, case_sensitive, preprocess, single_line_comment,
//	      single_line_comment_alt, comment_start, comment_end,
//	      single_quoted_strings, double_quoted_strings, string_escape,
//	      keywords, declarations, identifiers
//	Comments start with `#` or `;`.
//

const TextEditor::Language* TextEditor::Language::FromFile(const std::string& path)
{
	// Process-lifetime cache so pointers stay valid for the program duration.
	static std::unordered_map<std::string, std::unique_ptr<Language>> cache;
	auto it = cache.find(path);
	if (it != cache.end()) return it->second.get();

	std::ifstream stream(path);
	if (!stream.is_open()) return nullptr;

	auto lang = std::make_unique<Language>();
	auto trim = [](std::string s)
		{
			size_t a = s.find_first_not_of(" \t\r\n");
			size_t b = s.find_last_not_of(" \t\r\n");
			return (a == std::string::npos) ? std::string{} : s.substr(a, b - a + 1);
		};
	auto truthy = [](const std::string& v)
		{
			return v == "true" || v == "1" || v == "yes" || v == "on";
		};
	auto splitWords = [&](const std::string& v, std::unordered_set<std::string>& out)
		{
			std::string token;
			for (char c : v)
			{
				if (c == ',' || c == ' ' || c == '\t')
				{
					if (!token.empty()) { out.insert(token); token.clear(); }
				}
				else token += c;
			}
			if (!token.empty()) out.insert(token);
		};

	std::string line;
	while (std::getline(stream, line))
	{
		auto trimmed = trim(line);
		if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;
		auto eq = trimmed.find('=');
		if (eq == std::string::npos) continue;
		std::string key = trim(trimmed.substr(0, eq));
		std::string value = trim(trimmed.substr(eq + 1));

		if (key == "name")                    lang->name = value;
		else if (key == "case_sensitive")          lang->caseSensitive = truthy(value);
		else if (key == "preprocess")              lang->preprocess = value.empty() ? 0 : value[0];
		else if (key == "single_line_comment")     lang->singleLineComment = value;
		else if (key == "single_line_comment_alt") lang->singleLineCommentAlt = value;
		else if (key == "comment_start")           lang->commentStart = value;
		else if (key == "comment_end")             lang->commentEnd = value;
		else if (key == "single_quoted_strings")   lang->hasSingleQuotedStrings = truthy(value);
		else if (key == "double_quoted_strings")   lang->hasDoubleQuotedStrings = truthy(value);
		else if (key == "string_escape")           lang->stringEscape = value.empty() ? 0 : value[0];
		else if (key == "keywords")                splitWords(value, lang->keywords);
		else if (key == "declarations")            splitWords(value, lang->declarations);
		else if (key == "identifiers")             splitWords(value, lang->identifiers);
		else if (key == "extensions")
		{
			// comma/space separated list of file extensions (lowercase, with leading dot)
			std::string token;
			auto flush = [&]()
				{
					if (token.empty()) return;
					std::transform(token.begin(), token.end(), token.begin(),
								   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
					if (token.front() != '.') token = "." + token;
					lang->extensions.push_back(token);
					token.clear();
				};
			for (char c : value)
			{
				if (c == ',' || c == ' ' || c == '\t') flush();
				else token += c;
			}
			flush();
		}
	}

	lang->isPunctuation = isCStylePunctuation;
	lang->getIdentifier = getCStyleIdentifier;
	lang->getNumber = getCStyleNumber;

	Language* raw = lang.get();
	cache.emplace(path, std::move(lang));
	return raw;
}
