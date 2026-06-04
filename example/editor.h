//	TextEditor - A syntax highlighting text editor for ImGui
//	Copyright (c) 2024-2026 Johan A. Goossens. All rights reserved.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


#pragma once


//
//	Include files
//

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <imgui.h>
#include "../TextEditor.h"
#include "../TextDiff.h"


//
//	Editor
//

class Editor {
	public:
	// constructor
	Editor();

	// file related functions
	void newFile();
	void newFile(std::string& path);

	void openFile();
		void openFile(const std::string& path);
	void saveFile();
	void saveFile(std::string& path);


	// manage program exit
	void tryToQuit();
	inline bool isDone() const { return done; }

	// render the editor
	void render();

	private:
	// per-document state — each doc is its own dockable window
	struct TabDocument {
		TextEditor editor;
		TextDiff   diff;
		std::string filename    = "untitled";
		std::string originalText;
		size_t      version     = 0;
		size_t      id          = 0;       // stable id for window labels
		bool        open        = true;    // window open flag
		bool        wantFocus   = false;   // request focus next frame
		bool        wantSplit   = false;   // dock as a split next to the active doc
		bool        dockedOnce  = false;   // have we forced this doc into the central node yet?
		TextEditor::Trie trie;
	};

	// document management — stored as unique_ptr so addresses remain stable
	std::vector<std::unique_ptr<TabDocument>> tabs;
	size_t activeTab = 0;
	size_t nextId    = 1;

	// "Reopen closed tab" stack — keeps the last few closed tabs' filenames+text
	struct ClosedTab { std::string filename; std::string text; };
	std::vector<ClosedTab> recentlyClosed;

	void reopenLastClosedTab();
	void openContainingFolder();   // OS file explorer at the active doc's dir

	// Alt+O: switch between the .h/.hpp/.hxx file and its sibling .c/.cpp/.cc/.cxx
	// (or vice versa). Re-uses an already-open tab when found.
	void toggleHeaderSource();

	inline TabDocument& doc()             { return *tabs[activeTab]; }
	inline const TabDocument& doc() const { return *tabs[activeTab]; }

	bool isDirtyTab(size_t i) const { return tabs[i]->editor.GetUndoIndex() != tabs[i]->version; }
	inline bool isDirty()   const { return isDirtyTab(activeTab); }
	inline bool isSavable() const { return isDirty() && doc().filename != "untitled"; }

	// private functions
	void renderMenuBar();
	void renderStatusBar();
	void renderDockedDocuments();
	void renderDocumentWindow(TabDocument& t);

	// index = -1  → append at end
	// split = true → request the new doc be docked next to the active one
	TabDocument& newTab(const std::string& path, bool split = false, int index = -1);
	TabDocument& newTab();
	
	void closeTab(size_t idx);
	std::string windowLabelFor(const TabDocument& t) const;

	void showDiff();
	void showFileOpen();
	void showSaveFileAs();
	std::string dialogStartDir() const;   // start dir for open/save dialogs (active doc → project → cwd)
	void showConfirmClose(std::function<void()> callback);
	void showConfirmQuit();
	void showError(const std::string& message);

	void renderDiff();
	void renderFileOpen();
	void renderSaveAs();
	void renderConfirmClose();
	void renderConfirmQuit();
	void renderConfirmError();

	void setAutocompleteMode(bool flag);
	void buildAutocompleteTrie(TabDocument& t);
	void configureTabAutocomplete(TabDocument& t);   // wire autocomplete + build trie for ONE tab

	static const TextEditor::Language* languageForPath(const std::string& path);

public:
	// Set the navigation panel's project root and show it. Called from
	// main.cpp's --project arg path and from the Open Project dialog.
	void setProjectRoot(const std::filesystem::path& p);

	// Persistent settings root — same absolute location every run regardless
	// of cwd, so layout / favourites / etc. actually round-trip.
	//   Windows: %APPDATA%\ImGuiColorTextEdit\
	//   POSIX:   $XDG_CONFIG_HOME/imguicolortext  (or  $HOME/.config/...)
	// The directory is created on first call. Public so main.cpp can route
	// the ImGui ini path through it too.
	static std::filesystem::path userConfigDir();

	// Frame-pacing prefs, read by main.cpp each loop iteration.
	int  fpsLimit() const       { return prefFpsLimit; }
	bool idleThrottle() const   { return prefIdleThrottle; }
private:

