//	TextEditor - A syntax highlighting text editor for ImGui
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
#include <functional>
#include <memory>
#include <string>
#include <atomic>
#include <filesystem>
#include <future>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <imgui.h>
#include "../TextEditor.h"
#include "../TextDiff.h"
#include "tsindex.h"
#include "lsp_client.h"
#include "nav_history.h"
#include "updater.h"
#include "plugin_registry.h"


//
//	Editor
//

class Editor : public PluginHost {
	public:
	// constructor
	Editor();
	~Editor();   // stops the LSP client (joins its reader thread cleanly)

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

	// True once (then reset) when a forwarded single-instance open request asked
	// to surface the window — main.cpp raises the OS window. Public: main owns the
	// SDL window.
	bool consumeRaiseRequest() { bool r = wantRaiseWindow; wantRaiseWindow = false; return r; }

	// Per-project instance key (set once at startup by main.cpp). Instances are
	// keyed by project so a second launch coalesces into the window ALREADY on
	// that project, while a launch for a DIFFERENT project opens its own window.
	// The single-instance open inbox lives under <configDir>/open/<key>/, so
	// each instance only picks up requests routed to its project.
	void setInstanceKey(const std::string &k) { instanceKey = k; }

	// ── PluginHost — services exposed to in-process plugins (plugin_api.h) ──
	// Domain features (Unreal, UEVR/Blueprint) live in plugins and only touch
	// the editor through these; each delegates to an existing Editor facility.
	std::filesystem::path hostProjectRoot() const override { return projectRoot; }
	void hostSetProjectRoot(const std::string &path) override { setProjectRoot(path); }
	void hostOpenFile(const std::string &path) override { openFile(path); }
	void hostOpenLuaTab(const std::string &text) override { openLuaInNewTab(text); }
	std::string hostActiveText() const override { return tabs.empty() ? std::string() : doc().editor.GetText(); }
	std::string hostActiveSelection() const override {
		if (tabs.empty() || !doc().editor.AnyCursorHasSelection()) return {};
		return doc().editor.GetCurrentSelectionText();
	}
	void hostToast(const std::string &text) override { pushToast(text, IM_COL32(80, 160, 255, 255), 0); }
	void hostError(const std::string &message) override { showError(message); }
	void hostSendToClaude(const std::string &message) override { writeToastReply(message); }
	void hostSuppressAppShortcuts() override { appShortcutsSuppressed = true; }
	void hostRunInDir(const std::string &command, const std::filesystem::path &dir) override { runCommandInOutputPanel(command, dir); }
	void hostRunProjectBuild() override { runProjectBuild(); }
	std::filesystem::path hostExeDir() const override;                 // exe's directory (get_module_path)
	std::filesystem::path hostRepoRoot() const override { return findSelfRepoRoot(); }
	bool hostPanInverted() const override { return prefInvertPan; }
	void hostMiddleMousePanScroll(int windowKey) override { middleMousePanScroll(windowKey); }
	void hostAugmentCppLanguage(const std::vector<std::string> &types,
	                            const std::vector<std::string> &keywords,
	                            bool (*isTypeLike)(const std::string &)) override;
	bool hostGetFlag(const std::string &key, bool def) const override {
		auto it = pluginFlags.find(key);
		return it == pluginFlags.end() ? def : it->second;
	}
	void hostSetFlag(const std::string &key, bool value) override { pluginFlags[key] = value; }

	private:
	// In-process plugins compiled into this build + their persisted key->bool
	// store. registerBuiltinPlugins() fills it in the ctor; hooks fan out at the
	// extension points. Empty in a core build (all IMGUIIDE_PLUGIN_* off).
	PluginRegistry pluginRegistry;
	std::unordered_map<std::string, bool> pluginFlags;
	// Set for one frame by hostSuppressAppShortcuts() when a focused plugin window
	// (e.g. the Blueprint editor) is handling its own Ctrl+Z/C/V; gates the app-level
	// keyboard shortcut dispatch so the keys route to the plugin, not the document.
	bool appShortcutsSuppressed = false;

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
		// External-change watch (safe co-editing with Claude / other tools):
		//   diskMtime    = last on-disk write time we've reconciled with
		//   externalChange = disk changed under a DIRTY buffer (unresolved conflict)
		std::filesystem::file_time_type diskMtime{};
		bool        externalChange = false;
		bool        externallyTouched = false;   // edited on disk since you last viewed this tab (badge)
		bool        externalMarkers   = false;   // gutter markers on externally-changed lines are live
		bool        largeFile         = false;   // >8 MB: whole-doc intelligence (trie/LSP/folds/brackets) disabled
		std::vector<std::pair<int,int>> changedRanges; // inclusive 0-based line ranges Claude/an external tool changed
		std::string syncedText;                  // last persisted content (load/save/reload) = 3-way merge base
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

