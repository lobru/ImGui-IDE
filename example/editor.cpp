//	TextEditor - A syntax highlighting text editor for ImGui
//	Copyright (c) 2024-2026 Johan A. Goossens. All rights reserved.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


//
//	Include files
//
#define _CRT_SECURE_NO_WARNINGS  // for std::getenv used in #include resolution
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <process.h>
#include <thread>

#include "ImGuiFileDialog.h"
#include "imgui.h"
#include "imgui_internal.h"

#include "editor.h"


// Forward decls so callers above the definitions (e.g. Editor::tryToQuit)
// can persist favourites at any quit/teardown path.
static void saveFileDialogPlaces();


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
	if (ext == ".c" || ext == ".h")                                          return TextEditor::Language::C();
	if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
		ext == ".hpp" || ext == ".hxx" || ext == ".inl")                     return TextEditor::Language::Cpp();
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

	// Runtime-defined languages (HTML, INI, YAML, CFG, BAT, PS1, etc.)
	auto& byExt = runtimeLanguagesByExt();
	auto it = byExt.find(ext);
	if (it != byExt.end()) return it->second;
	return nullptr;
}


//
//	Editor::Editor
//

Editor::Editor()
{
	// Load user-defined language definitions (HTML, INI, YAML, CFG, BAT, PS1).
	loadRuntimeLanguages();

	auto& t = newTab();
	t.originalText = demo;
	t.editor.SetText(demo);
	t.editor.SetLanguage(TextEditor::Language::Cpp());
	t.version = t.editor.GetUndoIndex();
	t.filename = "demo.cpp";
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
	tabs.push_back(std::move(t));
	activeTab = tabs.size() - 1;
	if (autocomplete) setAutocompleteMode(true);
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
	if (autocomplete) setAutocompleteMode(true);
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
	if (ImGui::Begin("Output##scriptRunner", &script->visible))
	{
		if (ImGui::SmallButton("Clear"))
		{
			std::lock_guard<std::mutex> lock(script->mutex);
			script->output.clear();
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


void Editor::runScriptForDoc()
{
	if (tabs.empty()) return;
	auto& t = doc();
	if (t.filename == "untitled") return;

	auto ext = std::filesystem::path(t.filename).extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(),
				   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	const char* interp = interpreterForExt(ext);
	if (!interp)
	{
		std::lock_guard<std::mutex> lock(script->mutex);
		script->output = "[no interpreter mapped for " + ext + "]\n";
		script->visible = true;
		return;
	}

	if (isDirty()) saveFile();

	std::string cmd;
	if (interp[0] != '\0') cmd = std::string(interp) + " \"" + t.filename + "\"";
	else                   cmd = "\"" + t.filename + "\"";

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
		// 2. Exists on disk? Open it.
		std::error_code ec;
		if (std::filesystem::exists(candidate, ec))
		{
			openFile(candidate.string());
			return;
		}
	}
	// 3. Nothing found — silently no-op (could add status message later).
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
	ImGui::DockSpace(dockId, dockArea, ImGuiDockNodeFlags_None);

	renderDockedDocuments();
	renderScriptOutputWindow();

	ImGui::Spacing();
	renderStatusBar();

	if (state == State::diff) { renderDiff(); }
	else if (state == State::openFile) { renderFileOpen(); }
	else if (state == State::saveFileAs) { renderSaveAs(); }
	else if (state == State::confirmClose) { renderConfirmClose(); }
	else if (state == State::confirmQuit) { renderConfirmQuit(); }
	else if (state == State::confirmError) { renderConfirmError(); }
	else if (state == State::gotoLine)     { renderGotoLine(); }

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
	ImGuiID dockId = ImGui::GetID("MainDockSpace");

	// first time setup: dock all initial windows into the dockspace
	for (size_t i = 0; i < tabs.size(); ++i)
	{
		auto& t = *tabs[i];
		ImGui::SetNextWindowDockID(dockId, ImGuiCond_FirstUseEver);
		bool focusing = t.wantFocus;
		if (focusing)
		{
			ImGui::SetNextWindowFocus();
			t.wantFocus = false;
		}
		renderDocumentWindow(t);
		// In a docked area, SetNextWindowFocus alone sometimes isn't enough
		// to bring the tab forward — also call SetWindowFocus on the same
		// label after Begin/End so the dock node activates it.
		if (focusing)
		{
			ImGui::SetWindowFocus(windowLabelFor(t).c_str());
		}
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

	ImGui::PushFont(nullptr, fontSize);
	auto cursorPos = ImGui::GetCursorScreenPos();
	t.editor.SetTextContextMenuCallback([this, &t, &cursorPos](int line, int column)
										{
											// Refresh the trie at right-click time so it picks up identifiers
											// the colorizer has tokenised since the doc was opened (e.g. the
											// demo C++ doc — keywords are in immediately, but `main`,
											// `numbers`, etc. only become available after the first colorize
											// pass). Cheap enough to do on a single menu open.
											buildAutocompleteTrie(t);

											const auto word = t.editor.GetWordAtScreenPos(cursorPos);

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

													std::string label = "Go to File: " + inc;
													if (ImGui::MenuItem(label.c_str(), nullptr, false, found))
													{
														openFile(candidate.string());
													}
												}
											}

											if (!word.empty())
											{
												t.editor.SelectWord(line, column);
												// Only offer symbol-aware navigation for tokens the autocomplete
												// trie knows about (language keywords/declarations/identifiers +
												// identifiers the colorizer has seen in this document). Avoids
												// littering the menu when the user right-clicks on punctuation
												// or plain prose.
												bool isSymbol = t.trie.contains(word);
												if (isSymbol)
												{
													if (ImGui::MenuItem("Go to Definition"))
													{
														t.editor.GoToDefinitionOf(word, true);
													}
													if (ImGui::MenuItem("Go to Declaration"))
													{
														t.editor.GoToFirstOccurrenceOf(word, true, false);
													}
													if (ImGui::MenuItem("Select All Occurrences"))
													{
														t.editor.SelectAllOccurrencesOf(word, true, true);
													}
												}
											}
											ImGui::Separator();
											ImGui::Text("Line %d, column %d", line + 1, column + 1);
										});

	ImVec2 editorSize = ImGui::GetContentRegionAvail();
	t.editor.Render("##editorContent", editorSize);

	ImGui::PopFont();
	ImGui::End();
}


//
//	Editor::tryToQuit
//

void Editor::tryToQuit()
{
	// Persist file-dialog favourites BEFORE the quit path can pull us out
	// of the message loop — covers the case where the user added a place
	// via the dialog UI but never closed the dialog cleanly.
	saveFileDialogPlaces();
	// And flush the ImGui layout right now too, so window positions /
	// docking state are guaranteed-persisted even on abrupt close.
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
			// Path utilities — handy when the current doc lives on disk.
			bool hasPath = !tabs.empty() && doc().filename != "untitled";
			if (ImGui::MenuItem("Open Containing Folder", nullptr, false, hasPath)) {
				openContainingFolder();
			}
			if (ImGui::MenuItem("Copy File Path", nullptr, false, hasPath)) {
				ImGui::SetClipboardText(doc().filename.c_str());
			}
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

		if (ImGui::BeginMenu("View"))
		{
			if (ImGui::MenuItem("Zoom In", " " SHORTCUT "+")) { increaseFontSIze(); }
			if (ImGui::MenuItem("Zoom Out", " " SHORTCUT "-")) { decreaseFontSIze(); }
			ImGui::Separator();
			bool flag;
			if (ImGui::MenuItem("Autocomplete", nullptr, &autocomplete)) { setAutocompleteMode(autocomplete); }
			flag = e.IsShowWhitespacesEnabled();        if (ImGui::MenuItem("Show Whitespaces", nullptr, &flag)) { e.SetShowWhitespacesEnabled(flag); }
			flag = e.IsShowSpacesEnabled();             if (ImGui::MenuItem("Show Spaces", nullptr, &flag)) { e.SetShowSpacesEnabled(flag); }
			flag = e.IsShowTabsEnabled();               if (ImGui::MenuItem("Show Tabs", nullptr, &flag)) { e.SetShowTabsEnabled(flag); }
			flag = e.IsShowLineNumbersEnabled();        if (ImGui::MenuItem("Show Line Numbers", nullptr, &flag)) { e.SetShowLineNumbersEnabled(flag); }
			flag = e.IsShowingMatchingBrackets();       if (ImGui::MenuItem("Show Matching Brackets", nullptr, &flag)) { e.SetShowMatchingBrackets(flag); }
			flag = e.IsCompletingPairedGlyphs();        if (ImGui::MenuItem("Complete Matching Glyphs", nullptr, &flag)) { e.SetCompletePairedGlyphs(flag); }
			flag = e.IsShowPanScrollIndicatorEnabled(); if (ImGui::MenuItem("Show Pan/Scroll Indicator", nullptr, &flag)) { e.SetShowPanScrollIndicatorEnabled(flag); }
			flag = e.IsMiddleMousePanMode();            if (ImGui::MenuItem("Middle Mouse Pan Mode", nullptr, &flag)) { if (flag) e.SetMiddleMousePanMode(); else e.SetMiddleMouseScrollMode(); }
			flag = e.IsFoldingEnabled();                if (ImGui::MenuItem("Enable Folding", nullptr, &flag)) { e.SetFoldingEnabled(flag); }
			if (ImGui::MenuItem("Fold All", " " SHORTCUT "0", nullptr, e.IsFoldingEnabled())) { e.FoldAll(); }
			if (ImGui::MenuItem("Unfold All", " " SHORTCUT "J", nullptr, e.IsFoldingEnabled())) { e.UnfoldAll(); }
			if (ImGui::MenuItem("Fold Current", " " SHORTCUT "Shift+[", nullptr, e.IsFoldingEnabled())) { e.FoldCurrent(); }
			if (ImGui::MenuItem("Unfold Current", " " SHORTCUT "Shift+]", nullptr, e.IsFoldingEnabled())) { e.UnfoldCurrent(); }
			ImGui::Separator();
			if (ImGui::MenuItem("File History…", " " SHORTCUT "I")) { showDiff(); }
			if (ImGui::MenuItem("Show Output", "F5", &script->visible)) {}
			if (ImGui::MenuItem("Build Project", "F6")) { runProjectBuild(); }
			ImGui::Separator();
			if (ImGui::MenuItem("Save Layout Now"))
			{
				if (auto* fn = ImGui::GetIO().IniFilename) ImGui::SaveIniSettingsToDisk(fn);
			}
			if (ImGui::MenuItem("Reset Layout"))
			{
				// Wipe the in-memory layout; on next save it'll persist the
				// fresh "no settings" state. User must dock things again.
				ImGui::LoadIniSettingsFromMemory("");
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Find"))
		{
			if (ImGui::MenuItem("Find",        " " SHORTCUT "F")) { e.OpenFindReplaceWindow(); }
			if (ImGui::MenuItem("Find Next",   "F3",        nullptr, e.HasFindString())) { e.FindNext(); }
			if (ImGui::MenuItem("Find All",    "^" SHORTCUT "G", nullptr, e.HasFindString())) { e.FindAll(); }
			ImGui::Separator();
			if (ImGui::MenuItem("Go to Line…", " " SHORTCUT "G")) { showGotoLine(); }
			ImGui::EndMenu();
		}

		ImGui::EndMenuBar();
	}

	// global keyboard shortcuts (work whenever no input wants the keys)
	ImGuiIO& io = ImGui::GetIO();
	if (!io.WantCaptureKeyboard || ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
	{

		if (ImGui::IsKeyDown(ImGuiMod_Ctrl))
		{
			// Ctrl+Shift+T = reopen last closed tab. Check shift first so it
			// doesn't fall through to plain Ctrl+T (unused) or other branches.
			if (ImGui::IsKeyDown(ImGuiMod_Shift) && ImGui::IsKeyPressed(ImGuiKey_T, false))
			{
				reopenLastClosedTab();
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_N, false)) { newFile(); }
			else if (ImGui::IsKeyPressed(ImGuiKey_O, false)) { openFile(); }
			else if (ImGui::IsKeyPressed(ImGuiKey_W, false))
			{
				if (isDirty()) showConfirmClose([this]() { closeTab(activeTab); });
				else closeTab(activeTab);
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_S, false))
			{
				// Ctrl+Shift+S → always Save As. Plain Ctrl+S → Save (falls
				// back to Save As only for untitled docs).
				if (ImGui::IsKeyDown(ImGuiMod_Shift) || doc().filename == "untitled") {
					showSaveFileAs();
				} else {
					saveFile();
				}
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_I, false)) { showDiff(); }
			else if (ImGui::IsKeyPressed(ImGuiKey_G, false) && !ImGui::IsKeyDown(ImGuiMod_Shift)) {
				// Ctrl+G — Go to Line. Ctrl+Shift+G falls through to the
				// editor's own Find-All binding below.
				showGotoLine();
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_Equal, false)) { increaseFontSIze(); }
			else if (ImGui::IsKeyPressed(ImGuiKey_Minus, false)) { decreaseFontSIze(); }
			else if (ImGui::IsKeyPressed(ImGuiKey_Tab, false))
			{
				if (!tabs.empty())
				{
					if (ImGui::IsKeyDown(ImGuiMod_Shift))
						activeTab = (activeTab == 0) ? tabs.size() - 1 : activeTab - 1;
					else
						activeTab = (activeTab + 1) % tabs.size();
					tabs[activeTab]->wantFocus = true;
				}
			}
		}
		// Alt-only: Alt+O = header <-> source toggle
		if (!ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyDown(ImGuiMod_Alt) &&
			!ImGui::IsKeyDown(ImGuiMod_Shift))
		{
			if (ImGui::IsKeyPressed(ImGuiKey_O, false))
			{
				toggleHeaderSource();
			}
		}
		// F5: run the active document with its interpreter (async, threaded).
		if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
		{
			runScriptForDoc();
		}
		// F6: run the project's build script (walk up for build.bat / .ps1 /
		// Makefile / CMakeLists.txt). Output streams into the same window.
		if (ImGui::IsKeyPressed(ImGuiKey_F6, false))
		{
			runProjectBuild();
		}
	}
}


