//	TextEditor - A syntax highlighting text editor for Dear ImGui.
//	Copyright (c) 2024-2026 Johan A. Goossens. All rights reserved.
//	Copyright (c) 2026 Logan Brunet (ImGui-IDE). All rights reserved.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


#pragma once


//
//	Include files
//

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "imgui.h"
#include <atomic>



//
//	TextEditor
//

class TextEditor {
public:
	// constructor
	TextEditor();

	//
	// Below is the public API
	// Public member functions start with an uppercase character to be consistent with Dear ImGui
	//

	// access editor options
	inline void SetTabSize(int value) {
		// this must be called before text is loaded/edited
		if (document.isEmpty() && transactions.empty()) {
			if (value > 8) value = 8;
			if (value < 1) value  =1;
			document.setTabSize(value);
		}
	}

	inline int GetTabSize() const { return document.getTabSize(); }
	inline void SetInsertSpacesOnTabs(bool value) { document.setInsertSpacesOnTabs(value); }
	inline bool IsInsertSpacesOnTabs() const { return document.isInsertSpacesOnTabs(); }
	inline void SetLineSpacing(float value) {
						if (value > 2.0f) value = 2.0f;
						if (value < 1.0f) value = 1.0f;
 lineSpacing =value; }
	inline float GetLineSpacing() const { return lineSpacing; }
	inline void SetReadOnlyEnabled(bool value) { readOnly = value; }
	inline bool IsReadOnlyEnabled() const { return readOnly; }
	inline void SetAutoIndentEnabled(bool value) { autoIndent = value; }
	inline bool IsAutoIndentEnabled() const { return autoIndent; }
	inline void SetShowWhitespacesEnabled(bool value) { showSpaces = value; showTabs = value; }
	inline bool IsShowWhitespacesEnabled() const { return showSpaces && showTabs; }
	inline void SetShowSpacesEnabled(bool value) { showSpaces = value; }
	inline bool IsShowSpacesEnabled() const { return showSpaces; }
	inline void SetShowTabsEnabled(bool value) { showTabs = value; }
	inline bool IsShowTabsEnabled() const { return showTabs; }
	inline void SetShowLineNumbersEnabled(bool value) { showLineNumbers = value; }
	inline bool IsShowLineNumbersEnabled() const { return showLineNumbers; }
	inline void SetShowScrollbarMiniMapEnabled(bool value) { showScrollbarMiniMap = value; }
	inline bool IsShowScrollbarMiniMapEnabled() const { return showScrollbarMiniMap; }
	inline void SetShowPanScrollIndicatorEnabled(bool value) { showPanScrollIndicator = value; }
	inline bool IsShowPanScrollIndicatorEnabled() const { return showPanScrollIndicator; }
	inline void SetShowMatchingBrackets(bool value) { showMatchingBrackets = value; showMatchingBracketsChanged = true; }
	inline bool IsShowingMatchingBrackets() const { return showMatchingBrackets; }
	inline void SetCompletePairedGlyphs(bool value) { completePairedGlyphs = value; }
	inline bool IsCompletingPairedGlyphs() const { return completePairedGlyphs; }
	inline void SetOverwriteEnabled(bool value) { overwrite = value; }
	inline bool IsOverwriteEnabled() const { return overwrite; }
	inline void SetMiddleMousePanMode() { panMode = true; }
	inline void SetMiddleMouseScrollMode() { panMode = false; }
	inline bool IsMiddleMousePanMode() const { return panMode; }
	inline void SetPanInverted(bool value) { panInverted = value; }
	inline bool IsPanInverted() const { return panInverted; }
	// Pan/scroll acceleration gain (0 = linear). See panScrollAccelGain.
	inline void SetPanScrollAccel(float gain) { panScrollAccelGain = (gain < 0.0f) ? 0.0f : gain; }
	inline float GetPanScrollAccel() const { return panScrollAccelGain; }
	// Word wrap. When on, long lines wrap to the view width (or wrapWidthPx if
	// > 0) at word boundaries; horizontal scrolling is disabled. Off by default
	// — the normal column-grid path is unchanged when this is false.
	inline void SetWordWrap(bool value) { wordWrap = value; }
	inline bool IsWordWrap() const { return wordWrap; }
	inline void SetWrapWidth(float px) { wrapWidthPx = px; }
	inline float GetWrapWidth() const { return wrapWidthPx; }
	inline void SetFoldingEnabled(bool enabled) { foldRanges.foldingEnabled = enabled; }
	inline bool IsFoldingEnabled() const { return foldRanges.foldingEnabled; }

	// Fold control (no-ops when folding is disabled)
	inline void FoldAll()   { foldRanges.foldAll(document); }
	inline void UnfoldAll() { foldRanges.unfoldAll(document); }
	// Toggle / force the innermost fold around the current cursor.
	inline void ToggleCurrentFold() {
		int line = cursors.getCurrent().getInteractiveEnd().line;
		foldRanges.toggleCurrent(line, document, 0);
	}
	inline void FoldCurrent()   {
		int line = cursors.getCurrent().getInteractiveEnd().line;
		foldRanges.toggleCurrent(line, document, +1);
	}
	inline void UnfoldCurrent() {
		int line = cursors.getCurrent().getInteractiveEnd().line;
		foldRanges.toggleCurrent(line, document, -1);
	}

	// Headless test accessors. setText() already colorizes + rebuilds folds with
	// no ImGui context, so these expose the results for --selftest without a GUI.
	inline size_t GetFoldCount() const { return foldRanges.size(); }
	inline int GetGlyphColorAt(int line, int column) const {
		if (line < 0 || line >= document.lineCount()) return -1;
		const Line& gl = document[line];
		size_t idx = document.getIndex(gl, column);
		if (idx >= gl.size()) return -1;
		return static_cast<int>(gl[idx].color);
	}

	// access text (using UTF-8 encoded strings)
	// (see note below on cursor and scroll manipulation after setting new text)
	inline void SetText(const std::string_view& text) { setText(text); }
	inline std::string GetText() const { return document.getText(); }
	inline std::string GetCursorText(size_t cursor) const { return getCursorText(cursor); }

	inline std::string GetLineText(int line) const {
		return (line < 0 || line > static_cast<int>(document.size())) ? "" : document.getLineText(line);
	}

	// A cursor's position as (line, UTF-8 BYTE offset on that line). The stored
	// column is a tab-expanded VISIBLE column, so this converts visible -> codepoint
	// -> byte. Used for LSP positions (clangd negotiates utf-8 = byte offsets).
	inline void GetCursorBytePosition(int& line, int& byteOffset, size_t cursor) const {
		int col = 0;
		getCursor(line, col, cursor);
		byteOffset = 0;
		if (line < 0 || line >= document.lineCount()) return;
		size_t cp = document.getIndex(document[line], col);   // visible column -> codepoint index
		std::string ln = document.getLineText(line);
		size_t b = 0;
		for (size_t i = 0; i < cp && b < ln.size(); ++i) {
			++b;
			while (b < ln.size() && (((unsigned char) ln[b]) & 0xC0) == 0x80) ++b;
		}
		byteOffset = static_cast<int>(b);
	}

	inline std::string GetSectionText(int startLine, int startColumn, int endLine, int endColumn) const {
		return document.getSectionText(
			document.normalizeCoordinate(Coordinate(startLine, startColumn)),
			document.normalizeCoordinate(Coordinate(endLine, endColumn)));
	}

	inline void ReplaceSectionText(int startLine, int startColumn, int endLine, int endColumn, const std::string_view& text) {
		return replaceSectionText(
			document.normalizeCoordinate(Coordinate(startLine, startColumn)),
			document.normalizeCoordinate(Coordinate(endLine, endColumn)),
			text
		);
	}

	inline void ClearText() { SetText(""); }

	inline bool IsEmpty() const { return document.isEmpty(); }
	inline int GetLineCount() const { return document.lineCount(); }

	// render the text editor in a Dear ImGui context
	inline void Render(const char* title, const ImVec2& size=ImVec2(), bool border=false) { render(title, size, border); }

	// programmatically set focus on the editor
	inline void SetFocus() { focusOnEditor = true; }

	// clipboard actions
	inline void Cut() { if (!readOnly) cut(); }
	inline void Copy() const { copy(); }
	inline void Paste() { if (!readOnly) paste(); }
	inline void Undo() { if (!readOnly) undo(); }
	inline void Redo() { if (!readOnly) redo(); }

	// Keybinding remap for editor-internal actions. The host app sets a chord
	// string (e.g. "Ctrl+Shift+U") for an action id; when that chord is pressed
	// and this editor has focus, the action runs instead of (or in addition to)
	// its default. Recognised ids: "undo", "redo", "cut", "copy", "paste",
	// "selectAll", "toggleComments", "foldAll", "unfoldAll", "foldCurrent",
	// "unfoldCurrent", "upperCase", "lowerCase", "indent", "deindent",
	// "addNextOccurrence". An empty chord clears the override. Defaults still
	// apply for any action without an override.
	void SetKeyChordOverride(const std::string& action, const std::string& chord);
	inline void ClearKeyChordOverrides() { keyChordOverrides.clear(); }
	inline bool CanUndo() const { return !readOnly && transactions.canUndo(); };
	inline bool CanRedo() const { return !readOnly && transactions.canRedo(); };
	inline size_t GetUndoIndex() const { return transactions.getUndoIndex(); };