	// External-change watch — keeps open docs in sync when Claude or another
	// tool edits the same files on disk. recordDiskMtime() re-baselines after
	// our own load/save; checkExternalChanges() polls (throttled) and either
	// silently reloads a clean buffer or flags a conflict on a dirty one;
	// reloadFromDisk() re-reads, preserving a clamped cursor.
	void recordDiskMtime(TabDocument& t);
	void reloadFromDisk(TabDocument& t, bool quiet = false); // quiet: no change markers (log/non-code)
	static bool isCodeExtension(const std::string& extLower); // source code vs log/data/text
	void checkExternalChanges();
	void markChangedLines(TabDocument& t, const std::string& oldText); // diff oldText vs the doc's current text
	void clearChangeMarks(TabDocument& t);   // drop gutter markers + reply-dot decorator + ranges together
	double extWatchTime = 0.0;   // last poll time (throttle)

	// Frame-time watchdog — flags potential perf issues. Measures the wall-clock
	// cost of building one frame's UI (the thing that historically tanked fps,
	// e.g. per-keystroke fold rebuilds). Publishes a rolling 3s worst + a running
	// count of frames over the 30fps budget; logs slow frames to stderr.
	float  fpsWorstMs       = 0.0f;
	int    fpsSlowCount     = 0;
	double fpsWindowStart   = 0.0;
	float  fpsWindowWorstMs = 0.0f;
	void   updateFpsWatch(double renderMs);

	// Transient corner notifications — used to make external edits loud. Each
	// toast fades out after `expiry`. pushToast() enqueues; renderToasts()
	// draws them stacked over the main viewport.
	// action: 0 = clicking writes the text to the reply outbox (feedback bridge),
	// 1 = clicking opens the Update dialog. Toasts are click-to-act + click-to-dismiss.
	struct Toast { std::string text; double expiry; ImU32 accent; int action = 0; };
	std::vector<Toast> toasts;
	void pushToast(const std::string& text, ImU32 accent, int action = 0);
	void renderToasts();
	void writeToastReply(const std::string& text);   // -> <configDir>/replies/* (bridge outbox)

	// Single-instance open inbox: a second launch for THIS project writes its
	// project/files to <configDir>/open/<instanceKey>/; the instance owning that
	// project polls + opens them here. Sets wantRaiseWindow so main.cpp surfaces
	// the window.
	void pollOpenInbox();
	bool wantRaiseWindow = false;
	std::string instanceKey = "none"; // per-project routing key (main.cpp sets it)

	// "Reply to Claude" feedback popup: type a message about a Claude/external change
	// (or a toast) and either send it now or queue it for batch submission. All replies
	// land in the <configDir>/replies/* outbox the bridge/CLI tails.
	void requestReply(const std::string& file, int line0, const std::string& contextLabel);
	void renderReplyPopup();
	void submitReply(const char* message, bool immediate); // immediate=Send now, else queue for batch
	void flushReplyBatch();
	bool lineIsChanged(const TabDocument& t, int line) const;
	bool replyPopupRequested = false;
	std::string replyContextLabel;             // shown at the top of the popup
	std::string replyTargetFile;               // file the reply is about ("" = general/toast)
	int replyTargetLine = -1;                  // 0-based line, or -1
	std::vector<std::string> replyBatch;       // queued comments awaiting batch submission
	char replyBuf[2048] = {};                  // popup text buffer
	// External toast API: any process drops a text file in <configDir>/toasts/ and
	// it shows as a toast here (optional "info|warn|error|success" severity prefix,
	// then the message). Polled (throttled) once per frame; files are consumed.
	void pollToastInbox();