	// Languages discovered at startup from `./languages/*.lang` (or relative to
	// the executable). Indexed by lowercase extension, e.g. ".html" → HTML.
	static std::unordered_map<std::string, const TextEditor::Language*>& runtimeLanguagesByExt();
	static void loadRuntimeLanguages();
	static void runScriptForActiveDoc();  // F5 in script-runner

	// application state
	bool done = false;
	std::string errorMessage;
	std::function<void()> onConfirmClose;

	// remember which viewport was focused when a modal/file dialog was opened
	// so we can place the popup on that viewport (matters with multi-viewport).
	// `dialogNeedsPlacement` is set on open and cleared after one render so the
	// dialog isn't snapped back to the original viewport every frame.
	unsigned int dialogViewportId    = 0;
	bool         dialogNeedsPlacement = false;
	float fontSize = 17.0f;
	inline void increaseFontSIze() { fontSize = std::clamp(fontSize + 1.0f, 8.0f, 24.0f); }
	inline void decreaseFontSIze() { fontSize = std::clamp(fontSize - 1.0f, 8.0f, 24.0f); }

	bool autocomplete = true;   // on by default; underpins identifier navigation

	// Script runner state — F5 spawns the interpreter for the current document
	// in a background thread and streams stdout/stderr into a docked "Output"
	// window. The state lives in a shared_ptr so that detached worker threads
	// keep their write target alive after the Editor itself is destroyed
	// (e.g. on program shutdown while a script is still hanging). Without
	// this, the thread would crash writing into a freed Editor instance.
	struct ScriptState {
		std::mutex          mutex;
		std::string         output;
		bool                visible = false;
		std::atomic<bool>   running{false};
		std::atomic<int>    gen{0};
	};
	std::shared_ptr<ScriptState> script = std::make_shared<ScriptState>();
	void renderScriptOutputWindow();
	void runScriptForDoc();

	// C# "Go to Decompiled Source": resolve a BCL type to its runtime assembly
	// and decompile it with ilspycmd (auto-installed if missing), caching the
	// generated .cs. Runs on a detached thread (survives Editor via shared_ptr,
	// same pattern as ScriptState); results published to the UI thread by
	// pollDecompile(), which opens the cached file read-only.
	struct DecompileState {
		std::mutex        mutex;
		std::string       symbol;       // requested type (for messaging)
		std::string       resultPath;   // cached .cs path on success
		std::string       error;        // non-empty on failure → caller falls back to Learn
		std::atomic<bool> running{false};
		std::atomic<bool> done{false};
		std::atomic<bool> published{true};   // false while a result awaits the UI thread
	};
	std::shared_ptr<DecompileState> decompileState = std::make_shared<DecompileState>();
	void openCSharpDecompiled(const std::string& rawSymbol);   // spawn decompile (BCL types)
	void pollDecompile();                                       // UI-thread publish → open tab

	// Per-project symbol index. Built once in the background when a project is
	// opened (and refreshed on demand), it caches every identifier seen + the
	// definition sites for each symbol, so autocomplete and Go-to-Definition
	// read a table instead of grepping the tree each time. Lives in a shared_ptr
	// (same survival reason as ScriptState) and is published atomically under a
	// mutex after each build. `gen` discards a stale build when the project
	// changes mid-index.
	struct DefSite { std::string file; int line; int score; };
	struct ProjectIndex {
		std::vector<std::string>                              identifiers; // sorted unique
		std::unordered_map<std::string, std::vector<DefSite>> defs;        // symbol -> sites
	};
	struct IndexState {
		std::mutex                          mutex;
		std::shared_ptr<const ProjectIndex> index;
		std::atomic<bool>                   building{false};
		std::atomic<int>                    gen{0};
	};
	std::shared_ptr<IndexState> indexState = std::make_shared<IndexState>();
	void rebuildProjectIndex();                              // spawn background build
	std::shared_ptr<const ProjectIndex> indexSnapshot();     // thread-safe read
	TextEditor::Trie projectTrie;        // shared project-wide identifier trie (one copy, not per-tab)
	int projectTrieGen = -1;             // index gen projectTrie was built from (-1 = never)
	void buildProjectTrie();             // (re)build projectTrie from the current index snapshot
	// F6: walk up from the active doc to find a project build script
	// (build.bat / build.ps1 / build.sh / Makefile / CMakeLists.txt) and run it.
	void runProjectBuild();