	// manipulate cursors and selections (line numbers are zero-based)
	inline void SetCursor(int line, int column) { foldRanges.unfoldContaining(line, document); moveTo(document.normalizeCoordinate(Coordinate(line, column)), false); }
	inline void AddCursor(int line, int column) { cursors.addCursor(document.normalizeCoordinate(Coordinate(line, column))); }
	inline void SelectAll() { selectAll(); }
	inline void SelectLine(int line) { if (line >= 0 && line < document.lineCount()) selectLine(line); }
	inline void SelectCursorLines() { selectCursorLines(); }   // Ctrl+L for every cursor
	inline void SelectLines(int start, int end) { if (start >= 0 && end < document.lineCount() && start <= end) selectLines(start, end); }
	inline void SelectRegion(int startLine, int startColumn, int endLine, int endColumn) { selectRegion(startLine, startColumn, endLine, endColumn); }
	inline void SelectWord(int line, int column) {selectWord(line, column); }
	inline void SelectToBrackets(bool includeBrackets=true) { selectToBrackets(includeBrackets); }
	inline void GrowSelectionsToCurlyBrackets() { growSelectionsToCurlyBrackets(); }
	inline void ShrinkSelectionsToCurlyBrackets() { shrinkSelectionsToCurlyBrackets(); }
	inline void AddNextOccurrence() { addNextOccurrence(); }
	inline void SelectAllOccurrences() { selectAllOccurrences(); }
	inline bool AnyCursorHasSelection() const { return cursors.anyHasSelection(); }
	inline bool AllCursorsHaveSelection() const { return cursors.allHaveSelection(); }
	inline bool CurrentCursorHasSelection() const { return cursors.currentCursorHasSelection(); }
	inline void ClearCursors() { cursors.clearAll(); }

	// get cursor positions (the meaning of main and current is explained in README.md)
	inline size_t GetNumberOfCursors() const { return cursors.size(); }
	inline void GetCursor(int& line, int& column, size_t cursor) const { return getCursor(line, column, cursor); }
	inline void GetCursor(int& startLine, int& startColumn, int& endLine, int& endColumn, size_t cursor) const { return getCursor(startLine, startColumn, endLine, endColumn, cursor); }
	inline void GetMainCursor(int& line, int& column) const { return getCursor(line, column, cursors.getMainIndex()); }
	inline void GetCurrentCursor(int& line, int& column) const { return getCursor(line, column, cursors.getCurrentIndex()); }

	// alternative API for cursor and selection position using lightweight out struct (line and column are zero-based)
	struct CursorPosition {
		CursorPosition() = default;
		CursorPosition(int l, int c) : line(l), column(c) {}
		int line = 0;
		int column = 0;
	};

	struct CursorSelection {
		CursorSelection() = default;
		CursorSelection(CursorPosition s, CursorPosition e) : start(s), end(e) {}
		CursorPosition start;
		CursorPosition end;
	};

	inline CursorPosition GetMainCursorPosition() const { CursorPosition p; getCursor(p.line, p.column, cursors.getMainIndex()); return p; }
	inline CursorPosition GetCurrentCursorPosition() const { CursorPosition p; getCursor(p.line, p.column, cursors.getCurrentIndex()); return p; }
	// Text of whatever the user has highlighted under the current cursor.
	// Empty when there's no selection.
	inline std::string GetCurrentSelectionText() const { return getCursorText(cursors.getCurrentIndex()); }
	inline CursorPosition GetCursorPosition(size_t cursor) const { CursorPosition p; getCursor(p.line, p.column, cursor); return p; }
	inline CursorSelection GetCursorSelection(size_t cursor) const { CursorSelection s; getCursor(s.start.line, s.start.column, s.end.line, s.end.column, cursor); return s; }
	inline CursorSelection GetMainCursorSelection() const { return GetCursorSelection(cursors.getMainIndex()); }

	// get the word at a screen position
	std::string GetWordAtScreenPos(const ImVec2& screenPos) const;
	// Map a screen position to (line, UTF-8 byte offset) for LSP requests (hover).
	// Returns false if the position isn't over a valid line.
	bool GetBytePosAtScreenPos(const ImVec2& screenPos, int& line, int& byteOffset) const;
	// Extract the word at a (line, column) text coordinate. Used by the
	// right-click context-menu callback, which receives the click position in
	// text coords — screenPos-based lookup gets stale by the time the menu's
	// MenuItem fires (mouse has moved onto the menu).
	inline std::string GetWordAt(int line, int column) const {
		auto c = document.normalizeCoordinate(Coordinate(line, column));
		auto s = document.findWordStart(c, true);
		auto e = document.findWordEnd(c, true);
		return document.getSectionText(s, e);
	}

	// Qualified name at a (line, column): the word, extended across `.` and
	// `::` separators (and `->`) so `System.Diagnostics.Process` /
	// `std::vector` come back whole instead of a single segment. Used for
	// namespaced go-to-definition.
	std::string GetQualifiedWordAt(int line, int column) const {
		std::string ln = GetLineText(line);
		if (ln.empty()) return {};
		int n = static_cast<int>(ln.size());
		if (column < 0) column = 0;
		if (column > n) column = n;
		auto isIdent = [](char c) {
			return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			       (c >= '0' && c <= '9') || c == '_';
		};
		// A separator is `.`, or `::`, or `->` joining two identifier chars.
		auto sepLenAt = [&](int i) -> int {       // length of separator starting at i, else 0
			if (i < 0 || i >= n) return 0;
			if (ln[i] == '.') return 1;
			if (ln[i] == ':' && i + 1 < n && ln[i + 1] == ':') return 2;
			if (ln[i] == '-' && i + 1 < n && ln[i + 1] == '>') return 2;
			return 0;
		};
		// Expand left.
		int start = column;
		while (start > 0) {
			if (isIdent(ln[start - 1])) { --start; continue; }
			int s2 = sepLenAt(start - 2 >= 0 && ln[start-1]==':' ? start - 2 : start - 1);
			// step back over a separator only if an identifier char precedes it
			int sl = (ln[start-1]=='.') ? 1
			       : (start-2>=0 && ln[start-1]==':' && ln[start-2]==':') ? 2
			       : (start-2>=0 && ln[start-1]=='>' && ln[start-2]=='-') ? 2 : 0;
			(void)s2;
			if (sl > 0 && start - sl - 1 >= 0 && isIdent(ln[start - sl - 1])) { start -= sl; continue; }
			break;
		}
		// Expand right.
		int end = column;
		while (end < n) {
			if (isIdent(ln[end])) { ++end; continue; }
			int sl = sepLenAt(end);
			if (sl > 0 && end + sl < n && isIdent(ln[end + sl])) { end += sl; continue; }
			break;
		}
		if (end <= start) return GetWordAt(line, column);
		return ln.substr(start, end - start);
	}


	// scrolling support
	enum class Scroll {
		alignTop,
		alignMiddle,
		alignBottom
	};

	inline void ScrollToLine(int line, Scroll alignment) { foldRanges.unfoldContaining(line, document); scrollToLine(line, alignment); }
	inline int GetFirstVisibleLine() const { return firstVisibleLine; }
	inline int GetLastVisibleLine() const { return lastVisibleLine; }
	inline int GetFirstVisibleColumn() const { return firstVisibleColumn; }
	inline int GetLastVisibleColumn() const { return lastVisibleColumn; }

	inline float GetLineHeight() const { return glyphSize.y; }
	inline float GetGlyphWidth() const { return glyphSize.x; }

	// note on setting cursor and scrolling
	//
	// calling SetCursor or ScrollToLine has no effect until the next call to Render
	// this is because we can only do layout calculations when we are in a Dear ImGui drawing context
	// as a result, SetCursor or ScrollToLine just mark the request and let Render execute it
	//
	// the order of the calls is therefore important as they can interfere with each other
	// so if you call SetText, SetCursor and/or ScrollToLine before Render, the order should be:
	//
	// * call SetText first as it resets the entire editor state including cursors and scrolling
	// * then call SetCursor as it sets the cursor and requests that we make the cursor visible (i.e. scroll to it)
	// * then call ScrollToLine to mark the exact scroll location (it cancels the possible SetCursor scroll request)
	// * call Render to properly update the entire state
	//
	// this works on opening the editor as well as later

	// find/replace support
	inline void GoToFirstOccurrenceOf(const std::string_view& text, bool caseSensitive = true, bool select = false) { goToFirstOccurrenceOf(text, caseSensitive, select); }
	// Heuristic "Go to Definition": scans all whole-word matches of `text`
	// and prefers ones that look like a function/type body (line contains
	// `{` after the match, or for Python, the line ends with `:`). Falls
	// back to first occurrence if no better match exists.
	inline void GoToDefinitionOf(const std::string_view& text, bool caseSensitive = true) { goToDefinitionOf(text, caseSensitive); }

	inline void SelectFirstOccurrenceOf(const std::string_view& text, bool caseSensitive=true, bool wholeWord=false) { selectFirstOccurrenceOf(text, caseSensitive, wholeWord); }
	inline void SelectNextOccurrenceOf(const std::string_view& text, bool caseSensitive=true, bool wholeWord=false) { selectNextOccurrenceOf(text, caseSensitive, wholeWord); }
	inline void SelectAllOccurrencesOf(const std::string_view& text, bool caseSensitive=true, bool wholeWord=false) { selectAllOccurrencesOf(text, caseSensitive, wholeWord); }
	inline void ReplaceTextInCurrentCursor(const std::string_view& text) { if (!readOnly) replaceTextInCurrentCursor(text); }


	inline void ReplaceTextInAllCursors(const std::string_view& text) { if (!readOnly) replaceTextInAllCursors(text); }