	// ── In-app updater (GitHub Releases via WinHTTP) ──────────────────────────
	// Checks <owner>/<repo>'s latest release against kAppVersion. Auto every 12 h
	// (when prefAutoUpdate), or on demand from Help > Check for Updates. The check
	// + download run on a worker (std::async); pollUpdates() drains them each frame.
#ifndef IMGUI_IDE_VERSION
#define IMGUI_IDE_VERSION "0.0.0-dev"   // overridden by CMake (git describe) for the example target
#endif
	static constexpr const char* kAppVersion = IMGUI_IDE_VERSION;
	int    prefUpdateChannel = 0;   // 0 = stable (latest release), 1 = nightly (newest prerelease)
	bool   prefAutoUpdate        = true;   // background 12 h check
	long long lastUpdateCheckEpoch = 0;    // wall-clock (time_t) of last check; persisted
	std::future<updater::Release> updateFuture;
	bool   updateCheckManual     = false;  // the in-flight check was user-initiated
	updater::Release updateInfo;           // last result
	bool   updateAvailable       = false;  // updateInfo.tag is newer than kAppVersion
	bool   showUpdateDialog      = false;
	std::future<bool> updateDownloadFuture;
	std::string updateDownloadPath;
	int    updateDownloadState   = 0;      // 0 idle, 1 downloading, 2 done, 3 failed
	void   checkForUpdates(bool manual);
	void   pollUpdates();
	void   renderUpdateDialog();

	// ── Focus mode ────────────────────────────────────────────────────────────
	// Distraction-free + cheaper-to-render: hides every side panel (nav, symbols,
	// references, find-in-files, output, dev tools, external changes, md preview)
	// and the editor minimaps, leaving just the document panes. Toggling restores
	// the previous panel layout. F11 / View menu, or --focus at launch.
	bool focusMode = false;
	struct FocusSnapshot { bool nav, sym, refs, fif, output, dev, ext, md; } focusSnap{};
public:
	void toggleFocusMode();
	void setFocusMode(bool on);   // public so main.cpp can honor --focus at launch
private:

	// Effective nav/browse root: projectRoot if set, else the active document's
	// folder, else empty. Never the process CWD — a shell/Explorer launch can put
	// that at C:\Windows\System32, and walking it freezes the nav.
	std::filesystem::path workspaceRoot() const;

	// ── Autosave ──────────────────────────────────────────────────────────────
	// Periodically writes dirty documents that already have a path (skips
	// "untitled" — those need a Save-As). Off by default; interval persisted.
	bool   prefAutoSave    = false;
	int    prefAutoSaveSec = 30;
	double lastAutoSave    = 0.0;   // ImGui::GetTime() of the last sweep
	void   autoSaveTick();
	int    saveDirtyTitledDocs();   // write all dirty on-disk docs; returns count saved

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
	// (Re)evaluate large-file mode for `bytes` of content and apply/lift the
	// intelligence gates on transition. Called on open, reload, and merge — the
	// file's size can change under us (review finding: the flag was set once).
	void updateLargeFileMode(TabDocument& t, size_t bytes);
	void configureTabAutocomplete(TabDocument& t);   // wire autocomplete + build trie for ONE tab

	static const TextEditor::Language* languageForPath(const std::string& path);
	// Resolve a language by display name ("C++", "Lua", a runtime lang's name, or
	// "None"/empty -> nullptr). Used to apply persisted file-type associations.
	static const TextEditor::Language* languageByName(const std::string& name);
	// Persistent ".ext" -> language display-name associations (Settings [filetypes]),
	// set from the status-bar language picker; languageForPath consults these first.
	static std::unordered_map<std::string, std::string>& extLanguageOverrides();

public:
	// Set the navigation panel's project root and show it. Called from
	// main.cpp's --project arg path and from the Open Project dialog.
	void setProjectRoot(const std::filesystem::path& p);

	// Persistent settings root — same absolute location every run regardless
	// of cwd, so layout / favourites / etc. actually round-trip.
	//   Windows: %APPDATA%\ImGuiColorTextEdit
	//   POSIX:   $XDG_CONFIG_HOME/imguicolortext  (or  $HOME/.config/...)
	// The directory is created on first call. Public so main.cpp can route
	// the ImGui ini path through it too.
	static std::filesystem::path userConfigDir();

