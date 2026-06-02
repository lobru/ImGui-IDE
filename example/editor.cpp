//	TextEditor - A syntax highlighting text editor for ImGui
//	Copyright (c) 2024-2026 Johan A. Goossens. All rights reserved.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


//
//	Include files
//
#define _CRT_SECURE_NO_WARNINGS  // for std::getenv used in #include resolution
#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>   // SHFileOperation for recycle-bin delete
#include <process.h>    // _popen / _pclose
#endif
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "ImGuiFileDialog.h"
#include "imgui.h"
#include "imgui_internal.h"

// stb_image — define our own static-linkage copy in this translation unit so
// the image viewer can load PNG/JPG/etc. (ImGuiFileDialog also embeds it, but
// gates exports inconsistently — easier to just have our own private copy.)
// Suppress C4505 (unreferenced static functions) — stb_image has many.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#pragma warning(push)
#pragma warning(disable: 4505)
#include "stb/stb_image.h"
#pragma warning(pop)

#include "editor.h"

#include <chrono>

namespace {
// Lightweight scoped timer. Logs to stderr only when a block exceeds 1ms, so
// it is silent in the common case and surfaces real stalls. Visible in a
// terminal (AttachConsole). Remove the threshold to log everything.
struct ScopedTimer {
	const char* name;
	std::chrono::steady_clock::time_point t0;
	explicit ScopedTimer(const char* n) : name(n), t0(std::chrono::steady_clock::now()) {}
	~ScopedTimer() {
		double ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - t0).count();
		if (ms > 1.0) std::fprintf(stderr, "[perf] %s: %.1f ms\n", name, ms);
	}
};
} // namespace


// Forward decls so callers above the definitions (e.g. Editor::tryToQuit)
// can persist favourites at any quit/teardown path.
static void saveFileDialogPlaces();
static void populateFileDialogPlaces();
static std::filesystem::path resolveOutermostRoot(std::filesystem::path start);


//
//	Constants
//

#if __APPLE__
#define SHORTCUT "Cmd-"
#else
#define SHORTCUT "Ctrl-"
#endif

static const char* demo =
"// Demo C++ Code\n"
"\n"
"#include <iostream>\n"
"#include <random>\n"
"#include <vector>\n"
"\n"
"int main(int, char**) {\n"
"	std::random_device rd;\n"
"	std::mt19937 gen(rd());\n"
"	std::uniform_int_distribution<> distrib(0, 1000);\n"
"	std::vector<int> numbers;\n"
"\n"
"	for (auto i = 0; i < 100; i++) {\n"
"		numbers.emplace_back(distrib(gen));\n"
"	}\n"
"\n"
"	for (auto n : numbers) {\n"
"		std::cout << n << std::endl;\n"
"	}\n"
"\n"
"	return 0;\n"
"}\n";


//
//	Editor::languageForPath
//

std::unordered_map<std::string, const TextEditor::Language*>& Editor::runtimeLanguagesByExt()
{
	static std::unordered_map<std::string, const TextEditor::Language*> map;
	return map;
}

static std::filesystem::path get_module_path()
{
	char buf[256]{};
	if (auto d = GetModuleFileNameA(nullptr, buf, sizeof(buf)))
	{
		return std::filesystem::path(buf);
	}
	return std::filesystem::current_path();
}

void Editor::loadRuntimeLanguages()
{
	auto& byExt = runtimeLanguagesByExt();
	if (!byExt.empty()) return;  // already loaded
	// Try a few candidate roots: cwd/languages, exe-dir/languages, ../languages
	std::vector<std::filesystem::path> roots = {
		get_module_path() / "languages",
		std::filesystem::current_path() / "languages",
		std::filesystem::current_path() / ".." / "languages",
		std::filesystem::current_path() / ".." / ".." / "languages",
		std::filesystem::current_path() / ".." / ".." / ".." / "example" / "languages",
	};
	for (const auto& root : roots)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(root, ec)) continue;
		for (const auto& entry : std::filesystem::directory_iterator(root, ec))
		{
			if (!entry.is_regular_file()) continue;
			auto ext = entry.path().extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(),
						   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (ext != ".lang") continue;
			if (auto* lang = TextEditor::Language::FromFile(entry.path().string()))
			{
				for (const auto& e : lang->extensions) byExt[e] = lang;
			}
		}
		if (!byExt.empty()) return;  // stop at first non-empty hit
	}
}


const TextEditor::Language* Editor::languageForPath(const std::string& path)
{
	auto ext = std::filesystem::path(path).extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(),
				   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	// Filename-based matches (extensionless files: CMakeLists.txt, Dockerfile, …)
	auto fname = std::filesystem::path(path).filename().string();
	std::string fnameLower = fname;
	std::transform(fnameLower.begin(), fnameLower.end(), fnameLower.begin(),
				   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (fnameLower == "cmakelists.txt")
	{
		auto& byExt = runtimeLanguagesByExt();
		auto it = byExt.find(".cmake");
		if (it != byExt.end()) return it->second;
	}

	// Built-in matches take precedence; fall back to user-loaded definitions.
	// `.h` maps to C++ (a superset of C): C code still highlights correctly, but
	// C++ headers stop losing class/namespace/template highlighting. Only `.c`
	// stays pure C.
	if (ext == ".c")                                                         return TextEditor::Language::C();
	if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
		ext == ".h"   || ext == ".hpp" || ext == ".hxx" || ext == ".inl")     return TextEditor::Language::Cpp();
	if (ext == ".cs")                                                         return TextEditor::Language::Cs();
	if (ext == ".as")                                                         return TextEditor::Language::AngelScript();
	if (ext == ".lua")                                                        return TextEditor::Language::Lua();
	if (ext == ".py" || ext == ".pyw")                                                         return TextEditor::Language::Python();
	if (ext == ".glsl" || ext == ".vert" || ext == ".frag" ||
		ext == ".geom" || ext == ".comp" || ext == ".tesc" || ext == ".tese") return TextEditor::Language::Glsl();
	if (ext == ".hlsl" || ext == ".fx" || ext == ".fxh" || ext == ".addonfx")                     return TextEditor::Language::Hlsl();
	if (ext == ".json" || ext == ".jsonl" || ext == ".uplugin" || ext == ".uproject" || ext == ".gltf")                                                       return TextEditor::Language::Json();
	if (ext == ".md" || ext == ".markdown")                                   return TextEditor::Language::Markdown();
	if (ext == ".sql")                                                        return TextEditor::Language::Sql();
	if (ext == ".ini" || ext == ".cfg" || ext == ".conf")                     return TextEditor::Language::Ini();

	// Runtime-defined languages (HTML, INI, YAML, CFG, BAT, PS1, etc.)
	auto& byExt = runtimeLanguagesByExt();
	auto it = byExt.find(ext);
	if (it != byExt.end()) return it->second;
	return nullptr;
}


//
//	Editor::Editor
//

bool Editor::sSkipDemo = false;

Editor::Editor()
{
	// Load user-defined language definitions (HTML, INI, YAML, CFG, BAT, PS1).
	loadRuntimeLanguages();
	// Load editor-wide preferences (interpreter overrides, build commands, etc.)
	loadSettings();
	// If the user picked a custom font in a previous session, load it now into
	// the atlas so the first render uses it. Empty path = stick with bundled.
	if (!fontPath.empty()) applyFont();

	// Skip the demo when this is a second-or-later launch (settings file
	// already exists) OR when main.cpp told us to (via --project or
	// positional file arguments). On a true first run, fall through to the
	// demo so the user has something to look at.
	if (seenFirstRun || sSkipDemo) {
		auto& t = newTab();
		(void)t;
		return;
	}

	auto& t = newTab();
	t.originalText = demo;
	t.editor.SetText(demo);
	t.editor.SetLanguage(TextEditor::Language::Cpp());
	t.version = t.editor.GetUndoIndex();
	t.filename = "untitled";
	// Populate the trie up front so context-menu items like
	// "Go to Definition" can gate themselves on `trie.contains(word)` even
	// when autocomplete is disabled. Cheap for short docs.
	buildAutocompleteTrie(t);
}


//
//	Editor::newTab
//

Editor::TabDocument& Editor::newTab()
{
	auto t = std::make_unique<TabDocument>();
	t->id = nextId++;
	t->filename = "untitled";
	t->open = true;
	t->wantFocus = true;
	t->version = t->editor.GetUndoIndex();
	// Apply current editor prefs so toggles from Settings persist into new tabs.
	t->editor.SetAutoIndentEnabled(prefAutoIndent);
	t->editor.SetCompletePairedGlyphs(prefCompletePairs);
	t->editor.SetPanInverted(prefInvertPan);
	t->editor.SetWordWrap(prefWordWrap);
	t->editor.SetWrapWidth(static_cast<float>(prefWrapWidthPx));
	applyKeybindOverridesToEditor(t->editor);   // user keybind remaps into this editor
	tabs.push_back(std::move(t));
	activeTab = tabs.size() - 1;
	// Configure autocomplete for ONLY the new tab. Do NOT call
	// setAutocompleteMode() here: it rebuilds EVERY open tab's trie
	// (re-scanning each doc's identifiers + re-inserting the whole project
	// index), which produced a multi-second stall when adding a tab while a
	// large file like imgui.cpp was open. Other tabs' tries are already built
	// and unchanged, so only the brand-new tab needs setup.
	if (autocomplete) configureTabAutocomplete(*tabs.back());
	return *tabs.back();
}
Editor::TabDocument& Editor::newTab(const std::string& path, bool split, int index)
{
	auto t = std::make_unique<TabDocument>();
	t->id = nextId++;
	t->filename = path;
	t->open = true;
	t->wantFocus = true;
	t->wantSplit = split;
	t->version = t->editor.GetUndoIndex();

	size_t insertedAt = 0;
	if (index >= 0 && static_cast<size_t>(index) <= tabs.size())
	{
		auto it = tabs.emplace(tabs.begin() + index, std::move(t));
		insertedAt = static_cast<size_t>(it - tabs.begin());
	}
	else
	{
		tabs.push_back(std::move(t));
		insertedAt = tabs.size() - 1;
	}
	activeTab = insertedAt;
	applyKeybindOverridesToEditor(tabs[insertedAt]->editor);   // user keybind remaps into this editor
	if (autocomplete) configureTabAutocomplete(*tabs[insertedAt]);
	return *tabs[insertedAt];
}


//
//	Editor::closeTab
//

void Editor::closeTab(size_t idx)
{
	if (idx >= tabs.size()) return;

	// Remember this tab so Ctrl+Shift+T can bring it back. Skip empty untitled.
	{
		const auto& t = *tabs[idx];
		std::string txt = t.editor.GetText();
		if (!(t.filename == "untitled" && txt.empty()))
		{
			recentlyClosed.push_back({ t.filename, std::move(txt) });
			if (recentlyClosed.size() > 32) recentlyClosed.erase(recentlyClosed.begin());
		}
	}

	if (tabs.size() <= 1)
	{
		auto& t = *tabs[0];
		t.originalText.clear();
		t.editor.SetText("");
		t.editor.SetLanguage(nullptr);
		t.version = t.editor.GetUndoIndex();
		t.filename = "untitled";
		t.open = true;
		activeTab = 0;
		return;
	}
	tabs.erase(tabs.begin() + static_cast<std::ptrdiff_t>(idx));
	if (activeTab >= tabs.size()) activeTab = tabs.size() - 1;
	else if (activeTab > idx)     activeTab -= 1;
}


// ── Script runner ─────────────────────────────────────────────────────
// Map file extension → interpreter command. Adjust by editing here or via
// runtimeLanguages later. Output is captured into `script->output` and shown
// in a docked Output window. Synchronous — fine for small scripts; large
// outputs may block briefly.
// Returns an interpreter command prefix, or "" if the file should be invoked
// directly (the OS shell knows how to handle the extension). nullptr means
// "no interpreter mapped — don't run".
static const char* interpreterForExt(const std::string& ext)
{
	if (ext == ".py" || ext == ".pyw") return "python";
	if (ext == ".ps1")                 return "powershell -NoProfile -ExecutionPolicy Bypass -File";
	// .bat / .cmd: _popen on Windows already pipes through cmd.exe /c, so
	// passing the path directly is enough. Wrapping it in another `cmd /c`
	// caused the quote-collapsing rules to mangle paths with spaces and
	// could crash the spawned shell on some setups.
	if (ext == ".bat" || ext == ".cmd") return "";
	if (ext == ".sh")                  return "bash";
	if (ext == ".lua")                 return "lua";
	if (ext == ".js")                  return "node";
	return nullptr;
}

// Static wrapper expected by editor.h; runs the active doc's script.
void Editor::runScriptForActiveDoc() {}

void Editor::renderScriptOutputWindow()
{
	if (!script->visible) return;
	ImGui::SetNextWindowSize(ImVec2(600, 200), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Output###outputPanel", &script->visible))
	{
		if (ImGui::SmallButton("Clear"))
		{
			std::lock_guard<std::mutex> lock(script->mutex);
			script->output.clear();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Copy"))
		{
			std::string snap;
			{ std::lock_guard<std::mutex> lock(script->mutex); snap = script->output; }
			ImGui::SetClipboardText(snap.c_str());
		}
		ImGui::SameLine();
		if (script->running.load()) ImGui::TextDisabled("(running…)");
		else                      ImGui::TextDisabled("(F5 to re-run)");
		ImGui::Separator();
		// CLAUDE make the text split into lines and add line selection and text copying
		ImGui::BeginChild("##outputScroll", ImVec2(0, 0), 0,
						  ImGuiWindowFlags_HorizontalScrollbar |
						  ImGuiWindowFlags_AlwaysVerticalScrollbar);
		// Snapshot under the lock so the worker thread can keep appending
		// while we draw (worst case the next frame catches up).
		std::string snapshot;
		{
			std::lock_guard<std::mutex> lock(script->mutex);
			snapshot = script->output;
		}
		ImGui::TextUnformatted(snapshot.c_str());
		if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
		{
			ImGui::SetScrollHereY(1.0f);
		}
		ImGui::EndChild();
	}
	ImGui::End();
}


// Spawn the active document's interpreter on a background thread, stream
// stdout+stderr into `script->output`. Detached — multiple presses of F5 in
// quick succession will queue threads, only the latest one's output wins
// (older ones drop their writes when they discover `script->gen` has moved).
// Walk up from a starting directory looking for the first build entry-point
// — convention-based discovery so each project just drops a script in its root.
//
// On Windows we prefer `build.bat`/`build.cmd` first (most projects ship one),
// then `build.ps1`, then a Makefile / CMakeLists.txt at the same level. The
// search is capped at 8 levels up to avoid recursing into the filesystem root.
static std::pair<std::filesystem::path, std::string> findProjectBuildCommand(std::filesystem::path start)
{
	std::error_code ec;
	struct Candidate { const char* name; const char* interp; };
	static const Candidate cands[] = {
#ifdef _WIN32
		{ "build.bat",  "" },                                              // direct invoke via cmd.exe
		{ "build.cmd",  "" },
		{ "build.ps1",  "powershell -NoProfile -ExecutionPolicy Bypass -File" },
#else
		{ "build.sh",   "bash" },
#endif
		{ "Makefile",   "make" },
		// CMakeLists.txt isn't directly runnable but tells us the project root
		// — fall back to invoking `cmake --build .` from here.
	};

	auto cur = start;
	for (int level = 0; level < 8; ++level)
	{
		for (auto& c : cands)
		{
			auto p = cur / c.name;
			if (std::filesystem::is_regular_file(p, ec))
			{
				return { p, c.interp };
			}
		}

		// Project files at this level — scan once, pick the most specific.
		std::filesystem::path sln, csproj, vcxproj, uproject;
		for (auto& entry : std::filesystem::directory_iterator(cur, ec))
		{
			if (!entry.is_regular_file()) continue;
			auto ext = entry.path().extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(),
				[](unsigned char ch){ return static_cast<char>(std::tolower(ch)); });
			if      (ext == ".sln"      && sln.empty())      sln      = entry.path();
			else if (ext == ".csproj"   && csproj.empty())   csproj   = entry.path();
			else if (ext == ".vcxproj"  && vcxproj.empty())  vcxproj  = entry.path();
			else if (ext == ".uproject" && uproject.empty()) uproject = entry.path();
		}
		// Prefer .sln (covers C# + C++ multi-project), then .csproj, then .vcxproj.
		// dotnet's CLI handles .sln, .csproj transparently; vcxproj needs msbuild.
		if (!sln.empty())     return { sln,     "dotnet build" };
		if (!csproj.empty())  return { csproj,  "dotnet build" };
		if (!vcxproj.empty()) return { vcxproj, "msbuild" };
		if (!uproject.empty()) {
			// Unreal: run the engine's Build.bat / build script if we can find
			// it; fall back to noop so the user can wire it themselves.
			return { uproject, "" };  // "" → run directly via OS shell (won't work, but the user is signalled)
		}

		// CMakeLists.txt → `cmake --build <buildDir>` if we can find one.
		if (std::filesystem::is_regular_file(cur / "CMakeLists.txt", ec))
		{
			for (const char* sub :{ "build", "out/build/x64-Debug", "out/build" })
			{
				auto bd = cur / sub;
				if (std::filesystem::is_directory(bd, ec))
				{
					return { bd, "cmake --build ." };
				}
			}
			return { cur, "cmake -B build && cmake --build build" };
		}
		if (!cur.has_parent_path() || cur.parent_path() == cur) break;
		cur = cur.parent_path();
	}
	return { {}, {} };
}


void Editor::runProjectBuild()
{
	std::filesystem::path startDir;
	if (!tabs.empty() && doc().filename != "untitled")
	{
		startDir = std::filesystem::path(doc().filename).parent_path();
	}
	else
	{
		startDir = std::filesystem::current_path();
	}

	auto [scriptPath, interp] = findProjectBuildCommand(startDir);
	if (scriptPath.empty())
	{
		std::lock_guard<std::mutex> lock(script->mutex);
		script->output = "[no build.bat / build.ps1 / build.sh / Makefile / CMakeLists.txt found above " +
			startDir.string() + "]\n";
		script->visible = true;
		return;
	}

	// For Makefile / CMake the "script path" we found is a directory we should
	// `cd` to before running; for build.* it's the script file itself.
	std::filesystem::path runDir;
	std::string cmd;
	bool isFileScript = std::filesystem::is_regular_file(scriptPath);
	if (isFileScript)
	{
		runDir = scriptPath.parent_path();
		if (!interp.empty()) cmd = interp + " \"" + scriptPath.string() + "\"";
		else                 cmd = "\"" + scriptPath.string() + "\"";
	}
	else
	{
		runDir = scriptPath;
		cmd = interp;
	}

	// Substitute %CONFIG% with the active build configuration so presets like
	// "cmake --build . --config %CONFIG%" pick up Debug / Release.
	{
		std::string token = "%CONFIG%";
		for (size_t pos = 0; (pos = cmd.find(token, pos)) != std::string::npos; ) {
			cmd.replace(pos, token.size(), activeBuildConfig);
			pos += activeBuildConfig.size();
		}
	}

	int gen = ++script->gen;
	{
		std::lock_guard<std::mutex> lock(script->mutex);
		script->output = "$ cd " + runDir.string() + " && " + cmd + "\n";
		script->visible = true;
	}
	script->running = true;

	// Capture the shared_ptr BY VALUE so the ScriptState stays alive even if
	// the Editor is destroyed while this thread is still running (e.g. the
	// user quits while a hung script is blocked in fread). Without this, the
	// thread would dereference a dangling `this->script` and crash on exit.
	auto scriptCtx = script;
	std::thread([scriptCtx, cmd, runDir, gen]()
				{
					std::string full;
#ifdef _WIN32
					full = "\"< NUL pushd \"" + runDir.string() + "\" && " + cmd + " 2>&1 & popd\"";
					FILE* pipe = _popen(full.c_str(), "r");
#else
					full = "cd \"" + runDir.string() + "\" && < /dev/null " + cmd + " 2>&1";
					FILE* pipe = popen(full.c_str(), "r");
#endif
					if (!pipe)
					{
						std::lock_guard<std::mutex> lock(scriptCtx->mutex);
						if (gen == scriptCtx->gen.load()) scriptCtx->output += "[failed to spawn]\n";
						scriptCtx->running = false;
						return;
					}
					char buf[4096];
					while (size_t n = fread(buf, 1, sizeof(buf), pipe))
					{
						std::lock_guard<std::mutex> lock(scriptCtx->mutex);
						if (gen != scriptCtx->gen.load()) break;
						scriptCtx->output.append(buf, n);
						if (scriptCtx->output.size() > (8u << 20))
						{
							scriptCtx->output.append("\n[…truncated after 8 MB…]\n");
							break;
						}
					}
					int rc =
#ifdef _WIN32
						_pclose(pipe);
#else
						pclose(pipe);
#endif
					std::lock_guard<std::mutex> lock(scriptCtx->mutex);
					if (gen == scriptCtx->gen.load())
					{
						scriptCtx->output += "\n[exit " + std::to_string(rc) + "]\n";
					}
					scriptCtx->running = false;
				}).detach();
}


// Walk projectRoot looking for a built executable under the usual build
// dirs. Returns the newest exe so a fresh `cmake --build` is picked up.
std::filesystem::path Editor::findBuiltExe() const
{
	if (projectRoot.empty()) return {};
	std::vector<std::filesystem::path> searchRoots = {
		projectRoot / "out" / "build",
		projectRoot / "build",
		projectRoot / "bin",
		projectRoot / "x64",
		projectRoot,
	};
	std::filesystem::path best;
	std::filesystem::file_time_type bestTime{};
	std::error_code ec;
	for (auto& root : searchRoots) {
		if (!std::filesystem::exists(root, ec)) continue;
		for (auto it = std::filesystem::recursive_directory_iterator(
				root, std::filesystem::directory_options::skip_permission_denied, ec);
			it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
		{
			if (ec) { ec.clear(); continue; }
			if (!it->is_regular_file(ec)) continue;
			auto ext = it->path().extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(),
				[](unsigned char c){ return (char)std::tolower(c); });
#ifdef _WIN32
			if (ext != ".exe") continue;
#else
			if (ext == ".so" || ext == ".dylib") continue;
			// On POSIX, we look for files with no extension that are executable.
			// (Naive — good enough for common CMake / Ninja output layouts.)
			if (!ext.empty()) continue;
#endif
			auto t = std::filesystem::last_write_time(it->path(), ec);
			if (ec) { ec.clear(); continue; }
			if (best.empty() || t > bestTime) { best = it->path(); bestTime = t; }
		}
	}
	return best;
}

void Editor::runProjectExeOrScript()
{
	// Prefer running a built exe when we have a project root with one.
	// Falls back to the per-doc script interpreter otherwise.
	if (!projectRoot.empty()) {
		auto exe = findBuiltExe();
		if (!exe.empty()) {
			std::string cmd = "\"" + exe.string() + "\"";
			int gen = ++script->gen;
			{
				std::lock_guard<std::mutex> lock(script->mutex);
				script->output = "$ " + cmd + "\n";
				script->visible = true;
			}
			script->running = true;
			auto scriptCtx = script;
			auto runDir = exe.parent_path();
			std::thread([scriptCtx, cmd, runDir, gen]() {
				std::string full;
#ifdef _WIN32
				full = "\"< NUL pushd \"" + runDir.string() + "\" && " + cmd + " 2>&1 & popd\"";
				FILE* pipe = _popen(full.c_str(), "r");
#else
				full = "cd \"" + runDir.string() + "\" && < /dev/null " + cmd + " 2>&1";
				FILE* pipe = popen(full.c_str(), "r");
#endif
				if (!pipe) {
					std::lock_guard<std::mutex> lock(scriptCtx->mutex);
					if (gen == scriptCtx->gen.load()) scriptCtx->output += "[failed to spawn]\n";
					scriptCtx->running = false;
					return;
				}
				char buf[4096];
				while (size_t n = fread(buf, 1, sizeof(buf), pipe)) {
					std::lock_guard<std::mutex> lock(scriptCtx->mutex);
					if (gen != scriptCtx->gen.load()) break;
					scriptCtx->output.append(buf, n);
					if (scriptCtx->output.size() > (8u << 20)) {
						scriptCtx->output.append("\n[…truncated after 8 MB…]\n");
						break;
					}
				}
				int rc =
#ifdef _WIN32
					_pclose(pipe);
#else
					pclose(pipe);
#endif
				std::lock_guard<std::mutex> lock(scriptCtx->mutex);
				if (gen == scriptCtx->gen.load())
					scriptCtx->output += "\n[exit " + std::to_string(rc) + "]\n";
				scriptCtx->running = false;
			}).detach();
			return;
		}
	}
	runScriptForDoc();
}


void Editor::runScriptForDoc()
{
	if (tabs.empty()) return;
	auto& t = doc();
	if (t.filename == "untitled") return;

	auto ext = std::filesystem::path(t.filename).extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(),
				   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	// Settings interpreter override takes precedence over the built-in
	// per-extension defaults so the user can swap python -> python3, etc.
	std::string interpStr;
	auto over = interpreterOverrides.find(ext);
	if (over != interpreterOverrides.end()) {
		interpStr = over->second;
	} else if (const char* def = interpreterForExt(ext)) {
		interpStr = def;
	} else {
		std::lock_guard<std::mutex> lock(script->mutex);
		script->output = "[no interpreter mapped for " + ext + "]\n";
		script->visible = true;
		return;
	}

	if (isDirty()) saveFile();

	std::string cmd;
	if (!interpStr.empty()) cmd = interpStr + " \"" + t.filename + "\"";
	else                    cmd = "\"" + t.filename + "\"";

	int gen = ++script->gen;
	{
		std::lock_guard<std::mutex> lock(script->mutex);
		script->output = "$ " + cmd + "\n";
		script->visible = true;
	}
	script->running = true;

	// Capture the shared_ptr BY VALUE so a still-running script thread keeps
	// the ScriptState alive past Editor's destruction (program shutdown).
	auto scriptCtx = script;
	std::thread([scriptCtx, cmd, gen]()
				{
					std::string full;
#ifdef _WIN32
					full = "\"< NUL " + cmd + " 2>&1\"";
					FILE* pipe = _popen(full.c_str(), "r");
#else
					full = "< /dev/null " + cmd + " 2>&1";
					FILE* pipe = popen(full.c_str(), "r");
#endif
					if (!pipe)
					{
						std::lock_guard<std::mutex> lock(scriptCtx->mutex);
						if (gen == scriptCtx->gen.load()) scriptCtx->output += "[failed to spawn]\n";
						scriptCtx->running = false;
						return;
					}
					char buf[4096];
					bool truncated = false;
					while (size_t n = fread(buf, 1, sizeof(buf), pipe))
					{
						std::lock_guard<std::mutex> lock(scriptCtx->mutex);
						if (gen != scriptCtx->gen.load()) break;   // superseded by newer F5
						scriptCtx->output.append(buf, n);
						if (scriptCtx->output.size() > (8u << 20))
						{
							scriptCtx->output.append("\n[…truncated after 8 MB…]\n");
							truncated = true;
							break;
						}
					}
					int rc =
#ifdef _WIN32
						_pclose(pipe);
#else
						pclose(pipe);
#endif
					std::lock_guard<std::mutex> lock(scriptCtx->mutex);
					if (gen == scriptCtx->gen.load())
					{
						if (!truncated) scriptCtx->output += "\n[exit " + std::to_string(rc) + "]\n";
					}
					scriptCtx->running = false;
				}).detach();
}


void Editor::toggleHeaderSource()
{
	if (tabs.empty()) return;
	std::filesystem::path p(doc().filename);
	if (!p.has_extension()) return;
	std::string ext = p.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(),
				   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	// Candidate extensions for the "other side"
	std::vector<std::string> candidates;
	if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh")
	{
		candidates = { ".cpp", ".cc", ".cxx", ".c", ".m", ".mm", ".inl" };
	}
	else if (ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
			 ext == ".m" || ext == ".mm" || ext == ".inl")
	{
		candidates = { ".h", ".hpp", ".hxx", ".hh" };
	}
	else
	{
		return;
	}

	auto stem = p.parent_path() / p.stem();
	for (const auto& candExt : candidates)
	{
		auto candidate = stem;
		candidate += candExt;
		// 1. Already open? Just focus its tab.
		for (size_t i = 0; i < tabs.size(); ++i)
		{
			if (std::filesystem::path(tabs[i]->filename) == candidate)
			{
				activeTab = i;
				tabs[i]->wantFocus = true;
				return;
			}
		}
		// 2. Exists on disk next to the source? Open it.
		std::error_code ec;
		if (std::filesystem::exists(candidate, ec))
		{
			openFile(candidate.string());
			return;
		}
	}

	// 3. Project-tree fallback — handles include/ vs src/ splits (the common
	// case the same-dir search misses). Search the project root (or the doc's
	// repo root) for <stem><candidateExt>; pick the match whose path shares the
	// longest prefix with the source file (nearest sibling tree wins).
	{
		std::filesystem::path searchRoot = projectRoot;
		if (searchRoot.empty()) searchRoot = resolveOutermostRoot(p.parent_path());
		std::error_code ec;
		std::unordered_set<std::string> wanted;
		for (auto& candExt : candidates) wanted.insert(p.stem().string() + candExt);

		std::filesystem::path best;
		size_t bestCommon = 0;
		auto srcStr = p.generic_string();
		int budget = 40000;
		for (auto it = std::filesystem::recursive_directory_iterator(
				searchRoot, std::filesystem::directory_options::skip_permission_denied, ec);
			 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
			if (ec) { ec.clear(); continue; }
			if (it->is_directory(ec)) {
				auto n = it->path().filename().string();
				if (n == ".git" || n == "node_modules" || n == "out" || n == "build" ||
					n == "obj" || n == "bin" || n == ".vs") it.disable_recursion_pending();
				continue;
			}
			if (--budget < 0) break;
			if (!wanted.count(it->path().filename().string())) continue;
			// Longest common path prefix with the source = nearest relative.
			auto cand = it->path().generic_string();
			size_t common = 0;
			while (common < cand.size() && common < srcStr.size() && cand[common] == srcStr[common]) ++common;
			if (best.empty() || common > bestCommon) { best = it->path(); bestCommon = common; }
		}
		if (!best.empty()) {
			// Re-check open tabs for the resolved path before opening.
			for (size_t i = 0; i < tabs.size(); ++i) {
				if (std::filesystem::path(tabs[i]->filename) == best) {
					activeTab = i; tabs[i]->wantFocus = true; return;
				}
			}
			openFile(best.string());
			return;
		}
	}
	// 4. Nothing found — silently no-op.
}


// ── Navigation panel ─────────────────────────────────────────────────
//
// Dockable file-tree view of the project root. Lazy directory iteration:
// folder children are only enumerated when the user expands the node, so
// huge project trees don't stall startup.

// Snapshot the currently-open file tabs (skip untitled / dirty) into
// projectSessions, keyed by the current projectRoot. Idempotent — overwrites
// the previous snapshot for this root.
void Editor::saveCurrentProjectSession()
{
	if (projectRoot.empty()) return;
	std::vector<std::string> files;
	for (auto& up : tabs) {
		auto& t = *up;
		if (t.filename == "untitled") continue;
		files.push_back(t.filename);
	}
	std::error_code ec;
	auto canon = std::filesystem::weakly_canonical(projectRoot, ec);
	projectSessions[(ec ? projectRoot : canon).string()] = std::move(files);
}

// Close every non-untitled tab. Dirty tabs are kept so the user doesn't lose
// unsaved work when switching projects — they'll just remain open in the new
// project view until the user closes / saves them explicitly.
void Editor::closeAllProjectTabs()
{
	for (size_t i = tabs.size(); i-- > 0; ) {
		auto& t = *tabs[i];
		if (t.filename == "untitled") continue;
		if (isDirtyTab(i)) continue;
		closeTab(i);
	}
}

// Open files from the saved session for `root` (if any).
void Editor::restoreProjectSession(const std::filesystem::path& root)
{
	std::error_code ec;
	auto canon = std::filesystem::weakly_canonical(root, ec);
	auto it = projectSessions.find((ec ? root : canon).string());
	if (it == projectSessions.end()) return;
	for (auto& f : it->second) {
		std::error_code fec;
		if (std::filesystem::exists(f, fec)) openFile(f);
	}
}

// Walk up from a path to the outermost enclosing repository root, so opening a
// project file buried inside a larger tree (e.g. imgui/examples/foo) roots the
// nav panel at the whole project (imgui/) where its local dependencies live.
// Strategy: the outermost ancestor that contains a `.git` entry; failing that,
// the highest ancestor still holding a solution/workspace-level marker; else the
// folder itself.
static std::filesystem::path resolveOutermostRoot(std::filesystem::path start)
{
	std::error_code ec;
	if (std::filesystem::is_regular_file(start, ec)) start = start.parent_path();
	std::filesystem::path best = start;
	std::filesystem::path cur  = start;
	for (int i = 0; i < 64; ++i) {
		// Outermost wins: keep overwriting `best` as we climb past each marker.
		if (std::filesystem::exists(cur / ".git", ec) ||
			std::filesystem::exists(cur / ".hg", ec)  ||
			std::filesystem::exists(cur / ".svn", ec))
			best = cur;
		if (!cur.has_parent_path() || cur.parent_path() == cur) break;
		cur = cur.parent_path();
	}
	return best;
}

void Editor::setProjectRoot(const std::filesystem::path& p)
{
	std::error_code ec;
	auto abs = std::filesystem::absolute(p, ec);
	if (ec || abs.empty()) return;
	// Respect exactly what the user opened: a folder is used as-is, a project
	// file resolves to its containing folder. (Earlier we climbed to the
	// outermost git root, but that over-reached on monorepos — e.g. opening
	// uevr-frontend/uevr jumped up to the whole uevr-frontend tree.)
	if (std::filesystem::is_regular_file(abs, ec)) abs = abs.parent_path();
	// If we're switching projects, snapshot the outgoing project's open tabs
	// so reopening that project later restores the workspace.
	if (!projectRoot.empty()) {
		auto oldCanon = std::filesystem::weakly_canonical(projectRoot, ec);
		auto newCanon = std::filesystem::weakly_canonical(abs, ec);
		if (oldCanon != newCanon) {
			saveCurrentProjectSession();
			closeAllProjectTabs();
		}
	}
	projectRoot = abs;
	navPanelVisible = true;
	rememberRecentProject(abs.string());
	restoreProjectSession(abs);
	// Kick off the background symbol index for this project (autocomplete + nav).
	rebuildProjectIndex();
	// Persist immediately so a crash/quit doesn't lose the session.
	saveSettings();
}

void Editor::openProjectFolderPicker()
{
	// Re-use the file dialog. Accepting either a folder OR a project file
	// (.sln/.csproj/.vcxproj/.uproject/CMakeLists.txt). Project files are
	// resolved to their parent directory below in the dialog-close path.
	if (auto* vp = ImGui::GetWindowViewport()) dialogViewportId = vp->ID;
	else dialogViewportId = ImGui::GetMainViewport()->ID;
	dialogNeedsPlacement = true;
	IGFD::FileDialogConfig config;
	config.countSelectionMax = 1;
	// OptionalFileName lets the user navigate into a folder and just hit
	// "Open" without selecting any file — that's how you pick a plain folder
	// as the project root. Without this, the dialog refuses to validate
	// unless a file is highlighted.
	config.path = dialogStartDir();
	config.flags = ImGuiFileDialogFlags_DontShowHiddenFiles
	             | ImGuiFileDialogFlags_OptionalFileName;
	populateFileDialogPlaces();
	// Filter spec is OR-of-extensions plus a "Any folder" pass-through.
	const char* filter = "Project ({.sln,.csproj,.vcxproj,.uproject,.uplugin}){.sln,.csproj,.vcxproj,.uproject,.uplugin},CMakeLists.txt{CMakeLists.txt},All{.*}";
	ImGuiFileDialog::Instance()->OpenDialog("project-open", "Open Project (folder or project file)...", filter, config);
	state = State::openProject;
}

bool Editor::navIsExcluded(const std::filesystem::path& p) const
{
	std::error_code ec;
	auto key = std::filesystem::weakly_canonical(p, ec);
	auto it = navExcluded.find((ec ? p : key).string());
	return it != navExcluded.end() && it->second;
}

bool Editor::navIsCodeFile(const std::filesystem::path& p) const
{
	// "Code-only" filter — same set as the project-wide grep walker, plus a
	// few project-file extensions so .sln / .csproj show up in the tree.
	auto ext = p.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(),
		[](unsigned char c){ return (char) std::tolower(c); });
	static const std::unordered_set<std::string> ok = {
		".c", ".h", ".cpp", ".hpp", ".cxx", ".hxx", ".cc", ".hh", ".m", ".mm", ".inl",
		".cs", ".vb", ".fs", ".fsx",
		".java", ".kt", ".scala", ".groovy",
		".py", ".pyw", ".rb", ".php", ".pl",
		".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs",
		".go", ".rs", ".swift", ".lua", ".sh", ".ps1", ".psm1",
		".sql", ".r", ".jl", ".dart",
		".sln", ".csproj", ".vcxproj", ".uproject", ".uplugin",
		".cmake", ".cmakelists",
		".html", ".htm", ".xml", ".xaml", ".json", ".yaml", ".yml", ".toml", ".ini",
		".md", ".rst", ".tex", ".lang",
	};
	return ok.count(ext) != 0
		|| p.filename() == "CMakeLists.txt"
		|| p.filename() == "Makefile"
		|| p.filename() == "Dockerfile"
		|| p.filename() == "requirements.txt";
}