	enum class State {
		edit,
		diff,
		newFile,
		openFile,
		saveFileAs,
		confirmClose,
		confirmQuit,
		confirmError,
		gotoLine,
		openProject
	} state = State::edit;

	// Goto-line dialog state
	void showGotoLine();
	void renderGotoLine();
	char gotoLineBuf[32] = "";

	// Navigation panel — dockable file tree rooted at `projectRoot`.
	void renderNavigationPanel();
	void openProjectFolderPicker();
	bool                  navPanelVisible = true;      // docked by default
	bool                  dockLayoutInitialized = false; // first-time DockBuilder split
	unsigned int          centralDockId = 0;           // node new doc windows dock into
	std::filesystem::path projectRoot;            // empty = none set; default to cwd

	// Nav tree context menu state — set when a file/folder is right-clicked.
	std::string  navContextPath;            // path the menu acts on this frame
	bool         navShowDotFiles = false;   // toggle in nav header
	bool         navShowExcluded = false;   // expose hidden-from-view items
	bool         navCodeOnly     = false;   // hide non-source files
	bool         navFlatFiles    = false;   // collapse folder bodies (show folders only)
	std::unordered_map<std::string, bool> navExcluded; // abs path → true = hidden in tree
	std::string  navClipboardPath;          // last "Copy" target — used by Paste
	bool         navClipboardIsCut = false; // true = move on paste, false = copy
	std::string  navRenameTarget;           // path currently being renamed in-place
	char         navRenameBuf[256] = {0};   // edit buffer for the rename input
	std::string  navPendingDelete;          // path queued for confirm-delete modal
	std::string  navDragSource;             // payload for the move/copy drag-source
	void         navOpenPathInExplorer(const std::string& path);
	void         navInitDockLayout(unsigned int dockId);   // first-frame split layout
	void         navDeletePath(const std::string& p);      // recycle bin (Win) / fallback
public:
	bool         navIsCodeFile(const std::filesystem::path& p) const;
	void         navOpenExternally(const std::string& path); // for non-text non-image
	bool         navIsExcluded(const std::filesystem::path& p) const;
	bool         navIsCodeOnly() const { return navCodeOnly; }
	bool         navIsFlat() const     { return navFlatFiles; }
	bool         navIsShowingExcluded() const { return navShowExcluded; }
	void         navDragSourceSet(const std::string& p) { navDragSource = p; }
	void         navMoveOrCopy(const std::string& src,
	                           const std::string& destDir, bool copy);
	// Public so the static nav-render helpers (navRenderEntry/navRenderFlat) can
	// draw an image thumbnail in a file's hover tooltip.
	static bool  isImageExt(const std::string& ext);
	void         navShowImageThumbnail(const std::string& path);
private:

	// Split-right command (Ctrl+\): dock the next-active doc into a fresh
	// split node to the right of the current one. Builds via DockBuilder.
	void splitActiveTabRight();
	bool wantSplitRight = false;

	// Open a file into a split pane on the left (dir<0) or right (dir>0) of the
	// document area — used by the nav panel's "Open to Left/Right". Opens the
	// file, then a deferred DockBuilder split in renderDockedDocuments docks it
	// to the side (handled next frame via these pending fields).
	void openFileToSide(const std::string& path, int dir);
	size_t pendingSideDocId = 0;
	int    pendingSideDir   = 0;   // -1 left, +1 right, 0 none