	// Frame-pacing prefs, read by main.cpp each loop iteration.
	int  fpsLimit() const       { return prefFpsLimit; }
	bool idleThrottle() const   { return prefIdleThrottle; }

	// Themes — public so main.cpp can set the default before the Editor exists.
	static int  themeCount();
	static const char* themeName(int index);
	static void applyTheme(int index);      // sets ImGui style colors + rounding
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
	struct TsDef   { std::string file; int line; ts::Kind kind; };   // accurate, kind-ranked
	struct ProjectIndex {
		std::vector<std::string>                              identifiers; // sorted unique
		std::unordered_map<std::string, std::vector<DefSite>> defs;        // symbol -> sites (heuristic)
		std::unordered_map<std::string, std::vector<TsDef>>   tsDefs;      // symbol -> tree-sitter sites
		std::unordered_map<std::string, std::vector<std::string>> members; // type name -> member names
		// Per-file symbol lists (tree-sitter). Backs the Symbols panel's Project
		// tree (file -> types -> members) and the on-disk cache.
		std::unordered_map<std::string, std::vector<ts::Symbol>> fileSymbols;
		// type -> member -> member's type, merged project-wide. Lets member-chain
		// completion (a.b.c) hop into types defined in other files.
		ts::MemberTypeMap memberTypes;
	};
	struct IndexState {
		std::mutex                          mutex;
		std::shared_ptr<const ProjectIndex> index;
		std::atomic<bool>                   building{false};
		std::atomic<int>                    gen{0};
		std::atomic<bool>                   rebuildRequested{false}; // a save arrived mid-build
		// Last-built per-file symbol cache (also persisted to disk). Read at the
		// start of a build to skip re-parsing unchanged files; guarded by `mutex`.
		std::unordered_map<std::string, ts::FileSyms> cache;
	};
	std::shared_ptr<IndexState> indexState = std::make_shared<IndexState>();
	void rebuildProjectIndex();                              // spawn background build
	std::string indexCachePath() const;                      // %APPDATA% cache file for projectRoot
	void loadIndexCache();                                   // load cache + publish it as the initial index
	std::shared_ptr<const ProjectIndex> indexSnapshot();     // thread-safe read
	TextEditor::Trie projectTrie;        // shared project-wide identifier trie (one copy, not per-tab)
	int projectTrieGen = -1;             // index gen projectTrie was built from (-1 = never)
	void buildProjectTrie();             // (re)build projectTrie from the current index snapshot
	// F6: walk up from the active doc to find a project build script
	// (build.bat / build.ps1 / build.sh / Makefile / CMakeLists.txt) and run it.
	void runProjectBuild();
	// Resolve {path, command} for the project rooted at/above `start`: build
	// scripts, .sln/.csproj/.vcxproj, CMakeLists, or a plugin project type
	// (e.g. Unreal). path may be a script file or a directory to cd into.
	std::pair<std::filesystem::path, std::string> findProjectBuildCommand(std::filesystem::path start);

	// ── LSP (clangd) — real intellisense for C/C++; tree-sitter stays the
	// instant fallback + Symbols outline. Off unless clangd is found.
	lsp::LspClient lspClient;
	std::string    clangdPath;              // resolved clangd.exe ("" = none)
	bool           lspEnabled = true;       // user toggle (View menu)
	std::unordered_map<std::size_t, std::size_t> lspDocHash;   // tab id -> last-synced GetText() hash
	int            lspCompletionId = 0;     // latest outstanding completion request id
	const TabDocument* lspCompletionTab = nullptr;     // tab that completion was fired for
	int            lspDefinitionId = 0;     // latest outstanding definition request id
	int            lspDefLine = -1, lspDefCol = -1;     // cursor snapshot at def request (staleness)
	int            lspHoverId = 0;          // latest outstanding hover request id
	std::string    lspHoverText;            // clangd hover text for the current hover word
	std::unordered_map<std::string, std::vector<lsp::Diagnostic>> lspDiagnostics;  // file uri -> diagnostics
	void        detectClangd();             // locate clangd (VS -> PATH -> LLVM)
	void        startLspForProject();       // (re)start the client for projectRoot
	void        pollLsp();                  // per-frame: drain results, refine popup / navigate
	void        lspSyncDoc(TabDocument& t); // didOpen / didChange as needed
	bool        lspActiveForExt(const std::string& ext) const;   // C/C++ + enabled + ready
	std::string lspUriForTab(const TabDocument& t) const;