// Drag-drop a file or folder onto a target folder. Ctrl held during drop =
// copy, otherwise move. Refuses to move a folder into itself or its own
// descendant.
void Editor::navMoveOrCopy(const std::string& src,
                           const std::string& destDir, bool copy)
{
	std::error_code ec;
	std::filesystem::path s(src), d(destDir);
	if (!std::filesystem::exists(d, ec) || !std::filesystem::is_directory(d, ec)) return;
	auto canonSrc = std::filesystem::weakly_canonical(s, ec);
	auto canonDst = std::filesystem::weakly_canonical(d, ec);
	// Block move-into-self / move-into-own-subtree.
	for (auto p = canonDst; !p.empty(); p = p.parent_path()) {
		if (p == canonSrc) return;
		if (p == p.parent_path()) break;
	}
	auto target = canonDst / s.filename();
	if (copy) {
		std::filesystem::copy(s, target,
			std::filesystem::copy_options::recursive |
			std::filesystem::copy_options::overwrite_existing, ec);
	} else {
		std::filesystem::rename(s, target, ec);
	}
}


// Best-effort "send to recycle bin" so right-click → Delete isn't a one-way
// road. On Windows uses SHFileOperation with FOF_ALLOWUNDO; on POSIX falls
// back to xdg-trash if available, otherwise plain remove_all.
void Editor::navDeletePath(const std::string& p)
{
#ifdef _WIN32
	// SHFileOperationA needs a double-NUL-terminated source buffer.
	std::string buf = p;
	buf.push_back('\0');
	buf.push_back('\0');
	SHFILEOPSTRUCTA op{};
	op.wFunc  = FO_DELETE;
	op.pFrom  = buf.c_str();
	op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
	if (SHFileOperationA(&op) != 0) {
		// Fall back to permanent delete if recycle bin failed (e.g. network drive).
		std::error_code dec;
		std::filesystem::remove_all(p, dec);
	}
#else
	// Try gio trash, then fallback to permanent delete.
	std::string cmd = "gio trash \"" + p + "\" 2>/dev/null";
	if (std::system(cmd.c_str()) != 0) {
		std::error_code dec;
		std::filesystem::remove_all(p, dec);
	}
#endif
}

void Editor::navOpenPathInExplorer(const std::string& path)
{
#ifdef _WIN32
	// /select, opens Explorer with the file/dir highlighted; for a dir,
	// passing the dir itself opens it.
	std::error_code ec;
	bool isDir = std::filesystem::is_directory(path, ec);
	std::string cmd;
	if (isDir) cmd = "explorer \"" + path + "\"";
	else       cmd = "explorer /select,\"" + path + "\"";
	std::system(cmd.c_str());
#else
	std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
	std::system(cmd.c_str());
#endif
}

void Editor::navOpenExternally(const std::string& path)
{
#ifdef _WIN32
	ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
	std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
	std::system(cmd.c_str());
#endif
}

// One tree-node row for a single entry. Encapsulates the context menu, rename
// inline-edit, click-to-open behaviour, and recurses for directories.
static void renderDirNode(Editor* self,
                          const std::filesystem::path& dir,
                          int depth,
                          bool showDot,
                          std::string& contextPath,
                          std::string& renameTarget,
                          char* renameBuf,
                          std::string& pendingDelete);

static void navRenderEntry(Editor* self,
                           const std::filesystem::directory_entry& e,
                           int depth,
                           bool showDot,
                           std::string& contextPath,
                           std::string& renameTarget,
                           char* renameBuf,
                           std::string& pendingDelete)
{
	std::error_code ec;
	auto name = e.path().filename().string();
	bool isDir = e.is_directory(ec);
	auto absPath = e.path().string();

	// In-place rename: replace the row with an InputText.
	if (renameTarget == absPath) {
		ImGui::PushID(absPath.c_str());
		ImGui::SetNextItemWidth(-FLT_MIN);
		bool commit = ImGui::InputText("##rename", renameBuf, 256,
			ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		if (ImGui::IsItemDeactivated()) {
			// Treat focus loss as cancel unless Enter committed.
			if (!commit) renameTarget.clear();
		}
		if (commit) {
			std::string newName = renameBuf;
			if (!newName.empty() && newName != name) {
				auto target = e.path().parent_path() / newName;
				std::error_code rec;
				std::filesystem::rename(e.path(), target, rec);
			}
			renameTarget.clear();
		}
		ImGui::PopID();
		return;
	}

	// Helper — hover tooltip with file type + timestamps. Cheap: only fires
	// when the user dwells on the row.
	auto navTooltip = [&](const std::filesystem::directory_entry& ent, bool dir) {
		if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
			return;
		ImGui::BeginTooltip();
		ImGui::TextUnformatted(ent.path().filename().string().c_str());
		ImGui::Separator();
		std::error_code lec;
		if (dir) {
			ImGui::TextDisabled("Folder");
		} else {
			auto ext = ent.path().extension().string();
			ImGui::TextDisabled("Type: %s", ext.empty() ? "(none)" : ext.c_str() + 0);
			auto sz = std::filesystem::file_size(ent.path(), lec);
			if (!lec) {
				if (sz < 1024) ImGui::TextDisabled("Size: %llu B", (unsigned long long) sz);
				else if (sz < 1024ull * 1024)
					ImGui::TextDisabled("Size: %.1f KB", sz / 1024.0);
				else if (sz < 1024ull * 1024 * 1024)
					ImGui::TextDisabled("Size: %.1f MB", sz / (1024.0 * 1024.0));
				else
					ImGui::TextDisabled("Size: %.2f GB", sz / (1024.0 * 1024.0 * 1024.0));
			}
		}
		// Modified time is the only one std::filesystem gives us portably.
		// Created time is Win32-only — gate accordingly.
#ifdef _WIN32
		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (GetFileAttributesExA(ent.path().string().c_str(), GetFileExInfoStandard, &fad)) {
			auto fmt = [](FILETIME ft) -> std::string {
				SYSTEMTIME st;
				FileTimeToSystemTime(&ft, &st);
				char buf[64];
				std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
					st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
				return buf;
			};
			ImGui::TextDisabled("Created:  %s", fmt(fad.ftCreationTime).c_str());
			ImGui::TextDisabled("Modified: %s", fmt(fad.ftLastWriteTime).c_str());
		}
#else
		auto mt = std::filesystem::last_write_time(ent.path(), lec);
		if (!lec) {
			auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
				mt - decltype(mt)::clock::now() + std::chrono::system_clock::now());
			std::time_t cftime = std::chrono::system_clock::to_time_t(sctp);
			char buf[64];
			std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&cftime));
			ImGui::TextDisabled("Modified: %s", buf);
		}
#endif
		// Image files get a thumbnail preview below the metadata.
		if (!dir) {
			auto ext = ent.path().extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
			if (Editor::isImageExt(ext)) {
				ImGui::Separator();
				self->navShowImageThumbnail(ent.path().string());
			}
		}
		ImGui::EndTooltip();
	};

	// Helper — drag source + drag target. Folders accept drops as
	// move/copy targets; files act as drag sources.
	auto navDnD = [&](bool dir) {
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
			self->navDragSourceSet(absPath);
			ImGui::SetDragDropPayload("NAVPATH", absPath.c_str(), absPath.size() + 1);
			ImGui::TextDisabled("%s", name.c_str());
			ImGui::EndDragDropSource();
		}
		if (dir && ImGui::BeginDragDropTarget()) {
			if (const auto* payload = ImGui::AcceptDragDropPayload("NAVPATH")) {
				std::string src((const char*) payload->Data, payload->DataSize - 1);
				bool copy = ImGui::GetIO().KeyCtrl;
				self->navMoveOrCopy(src, absPath, copy);
			}
			ImGui::EndDragDropTarget();
		}
	};

	if (isDir) {
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
		bool open = ImGui::TreeNodeEx(name.c_str(), flags);
		navTooltip(e, true);
		navDnD(true);
		if (ImGui::BeginPopupContextItem()) {
			contextPath = absPath;
			ImGui::EndPopup();
		}
		if (open) {
			if (!self->navIsFlat()) {
				renderDirNode(self, e.path(), depth + 1, showDot,
					contextPath, renameTarget, renameBuf, pendingDelete);
			}
			ImGui::TreePop();
		}
	} else {
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf
			| ImGuiTreeNodeFlags_NoTreePushOnOpen
			| ImGuiTreeNodeFlags_SpanAvailWidth;
		ImGui::TreeNodeEx(name.c_str(), flags);
		navTooltip(e, false);
		navDnD(false);
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
			self->openFile(absPath);
		}
		if (ImGui::BeginPopupContextItem()) {
			contextPath = absPath;
			ImGui::EndPopup();
		}
	}
}

static void renderDirNode(Editor* self,
                          const std::filesystem::path& dir,
                          int depth,
                          bool showDot,
                          std::string& contextPath,
                          std::string& renameTarget,
                          char* renameBuf,
                          std::string& pendingDelete)
{
	if (depth > 20) return;
	std::error_code ec;
	std::vector<std::filesystem::directory_entry> entries;
	for (auto& e : std::filesystem::directory_iterator(
			dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
		if (ec) break;
		entries.push_back(e);
	}
	std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
		bool aDir = a.is_directory(), bDir = b.is_directory();
		if (aDir != bDir) return aDir;
		auto an = a.path().filename().string(), bn = b.path().filename().string();
		std::transform(an.begin(), an.end(), an.begin(),
			[](unsigned char c){ return (char)std::tolower(c); });
		std::transform(bn.begin(), bn.end(), bn.begin(),
			[](unsigned char c){ return (char)std::tolower(c); });
		return an < bn;
	});
	for (auto& e : entries) {
		auto name = e.path().filename().string();
		if (!showDot && !name.empty() && name[0] == '.') continue;
		bool excluded = self->navIsExcluded(e.path());
		if (excluded && !self->navIsShowingExcluded()) continue;
		std::error_code dec;
		bool isDir = e.is_directory(dec);
		// Code-only filter: still show folders (you need them to navigate),
		// but hide non-source files.
		if (self->navIsCodeOnly() && !isDir && !self->navIsCodeFile(e.path()))
			continue;
		navRenderEntry(self, e, depth, showDot,
			contextPath, renameTarget, renameBuf, pendingDelete);
	}
}

// Flat view: every file under root in one alphabetical list (no folder
// hierarchy), honoring the same filters. Click opens; hover shows the relative
// path; right-click sets the context path for the shared popup.
static void navRenderFlat(Editor* self, const std::filesystem::path& root,
                          bool showDot, std::string& contextPath)
{
	std::error_code ec;
	std::vector<std::filesystem::path> files;
	std::unordered_set<std::string> seen;   // dedupe by canonical path
	int budget = 20000;
	auto isBuildDir = [](const std::string& n) {
		return n == ".git" || n == ".svn" || n == ".hg" || n == "node_modules" ||
		       n == "bin" || n == "obj" || n == "out" || n == "build" ||
		       n == "target" || n == ".vs" || n == ".vscode" || n == ".idea" ||
		       n == "__pycache__" || n == "packages" || n == "Debug" || n == "Release";
	};
	for (auto it = std::filesystem::recursive_directory_iterator(
			root, std::filesystem::directory_options::skip_permission_denied, ec);
		 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
		if (ec) { ec.clear(); continue; }
		auto name = it->path().filename().string();
		if (it->is_directory(ec)) {
			// Don't descend into build/vendor/vcs trees — that's what produced
			// junk + duplicate copies of project files in the flat list.
			bool dotdir = !showDot && !name.empty() && name[0] == '.';
			if (dotdir || isBuildDir(name) || self->navIsExcluded(it->path()))
				it.disable_recursion_pending();
			continue;
		}
		if (--budget < 0) break;
		if (!it->is_regular_file(ec)) continue;            // actual files only
		if (!showDot && !name.empty() && name[0] == '.') continue;
		if (self->navIsExcluded(it->path())) continue;
		// Flat view shows project source/content only.
		if (!self->navIsCodeFile(it->path())) continue;
		auto canon = std::filesystem::weakly_canonical(it->path(), ec);
		if (!seen.insert((ec ? it->path() : canon).string()).second) continue;  // no dupes
		files.push_back(it->path());
	}
	std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
		auto an = a.filename().string(), bn = b.filename().string();
		std::transform(an.begin(), an.end(), an.begin(), [](unsigned char c){ return (char)std::tolower(c); });
		std::transform(bn.begin(), bn.end(), bn.begin(), [](unsigned char c){ return (char)std::tolower(c); });
		return an < bn;
	});
	for (size_t i = 0; i < files.size(); ++i) {
		ImGui::PushID((int)i);
		auto name = files[i].filename().string();
		if (ImGui::Selectable(name.c_str())) self->openFile(files[i].string());
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
			std::error_code rec;
			auto rel = std::filesystem::relative(files[i], root, rec);
			auto ext = files[i].extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
			ImGui::BeginTooltip();
			ImGui::TextUnformatted((rec ? files[i] : rel).string().c_str());
			if (Editor::isImageExt(ext)) self->navShowImageThumbnail(files[i].string());
			ImGui::EndTooltip();
		}
		if (ImGui::BeginPopupContextItem()) { contextPath = files[i].string(); ImGui::EndPopup(); }
		ImGui::PopID();
	}
}

void Editor::renderNavigationPanel()
{
	if (!navPanelVisible) return;
	ImGui::SetNextWindowSize(ImVec2(280.0f, 480.0f), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Navigation##projectNav", &navPanelVisible))
	{
		auto root = projectRoot.empty() ? std::filesystem::current_path() : projectRoot;
		ImGui::TextDisabled("%s", root.string().c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("...")) { openProjectFolderPicker(); }
		// Toggle row — defaults are persisted in [editor] of settings.txt.
		// Hover the icons for what each filter does.
		ImGui::Checkbox(".dot", &navShowDotFiles);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show dotfiles (.git, .env, etc.)");
		ImGui::SameLine();
		ImGui::Checkbox("code", &navCodeOnly);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Code/project files only — hide build artefacts and docs.");
		ImGui::SameLine();
		ImGui::Checkbox("excl", &navShowExcluded);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show items you've excluded so you can re-include them.");
		ImGui::SameLine();
		ImGui::Checkbox("flat", &navFlatFiles);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flat view — every file in one list, no folder nesting.");
		ImGui::Separator();

		navContextPath.clear();
		std::error_code ec;
		if (std::filesystem::is_directory(root, ec)) {
			if (navFlatFiles)
				navRenderFlat(this, root, navShowDotFiles, navContextPath);
			else
				renderDirNode(this, root, 0, navShowDotFiles,
					navContextPath, navRenameTarget, navRenameBuf, navPendingDelete);
		} else {
			ImGui::TextDisabled("(no project root set)");
		}

		// Context-menu popup. BeginPopupContextItem above sets contextPath when
		// the user right-clicks a tree row; we open the popup here with that
		// path baked in so menu items act on the correct entry.
		if (!navContextPath.empty()) {
			ImGui::OpenPopup("##navCtx");
		}
		if (ImGui::BeginPopup("##navCtx")) {
			// Cache the path on first open so it survives across frames while
			// the popup is up. (BeginPopupContextItem only fires once.)
			static std::string ctxPath;
			if (!navContextPath.empty()) ctxPath = navContextPath;
			std::error_code cec;
			bool isDir = std::filesystem::is_directory(ctxPath, cec);
			ImGui::TextDisabled("%s", std::filesystem::path(ctxPath).filename().string().c_str());
			ImGui::Separator();
			if (!isDir && ImGui::MenuItem("Open")) { openFile(ctxPath); }
			if (!isDir && ImGui::MenuItem("Open to Left"))  { openFileToSide(ctxPath, -1); }
			if (!isDir && ImGui::MenuItem("Open to Right")) { openFileToSide(ctxPath, +1); }
			if (ImGui::MenuItem("Open in Explorer")) { navOpenPathInExplorer(ctxPath); }
			if (ImGui::MenuItem("Copy path")) { ImGui::SetClipboardText(ctxPath.c_str()); }
			ImGui::Separator();
			if (ImGui::MenuItem("Copy")) { navClipboardPath = ctxPath; navClipboardIsCut = false; }
			if (ImGui::MenuItem("Cut"))  { navClipboardPath = ctxPath; navClipboardIsCut = true;  }
			if (ImGui::MenuItem("Paste", nullptr, false,
				!navClipboardPath.empty() && isDir))
			{
				auto src = std::filesystem::path(navClipboardPath);
				auto dst = std::filesystem::path(ctxPath) / src.filename();
				std::error_code pec;
				if (navClipboardIsCut) std::filesystem::rename(src, dst, pec);
				else std::filesystem::copy(src, dst,
						std::filesystem::copy_options::recursive |
						std::filesystem::copy_options::overwrite_existing, pec);
				if (navClipboardIsCut) navClipboardPath.clear();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Rename")) {
				navRenameTarget = ctxPath;
				auto leaf = std::filesystem::path(ctxPath).filename().string();
				std::snprintf(navRenameBuf, sizeof(navRenameBuf), "%s", leaf.c_str());
			}
			if (ImGui::MenuItem("Delete")) {
				navPendingDelete = ctxPath;
				ImGui::OpenPopup("##navConfirmDel");
			}
			ImGui::Separator();
			std::error_code wec;
			auto canon = std::filesystem::weakly_canonical(ctxPath, wec);
			std::string canonKey = (wec ? std::filesystem::path(ctxPath) : canon).string();
			bool excluded = navExcluded.count(canonKey) && navExcluded[canonKey];
			if (!excluded) {
				if (ImGui::MenuItem("Exclude from view")) navExcluded[canonKey] = true;
			} else {
				if (ImGui::MenuItem("Re-include in view")) navExcluded.erase(canonKey);
			}
			ImGui::EndPopup();
		}

		// Confirm delete — destructive, so gate behind a yes/no.
		// Delete sends to recycle bin on Windows (FOF_ALLOWUNDO) so it's
		// recoverable. The "Force delete" checkbox bypasses that for the
		// rare case where the user really wants permanent removal.
		static bool forceDelete = false;
		if (!navPendingDelete.empty()) ImGui::OpenPopup("##navConfirmDel");
		if (ImGui::BeginPopupModal("##navConfirmDel", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Delete %s?",
				std::filesystem::path(navPendingDelete).filename().string().c_str());
#ifdef _WIN32
			ImGui::TextDisabled(forceDelete
				? "Permanent — bypasses Recycle Bin."
				: "Sent to Recycle Bin (recoverable).");
#else
			ImGui::TextDisabled(forceDelete
				? "Permanent — bypasses trash."
				: "Sent to trash via gio (if available).");
#endif
			ImGui::Checkbox("Force delete (skip recycle bin)", &forceDelete);
			ImGui::Separator();
			if (ImGui::Button("Delete")) {
				if (forceDelete) {
					std::error_code dec;
					std::filesystem::remove_all(navPendingDelete, dec);
				} else {
					navDeletePath(navPendingDelete);
				}
				navPendingDelete.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) {
				navPendingDelete.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}
	ImGui::End();
}


void Editor::navInitDockLayout(unsigned int dockId)
{
	// One-time default layout (only when imgui.ini has no saved layout):
	//   ┌──────┬─────────────────┬────────┐
	//   │ Nav  │   documents     │  Find  │
	//   │ left │   (central)     │  Refs  │  ← right
	//   │      ├─────────────────┤ (right)│
	//   │      │   Output        │        │  ← Output spans the bottom of center
	//   └──────┴─────────────────┴────────┘
	// Documents dock into the central node (centralDockId) so new ones open
	// as tabs there. Find References → right pane, Output → bottom pane.
	if (dockLayoutInitialized) return;
	dockLayoutInitialized = true;
	if (ImGui::DockBuilderGetNode(dockId) != nullptr) {
		// A saved layout exists — don't clobber it. Docs still route to the
		// root's central node via SetNextWindowDockID below.
		centralDockId = dockId;
		return;
	}
	ImGui::DockBuilderRemoveNode(dockId);
	ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);

	ImGuiID dockMain = dockId;
	ImGuiID leftId   = 0, rightId = 0, bottomId = 0;
	ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left,  0.20f, &leftId,   &dockMain);
	ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.25f, &rightId,  &dockMain);
	ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down,  0.25f, &bottomId, &dockMain);

	ImGui::DockBuilderDockWindow("Navigation##projectNav", leftId);
	ImGui::DockBuilderDockWindow("###refsPanel",   rightId);
	ImGui::DockBuilderDockWindow("###outputPanel", bottomId);
	centralDockId = dockMain;   // documents go here as tabs
	ImGui::DockBuilderFinish(dockId);
}


// ── Manual viewport control ──────────────────────────────────────────
//
// Multi-viewport is enabled; windows only leave the main OS window when the
// user drags them out or invokes these. Pop-out undocks the active document
// and positions it just outside the main viewport (which makes ImGui give it
// its own OS window next frame). Remerge docks it back into the central node.