	inline void OpenFindReplaceWindow() { openFindReplace(); }
	inline void CloseFindReplaceWindow() { closeFindReplace(); }
	inline void SetFindButtonLabel(const std::string_view& label) { findButtonLabel = label; }
	inline void SetFindAllButtonLabel(const std::string_view& label) { findAllButtonLabel = label; }
	inline void SetReplaceButtonLabel(const std::string_view& label) { replaceButtonLabel = label; }
	inline void SetReplaceAllButtonLabel(const std::string_view& label) { replaceAllButtonLabel = label; }
	inline bool HasFindString() const { return findText.size(); }
	inline void FindNext() { findNext(); }
	inline void FindAll() { findAll(); }

	// access markers (line numbers are zero-based)
	inline void AddMarker(int line, ImU32 lineNumberColor, ImU32 textColor, const std::string_view& lineNumberTooltip, const std::string_view& textTooltip) { addMarker(line, lineNumberColor, textColor, lineNumberTooltip, textTooltip); }
	inline void ClearMarkers() { clearMarkers(); }
	inline bool HasMarkers() const { return markers.size() != 0; }

	// specify a change callback (called when changes are made (including undo/redo))
	// the delay parameter specifies a time in miliseconds that the editor will wait for before calling
	// which helps in case you don't need to track every keystroke
	// passing nullptr deactivates the callback
	inline void SetChangeCallback(std::function<void()> callback, int delay=0) {
		delayedChangeCallback = callback;
		delayedChangeDelay = std::chrono::milliseconds{delay};
	}

	// detailed change report passed to callback below
	// this callback is different from the one above as is reports every change (not just a summary) and is very detailed
	// the insert flag states whether the change was an insert (true) or a delete (false)
	// in case of an overwrite, there will be two actions (first a delete and then an insert)
	// the start parameters refer to the insert point or the start of the delete
	// the end parameters refer to the end of the inserted text or the end of the deleted text
	// the text parameter contains the inserted or deleted text
	// line, column and index values are zero-based
	struct Change {
		bool insert;
		int startLine;
		int startColumn;
		int startIndex;
		int endLine;
		int endColumn;
		int endIndex;
		std::string text;
	};

	// specify a transaction callback (live document changes in great detail)
	// it provides a list of changes made to the document in a single transaction (in the right order)
	// be carefull with this callback as it gets very verbose (called on every keystroke, delete, cut, paste, undo and redo)
	// passing nullptr deactivates the callback
	inline void SetTransactionCallback(std::function<void(std::vector<Change>&)> callback) { transactionCallback = callback; }

	// line-based callbacks (line numbers are zero-based)
	// insertor callback is called when for each line inserted and the result is used as the new line specific user data
	// deletor callback is called for each line deleted (line specific user data is passed to callback)
	// setting either callback to nullptr will deactivate that callback
	inline void SetInsertor(std::function<void*(int line)> callback) { document.setInsertor(callback); }
	inline void SetDeletor(std::function<void(int line, void* data)> callback) { document.setDeletor(callback); }

	// line-based user data (line numbers are zero-based)
	// allowing integrators to associate external data with select lines or all lines
	// user data is an opaque void* that must be managed externally
	// user data is also passed to the decorator callback (see below)
	// user data is attached to a line and additions/insertions/deletions don't effect this
	// if a line with user data is removed, it won't come back on a redo
	// the deletor callback (if specified) is called when a line is deleted (see above)
	inline void SetUserData(int line, void* data) { document.setUserData(line, data); }
	inline void* GetUserData(int line) const { return document.getUserData(line); }
	inline void IterateUserData(std::function<void(int line, void* data)> callback) const { document.iterateUserData(callback); }

	// line-based decoration
	struct Decorator {
		int line; // zero-based
		float width;
		float height;
		ImVec2 glyphSize;
		void* userData;
		const TextEditor* editor;
		int realLine;
	};

	// positive width is number of pixels, negative with is number of glyphs
	inline void SetLineDecorator(float width, std::function<void(Decorator& decorator)> callback) {
		decoratorWidth = width;
		decoratorCallback = callback;
	}

	inline void ClearLineDecorator() { SetLineDecorator(0.0f, nullptr); }
	inline bool HasLineDecorator() const { return decoratorWidth != 0.0f && decoratorCallback != nullptr; }

	// setup context menu callbacks (these are called when a user right clicks line numbers or somewhere in the text)
	// the editor sets up the popup menus, the callback has to populate them
	inline void SetLineNumberContextMenuCallback(std::function<void(int line)> callback) { lineNumberContextMenuCallback = callback; }
	inline void ClearLineNumberContextMenuCallback() { SetLineNumberContextMenuCallback(nullptr); }
	inline bool HasLineNumberContextMenuCallback() const { return lineNumberContextMenuCallback != nullptr; }

	inline void SetTextContextMenuCallback(std::function<void(int line, int column)> callback) { textContextMenuCallback = callback; }
	inline void ClearTextContextMenuCallback() { SetTextContextMenuCallback(nullptr); }
	inline bool HasTextContextMenuCallback() const { return textContextMenuCallback != nullptr; }

	// useful functions to work on selections
	// NOTE: functions provided to FilterSelections or FilterLines should accept and return UTF-8 encoded strings
	inline void IndentLines() { if (!readOnly) indentLines(); }
	inline void DeindentLines() { if (!readOnly) deindentLines(); }
	inline void MoveUpLines() { if (!readOnly) moveUpLines(); }
	inline void MoveDownLines() { if (!readOnly) moveDownLines(); }
	inline void ToggleComments() { if (!readOnly && language) toggleComments(); }

	inline void ToggleCommentsShift() { if (!readOnly && language) toggleCommentsShift(); }
	inline void FilterSelections(std::function<std::string(std::string_view)> filter) { if (!readOnly) filterSelections(filter); }
	inline void SelectionToLowerCase() { if (!readOnly) selectionToLowerCase(); }
	inline void SelectionToUpperCase() { if (!readOnly) selectionToUpperCase(); }

	// useful functions to work on entire text
	inline void StripTrailingWhitespaces() { if (!readOnly) stripTrailingWhitespaces(); }
	inline void FilterLines(std::function<std::string(std::string_view)> filter) { if (!readOnly) filterLines(filter); }
	inline void TabsToSpaces() { if (!readOnly) tabsToSpaces(); }
	inline void SpacesToTabs() { if (!readOnly) spacesToTabs(); }

	// color palette support
	enum class Color : char {
		text,
		keyword,
		declaration,
		number,
		string,
		punctuation,
		preprocessor,
		identifier,
		knownIdentifier,
		comment,
		background,
		cursor,
		selection,
		whitespace,
		matchingBracketBackground,
		matchingBracketActive,
		matchingBracketLevel1,
		matchingBracketLevel2,
		matchingBracketLevel3,
		matchingBracketError,
		lineNumber,
		currentLineNumber,
		count
	};

	class Palette : public std::array<ImU32, static_cast<size_t>(Color::count)> {
	public:
		inline ImU32 get(Color color) const { return at(static_cast<size_t>(color)); }
	};

	inline void SetPalette(const Palette& newPalette) { paletteBase = newPalette; paletteAlpha = -1.0f; }
	inline const Palette& GetPalette() const { return paletteBase; }
	static inline void SetDefaultPalette(const Palette& aValue) { defaultPalette = aValue; }
	static inline Palette& GetDefaultPalette() { return defaultPalette; }

	static const Palette& GetDarkPalette();
	static const Palette& GetLightPalette();

	// a single colored character (a glyph)
	class Glyph {
	public:
		// constructors
		Glyph() = default;
		Glyph(ImWchar cp) : codepoint(cp) {}
		Glyph(ImWchar cp, Color col) : codepoint(cp), color(col) {}

		// properties
		ImWchar codepoint = 0;
		Color color = Color::text;
	};

	// iterator used in language-specific tokenizers
	// this iterator points to unicode codepoints
	class Iterator {
	public:
		// constructors
		Iterator() = default;
		Iterator(Glyph* g) : glyph(g) {}

		using iterator_category = std::forward_iterator_tag;
		using difference_type = std::ptrdiff_t;
		using value_type = ImWchar;
		using pointer = ImWchar*;
		using reference = ImWchar&;

		inline reference operator*() const { return glyph->codepoint; }
		inline pointer operator->() const { return &(glyph->codepoint); }
		inline Iterator& operator++() { glyph++; return *this; }
		inline Iterator operator++(int) { Iterator tmp = *this; glyph++; return tmp; }
		inline size_t operator-(const Iterator& a) { return glyph - a.glyph; }
		inline friend bool operator==(const Iterator& a, const Iterator& b) { return a.glyph == b.glyph; };
		inline friend bool operator!=(const Iterator& a, const Iterator& b) { return !(a.glyph == b.glyph); };
		inline friend bool operator<(const Iterator& a, const Iterator& b) { return a.glyph < b.glyph; };
		inline friend bool operator<=(const Iterator& a, const Iterator& b) { return a.glyph <= b.glyph; };
		inline friend bool operator>(const Iterator& a, const Iterator& b) { return a.glyph > b.glyph; };
		inline friend bool operator>=(const Iterator& a, const Iterator& b) { return a.glyph >= b.glyph; };

	private:
		// properties
		Glyph* glyph;
	};

	// language support
	class Language {
	public:
		// name of the language
		std::string name;

		// flag to describe if keywords and identifiers are case sensitive (which is the default)
		bool caseSensitive = true;

		// the character that starts a preprocessor directive (can be 0 if language doesn't have this feature)
		ImWchar preprocess = 0;

		// a character sequence that start a single line comment (can be blank if language doesn't have this feature)
		std::string singleLineComment;

		// an alternate single line comment character sequence (can be blank if language doesn't have this feature)
		std::string singleLineCommentAlt;