//
//	Editor::renderStatusBar
//

void Editor::renderStatusBar()
{
	static const char* langNames[] = { "None", "C", "C++", "C#", "AngelScript", "Lua", "Python", "GLSL", "HLSL", "JSON", "Markdown", "SQL" };
	static const TextEditor::Language* langDefs[] = {
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
		TextEditor::Language::Sql()
	};

	auto& t = doc();
	auto& e = t.editor;
	std::string langName = e.GetLanguageName();

	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
	ImGui::BeginChild("StatusBar", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
	ImGui::SetNextItemWidth(120.0f);

	if (ImGui::BeginCombo("##LangSel", langName.c_str()))
	{
		for (int n = 0; n < static_cast<int>(IM_ARRAYSIZE(langNames)); n++)
		{
			bool selected = (langName == langNames[n]);
			if (ImGui::Selectable(langNames[n], selected))
			{
				e.SetLanguage(langDefs[n]);
				buildAutocompleteTrie(t);
			}
			if (selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	ImGui::SameLine(0.0f, 0.0f);
	ImGui::AlignTextToFramePadding();

	int line, column;
	e.GetCurrentCursor(line, column);
	float glyphWidth = ImGui::CalcTextSize("#").x;
	char status[256];
	int statusSize = std::snprintf(status, sizeof(status),
								   "Ln %d, Col %d  Tab: %d  %s",
								   line + 1, column + 1,
								   e.GetTabSize(),
								   std::filesystem::path(t.filename).filename().string().c_str());

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


void Editor::showFileOpen()
{
	if (auto* vp = ImGui::GetWindowViewport()) dialogViewportId = vp->ID;
	else dialogViewportId = ImGui::GetMainViewport()->ID;
	dialogNeedsPlacement = true;
	populateFileDialogPlaces();
	IGFD::FileDialogConfig config;
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

void Editor::setAutocompleteMode(bool flag)
{
	for (auto& tp : tabs)
	{
		auto& t = *tp;
		if (flag)
		{
			TextEditor::AutoCompleteConfig config;
			TabDocument* tptr = &t;
			config.callback = [tptr](TextEditor::AutoCompleteState& state)
				{
					tptr->trie.findSuggestions(state.suggestions, state.searchTerm);
				};
			t.editor.SetAutoCompleteConfig(&config);
			t.editor.SetChangeCallback([this, tptr]() { buildAutocompleteTrie(*tptr); }, 3000);
		}
		else
		{
			t.editor.SetAutoCompleteConfig(nullptr);
			t.editor.SetChangeCallback(nullptr);
		}
	}
	if (flag)
	{
		for (auto& tp : tabs) buildAutocompleteTrie(*tp);
	}
}


//
//	Editor::buildAutocompleteTrie
//

void Editor::buildAutocompleteTrie(TabDocument& t)
{
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