	// Settings dialog — interpreters per extension, build overrides,
	// editor toggles. Persisted to <configDir>/settings.json (minimal
	// hand-rolled writer; no JSON dep).
	bool settingsVisible = false;
	bool settingsFocusRequest = false;   // un-collapse + focus the settings window next frame
	// Middle-mouse pan/scroll for non-editor windows (nav tree, settings). Mirrors
	// the editor widget's pan using the public scroll API; tracks its own per-frame
	// delta so it never contends with the editor's global drag-delta reset.
	void middleMousePanScroll(int windowKey);
	std::unordered_map<std::string, std::string> interpreterOverrides; // ".py" → "python"
	std::unordered_map<std::string, std::string> projectBuildOverrides; // abs root → cmd
	// User keybinding overrides for APP-LEVEL actions, keyed by bind id
	// (e.g. "file.new" → "Ctrl+Shift+N"). Persisted in [keybinds]. Editor-
	// internal chords (undo/cut/fold/Ctrl+K…) live in the TextEditor widget and
	// are not remappable here. Empty/absent = use the action's default chord.
	std::unordered_map<std::string, std::string> keybindOverrides;
	// Two-stroke chord state for the app-level matcher: when a chord string has
	// two combos ("Ctrl+K Ctrl+U"), the first combo arms this prefix; the second
	// must arrive within a short window. Reset on timeout / Escape / mismatch.
	mutable std::string keyChordPending;   // first stroke already seen this sequence, or empty
	mutable float       keyChordPendingAge = 0.0f;   // seconds since the prefix armed
	void tickKeyChordPending();            // per-frame decay/cancel of the pending prefix
	// True if the live keyboard state this frame matches `chord` (e.g.
	// "Ctrl+Shift+N"): exact modifier set + the named key just pressed.
	bool keyChordPressed(const std::string& chord) const;
	// Resolve a bind id to its active chord string (override or default), then
	// test it this frame. defaultChord is used when no override exists.
	bool keybindPressed(const char* id, const char* defaultChord) const;
	// Push the editor-internal keybind overrides (edit.*/code.* with a widget
	// action id) into every open tab's TextEditor via SetKeyChordOverride, so a
	// rebind of e.g. "to UPPERCASE" actually takes effect inside the widget.
	// Called after settings load, on new tabs, and whenever a chord is recorded.
	void applyKeybindOverridesToEditors();
	void applyKeybindOverridesToEditor(TextEditor& ed) const;
	bool   prefAutoIndent      = true;
	bool   prefCompletePairs   = true;
	// Format Document brace placement: braces on their own line (Allman) vs
	// attached, PER LANGUAGE group. prefFormatBraceNewLine is the default for
	// languages without a specific entry.
	bool   prefFormatBraceNewLine = true;   // default (and "Other" languages)
	bool   prefFormatBraceCpp  = true;      // C / C++ / ObjC
	bool   prefFormatBraceCs   = true;      // C#
	bool   prefFormatBraceJs   = false;     // JS / TS (conventionally same-line)
	bool   prefFormatBraceJava = false;     // Java (conventionally same-line)
	bool   formatBraceNewLineForExt(const std::string& ext) const;   // resolve per-lang brace pref
	void   formatActiveDocument();          // run clang-format over the active doc (undo-safe)
	bool   prefShowFps         = false;   // FPS readout on the status bar
	bool   prefCtrlScrollZoom  = true;    // Ctrl+wheel adjusts editor font size
	bool   prefInvertPan       = false;   // flip middle-mouse pan direction
	bool   prefWordWrap        = false;   // soft-wrap long lines
	int    prefWrapWidthPx     = 0;       // 0 = wrap to view width, else fixed px
	float  prefPanScrollAccel  = 1.0f;    // middle-mouse pan/scroll accel gain (1.0 = default feel, 0 = linear)
	int    prefFpsLimit        = 60;      // target framerate cap; 0 = unlimited
	bool   prefIdleThrottle    = true;    // drop to ~10 fps when window unfocused

	// Manual viewport control (multi-viewport is always on; windows only
	// leave the main window when dragged out or popped via these).
	void popOutActiveDoc(int dir);        // dir: -1 = left, +1 = right
	void remergeActiveWindow();           // dock active doc back into the main window
	void remergeAllWindows();             // rebuild the default docked layout
	float  prefFontSize        = 17.0f;
	std::string fontPath;            // absolute path to user-chosen TTF (empty = bundled DejaVu)
	ImFont*     activeFont = nullptr; // result of AddFontFromFileTTF for fontPath; null = default
	std::vector<std::string> availableFonts; // discovered TTF files; lazy-filled on first Settings open
	void   discoverFonts();
	static bool fontNameLooksMonospace(const std::string& lowerName);   // filename heuristic for sort grouping
	void   applyFont();              // (re)load fontPath into the atlas
	void   renderSettings();
	void   loadSettings();
	void   saveSettings();

	// Detected toolchains — discovered once on first Settings open and
	// cached. User can edit which one is the active path for builds.
	struct DetectedTool { std::string label; std::string path; };
	std::vector<DetectedTool> detectedMsvc;
	std::vector<DetectedTool> detectedDotnetSdks;
	std::string activeMsvcPath;    // selected from detectedMsvc (or hand-edited)
	std::string activeDotnetPath;
	std::string activeBuildConfig = "Debug"; // Debug | Release | RelWithDebInfo | MinSizeRel
	bool         toolchainsDetected = false;
	void         detectToolchains();