	// Navigation history (back/forward after Go-to-Definition). The origin is
	// captured when a go-to-def is invoked and committed to the stack at the
	// first actual jump (sync or async LSP), so back returns to where you were.
	NavHistory  navHistory;
	NavLocation navJumpOrigin;              // pending origin for the in-flight go-to-def
	bool        navJumpOriginValid = false; // origin set, not yet committed to history
	NavLocation currentNavLocation() const; // active tab's file + cursor (invalid if none)
	void        commitPendingNavJump();     // push the pending origin to history (once)
	void        applyNavLocation(const NavLocation& l);  // openFile + cursor + scroll
	void        navigateBack();             // return to the previous location
	void        navigateForward();          // re-apply a location we backed out of
	bool        showAboutDialog = false;    // Help > About ImGui-IDE popup request

	// Theme system: a few built-in palettes + the live ImGui style editor. The
	// chosen theme persists (prefTheme) and is applied after settings load + on
	// every menu pick. applyTheme is static so main.cpp can set the default before
	// the Editor exists.
	int         prefTheme = 0;              // index into the theme table
	bool        showStyleEditor = false;    // View > Style Editor window toggle

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
	// Root dockspace ID, captured in the ##EditorHost window's ID-stack context.
	// MUST be reused (not recomputed via GetID) by menu/keybind handlers — GetID is
	// ID-stack-relative, so calling it from a menu popup yields a DIFFERENT id and
	// the reset/remerge would rebuild a phantom node, leaving windows adrift.
	unsigned int          mainDockId = 0;
	bool                  wantResetLayout = false;     // deferred: rebuild layout before next DockSpace()
	std::filesystem::path projectRoot;            // empty = none set; default to cwd

	// Nav tree context menu state — set when a file/folder is right-clicked.
	std::string  navContextPath;            // path the menu acts on this frame
	bool         navShowDotFiles = false;   // toggle in nav header
	bool         navShowExcluded = false;   // expose hidden-from-view items
	bool         navCodeOnly     = false;   // hide non-source files
	bool         navFlatFiles    = false;   // collapse folder bodies (show folders only)
	bool         navPathWrap     = false;   // project-path display: wrap (true) vs right-align+truncate (false)
	bool         navShowUeSource = true;    // UE projects: show the engine Source tree (collapsed; lazy)
	int          navSetAllOpen   = -1;      // one-shot bulk tree open/close: -1 none, 0 collapse all, 1 expand all