		// the start and end character sequence for multiline comments (can be blank language doesn't have this feature)
		std::string commentStart;
		std::string commentEnd;

		// flags specifying whether language supports single quoted ['] and/or double quoted [""] strings
		bool hasSingleQuotedStrings = false;
		bool hasDoubleQuotedStrings = false;

		// other character sequences that start and end strings (can be blank if language doesn't have this feature)
		std::string otherStringStart;
		std::string otherStringEnd;

		// alternate character sequences that start and end strings (can be blank if language doesn't have this feature)
		std::string otherStringAltStart;
		std::string otherStringAltEnd;

		// character inside string used to escape the next character (can be 0 if language doesn't have this feature)
		ImWchar stringEscape = 0;

		// set of keywords, declarations, identifiers used in the language (can be blank if language doesn't have these features)
		// if language is not case sensitive, all entries should be in lower case
		std::unordered_set<std::string> keywords;
		std::unordered_set<std::string> declarations;
		std::unordered_set<std::string> identifiers;

		// File extensions this language applies to (lowercase, leading dot, e.g. ".html").
		// Populated by FromFile when an `extensions=` line is present.
		std::vector<std::string> extensions;

		// function to determine if specified character in considered punctuation
		std::function<bool(ImWchar)> isPunctuation;

		// functions to tokenize identifiers and numbers (can be nullptr if language doesn't have this feature)
		// start and end refer to the characters being tokenized
		// functions should return an iterator to the character after the detected token
		// returning start means no token was found
		std::function<Iterator(Iterator start, Iterator end)> getIdentifier;
		std::function<Iterator(Iterator start, Iterator end)> getNumber;

		// function to implement custom tokenizer
		// if a token is found, function should return an iterator to the character after the token and set the color
		std::function<Iterator(Iterator start, Iterator end, Color& color)> customTokenizer;

		// predefined language definitions
		static const Language* C();
		static const Language* Cpp();
		static const Language* Cs();
		static const Language* AngelScript();
		static const Language* Lua();
		static const Language* Python();
		static const Language* Glsl();
		static const Language* Hlsl();
		static const Language* Json();
		static const Language* Markdown();
		static const Language* Sql();
		static const Language* Ini();

		// Load a language definition from a simple key=value text file. Lines are
		//   key = value
		// Recognised keys: name, case_sensitive (true/false), preprocess,
		//   single_line_comment, single_line_comment_alt, comment_start, comment_end,
		//   single_quoted_strings (true/false), double_quoted_strings (true/false),
		//   string_escape (single char), keywords (comma/space separated),
		//   declarations, identifiers.
		// Lines starting with `#` or `;` are comments.
		static const Language* FromFile(const std::string& path);
	};

	inline void SetLanguage(const Language* l) { language = l; languageChanged = true; }
	inline const Language* GetLanguage() const { return language; };
	inline bool HasLanguage() const { return language != nullptr; }
	inline std::string GetLanguageName() const { return language == nullptr ? "None" : language->name; }

	// iterate through identifiers detected by the colorizer (based on current language)
	inline void IterateIdentifiers(std::function<void(const std::string& identifier)> callback) const { document.iterateIdentifiers(callback); }

	// autocomplete state (acts as API between editor and outer application)
	class AutoCompleteState {
	public:
		// current context (strings = UTF-8, columns = Nth visible column and indices = Nth codepoint)
		// to understand the difference between column and index, think like a tab :-)
		std::string searchTerm;
		size_t line;
		size_t searchTermStartColumn;
		size_t searchTermStartIndex;
		size_t searchTermEndColumn;
		size_t searchTermEndIndex;

		bool inIdentifier;
		bool inNumber;
		bool inComment;
		bool inString;

		// currently selected language (could be nullptr if no language is selected)
		const Language* language;

		// optional opaque void* provided by app when autocomplete was setup
		void* userData;

		// auto complete suggestions te be provided by app callback (the app is responsible for sorting)

		// the editor does not automatically include language specific keywords or identifiers in the suggestion list
		// this is left to the application so it can be context specific in case a language server is used
		// a pointer to the current language definition is provided so callbacks have easy access
		std::vector<std::string> suggestions;

		// set this to true if you are building the suggestion list asynchronously and provide it later
		// this way autocomplete is not cancelled if the suggestion list is empty and the user hits tab or enter
		bool suggestionsPromise = false;
	};

	// autocomplete configuration (defaults are like Visual Studio Code)
	class AutoCompleteConfig {
	public:
		// specifies whether typing by the user triggers autocomplete
		bool triggerOnTyping = true;

		// specifies whether the specified shortcut triggers autocomplete
		bool triggerOnShortcut = true;

		// specifies whether typing (or shortcut) in comments or strings triggers autocomplete
		bool triggerInComments = false;
		bool triggerInStrings = false;

		// manual trigger key sequence (default is Ctrl+space on all platforms, even MacOS)
		// remember Dear ImGui reverses Ctrl and Command on MacOS
#if __APPLE__
		ImGuiKeyChord triggerShortcut = ImGuiMod_Super | ImGuiKey_Space;
#else
		ImGuiKeyChord triggerShortcut = ImGuiMod_Ctrl | ImGuiKey_Space;
#endif

		// see if single suggestions are automatically inserted
		// this only works when triggered manually
		bool autoInsertSingleSuggestions = false;

		// delay in milliseconds between autocomplete trigger and suggestions popup
		std::chrono::milliseconds triggerDelay{200};

		// text label used when no suggestions are available (this allows for internationalization)
		std::string noSuggestionsLabel = "No suggestions";

		// called when autocomplete is configured, active and the editor needs an updated suggestions list
		// callback must populate and order suggestions in state object
		// suggestion list is not cleared by editor between callbacks
		// callback is called during the rendering loop (so don't take too long)

		// if it takes too long, applications should do search in separate thread and
		// use API to report results (see SetAutoCompleteSuggestions)
		// callback should set suggestionsPromise to true in this case
		std::function<void(AutoCompleteState&)> callback;

		// opaque void* that must be managed externally but passed to callback
		void* userData = nullptr;
	};

	// configure and activate autocomplete (passing nullptr deactivates it)
	inline void SetAutoCompleteConfig(const AutoCompleteConfig* config) { autocomplete.setConfig(config); }

	// provide autocomplete suggestions asynchronously (in case a callback takes to long and lookup is handled in a separate thread/process)
	// this call is not threadsafe and must be called from the rendering thread (you must synchronize with your lookup thread yourself)
	inline void SetAutoCompleteSuggestions(const std::vector<std::string>& suggestions) { autocomplete.setSuggestions(suggestions); }

	// utility class to support some autocomplete implementations
	// this is not used by default but can be used in autocomplete callbacks (see example app)
	class Trie {
	public:
		// constructor
		Trie() { clear(); }

		// clear word tree
		inline void clear() { root = std::make_unique<Node>(); }

		// insert word (UTF-8 encoded) into tree
		void insert(const std::string_view& word);

		// Returns true if the exact word is stored in the trie. Used to gate
		// "Go to definition / Select All Occurrences" so they only show for
		// known identifiers/keywords/declarations, not random text.
		bool contains(const std::string_view& word) const;

		// populate list of suggestions based on provided search term (which is UTF-8 encoded)
		// limit is maximum number of suggestions returned after they are sorted by relevance
		// maxSkippedLetters is a the largest number of letters that can be skipped to find the next match
		// this allows for missing letters (out of order letters are not taken into account)
		void findSuggestions(std::vector<std::string>& suggestions, const std::string_view& searchTerm, size_t limit=20, size_t maxSkippedLetters=2);

	private:
		// definition of single node in the word graph
		struct Node {
			std::unordered_map<ImWchar, std::unique_ptr<Node>> children;
			std::string word;
		};

		// the root node
		std::unique_ptr<Node> root;

		// maximum number of letters that can be skipped skip in matching algorithm
		size_t maxSkip;

		// search term as codepoint vector
		std::vector<ImWchar> searchCodepoints;

		// possible autocomplete candidates
		struct Candidate {
			Candidate(const Node* n, size_t c) : node(n), cost(c) {}
			bool operator<(const Candidate& rhs) const { return cost < rhs.cost; }
			bool operator==(const Candidate& rhs) const { return node == rhs.node; }
			const Node* node;
			size_t cost;
		};

		std::vector<Candidate> candidates;

		// utility functions
		void evaluateNode(const Node* node, size_t index, size_t cost, size_t skip);
		void addCandidates(const Node* node, size_t cost);
	};

	// support functions for unicode codepoints
	class CodePoint {
	public:
		static std::string_view::const_iterator skipBOM(std::string_view::const_iterator i, std::string_view::const_iterator end);
		static std::string_view::const_iterator read(std::string_view::const_iterator i, std::string_view::const_iterator end, ImWchar* codepoint);
		static size_t write(char* i, ImWchar codepoint); // must point to buffer of 4 characters (returns number of characters written)
		static bool isLetter(ImWchar codepoint);
		static bool isNumber(ImWchar codepoint);
		static bool isWord(ImWchar codepoint);
		static bool isWhiteSpace(ImWchar codepoint);
		static bool isXidStart(ImWchar codepoint);
		static bool isXidContinue(ImWchar codepoint);
		static bool isLower(ImWchar codepoint);
		static bool isUpper(ImWchar codepoint);
		static ImWchar toUpper(ImWchar codepoint);
		static ImWchar toLower(ImWchar codepoint);