	// `dotnet --list-sdks` can block for seconds; it runs on a detached thread
	// (this shared state survives Editor destruction) and results are published
	// into detectedDotnetSdks on the main thread by pollDotnetProbe().
	struct DotnetProbe {
		std::mutex                mutex;
		std::vector<DetectedTool> sdks;
		std::atomic<bool>         done{false};
	};
	std::shared_ptr<DotnetProbe> dotnetState = std::make_shared<DotnetProbe>();
	bool dotnetPublished = false;
	void pollDotnetProbe();

	// Smart F5: locate the most-recent built exe under projectRoot's
	// common build dirs (out/build/*, build/, bin/{Debug,Release}/) and
	// run it. Returns empty if none found.
	std::filesystem::path findBuiltExe() const;
	void        runProjectExeOrScript();   // F5: prefer exe, fall back to script
	void        runProjectWithArgs();      // like F5 but prompts for arguments

	// Shared launcher: spawn `cmd` (optionally cd'd to runDir) on a detached
	// thread, streaming stdout+stderr into the Output panel. The single home
	// for the _popen/popen pipe loop that build / run / nav-run all share.
	void        runCommandInOutputPanel(const std::string& cmd,
	                                     const std::filesystem::path& runDir = {});
	// Resolve the active document to a runnable command (interpreter + path),
	// saving it first; {empty,_} with a reason written to Output if it can't run.
	std::pair<std::string, std::filesystem::path> docScriptCommand();
	// What F5 runs: freshest built exe under projectRoot, else the active doc.
	std::pair<std::string, std::filesystem::path> projectRunCommand();
	// Is `p` something we know how to launch (mapped interpreter, .exe, etc.)?
	bool        navIsRunnable(const std::string& p) const;
	// Build {command, workingDir} to launch an arbitrary file from the nav tree.
	std::pair<std::string, std::filesystem::path> runCommandForPath(const std::string& p) const;

	// "Run with arguments…" modal — stashes a base command + dir, prompts for an
	// argument string, then appends it verbatim and launches. Rendered each frame
	// at render() top level so any menu/context item can request it.
	void        requestRunWithArgs(const std::string& baseCmd, const std::filesystem::path& runDir);
	void        renderRunArgsPopup();
	bool         runArgsRequested = false;
	std::string  runArgsBaseCmd;
	std::filesystem::path runArgsDir;
	char         runArgsBuf[1024] = {};

	// Image viewer — load .png/.jpg/etc via stb_image, upload as ImTextureData,
	// display in a dockable window. Multiple images can be open at once.
	struct ImageDoc {
		std::string         path;
		std::string         windowTitle;
		ImTextureData*      tex = nullptr;
		int                 w = 0;
		int                 h = 0;
		bool                open = true;
		bool                wantFocus = false;
		bool                fitted = false;   // auto-fit-to-window done once on first display
		float               zoom = 1.0f;
	};
	std::vector<std::unique_ptr<ImageDoc>> images;
	void openImageFile(const std::string& path);
	void renderImageWindows();

	// Lazy thumbnail cache for nav-panel hover previews. One GPU texture per
	// image path, loaded on first hover and reused; capped so a big image tree
	// can't balloon VRAM. Drawn inside the file's hover tooltip.
	struct Thumb {
		ImTextureData* tex = nullptr;
		int w = 0, h = 0;        // source pixel size
		bool failed = false;     // load failed — don't retry every hover
	};
	std::unordered_map<std::string, Thumb> thumbCache;
	// isImageExt + navShowImageThumbnail are declared public above (used by the
	// static nav-render helpers).
	static bool isBinaryExt(const std::string& ext); // exe/dll/so/dylib/zip/etc — don't open as text

	// Diff-against-another-file. Tracks the path of the alt file we're
	// diffing against, so renderDiff can offer file vs file mode.
	std::string  diffOtherPath;
	bool         diffOtherMode = false;
	void         openDiffOtherDialog();   // pick second file

	// Hover hint state — picked up from imgui-bundle's demo behaviour.
	// While the user holds the mouse still over a word for `hoverDelaySec`,
	// we show a tooltip with the symbol's location + reference count.
	std::string  hoverWord;
	ImVec2       hoverPos{0, 0};
	float        hoverIdleSec   = 0.0f;
	float        hoverDelaySec  = 0.5f;
	void renderHoverTooltip(TabDocument& t);

