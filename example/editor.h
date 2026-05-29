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
private:

	// Split-right command (Ctrl+\): dock the next-active doc into a fresh
	// split node to the right of the current one. Builds via DockBuilder.
	void splitActiveTabRight();
	bool wantSplitRight = false;

	// Settings dialog — interpreters per extension, build overrides,
	// editor toggles. Persisted to <configDir>/settings.json (minimal
	// hand-rolled writer; no JSON dep).
	bool settingsVisible = false;
	std::unordered_map<std::string, std::string> interpreterOverrides; // ".py" → "python"
	std::unordered_map<std::string, std::string> projectBuildOverrides; // abs root → cmd
	bool   prefAutoIndent      = true;
	bool   prefCompletePairs   = true;
	bool   prefShowFps         = false;   // FPS readout on the status bar
	bool   prefCtrlScrollZoom  = true;    // Ctrl+wheel adjusts editor font size
	bool   prefInvertPan       = false;   // flip middle-mouse pan direction
	bool   prefWordWrap        = false;   // soft-wrap long lines
	int    prefWrapWidthPx     = 0;       // 0 = wrap to view width, else fixed px
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

	// Smart F5: locate the most-recent built exe under projectRoot's
	// common build dirs (out/build/*, build/, bin/{Debug,Release}/) and
	// run it. Returns empty if none found.
	std::filesystem::path findBuiltExe() const;
	void        runProjectExeOrScript();   // F5: prefer exe, fall back to script

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
	static bool isImageExt(const std::string& ext);
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

	// Find References results panel — project-wide. Each hit records the file
	// it was found in so clicking opens that file and jumps to the line.
	struct RefHit { std::string file; int line; std::string text; };
	bool                            referencesVisible = false;
	std::string                     referencesWord;
	std::vector<RefHit>             referencesHits;
	int                             referencesFileCount = 0;
	void findReferencesOf(TabDocument& t, const std::string& word);
	void renderReferencesPanel();

	// Project-wide Go to Definition — greps files under projectRoot (or the
	// active doc's directory) for definition patterns (class/struct/interface/
	// enum/record, method signatures, #define, etc.). Opens the first hit at
	// the matching line. Works for languages the in-file trie doesn't cover
	// (notably C# cross-file lookups).
	void goToDefinitionProjectWide(const std::string& word, bool declaration = false);

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