		static constexpr ImWchar singleQuote = '\'';
		static constexpr ImWchar doubleQuote = '"';
		static constexpr ImWchar openCurlyBracket = '{';
		static constexpr ImWchar closeCurlyBracket = '}';
		static constexpr ImWchar openSquareBracket = '[';
		static constexpr ImWchar closeSquareBracket = ']';
		static constexpr ImWchar openParenthesis = '(';
		static constexpr ImWchar closeParenthesis = ')';

		static inline bool isUpperToLower(ImWchar left, ImWchar right) {
			return
				isUpper(left) && isLower(right);
		}


		static inline bool isLowerToUpper(ImWchar left, ImWchar right) {
			return
				isUpper(right) && isLower(left);
		}


		static inline bool isSeparator(ImWchar ch) {
			return ch == '_';
		}


		static inline bool isPairOpener(ImWchar ch) {
			return
				ch == openCurlyBracket ||
				ch == openSquareBracket ||
				ch == openParenthesis ||
				ch == singleQuote ||
				ch == doubleQuote;
		}

		static inline bool isPairCloser(ImWchar ch) {
			return
				ch == closeCurlyBracket ||
				ch == closeSquareBracket ||
				ch == closeParenthesis ||
				ch == singleQuote ||
				ch == doubleQuote;
		}

		static inline ImWchar toPairCloser(ImWchar ch) {
			return
				(ch == openCurlyBracket) ? closeCurlyBracket :
				(ch == openSquareBracket) ? closeSquareBracket :
				(ch == openParenthesis) ? closeParenthesis:
				ch;
		}

		static inline ImWchar toPairOpener(ImWchar ch) {
			return
				(ch == closeCurlyBracket) ? openCurlyBracket :
				(ch == closeSquareBracket) ? openSquareBracket :
				(ch == closeParenthesis) ? openParenthesis:
				ch;
		}

		static inline bool isMatchingPair(ImWchar open, ImWchar close) {
			return isPairOpener(open) && close == toPairCloser(open);
		}

		static inline bool isBracketOpener(ImWchar ch) {
			return
				ch == openCurlyBracket ||
				ch == openSquareBracket ||
				ch == openParenthesis;
		}

		static inline bool isBracketCloser(ImWchar ch) {
			return
				ch == closeCurlyBracket ||
				ch == closeSquareBracket ||
				ch == closeParenthesis;
		}

		static inline bool isMatchingBrackets(ImWchar open, ImWchar close) {
			return isBracketOpener(open) && close == toPairCloser(open);
		}
	};

protected:
	//
	// below is the private API
	// private members (function and variables) start with a lowercase character
	// private class names start with a lowercase character
	//

	class Coordinate {
		// represent a character coordinate from the user's point of view, i. e. consider an uniform grid
		// on the screen as it is rendered, and each cell has its own coordinate, starting from 0
		//
		// tabs are counted as [1..tabsize] count spaces, depending on how many spaces are necessary to
		// reach the next tab stop
		//
		// for example, coordinate (1, 5) represents the character 'B' in a line "\tABC", when tabsize = 4,
		// because it is rendered as "    ABC" on the screen

	public:
		Coordinate() = default;
		Coordinate(int l, int c) : line(l), column(c) {}

		inline bool operator ==(const Coordinate& o) const { return line == o.line && column == o.column; }
		inline bool operator !=(const Coordinate& o) const { return line != o.line || column != o.column; }
		inline bool operator <(const Coordinate& o) const { return line != o.line ? line < o.line : column < o.column; }
		inline bool operator >(const Coordinate& o) const { return line != o.line ? line > o.line : column > o.column; }
		inline bool operator <=(const Coordinate& o) const { return line != o.line ? line < o.line : column <= o.column; }
		inline bool operator >=(const Coordinate& o) const { return line != o.line ? line > o.line : column >= o.column; }

		inline Coordinate operator -(const Coordinate& o) const { return Coordinate(line - o.line, column - o.column); }
		inline Coordinate operator +(const Coordinate& o) const { return Coordinate(line + o.line, column + o.column); }

		static inline Coordinate invalid() { static Coordinate invalid(-1, -1); return invalid; }
		inline bool isValid() const { return line >= 0 && column >= 0; }

		int line = 0;
		int column = 0;
	};

	// a single cursor
	class Cursor {
	public:
		// constructors
		Cursor() = default;
		Cursor(Coordinate coordinate) : start(coordinate), end(coordinate) {}
		Cursor(Coordinate s, Coordinate e) : start(s), end(e) {}

		// update the cursor
		inline void update(Coordinate coordinate) { end = coordinate; updated = true; }
		inline void update(Coordinate s, Coordinate e) { start = s; end = e; updated = true; }
		inline void update(Coordinate coordinate, bool keep) { if (keep) update(coordinate); else update(coordinate, coordinate); updated = true; }

		// adjust cursor for insert/delete operations
		// (these functions assume that insert or delete points are before the cursor)
		void adjustForInsert(Coordinate insertStart, Coordinate insertEnd);
		void adjustForDelete(Coordinate deleteStart, Coordinate deleteEnd);

		// access cursor properties
		inline Coordinate getInteractiveStart() const { return start; }
		inline Coordinate getInteractiveEnd() const { return end; }
		inline Coordinate getSelectionStart() const { return start < end ? start : end; }
		inline Coordinate getSelectionEnd() const { return start > end ? start : end; }
		inline bool hasSelection() const { return start != end; }

		inline void resetToStart() { update(getSelectionStart(), getSelectionStart()); }
		inline void resetToEnd() { update(getSelectionEnd(), getSelectionEnd()); }

		inline void setMain(bool value) { main = value; }
		inline bool isMain() const { return main; }

		inline void setCurrent(bool value) { current = value; }
		inline bool isCurrent() const { return current; }

		inline void setUpdated(bool value) { updated = value; }
		inline bool isUpdated() const { return updated; }

	private:
		// helper functions
		Coordinate adjustCoordinateForInsert(Coordinate coordinate, Coordinate insertStart, Coordinate insertEnd);
		Coordinate adjustCoordinateForDelete(Coordinate coordinate, Coordinate deleteStart, Coordinate deleteEnd);

		// properties
		Coordinate start{0, 0};
		Coordinate end{0, 0};
		bool main = false;
		bool current = true;
		bool updated = true;
	};

	// the current list of cursors
	class Cursors : public std::vector<Cursor> {
	public:
		// constructor
		Cursors() { clearAll(); }

		// reset the cursors
		void reset();

		// erase all cursors and specify a new one
		inline void setCursor(Coordinate coordinate) { setCursor(coordinate, coordinate); }
		void setCursor(Coordinate start, Coordinate end);

		// add a cursor to the list
		inline void addCursor(Coordinate c) { addCursor(c, c); }
		void addCursor(Coordinate cursorStart, Coordinate cursorEnd);

		// update the current cursor (the one last added)
		inline void updateCurrentCursor(Coordinate coordinate) { at(current).update(coordinate); }
		inline void updateCurrentCursor(Coordinate start, Coordinate end) { at(current).update(start, end); }
		inline void updateCurrentCursor(Coordinate coordinate, bool keep) { at(current).update(coordinate, keep); }

		// check cursor status
		inline bool hasMultiple() const { return size() > 1; }
		bool anyHasSelection() const;
		bool allHaveSelection() const;
		inline bool mainCursorHasSelection() const { return at(main).hasSelection(); }
		inline bool currentCursorHasSelection() const { return at(current).hasSelection(); }
		inline bool mainHasUpdate() const { return at(main).isUpdated(); }
		bool anyHasUpdate() const;

		// clear the selections and create the default cursor
		void clearAll();

		// clear all additional cursors
		void clearAdditional(bool reset=false);

		// clear all updated flags
		void clearUpdated();

		// get main/current cursor
		inline Cursor& getMain() { return at(main); }
		inline size_t getMainIndex() const { return main; }
		inline Cursor& getCurrent() { return at(current); }
		inline size_t getCurrentIndex() const { return current; }
		inline iterator getMainAsIterator() { return begin() + main; }
		inline iterator getCurrentAsIterator() { return begin() + current; }

		// update cursors
		void update();

		// adjust cursors for insert/delete operations
		// (these functions assume that insert or delete points are before the cursor)
		void adjustForInsert(iterator start, Coordinate insertStart, Coordinate insertEnd);
		void adjustForDelete(iterator start, Coordinate deleteStart, Coordinate deleteEnd);

	private:
		size_t main = 0;
		size_t current = 0;
	} cursors;

	// the list of text markers
	class Marker {
	public:
		Marker(ImU32 lc, ImU32 tc, const std::string_view& lt, const std::string_view& tt) :
			lineNumberColor(lc), textColor(tc), lineNumberTooltip(lt), textTooltip(tt) {}

		ImU32 lineNumberColor;
		ImU32 textColor;
		std::string lineNumberTooltip;
		std::string textTooltip;
	};

	std::vector<Marker> markers;

	// tokenizer state
	enum class State : char {
		inText,
		inComment,
		inSingleQuotedString,
		inDoubleQuotedString,
		inOtherString,
		inOtherStringAlt,
		inPreprocessor   // continuation of a backslash-continued #define etc.
	};
	// a single line in a document
	class Line : public std::vector<Glyph> {
	public:
		// state at start of line
		State state = State::inText;

		// marker reference (0 means no marker for this line)
		size_t marker = 0;

		// width of this line (in visible columns)
		int maxColumn = 0;

		// do we need to (re)colorize this line
		bool colorize = true;

		// do we need to draw this line
		bool visible = true;

		int foldLevel = 0;

		// user data associated with this line
		void* userData = nullptr;
	};

	// the document being edited (Lines of Glyphs)
	class Document : public std::vector<Line> {
	public:
		// constructor
		Document() { emplace_back(); }