	// Memoized #include "Go to File" resolution. The context-menu callback
	// re-runs every frame the popup is open, and resolving an include
	// recursively walks the project tree + the MSVC/Windows SDK system include
	// dirs (tens of thousands of files) — doing that ~60x/second froze the app.
	// Cache the result keyed by "<doc>|<include>".
	std::string  ctxIncludeKey;
	std::string  ctxIncludeResult;
	bool         ctxIncludeFound = false;

	// Find References results panel — project-wide. Each hit records the file
	// it was found in so clicking opens that file and jumps to the line.
	struct RefHit { std::string file; int line; std::string text; };
	bool                            referencesVisible = false;
	std::string                     referencesWord;
	std::vector<RefHit>             referencesHits;
	int                             referencesFileCount = 0;
	bool                            referencesAllFiles = false;   // false = active file only (default), true = whole project
	TabDocument*                    referencesTab = nullptr;      // tab the last search ran against (for the All-files re-run)
	void findReferencesOf(TabDocument& t, const std::string& word);
	void renderReferencesPanel();

	// Find in Files — project-wide text search with a query box + options.
	bool                            findInFilesVisible = false;
	bool                            findInFilesFocus   = false;   // focus the query box next frame
	char                            findInFilesQuery[256] = {};
	bool                            findInFilesCase = false;       // match case
	bool                            findInFilesWholeWord = false;  // whole-word matches only
	std::vector<RefHit>             findInFilesHits;
	int                             findInFilesFileCount = 0;
	bool                            findInFilesTruncated = false;  // hit the result cap
	void runFindInFiles();
	void renderFindInFilesPanel();
	void openFindInFiles();        // show the panel, focus the query, seed from selection

	// Developer tools — Dear ImGui's own inspectors + a "where is this feature's
	// code" source map (click a row -> project go-to-def to that function), so the
	// editor can be developed inside itself.
	bool devToolsVisible  = false;
	bool devShowMetrics   = false;
	bool devShowStackTool = false;
	bool devShowDebugLog  = false;
	bool devShowDemo      = false;
	void renderDevTools();

	// Project-wide Go to Definition — greps files under projectRoot (or the
	// active doc's directory) for definition patterns (class/struct/interface/
	// enum/record, method signatures, #define, etc.). Opens the first hit at
	// the matching line. Works for languages the in-file trie doesn't cover
	// (notably C# cross-file lookups).
	void goToDefinitionProjectWide(const std::string& word, bool declaration = false);
	void openCSharpLearn(const std::string& rawSymbol);   // C# SDK types -> Microsoft Learn docs (no on-disk source)

	// MSVC toolchain + Windows SDK include directories, discovered ourselves
	// (vswhere / registry / well-known paths) and cached, so system headers like
	// <vector> / <windows.h> resolve in "Go to File" regardless of how the editor
	// was launched (no Developer-Command-Prompt %INCLUDE% required).
	const std::vector<std::filesystem::path>& systemIncludeDirs();
	std::vector<std::filesystem::path> sysIncludeDirs_;
	bool sysIncludeComputed_ = false;

	// Recents — MRU lists of recently opened files and projects.
	// Capped at 20 entries each, persisted in settings.
	std::vector<std::string> recentFiles;
	std::vector<std::string> recentProjects;
	void rememberRecentFile(const std::string& path);
	void rememberRecentProject(const std::string& path);

	// Per-project tab sessions — remember the set of files that were open
	// when the user last had this project active. When they re-open the
	// project, the saved file list is restored. Persisted in settings.txt
	// under [project_sessions] as `<abs root>=<file1>|<file2>|...` lines.
	std::unordered_map<std::string, std::vector<std::string>> projectSessions;
	void saveCurrentProjectSession();      // snapshot of currently-open files
	void closeAllProjectTabs();            // close non-dirty, non-untitled tabs
	void restoreProjectSession(const std::filesystem::path& root);

	// First-run flag — used to suppress the demo doc on second+ launches
	// and on any --project launch.
	bool seenFirstRun = false;

public:
	// Set BEFORE constructing the Editor to suppress the on-first-run demo
	// doc (main.cpp does this when --project or positional files are passed).
	static bool sSkipDemo;
private:
};