	// Memo for the plugin-provided extra nav source root (e.g. a UE project's
	// engine Source/ tree) — resolving it walks the filesystem, so cache it per
	// workspace root. The label comes from the contributing plugin.
	std::string  ueSourceKey;               // workspace root the cache was computed for
	std::string  ueSourceDir;               // resolved extra source root ("" = none for this project)
	std::string  ueSourceLabel;             // plugin's label for it (e.g. "Unreal Engine Source")
	std::unordered_map<std::string, bool> navExcluded; // abs path → true = hidden in tree
	std::unordered_set<std::string> navSelected;       // canonical abs paths multi-selected in the tree
	std::string  navClipboardPath;          // last "Copy" target — used by Paste
	bool         navClipboardIsCut = false; // true = move on paste, false = copy
	std::string  navRenameTarget;           // path currently being renamed in-place
	char         navRenameBuf[256] = {0};   // edit buffer for the rename input
	char         navFilterBuf[128] = {0};   // "filter by name" box (case-insensitive substring)
	mutable std::unordered_set<std::string> navMatchDirs;   // dirs with a descendant matching the filter
	mutable std::string navMatchFilterCached;               // filter text navMatchDirs was built for
	std::string navSelAnchor;                  // shift-click range anchor (canon key)
	std::string navRangeTarget;                // pending shift-click target, applied at end of frame
	std::vector<std::string> navVisibleOrder;  // entries in render order this frame (for range select)
	std::string  navPendingDelete;          // path queued for confirm-delete modal
	std::string  navDragSource;             // payload for the move/copy drag-source
	// Nav directory-listing cache (see navCachedDir/navCachedFlatList below).
	struct NavDirListing { std::vector<std::filesystem::directory_entry> entries; double scanned = -1.0; };
	struct NavFlatItem { std::filesystem::path path; std::string name; };
	std::unordered_map<std::string, NavDirListing> navDirCache;  // tree view: per-dir, sorted
	std::vector<NavFlatItem> navFlatCache;  // flat view: whole-tree file list, sorted, unfiltered
	std::string  navFlatCacheRoot;          // root the flat cache was built for
	bool         navFlatCacheDot = false;   // showDot state the flat cache was built for
	double       navFlatCacheTime = -1.0;   // ImGui::GetTime() of the last flat rebuild
	bool         navListDirty = false;      // an edit invalidated the listings; rebuild next render
	void         navOpenPathInExplorer(const std::string& path);
	void         navInitDockLayout(unsigned int dockId);   // first-frame split layout
	void         navDeletePath(const std::string& p);      // recycle bin (Win) / fallback
public:
	bool         navIsCodeFile(const std::filesystem::path& p) const;
	void         navOpenExternally(const std::string& path); // for non-text non-image
	bool         navIsExcluded(const std::filesystem::path& p) const;
	bool         navFilterActive() const { return navFilterBuf[0] != 0; }
	bool         navNameMatches(const std::string& name) const;           // case-insensitive substring of the filter
	bool         navDirHasMatch(const std::filesystem::path& dir) const;  // any descendant matches the filter (bounded)
	bool         navIsSelected(const std::filesystem::path& p) const;     // multi-select state
	void         navToggleSelected(const std::filesystem::path& p);       // ctrl-click
	void         navSetOnlySelected(const std::filesystem::path& p);      // plain click / right-click
	void         navTrackVisible(const std::filesystem::path& p);         // record render order (range select)
	void         navSetAnchor(const std::filesystem::path& p);            // remember the range anchor
	void         navRangeRequestTo(const std::filesystem::path& p);       // shift-click → range to here
	void         navApplyRangeSelect();                                   // resolve a pending range (end of frame)
	bool         navIsCodeOnly() const { return navCodeOnly; }
	bool         navIsFlat() const     { return navFlatFiles; }
	int          navBulkOpenRequest() const { return navSetAllOpen; }   // -1 none / 0 collapse / 1 expand
	bool         navIsShowingExcluded() const { return navShowExcluded; }
	void         navDragSourceSet(const std::string& p) { navDragSource = p; }
	void         navMoveOrCopy(const std::string& src,
	                           const std::string& destDir, bool copy);
	// Cached directory listings — the nav-render helpers call these instead of
	// walking the filesystem every frame (the panel's dominant cost). A short TTL
	// refreshes them; navMarkListDirty() forces an immediate rebuild after the
	// nav's own edits (rename/move/delete) so the change shows next frame.
	const std::vector<std::filesystem::directory_entry>& navCachedDir(const std::filesystem::path& dir);
	const std::vector<NavFlatItem>& navCachedFlatList(const std::filesystem::path& root, bool showDot);
	void         navMarkListDirty() { navListDirty = true; }
	// Public so the static nav-render helpers (navRenderEntry/navRenderFlat) can
	// draw an image thumbnail in a file's hover tooltip.
	static bool  isImageExt(const std::string& ext);
	static bool  isMarkdownExt(const std::string& ext);
	void         navShowImageThumbnail(const std::string& path);
private:

	// Split-right command (Ctrl+\): dock the next-active doc into a fresh
	// split node to the right of the current one. Builds via DockBuilder.
	void splitActiveTabRight();
	bool wantSplitRight = false;
	bool wantSplitLeft  = false;   // same, to the left (from the tab context menu)