		// access document's tab size and processing options
		inline void setTabSize(int value) { tabSize = value; }
		inline int getTabSize() const { return tabSize; }
		inline void setInsertSpacesOnTabs(bool value) { insertSpacesOnTabs = value; }
		inline bool isInsertSpacesOnTabs() const { return insertSpacesOnTabs; }

		// manipulate document text (strings should be UTF-8 encoded)
		void setText(const std::string_view& text);
		void setText(const std::vector<std::string_view>& text);
		Coordinate insertText(Coordinate start, const std::string_view& text);
		void deleteText(Coordinate start, Coordinate end);

		// access document text (strings are UTF-8 encoded)
		std::string getText() const;
		std::string getLineText(int line) const;
		std::string getSectionText(Coordinate start, Coordinate end) const;
		ImWchar getCodePoint(Coordinate location) const;

		// get line or color state
		inline State getLineState(int line) const { return at(line).state; }
		Color getColor(Coordinate location) const;

		inline bool getLineVisibility(int line)	const {return at(line).visible; }

		inline int getLineFoldLevel(int line)	const { return at(line).foldLevel; }


		// see if document is empty
		inline bool isEmpty() const { return size() == 1 && at(0).size() == 0; }

		// get number of lines (as an int)
		inline int lineCount() const { return static_cast<int>(size()); }

		// update maximum column numbers for this document and the specified lines
		void updateMaximumColumn(int first, int last);
		inline int getMaxColumn() const { return maxColumn; }

		// translate visible column to line index (and visa versa)
		size_t getIndex(const Line& line, int column) const;
		inline size_t getIndex(Coordinate coordinate) const { return getIndex(at(coordinate.line), coordinate.column); }
		int getColumn(const Line& line, size_t index) const;
		inline int getColumn(int line, size_t index) const { return getColumn(at(line), index); }

		// coordinate operations in context of document
		Coordinate getUp(Coordinate from, int lines=1) const;
		Coordinate getDown(Coordinate from, int lines=1) const;
		Coordinate getLeft(Coordinate from, bool wordMode=false) const;
		Coordinate getRight(Coordinate from, int wordMode=0) const;
		Coordinate getTop() const;
		Coordinate getBottom() const;
		Coordinate getStartOfLine(Coordinate from) const;
		Coordinate getEndOfLine(Coordinate from) const;
		inline Coordinate getNextLine(Coordinate from) const { return getRight(getEndOfLine(from)); }

		// search in document
		Coordinate findWordStart(Coordinate from, bool wordOnly=false) const;
		Coordinate findWordEnd(Coordinate from, bool wordOnly=false) const;
		// Subword (camelCase / snake_case) navigation
		Coordinate findSubWordLeft(Coordinate from) const;
		Coordinate findSubWordRight(Coordinate from) const;
		bool findText(Coordinate from, const std::string_view& text, bool caseSensitive, bool wholeWord, Coordinate& start, Coordinate& end) const;

		// see if document was updated this frame (can only be called once)
		inline bool isUpdated() { auto result = updated; updated = false; return result; }
		inline void resetUpdated() { updated = false; }

		// line-based callbacks
		inline void setInsertor(std::function<void*(int line)> callback) { insertor = callback; }
		inline void setDeletor(std::function<void(int line, void* data)> callback) { deletor = callback; }

		// access line user data
		void setUserData(int line, void* data);
		void* getUserData(int line) const;
		void iterateUserData(std::function<void(int line, void* data)> callback) const;

		// iterate through document to find identifiers
		void iterateIdentifiers(std::function<void(const std::string& identifier)> callback) const;

		// utility functions
		bool isWholeWord(Coordinate start, Coordinate end) const;
		inline bool isEndOfLine(Coordinate from) const { return getIndex(from) == at(from.line).size(); }
		inline bool isLastLine(int line) const { return line == lineCount() - 1; }
		Coordinate findPreviousNonWhiteSpace(Coordinate from, bool includeEndOfLine=true) const;
		Coordinate findNextNonWhiteSpace(Coordinate from, bool includeEndOfLine=true) const;
		Coordinate normalizeCoordinate(Coordinate coordinate) const;
		void normalizeCoordinate(float line, float column, Coordinate& glyphCoordinate, Coordinate& cursorCoordinate) const;

		void setUpdated(bool Updated = true) { updated = Updated; }
		bool isUpdated() const { return updated; }

		inline void setLanguage(const Language* l) { language = l; }
		inline const Language* getLanguage() const { return language; }
	private:
		int tabSize = 4;
		bool insertSpacesOnTabs = false;
		int maxColumn = 0;
		bool updated = false;
		const Language* language = nullptr;
		std::function<void*(int)> insertor;
		std::function<void(int, void*)> deletor;

		void appendLine();
		void insertLine(int line);
		void deleteLines(int start, int end);
		void clearDocument();
	} document;
	enum FoldType {
		Braces,
		IfDef,
		Indent,
		Comment,
		Region,
		PragmaRegion
	};
	struct FoldRange {
		Coordinate start;
		Coordinate end;
		FoldType type;
		bool folded = false;

		FoldRange() = default;
		FoldRange(const Coordinate& s, const Coordinate& e, FoldType t, bool f = false)
			: start(s), end(e), type(t), folded(f) {
			if (end.line < start.line || (end.line == start.line && end.column < start.column)) {
				std::swap(start, end);
			}
		}

		bool operator<(const FoldRange& other) const {
			return start.line < other.start.line || (start.line == other.start.line && start.column < other.start.column);
		}

		bool operator==(const FoldRange& other) const {
			return start.line == other.start.line && end.line == other.end.line && type == other.type;
		}
	};


	// All foldable ranges
	class Folder : public std::vector<FoldRange>
	{
	public:
		// Global folding flag
		bool foldingEnabled = true;

		// Number of lines currently hidden by folds, refreshed in
		// updateVisibility(). When 0, visual index == document line, letting the
		// (per-frame, called many times) visual-index mapping take an O(1)
		// identity path instead of scanning all lines — critical for large files.
		int hiddenLineCount = 0;

		// Visual<->document line maps, rebuilt in updateVisibility() (fold/doc
		// changes only, NOT per frame). They keep the mapping O(1) even when
		// something IS folded — without them, folding a block in a large file
		// tanked FPS because every mapping call went O(lines).
		//   visibleToDoc[vi]   = document line for visual index vi
		//   docToVisible[line] = visual index of `line` (nearest visible
		//                        at-or-before it when `line` is folded away)
		std::vector<int> visibleToDoc;
		std::vector<int> docToVisible;

		// Folding API
		void rebuildFoldRanges(Document& document);
		void updateVisibility(Document& document);
		void toggleFold(int line, Document& document);

		// Unfold every fold that hides `line`, so the line becomes visible.
		// Returns true if anything was changed.
		bool unfoldContaining(int line, Document& document);

		// Bulk fold / unfold operations
		void foldAll(Document& document);
		void unfoldAll(Document& document);

		// Toggle the innermost fold whose range covers `line` (cursor's line).
		// `forceFolded` = +1 means force fold, -1 means force unfold, 0 means toggle.
		// Returns true if anything was changed.
		bool toggleCurrent(int line, Document& document, int forceFolded = 0);

		inline bool isLineFolded(int line) const {
			for (const auto& fr : *this) {
				if (fr.folded &&
					fr.start.line < line && fr.end.line >= line) {
					return true;
				}
			}
			return false;
		}

		inline void sortFoldRanges() {
			std::sort(begin(), end());
			auto last = std::unique(begin(), end());
			erase(last, end());
		}

	private:
		std::unordered_set<int> foldedStartLines;   // remember which folds the user has folded
	} foldRanges;



	// single action to be performed on the document as part of a larger transaction
	class Action {
	public:
		// action types
		enum class Type : char {
			insertText,
			deleteText
		};

		// constructors
		Action() = default;
		Action(Type t, Coordinate s, Coordinate e, const std::string_view& txt) : type(t), start(s), end(e), text(txt) {}

		// properties
		Type type;
		Coordinate start;
		Coordinate end;
		std::string text;
	};

	// a collection of actions for a complete transaction
 	class Transaction : public std::vector<Action> {
	public:
		// access state before/after transactions
		inline void setBeforeState(const Cursors& cursors) { before = cursors; }
		inline const Cursors& getBeforeState() const { return before; }
		inline void setAfterState(const Cursors& cursors) { after = cursors; }
		inline const Cursors& getAfterState() const { return after; }

		// add actions by type
		void addInsert(Coordinate start, Coordinate end, std::string_view text) { emplace_back(Action::Type::insertText, start, end, text); };
		void addDelete(Coordinate start, Coordinate end, std::string_view text) { emplace_back(Action::Type::deleteText, start, end, text); };

		// get number of actions
		inline int actions() const { return static_cast<int>(size()); }

	private:
		// properties
		Cursors before;
		Cursors after;
	};

	// transaction list to support do/undo/redo
 	class Transactions : public std::vector<std::shared_ptr<Transaction>> {
	public:
		// reset the transactions
		void reset();

		// create a new transaction
		static inline std::shared_ptr<Transaction> create() { return std::make_shared<Transaction>(); }

		// add a transaction to the list, execute it and make it undoable
		void add(std::shared_ptr<Transaction> transaction);

		// undo the last transaction
		void undo(Document& document, Cursors& cursors);

		// redo the last undone transaction;
		void redo(Document& document, Cursors& cursors);

		// get status information
		inline size_t getUndoIndex() const { return undoIndex; }
		inline bool canUndo() const { return undoIndex > 0; }
		inline bool canRedo() const { return undoIndex < size(); }
		inline size_t getVersion() const { return version; }