void Editor::popOutActiveDoc(int dir)
{
	if (tabs.empty()) return;
	auto label = windowLabelFor(*tabs[activeTab]);
	ImGui::DockBuilderDockWindow(label.c_str(), 0);   // undock → floating
	ImGuiViewport* mv = ImGui::GetMainViewport();
	float w = mv->Size.x * 0.45f;
	float x = (dir < 0) ? (mv->Pos.x - w - 8.0f)        // just left of main
	                    : (mv->Pos.x + mv->Size.x + 8.0f); // just right of main
	ImGui::SetWindowPos(label.c_str(), ImVec2(x, mv->Pos.y), ImGuiCond_Always);
	ImGui::SetWindowSize(label.c_str(), ImVec2(w, mv->Size.y * 0.9f), ImGuiCond_Always);
	tabs[activeTab]->wantFocus = true;
}

void Editor::remergeActiveWindow()
{
	if (tabs.empty()) return;
	auto label = windowLabelFor(*tabs[activeTab]);
	ImGuiID rootId = ImGui::GetID("MainDockSpace");
	ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(rootId);
	ImGuiID target = central ? central->ID
	               : (centralDockId ? (ImGuiID) centralDockId : rootId);
	ImGui::DockBuilderDockWindow(label.c_str(), target);
	ImGui::DockBuilderFinish(rootId);
	tabs[activeTab]->wantFocus = true;
}

void Editor::remergeAllWindows()
{
	// Rebuild the default docked layout AND explicitly re-dock every open
	// document into the central node. (SetNextWindowDockID with FirstUseEver
	// won't move windows that have already been positioned, so we must dock
	// them here directly.)
	ImGuiID root = ImGui::GetID("MainDockSpace");
	ImGui::DockBuilderRemoveNode(root);
	ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->WorkSize);

	ImGuiID dockMain = root, leftId = 0, rightId = 0, bottomId = 0;
	ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left,  0.20f, &leftId,   &dockMain);
	ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.25f, &rightId,  &dockMain);
	ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down,  0.25f, &bottomId, &dockMain);

	ImGui::DockBuilderDockWindow("Navigation##projectNav", leftId);
	ImGui::DockBuilderDockWindow("###refsPanel",   rightId);
	ImGui::DockBuilderDockWindow("###outputPanel", bottomId);
	for (auto& up : tabs) {
		ImGui::DockBuilderDockWindow(windowLabelFor(*up).c_str(), dockMain);
		up->dockedOnce = true;      // they're now docked where we put them
	}
	ImGui::DockBuilderFinish(root);

	centralDockId = dockMain;
	dockLayoutInitialized = true;   // we just built it; don't let the one-shot re-run
}


// ── Split current tab to the right ────────────────────────────────────
//
// Uses ImGui's DockBuilder to split the central dock node horizontally and
// move the active doc into the new right pane. Builds the split once on
// command and clears the flag, so subsequent frames don't re-split.
void Editor::splitActiveTabRight()
{
	if (tabs.size() < 2) return;             // need a second tab to split into
	wantSplitRight = true;
}

void Editor::openFileToSide(const std::string& path, int dir)
{
	openFile(path);                          // opens or focuses the file
	if (tabs.empty() || dir == 0) return;
	// The opened/focused file is the active tab; queue it to be docked into a
	// side split next frame (renderDockedDocuments does the DockBuilder work).
	pendingSideDocId = tabs[activeTab]->id;
	pendingSideDir   = dir;
}


// ── Per-project symbol index ─────────────────────────────────────────

std::shared_ptr<const Editor::ProjectIndex> Editor::indexSnapshot()
{
	std::lock_guard<std::mutex> lock(indexState->mutex);
	return indexState->index;
}

// Background walk of the project's code files. Collects every identifier token
// (for autocomplete) and the definition sites of each symbol (for Go-to-Def).
// Published atomically when done; a newer build (gen) supersedes an older one.
void Editor::rebuildProjectIndex()
{
	if (projectRoot.empty()) return;
	auto st = indexState;                       // by value → outlives Editor
	if (st->building.exchange(true)) return;    // one build at a time
	int gen = ++st->gen;
	std::filesystem::path root = projectRoot;

	std::thread([st, gen, root]() {
		auto extOk = [](const std::string& e) {
			static const std::unordered_set<std::string> ok = {
				".c",".h",".cpp",".hpp",".cxx",".hxx",".cc",".hh",".m",".mm",".inl",
				".cs",".vb",".fs",".fsx",".java",".kt",".kts",".scala",".groovy",
				".py",".pyw",".rb",".php",".pl",".js",".jsx",".ts",".tsx",".mjs",".cjs",
				".go",".rs",".swift",".lua",".sh",".ps1",".psm1",".sql",".r",".jl",".dart",
			};
			return ok.count(e) != 0;
		};
		auto skipDir = [](const std::string& n) {
			return n == ".git" || n == ".svn" || n == ".hg" || n == "node_modules" ||
			       n == "bin" || n == "obj" || n == "out" || n == "build" ||
			       n == "target" || n == ".vs" || n == ".vscode" || n == ".idea" ||
			       n == "__pycache__" || n == "packages" || n == "deps" || n == "vendor";
		};
		static const std::unordered_set<std::string> defKw = {
			"class","struct","interface","enum","record","namespace","trait",
			"typedef","type","def","fn","function","module","protocol","actor","union",
		};
		auto isIdentStart = [](char c){ return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_'; };
		auto isIdent      = [](char c){ return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'; };

		auto idx = std::make_shared<ProjectIndex>();
		std::unordered_set<std::string> idset;
		std::error_code ec;
		int budget = 12000;

		for (auto it = std::filesystem::recursive_directory_iterator(
				root, std::filesystem::directory_options::skip_permission_denied, ec);
			 it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
		{
			if (gen != st->gen.load()) { st->building = false; return; }   // superseded
			if (ec) { ec.clear(); continue; }
			if (it->is_directory(ec)) {
				if (skipDir(it->path().filename().string())) it.disable_recursion_pending();
				continue;
			}
			if (--budget < 0) break;
			auto ext = it->path().extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
			if (!extOk(ext)) continue;

			std::ifstream f(it->path());
			if (!f.is_open()) continue;
			std::string fileStr = it->path().string();
			std::string line;
			int lineNo = 0;
			while (std::getline(f, line)) {
				int curLine = lineNo++;
				size_t n = line.size();
				size_t lead = 0;
				while (lead < n && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
				bool hashLine = lead < n && line[lead] == '#';
				// A comment line still contributes its identifiers (autocomplete
				// wants them) but must NOT register definition sites — otherwise a
				// commented-out foreign snippet (C++ "-- struct FVector ..." in a
				// Lua file) indexes as a real def and Go-to-Definition jumps to it.
				bool commentLine = false;
				if (lead < n) {
					char c0 = line[lead];
					char c1 = (lead + 1 < n) ? line[lead + 1] : '\0';
					commentLine = (c0 == '/' && c1 == '/') || (c0 == '-' && c1 == '-')
						|| c0 == ';' || c0 == '*';
				}

				std::string prevTok;
				size_t i = 0;
				bool firstTok = true;
				while (i < n) {
					if (!isIdentStart(line[i])) { ++i; continue; }
					size_t s2 = i;
					while (i < n && isIdent(line[i])) ++i;
					std::string tok = line.substr(s2, i - s2);
					idset.insert(tok);

					int score = 0;
					if (commentLine) score = 0;                            // comment → never a def site
					else if (defKw.count(prevTok)) score = 100;            // class/def/... NAME
					else if (hashLine && prevTok == "define") score = 80;  // #define NAME
					else {
						size_t p = i; while (p < n && line[p] == ' ') ++p;
						// `type NAME(` — a signature, but ONLY when a real type token
						// precedes NAME. Without this, a constructor call / usage like
						// `return ImVec4(` or `= Foo(` indexes the USE site as a def.
						static const std::unordered_set<std::string> notType = {
							"return", "new", "delete", "sizeof", "throw", "case",
							"co_return", "co_await", "and", "or", "not", "if",
							"while", "for", "switch", "do", "else",
						};
						bool typeBefore = !prevTok.empty() && !notType.count(prevTok);
						if (p < n && line[p] == '(' && !firstTok && typeBefore) score = 50;   // type NAME(
						else if (firstTok && s2 == lead) {                       // NAME = at col0
							size_t q = i; while (q < n && line[q] == ' ') ++q;
							if (q < n && line[q] == '=' && (q+1 >= n || line[q+1] != '=')) score = 30;
						}
					}
					if (score > 0) {
						auto& v = idx->defs[tok];
						if (v.size() < 32) v.push_back({ fileStr, curLine, score });   // cap per symbol
					}
					prevTok = tok;
					firstTok = false;
				}
			}
		}

		idx->identifiers.assign(idset.begin(), idset.end());
		std::sort(idx->identifiers.begin(), idx->identifiers.end());

		if (gen == st->gen.load()) {
			std::lock_guard<std::mutex> lock(st->mutex);
			st->index = idx;
		}
		st->building = false;
	}).detach();
}


// ── Hover hints + Find References (imgui-bundle style) ──────────────

void Editor::renderHoverTooltip(TabDocument& t)
{
	// Track mouse idleness over the editor window. When the cursor sits
	// still on the same word for hoverDelaySec, pop a tooltip with the
	// symbol info: known/unknown, count of references in the doc, and the
	// best-effort definition line via the existing scoring heuristic.
	ImVec2 mp = ImGui::GetMousePos();
	bool stillOnSameSpot =
		std::abs(mp.x - hoverPos.x) < 2.0f && std::abs(mp.y - hoverPos.y) < 2.0f;
	if (!stillOnSameSpot) {
		hoverPos      = mp;
		hoverIdleSec  = 0.0f;
		hoverWord.clear();
	} else {
		hoverIdleSec += ImGui::GetIO().DeltaTime;
	}
	if (hoverIdleSec < hoverDelaySec) return;

	if (hoverWord.empty()) {
		hoverWord = t.editor.GetWordAtScreenPos(mp);
		if (hoverWord.empty()) { hoverIdleSec = 0.0f; return; }
	}

	if (!t.trie.contains(hoverWord)) return;

	// Count occurrences and find the best-scoring definition line.
	int refCount = 0;
	int defLine  = -1;
	{
		auto& ed = t.editor;
		int lines = ed.GetLineCount();
		// Quick whole-word scan; cheap enough for an interactive tooltip.
		for (int ln = 0; ln < lines; ++ln) {
			auto text = ed.GetLineText(ln);
			size_t pos = 0;
			while ((pos = text.find(hoverWord, pos)) != std::string::npos) {
				bool leftOk  = (pos == 0)
					|| (!std::isalnum(static_cast<unsigned char>(text[pos - 1]))
						&& text[pos - 1] != '_');
				size_t end = pos + hoverWord.size();
				bool rightOk = (end >= text.size())
					|| (!std::isalnum(static_cast<unsigned char>(text[end]))
						&& text[end] != '_');
				if (leftOk && rightOk) {
					++refCount;
					if (defLine < 0 && text.find('{', end) != std::string::npos)
						defLine = ln;
				}
				pos = end;
			}
		}
	}
	ImGui::BeginTooltip();
	ImGui::Text("%s", hoverWord.c_str());
	ImGui::Separator();
	if (defLine >= 0) ImGui::TextDisabled("definition near line %d", defLine + 1);
	ImGui::TextDisabled("%d reference%s in file", refCount, refCount == 1 ? "" : "s");
	ImGui::TextDisabled("(right-click → Find References)");
	ImGui::EndTooltip();
}


// Project-wide Go to Definition. Walks the project tree, grepping each text
// file for patterns that look like a definition of `word`. First file+line
// match wins — opens it and scrolls there. Generic across languages; works
// for C# (class/struct/interface/record/enum/method), C/C++ (struct/class/
// typedef/method/#define), Python (def/class), Lua (function/local), JS/TS
// (function/class/const/let).
void Editor::openCSharpLearn(const std::string& rawSymbol)
{
	// C# SDK types (Console, List<T>, System.Diagnostics.Process, ...) ship as
	// metadata-only reference assemblies -- no .cs source on disk to grep to. So
	// "navigate to an SDK item" means open its Microsoft Learn page. Default to
	// the Learn SEARCH endpoint (resolves for bare names, generics, locals); only
	// deep-link to the API page when the token is rooted in a known BCL namespace.
	std::string sym = rawSymbol;
	if (auto lt = sym.find('<'); lt != std::string::npos) sym.erase(lt);   // drop generics
	while (!sym.empty() && (sym.back() == ' ' || sym.back() == '.' || sym.back() == '\t')) sym.pop_back();
	while (!sym.empty() && (sym.front() == ' ' || sym.front() == '\t')) sym.erase(sym.begin());
	if (sym.empty()) return;

	auto urlEncode = [](const std::string& s) {
		static const char* hex = "0123456789ABCDEF";
		std::string out;
		for (unsigned char c : s) {
			if (std::isalnum(c) || c == '.' || c == '_' || c == '-') out.push_back((char)c);
			else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 0xF]); }
		}
		return out;
	};

	auto rootIsBcl = [&]() {
		auto dot = sym.find('.');
		if (dot == std::string::npos) return false;          // unqualified -> search
		std::string root = sym.substr(0, dot);
		return root == "System" || root == "Microsoft" || root == "Windows"
		    || root == "Internal" || root == "Mono";
	};

	// Resolve the Learn "?view=" moniker so the docs match the version the user
	// actually targets. Priority: (1) the project's <TargetFramework> from a
	// nearby .csproj — the version their code references; (2) the highest .NET
	// runtime installed locally; (3) nothing (Learn then shows its latest).
	auto dotnetView = [this]() -> std::string {
		auto tfmToView = [](std::string tfm) -> std::string {
			std::transform(tfm.begin(), tfm.end(), tfm.begin(),
				[](unsigned char c){ return (char)std::tolower(c); });
			auto majMinAt = [](const std::string& s, size_t at) -> std::string {
				size_t i = at; std::string maj, min;
				while (i < s.size() && std::isdigit((unsigned char)s[i])) maj += s[i++];
				if (i < s.size() && s[i] == '.') { ++i; while (i < s.size() && std::isdigit((unsigned char)s[i])) min += s[i++]; }
				if (maj.empty()) return "";
				return maj + "." + (min.empty() ? "0" : min);
			};
			if (tfm.rfind("netstandard", 0) == 0) { auto v = majMinAt(tfm, 11); return v.empty() ? "" : "netstandard-" + v; }
			if (tfm.rfind("netcoreapp", 0) == 0)  { auto v = majMinAt(tfm, 10); return v.empty() ? "" : "net-" + v; }
			if (tfm.rfind("net", 0) == 0 && tfm.size() > 3 && std::isdigit((unsigned char)tfm[3])) {
				if (tfm.find('.') != std::string::npos) { auto v = majMinAt(tfm, 3); return v.empty() ? "" : "net-" + v; }
				std::string d = tfm.substr(3);   // net48 -> netframework-4.8
				if (d.size() >= 2) return "netframework-" + std::string(1, d[0]) + "." + d.substr(1);
			}
			return "";
		};
		std::error_code ec;
		std::filesystem::path root = projectRoot;
		if (root.empty() && !tabs.empty() && doc().filename != "untitled")
			root = std::filesystem::path(doc().filename).parent_path();
		// 1. nearest .csproj's TargetFramework, walking up a few levels.
		std::filesystem::path cur = root;
		for (int up = 0; !cur.empty() && up < 5; ++up) {
			for (auto& e : std::filesystem::directory_iterator(cur, ec)) {
				if (ec) break;
				if (!e.is_regular_file(ec)) continue;
				auto ext = e.path().extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
				if (ext != ".csproj") continue;
				std::ifstream f(e.path());
				std::stringstream ss; ss << f.rdbuf();
				std::string text = ss.str();
				auto tag = [&](const std::string& t) -> std::string {
					auto a = text.find("<" + t + ">");
					if (a == std::string::npos) return "";
					a += t.size() + 2;
					auto b = text.find("</" + t + ">", a);
					return b == std::string::npos ? "" : text.substr(a, b - a);
				};
				std::string tfm = tag("TargetFramework");
				if (tfm.empty()) { std::string m = tag("TargetFrameworks"); auto sc = m.find(';'); tfm = (sc == std::string::npos) ? m : m.substr(0, sc); }
				while (!tfm.empty() && (tfm.back()==' '||tfm.back()=='\r'||tfm.back()=='\n'||tfm.back()=='\t')) tfm.pop_back();
				while (!tfm.empty() && (tfm.front()==' '||tfm.front()=='\t')) tfm.erase(tfm.begin());
				auto view = tfmToView(tfm);
				if (!view.empty()) return view;
			}
			if (cur.has_parent_path() && cur.parent_path() != cur) cur = cur.parent_path(); else break;
		}
#ifdef _WIN32
		// 2. highest installed runtime under the shared framework folder.
		std::filesystem::path shared = "C:/Program Files/dotnet/shared/Microsoft.NETCore.App";
		int bestMaj = -1, bestMin = -1;
		for (auto& e : std::filesystem::directory_iterator(shared, ec)) {
			if (ec) break;
			if (!e.is_directory(ec)) continue;
			std::string nm = e.path().filename().string();
			size_t i = 0; std::string a, b;
			while (i < nm.size() && std::isdigit((unsigned char)nm[i])) a += nm[i++];
			if (i < nm.size() && nm[i] == '.') { ++i; while (i < nm.size() && std::isdigit((unsigned char)nm[i])) b += nm[i++]; }
			if (a.empty() || b.empty()) continue;
			int maj = std::atoi(a.c_str()), min = std::atoi(b.c_str());
			if (maj > bestMaj || (maj == bestMaj && min > bestMin)) { bestMaj = maj; bestMin = min; }
		}
		if (bestMaj >= 0) return "net-" + std::to_string(bestMaj) + "." + std::to_string(bestMin);
#endif
		return "";
	};

	std::string url;
	if (rootIsBcl()) {
		std::string lower = sym;
		std::transform(lower.begin(), lower.end(), lower.begin(),
			[](unsigned char c){ return (char)std::tolower(c); });
		url = "https://learn.microsoft.com/en-us/dotnet/api/" + urlEncode(lower);
		std::string view = dotnetView();
		if (!view.empty()) url += "?view=" + view;
	} else {
		url = "https://learn.microsoft.com/en-us/search/?terms=" + urlEncode(sym)
		    + "&category=Reference";
	}

#ifdef _WIN32
	ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
	(void)std::system(("open \"" + url + "\"").c_str());
#else
	(void)std::system(("xdg-open \"" + url + "\"").c_str());
#endif
}


// Decompile a BCL type to real C# via ilspycmd and open it read-only. The whole
// pipeline (tool auto-install, DLL resolution, decompile) runs on a detached
// thread; pollDecompile() opens the result on the UI thread. On any failure the
// state's `error` is set and pollDecompile falls back to the Learn page.
//
// Resolution (verified against ilspycmd 10.x): most System.* types are FORWARDED
// — they aren't in their namesake runtime DLL but in System.Private.CoreLib.
// ilspycmd reports this as "... was not found in the module being decompiled,
// but only in <X>", so we try a best-guess DLL first and, on that error, parse
// the named module and retry once.
void Editor::openCSharpDecompiled(const std::string& rawSymbol)
{
#ifdef _WIN32
	if (decompileState->running.load()) return;   // one at a time

	// Last segment + generic arity: "System.Collections.Generic.List<int>" ->
	// fullName "System.Collections.Generic.List`1" for ilspycmd's -t.
	std::string full = rawSymbol;
	{
		// strip a generic argument list but remember its arity
		int arity = 0;
		if (auto lt = full.find('<'); lt != std::string::npos) {
			// count top-level commas + 1 for the arg count
			int depth = 0; arity = 1;
			for (size_t i = lt; i < full.size(); ++i) {
				if (full[i] == '<') ++depth;
				else if (full[i] == '>') { if (--depth == 0) break; }
				else if (full[i] == ',' && depth == 1) ++arity;
			}
			full = full.substr(0, lt);
		}
		while (!full.empty() && (full.back()==' '||full.back()=='.'||full.back()=='\t')) full.pop_back();
		while (!full.empty() && (full.front()==' '||full.front()=='\t')) full.erase(full.begin());
		if (arity > 0) full += "`" + std::to_string(arity);
	}
	if (full.empty()) { decompileState->error = "empty symbol"; return; }

	auto st = decompileState;
	st->running = true;
	st->done = false;
	st->published = true;     // nothing to publish yet
	{ std::lock_guard<std::mutex> lk(st->mutex); st->symbol = full; st->resultPath.clear(); st->error.clear(); }
	std::filesystem::path cacheDir = userConfigDir() / "decompiled";

	std::thread([st, full, cacheDir]() {
		auto fail = [&](const std::string& msg) {
			std::lock_guard<std::mutex> lk(st->mutex);
			st->error = msg; st->done = true; st->published = false; st->running = false;
		};
		std::error_code ec;

		// ilspycmd path; auto-install if missing.
		std::filesystem::path home = std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "";
		std::filesystem::path ilspy = home / ".dotnet" / "tools" / "ilspycmd.exe";
		if (!std::filesystem::is_regular_file(ilspy, ec)) {
			(void)std::system("dotnet tool install -g ilspycmd >NUL 2>&1");
			if (!std::filesystem::is_regular_file(ilspy, ec)) { fail("ilspycmd not available (install failed)"); return; }
		}

		// Highest installed runtime dir.
		std::filesystem::path shared = "C:/Program Files/dotnet/shared/Microsoft.NETCore.App";
		std::filesystem::path rtDir; int bestMaj = -1, bestMin = -1, bestPatch = -1;
		for (auto& e : std::filesystem::directory_iterator(shared, ec)) {
			if (ec) break;
			if (!e.is_directory(ec)) continue;
			std::string nm = e.path().filename().string();
			int v[3] = {0,0,0}; size_t i = 0;
			for (int part = 0; part < 3 && i < nm.size(); ++part) {
				std::string num;
				while (i < nm.size() && std::isdigit((unsigned char)nm[i])) num += nm[i++];
				if (i < nm.size() && nm[i] == '.') ++i;
				v[part] = num.empty() ? 0 : std::atoi(num.c_str());
			}
			if (v[0] > bestMaj || (v[0]==bestMaj && (v[1]>bestMin || (v[1]==bestMin && v[2]>bestPatch))))
			{ bestMaj=v[0]; bestMin=v[1]; bestPatch=v[2]; rtDir = e.path(); }
		}
		if (rtDir.empty()) { fail("no .NET runtime found"); return; }

		// Plain type name (no arity) for cache filename + a first DLL guess.
		std::string plain = full;
		if (auto bt = plain.find('`'); bt != std::string::npos) plain = plain.substr(0, bt);

		// Run ilspycmd <dll> -t <full>, capture stdout+stderr.
		auto runIlspy = [&](const std::filesystem::path& dll, std::string& out) -> int {
			std::string cmd = "\"" + ilspy.string() + "\" \"" + dll.string()
				+ "\" -t \"" + full + "\" 2>&1";
			FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
			if (!p) return -1;
			char buf[4096]; out.clear();
			while (fgets(buf, sizeof(buf), p)) out += buf;
			return _pclose(p);
		};

		// First guess: a DLL named after the namespace (works for non-forwarded
		// types like System.Diagnostics.Process). Fall back to CoreLib if the
		// guessed DLL doesn't exist.
		std::filesystem::path guess = rtDir / (plain.substr(0, plain.find_last_of('.')) + ".dll");
		if (plain.find('.') == std::string::npos || !std::filesystem::is_regular_file(guess, ec))
			guess = rtDir / "System.Private.CoreLib.dll";

		std::string out;
		runIlspy(guess, out);
		// Forwarded-type retry: "... only in <Module>".
		if (out.find("was not found in the module") != std::string::npos) {
			auto k = out.find("but only in ");
			if (k != std::string::npos) {
				std::string mod = out.substr(k + 12);
				// take up to end-of-line / whitespace
				size_t e2 = mod.find_first_of("\r\n \t");
				if (e2 != std::string::npos) mod = mod.substr(0, e2);
				if (!mod.empty()) {
					std::filesystem::path dll2 = rtDir / (mod + ".dll");
					if (std::filesystem::is_regular_file(dll2, ec)) runIlspy(dll2, out);
				}
			}
		}

		// Validate: output must actually contain the type, not an error/empty.
		if (out.find("was not found") != std::string::npos
			|| out.find(plain.substr(plain.find_last_of('.') + 1)) == std::string::npos
			|| out.size() < 40) {
			fail("could not decompile " + full); return;
		}

		// Cache to <config>/decompiled/<Type>.cs (sanitize the name).
		std::filesystem::create_directories(cacheDir, ec);
		std::string fname = plain;
		for (auto& c : fname) if (c=='/'||c=='\\'||c==':'||c=='<'||c=='>'||c=='`') c = '_';
		std::filesystem::path outPath = cacheDir / (fname + ".cs");
		{
			std::ofstream f(outPath, std::ios::binary);
			if (!f) { fail("cannot write cache file"); return; }
			f << "// Decompiled from " << rtDir.filename().string()
			  << " by ilspycmd — read-only. Not original source.\n\n" << out;
		}
		std::lock_guard<std::mutex> lk(st->mutex);
		st->resultPath = outPath.string();
		st->done = true; st->published = false; st->running = false;
	}).detach();
#else
	(void)rawSymbol;
#endif
}

void Editor::pollDecompile()
{
	if (decompileState->published.load()) return;
	std::string path, err, sym;
	{
		std::lock_guard<std::mutex> lk(decompileState->mutex);
		path = decompileState->resultPath;
		err  = decompileState->error;
		sym  = decompileState->symbol;
	}
	decompileState->published = true;
	if (!err.empty() || path.empty()) {
		// Fall back to the Learn page — better than nothing.
		showError("Decompile failed for '" + sym + "': " + (err.empty() ? "unknown" : err)
			+ "\nFalling back to Microsoft Learn.");
		openCSharpLearn(sym);
		return;
	}
	openFile(path);
	if (!tabs.empty()) doc().editor.SetReadOnlyEnabled(true);
}


// Test a chord string ("Ctrl+Shift+N", "Alt+O", "F6") against the live key
// state this frame: EXACT modifier set (so Ctrl+G doesn't also fire on
// Ctrl+Shift+G) plus the named key just pressed. The key name is matched via
// ImGui::GetKeyName so it round-trips with the recorder, which builds chords
// the same way. Multi-stroke chords (containing a space, e.g. "Ctrl+K Ctrl+U")
// are widget-internal and intentionally not handled here.
// Map a single key token to the name ImGui::GetKeyName returns. Chord strings
// are authored with glyphs ("=", "-", "\\", "/", "[", "]", ";", "'", ",", ".",
// "`") but GetKeyName yields words ("Equal", "Minus", "Backslash", …). Without
// this, every punctuation binding silently fails to match — which is what broke
// Ctrl+= / Ctrl+- / Ctrl+\ (zoom in/out, split right). Letter/digit/F-key/named
// tokens already match GetKeyName verbatim, so they pass through unchanged.
static std::string normalizeKeyToken(const std::string& tok)
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
	if (tok == "+")  return "KeypadAdd";   // bare '+' captured from the keypad
	return tok;
}

bool Editor::keyChordPressed(const std::string& chord) const
{
	if (chord.empty()) return false;

	// Single-combo matcher ("Ctrl+Shift+U"): exact modifier set + named key
	// pressed this frame. Used directly for one-stroke binds and as each half of
	// a two-stroke chord.
	auto matchCombo = [](const std::string& combo) -> bool {
		if (combo.empty()) return false;
		bool needCtrl = false, needShift = false, needAlt = false, needSuper = false;
		std::string keyName;
		size_t pos = 0;
		while (pos < combo.size()) {
			size_t plus = combo.find('+', pos);
			std::string tok = (plus == std::string::npos) ? combo.substr(pos)
			                                              : combo.substr(pos, plus - pos);
			if      (tok == "Ctrl")  needCtrl  = true;
			else if (tok == "Shift") needShift = true;
			else if (tok == "Alt")   needAlt   = true;
			else if (tok == "Super") needSuper = true;
			else if (!tok.empty())   keyName   = tok;
			if (plus == std::string::npos) break;
			pos = plus + 1;
			if (pos < combo.size() && combo[pos] == '+') { keyName = "+"; break; }
		}
		if (keyName.empty()) return false;
		keyName = normalizeKeyToken(keyName);
		ImGuiIO& io = ImGui::GetIO();
		if (io.KeyCtrl != needCtrl || io.KeyShift != needShift ||
		    io.KeyAlt != needAlt || io.KeySuper != needSuper)
			return false;
		for (ImGuiKey k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END;
		     k = (ImGuiKey)(k + 1))
		{
			const char* n = ImGui::GetKeyName(k);
			if (n && n[0] && keyName == n)
				return ImGui::IsKeyPressed(k, false);
		}
		return false;
	};

	// Two-stroke chord ("Ctrl+K Ctrl+U"): match the first combo to arm a pending
	// prefix, then the second within a short window. keyChordPending holds the
	// SECOND combo we're waiting for (or empty). Decay/cancel handled in
	// tickKeyChordPending(), called once per frame from render().
	auto sp = chord.find(' ');
	if (sp == std::string::npos)
		return matchCombo(chord);

	std::string first = chord.substr(0, sp);
	std::string second = chord.substr(sp + 1);
	if (!keyChordPending.empty() && keyChordPending == second) {
		if (matchCombo(second)) { keyChordPending.clear(); return true; }
		return false;   // still waiting; tick() expires it
	}
	// Not yet armed: arm when the first combo is pressed. Don't fire this frame.
	if (matchCombo(first)) {
		keyChordPending = second;
		keyChordPendingAge = 0.0f;
	}
	return false;
}