	// Per-tab context menu (right-click a document tab). tabCtxIdx is the tab
	// the menu targets; detection uses the docked tab's screen rect.
	void renderTabContextMenu(int idx);
	int  tabCtxIdx = -1;
	int  countDocNodes() const;     // distinct dock nodes hosting document tabs (split limit)

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
	bool keybindCapturing = false;   // Settings keybind capture is listening — block app shortcuts
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
	bool   prefInvertPan       = true;    // inverted ("grab the content") pan is the default
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
	// Locate a project virtualenv's python for a script (VIRTUAL_ENV, or a
	// .venv/venv/env dir walking up from the script / project root). Empty = none.
	std::string venvPythonFor(const std::filesystem::path& scriptPath) const;
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

	// PDF viewer — pages rendered lazily (visible pages only) by the OS PDF engine
	// (pdfview.*) into GPU textures; an LRU cap keeps a long document from
	// ballooning VRAM. Same texture lifecycle as ImageDoc.
	struct PdfDoc {
		std::string  path;
		std::string  windowTitle;
		bool         open = true;
		bool         wantFocus = false;
		bool         fitted = false;
		float        zoom = 1.0f;
		float        renderScale = 1.5f;      // texture supersampling vs natural 96-dpi size
		int          pageCount = 0;
		std::string  error;                   // load error (shown in-window)
		std::vector<std::pair<float,float>> pageSizes; // natural DIP sizes (placeholders)
		struct PageTex {
			ImTextureData* tex = nullptr;
			int    w = 0, h = 0;
			bool   failed = false;            // render failed — don't retry every frame
			double lastVisible = 0.0;         // for LRU eviction
		};
		std::vector<PageTex> pages;
	};
	std::vector<std::unique_ptr<PdfDoc>> pdfs;
	void openPdfFile(const std::string& path);
	void renderPdfWindows();

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

	// Same memo pattern for log-file references: "path(123)" / "path:123" /
	// UE "[File:...] [Line: 123]" on the current line resolve to a project file
	// + line so crash logs jump straight to code.
	std::string  ctxLogKey;
	std::string  ctxLogFile;     // resolved absolute path ("" = unresolved)
	std::string  ctxLogLabel;    // menu label (file(line))
	int          ctxLogLine = 0; // 1-based

	// Same memo pattern for C++ definition/declaration generation. The class
	// scan reads whole-file text, so recompute only when (doc|line|lineCount|
	// undoIndex) change rather than every frame the popup is open.
	std::string  ctxGenKey;
	std::string  ctxGenClass;    // enclosing class name ("" = not in a class)
	std::string  ctxGenOneDef;   // single-member definition stub ("" = N/A)
	std::string  ctxGenAllDefs;  // all undefined members' stubs ("" = none)
	std::string  ctxGenDecl;     // in-class declaration from an out-of-line def

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
	// Apply a .editorconfig cascade (file dir upward, stop at root=true) to a tab:
	// indent_style / indent_size / tab_width override the editor defaults. Standard
	// per-project + per-language settings hierarchy (interops with VS / VSCode).
	void applyEditorConfig(TabDocument& t);
	void renderReferencesPanel();

	// Symbol / definition browser (View → Symbols). Two modes the user toggles:
	//  - Document: live outline of the active file (tree-sitter parse, cached by
	//    filename + edit count so it re-parses only on change).
	//  - Project: every indexed symbol + index status — "monitor the index".
	bool                            symbolsPanelVisible = false;
	bool                            symbolsProjectMode  = false;   // false = document outline
	char                            symbolsFilter[128]  = {};
	void renderSymbolsPanel();
	std::string                     symbolsCacheFile;              // doc-mode cache key (filename)
	size_t                          symbolsCacheUndo = (size_t) -1;// doc-mode cache key (edit count)
	std::vector<ts::Symbol>         symbolsCacheSyms;              // parsed outline of the active doc
	struct SymRow { std::string name; std::string file; int line; ts::Kind kind; std::string lname; };
	std::vector<SymRow>             symbolsProjectRows;            // flattened project index (filtered flat view)
	std::vector<std::string>        symbolsFiles;                  // sorted file paths (project tree view)
	int                             symbolsProjectGen = -1;        // index gen the two caches above are from
	std::string                     symbolsFilterCache = std::string(1, '\x01'); // filter the matched-index cache is for (sentinel forces first build)
	std::vector<int>                symbolsFilteredIdx;            // indices into symbolsProjectRows matching symbolsFilterCache