	private:
		size_t undoIndex = 0;
		size_t version = 0;
	} transactions;

	// text colorizer (handles language tokenizing)
	class Colorizer {
	public:
		// update colors in entire document
		void updateEntireDocument(Document& document, const Language* language);

		// update colors in changed lines in specified document
		void updateChangedLines(Document& document, const Language* language);

	private:
		// update color in a single line
		State update(Line& line, const Language* language);

		// see if string matches part of line
		bool matches(Line::iterator start, Line::iterator end, const std::string_view& text);

		// set color for specified range of glyphs
		inline void setColor(Line::iterator start, Line::iterator end, Color color) { while (start < end) (start++)->color = color; }
	} colorizer;

	// details about bracketed text
	class BracketPair {
	public:
		BracketPair(ImWchar sc, Coordinate s, ImWchar ec, Coordinate e, int l) : startChar(sc), start(s), endChar(ec), end(e), level(l) {}
		ImWchar startChar;
		Coordinate start;
		ImWchar endChar;
		Coordinate end;
		int level;

		inline bool isAfter(Coordinate location) const { return start > location; }
		inline bool isAround(Coordinate location) const { return start < location && end >= location; }
	};

	// class responsible for matching brackets
	class Bracketeer : public std::vector<BracketPair> {
	public:
		// reset the bracketeer
		void reset();

		// update the list of bracket pairs in the document and colorize the relevant glyphs
		void update(Document& document);

		// find relevant brackets
		iterator getEnclosingBrackets(Coordinate location);
		iterator getEnclosingCurlyBrackets(Coordinate first, Coordinate last);
		iterator getInnerCurlyBrackets(Coordinate first, Coordinate last);

		// utility functions
		static inline bool isBracketCandidate(Glyph& glyph) {
			return glyph.color == Color::punctuation ||
				glyph.color == Color::matchingBracketLevel1 ||
				glyph.color == Color::matchingBracketLevel2 ||
				glyph.color == Color::matchingBracketLevel3 ||
				glyph.color == Color::matchingBracketError;
		}
	} bracketeer;

	// autocomplete class
	class Autocomplete {
	public:
		// set the autocomplete configuration
		void setConfig(const AutoCompleteConfig* c);

		// request autocomplete mode based on triggers (and if allowed by current state)
		// return value indicates whether autocomplete was initiated
		bool startTyping(Cursors& cursors);
		bool startShortcut(Cursors& cursors);

		// cancel autocomplete mode (if required)
		void cancel();

		// update autocomplete state and render (if required)
		bool render(Document& document, Cursors& cursors, const Language* language, float textOffset, ImVec2 glyphSize);

		// specify a new set of suggestions
		void setSuggestions(const std::vector<std::string>& suggestions);

		// get information
		inline bool isActive() const { return active; }
		inline bool hasSuggestions() const { return state.suggestions.size() > 0 || state.suggestionsPromise; }
		bool isSpecialKeyPressed() const;
		inline ImGuiKeyChord getTriggerShortcut() const { return configuration.triggerShortcut; }
		inline Coordinate getStart() const { return startLocation; }
		inline std::string getReplacement() { return currentSelection < state.suggestions.size() ? state.suggestions[currentSelection] : ""; }

	private:
		// properties

		bool configured = false;
		bool active = false;
		bool requestActivation = false;
		bool requestDeactivation = false;
		Coordinate currentLocation;
		Coordinate startLocation;
		std::chrono::system_clock::time_point activationTime;
		AutoCompleteConfig configuration;
		AutoCompleteState state;
		bool triggeredManually = false;
		size_t currentSelection = 0;
		static constexpr float suggestionWidth = 250.0f;

		// support functions
		void start(Cursors& cursors);
		void updateState(Document& document, const Language* language);
		void refreshSuggestions();
	} autocomplete;

	// access the editor's text
	void setText(const std::string_view& text);

	// render (parts of) the text editor
	void render(const char* title, const ImVec2& size, bool border);
	void renderSelections();
	void renderMarkers();
	void renderMatchingBrackets();
	void renderText();
	void renderTextWrapped();   // word-wrap render path (text + selection + cursors)
	void renderCursors();
	void renderMargin();
	void renderLineNumbers();
	void renderDecorations();
	void renderScrollbarMiniMap();
	void renderPanScrollIndicator();

	void renderFindReplace(ImVec2 pos, float width);

	// keyboard and mouse interactions
	void handleKeyboardInputs();
	void handleMouseInteractions();

	// Host-app remap of editor-internal actions: action id -> chord string.
	// Consulted at the top of handleKeyboardInputs (see tryKeyChordOverrides).
	std::unordered_map<std::string, std::string> keyChordOverrides;
	// Two-stroke pending state for override chords ("Ctrl+K Ctrl+U"): the second
	// combo we're waiting for, plus its age for timeout. Mirrors the host app.
	std::string keyChordPending;
	float       keyChordPendingAge = 0.0f;
	// Returns true if an override chord fired this frame (and ran its action),
	// so the default keyboard handling should be skipped for this frame.
	bool tryKeyChordOverrides();

	// manipulate selections/cursors
	void selectAll();
	void selectLine(int line);
	void selectCursorLines();   // Ctrl+L: expand EVERY cursor's selection to whole line(s); grows down on repeat

	void selectWord(int line, int column);

	void selectLines(int startLine, int endLine);
	void selectRegion(int startLine, int startColumn, int endLine, int endColumn);
	void selectToBrackets(bool includeBrackets);
	void growSelectionsToCurlyBrackets();
	void shrinkSelectionsToCurlyBrackets();

	void cut();
	void copy() const;
	void paste();
	void undo();
	void redo();

	// access cursor locations
	void getCursor(int& line, int& column, size_t cursor) const;
	void getCursor(int& startLine, int& startColumn, int& endLine, int& endColumn, size_t cursor) const;
	std::string	getCursorText(size_t cursor) const;

	// scrolling support
	void makeCursorVisible();
	void scrollToLine(int line, Scroll alignment);

	// find/replace support
	void goToFirstOccurrenceOf(const std::string_view& text, bool caseSensitive = true, bool select = false);
	void goToDefinitionOf(const std::string_view& text, bool caseSensitive = true);
	void selectFirstOccurrenceOf(const std::string_view& text, bool caseSensitive, bool wholeWord);
	void selectNextOccurrenceOf(const std::string_view& text, bool caseSensitive, bool wholeWord);
	void selectAllOccurrencesOf(const std::string_view& text, bool caseSensitive, bool wholeWord);
	void addNextOccurrence();
	void selectAllOccurrences();

	void replaceTextInCurrentCursor(const std::string_view& text);
	void replaceTextInAllCursors(const std::string_view& text);
	void replaceSectionText(const Coordinate& start, const Coordinate& end, const std::string_view& text);

	void openFindReplace();
	void closeFindReplace();
	void find();
	void findNext();
	void findAll();
	void replace();
	void replaceAll();

	// marker support
	void addMarker(int line, ImU32 lineNumberColor, ImU32 textColor, const std::string_view& lineNumberTooltip, const std::string_view& textTooltip);
	void clearMarkers();

	// cursor/selection functions
	void moveUp(int lines, bool select);
	void moveDown(int lines, bool select);
	void moveLeft(bool select, bool wordMode, bool subWord = false);
	void moveRight(bool select, bool wordMode, bool subWord = false);
	void moveToTop(bool select);
	void moveToBottom(bool select);
	void moveToStartOfLine(bool select);
	void moveToEndOfLine(bool select);
	void moveTo(Coordinate coordinate, bool select);

	// add/delete characters
	void handleCharacter(ImWchar character);
	void handleBackspace(bool wordMode);
	void handleDelete(bool wordMode);

	// add/delete lines
	void removeSelectedLines();
	void insertLineAbove();
	void insertLineBelow();

	// transform selected lines
	void indentLines();
	void deindentLines();
	void moveUpLines();
	void moveDownLines();
	void toggleComments();

	void toggleCommentsShift();

	// transform selections (filter function should accept and return UTF-8 encoded strings)
	void filterSelections(std::function<std::string(std::string_view)> filter);
	void selectionToLowerCase();
	void selectionToUpperCase();

	// transform entire document (filter function should accept and return UTF-8 encoded strings)
	void stripTrailingWhitespaces();
	void filterLines(std::function<std::string(std::string_view)> filter);
	void tabsToSpaces();
	void spacesToTabs();

	// transaction functions
	// note that strings must be UTF-8 encoded
	std::shared_ptr<Transaction> startTransaction(bool cancelsAutoComplete=true);
	bool endTransaction(std::shared_ptr<Transaction> transaction);

	void insertTextIntoAllCursors(std::shared_ptr<Transaction> transaction, const std::string_view& text);
	void deleteTextFromAllCursors(std::shared_ptr<Transaction> transaction);
	void autoIndentAllCursors(std::shared_ptr<Transaction> transaction);
	Coordinate insertText(std::shared_ptr<Transaction> transaction, Coordinate start, const std::string_view& text);
	void deleteText(std::shared_ptr<Transaction> transaction, Coordinate start, Coordinate end);

	// editor options
	float lineSpacing = 1.0f;
	bool readOnly = false;
	bool autoIndent = true;
	bool showSpaces = true;
	bool showTabs = true;
	bool showLineNumbers = true;
	bool showScrollbarMiniMap = true;
	bool showMatchingBrackets = true;
	bool completePairedGlyphs = true;
	bool overwrite = false;