// Per-frame decay of the pending two-stroke prefix: a chord must complete within
// ~1.2s, and Escape cancels it. Called from render() before shortcut dispatch.
void Editor::tickKeyChordPending()
{
	if (keyChordPending.empty()) return;
	keyChordPendingAge += ImGui::GetIO().DeltaTime;
	if (keyChordPendingAge > 1.2f || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		keyChordPending.clear();
}

bool Editor::keybindPressed(const char* id, const char* defaultChord) const
{
	auto it = keybindOverrides.find(id);
	const std::string& chord = (it != keybindOverrides.end() && !it->second.empty())
		? it->second : std::string(defaultChord);
	return keyChordPressed(chord);
}

namespace {
	// Catalogue ids that map to a TextEditor-internal action, paired with the
	// widget action id and the editor's DEFAULT chord. Single source of truth
	// for pushing overrides into the widget; mirrors the Settings → Keybinds
	// catalogue rows whose widgetAction is non-null. Default chord is applied
	// when the user has no override, so a rebind cleanly reverts on reset.
	struct WidgetBind { const char* id; const char* widgetAction; const char* defaultChord; };
	static const WidgetBind kWidgetBinds[] = {
		{ "edit.undo",     "undo",              "Ctrl+Z" },
		{ "edit.redo",     "redo",              "Ctrl+Y" },
		{ "edit.cut",      "cut",               "Ctrl+X" },
		{ "edit.copy",     "copy",              "Ctrl+C" },
		{ "edit.paste",    "paste",             "Ctrl+V" },
		{ "edit.selAll",   "selectAll",         "Ctrl+A" },
		{ "edit.addOcc",   "addNextOccurrence", "Ctrl+D" },
		{ "edit.indent",   "indent",            "Ctrl+]" },
		{ "edit.deindent", "deindent",          "Ctrl+[" },
		{ "edit.comment",  "toggleComments",    "Ctrl+/" },
		{ "edit.selAllOcc","selectAllOccurrences","Ctrl+Shift+D" },
		{ "edit.moveUp",   "moveLineUp",        "Alt+UpArrow" },
		{ "edit.moveDown", "moveLineDown",      "Alt+DownArrow" },
		{ "find.find",     "find",              "Ctrl+F" },
		{ "find.next",     "findNext",          "F3" },
		{ "find.findAll",  "findAll",           "Ctrl+Shift+G" },
		{ "code.foldAll",  "foldAll",           "Ctrl+0" },
		{ "code.unfoldAll","unfoldAll",         "Ctrl+J" },
		{ "code.foldCur",  "foldCurrent",       "Ctrl+Shift+[" },
		{ "code.unfoldCur","unfoldCurrent",     "Ctrl+Shift+]" },
		// upperCase/lowerCase default to two-stroke Ctrl+K chords, which the
		// widget's single-combo override can't express. Only push an override
		// when the user actually rebinds them to a single chord; otherwise leave
		// the widget's built-in Ctrl+K Ctrl+U / Ctrl+K Ctrl+L default in place.
		{ "code.upper",    "upperCase",         "" },
		{ "code.lower",    "lowerCase",         "" },
	};
}

void Editor::applyKeybindOverridesToEditor(TextEditor& ed) const
{
	for (auto& wb : kWidgetBinds) {
		auto it = keybindOverrides.find(wb.id);
		bool haveOverride = (it != keybindOverrides.end() && !it->second.empty());
		// Push the override when set; otherwise push the default (or clear, for
		// the two-stroke-default actions whose defaultChord is empty).
		std::string chord = haveOverride ? it->second : std::string(wb.defaultChord);
		ed.SetKeyChordOverride(wb.widgetAction, chord);
	}
}

void Editor::applyKeybindOverridesToEditors()
{
	for (auto& tp : tabs) applyKeybindOverridesToEditor(tp->editor);
}

void Editor::goToDefinitionProjectWide(const std::string& word, bool declaration)
{
	ScopedTimer _t(declaration ? "goToDeclaration" : "goToDefinition");
	if (word.empty()) return;

	// Qualified names (System.Diagnostics.Process, std::vector, foo->bar): grep
	// for the LAST segment (the type/member name that actually appears at a
	// definition site); keep the full name for messaging + filename matching.
	std::string symbol = word;
	{
		size_t cut = symbol.size();
		for (size_t i = 0; i < symbol.size(); ++i) {
			if (symbol[i] == '.') cut = i + 1;
			else if (symbol[i] == ':' && i + 1 < symbol.size() && symbol[i+1] == ':') cut = i + 2;
			else if (symbol[i] == '-' && i + 1 < symbol.size() && symbol[i+1] == '>') cut = i + 2;
		}
		if (cut < symbol.size()) symbol = symbol.substr(cut);
	}
	if (symbol.empty()) return;

	// Search root: projectRoot if set, else the active doc's directory.
	std::filesystem::path root = projectRoot;
	if (root.empty() && !tabs.empty() && doc().filename != "untitled") {
		root = std::filesystem::path(doc().filename).parent_path();
	}
	if (root.empty()) root = std::filesystem::current_path();

	// Fast path: the prebuilt project index already knows the definition sites.
	// Definition mode only — the index can't tell a declaration (header
	// prototype) from a definition (body), so "Go to Declaration" must fall
	// through to the grep below, which applies the header/body bias. The grep
	// is now fast for declarations too because deps/ is excluded from the walk.
	if (!declaration) {
		if (auto idx = indexSnapshot()) {
			auto it = idx->defs.find(symbol);
			if (it != idx->defs.end() && !it->second.empty()) {
				const DefSite* best = nullptr;
				for (auto& d : it->second) if (!best || d.score > best->score) best = &d;
				// Only trust a STRONG index hit (a real definition keyword, score
				// 100). A weak hit (heuristic `type NAME(` / `NAME =`, score < 100)
				// can be a usage mis-scored as a def, so fall through to the grep +
				// deps-fallback below, which scores more carefully and can reach
				// bundled libraries.
				if (best && best->score >= 100) {
					openFile(best->file);
					if (!tabs.empty()) {
						auto& e = doc().editor;
						e.SetCursor(best->line, 0);
						e.SelectLine(best->line);   // highlight the whole line, not just the gutter
						e.ScrollToLine(best->line, TextEditor::Scroll::alignMiddle);
					}
					return;
				}
			}
		}
	}

	// Definition patterns — checked in order; first match wins. Keep these
	// language-agnostic where possible.
	const std::vector<std::string> defKeywords = {
		"class", "struct", "interface", "enum", "record", "namespace",
		"trait", "typedef", "type", "def", "fn", "function", "module",
		"protocol", "actor",
	};

	// File extensions worth scanning — source-y, excludes binaries and
	// build / vcs / cache dirs.
	auto extOk = [](const std::string& e) {
		static const std::unordered_set<std::string> ok = {
			".c", ".h", ".cpp", ".hpp", ".cxx", ".hxx", ".cc", ".hh",
			".m", ".mm", ".inl",
			".cs", ".vb", ".fs", ".fsx",
			".java", ".kt", ".kts", ".scala", ".groovy",
			".py", ".pyw", ".rb", ".php", ".pl",
			".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs",
			".go", ".rs", ".swift",
			".lua", ".sh", ".ps1", ".psm1",
			".sql", ".r", ".jl", ".dart",
			".cmake", ".txt",
			".xaml", ".axaml", ".xml",   // XAML/XML — lets C#↔XAML jump by x:Name / x:Class
		};
		return ok.count(e) != 0;
	};
	// deps/vendor are skipped on the FIRST pass (keeps project go-to-def fast and
	// out of library internals). If nothing is found we retry WITH them included
	// (two-pass call below), so a symbol that lives only in a bundled dependency
	// — e.g. ImVector in deps/imgui/imgui.h — is still reachable.
	bool includeDeps = false;
	auto skipDir = [&includeDeps](const std::string& name) {
		if (!includeDeps && (name == "deps" || name == "vendor")) return true;
		return name == ".git" || name == ".svn" || name == ".hg"
			|| name == "node_modules" || name == "bin" || name == "obj"
			|| name == "out" || name == "build" || name == "target"
			|| name == ".vs" || name == ".vscode" || name == ".idea"
			|| name == "__pycache__";
	};

	auto isWordBoundary = [](char c) {
		return !(std::isalnum((unsigned char) c) || c == '_');
	};

	// Score each match — favours stronger definition signals.
	struct Hit { std::filesystem::path path; int line; int score; std::string preview; };
	std::vector<Hit> hits;
	std::error_code ec;

	// One full directory walk + scan. Wrapped in a lambda so we can run it twice:
	// once with deps excluded (fast, project-only) and, if that finds nothing,
	// again with deps included (reaches symbols defined only in bundled libs).
	auto runScan = [&]() {
	int budget = 8000;   // file budget — keep walks bounded on huge projects

	for (auto it = std::filesystem::recursive_directory_iterator(
			root, std::filesystem::directory_options::skip_permission_denied, ec);
		 it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
	{
		if (ec) { ec.clear(); continue; }
		if (it->is_directory(ec)) {
			if (skipDir(it->path().filename().string())) it.disable_recursion_pending();
			continue;
		}
		if (--budget < 0) break;
		auto ext = it->path().extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(),
			[](unsigned char c){ return (char) std::tolower(c); });
		if (!extOk(ext)) continue;
		if (navIsExcluded(it->path())) continue;

		// File-level bias. Declaration mode favours headers; definition mode
		// favours implementation files. A file named after the symbol
		// (User.cs / User.h for "User") gets a bonus — "go to the file".
		bool isHeader = (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh");
		int fileBonus = 0;
		if (declaration && isHeader) fileBonus += 8;
		if (!declaration && !isHeader) fileBonus += 4;
		{
			auto stem = it->path().stem().string();
			if (stem == symbol) fileBonus += 6;
		}

		std::ifstream f(it->path());
		if (!f.is_open()) continue;
		std::string line;
		int lineNo = 0;
		while (std::getline(f, line)) {
			++lineNo;
			// Strip leading whitespace for cheap pattern checks.
			size_t s = 0;
			while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
			// Skip comment lines. A definition never lives in a comment, and
			// pasted foreign-language snippets (e.g. C++ "-- struct FVector ..."
			// commented out inside a Lua file) otherwise score as real defs and
			// hijack Go-to-Definition. Covers the common leading markers across
			// languages: // (C/C++/C#/JS), -- (Lua/SQL), # (Python/shell/Ruby),
			// ; (ini/asm), * (inside C block comments / doc comments).
			if (s < line.size()) {
				char c0 = line[s];
				char c1 = (s + 1 < line.size()) ? line[s + 1] : '\0';
				if ((c0 == '/' && c1 == '/') || (c0 == '-' && c1 == '-')
					|| c0 == '#' || c0 == ';' || c0 == '*') {
					continue;
				}
			}
			// Whole-word scan for the (last-segment) symbol.
			size_t pos = 0;
			while ((pos = line.find(symbol, pos)) != std::string::npos) {
				bool leftOk  = (pos == 0) || isWordBoundary(line[pos - 1]);
				bool rightOk = (pos + symbol.size() >= line.size())
					|| isWordBoundary(line[pos + symbol.size()]);
				if (!leftOk || !rightOk) { pos += 1; continue; }

				int score = 0;
				// Strong signals: a definition keyword immediately to the left.
				for (auto& kw : defKeywords) {
					if (pos >= kw.size() + 1) {
						size_t kwStart = pos - kw.size() - 1;
						if (line[kwStart + kw.size()] == ' ' &&
							line.compare(kwStart, kw.size(), kw) == 0 &&
							(kwStart == 0 || isWordBoundary(line[kwStart - 1])))
						{
							score = 100;
							break;
						}
					}
				}
				// Medium: #define <symbol>.
				if (score == 0 && s < line.size() && line[s] == '#') {
					if (line.find("#define", s) == s) score = 80;
				}
				// XAML/XML named element: `x:Name="symbol"`, `x:Key="symbol"`,
				// `x:Class="…symbol"`, or a bare `Name="symbol"`. The symbol is the
				// quoted value, so look just left of the match for `="` and an
				// attribute name. This is what lets Go-to-Definition jump from a
				// C# member to the XAML element that declares it (and back).
				if (score == 0 && pos >= 2 && (line[pos - 1] == '"' || line[pos - 1] == '\'')
					&& line[pos - 2] == '=')
				{
					size_t a = (pos >= 2) ? pos - 2 : 0;   // at '='
					size_t e2 = a;                          // walk back over attr name
					while (e2 > 0 && (std::isalnum((unsigned char) line[e2 - 1]) ||
						line[e2 - 1] == ':' || line[e2 - 1] == '_' || line[e2 - 1] == '.' ||
						line[e2 - 1] == '-')) --e2;
					std::string attr = line.substr(e2, a - e2);
					if (attr == "x:Name" || attr == "Name" || attr == "x:Key" ||
						attr == "x:Class" || attr == "x:Uid")
						score = 90;
				}
				// Is `pos` inside a "..." or '...' string literal on this line?
				// Count unescaped quotes before pos; an odd count = inside a string.
				// Definitions never live inside string literals, so the loose
				// `type NAME(` / `NAME =` heuristics below must NOT fire there —
				// otherwise text like Content="...with Windows (user mode)" scores
				// `Windows` as a fake definition. (The XAML attr="value" rule above
				// is deliberately exempt: it matches the quoted value on purpose.)
				bool insideString = false;
				{
					char q = 0;
					for (size_t ci = 0; ci < pos && ci < line.size(); ++ci) {
						char c = line[ci];
						if (q) { if (c == q && line[ci - 1] != '\\') q = 0; }
						else if (c == '"' || c == '\'') q = c;
					}
					insideString = (q != 0);
				}
				// Medium-weak: `<type> <symbol>(` — a method/function signature.
				// Require a type-like token immediately before the symbol so a
				// constructor CALL or type usage (`= ImVec4(`, `return Foo(`,
				// `, Bar(`) is NOT mistaken for a definition. Qualified method
				// defs (`Ret Class::Method(`) and pointer/ref/template returns
				// (`Foo* f(`, `vector<int> g(`) still count.
				if (score == 0 && !insideString) {
					size_t p = pos + symbol.size();
					while (p < line.size() && line[p] == ' ') ++p;
					if (p < line.size() && line[p] == '(' && pos > 0) {
						size_t b = pos;
						while (b > 0 && line[b - 1] == ' ') --b;
						bool typeBefore = false;
						if (b > 0) {
							char pc = line[b - 1];
							if (pc == '*' || pc == '&' || pc == '>' || pc == ':') {
								typeBefore = true;
							} else if (std::isalnum((unsigned char) pc) || pc == '_') {
								size_t s3 = b;
								while (s3 > 0 && (std::isalnum((unsigned char) line[s3 - 1]) || line[s3 - 1] == '_')) --s3;
								std::string prev = line.substr(s3, b - s3);
								static const std::unordered_set<std::string> notType = {
									"return", "new", "delete", "sizeof", "throw", "case",
									"co_return", "co_await", "and", "or", "not", "if",
									"while", "for", "switch", "do",
								};
								typeBefore = !notType.count(prev);
							}
						}
						if (typeBefore) score = 50;
					}
				}
				// Lua: a name introduced by `local` is a definition site. Matches
				// `local NAME = …` and comma lists `local a, b, c = …`; the names
				// are the identifiers before the first '=' on a `local` line. The
				// `function` keyword is excluded so `local function f` still scores
				// `f` via the strong-keyword rule above, not the bare `local`.
				if (score == 0 && ext == ".lua" && symbol != "function") {
					bool startsLocal = s + 5 <= line.size()
						&& line.compare(s, 5, "local") == 0
						&& (s + 5 >= line.size() || isWordBoundary(line[s + 5]));
					if (startsLocal && pos > s) {
						// First '=' that is a real assignment (not ==, ~=, <=, >=).
						size_t eq = std::string::npos;
						for (size_t k = s; k < line.size(); ++k) {
							char c = line[k];
							if (c == '=' && (k + 1 >= line.size() || line[k + 1] != '=')
								&& (k == 0 || (line[k - 1] != '=' && line[k - 1] != '~'
									&& line[k - 1] != '<' && line[k - 1] != '>'))) { eq = k; break; }
						}
						if (eq == std::string::npos || pos < eq) score = 70;
					}
				}
				// Weak: `<symbol> = ` top-level assignment.
				if (score == 0 && !insideString && pos == s) {
					size_t p = pos + symbol.size();
					while (p < line.size() && line[p] == ' ') ++p;
					if (p < line.size() && line[p] == '=' &&
						(p + 1 >= line.size() || line[p + 1] != '='))
					{
						score = 30;
					}
				}
				if (score > 0) {
					// Declaration vs definition tie-break: a line ending in ';'
					// is a prototype/declaration; one with '{' is a body. Nudge
					// toward the requested kind.
					bool endsSemicolon = !line.empty() && line.find_last_not_of(" \t") != std::string::npos
						&& line[line.find_last_not_of(" \t")] == ';';
					bool hasBrace = line.find('{', pos) != std::string::npos;
					if (declaration && endsSemicolon) score += 12;
					if (!declaration && hasBrace)     score += 12;
					score += fileBonus;

					Hit h;
					h.path    = it->path();
					h.line    = lineNo - 1;   // 0-based for SetCursor
					h.score   = score;
					h.preview = line.substr(0, (std::min)((size_t) 200, line.size()));
					hits.push_back(std::move(h));
				}
				pos += symbol.size();
			}
		}
	}
	};   // end runScan

	// First pass: project source only (deps excluded → fast, no library noise).
	runScan();
	// Fallback: if the project has no hit, widen to bundled deps/vendor. This is
	// the only path that reaches a symbol like ImVector that lives solely in
	// deps/imgui/imgui.h, while keeping the common case off the library tree.
	if (hits.empty()) {
		includeDeps = true;
		ec.clear();
		runScan();
	}

	if (hits.empty()) {
		showError(std::string("No ") + (declaration ? "declaration" : "definition")
			+ " of '" + word + "' found under " + root.string());
		return;
	}
	// Best score wins; ties broken by first file encountered.
	std::stable_sort(hits.begin(), hits.end(),
		[](const Hit& a, const Hit& b){ return a.score > b.score; });
	auto& best = hits.front();
	openFile(best.path.string());
	// openFile leaves the newly-opened tab as the active one — scroll it.
	if (!tabs.empty()) {
		auto& e = doc().editor;
		e.SetCursor(best.line, 0);
		e.SelectLine(best.line);   // highlight the whole line, not just the gutter
		e.ScrollToLine(best.line, TextEditor::Scroll::alignMiddle);
	}
}


void Editor::findReferencesOf(TabDocument& t, const std::string& word)
{
	referencesWord = word;
	referencesHits.clear();
	referencesFileCount = 0;
	if (word.empty()) { referencesVisible = true; return; }

	auto isBoundary = [](char c) {
		return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_');
	};
	// Whole-word scan of one text blob → append every matching line.
	auto scanText = [&](const std::string& file, std::istream& in) {
		std::string line;
		int ln = 0;
		bool counted = false;
		while (std::getline(in, line)) {
			size_t pos = 0;
			while ((pos = line.find(word, pos)) != std::string::npos) {
				bool leftOk  = (pos == 0) || isBoundary(line[pos - 1]);
				size_t end = pos + word.size();
				bool rightOk = (end >= line.size()) || isBoundary(line[end]);
				if (leftOk && rightOk) {
					std::string trimmed = line;
					size_t s = trimmed.find_first_not_of(" \t");
					if (s != std::string::npos) trimmed = trimmed.substr(s);
					referencesHits.push_back({ file, ln, trimmed });
					if (!counted) { ++referencesFileCount; counted = true; }
					break;
				}
				pos = end;
			}
			++ln;
		}
	};

	// Remember which tab this search ran against so the panel's "Search all
	// files" checkbox can re-run the same query at a wider scope.
	referencesTab = &t;

	// Default scope is the ACTIVE FILE only (its live buffer, so unsaved edits
	// count). The user widens to the whole project via the panel checkbox, which
	// sets referencesAllFiles and re-runs. Current-file scan is always cheap.
	if (!referencesAllFiles) {
		std::istringstream ss(t.editor.GetText());
		scanText(t.filename == "untitled" ? "(untitled)" : t.filename, ss);
		referencesVisible = true;
		return;
	}

	// Project-wide when we have a root (or the active doc's dir); the active
	// doc is scanned from its live buffer so unsaved edits are reflected.
	std::filesystem::path root = projectRoot;
	if (root.empty() && t.filename != "untitled")
		root = std::filesystem::path(t.filename).parent_path();

	std::string activeCanon;
	{
		std::error_code ec;
		if (t.filename != "untitled")
			activeCanon = std::filesystem::weakly_canonical(t.filename, ec).string();
	}

	if (root.empty()) {
		// No project context — just scan the active buffer.
		std::istringstream ss(t.editor.GetText());
		scanText(t.filename == "untitled" ? "(untitled)" : t.filename, ss);
		referencesVisible = true;
		return;
	}

	std::error_code ec;
	int budget = 6000;
	for (auto it = std::filesystem::recursive_directory_iterator(
			root, std::filesystem::directory_options::skip_permission_denied, ec);
		 it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
	{
		if (ec) { ec.clear(); continue; }
		if (it->is_directory(ec)) {
			auto n = it->path().filename().string();
			if (n == ".git" || n == ".svn" || n == "node_modules" || n == "bin" ||
				n == "obj" || n == "out" || n == "build" || n == "target" ||
				n == ".vs" || n == ".vscode" || n == "__pycache__")
				it.disable_recursion_pending();
			continue;
		}
		if (--budget < 0) break;
		if (!navIsCodeFile(it->path())) continue;
		auto canon = std::filesystem::weakly_canonical(it->path(), ec);
		// Skip the active doc here; we scan its live buffer separately below.
		if (!activeCanon.empty() && canon.string() == activeCanon) continue;
		std::ifstream f(it->path());
		if (!f.is_open()) continue;
		scanText(it->path().string(), f);
	}
	// Active doc from its live buffer.
	if (!activeCanon.empty()) {
		std::istringstream ss(t.editor.GetText());
		scanText(t.filename, ss);
	}
	referencesVisible = true;
}


void Editor::renderReferencesPanel()
{
	if (!referencesVisible) return;
	ImGui::SetNextWindowSize(ImVec2(440.0f, 360.0f), ImGuiCond_FirstUseEver);
	// Stable dock ID (### resets the hash seed) so the window can be pre-docked
	// to the right by navInitDockLayout regardless of the symbol in the title.
	std::string title = std::string("References: ") + referencesWord + "###refsPanel";
	if (ImGui::Begin(title.c_str(), &referencesVisible))
	{
		ImGui::TextDisabled("%zu match%s across %d file%s",
			referencesHits.size(), referencesHits.size() == 1 ? "" : "es",
			referencesFileCount, referencesFileCount == 1 ? "" : "s");
		// Scope toggle: default is the active file; tick to widen to the whole
		// project and re-run the same query against the tab it came from.
		if (ImGui::Checkbox("Search all files", &referencesAllFiles)) {
			// Guard the stored pointer: the source tab may have been closed since
			// the search ran. Only re-run if it's still a live tab.
			bool alive = false;
			for (auto& up : tabs) if (up.get() == referencesTab) { alive = true; break; }
			if (alive) findReferencesOf(*referencesTab, referencesWord);
		}
		ImGui::Separator();

		std::string lastFile;
		for (size_t i = 0; i < referencesHits.size(); ++i) {
			auto& hit = referencesHits[i];
			if (hit.file != lastFile) {
				lastFile = hit.file;
				auto leaf = std::filesystem::path(hit.file).filename().string();
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
				ImGui::TextUnformatted(leaf.empty() ? hit.file.c_str() : leaf.c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hit.file.c_str());
			}
			ImGui::PushID((int)i);
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%5d", hit.line + 1);
			if (ImGui::Selectable(buf, false, 0, ImVec2(48.0f, 0.0f))) {
				openFile(hit.file);
				if (!tabs.empty()) {
					auto& ed = doc().editor;
					ed.SetCursor(hit.line, 0);
					ed.SelectLine(hit.line);   // highlight the whole line, not just the gutter
					ed.ScrollToLine(hit.line, TextEditor::Scroll::alignMiddle);
				}
			}
			ImGui::SameLine();
			ImGui::TextDisabled("%s", hit.text.c_str());
			ImGui::PopID();
		}
	}
	ImGui::End();
}


// ── Image viewer + non-text dispatch ───────────────────────────────

bool Editor::isImageExt(const std::string& ext)
{
	return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp"
		|| ext == ".tga" || ext == ".gif" || ext == ".psd" || ext == ".hdr"
		|| ext == ".pic";
}

bool Editor::isBinaryExt(const std::string& ext)
{
	// Files we should not try to render as text. Executables are handled
	// separately (run them); everything else here hands off to the OS.
	return ext == ".exe" || ext == ".dll" || ext == ".so" || ext == ".dylib"
		|| ext == ".lib" || ext == ".a"   || ext == ".obj" || ext == ".o"
		|| ext == ".pdb" || ext == ".zip" || ext == ".7z"  || ext == ".tar"
		|| ext == ".gz"  || ext == ".rar" || ext == ".pdf" || ext == ".mp3"
		|| ext == ".mp4" || ext == ".mov" || ext == ".wav" || ext == ".ogg"
		|| ext == ".flac"|| ext == ".webm"|| ext == ".mkv" || ext == ".bin"
		|| ext == ".iso";
}

void Editor::openImageFile(const std::string& path)
{
	// If already open, focus the existing image window.
	for (auto& img : images) {
		if (img->path == path) { img->wantFocus = true; img->open = true; return; }
	}
	int w = 0, h = 0, n = 0;
	stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &n, 4);
	if (!pixels) {
		showError(std::string("Could not load image: ") + path
			+ "\n(" + (stbi_failure_reason() ? stbi_failure_reason() : "unknown") + ")");
		return;
	}
	auto img = std::make_unique<ImageDoc>();
	img->path        = path;
	img->windowTitle = std::filesystem::path(path).filename().string() + "##img:" + path;
	img->w           = w;
	img->h           = h;
	img->tex         = IM_NEW(ImTextureData)();
	img->tex->Create(ImTextureFormat_RGBA32, w, h);
	std::memcpy(img->tex->GetPixels(), pixels, (size_t) w * h * 4);
	img->tex->Status   = ImTextureStatus_WantCreate;
	img->tex->UseColors = true;
	stbi_image_free(pixels);
	// Register so PlatformIO.Textures sees this each frame. Pushing to
	// PlatformIO.Textures directly is wrong — UpdateTexturesEndFrame()
	// resize(0)'s that list every frame and rebuilds it from FontAtlases +
	// g.UserTextures, so a direct push only survives the current frame
	// (which is why the first-time upload never happened and the image
	// stayed in "loading…" forever).
	ImGui::RegisterUserTexture(img->tex);
	img->wantFocus = true;
	images.push_back(std::move(img));
}

void Editor::navShowImageThumbnail(const std::string& path)
{
	auto it = thumbCache.find(path);
	if (it == thumbCache.end()) {
		// First hover for this path — load + downscale to a thumbnail once.
		Thumb th;
		int w = 0, h = 0, n = 0;
		stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &n, 4);
		if (!pixels || w <= 0 || h <= 0) {
			if (pixels) stbi_image_free(pixels);
			th.failed = true;
			thumbCache.emplace(path, th);
			return;
		}
		// Cap the long edge so the GPU texture (and tooltip) stays small.
		const int kMax = 256;
		int tw = w, thh = h;
		if (w > kMax || h > kMax) {
			float s = (float) kMax / (float) (w > h ? w : h);
			tw = (std::max)(1, (int) (w * s));
			thh = (std::max)(1, (int) (h * s));
		}
		std::vector<stbi_uc> scaled((size_t) tw * thh * 4);   // NB: not 'small' — that's a Windows macro
		// Nearest-neighbour box sample — fine for a hover thumbnail, no deps.
		for (int y = 0; y < thh; ++y) {
			int sy = (int) ((long long) y * h / thh);
			for (int x = 0; x < tw; ++x) {
				int sx = (int) ((long long) x * w / tw);
				const stbi_uc* src = pixels + ((size_t) sy * w + sx) * 4;
				stbi_uc* dst = scaled.data() + ((size_t) y * tw + x) * 4;
				dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
			}
		}
		stbi_image_free(pixels);
		th.tex = IM_NEW(ImTextureData)();
		th.tex->Create(ImTextureFormat_RGBA32, tw, thh);
		std::memcpy(th.tex->GetPixels(), scaled.data(), scaled.size());
		th.tex->Status = ImTextureStatus_WantCreate;
		th.tex->UseColors = true;
		th.w = tw; th.h = thh;
		ImGui::RegisterUserTexture(th.tex);
		it = thumbCache.emplace(path, th).first;
	}

	Thumb& th = it->second;
	if (th.failed) { ImGui::TextDisabled("(preview unavailable)"); return; }
	if (th.tex && th.tex->Status == ImTextureStatus_OK
		&& th.tex->TexID != ImTextureID_Invalid)
		ImGui::Image(th.tex->GetTexRef(), ImVec2((float) th.w, (float) th.h));
	else
		ImGui::TextDisabled("loading preview…");
}

void Editor::renderImageWindows()
{
	for (auto it = images.begin(); it != images.end(); ) {
		auto& img = **it;
		if (!img.open) {
			// Queue the GPU texture for destroy + unregister so the next
			// EndFrame removes it from PlatformIO.Textures. The ImTextureData
			// itself gets IM_DELETE'd by ImGui's lifecycle once the backend
			// reports it Destroyed.
			if (img.tex) {
				img.tex->WantDestroyNextFrame = true;
				ImGui::UnregisterUserTexture(img.tex);
			}
			it = images.erase(it);
			continue;
		}
		if (img.wantFocus) { ImGui::SetNextWindowFocus(); img.wantFocus = false; }
		ImGui::SetNextWindowSize(ImVec2((float) (std::min)(img.w + 40, 900),
		                                (float) (std::min)(img.h + 80, 700)),
		                        ImGuiCond_FirstUseEver);
		if (ImGui::Begin(img.windowTitle.c_str(), &img.open))
		{
			ImGui::Text("%dx%d", img.w, img.h);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(120.0f);
			ImGui::SliderFloat("zoom", &img.zoom, 0.1f, 8.0f, "%.2fx");
			if (ImGui::SmallButton("1:1"))  { img.zoom = 1.0f; }
			ImGui::SameLine();
			if (ImGui::SmallButton("Fit")) {
				auto avail = ImGui::GetContentRegionAvail();
				float zx = avail.x / (float) img.w;
				float zy = (avail.y - 30.0f) / (float) img.h;
				img.zoom = (std::max)(0.05f, (std::min)(zx, zy));
			}
			ImGui::Separator();
			ImGui::BeginChild("##imgScroll", ImVec2(0, 0),
				ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
			// Only draw once the backend has uploaded the texture to the GPU.
			// On the first frame after openImageFile pushes the ImTextureData,
			// its TexID is still ImTextureID_Invalid — calling Image() then
			// generates an ImDrawCmd referencing a non-uploaded texture and
			// trips the renderer's "tex_id != Invalid" assert. Wait one frame.
			if (img.tex && img.tex->Status == ImTextureStatus_OK
				&& img.tex->TexID != ImTextureID_Invalid)
			{
				// Auto-fit on first display: scale so the whole image is
				// visible, but never magnify past 1:1 (small images stay
				// crisp at native size). Avoids a huge image opening at 1:1
				// and overflowing the window.
				if (!img.fitted && img.w > 0 && img.h > 0) {
					auto avail = ImGui::GetContentRegionAvail();
					if (avail.x > 1.0f && avail.y > 1.0f) {
						float zx = avail.x / (float) img.w;
						float zy = avail.y / (float) img.h;
						img.zoom = (std::min)(1.0f, (std::min)(zx, zy));
						img.fitted = true;
					}
				}
				ImGui::Image(img.tex->GetTexRef(),
					ImVec2(img.w * img.zoom, img.h * img.zoom));
			}
			else
			{
				ImGui::TextDisabled("loading…");
			}
			ImGui::EndChild();
		}
		ImGui::End();
		++it;
	}
}


// ── Font discovery + dynamic loading ────────────────────────────────

void Editor::discoverFonts()
{
	availableFonts.clear();
	std::vector<std::filesystem::path> roots;
#ifdef _WIN32
	if (auto win = std::getenv("WINDIR")) roots.emplace_back(std::filesystem::path(win) / "Fonts");
	else roots.emplace_back("C:\\Windows\\Fonts");
	if (auto local = std::getenv("LOCALAPPDATA"))
		roots.emplace_back(std::filesystem::path(local) / "Microsoft" / "Windows" / "Fonts");
#else
	roots.emplace_back("/usr/share/fonts");
	roots.emplace_back("/usr/local/share/fonts");
	if (auto home = std::getenv("HOME")) roots.emplace_back(std::filesystem::path(home) / ".fonts");
#endif
	std::error_code ec;
	for (auto& root : roots) {
		if (!std::filesystem::exists(root, ec)) continue;
		for (auto it = std::filesystem::recursive_directory_iterator(
				root, std::filesystem::directory_options::skip_permission_denied, ec);
			 it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
		{
			if (ec) { ec.clear(); continue; }
			if (!it->is_regular_file(ec)) continue;
			auto ext = it->path().extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(),
				[](unsigned char c) { return (char) std::tolower(c); });
			if (ext == ".ttf" || ext == ".otf")
				availableFonts.push_back(it->path().string());
		}
	}
	// Likely-monospace fonts sort first (this is a grid editor; mono is the
	// right pick). Heuristic on the filename — cheap and good enough; the
	// authoritative non-monospace warning on the active font (advance-based)
	// still fires if a mis-sorted font is selected. Within each group: A→Z.
	std::sort(availableFonts.begin(), availableFonts.end(),
		[](const std::string& a, const std::string& b) {
			auto lower = [](std::string s) {
				std::transform(s.begin(), s.end(), s.begin(),
					[](unsigned char c) { return (char) std::tolower(c); });
				return s;
			};
			std::string fa = lower(std::filesystem::path(a).filename().string());
			std::string fb = lower(std::filesystem::path(b).filename().string());
			bool ma = fontNameLooksMonospace(fa);
			bool mb = fontNameLooksMonospace(fb);
			if (ma != mb) return ma;       // monospace group first
			return fa < fb;
		});
}

// Filename heuristic for "probably a monospace font". Catches the common
// naming conventions; not authoritative (the advance-based warning is).
bool Editor::fontNameLooksMonospace(const std::string& lowerName)
{
	static const char* hints[] = {
		"mono", "consol", "courier", "cascadia", "code", "fixed", "term",
		"jetbrains", "firacode", "fira code", "hack", "inconsolata",
		"sourcecodepro", "source code", "ubuntu mono", "dejavusansmono",
		"liberation mono", "menlo", "andale", "pragmata", "iosevka", "noto mono",
		"sfmono", "victor mono", "space mono", "anonymous", "operator mono",
	};
	for (auto h : hints)
		if (lowerName.find(h) != std::string::npos) return true;
	return false;
}

void Editor::applyFont()
{
	activeFont = nullptr;
	if (fontPath.empty()) return;
	std::error_code ec;
	if (!std::filesystem::exists(fontPath, ec)) return;
	ImFontConfig cfg;
	auto leaf = std::filesystem::path(fontPath).stem().string();
	std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", leaf.c_str());
	cfg.OversampleH = 1;
	cfg.OversampleV = 1;
	// Bake at the user's editor size (not a hardcoded 15) so the atlas is sharp
	// at the size we actually render. Clamp to the Settings slider range.
	float bake = (prefFontSize >= 8.0f && prefFontSize <= 40.0f) ? prefFontSize : 15.0f;
	activeFont = ImGui::GetIO().Fonts->AddFontFromFileTTF(fontPath.c_str(), bake, &cfg);
}


// ── Settings dialog + persistence ────────────────────────────────────
//
// Tiny hand-rolled "lines of key=value" serializer at <configDir>/settings.txt.
// Sections are headers in square brackets. Avoid a real JSON dep for now
// since the surface is small.

static std::filesystem::path settingsPath()
{
	return Editor::userConfigDir() / "settings.txt";
}

void Editor::loadSettings()
{
	std::ifstream f(settingsPath());
	if (!f.is_open()) return;
	std::string section;
	std::string line;
	while (std::getline(f, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		if (line.front() == '[' && line.back() == ']') {
			section = line.substr(1, line.size() - 2);
			continue;
		}
		auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string k = line.substr(0, eq), v = line.substr(eq + 1);
		if (section == "interpreters") interpreterOverrides[k] = v;
		else if (section == "build")    projectBuildOverrides[k] = v;
		else if (section == "keybinds") { if (!v.empty()) keybindOverrides[k] = v; }
		else if (section == "editor") {
			if      (k == "auto_indent")    prefAutoIndent    = (v == "1" || v == "true");
			else if (k == "complete_pairs") prefCompletePairs = (v == "1" || v == "true");
			else if (k == "show_fps")       prefShowFps       = (v == "1" || v == "true");
			else if (k == "ctrl_scroll_zoom") prefCtrlScrollZoom = (v == "1" || v == "true");
			else if (k == "autocomplete")   autocomplete      = (v == "1" || v == "true");
			else if (k == "invert_pan")     prefInvertPan     = (v == "1" || v == "true");
			else if (k == "word_wrap")      prefWordWrap      = (v == "1" || v == "true");
			else if (k == "wrap_width")     prefWrapWidthPx   = std::atoi(v.c_str());
			else if (k == "fps_limit")      prefFpsLimit      = std::atoi(v.c_str());
			else if (k == "idle_throttle")  prefIdleThrottle  = (v == "1" || v == "true");
			else if (k == "font_size")      prefFontSize      = std::strtof(v.c_str(), nullptr);
			else if (k == "font_path")      fontPath          = v;
			else if (k == "nav_show_dot")   navShowDotFiles   = (v == "1" || v == "true");
			else if (k == "nav_show_excl")  navShowExcluded   = (v == "1" || v == "true");
			else if (k == "nav_code_only")  navCodeOnly       = (v == "1" || v == "true");
			else if (k == "nav_flat")       navFlatFiles      = (v == "1" || v == "true");
		}
		else if (section == "toolchains") {
			if      (k == "msvc")    activeMsvcPath   = v;
			else if (k == "dotnet")  activeDotnetPath = v;
			else if (k == "config")  activeBuildConfig = v;
		}
		else if (section == "state") {
			if (k == "seen_first_run") seenFirstRun = (v == "1" || v == "true");
		}
		else if (section == "recent_files") {
			if (!v.empty() &&
				std::find(recentFiles.begin(), recentFiles.end(), v) == recentFiles.end())
				recentFiles.push_back(v);
		}
		else if (section == "recent_projects") {
			if (!v.empty() &&
				std::find(recentProjects.begin(), recentProjects.end(), v) == recentProjects.end())
				recentProjects.push_back(v);
		}
		else if (section == "project_sessions") {
			// Format: <abs_root>=<file1>|<file2>|...
			std::vector<std::string> files;
			size_t pos = 0;
			while (pos < v.size()) {
				size_t bar = v.find('|', pos);
				if (bar == std::string::npos) bar = v.size();
				if (bar > pos) files.push_back(v.substr(pos, bar - pos));
				pos = bar + 1;
			}
			if (!files.empty()) projectSessions[k] = std::move(files);
		}
	}
	if (recentFiles.size()    > 20) recentFiles.resize(20);
	if (recentProjects.size() > 20) recentProjects.resize(20);
}

void Editor::rememberRecentFile(const std::string& path)
{
	if (path.empty() || path == "untitled") return;
	std::error_code ec;
	auto canon = std::filesystem::weakly_canonical(path, ec);
	std::string key = ec ? path : canon.string();
	auto it = std::find(recentFiles.begin(), recentFiles.end(), key);
	if (it != recentFiles.end()) recentFiles.erase(it);
	recentFiles.insert(recentFiles.begin(), key);
	if (recentFiles.size() > 20) recentFiles.resize(20);
}

void Editor::rememberRecentProject(const std::string& path)
{
	if (path.empty()) return;
	std::error_code ec;
	auto canon = std::filesystem::weakly_canonical(path, ec);
	std::string key = ec ? path : canon.string();
	auto it = std::find(recentProjects.begin(), recentProjects.end(), key);
	if (it != recentProjects.end()) recentProjects.erase(it);
	recentProjects.insert(recentProjects.begin(), key);
	if (recentProjects.size() > 20) recentProjects.resize(20);
}


void Editor::saveSettings()
{
	std::error_code ec;
	std::filesystem::create_directories(settingsPath().parent_path(), ec);
	std::ofstream f(settingsPath(), std::ios::trunc);
	if (!f.is_open()) return;
	f << "# imguicolortext settings — generated by the editor's Settings dialog.\n";
	f << "[editor]\n";
	f << "auto_indent="      << (prefAutoIndent     ? "1" : "0") << "\n";
	f << "complete_pairs="   << (prefCompletePairs  ? "1" : "0") << "\n";
	f << "show_fps="         << (prefShowFps        ? "1" : "0") << "\n";
	f << "ctrl_scroll_zoom=" << (prefCtrlScrollZoom ? "1" : "0") << "\n";
	f << "autocomplete="     << (autocomplete       ? "1" : "0") << "\n";
	f << "invert_pan="       << (prefInvertPan      ? "1" : "0") << "\n";
	f << "word_wrap="        << (prefWordWrap       ? "1" : "0") << "\n";
	f << "wrap_width="       << prefWrapWidthPx << "\n";
	f << "fps_limit="        << prefFpsLimit << "\n";
	f << "idle_throttle="    << (prefIdleThrottle   ? "1" : "0") << "\n";
	f << "nav_show_dot="     << (navShowDotFiles    ? "1" : "0") << "\n";
	f << "nav_show_excl="    << (navShowExcluded    ? "1" : "0") << "\n";
	f << "nav_code_only="    << (navCodeOnly        ? "1" : "0") << "\n";
	f << "nav_flat="         << (navFlatFiles       ? "1" : "0") << "\n";
	f << "font_size="        << prefFontSize << "\n";
	if (!fontPath.empty()) f << "font_path=" << fontPath << "\n";
	f << "\n";
	f << "[toolchains]\n";
	if (!activeMsvcPath.empty())   f << "msvc="   << activeMsvcPath   << "\n";
	if (!activeDotnetPath.empty()) f << "dotnet=" << activeDotnetPath << "\n";
	f << "config=" << activeBuildConfig << "\n";
	f << "\n";
	f << "[state]\n";
	f << "seen_first_run=1\n\n";
	f << "[recent_files]\n";
	for (auto& p : recentFiles)    f << "path=" << p << "\n";
	f << "\n[recent_projects]\n";
	for (auto& p : recentProjects) f << "path=" << p << "\n";
	f << "\n";
	f << "[interpreters]\n";
	for (auto& [k, v] : interpreterOverrides) f << k << "=" << v << "\n";
	f << "\n[build]\n";
	for (auto& [k, v] : projectBuildOverrides) f << k << "=" << v << "\n";
	f << "\n[keybinds]\n";
	for (auto& [k, v] : keybindOverrides) f << k << "=" << v << "\n";
	f << "\n[project_sessions]\n";
	for (auto& [root, files] : projectSessions) {
		if (files.empty()) continue;
		f << root << "=";
		for (size_t i = 0; i < files.size(); ++i) {
			if (i) f << "|";
			f << files[i];
		}
		f << "\n";
	}
}

void Editor::detectToolchains()
{
	if (toolchainsDetected) return;
	toolchainsDetected = true;
	std::error_code ec;

#ifdef _WIN32
	// Enumerate MSVC installations under each VS edition.
	for (const char* edition : { "2022", "2019" }) {
		for (const char* flavour : { "Community", "Professional", "Enterprise", "BuildTools", "Preview" }) {
			std::filesystem::path msvcRoot = std::string("C:\\Program Files\\Microsoft Visual Studio\\")
				+ edition + "\\" + flavour + "\\VC\\Tools\\MSVC";
			if (!std::filesystem::is_directory(msvcRoot, ec)) continue;
			for (auto& e : std::filesystem::directory_iterator(msvcRoot, ec)) {
				if (!e.is_directory()) continue;
				auto cl = e.path() / "bin" / "Hostx64" / "x64" / "cl.exe";
				if (std::filesystem::is_regular_file(cl, ec)) {
					std::string ver = e.path().filename().string();
					detectedMsvc.push_back({ std::string("VS ") + edition + " " + flavour + " — " + ver, cl.string() });
				}
			}
		}
	}
	// Also accept an active env var if we were launched from a Dev Prompt.
	if (const char* envVer = std::getenv("VCToolsVersion")) {
		if (const char* envDir = std::getenv("VCToolsInstallDir")) {
			std::filesystem::path cl = std::filesystem::path(envDir) / "bin" / "Hostx64" / "x64" / "cl.exe";
			detectedMsvc.push_back({ std::string("Active env — ") + envVer, cl.string() });
		}
	}
#endif

	if (activeMsvcPath.empty() && !detectedMsvc.empty()) activeMsvcPath = detectedMsvc.front().path;

	// .NET SDKs via `dotnet --list-sdks`, on a DETACHED background thread.
	// Running it synchronously here is what hung the Settings dialog: dotnet's
	// _popen can block for seconds, or indefinitely on first-run/telemetry
	// prompts. Results land in detectedDotnetSdks via pollDotnetProbe() on the
	// main thread. Captures `dotnetState` by value so it survives Editor
	// destruction (script-runner pattern).
	auto ds = dotnetState;
	std::thread([ds]() {
#ifdef _WIN32
		FILE* p = _popen("dotnet --list-sdks 2>NUL", "r");
#else
		FILE* p = popen("dotnet --list-sdks 2>/dev/null", "r");
#endif
		std::vector<DetectedTool> found;
		if (p) {
			char buf[256];
			while (fgets(buf, sizeof(buf), p)) {
				std::string line(buf);
				while (!line.empty() && (line.back()=='\n'||line.back()=='\r'||line.back()==' ')) line.pop_back();
				if (line.empty()) continue;
				auto br = line.find('[');
				std::string ver = (br != std::string::npos) ? line.substr(0, br) : line;
				while (!ver.empty() && ver.back() == ' ') ver.pop_back();
				std::string path = (br != std::string::npos && line.back() == ']')
					? line.substr(br + 1, line.size() - br - 2) : "";
				found.push_back({ ver, path });
			}
#ifdef _WIN32
			_pclose(p);
#else
			pclose(p);
#endif
		}
		std::lock_guard<std::mutex> lock(ds->mutex);
		ds->sdks = std::move(found);
		ds->done = true;
	}).detach();
}

// Publish background `dotnet --list-sdks` results into detectedDotnetSdks once
// the probe finishes. Cheap to call every frame (an atomic check until done).
void Editor::pollDotnetProbe()
{
	if (dotnetPublished || !dotnetState->done.load()) return;
	std::lock_guard<std::mutex> lock(dotnetState->mutex);
	detectedDotnetSdks = dotnetState->sdks;
	if (activeDotnetPath.empty() && !detectedDotnetSdks.empty()) activeDotnetPath = "dotnet";
	dotnetPublished = true;
}


void Editor::renderSettings()
{
	if (!settingsVisible) return;
	detectToolchains();
	pollDotnetProbe();
	ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Settings##editorSettings", &settingsVisible))
	{
		if (ImGui::BeginTabBar("##settingsTabs"))
		{
			if (ImGui::BeginTabItem("Editor"))
			{
				ImGui::Checkbox("Auto-indent on new line", &prefAutoIndent);
				ImGui::Checkbox("Auto-complete matching brackets / quotes", &prefCompletePairs);
				ImGui::Checkbox("Show FPS on status bar", &prefShowFps);
				ImGui::Checkbox("Ctrl + scroll wheel adjusts editor font size", &prefCtrlScrollZoom);
				ImGui::Checkbox("Invert middle-mouse pan direction", &prefInvertPan);
				ImGui::Checkbox("Word wrap", &prefWordWrap);
				if (prefWordWrap) {
					ImGui::SetNextItemWidth(220.0f);
					ImGui::SliderInt("Wrap width (px, 0 = fit window)", &prefWrapWidthPx, 0, 2000);
				}
				ImGui::SliderFloat("Editor font size", &prefFontSize, 10.0f, 28.0f, "%.0f");

				ImGui::Separator();
				ImGui::TextDisabled("Performance");
				// 0 = unlimited. Clamp display so the slider can reach "off".
				ImGui::SliderInt("Max FPS (0 = unlimited)", &prefFpsLimit, 0, 240);
				ImGui::Checkbox("Throttle to ~10 FPS when window unfocused (idle)", &prefIdleThrottle);

				if (!tabs.empty()) {
					auto& e = doc().editor;
					e.SetAutoIndentEnabled(prefAutoIndent);
					e.SetCompletePairedGlyphs(prefCompletePairs);
				}
				// Pan-invert + word-wrap apply to every open doc, not just the active one.
				for (auto& up : tabs) {
					up->editor.SetPanInverted(prefInvertPan);
					up->editor.SetWordWrap(prefWordWrap);
					up->editor.SetWrapWidth(static_cast<float>(prefWrapWidthPx));
				}
				fontSize = prefFontSize;

				// Font selector — enumerate system TTFs once, then pick by filename.
				// "(Bundled DejaVu)" maps to fontPath empty / activeFont nullptr.
				if (availableFonts.empty()) discoverFonts();
				std::string current = fontPath.empty()
					? std::string("(Bundled DejaVu)")
					: std::filesystem::path(fontPath).filename().string();
				if (ImGui::BeginCombo("Editor font", current.c_str())) {
					if (ImGui::Selectable("(Bundled DejaVu)", fontPath.empty())) {
						fontPath.clear();
						activeFont = nullptr;
					}
					// availableFonts is pre-sorted monospace-first. Drop a labeled
					// separator at the boundary so the recommended (mono) fonts are
					// visually grouped above the rest.
					bool emittedNonMonoHeader = false;
					for (auto& p : availableFonts) {
						std::string name = std::filesystem::path(p).filename().string();
						std::string lname = name;
						std::transform(lname.begin(), lname.end(), lname.begin(),
							[](unsigned char c){ return (char)std::tolower(c); });
						if (!emittedNonMonoHeader && !fontNameLooksMonospace(lname)) {
							emittedNonMonoHeader = true;
							ImGui::Separator();
							ImGui::TextDisabled("Other fonts (not monospace — may look uneven)");
						}
						bool selected = (p == fontPath);
						if (ImGui::Selectable(name.c_str(), selected)) {
							fontPath = p;
							applyFont();
						}
						if (selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				// This is a monospace (grid) editor: every column is one cell wide.
				// A proportional font (Comic Sans, Arial, ...) gets each glyph
				// jammed into a fixed cell, so narrow letters look gappy and wide
				// ones cramped. Detect it by comparing a narrow vs wide glyph's
				// advance under the active font and warn — the fix is to pick a
				// monospace font, not a spacing tweak.
				{
					ImFont* f = activeFont ? activeFont : ImGui::GetFont();
					if (f) {
						ImGui::PushFont(f, prefFontSize);
						float wi = ImGui::CalcTextSize("i").x;
						float wm = ImGui::CalcTextSize("M").x;
						ImGui::PopFont();
						if (wi > 0.0f && std::fabs(wm - wi) > 0.5f) {
							ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
							ImGui::TextWrapped("This font is not monospaced — text will look unevenly spaced "
								"in the editor grid. Pick a fixed-width font (e.g. Consolas, "
								"Cascadia Mono, DejaVu Sans Mono) for proper alignment.");
							ImGui::PopStyleColor();
						}
					}
				}
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Interpreters"))
			{
				ImGui::TextDisabled("Run command for F5, per file type. Empty = use OS default.");
				ImGui::Separator();
				// Each row: language label (e.g. "Python (.py, .pyw)") + input.
				// Group extensions that share an interpreter so we don't show
				// .py and .pyw as two rows for the same Python config.
				struct LangRow { const char* name; std::vector<const char*> exts; const char* def; };
				static const LangRow rows[] = {
					{ "Python",      { ".py", ".pyw" },   "python" },
					{ "PowerShell",  { ".ps1" },          "powershell -NoProfile -ExecutionPolicy Bypass -File" },
					{ "Bash",        { ".sh" },           "bash" },
					{ "Batch",       { ".bat", ".cmd" },  "" },  // direct via cmd.exe
					{ "Lua",         { ".lua" },          "lua" },
					{ "JavaScript",  { ".js" },           "node" },
				};
				for (size_t i = 0; i < IM_ARRAYSIZE(rows); ++i) {
					ImGui::PushID((int)i);
					// Label on its own line (input goes full-width below, otherwise
					// SetNextItemWidth(-FLT_MIN) pushes the right-side label off-screen).
					std::string label = rows[i].name;
					label += " (";
					for (size_t e = 0; e < rows[i].exts.size(); ++e) {
						if (e) label += ", ";
						label += rows[i].exts[e];
					}
					label += ")";
					ImGui::TextUnformatted(label.c_str());
					// Read first ext's value as the canonical, write to all exts
					// so editing once applies to .py and .pyw together.
					auto& canon = interpreterOverrides[rows[i].exts[0]];
					if (canon.empty()) canon = rows[i].def;
					char buf[256];
					std::snprintf(buf, sizeof(buf), "%s", canon.c_str());
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::InputText("##interp", buf, sizeof(buf))) {
						canon = buf;
						for (auto ext : rows[i].exts) interpreterOverrides[ext] = canon;
					}
					ImGui::Spacing();
					ImGui::PopID();
				}
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Toolchains"))
			{
				// MSVC: pick from detected installs, or hand-edit the cl.exe path.
				ImGui::TextDisabled("C / C++ compiler (MSVC). Detected installs:");
				if (detectedMsvc.empty()) {
					ImGui::BulletText("(none detected — install Visual Studio's C++ workload, or set %%VCToolsInstallDir%%)");
				} else {
					if (ImGui::BeginCombo("##msvcSel",
						activeMsvcPath.empty() ? "(none selected)" : activeMsvcPath.c_str())) {
						for (auto& t : detectedMsvc) {
							bool selected = (t.path == activeMsvcPath);
							std::string item = t.label + "   " + t.path;
							if (ImGui::Selectable(item.c_str(), selected)) activeMsvcPath = t.path;
							if (selected) ImGui::SetItemDefaultFocus();
						}
						ImGui::EndCombo();
					}
				}
				{
					char buf[512];
					std::snprintf(buf, sizeof(buf), "%s", activeMsvcPath.c_str());
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::InputText("##msvcPath", buf, sizeof(buf))) activeMsvcPath = buf;
				}
				ImGui::Spacing();
				ImGui::Separator();
				ImGui::TextDisabled(".NET SDK. Detected SDKs:");
				if (detectedDotnetSdks.empty()) {
					ImGui::BulletText("(dotnet --list-sdks returned nothing — install .NET SDK)");
				} else {
					if (ImGui::BeginCombo("##dotnetSel",
						activeDotnetPath.empty() ? "(none selected)" : activeDotnetPath.c_str())) {
						// "dotnet" is on PATH — that's the common case.
						bool isPath = (activeDotnetPath == "dotnet");
						if (ImGui::Selectable("dotnet (on PATH)", isPath)) activeDotnetPath = "dotnet";
						if (isPath) ImGui::SetItemDefaultFocus();
						for (auto& sdk : detectedDotnetSdks) {
							std::string item = "SDK " + sdk.label + "   " + sdk.path;
							bool sel = (activeDotnetPath == sdk.path);
							if (ImGui::Selectable(item.c_str(), sel)) activeDotnetPath = sdk.path;
						}
						ImGui::EndCombo();
					}
				}
				{
					char buf[512];
					std::snprintf(buf, sizeof(buf), "%s", activeDotnetPath.c_str());
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::InputText("##dotnetPath", buf, sizeof(buf))) activeDotnetPath = buf;
				}
				ImGui::Spacing();
				ImGui::Separator();
				// Build configuration — applied to MSBuild / CMake / dotnet presets.
				// Tokens %CONFIG% expand to this in any build command.
				ImGui::TextDisabled("Build configuration (substituted as %%CONFIG%% in build commands):");
				static const char* configs[] = {
					"Debug", "Release", "RelWithDebInfo", "MinSizeRel"
				};
				int curIdx = 0;
				for (int i = 0; i < IM_ARRAYSIZE(configs); ++i)
					if (activeBuildConfig == configs[i]) curIdx = i;
				if (ImGui::Combo("##buildCfg", &curIdx, configs, IM_ARRAYSIZE(configs)))
					activeBuildConfig = configs[curIdx];
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Build"))
			{
				ImGui::TextDisabled("Per-project build command (F6). Keyed by absolute project root.");
				ImGui::Separator();

				// Preset commands the user can pick from for a fresh override.
				static const char* presets[] = {
					"dotnet build",
					"msbuild /m /v:minimal",
					"cmake --build .",
					"cmake --build build",
					"cmake --build out/build/x64-Debug",
					"ninja",
					"make",
					"make -j8",
				};
				static int presetIdx = 0;
				ImGui::Combo("Preset", &presetIdx, presets, IM_ARRAYSIZE(presets));
				ImGui::SameLine();
				if (!projectRoot.empty()) {
					if (ImGui::Button("Apply to current project"))
						projectBuildOverrides[projectRoot.string()] = presets[presetIdx];
				} else {
					ImGui::TextDisabled("(no current project — Open Project... first)");
				}
				ImGui::Spacing();

				if (projectBuildOverrides.empty()) {
					ImGui::TextDisabled("(no overrides yet)");
				}
				for (auto it = projectBuildOverrides.begin(); it != projectBuildOverrides.end(); ) {
					ImGui::PushID(it->first.c_str());
					ImGui::TextDisabled("%s", it->first.c_str());
					char buf[512];
					std::snprintf(buf, sizeof(buf), "%s", it->second.c_str());
					ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80.0f);
					if (ImGui::InputText("##cmd", buf, sizeof(buf))) it->second = buf;
					ImGui::SameLine();
					if (ImGui::SmallButton("Remove")) { it = projectBuildOverrides.erase(it); ImGui::PopID(); continue; }
					ImGui::PopID();
					++it;
				}
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Keybinds"))
			{
				ImGui::TextDisabled("Click a chord to record a new one. Esc cancels, Backspace clears.");
				ImGui::Separator();
				// Editable keybind catalogue. The default chord lives here as a
				// fallback so user overrides can be diffed/reset. Overrides go
				// into `keybindOverrides` (in-memory only for now — persist in a
				// follow-up if the user actually rebinds anything).
				struct Bind { const char* id; const char* action; const char* defaultCombo; const char* group; bool rebindable; const char* widgetAction; };
				static const Bind binds[] = {
					{ "file.new",        "New tab",                  "Ctrl+N",        "File", true, nullptr },
					{ "file.open",       "Open file...",             "Ctrl+O",        "File", true, nullptr },
					{ "file.save",       "Save",                     "Ctrl+S",        "File", true, nullptr },
					{ "file.saveAs",     "Save As...",               "Ctrl+Shift+S",  "File", true, nullptr },
					{ "file.close",      "Close tab",                "Ctrl+W",        "File", true, nullptr },
					{ "file.reopen",     "Reopen last closed tab",   "Ctrl+Shift+T",  "File", true, nullptr },
					{ "file.history",    "File History",             "Ctrl+I",        "File", true, nullptr },

					{ "edit.undo",       "Undo",                     "Ctrl+Z",        "Edit", true, "undo" },
					{ "edit.redo",       "Redo",                     "Ctrl+Y",        "Edit", true, "redo" },
					{ "edit.cut",        "Cut",                      "Ctrl+X",        "Edit", true, "cut" },
					{ "edit.copy",       "Copy",                     "Ctrl+C",        "Edit", true, "copy" },
					{ "edit.paste",      "Paste",                    "Ctrl+V",        "Edit", true, "paste" },
					{ "edit.selAll",     "Select all",               "Ctrl+A",        "Edit", true, "selectAll" },
					{ "edit.addOcc",     "Add next occurrence",      "Ctrl+D",        "Edit", true, "addNextOccurrence" },
					{ "edit.selAllOcc",  "Select all occurrences",   "Ctrl+Shift+D",  "Edit", true, "selectAllOccurrences" },
					{ "edit.indent",     "Indent line(s)",           "Ctrl+]",        "Edit", true, "indent" },
					{ "edit.deindent",   "De-indent line(s)",        "Ctrl+[",        "Edit", true, "deindent" },
					{ "edit.comment",    "Toggle comments",          "Ctrl+/",        "Edit", true, "toggleComments" },
					{ "edit.moveUp",     "Move line(s) up",          "Alt+UpArrow",   "Edit", true, "moveLineUp" },
					{ "edit.moveDown",   "Move line(s) down",        "Alt+DownArrow", "Edit", true, "moveLineDown" },

					{ "find.find",       "Find",                     "Ctrl+F",        "Find", true, "find" },
					{ "find.next",       "Find next",                "F3",            "Find", true, "findNext" },
					{ "find.findAll",    "Find all",                 "Ctrl+Shift+G",  "Find", true, "findAll" },
					{ "find.goto",       "Go to Line...",            "Ctrl+G",        "Find", true, nullptr },

					{ "code.foldAll",    "Fold all",                 "Ctrl+0",        "Code", true, "foldAll" },
					{ "code.unfoldAll",  "Unfold all",               "Ctrl+J",        "Code", true, "unfoldAll" },
					{ "code.foldCur",    "Fold current",             "Ctrl+Shift+[",  "Code", true, "foldCurrent" },
					{ "code.unfoldCur",  "Unfold current",           "Ctrl+Shift+]",  "Code", true, "unfoldCurrent" },
					{ "code.upper",      "Selection -> UPPERCASE",   "Ctrl+K Ctrl+U", "Code", true, "upperCase" },
					{ "code.lower",      "Selection -> lowercase",   "Ctrl+K Ctrl+L", "Code", true, "lowerCase" },
					{ "code.hSrc",       "Switch Header / Source",   "Alt+O",         "Code", true, nullptr },

					{ "view.splitR",     "Split tab right",          "Ctrl+\\",       "View", true, nullptr },
					{ "view.zoomIn",     "Zoom in",                  "Ctrl++",        "View", true, nullptr },
					{ "view.zoomOut",    "Zoom out",                 "Ctrl+-",        "View", true, nullptr },
					{ "view.cycleNext",  "Cycle tabs forward",       "Ctrl+Tab",      "View", true, nullptr },
					{ "view.cyclePrev",  "Cycle tabs backward",      "Ctrl+Shift+Tab","View", true, nullptr },

					{ "proj.run",        "Run",                      "F5",            "Project", true, nullptr },
					{ "proj.build",      "Build project",            "F6",            "Project", true, nullptr },
				};

				static std::string capturingId;       // id of the row currently waiting for a chord
				static int         capturingFrame = 0; // frame we started — avoid catching the click that opened us
				static int         curFrame = 0; ++curFrame;
				static std::string captureStroke1;    // first combo of a two-stroke chord being recorded, or empty

				// Build the list of distinct groups in declaration order.
				std::vector<const char*> groupOrder;
				for (auto& b : binds) {
					bool present = false;
					for (auto g : groupOrder) if (std::strcmp(g, b.group) == 0) { present = true; break; }
					if (!present) groupOrder.push_back(b.group);
				}

				auto chordFor = [&](const Bind& b) -> std::string {
					auto it = keybindOverrides.find(b.id);
					return it != keybindOverrides.end() ? it->second : b.defaultCombo;
				};

				// Capture-next-chord logic. We watch IsKeyPressed across the
				// modifier-aware keys and assemble a "Mod+Key" string.
				auto tryCaptureChord = [&](const std::string& targetId) {
					if (capturingId != targetId) return;
					if (curFrame == capturingFrame) return;  // skip frame the popup opened on
					ImGuiIO& io = ImGui::GetIO();
					// A staged first stroke commits as a SINGLE chord if the user
					// lets go of Ctrl without pressing a second combo.
					if (!captureStroke1.empty() && !io.KeyCtrl) {
						keybindOverrides[targetId] = captureStroke1;
						captureStroke1.clear();
						applyKeybindOverridesToEditors();
						saveSettings();
						capturingId.clear();
						return;
					}
					if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
						capturingId.clear();
						captureStroke1.clear();
						return;
					}
					if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
						keybindOverrides.erase(targetId);
						applyKeybindOverridesToEditors();
						saveSettings();
						capturingId.clear();
						captureStroke1.clear();
						return;
					}
					// Walk every named key and grab the first non-modifier one
					// pressed this frame. We must skip BOTH the physical modifier
					// keys (Left/Right Ctrl/Shift/Alt/Super) AND the four
					// ImGuiKey_ReservedForModXXX aliases — those live inside the
					// NamedKey range too, and IsKeyPressed fires on them when a
					// modifier goes down, which previously captured garbage like
					// "Ctrl+" with no real key (the "polls mods twice" bug).
					auto isModifierKey = [](ImGuiKey k) {
						return k == ImGuiKey_LeftCtrl  || k == ImGuiKey_RightCtrl  ||
						       k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift ||
						       k == ImGuiKey_LeftAlt   || k == ImGuiKey_RightAlt   ||
						       k == ImGuiKey_LeftSuper || k == ImGuiKey_RightSuper ||
						       k == ImGuiKey_ReservedForModCtrl  ||
						       k == ImGuiKey_ReservedForModShift ||
						       k == ImGuiKey_ReservedForModAlt   ||
						       k == ImGuiKey_ReservedForModSuper;
					};
					for (ImGuiKey k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END;
						 k = (ImGuiKey)(k + 1))
					{
						if (isModifierKey(k)) continue;
						if (!ImGui::IsKeyPressed(k, false)) continue;
						// Assemble one combo: every held modifier + the key.
						std::string combo;
						if (io.KeyCtrl)  combo += "Ctrl+";
						if (io.KeyShift) combo += "Shift+";
						if (io.KeyAlt)   combo += "Alt+";
						if (io.KeySuper) combo += "Super+";
						const char* name = ImGui::GetKeyName(k);
						if (!name || !name[0]) continue;   // unnamed key — ignore
						combo += name;

						// Two-stroke recording (VSCode "Ctrl+K Ctrl+U" style): if this
						// is the FIRST combo and Ctrl is still held, stage it and keep
						// listening for a second combo. Pressing a second combo joins
						// them with a space; pressing the same first combo again (or a
						// non-Ctrl combo) commits a single chord. This makes both
						// single and chord rebinds reachable without an extra button.
						if (captureStroke1.empty()) {
							if (io.KeyCtrl) {
								// stage as potential first stroke; commit if nothing follows
								captureStroke1 = combo;
								return;   // keep capturing this frame's target
							}
							keybindOverrides[targetId] = combo;       // plain single chord
						} else {
							// We already have a first stroke. A different second combo
							// makes a two-stroke chord; repeating the first commits single.
							if (combo == captureStroke1)
								keybindOverrides[targetId] = combo;   // user repeated → single
							else
								keybindOverrides[targetId] = captureStroke1 + " " + combo;
							captureStroke1.clear();
						}
						applyKeybindOverridesToEditors();
						saveSettings();
						capturingId.clear();
						return;
					}
				};

				// One collapsible TreeNode per group, each containing its own
				// nested table so groups visually mean something.
				for (auto group : groupOrder) {
					ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
					if (!ImGui::TreeNodeEx(group, ImGuiTreeNodeFlags_DefaultOpen)) continue;
					if (ImGui::BeginTable((std::string("##kbtbl_") + group).c_str(), 3,
						ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
						ImGuiTableFlags_SizingStretchProp))
					{
						ImGui::TableSetupColumn("Action",    ImGuiTableColumnFlags_WidthStretch, 0.55f);
						ImGui::TableSetupColumn("Shortcut",  ImGuiTableColumnFlags_WidthStretch, 0.35f);
						ImGui::TableSetupColumn("",          ImGuiTableColumnFlags_WidthStretch, 0.10f);
						for (auto& b : binds) {
							if (std::strcmp(b.group, group) != 0) continue;
							ImGui::PushID(b.id);
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(b.action);
								ImGui::TableNextColumn();
								if (b.rebindable) {
									std::string label;
									if (capturingId == b.id)
										label = captureStroke1.empty()
											? std::string("press chord…")
											: (captureStroke1 + " , press 2nd (or release Ctrl)");
									else
										label = chordFor(b);
									if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0))) {
										capturingId    = b.id;
										capturingFrame = curFrame;
										captureStroke1.clear();   // fresh capture
									}
									tryCaptureChord(b.id);
								} else {
									// Editor-internal / fixed chord — shown for reference, not rebindable here.
									ImGui::TextDisabled("%s", chordFor(b).c_str());
								}
								ImGui::TableNextColumn();
								if (b.rebindable && keybindOverrides.count(b.id) != 0) {
									if (ImGui::SmallButton("reset")) { keybindOverrides.erase(b.id); applyKeybindOverridesToEditors(); saveSettings(); }
								}
							ImGui::PopID();
						}
						ImGui::EndTable();
					}
					ImGui::TreePop();
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::Separator();
		if (ImGui::Button("Save"))   { saveSettings(); }
		ImGui::SameLine();
		if (ImGui::Button("Close"))  { saveSettings(); settingsVisible = false; }
	}
	ImGui::End();
}


// ── Diff against a chosen second file ────────────────────────────────
void Editor::openDiffOtherDialog()
{
	if (auto* vp = ImGui::GetWindowViewport()) dialogViewportId = vp->ID;
	else dialogViewportId = ImGui::GetMainViewport()->ID;
	dialogNeedsPlacement = true;
	IGFD::FileDialogConfig config;
	config.countSelectionMax = 1;
	config.flags = ImGuiFileDialogFlags_DontShowHiddenFiles;
	populateFileDialogPlaces();
	ImGuiFileDialog::Instance()->OpenDialog("diff-other", "Pick file to diff against active doc...", ".*", config);
	diffOtherMode = true;
}


void Editor::openContainingFolder()
{
	if (tabs.empty() || doc().filename == "untitled") return;
#ifdef _WIN32
	// "explorer /select,<path>" highlights the file inside its folder.
	std::string cmd = "/select,\"" + doc().filename + "\"";
	ShellExecuteA(nullptr, "open", "explorer.exe", cmd.c_str(), nullptr, SW_SHOWNORMAL);
#else
	std::string cmd = "xdg-open \"" +
		std::filesystem::path(doc().filename).parent_path().string() + "\"";
	(void)std::system(cmd.c_str());
#endif
}


void Editor::reopenLastClosedTab()
{
	if (recentlyClosed.empty()) return;
	ClosedTab last = std::move(recentlyClosed.back());
	recentlyClosed.pop_back();

	auto& t = newTab();
	t.filename = last.filename;
	t.originalText = last.text;
	t.editor.SetText(last.text);
	t.editor.SetLanguage(languageForPath(last.filename));
	t.version = t.editor.GetUndoIndex();
	t.wantFocus = true;
	buildAutocompleteTrie(t);
}


//
//	Editor::windowLabelFor
//

std::string Editor::windowLabelFor(const TabDocument& t) const
{
	std::string title = std::filesystem::path(t.filename).filename().string();
	if (title.empty()) title = "untitled";
	if (t.editor.GetUndoIndex() != t.version) title += " *";
	title += "###Doc" + std::to_string(t.id);
	return title;
}


//
//	Editor::newFile
//

void Editor::newFile()
{
	newTab();
}


void Editor::newFile(std::string& path)
{
	// create a new untitled tab pre-named "path" (no read from disk)
	int idx = activeTab < tabs.size() ? static_cast<int>(activeTab) + 1 : -1;
	auto& t = newTab(path, /*split*/ false, idx);
	t.editor.SetLanguage(languageForPath(path));
}


//
//	Editor::openFile
//

void Editor::openFile()
{
	showFileOpen();
}

void Editor::openFile(const std::string& path)
{
	// Dispatch by file kind: images go to the image viewer; executables run;
	// other binary blobs open in the OS default handler. Only text-ish files
	// fall through to the editor below.
	{
		auto ext = std::filesystem::path(path).extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(),
			[](unsigned char c){ return (char)std::tolower(c); });
		if (isImageExt(ext)) {
			openImageFile(path);
			return;
		}
		if (ext == ".exe") {
			navOpenExternally(path);
			return;
		}
		if (isBinaryExt(ext)) {
			navOpenExternally(path);
			return;
		}
		// Unknown extension? Sniff the first 4 KB — if it contains NUL bytes
		// it's almost certainly binary. Hand off to the OS rather than
		// dumping garbage into the text editor.
		{
			std::ifstream f(path, std::ios::binary);
			if (f.is_open()) {
				char probe[4096];
				f.read(probe, sizeof(probe));
				std::streamsize n = f.gcount();
				for (std::streamsize i = 0; i < n; ++i) {
					if (probe[i] == '\0') {
						navOpenExternally(path);
						return;
					}
				}
			}
		}
	}
	// If this file is already open in some tab, just bring it forward instead
	// of opening a second copy. Compare canonical paths so different ways of
	// spelling the same path (relative vs absolute, mixed separators, etc.)
	// resolve to the same tab.
	{
		std::error_code ec;
		auto target = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
		if (ec) target = std::filesystem::path(path);
		for (size_t i = 0; i < tabs.size(); ++i) {
			auto& tab = *tabs[i];
			if (tab.filename == "untitled") continue;
			auto existing = std::filesystem::weakly_canonical(std::filesystem::path(tab.filename), ec);
			if (ec) existing = std::filesystem::path(tab.filename);
			if (existing == target) {
				activeTab = i;
				tab.wantFocus = true;
				return;
			}
		}
	}
	try
	{
		std::ifstream stream(path.c_str());
		std::string text;
		stream.seekg(0, std::ios::end);
		text.reserve(stream.tellg());
		stream.seekg(0, std::ios::beg);
		text.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		stream.close();

		// reuse current tab if it's an empty untitled document
		bool reuse = !tabs.empty() &&
			doc().filename == "untitled" &&
			doc().editor.IsEmpty() &&
			!isDirty();
		TabDocument* target = nullptr;
		if (reuse)
		{
			target = &doc();
		}
		else
		{
			target = &newTab();
		}

		target->originalText = text;
		target->editor.SetText(text);
		target->editor.SetLanguage(languageForPath(path));
		target->version = target->editor.GetUndoIndex();
		target->filename = path;
		target->wantFocus = true;
		rememberRecentFile(path);
		// Make this tab the active one so SetNextWindowFocus in
		// renderDockedDocuments actually points at this doc.
		for (size_t i = 0; i < tabs.size(); ++i)
		{
			if (tabs[i].get() == target) { activeTab = i; break; }
		}

		buildAutocompleteTrie(*target);

	}
	catch (std::exception& e)
	{
		showError(e.what());
	}
}


//
//	Editor::saveFile
//

void Editor::saveFile()
{
	try
	{
		auto& t = doc();
		t.editor.StripTrailingWhitespaces();
		std::ofstream stream(t.filename.c_str());
		stream << t.editor.GetText();
		stream.close();
		t.version = t.editor.GetUndoIndex();
		// Refresh the project symbol index so go-to-def / autocomplete pick up
		// edits. Cheap: the build is one-at-a-time guarded + gen-superseded.
		if (!projectRoot.empty()) rebuildProjectIndex();
	}
	catch (std::exception& e)
	{
		showError(e.what());
	}
}


void Editor::saveFile(std::string& path)
{
	// save active document under a new path; auto-detect language if unset
	try
	{
		auto& t = doc();
		t.editor.StripTrailingWhitespaces();
		std::ofstream stream(path.c_str());
		stream << t.editor.GetText();
		stream.close();
		t.filename = path;
		if (t.editor.GetLanguage() == nullptr)
			t.editor.SetLanguage(languageForPath(path));
		t.version = t.editor.GetUndoIndex();
		if (!projectRoot.empty()) rebuildProjectIndex();
	}
	catch (std::exception& e)
	{
		showError(e.what());
	}
}


//
//	Editor::render — host viewport, dockspace, per-document windows
//

void Editor::render()
{
	// Publish a finished background decompile (opens the cached .cs read-only,
	// or falls back to the Learn page on failure). Cheap atomic check per frame.
	pollDecompile();

	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGui::SetNextWindowViewport(viewport->ID);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

	ImGuiWindowFlags hostFlags =
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
		ImGuiWindowFlags_NoDocking  | ImGuiWindowFlags_MenuBar;

	ImGui::Begin("##EditorHost", nullptr, hostFlags);
	ImGui::PopStyleVar(3);

	renderMenuBar();

	// dockspace fills remaining area minus status bar
	auto& style = ImGui::GetStyle();
	float statusBarHeight = ImGui::GetFrameHeight() + 2.0f * style.WindowPadding.y;
	ImVec2 dockArea = ImGui::GetContentRegionAvail();
	dockArea.y -= statusBarHeight + style.ItemSpacing.y;

	ImGuiID dockId = ImGui::GetID("MainDockSpace");
	// One-shot default layout: Nav docked into a left split. Only runs if
	// no imgui.ini layout already exists for this dockspace.
	navInitDockLayout(dockId);
	// Sticky single-doc layout: PassthruCentralNode lets the doc fill the
	// work area cleanly. (We don't auto-hide the tab bar — the user wants
	// it always visible to track open docs.)
	ImGui::DockSpace(dockId, dockArea, ImGuiDockNodeFlags_PassthruCentralNode);

	renderDockedDocuments();
	renderNavigationPanel();
	renderImageWindows();
	renderScriptOutputWindow();
	renderReferencesPanel();
	renderSettings();

	// Diff-against-other file picker — overlay on any state.
	if (diffOtherMode) {
		auto dlg = ImGuiFileDialog::Instance();
		ImGuiViewport* vp = ImGui::FindViewportByID(dialogViewportId);
		if (!vp) vp = ImGui::GetMainViewport();
		if (dialogNeedsPlacement) {
			ImGui::SetNextWindowViewport(vp->ID);
			ImVec2 sz(vp->Size.x * 0.6f, vp->Size.y * 0.6f);
			ImVec2 pos(vp->Pos.x + (vp->Size.x - sz.x) * 0.5f,
			           vp->Pos.y + (vp->Size.y - sz.y) * 0.5f);
			ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
			ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
			dialogNeedsPlacement = false;
		}
		bool finished = dlg->Display("diff-other", ImGuiWindowFlags_NoCollapse,
			ImVec2(480.0f, 320.0f), vp->Size);
		if (finished) {
			if (dlg->IsOk() && !tabs.empty()) {
				diffOtherPath = dlg->GetFilePathName();
				std::ifstream f(diffOtherPath);
				std::string contentB((std::istreambuf_iterator<char>(f)),
				                      std::istreambuf_iterator<char>());
				auto& t = doc();
				t.diff.SetLanguage(t.editor.GetLanguage());
				t.diff.SetText(contentB, t.editor.GetText());
				state = State::diff;
			}
			diffOtherMode = false;
			dlg->Close();
		}
	}

	ImGui::Spacing();
	renderStatusBar();

	if (state == State::diff) { renderDiff(); }
	else if (state == State::openFile) { renderFileOpen(); }
	else if (state == State::saveFileAs) { renderSaveAs(); }
	else if (state == State::confirmClose) { renderConfirmClose(); }
	else if (state == State::confirmQuit) { renderConfirmQuit(); }
	else if (state == State::confirmError) { renderConfirmError(); }
	else if (state == State::gotoLine)     { renderGotoLine(); }
	else if (state == State::openProject)
	{
		// Re-uses the existing file-open dialog mechanism but on the
		// "project-open" key, opened by openProjectFolderPicker().
		ImGuiViewport* vp = ImGui::FindViewportByID(dialogViewportId);
		if (!vp) vp = ImGui::GetMainViewport();
		auto dialog = ImGuiFileDialog::Instance();
		if (dialogNeedsPlacement) {
			ImGui::SetNextWindowViewport(vp->ID);
			ImVec2 sz(vp->Size.x * 0.7f, vp->Size.y * 0.7f);
			ImVec2 pos(vp->Pos.x + (vp->Size.x - sz.x) * 0.5f,
			           vp->Pos.y + (vp->Size.y - sz.y) * 0.5f);
			ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
			ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
			dialogNeedsPlacement = false;
		}
		bool finished = dialog->Display("project-open", ImGuiWindowFlags_NoCollapse,
			ImVec2(480.0f, 320.0f), vp->Size);
		saveFileDialogPlaces();
		if (finished) {
			if (dialog->IsOk()) {
				// If the user picked a *file* (e.g. project.sln, foo.csproj),
				// use its containing directory as the project root. Otherwise
				// use the current dialog directory.
				std::string picked = dialog->GetFilePathName();
				std::error_code ec;
				std::filesystem::path target;
				if (!picked.empty() && std::filesystem::is_regular_file(picked, ec)) {
					target = std::filesystem::path(picked).parent_path();
				} else {
					target = dialog->GetCurrentPath();
				}
				setProjectRoot(target);
			}
			state = State::edit;
			dialog->Close();
		}
	}

	ImGui::End();

	// remove any tabs that got closed via window 'X'
	for (size_t i = 0; i < tabs.size();)
	{
		if (!tabs[i]->open)
		{
			if (isDirtyTab(i))
			{
				size_t toClose = i;
				tabs[i]->open = true; // prevent immediate disappearance
				activeTab = i;
				showConfirmClose([this, toClose]() { closeTab(toClose); });
				++i;
			}
			else
			{
				closeTab(i);
			}
		}
		else
		{
			++i;
		}
	}

	// guarantee at least one document exists
	if (tabs.empty()) newTab();
}


//
//	Editor::renderDockedDocuments
//

void Editor::renderDockedDocuments()
{
	// Target the dockspace's CENTRAL node for new documents. DockBuilderGetCentralNode
	// returns it regardless of the saved layout, so new docs always land in the
	// editing area as tabs — never derived from a (possibly floating/popped-out)
	// window, which is what made tabs cascade out into their own viewports.
	ImGuiID rootId = ImGui::GetID("MainDockSpace");
	ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(rootId);
	ImGuiID centralId = central ? central->ID
	                  : (centralDockId ? (ImGuiID) centralDockId : rootId);

	// Honor a pending "split right" request by splitting the central node and
	// re-docking the active doc into the new right pane.
	if (wantSplitRight && tabs.size() >= 2 && activeTab < tabs.size()) {
		ImGuiID rightId = 0, leftId = 0;
		ImGui::DockBuilderSplitNode(centralId, ImGuiDir_Right, 0.5f, &rightId, &leftId);
		auto label = windowLabelFor(*tabs[activeTab]);
		ImGui::DockBuilderDockWindow(label.c_str(), rightId);
		ImGui::DockBuilderFinish(rootId);
		tabs[activeTab]->wantFocus = true;
		tabs[activeTab]->dockedOnce = true;
		wantSplitRight = false;
	}

	// Honor a pending "open to left/right" — a file opened from the nav panel
	// that should land in a split beside the central docs, not as a tab.
	if (pendingSideDir != 0) {
		for (auto& up : tabs) {
			if (up->id != pendingSideDocId) continue;
			ImGuiID sideId = 0, restId = 0;
			ImGui::DockBuilderSplitNode(centralId,
				pendingSideDir < 0 ? ImGuiDir_Left : ImGuiDir_Right,
				0.5f, &sideId, &restId);
			ImGui::DockBuilderDockWindow(windowLabelFor(*up).c_str(), sideId);
			ImGui::DockBuilderFinish(rootId);
			up->dockedOnce = true;     // don't let the loop re-dock it to centre
			up->wantFocus  = true;
			break;
		}
		pendingSideDir = 0;
		pendingSideDocId = 0;
	}

	for (size_t i = 0; i < tabs.size(); ++i)
	{
		auto& t = *tabs[i];
		// Force a brand-new doc into the central node exactly once (Always, so
		// it actually moves), then leave it alone so the user can drag it out
		// or split it afterwards.
		if (!t.dockedOnce)
		{
			ImGui::SetNextWindowDockID(centralId, ImGuiCond_Always);
			t.dockedOnce = true;
		}
		bool focusing = t.wantFocus;
		if (focusing)
		{
			ImGui::SetNextWindowFocus();
			t.wantFocus = false;
		}
		renderDocumentWindow(t);
		if (focusing)
		{
			ImGui::SetWindowFocus(windowLabelFor(t).c_str());
		}
	}

	// "+" button on the central node's tab bar → new tab. AmendTabBar lets us
	// append our own widget to a dock node's auto-generated tab bar.
	if (central && ImGui::DockNodeBeginAmendTabBar(central))
	{
		if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
		{
			newFile();
		}
		ImGui::DockNodeEndAmendTabBar();
	}
}


//
//	Editor::renderDocumentWindow
//

void Editor::renderDocumentWindow(TabDocument& t)
{
	std::string label = windowLabelFor(t);

	bool open = t.open;
	if (!ImGui::Begin(label.c_str(), &open, ImGuiWindowFlags_NoCollapse))
	{
		t.open = open;
		ImGui::End();
		return;
	}
	t.open = open;

	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
	{
		// find this tab's index and make it active
		for (size_t i = 0; i < tabs.size(); ++i)
		{
			if (tabs[i].get() == &t) { activeTab = i; break; }
		}
	}


	ImGui::PushFont(activeFont, fontSize);
	t.editor.SetTextContextMenuCallback([this, &t](int line, int column)
										{
											// (removed) per-open trie rebuild was dead work: nothing in this menu
											// reads the trie, and autocomplete maintains its own trie on tab open + edit.

											// Resolve the "operative word" for word-aware menu items:
											//   1. If there's an active selection, use that text — respects
											//      multi-select and lets the user pick the exact symbol.
											//   2. Otherwise, the word at the right-click's (line, column).
											// The screen-position lookup we used to do was wrong: it captured
											// the layout cursor at render time (top-left of editor child),
											// so GetWordAtScreenPos returned garbage from row 0.
											std::string word;
											if (t.editor.AnyCursorHasSelection()) {
												word = t.editor.GetCurrentSelectionText();
												// Strip whitespace — multi-line selections often start/end
												// with newlines that break GoToDefinitionOf lookups.
												while (!word.empty() && (word.back() == '\n' || word.back() == '\r'
													|| word.back() == ' ' || word.back() == '\t')) word.pop_back();
												while (!word.empty() && (word.front() == '\n' || word.front() == '\r'
													|| word.front() == ' ' || word.front() == '\t'))
													word.erase(word.begin());
											}
											if (word.empty()) {
												word = t.editor.GetWordAt(line, column);
											}

											if (ImGui::MenuItem("Copy", "Ctrl-C")) { t.editor.Copy(); }
											if (ImGui::MenuItem("Cut", "Ctrl-X")) { t.editor.Cut(); }
											if (ImGui::MenuItem("Paste", "Ctrl-V")) { t.editor.Paste(); }
											if (ImGui::MenuItem("Select All", "Ctrl-A")) { t.editor.SelectAll(); }
											// Path helpers — only on whitespace right-click. If the user clicked
											// on a word the word-aware items (Go to Definition, etc.) take priority.
											if (word.empty() && t.filename != "untitled") {
												ImGui::Separator();
												if (ImGui::MenuItem("Open Containing Folder")) { openContainingFolder(); }
												if (ImGui::MenuItem("Copy File Path"))         { ImGui::SetClipboardText(t.filename.c_str()); }
												ImGui::Separator();
											}

											// "Go to file" for #include / require / import "path" or <path> on the
											// current line. Best-effort lexical match — resolves relative to the
											// current document's directory.
											std::string lineText = t.editor.GetLineText(line);
											auto extractInclude = [](const std::string& s) -> std::string
												{
													auto p1 = s.find('"');
													auto p2 = (p1 != std::string::npos) ? s.find('"', p1 + 1) : std::string::npos;
													if (p1 != std::string::npos && p2 != std::string::npos && p2 > p1 + 1)
													{
														return s.substr(p1 + 1, p2 - p1 - 1);
													}
													auto a1 = s.find('<');
													auto a2 = (a1 != std::string::npos) ? s.find('>', a1 + 1) : std::string::npos;
													if (a1 != std::string::npos && a2 != std::string::npos && a2 > a1 + 1)
													{
														return s.substr(a1 + 1, a2 - a1 - 1);
													}
													return {};
												};
											auto trim = [](std::string s)
												{
													size_t a = s.find_first_not_of(" \t");
													size_t b = s.find_last_not_of(" \t");
													return (a == std::string::npos) ? std::string{} : s.substr(a, b - a + 1);
												};
											std::string trimmed = trim(lineText);
											bool isInclude = trimmed.rfind("#include", 0) == 0
												|| trimmed.rfind("import ", 0) == 0
												|| trimmed.rfind("from ", 0) == 0;
											if (isInclude)
											{
												std::string inc = extractInclude(trimmed);
												if (!inc.empty())
												{
													// MEMOIZED — this callback re-runs every frame the popup is open and the
													// resolution below recursively walks the project tree + MSVC/Windows SDK
													// include dirs (tens of thousands of files); running it ~60x/sec froze
													// the app on any #include line. Recompute only when (doc,include) change.
													std::string memoKey = t.filename + "|" + inc;
													if (memoKey != ctxIncludeKey)
													{
														ctxIncludeKey = memoKey;
													// Resolution strategy (in order):
													//  1. <docDir>/inc                       — sibling file
													//  2. Walk up the doc's parent directories for up to 6 levels;
													//     at each level try <level>/inc and also try every dir
													//     whose name is the inc's first path segment.
													//  3. Find a project root (directory containing *.sln, *.vcxproj,
													//     CMakeLists.txt, or .git); recursively search there (cap
													//     depth at 4) for any file matching the include's basename.
													//  4. System include roots (MSVC toolchain, Windows SDK) read
													//     from environment vars; lets <vector>, <string>, etc. work.
													std::error_code ec;
													std::filesystem::path docDir = std::filesystem::path(t.filename).parent_path();
													std::filesystem::path incPath(inc);
													std::filesystem::path candidate;
													bool found = false;

													auto tryPath = [&](const std::filesystem::path& p)
														{
															if (!found && std::filesystem::is_regular_file(p, ec))
															{
																candidate = p; found = true;
															}
														};

													// 1.
													tryPath(docDir / incPath);

													// 2.
													if (!found)
													{
														auto cur = docDir;
														for (int i = 0; i < 6 && !found; ++i)
														{
															tryPath(cur / incPath);
															if (cur.has_parent_path() && cur.parent_path() != cur) cur = cur.parent_path();
															else break;
														}
													}

													// 3. Find project root, then recursive search.
													if (!found)
													{
														std::filesystem::path projectRoot;
														{
															auto cur = docDir;
															for (int i = 0; i < 8 && projectRoot.empty(); ++i)
															{
																for (auto& entry : std::filesystem::directory_iterator(cur, ec))
																{
																	if (!entry.is_regular_file() && !entry.is_directory()) continue;
																	auto name = entry.path().filename().string();
																	auto ext = entry.path().extension().string();
																	if (ext == ".sln" || ext == ".vcxproj" ||
																		name == "CMakeLists.txt" || name == ".git")
																	{
																		projectRoot = cur; break;
																	}
																}
																if (!projectRoot.empty()) break;
																if (cur.has_parent_path() && cur.parent_path() != cur) cur = cur.parent_path();
																else break;
															}
														}
														if (!projectRoot.empty())
														{
															auto wantedName = incPath.filename().string();
															auto wantedSuffix = incPath.generic_string();
															int depth = 0;
															for (auto it = std::filesystem::recursive_directory_iterator(
																projectRoot, std::filesystem::directory_options::skip_permission_denied, ec);
																!ec && it != std::filesystem::recursive_directory_iterator(); ++it)
															{
																if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
																if (++depth > 50000) break;  // safety bound
																if (!it->is_regular_file(ec)) continue;
																auto p = it->path();
																if (p.filename().string() != wantedName) continue;
																// Prefer matches whose tail matches the include (so
																// "imgui/imgui.h" doesn't pick a random "imgui.h" in
																// the build tree).
																auto pStr = p.generic_string();
																if (pStr.size() >= wantedSuffix.size() &&
																	pStr.compare(pStr.size() - wantedSuffix.size(), wantedSuffix.size(), wantedSuffix) == 0)
																{
																	candidate = p; found = true; break;
																}
																// Remember first basename match as fallback.
																if (!found) { candidate = p; found = true; }
															}
														}
													}

													// 4. System include roots — MSVC + Windows SDK. Auto-detected
													// from environment vars set by vcvarsall.bat / a Developer
													// Command Prompt. Headers like <vector>, <string>, <windows.h>
													// only resolve when the editor itself was launched from such
													// a prompt (which our build script does, so .h files like
													// <vector> work out of the box in dev builds).
													if (!found)
													{
														auto addSystemRoots = [&](std::vector<std::filesystem::path>& roots)
															{
																auto pushEnv = [&](const char* var, const char* sub = nullptr)
																	{
																		if (const char* v = std::getenv(var))
																		{
																			std::filesystem::path p(v);
																			if (sub) p /= sub;
																			roots.push_back(std::move(p));
																		}
																	};
																pushEnv("VCToolsInstallDir", "include");          // MSVC STL: <vector>, <string>, …
																pushEnv("WindowsSdkDir", "Include");          // SDK root; subfolders by version
																pushEnv("UniversalCRTSdkDir", "Include");          // ucrt
																pushEnv("INCLUDE");                                // semicolon-separated combined path
															};
														std::vector<std::filesystem::path> sysRoots;
														addSystemRoots(sysRoots);

														// %INCLUDE% is actually a semicolon-separated list, not a
														// single path — expand it.
														if (const char* incEnv = std::getenv("INCLUDE"))
														{
															std::string s = incEnv;
															std::string part;
															for (char ch : s)
															{
																if (ch == ';')
																{
																	if (!part.empty()) sysRoots.push_back(part);
																	part.clear();
																}
																else part += ch;
															}
															if (!part.empty()) sysRoots.push_back(part);
														}

														auto wantedName = incPath.filename().string();
														for (auto& root : sysRoots)
														{
															if (!std::filesystem::is_directory(root, ec)) continue;
															// First try a direct join (handles <foo.h> sitting at the root)
															auto direct = root / incPath;
															if (std::filesystem::is_regular_file(direct, ec))
															{
																candidate = direct; found = true; break;
															}
															// Then recurse one level for SDK subfolders like ucrt/, shared/, um/
															int budget = 20000;
															for (auto it = std::filesystem::recursive_directory_iterator(
																root, std::filesystem::directory_options::skip_permission_denied, ec);
																!ec && it != std::filesystem::recursive_directory_iterator(); ++it)
															{
																if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
																if (--budget < 0) break;
																if (!it->is_regular_file(ec)) continue;
																if (it->path().filename().string() == wantedName)
																{
																	candidate = it->path(); found = true; break;
																}
															}
															if (found) break;
														}
													}

													ctxIncludeFound  = found;
													ctxIncludeResult = candidate.string();
												}
													std::string label = "Go to File: " + inc;
													if (ImGui::MenuItem(label.c_str(), nullptr, false, ctxIncludeFound))
													{
														openFile(ctxIncludeResult);
													}
												}
											}

											if (!word.empty())
											{
												// Preserve any existing selection — only auto-select the word
												// under the cursor when the user right-clicked on bare text.
												if (!t.editor.AnyCursorHasSelection())
												{
													t.editor.SelectWord(line, column);
												}
												// Qualified name (System.Diagnostics, std::vector) for the
												// navigation items; plain word for in-file occurrence ops.
												std::string qualified = t.editor.GetQualifiedWordAt(line, column);
												if (qualified.empty()) qualified = word;

												// Cheap gate for the navigation items: only show Go to
												// Definition / Declaration when the (last-segment) symbol is
												// actually navigable. Uses data we ALREADY have — no extra
												// scanning:
												//   * a language keyword/declaration (if/for/class/int/...) is
												//     never a definition target → hide;
												//   * otherwise it must be known: in this file's identifier trie
												//     OR have a recorded definition site in the project index.
												// This keeps the menu honest (no "Go to Definition" that just
												// pops "not found") and is O(keywords)+O(1), only while the
												// popup is open.
												const auto* lang = t.editor.GetLanguage();
												std::string seg = qualified;
												{
													size_t cut = seg.size();
													for (size_t i = 0; i < seg.size(); ++i) {
														if (seg[i] == '.') cut = i + 1;
														else if (seg[i] == ':' && i + 1 < seg.size() && seg[i+1] == ':') cut = i + 2;
														else if (seg[i] == '-' && i + 1 < seg.size() && seg[i+1] == '>') cut = i + 2;
													}
													if (cut < seg.size()) seg = seg.substr(cut);
												}
												bool isKeyword = false;
												if (lang) {
													for (auto& kw : lang->keywords)     if (kw == seg) { isKeyword = true; break; }
													if (!isKeyword) for (auto& kw : lang->declarations) if (kw == seg) { isKeyword = true; break; }
												}
												bool known = !seg.empty() && t.trie.contains(seg);
												if (!known && !seg.empty()) if (auto idx = indexSnapshot()) known = idx->defs.count(seg) != 0;
												bool navigable = known && !isKeyword;

												// Definition = where it's defined (bodies / .cpp). Declaration =
												// where it's declared (prototypes / headers). Both project-wide,
												// both handle namespaced/qualified names.
												if (navigable && ImGui::MenuItem("Go to Definition"))
												{
													goToDefinitionProjectWide(qualified, false);
												}
												// Declaration vs definition only means something where
												// headers exist (C/C++); other languages have a single
												// definition site, so don't show it for them.
												{
													bool hasDecl = lang && (lang->name == "C" || lang->name == "C++");
													if (navigable && hasDecl && ImGui::MenuItem("Go to Declaration"))
													{
														goToDefinitionProjectWide(qualified, true);
													}
												// C# SDK symbols have no on-disk source (ref assemblies).
												// For a fully namespace-qualified BCL type we can decompile
												// the runtime assembly to real C# (ilspycmd); for anything
												// else, fall back to the Microsoft Learn docs page.
												if (lang && lang->name == "C#" && !isKeyword && !seg.empty())
												{
													bool bclRooted = false;
													if (auto dot = qualified.find('.'); dot != std::string::npos) {
														std::string r = qualified.substr(0, dot);
														bclRooted = (r == "System" || r == "Microsoft" ||
															r == "Windows" || r == "Internal" || r == "Mono");
													}
													if (bclRooted && ImGui::MenuItem("Go to Decompiled Source"))
													{
														openCSharpDecompiled(qualified);
													}
													if (ImGui::MenuItem("Look up in Microsoft Learn"))
													{
														openCSharpLearn(qualified);
													}
												}
												}
												if (ImGui::MenuItem("Find References"))
												{
													findReferencesOf(t, word);
												}
												if (ImGui::MenuItem("Select All Occurrences (this file)"))
												{
													t.editor.SelectAllOccurrencesOf(word, true, true);
												}
											}
											ImGui::Separator();
											ImGui::Text("Line %d, column %d", line + 1, column + 1);
										});

	ImVec2 editorSize = ImGui::GetContentRegionAvail();
	t.editor.Render("##editorContent", editorSize);

	// Hover hint: when the user hovers a known symbol in the editor for
	// `hoverDelaySec` without moving the mouse, show a tooltip with the
	// symbol's best-guess definition line + a reference count.
	if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
		!ImGui::IsAnyItemActive())
	{
		renderHoverTooltip(t);
	}

	ImGui::PopFont();
	ImGui::End();
}


//
//	Editor::tryToQuit
//

void Editor::tryToQuit()
{
	// Persist file-dialog favourites + editor settings BEFORE the quit path
	// can pull us out of the message loop.
	saveFileDialogPlaces();
	// Snapshot the current tab set into projectSessions for whatever
	// projectRoot is active, so next launch with the same --project arg
	// restores the workspace.
	saveCurrentProjectSession();
	saveSettings();
	// Flush the ImGui layout right now too, so window positions / docking
	// state are guaranteed-persisted even on abrupt close.
	if (auto* fn = ImGui::GetIO().IniFilename) ImGui::SaveIniSettingsToDisk(fn);

	bool anyDirty = false;
	for (size_t i = 0; i < tabs.size(); ++i)
		if (isDirtyTab(i)) { anyDirty = true; break; }

	if (anyDirty) showConfirmQuit();
	else done = true;
}


//
//	Editor::renderMenuBar
//

void Editor::renderMenuBar()
{
	if (ImGui::BeginMenuBar())
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New Tab", SHORTCUT "N")) { newFile(); }
			if (ImGui::MenuItem("Open...", SHORTCUT "O")) { openFile(); }
			if (ImGui::MenuItem("Open Project...")) { openProjectFolderPicker(); }
			// Recent lists: show the FILENAME as the menu label (full absolute
			// paths made the submenu hundreds of px wide, overflowing back over
			// the parent menu). The full path goes in a hover tooltip.
			auto recentRow = [](const std::string& path) -> bool {
				std::string leaf = std::filesystem::path(path).filename().string();
				if (leaf.empty()) leaf = path;
				bool clicked = ImGui::MenuItem(leaf.c_str());
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
					ImGui::SetTooltip("%s", path.c_str());
				return clicked;
			};
			if (ImGui::BeginMenu("Open Recent File", !recentFiles.empty())) {
				// PushID per row — repeated leaf names (or paths containing ##
				// metacharacters) would otherwise alias to the same widget ID.
				for (size_t i = 0; i < recentFiles.size(); ++i) {
					ImGui::PushID((int) i);
					if (recentRow(recentFiles[i])) openFile(recentFiles[i]);
					ImGui::PopID();
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Clear")) { recentFiles.clear(); saveSettings(); }
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Open Recent Project", !recentProjects.empty())) {
				for (size_t i = 0; i < recentProjects.size(); ++i) {
					ImGui::PushID((int) i);
					// Projects are directories — show the folder name.
					std::string leaf = std::filesystem::path(recentProjects[i]).filename().string();
					if (leaf.empty()) leaf = recentProjects[i];
					if (ImGui::MenuItem(leaf.c_str())) setProjectRoot(recentProjects[i]);
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
						ImGui::SetTooltip("%s", recentProjects[i].c_str());
					ImGui::PopID();
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Clear")) { recentProjects.clear(); saveSettings(); }
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Save", SHORTCUT "S", nullptr, isSavable())) { saveFile(); }
			if (ImGui::MenuItem("Save As...", SHORTCUT "Shift+S")) { showSaveFileAs(); }
			ImGui::Separator();
			if (ImGui::MenuItem("Close Tab", SHORTCUT "W"))
			{
				if (isDirty()) showConfirmClose([this]() { closeTab(activeTab); });
				else closeTab(activeTab);
			}
			if (ImGui::MenuItem("Reopen Closed Tab", SHORTCUT "Shift+T",
								nullptr, !recentlyClosed.empty()))
			{
				reopenLastClosedTab();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("File History…",      " " SHORTCUT "I")) { showDiff(); }
			if (ImGui::MenuItem("Diff Against File…"))                  { openDiffOtherDialog(); }
			ImGui::Separator();
			if (ImGui::MenuItem("Settings...")) { loadSettings(); applyKeybindOverridesToEditors(); settingsVisible = true; }
			ImGui::Separator();
			// Path utilities — handy when the current doc lives on disk.
			bool hasPath = !tabs.empty() && doc().filename != "untitled";
			if (ImGui::MenuItem("Open Containing Folder", nullptr, false, hasPath)) {
				openContainingFolder();
			}
			if (ImGui::MenuItem("Copy File Path", nullptr, false, hasPath)) {
				ImGui::SetClipboardText(doc().filename.c_str());
			}
			ImGui::Separator();
			// Close the application (respects unsaved-changes confirmation).
			if (ImGui::MenuItem("Exit", SHORTCUT "Q")) { tryToQuit(); }
			// "Switch Header/Source" only really applies to C/C++ files — hide
			// it for other languages so the menu stays focused.
			bool isCxxDoc = false;
			if (hasPath) {
				auto* lang = doc().editor.GetLanguage();
				if (lang) {
					const std::string& n = lang->name;
					isCxxDoc = (n == "C" || n == "C++");
				}
			}
			if (isCxxDoc) {
				ImGui::Separator();
				if (ImGui::MenuItem("Switch Header/Source", "Alt+O")) {
					toggleHeaderSource();
				}
			}
			ImGui::EndMenu();
		}

		auto& e = doc().editor;

		if (ImGui::BeginMenu("Edit"))
		{
			if (ImGui::MenuItem("Undo", " " SHORTCUT "Z", nullptr, e.CanUndo())) { e.Undo(); }
#if __APPLE__
			if (ImGui::MenuItem("Redo", "^" SHORTCUT "Z", nullptr, e.CanRedo())) { e.Redo(); }
#else
			if (ImGui::MenuItem("Redo", " " SHORTCUT "Y", nullptr, e.CanRedo())) { e.Redo(); }
#endif
			ImGui::Separator();
			if (ImGui::MenuItem("Cut", " " SHORTCUT "X", nullptr, e.AnyCursorHasSelection())) { e.Cut(); }
			if (ImGui::MenuItem("Copy", " " SHORTCUT "C", nullptr, e.AnyCursorHasSelection())) { e.Copy(); }
			if (ImGui::MenuItem("Paste", " " SHORTCUT "V", nullptr, ImGui::GetClipboardText() != nullptr)) { e.Paste(); }
			ImGui::Separator();
			bool flag;
			flag = e.IsInsertSpacesOnTabs(); if (ImGui::MenuItem("Insert Spaces on Tabs", nullptr, &flag)) { e.SetInsertSpacesOnTabs(flag); }
			if (ImGui::MenuItem("Tabs To Spaces")) { e.TabsToSpaces(); }
			if (ImGui::MenuItem("Spaces To Tabs", nullptr, nullptr, !e.IsInsertSpacesOnTabs())) { e.SpacesToTabs(); }
			if (ImGui::MenuItem("Strip Trailing Whitespaces")) { e.StripTrailingWhitespaces(); }
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Selection"))
		{
			if (ImGui::MenuItem("Select All", " " SHORTCUT "A", nullptr, !e.IsEmpty())) { e.SelectAll(); }
			ImGui::Separator();
			if (ImGui::MenuItem("Indent Line(s)", " " SHORTCUT "]", nullptr, !e.IsEmpty())) { e.IndentLines(); }
			if (ImGui::MenuItem("Deindent Line(s)", " " SHORTCUT "[", nullptr, !e.IsEmpty())) { e.DeindentLines(); }
			if (ImGui::MenuItem("Move Line(s) Up", nullptr, nullptr, !e.IsEmpty())) { e.MoveUpLines(); }
			if (ImGui::MenuItem("Move Line(s) Down", nullptr, nullptr, !e.IsEmpty())) { e.MoveDownLines(); }
			if (ImGui::MenuItem("Toggle Comments", " " SHORTCUT "/", nullptr, e.HasLanguage())) { e.ToggleComments(); }
			ImGui::Separator();
			if (ImGui::MenuItem("To Uppercase", "Ctrl-K Ctrl-U", nullptr, e.AnyCursorHasSelection())) { e.SelectionToUpperCase(); }
			if (ImGui::MenuItem("To Lowercase", "Ctrl-K Ctrl-L", nullptr, e.AnyCursorHasSelection())) { e.SelectionToLowerCase(); }
			ImGui::Separator();
			if (ImGui::MenuItem("Add Next Occurrence", " " SHORTCUT "D", nullptr, e.CurrentCursorHasSelection())) { e.AddNextOccurrence(); }
			if (ImGui::MenuItem("Select All Occurrences", "^" SHORTCUT "D", nullptr, e.CurrentCursorHasSelection())) { e.SelectAllOccurrences(); }
			ImGui::EndMenu();
		}

		// VIEW — strictly window / appearance toggles. Code-editing toggles
		// moved to Edit; folding to Code; history/diff to File; build to Project.
		if (ImGui::BeginMenu("View"))
		{
			if (ImGui::MenuItem("Zoom In",  " " SHORTCUT "+")) { increaseFontSIze(); }
			if (ImGui::MenuItem("Zoom Out", " " SHORTCUT "-")) { decreaseFontSIze(); }
			ImGui::Separator();
			bool flag;
			flag = e.IsShowLineNumbersEnabled();        if (ImGui::MenuItem("Show Line Numbers",         nullptr, &flag)) { e.SetShowLineNumbersEnabled(flag); }
			flag = e.IsShowWhitespacesEnabled();        if (ImGui::MenuItem("Show Whitespaces",          nullptr, &flag)) { e.SetShowWhitespacesEnabled(flag); }
			flag = e.IsShowSpacesEnabled();             if (ImGui::MenuItem("Show Spaces",               nullptr, &flag)) { e.SetShowSpacesEnabled(flag); }
			flag = e.IsShowTabsEnabled();               if (ImGui::MenuItem("Show Tabs",                 nullptr, &flag)) { e.SetShowTabsEnabled(flag); }
			flag = e.IsShowingMatchingBrackets();       if (ImGui::MenuItem("Show Matching Brackets",    nullptr, &flag)) { e.SetShowMatchingBrackets(flag); }
			flag = e.IsShowPanScrollIndicatorEnabled(); if (ImGui::MenuItem("Show Pan/Scroll Indicator", nullptr, &flag)) { e.SetShowPanScrollIndicatorEnabled(flag); }
			flag = e.IsMiddleMousePanMode();            if (ImGui::MenuItem("Middle Mouse Pan",          nullptr, &flag)) { if (flag) e.SetMiddleMousePanMode(); else e.SetMiddleMouseScrollMode(); }
			if (ImGui::MenuItem("Word Wrap", nullptr, &prefWordWrap)) {
				for (auto& up : tabs) up->editor.SetWordWrap(prefWordWrap);
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Navigation Panel",  nullptr, &navPanelVisible)) {}
			if (ImGui::MenuItem("Output",            "F5",    &script->visible)) {}
			ImGui::Separator();
			if (ImGui::MenuItem("Split Right",       SHORTCUT "\\")) { splitActiveTabRight(); }
			ImGui::Separator();
			if (ImGui::MenuItem("Pop Out Left",  "Ctrl+Alt+←")) { popOutActiveDoc(-1); }
			if (ImGui::MenuItem("Pop Out Right", "Ctrl+Alt+→")) { popOutActiveDoc(+1); }
			if (ImGui::MenuItem("Merge Window Back", "Ctrl+Alt+M")) { remergeActiveWindow(); }
			if (ImGui::MenuItem("Merge All Windows", "Ctrl+Alt+Shift+M")) { remergeAllWindows(); }
			ImGui::Separator();
			if (ImGui::MenuItem("Save Layout Now"))  { if (auto* fn = ImGui::GetIO().IniFilename) ImGui::SaveIniSettingsToDisk(fn); }
			if (ImGui::MenuItem("Reset Layout"))
			{
				// Clear the saved layout AND re-arm the default-layout builder so
				// next frame rebuilds Nav-left / Refs-right / Output-bottom.
				ImGui::LoadIniSettingsFromMemory("");
				ImGui::DockBuilderRemoveNode(ImGui::GetID("MainDockSpace"));
				dockLayoutInitialized = false;
				centralDockId = 0;
			}
			ImGui::EndMenu();
		}

		// CODE — fold / comments / matching-glyphs / autocomplete; everything
		// that affects the code itself, not the chrome around it.
		if (ImGui::BeginMenu("Code"))
		{
			bool flag = e.IsFoldingEnabled();
			if (ImGui::MenuItem("Enable Folding",  nullptr, &flag)) { e.SetFoldingEnabled(flag); }
			if (ImGui::MenuItem("Fold All",        " " SHORTCUT "0",       nullptr, e.IsFoldingEnabled())) { e.FoldAll(); }
			if (ImGui::MenuItem("Unfold All",      " " SHORTCUT "J",       nullptr, e.IsFoldingEnabled())) { e.UnfoldAll(); }
			if (ImGui::MenuItem("Fold Current",    " " SHORTCUT "Shift+[", nullptr, e.IsFoldingEnabled())) { e.FoldCurrent(); }
			if (ImGui::MenuItem("Unfold Current",  " " SHORTCUT "Shift+]", nullptr, e.IsFoldingEnabled())) { e.UnfoldCurrent(); }
			ImGui::Separator();
			flag = e.IsCompletingPairedGlyphs(); if (ImGui::MenuItem("Auto-complete Brackets", nullptr, &flag)) { e.SetCompletePairedGlyphs(flag); prefCompletePairs = flag; }
			flag = e.IsAutoIndentEnabled();      if (ImGui::MenuItem("Auto-indent",            nullptr, &flag)) { e.SetAutoIndentEnabled(flag); prefAutoIndent = flag; }
			if (ImGui::MenuItem("Autocomplete (Trie)", nullptr, &autocomplete)) { setAutocompleteMode(autocomplete); }
			ImGui::EndMenu();
		}

		// PROJECT — build / run / project tooling.
		if (ImGui::BeginMenu("Project"))
		{
			if (ImGui::MenuItem("Build Project",      "F6")) { runProjectBuild(); }
			if (ImGui::MenuItem("Run",                "F5")) { runProjectExeOrScript(); }
			if (ImGui::MenuItem("Run Active Document (script)")) { runScriptForDoc(); }
			ImGui::Separator();
			if (ImGui::MenuItem("Open Project...")) { openProjectFolderPicker(); }
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Find"))
		{
			if (ImGui::MenuItem("Find",        " " SHORTCUT "F"))      { e.OpenFindReplaceWindow(); }
			if (ImGui::MenuItem("Find Next",   "F3",                   nullptr, e.HasFindString())) { e.FindNext(); }
			if (ImGui::MenuItem("Find All",    "^" SHORTCUT "G",       nullptr, e.HasFindString())) { e.FindAll(); }
			ImGui::Separator();
			if (ImGui::MenuItem("Go to Line…", " " SHORTCUT "G"))      { showGotoLine(); }
			ImGui::EndMenu();
		}

		ImGui::EndMenuBar();
	}

	// global keyboard shortcuts (work whenever no input wants the keys)
	ImGuiIO& io = ImGui::GetIO();
	if (!io.WantCaptureKeyboard || ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
	{

		// App-level shortcuts dispatched through the rebindable keybind registry.
		// keybindPressed(id, default) consults the user override (if any) for the
		// id, else the default chord, and does an EXACT modifier+key match — so a
		// rebind to a different chord takes effect here, and e.g. Ctrl+G never
		// also fires under Ctrl+Shift+G. The id/default pairs mirror the Settings
		// → Keybinds catalogue. Order: more-specific (Shift) chords first where a
		// plain chord could otherwise swallow them.
		tickKeyChordPending();   // age out / cancel a half-entered two-stroke chord
		if      (keybindPressed("file.reopen", "Ctrl+Shift+T")) { reopenLastClosedTab(); }
		else if (keybindPressed("file.saveAs", "Ctrl+Shift+S")) { showSaveFileAs(); }
		else if (keybindPressed("file.new",    "Ctrl+N"))       { newFile(); }
		else if (keybindPressed("file.open",   "Ctrl+O"))       { openFile(); }
		else if (keybindPressed("file.close",  "Ctrl+W")) {
			if (isDirty()) showConfirmClose([this]() { closeTab(activeTab); });
			else closeTab(activeTab);
		}
		else if (keybindPressed("file.save",   "Ctrl+S")) {
			if (doc().filename == "untitled") showSaveFileAs();
			else saveFile();
		}
		else if (keybindPressed("file.history","Ctrl+I"))       { showDiff(); }
		else if (keybindPressed("find.goto",   "Ctrl+G"))       { showGotoLine(); }
		else if (keybindPressed("view.splitR", "Ctrl+\\"))      { splitActiveTabRight(); }
		else if (keybindPressed("view.zoomIn", "Ctrl+="))       { increaseFontSIze(); }
		else if (keybindPressed("view.zoomOut","Ctrl+-"))       { decreaseFontSIze(); }
		else if (keybindPressed("code.hSrc",   "Alt+O"))        { toggleHeaderSource(); }
		else if (keybindPressed("proj.run",    "F5"))           { runProjectExeOrScript(); }
		else if (keybindPressed("proj.build",  "F6"))           { runProjectBuild(); }

		// Tab cycling — now individually rebindable. Check the backward (Shift)
		// binding first so the default Ctrl+Shift+Tab isn't swallowed by the
		// forward Ctrl+Tab match.
		if (!tabs.empty() && keybindPressed("view.cyclePrev", "Ctrl+Shift+Tab"))
		{
			activeTab = (activeTab == 0) ? tabs.size() - 1 : activeTab - 1;
			tabs[activeTab]->wantFocus = true;
		}
		else if (!tabs.empty() && keybindPressed("view.cycleNext", "Ctrl+Tab"))
		{
			activeTab = (activeTab + 1) % tabs.size();
			tabs[activeTab]->wantFocus = true;
		}
		// Ctrl+Alt viewport control: pop the active doc out to its own OS window
		// (←/→) or merge windows back in (M = current, Shift+M = all). Fixed
		// chords (multi-action group), not individually rebindable.
		if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyDown(ImGuiMod_Alt))
		{
			if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false)) popOutActiveDoc(-1);
			else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) popOutActiveDoc(+1);
			else if (ImGui::IsKeyPressed(ImGuiKey_M, false))
			{
				if (ImGui::IsKeyDown(ImGuiMod_Shift)) remergeAllWindows();
				else remergeActiveWindow();
			}
		}
		// Ctrl + scroll wheel: nudge editor font size. Toggleable in Settings
		// because some users prefer Ctrl+wheel to scroll their navigation, not
		// rescale text. Skip when an input widget is focused so it doesn't
		// fight with the find/replace text box.
		if (prefCtrlScrollZoom && ImGui::IsKeyDown(ImGuiMod_Ctrl) && !ImGui::IsAnyItemActive())
		{
			float w = ImGui::GetIO().MouseWheel;
			if (w > 0.0f) { increaseFontSIze(); prefFontSize = fontSize; }
			else if (w < 0.0f) { decreaseFontSIze(); prefFontSize = fontSize; }
		}
	}
}


//
//	Editor::renderStatusBar
//

void Editor::renderStatusBar()
{
	// Built-in language list + every runtime-defined language discovered at
	// startup, de-duplicated by name. Cached after first build.
	static std::vector<std::string> langNamesV;
	static std::vector<const TextEditor::Language*> langDefsV;
	if (langNamesV.empty()) {
		langNamesV = { "None", "C", "C++", "C#", "AngelScript", "Lua", "Python", "GLSL", "HLSL", "JSON", "Markdown", "SQL", "INI" };
		langDefsV  = {
			nullptr,
			TextEditor::Language::C(),
			TextEditor::Language::Cpp(),
			TextEditor::Language::Cs(),
			TextEditor::Language::AngelScript(),
			TextEditor::Language::Lua(),
			TextEditor::Language::Python(),
			TextEditor::Language::Glsl(),
			TextEditor::Language::Hlsl(),
			TextEditor::Language::Json(),
			TextEditor::Language::Markdown(),
			TextEditor::Language::Sql(),
			TextEditor::Language::Ini()
		};
		// Append runtime languages (HTML, INI, YAML, CFG, BAT, PS1, CMake, XML, XAML…)
		std::unordered_set<std::string> seen(langNamesV.begin(), langNamesV.end());
		for (auto& [ext, lang] : runtimeLanguagesByExt()) {
			(void)ext;
			if (!lang || seen.count(lang->name)) continue;
			seen.insert(lang->name);
			langNamesV.push_back(lang->name);
			langDefsV.push_back(lang);
		}
	}

	auto& t = doc();
	auto& e = t.editor;
	std::string langName = e.GetLanguageName();

	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
	ImGui::BeginChild("StatusBar", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
	ImGui::SetNextItemWidth(120.0f);

	if (ImGui::BeginCombo("##LangSel", langName.c_str()))
	{
		for (size_t n = 0; n < langNamesV.size(); n++)
		{
			bool selected = (langName == langNamesV[n]);
			if (ImGui::Selectable(langNamesV[n].c_str(), selected))
			{
				e.SetLanguage(langDefsV[n]);
				buildAutocompleteTrie(t);
			}
			if (selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	ImGui::SameLine(0.0f, 0.0f);
	ImGui::AlignTextToFramePadding();

	// Toolchain selector — only when a project is loaded AND the active doc
	// language is one we recognize a toolchain for. Renders as a combo so
	// the user can switch active MSVC / .NET inline without leaving the
	// editor. Selection persists via saveSettings on quit.
	const auto* langPtr = e.GetLanguage();
	if (langPtr && !projectRoot.empty()) {
		const std::string& ln = langPtr->name;
		if (ln == "C" || ln == "C++") {
			detectToolchains();
			std::string label = "MSVC: ";
			if (activeMsvcPath.empty()) label += "(none)";
			else {
				auto p = std::filesystem::path(activeMsvcPath);
				for (auto it = p; !it.empty() && it.has_parent_path(); it = it.parent_path()) {
					auto name = it.filename().string();
					if (!name.empty() && (name[0] >= '0' && name[0] <= '9') && name.find('.') != std::string::npos) {
						label += name; break;
					}
				}
			}
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
			ImGui::SetNextItemWidth(220.0f);
			if (ImGui::BeginCombo("##msvcStatusSel", label.c_str())) {
				for (auto& tc : detectedMsvc) {
					bool sel = (tc.path == activeMsvcPath);
					if (ImGui::Selectable(tc.label.c_str(), sel)) activeMsvcPath = tc.path;
					if (sel) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		} else if (ln == "C#") {
			detectToolchains();
			pollDotnetProbe();
			std::string label = ".NET: ";
			label += detectedDotnetSdks.empty() ? "(none)" : detectedDotnetSdks.front().label;
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
			ImGui::SetNextItemWidth(220.0f);
			if (ImGui::BeginCombo("##dotnetStatusSel", label.c_str())) {
				for (auto& sdk : detectedDotnetSdks) {
					bool sel = (sdk.path == activeDotnetPath);
					std::string item = "SDK " + sdk.label;
					if (ImGui::Selectable(item.c_str(), sel)) activeDotnetPath = sdk.path;
					if (sel) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}
	}

	int line, column;
	e.GetCurrentCursor(line, column);
	float glyphWidth = ImGui::CalcTextSize("#").x;
	char status[256];
	// Optionally prepend an FPS readout — settings-toggleable so it's not in
	// the user's face by default. The value is the smoothed framerate ImGui
	// already maintains for its own debug widgets.
	int statusSize = 0;
	if (prefShowFps) {
		statusSize = std::snprintf(status, sizeof(status),
			"%.0f fps  Ln %d, Col %d  Tab: %d  %s",
			ImGui::GetIO().Framerate, line + 1, column + 1, e.GetTabSize(),
			std::filesystem::path(t.filename).filename().string().c_str());
	} else {
		statusSize = std::snprintf(status, sizeof(status),
			"Ln %d, Col %d  Tab: %d  %s",
			line + 1, column + 1, e.GetTabSize(),
			std::filesystem::path(t.filename).filename().string().c_str());
	}

	float size = glyphWidth * static_cast<float>(statusSize + 3);
	float width = ImGui::GetContentRegionAvail().x;
	ImGui::SameLine(0.0f, width - size);
	ImGui::TextUnformatted(status);

	ImGui::SameLine(0.0f, glyphWidth);
	auto drawlist = ImGui::GetWindowDrawList();
	auto pos = ImGui::GetCursorScreenPos();
	auto offset = ImGui::GetFrameHeight() * 0.5f;
	auto radius = offset * 0.6f;
	auto color = isDirty() ? IM_COL32(164, 0, 0, 255) : IM_COL32(164, 164, 164, 255);
	drawlist->AddCircleFilled(ImVec2(pos.x + offset, pos.y + offset), radius, color);

	ImGui::EndChild();
	ImGui::PopStyleColor();
}


//
//	Editor::showDiff
//

void Editor::showDiff()
{
	auto& t = doc();
	t.diff.SetLanguage(t.editor.GetLanguage());
	t.diff.SetText(t.originalText, t.editor.GetText());
	if (auto* vp = ImGui::GetWindowViewport()) dialogViewportId = vp->ID;
	else dialogViewportId = ImGui::GetMainViewport()->ID;
	dialogNeedsPlacement = true;
	state = State::diff;
}


//
//	Editor::showFileOpen
//

std::filesystem::path Editor::userConfigDir()
{
	// Stable absolute path so settings round-trip across launches regardless
	// of which directory the editor was started from. Used for both the
	// favourites blob and the ImGui layout .ini file.
	//   Windows: %APPDATA%\ImGuiColorTextEdit\
	//   POSIX:   $XDG_CONFIG_HOME/imguicolortext  (or  $HOME/.config/...)
	static std::filesystem::path cached;
	if (!cached.empty()) return cached;

	std::filesystem::path base;
#ifdef _WIN32
	if (const char* appData = std::getenv("APPDATA")) {
		base = std::filesystem::path(appData) / "ImGuiColorTextEdit";
	}
#else
	if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
		base = std::filesystem::path(xdg) / "imguicolortext";
	} else if (const char* home = std::getenv("HOME")) {
		base = std::filesystem::path(home) / ".config" / "imguicolortext";
	}
#endif
	// Last-resort fallback: next to the cwd. Should only hit on weird envs.
	if (base.empty()) {
		base = std::filesystem::current_path() / ".imguicolortext";
	}

	std::error_code ec;
	std::filesystem::create_directories(base, ec);
	cached = std::move(base);
	return cached;
}


static std::filesystem::path placesStatePath()
{
	return Editor::userConfigDir() / "places.txt";
}

// One-shot population of the file dialog's built-in Places pane:
//   * "Drives"     — system drives (C:\, D:\, …) or "/" + "~" on POSIX
//     (canBeSaved=false so they don't get serialised).
//   * "Favourites" — user-editable; serialised by IGFD between runs.
// Persistence is via IGFD's SerializePlaces/DeserializePlaces blobs, written
// to .claude/places.txt on dialog close.
static void populateFileDialogPlaces()
{
	auto* dlg = ImGuiFileDialog::Instance();
	static bool done = false;
	if (done) return;
	done = true;

	// IMPORTANT ORDER: groups must be created BEFORE DeserializePlaces runs.
	// IGFD's DeserializePlaces calls GetPlacesGroupPtr (which does NOT create
	// missing groups) — if the "Favourites" group doesn't exist yet, every
	// saved place is silently dropped. Likewise AddPlacesGroup replaces the
	// existing group object, so doing it AFTER Deserialize wipes the loaded
	// places. Create the groups first, then load into them.

	// 1. Create both groups in their final state.
	dlg->AddPlacesGroup("Drives",     0, /*canEdit*/ false, /*opened*/ true);
	dlg->AddPlacesGroup("Favourites", 1, /*canEdit*/ true,  /*opened*/ true);

	// 2. Populate Drives (non-persistent — re-enumerated every run).
	if (auto* grp = dlg->GetPlacesGroupPtr("Drives"))
	{
#ifdef _WIN32
		wchar_t drives[256];
		DWORD n = GetLogicalDriveStringsW(255, drives);
		if (n > 0 && n < 256)
		{
			for (wchar_t* p = drives; *p; p += wcslen(p) + 1)
			{
				char buf[8] = { 0 };
				for (int i = 0; p[i] && i < 7; ++i) buf[i] = static_cast<char>(p[i]);
				std::string label(buf);
				if (!label.empty() && label.back() == '\\') label.pop_back();  // "C:"
				grp->AddPlace(label, buf, /*canBeSaved*/ false);
			}
		}
#else
		grp->AddPlace("/", "/", /*canBeSaved*/ false);
		if (const char* home = std::getenv("HOME")) grp->AddPlace("Home", home, false);
#endif
	}

	// 3. Load the persisted Favourites *into* the existing group.
	std::ifstream f(placesStatePath(), std::ios::binary);
	if (f.is_open())
	{
		std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (!blob.empty()) dlg->DeserializePlaces(blob);
	}
}

// Write the current Places state (only entries with canBeSaved=true) to disk.
// Idempotent — keeps the last-written blob in a static so per-frame calls
// while the dialog is open are essentially free unless the user actually
// changed something (added/removed a favourite).
static void saveFileDialogPlaces()
{
	static std::string lastWritten;  // process-lifetime cache
	auto blob = ImGuiFileDialog::Instance()->SerializePlaces(/*forceAll*/ false);
	if (blob == lastWritten) return;  // no change since last write
	std::error_code ec;
	std::filesystem::create_directories(placesStatePath().parent_path(), ec);
	std::ofstream f(placesStatePath(), std::ios::binary | std::ios::trunc);
	if (f.is_open()) {
		f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
		lastWritten = std::move(blob);
	}
}


// Where file/project dialogs should start. Prefer the active document's folder,
// then the open project root, then the process cwd — NOT wherever the app
// happened to be launched from. IGFD treats an empty path as "last/cwd", so we
// pass an explicit directory.
std::string Editor::dialogStartDir() const
{
	std::error_code ec;
	if (!tabs.empty() && doc().filename != "untitled") {
		auto p = std::filesystem::path(doc().filename).parent_path();
		if (!p.empty() && std::filesystem::is_directory(p, ec)) return p.string();
	}
	if (!projectRoot.empty() && std::filesystem::is_directory(projectRoot, ec))
		return projectRoot.string();
	return std::filesystem::current_path(ec).string();
}

void Editor::showFileOpen()
{
	if (auto* vp = ImGui::GetWindowViewport()) dialogViewportId = vp->ID;
	else dialogViewportId = ImGui::GetMainViewport()->ID;
	dialogNeedsPlacement = true;
	populateFileDialogPlaces();
	IGFD::FileDialogConfig config;
	config.path = dialogStartDir();
	config.countSelectionMax = 1;
	config.flags =
		ImGuiFileDialogFlags_DontShowHiddenFiles |
		ImGuiFileDialogFlags_ReadOnlyFileNameField;
	ImGuiFileDialog::Instance()->OpenDialog("file-open", "Select File to Open...", ".*", config);
	state = State::openFile;
}


//
//	Editor::showSaveFileAs
//

void Editor::showSaveFileAs()
{
	if (auto* vp = ImGui::GetWindowViewport()) dialogViewportId = vp->ID;
	else dialogViewportId = ImGui::GetMainViewport()->ID;
	dialogNeedsPlacement = true;
	IGFD::FileDialogConfig config;
	config.path = dialogStartDir();
	config.countSelectionMax = 1;
	config.flags =
		ImGuiFileDialogFlags_DontShowHiddenFiles |
		ImGuiFileDialogFlags_ConfirmOverwrite;
	populateFileDialogPlaces();
	ImGuiFileDialog::Instance()->OpenDialog("file-saveas", "Save File as...", "*", config);
	state = State::saveFileAs;
}


//
//	Editor::showConfirmClose
//

void Editor::showConfirmClose(std::function<void()> callback)
{
	if (auto* vp = ImGui::GetWindowViewport()) dialogViewportId = vp->ID;
	else dialogViewportId = ImGui::GetMainViewport()->ID;
	dialogNeedsPlacement = true;
	onConfirmClose = callback;
	state = State::confirmClose;
}


//
//	Editor::showConfirmQuit
//

void Editor::showConfirmQuit()
{
	if (auto* vp = ImGui::GetWindowViewport()) dialogViewportId = vp->ID;
	else dialogViewportId = ImGui::GetMainViewport()->ID;
	dialogNeedsPlacement = true;
	state = State::confirmQuit;
}


//
//	Editor::showError
//

void Editor::showError(const std::string& message)
{
	if (auto* vp = ImGui::GetWindowViewport()) dialogViewportId = vp->ID;
	else dialogViewportId = ImGui::GetMainViewport()->ID;
	dialogNeedsPlacement = true;
	errorMessage = message;
	state = State::confirmError;
}


//
//	Editor::renderDiff
//

void Editor::renderDiff()
{
	ImGuiViewport* viewport = ImGui::FindViewportByID(dialogViewportId);
	if (!viewport) viewport = ImGui::GetMainViewport();
	// Non-modal so the user can dock it, drag it onto another monitor (becomes
	// its own OS window via multi-viewport), and keep editing while it's open.
	// Position + viewport only on first appearance (same trick we use for the
	// file dialog), otherwise we'd snap it back to centre every frame.
	if (dialogNeedsPlacement) {
		ImGui::SetNextWindowViewport(viewport->ID);
		ImVec2 sz(viewport->Size.x * 0.8f, viewport->Size.y * 0.8f);
		ImVec2 pos(viewport->Pos.x + (viewport->Size.x - sz.x) * 0.5f,
		           viewport->Pos.y + (viewport->Size.y - sz.y) * 0.5f);
		ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(sz,  ImGuiCond_Always);
		dialogNeedsPlacement = false;
	}

	bool open = true;
	if (ImGui::Begin("File History — Changes since open##history", &open))
	{
		ImVec2 region = ImGui::GetContentRegionAvail();
		ImVec2 diffSize(region.x, (std::max)(region.y - ImGui::GetFrameHeightWithSpacing() - 8.0f, 100.0f));
		doc().diff.Render("diff", diffSize, true);
		ImGui::Separator();
		bool sideBySide = doc().diff.GetSideBySideMode();
		if (ImGui::Checkbox("Show side-by-side", &sideBySide))
		{
			doc().diff.SetSideBySideMode(sideBySide);
		}
		ImGui::SameLine();
		// Revert button — only enabled when the doc has unsaved changes
		// against the on-open snapshot. Use ReplaceSectionText so the revert
		// is a normal transaction the regular Undo / Redo can step through.
		bool dirty = isDirty();
		if (!dirty) ImGui::BeginDisabled();
		if (ImGui::Button("Revert to opened version"))
		{
			auto& ed = doc().editor;
			int last = (std::max)(ed.GetLineCount() - 1, 0);
			std::string lastLineText = ed.GetLineText(last);
			int lastCol = (int)lastLineText.size();
			ed.ReplaceSectionText(0, 0, last, lastCol, doc().originalText);
			doc().version = ed.GetUndoIndex();
		}
		if (!dirty) ImGui::EndDisabled();

		// Undo / Redo right here so the user can step back into the reverted
		// state if they hit Revert and changed their mind.
		ImGui::SameLine();
		auto& ed = doc().editor;
		if (!ed.CanUndo()) ImGui::BeginDisabled();
		if (ImGui::Button("Undo")) ed.Undo();
		if (!ed.CanUndo()) ImGui::EndDisabled();
		ImGui::SameLine();
		if (!ed.CanRedo()) ImGui::BeginDisabled();
		if (ImGui::Button("Redo")) ed.Redo();
		if (!ed.CanRedo()) ImGui::EndDisabled();
	}
	ImGui::End();
	if (!open) state = State::edit;
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
	    ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow)) {
		// Esc closes only when no editor input is active.
		// (left intentionally simple — full focus tracking is overkill here.)
	}
}


//
//	Editor::renderFileOpen
//

void Editor::renderFileOpen()
{
	ImGuiViewport* vp = ImGui::FindViewportByID(dialogViewportId);
	if (!vp) vp = ImGui::GetMainViewport();
	ImVec2 maxSize = vp->Size;
	ImVec2 minSize = ImVec2(480.0f, 320.0f);
	auto dialog = ImGuiFileDialog::Instance();
	// Only set viewport + initial pos/size ONCE when the dialog opens.
	// Calling SetNextWindowViewport every frame would snap the dialog back to
	// the original viewport even after the user drags it out into its own window.
	if (dialogNeedsPlacement)
	{
		ImGui::SetNextWindowViewport(vp->ID);
		ImVec2 initialSize(vp->Size.x * 0.7f, vp->Size.y * 0.7f);
		ImVec2 initialPos(vp->Pos.x + (vp->Size.x - initialSize.x) * 0.5f,
						  vp->Pos.y + (vp->Size.y - initialSize.y) * 0.5f);
		ImGui::SetNextWindowPos(initialPos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(initialSize, ImGuiCond_Always);
		dialogNeedsPlacement = false;
	}

	bool finished = dialog->Display("file-open", ImGuiWindowFlags_NoCollapse, minSize, maxSize);
	// Save every frame the dialog is open — the saver is no-op if the
	// serialized blob hasn't changed since the last successful write, so this
	// is cheap. Guarantees user-added favourites persist even if the user
	// kills the process before the dialog closes cleanly.
	saveFileDialogPlaces();
	if (finished)
	{
		if (dialog->IsOk())
		{
			openFile(dialog->GetFilePathName());
		}
		state = State::edit;
		dialog->Close();
		saveFileDialogPlaces();
	}
}


//
//	Editor::renderSaveAs
//

void Editor::renderSaveAs()
{
	ImGuiViewport* vp = ImGui::FindViewportByID(dialogViewportId);
	if (!vp) vp = ImGui::GetMainViewport();
	ImVec2 maxSize = vp->Size;
	ImVec2 minSize = ImVec2(480.0f, 320.0f);
	auto dialog = ImGuiFileDialog::Instance();
	if (dialogNeedsPlacement)
	{
		ImGui::SetNextWindowViewport(vp->ID);
		ImVec2 initialSize(vp->Size.x * 0.7f, vp->Size.y * 0.7f);
		ImVec2 initialPos(vp->Pos.x + (vp->Size.x - initialSize.x) * 0.5f,
						  vp->Pos.y + (vp->Size.y - initialSize.y) * 0.5f);
		ImGui::SetNextWindowPos(initialPos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(initialSize, ImGuiCond_Always);
		dialogNeedsPlacement = false;
	}

	bool finished = dialog->Display("file-saveas", ImGuiWindowFlags_NoCollapse, minSize, maxSize);
	saveFileDialogPlaces();   // same per-frame cheap-no-op save as in renderFileOpen
	if (finished)
	{
		if (dialog->IsOk())
		{
			doc().filename = dialog->GetFilePathName();
			if (doc().editor.GetLanguage() == nullptr)
				doc().editor.SetLanguage(languageForPath(doc().filename));
			saveFile();
			state = State::edit;
		}
		else
		{
			state = State::edit;
		}
		dialog->Close();
		saveFileDialogPlaces();
	}
}


//
//	Editor::renderConfirmClose
//

void Editor::renderConfirmClose()
{
	ImGui::OpenPopup("Confirm Close");
	ImGuiViewport* vp = ImGui::FindViewportByID(dialogViewportId);
	if (!vp) vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowViewport(vp->ID);
	ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (ImGui::BeginPopupModal("Confirm Close", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("This file has unsaved changes!\nDo you want to discard them?\n\n");
		ImGui::Separator();
		static constexpr float buttonWidth = 80.0f;
		ImGui::Indent(ImGui::GetContentRegionAvail().x - buttonWidth * 2.0f - ImGui::GetStyle().ItemSpacing.x);
		if (ImGui::Button("Discard", ImVec2(buttonWidth, 0.0f)))
		{
			state = State::edit;
			if (onConfirmClose) onConfirmClose();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			state = State::edit;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}


//
//	Editor::renderConfirmQuit
//

void Editor::renderConfirmQuit()
{
	ImGui::OpenPopup("Quit Editor?");
	ImGuiViewport* vp = ImGui::FindViewportByID(dialogViewportId);
	if (!vp) vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowViewport(vp->ID);
	ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (ImGui::BeginPopupModal("Quit Editor?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("You have unsaved changes!\nDo you really want to quit?\n\n");
		ImGui::Separator();
		static constexpr float buttonWidth = 80.0f;
		ImGui::Indent(ImGui::GetContentRegionAvail().x - buttonWidth * 2.0f - ImGui::GetStyle().ItemSpacing.x);
		if (ImGui::Button("Quit", ImVec2(buttonWidth, 0.0f)))
		{
			done = true;
			state = State::edit;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			state = State::edit;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}


//
//	Editor::renderConfirmError
//

void Editor::renderConfirmError()
{
	ImGui::OpenPopup("Error");
	ImGuiViewport* vp = ImGui::FindViewportByID(dialogViewportId);
	if (!vp) vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowViewport(vp->ID);
	ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("%s\n", errorMessage.c_str());
		ImGui::Separator();
		static constexpr float buttonWidth = 80.0f;
		ImGui::Indent(ImGui::GetContentRegionAvail().x - buttonWidth);
		if (ImGui::Button("OK", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			errorMessage.clear();
			state = State::edit;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}


//
//	Editor::showGotoLine / renderGotoLine
//

void Editor::showGotoLine()
{
	if (auto* vp = ImGui::GetWindowViewport()) dialogViewportId = vp->ID;
	else dialogViewportId = ImGui::GetMainViewport()->ID;
	gotoLineBuf[0] = '\0';
	state = State::gotoLine;
}


void Editor::renderGotoLine()
{
	ImGui::OpenPopup("Go to Line");
	ImGuiViewport* vp = ImGui::FindViewportByID(dialogViewportId);
	if (!vp) vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowViewport(vp->ID);
	ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(280.0f, 0.0f), ImGuiCond_Appearing);

	if (ImGui::BeginPopupModal("Go to Line", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		auto& t = doc();
		int lineCount = t.editor.GetLineCount();
		ImGui::Text("Line (1 - %d), optional :column", lineCount);
		ImGui::SetNextItemWidth(-FLT_MIN);
		// Auto-focus the input on first frame, accept Enter to commit.
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		bool enter = ImGui::InputText("##gotoLine", gotoLineBuf, sizeof(gotoLineBuf),
			ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank);

		auto commit = [&]() {
			// Parse "<line>" or "<line>:<col>" (1-based input, 0-based internal).
			int targetLine = 0, targetCol = 0;
			const char* p = gotoLineBuf;
			while (*p == ' ' || *p == '\t') ++p;
			while (*p >= '0' && *p <= '9') { targetLine = targetLine * 10 + (*p - '0'); ++p; }
			if (*p == ':') {
				++p;
				while (*p >= '0' && *p <= '9') { targetCol = targetCol * 10 + (*p - '0'); ++p; }
			}
			if (targetLine > 0) {
				// (std::min)/(std::max) parens — <Windows.h> defines min/max
				// as macros up at the top of this file and we don't want them
				// eating the function names.
				int maxLine = (std::max)(lineCount - 1, 0);
				int ln  = (std::min)(targetLine - 1, maxLine);
				int col = (std::max)(targetCol - 1, 0);
				t.editor.SetCursor(ln, col);
				t.editor.ScrollToLine(ln, TextEditor::Scroll::alignMiddle);
				t.wantFocus = true;
			}
			state = State::edit;
			ImGui::CloseCurrentPopup();
		};

		ImGui::Separator();
		static constexpr float buttonWidth = 80.0f;
		ImGui::Indent(ImGui::GetContentRegionAvail().x - buttonWidth * 2.0f - ImGui::GetStyle().ItemSpacing.x);
		if (ImGui::Button("Go", ImVec2(buttonWidth, 0.0f)) || enter) {
			commit();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
			state = State::edit;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}


//
//	Editor::setAutocompleteMode
//

void Editor::configureTabAutocomplete(TabDocument& t)
{
	// Wire the autocomplete callback + debounced rebuild for a SINGLE tab and
	// build its trie once. Split out of setAutocompleteMode() so newTab() can
	// set up only the new tab instead of rebuilding EVERY open tab's trie
	// (which re-scanned big docs + re-inserted the whole project index on every
	// "+" press — the multi-second new-tab stall).
	TabDocument* tptr = &t;
	TextEditor::AutoCompleteConfig config;
	config.callback = [this, tptr](TextEditor::AutoCompleteState& state)
		{
			// Merge this file's identifiers with the shared project-wide trie.
			// Queried separately because findSuggestions() clears its output
			// vector. File-local matches rank first; project matches fill the
			// rest, deduped, capped at 20.
			if (projectTrieGen != indexState->gen.load()) buildProjectTrie();
			std::vector<std::string> local, proj;
			tptr->trie.findSuggestions(local, state.searchTerm);
			projectTrie.findSuggestions(proj, state.searchTerm);
			state.suggestions.clear();
			std::unordered_set<std::string> seen;
			for (auto& s : local) {
				if (state.suggestions.size() >= 20) break;
				if (seen.insert(s).second) state.suggestions.push_back(s);
			}
			for (auto& s : proj) {
				if (state.suggestions.size() >= 20) break;
				if (seen.insert(s).second) state.suggestions.push_back(s);
			}
		};
	t.editor.SetAutoCompleteConfig(&config);
	t.editor.SetChangeCallback([this, tptr]() { buildAutocompleteTrie(*tptr); }, 3000);
	buildAutocompleteTrie(t);
}

void Editor::setAutocompleteMode(bool flag)
{
	// Global toggle: (re)configure or tear down autocomplete on ALL tabs. The
	// per-tab path lives in configureTabAutocomplete(); adding a tab must NOT
	// call this (use that helper for just the new tab) or it rebuilds every
	// trie.
	for (auto& tp : tabs)
	{
		auto& t = *tp;
		if (flag)
		{
			configureTabAutocomplete(t);
		}
		else
		{
			t.editor.SetAutoCompleteConfig(nullptr);
			t.editor.SetChangeCallback(nullptr);
		}
	}
}


//
//	Editor::buildProjectTrie
//

// One shared project-wide identifier trie for the whole editor, rebuilt once
// per index generation (NOT once per tab). Previously every tab kept its own
// full copy of the project index inside its trie; opening N tabs allocated N
// giant tries and froze the app. Now each tab's trie is file-local only and
// this single shared trie carries the project-wide identifiers.
void Editor::buildProjectTrie()
{
	ScopedTimer _t("buildProjectTrie");
	int g = indexState->gen.load();
	projectTrie.clear();
	if (auto idx = indexSnapshot()) {
		for (auto& id : idx->identifiers) projectTrie.insert(id);
	}
	projectTrieGen = g;
}


//
//	Editor::buildAutocompleteTrie
//

void Editor::buildAutocompleteTrie(TabDocument& t)
{
	ScopedTimer _t("buildAutocompleteTrie");
	t.trie.clear();
	auto language = t.editor.GetLanguage();
	if (language)
	{
		for (auto& word : language->keywords)     t.trie.insert(word);
		for (auto& word : language->declarations) t.trie.insert(word);
		for (auto& word : language->identifiers)  t.trie.insert(word);
	}
	t.editor.IterateIdentifiers([&](const std::string& id) { t.trie.insert(id); });
}