	// Open arbitrary text as a new, never-saved Lua editor tab. A host facility
	// (PluginHost::hostOpenLuaTab) used by the UEVR plugin's "Generate UEVR Lua"
	// and available to any codegen. Sets Lua language so highlighting +
	// UEVR-API autocomplete apply even though it is untitled; Ctrl+S therefore
	// prompts Save As.
	void openLuaInNewTab(const std::string &text);

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

	// Markdown preview — renders the active .md document (headings, lists, code
	// fences, blockquotes, rules, and inline bold/italic/code/links).
	bool mdPreviewVisible = false;
	void renderMarkdownPreview();
	std::string mdCacheText;            // cached GetText() for the preview (perf)
	std::string mdCacheKey;            // filename + undo index the cache was built for
	bool        mdPreviewWantDock = false;   // one-shot: dock the preview beside the doc
	bool        mdPreviewWasVisible = false; // edge-detect the open transition
	void renderMarkdownInline(const std::string& text, float wrapWidth);  // word-wrap + inline styles

	// External-changes feed — a persistent, dockable record of external (another
	// tool / Claude) edits to open files. Toasts are transient and easy to miss
	// while away; this log keeps the history so it's easy to track what changed.
	struct ExtChange { std::string file; std::string path; std::string kind; double time; };
	std::vector<ExtChange> extChangeLog;
	bool externalChangesVisible = false;
	void logExternalChange(const std::string& path, const std::string& kind);
	void renderExternalChanges();
	void mergeExternalChange(TabDocument& t);   // 3-way merge buffer + disk over the synced base

	// Git status (background-polled) — branch + dirty/ahead/behind for the status
	// bar. Read-only; destructive git actions (commit/push/revert) come later.
	struct GitInfo {
		std::mutex          mutex;
		std::atomic<bool>   building{ false };
		std::string         branch;
		int                 dirty = 0, ahead = 0, behind = 0;
	};
	std::shared_ptr<GitInfo> gitInfo = std::make_shared<GitInfo>();
	std::string gitPollRoot;
	double      gitPollTime = -1000.0;
	void        pollGitStatus();
	// Git actions (shell out to `git`, output in the Output panel). Commit opens a
	// message dialog; Discard confirms first (destructive).
	std::string gitRoot();
	void        runGit(const std::string& args);
	bool        gitCommitRequest  = false;
	char        gitCommitMsg[1024] = {};
	bool        gitDiscardRequest = false;
	bool        gitCloneRequest   = false;
	char        gitCloneUrl[512]  = {};
	char        gitCloneDir[512]  = {};   // parent directory to clone into
	bool        gitRevCompareRequest = false;
	char        gitRevBuf[128]    = "HEAD";   // revision to compare the active file against
	void        renderGitDialogs();   // commit / discard / clone / revision-compare modals
	void        cloneRepository(const std::string& url, const std::string& parentDir);
	void        compareActiveFileWithRevision(const std::string& rev);
	// After an async clone, rebuild the index once the repo's .git appears so
	// go-to-def / symbols populate without a manual Rebuild.
	std::string pendingCloneRoot;
	double      pendingCloneSince = 0.0;
	void        pollCloneCompletion();
	// Detect the IDE's own source repo (walks up from the exe / cwd looking for
	// example/editor.cpp). Used by Dev Tools to auto-open it as the project.
	std::filesystem::path findSelfRepoRoot() const;

	// Project-wide Go to Definition — greps files under projectRoot (or the
	// active doc's directory) for definition patterns (class/struct/interface/
	// enum/record, method signatures, #define, etc.). Opens the first hit at
	// the matching line. Works for languages the in-file trie doesn't cover
	// (notably C# cross-file lookups).
	void goToDefinitionProjectWide(const std::string& word, bool declaration = false);
	// Tree-sitter-accurate definition jump for the active doc's language (C++/C#).
	// Returns true if it resolved + navigated; false → fall through to grep.
	bool tsGoToDefinition(const std::string& symbol);
	bool tsGoToDefinitionInDoc(const std::string& symbol);   // live current-doc parse, index-free
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