	// rendering context
	ImFont* font;
	float fontSize;
	ImVec2 glyphSize;
	// Proportional-text support. When the active font is NOT fixed-pitch we map
	// columns to pixels by summing real glyph advances instead of column*cell.
	// `proportional` is detected once per render() from the font metrics; when
	// false, columnToX/xToColumn use the exact old arithmetic so monospace
	// rendering is byte-identical (and can't regress).
	bool proportional = false;
	// Pixel X (relative to text origin, i.e. excludes textOffset) of the LEFT
	// edge of `column` on `line`. Walks the glyphs in proportional mode (tabs
	// snap to tabSize cells using the space advance); O(1) multiply otherwise.
	float columnToX(int line, int column) const;
	// Inverse: nearest column for a pixel X (relative to text origin).
	int   xToColumn(int line, float x) const;
	// Advance (px) of one glyph at (line, index) given its start column — tab
	// expands to the next tab stop. Uses CalcTextSize so it matches RenderChar.
	float glyphAdvanceX(ImWchar codepoint, int column) const;
	ImVec2 lastRenderOrigin;
	float lineNumberLeftOffset;
	float lineNumberRightOffset;
	float decorationOffset;
	float textOffset;
	float visibleHeight;
	int visibleLines;
	int firstVisibleLine;
	int lastVisibleLine;
	float visibleWidth;
	int visibleColumns;
	int firstVisibleColumn;
	int lastVisibleColumn;
	float verticalScrollBarSize;
	float horizontalScrollBarSize;
	float cursorAnimationTimer = 0.0f;
	bool ensureCursorIsVisible = false;
	int scrollToLineNumber = -1;
	Scroll scrollToAlignment = Scroll::alignMiddle;
	bool showMatchingBracketsChanged = false;
	bool languageChanged = false;
	// Debounce for the two O(document) post-edit passes (fold-range reparse +
	// bracket rescan). Per-line recolor still runs every edit (live), but these
	// full-document passes only run once typing settles — otherwise editing a big
	// file reparses everything every keystroke and tanks the frame rate.
	bool   structureDirty = false;
	double structureDirtyTime = 0.0;

	float decoratorWidth = 0.0f;
	std::function<void(Decorator&)> decoratorCallback;

	std::function<void(int line)> lineNumberContextMenuCallback;
	std::function<void(int line, int column)> textContextMenuCallback;


	int contextMenuLine = 0;
	int contextMenuColumn = 0;

	static constexpr int leftMargin = 1; // margins are expressed in glyphs
	static constexpr int decorationMargin = 1;
	static constexpr int textMargin = 2;
	static constexpr int cursorWidth = 1;

	// find and replace context
	std::string findButtonLabel = "Find";
	std::string findAllButtonLabel = "Find All";
	std::string replaceButtonLabel = "Replace";
	std::string replaceAllButtonLabel = "Replace All";
	bool findReplaceVisible = false;
	bool focusOnEditor = true;
	bool focusOnFind = false;
	bool focusOnReplace= false;
	bool findCancelledAutocomplete = false;
	std::string findText;
	std::string replaceText;
	bool caseSensitiveFind = false;
	bool wholeWordFind = false;
	// Live match-count readout for the find bar (VSCode-style "3 of 12"). Cached
	// and only recomputed when the search term / options / document version
	// change, so it doesn't rescan the whole document every frame.
	int findMatchCount = 0;          // total matches of findText in the document
	int findMatchCurrent = 0;        // 1-based index of the match at the cursor, 0 if none
	std::string findCountCacheKey;   // term+flags+version the counts were computed for
	void recomputeFindMatches();     // refresh findMatchCount/findMatchCurrent if stale
	// Click-and-drag-of-selected-text state. When the user mouses down on an
	// existing selection we remember the text + its source range; on release
	// we either move it (default) or copy it (Ctrl held) to the drop point.
	std::string draggedText;
	bool        draggingSelection = false;
	Coordinate  dragSelectionStart{};
	Coordinate  dragSelectionEnd{};

	// If the user mouses down inside an existing selection we hold off on
	// updating the cursor — that way the click can become either a drag or
	// (on release without movement) a normal click-to-deselect.
	bool        pendingClickInSelection = false;
	Coordinate  pendingClickCoord{};

	// Column / box selection: Alt+Shift+drag, VSCode/Sublime style.
	bool        columnSelecting   = false;
	Coordinate  columnAnchor{};

	// Set on left-mousedown over text / line numbers, cleared on release.
	// Prevents drags that originated on the horizontal scrollbar (or any
	// other widget that happens to share the X column with the text) from
	// extending the selection.
	bool        selectingText     = false;
	Coordinate draggedSectionStart;
	// interaction context
	float lastClickTime = -1.0f;
	ImWchar completePairCloser = 0;
	Coordinate completePairLocation;
	bool panMode = true;
	bool panInverted = false;
	// Middle-mouse drag axis SNAP (1=horizontal, 2=vertical). Re-evaluated every
	// frame so you can change direction mid-drag, but the drag always snaps to ONE
	// axis (no diagonal drift). Hysteresis + a smoothed velocity (panVelEMA) stop it
	// flip-flopping near the boundary; strong vertical bias (vertical is the default
	// and is sticky, horizontal needs a clearly sideways motion to engage). Reset to
	// vertical on each middle-press.
	int    panSnapAxis = 2;
	ImVec2 panVelEMA{ 0.0f, 0.0f };
	// Middle-mouse pan/scroll acceleration. Scroll speed grows superlinearly with
	// the distance from the click anchor ("scroll cursor") to the live cursor:
	// speed *= 1 + (dist/refDist)^2 * gain * 0.25, capped. The internal 0.25 factor
	// makes the user-facing default of 1.0 feel right (1.0 here == the old 0.25).
	// 0 = linear (no acceleration); higher = farther pulls fly faster.
	float panScrollAccelGain = 1.0f;

	// Word-wrap state. wrapRows is rebuilt each frame in render() when wordWrap
	// is on; it is the 1:many visual-row model (one document line → N rows).
	// Empty / unused when wordWrap is off.
	bool  wordWrap = false;
	float wrapWidthPx = 0.0f;        // 0 = wrap to the view width
	int   wrapColumns = 0;           // computed: chars per row this frame
	struct WrapRow { int line; int startColumn; int endColumn; };
	std::vector<WrapRow> wrapRows;
	void  buildWrapRows();           // (re)compute wrapRows for visible lines
	int   wrapRowOfCoordinate(const Coordinate& c) const;  // row index for a doc coord
	bool panning = false;
	bool scrolling = false;
	ImVec2 scrollStart;
	ImVec2 panScrollAnchor;   // screen-space click point; the indicator draws here
	bool showPanScrollIndicator = true;
	std::function<void()> delayedChangeCallback;
	std::chrono::milliseconds delayedChangeDelay;
	std::chrono::system_clock::time_point delayedChangeReportTime;
	bool delayedChangeDetected = false;
	std::function<void(std::vector<Change>&)> transactionCallback;

	// color palette support
	void updatePalette();
	static Palette defaultPalette;
	Palette paletteBase;
	Palette palette;
	float paletteAlpha;

	// language support
	const Language* language = nullptr;
	public:
	int screenYToDocumentLine(float y) const;
	const FoldRange* GetFoldRangeStartingAt(int line);
	int getVisualLineCount() const {
		// Fast path: nothing folded → every line visible.
		if (foldRanges.hiddenLineCount == 0) return document.lineCount();
		// Cached path: visibleToDoc holds exactly the visible lines.
		if ((int) foldRanges.docToVisible.size() == document.lineCount())
			return (int) foldRanges.visibleToDoc.size();
		// Stale-size fallback (document changed before updateVisibility re-ran).
		int n = 0;
		for (int i = 0; i < document.lineCount(); ++i)
			if (document[i].visible) ++n;
		return n;
	}

	int visualIndexToLine(int visualIndex) const {
		if (visualIndex < 0) visualIndex = 0;
		// Fast path: nothing folded → visual index IS the document line.
		if (foldRanges.hiddenLineCount == 0) {
			int last = document.lineCount() - 1;
			if (last < 0) return 0;
			return visualIndex > last ? last : visualIndex;
		}
		// Cached path: O(1) lookup.
		const auto& v2d = foldRanges.visibleToDoc;
		if (!v2d.empty() && (int) foldRanges.docToVisible.size() == document.lineCount()) {
			if (visualIndex >= (int) v2d.size()) return v2d.back();
			return v2d[visualIndex];
		}
		// Stale-size fallback.
		int current = 0, lastVisible = 0;
		for (int i = 0; i < document.lineCount(); ++i) {
			if (!document[i].visible) continue;
			if (current == visualIndex) return i;
			lastVisible = i;
			++current;
		}
		return lastVisible;
	}

	int lineToVisualIndex(int line) const {
		if (line < 0) return 0;
		// Fast path: nothing folded → document line IS the visual index.
		if (foldRanges.hiddenLineCount == 0) {
			int last = document.lineCount() - 1;
			if (last < 0) return 0;
			return line > last ? last : line;
		}
		// Cached path: O(1) lookup.
		const auto& d2v = foldRanges.docToVisible;
		if (!d2v.empty() && (int) d2v.size() == document.lineCount()) {
			if (line >= (int) d2v.size()) return d2v.back();
			return d2v[line];
		}
		// Stale-size fallback.
		int current = 0, lastVisibleBeforeTarget = 0;
		for (int i = 0; i < document.lineCount(); ++i) {
			if (i > line) break;
			if (!document[i].visible) continue;
			if (i == line) return current;
			lastVisibleBeforeTarget = current;
			++current;
		}
		return lastVisibleBeforeTarget;
	}

};
