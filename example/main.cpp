//	TextEditor - A syntax highlighting text editor for ImGui
//	Copyright (c) 2024-2026 Johan A. Goossens. All rights reserved.
//	Copyright (c) 2026 Logan Brunet (ImGui-IDE). All rights reserved.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


//
//	Include files
//

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <string>
#include <vector>
#include "imgui.h"

#if STANDALONE

#include <SDL3/SDL.h>

#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#ifdef _WIN32
#include <Windows.h>
#include <crtdbg.h>
#include <DbgHelp.h>
#pragma comment(lib, "Dbghelp.lib")
#endif

#include "editor.h"
#include "dejavu.h"


//
//	Crash / assert capture — the "Debug Error" on close has been hard to pin
//	without the underlying text. Route every failure path (CRT assert, unhandled
//	C++ exception, SEH access violation) to a crash.log next to the settings, so
//	the next occurrence is self-reported instead of needing a manual repro.
//

static std::string gCrashLogPath;

static void writeCrashLine(const char* tag, const char* msg) {
	if (!gCrashLogPath.empty()) {
		FILE* f = nullptr;
#ifdef _WIN32
		if (fopen_s(&f, gCrashLogPath.c_str(), "a") == 0 && f) {
#else
		if ((f = std::fopen(gCrashLogPath.c_str(), "a")) != nullptr) {
#endif
			std::fprintf(f, "[%s] %s\n", tag, msg ? msg : "(null)");
			std::fflush(f);
			std::fclose(f);
		}
	}
	std::fprintf(stderr, "[%s] %s\n", tag, msg ? msg : "(null)");
	std::fflush(stderr);
}

#ifdef _WIN32
// Intercepts the Debug CRT assert/error MESSAGE (file, line, expression) before
// the dialog. Returning FALSE keeps the normal report behavior intact.
// [[maybe_unused]]: in Release builds _CrtSetReportHook macro-expands to a no-op
// (it's debug-CRT only), leaving this unreferenced — which /WX would make fatal.
[[maybe_unused]] static int __cdecl crtReportHook(int reportType, char* message, int* returnValue) {
	const char* kind = (reportType == _CRT_ASSERT) ? "CRT_ASSERT"
		: (reportType == _CRT_ERROR) ? "CRT_ERROR" : "CRT_WARN";
	if (reportType != _CRT_WARN) writeCrashLine(kind, message);
	if (returnValue) *returnValue = 0;   // don't force a debugger break
	return FALSE;                        // let default reporting proceed
}

static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
	// Log the faulting address AS A MODULE-RELATIVE RVA so it can be symbolized
	// against the build's PDB (`ln example.exe+<rva>` in cdb) regardless of ASLR.
	void* addr = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress : nullptr;
	HMODULE base = GetModuleHandleW(nullptr);
	unsigned long long rva = (addr && base) ? (unsigned long long)((char*)addr - (char*)base) : 0ULL;
	char buf[200];
	std::snprintf(buf, sizeof(buf), "code=0x%08lX addr=%p  example.exe+0x%llX",
		ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0UL, addr, rva);
	writeCrashLine("SEH", buf);

	// Write a minidump next to the settings so the next crash is fully diagnosable
	// with matching symbols (cdb -z crash.dmp).
	if (!gCrashLogPath.empty()) {
		std::string dumpPath = gCrashLogPath.substr(0, gCrashLogPath.find_last_of("/\\") + 1) + "crash.dmp";
		HANDLE f = CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f != INVALID_HANDLE_VALUE) {
			MINIDUMP_EXCEPTION_INFORMATION mei{};
			mei.ThreadId = GetCurrentThreadId();
			mei.ExceptionPointers = ep;
			mei.ClientPointers = FALSE;
			MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
				(MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory),
				ep ? &mei : nullptr, nullptr, nullptr);
			CloseHandle(f);
			writeCrashLine("SEH", ("minidump -> " + dumpPath).c_str());
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void onTerminate() {
	const char* what = "(no active exception)";
	try {
		auto e = std::current_exception();
		if (e) std::rethrow_exception(e);
	}
	catch (const std::exception& ex) { what = ex.what(); }
	catch (...)                      { what = "(non-std exception)"; }
	writeCrashLine("TERMINATE", what);
	std::abort();
}

static void installCrashHandlers() {
	gCrashLogPath = (Editor::userConfigDir() / "crash.log").string();
	std::set_terminate(onTerminate);
#ifdef _WIN32
	_CrtSetReportHook(crtReportHook);
	SetUnhandledExceptionFilter(crashFilter);
#endif
}


//
//	main
//

int main(int argc, char** argv) {
#ifdef _WIN32
	// We link as a /SUBSYSTEM:WINDOWS app so launching from Explorer never
	// flashes a console window. Two cases:
	//   1. Launched from a real terminal → attach to the parent's console so
	//      printf/fprintf appear there.
	//   2. Launched from Explorer (no parent console) → allocate our OWN console
	//      but keep its window HIDDEN. This matters because child processes we
	//      spawn via _popen / std::system (the `dotnet --list-sdks` probe, build
	//      and run commands, the script runner) inherit our console. Without one,
	//      EACH child's cmd.exe allocates and SHOWS its own console window — the
	//      "a console pops up when opening a project" regression. Giving them a
	//      hidden console to inherit keeps them silent.
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* fp = nullptr;
		freopen_s(&fp, "CONOUT$", "w", stdout);
		freopen_s(&fp, "CONOUT$", "w", stderr);
	} else if (AllocConsole()) {
		// Hide the window we just created; we only want the console *object* so
		// children have something to inherit, not a visible terminal.
		if (HWND hc = GetConsoleWindow()) ShowWindow(hc, SW_HIDE);
		FILE* fp = nullptr;
		freopen_s(&fp, "CONOUT$", "w", stdout);
		freopen_s(&fp, "CONOUT$", "w", stderr);
		freopen_s(&fp, "CONIN$",  "r", stdin);
	}
#endif
	// Route asserts / crashes to crash.log as early as possible.
	installCrashHandlers();

	// Parse --project <dir> and positional args. Positionals are sorted into
	// (a) project root (last folder OR project file we see — supports shell
	// context-menu integration: right-click a .sln in Explorer → opens here)
	// and (b) plain files to open as documents.
	std::string projectArg;
	std::vector<std::string> filesArg;
	bool focusArg = false;   // --focus: start in distraction-free focus mode
	auto isProjectFile = [](const std::string& p) {
		auto ext = std::filesystem::path(p).extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(),
			[](unsigned char c){ return (char) std::tolower(c); });
		return ext == ".sln" || ext == ".csproj" || ext == ".vcxproj"
			|| ext == ".uproject" || ext == ".uplugin"
			|| std::filesystem::path(p).filename() == "CMakeLists.txt";
	};
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--project" && i + 1 < argc) {
			projectArg = argv[++i];
			continue;
		}
		if (a == "--focus") { focusArg = true; continue; }
		if (a.rfind("--", 0) == 0) continue;   // unknown flag
		std::error_code ec;
		auto absPath = std::filesystem::absolute(a, ec);
		std::string p = ec ? a : absPath.string();
		if (std::filesystem::is_directory(p, ec)) {
			// Folder → treat as project root.
			projectArg = p;
		} else if (isProjectFile(p)) {
			// Project file → set its parent as root AND open the file as a doc.
			projectArg = std::filesystem::path(p).parent_path().string();
			filesArg.push_back(p);
		} else {
			filesArg.push_back(p);
		}
	}

	// Per-project instance key: instances are ONE PER PROJECT. Same project → a
	// second launch coalesces into the running window; different project → its
	// own window. The key source is the project root when given, else the
	// nearest project root ABOVE the first file (so opening a loose file that
	// belongs to an already-open project joins that window rather than spawning
	// a bare one), else "none" — so genuinely projectless launches stay single.
	// Wrapped: std::filesystem::path::string() converts the native (wide) path to
	// the ANSI code page and THROWS std::system_error on a filename with
	// characters the ACP can't represent. This runs before the render loop, so an
	// escape here aborts the process. A non-ANSI name in an ancestor directory of
	// a file opened with no --project used to crash on startup — key resolution is
	// best-effort, so on any failure just fall back to the projectless "none" key.
	std::string instanceKey = "none";
	try {
		std::string keySource = projectArg;
		if (keySource.empty() && !filesArg.empty()) {
			auto cur = std::filesystem::path(filesArg.front()).parent_path();
			// Compare extensions / names via path (native/wide) comparison — never
			// path::string() — so a non-ANSI sibling doesn't throw. Markers are
			// matched case-sensitively (conventionally lowercase); a missed match
			// only means the launch uses the "none" bucket, never a crash.
			auto isRootMarker = [](const std::filesystem::path& d) {
				std::error_code mec;
				for (auto it = std::filesystem::directory_iterator(
						 d, std::filesystem::directory_options::skip_permission_denied, mec);
					 !mec && it != std::filesystem::directory_iterator(); it.increment(mec)) {
					auto ext = it->path().extension();
					auto fn = it->path().filename();
					if (ext == ".sln" || ext == ".csproj" || ext == ".vcxproj" ||
						ext == ".uproject" || ext == ".uplugin" ||
						fn == "CMakeLists.txt" || fn == ".git")
						return true;
				}
				return false;
			};
			for (int depth = 0; depth < 30 && !cur.empty(); ++depth) {
				if (isRootMarker(cur)) { keySource = cur.string(); break; }
				auto parent = cur.parent_path();
				if (parent == cur) break;
				cur = parent;
			}
		}
		if (!keySource.empty()) {
			std::error_code kec;
			auto canon = std::filesystem::weakly_canonical(keySource, kec);
			std::string s = kec ? keySource : canon.string();
#ifdef _WIN32
			std::transform(s.begin(), s.end(), s.begin(),
				[](unsigned char c){ return (char) std::tolower(c); });
#endif
			instanceKey = std::to_string((unsigned long long) std::hash<std::string>{}(s));
		}
	}
	catch (const std::exception& ex) {
		std::fprintf(stderr, "instance-key resolution failed (%s) — using 'none'\n", ex.what());
		instanceKey = "none";
	}

#ifdef _WIN32
	// Single instance PER PROJECT: if an ImGui-IDE is already running ON THIS
	// PROJECT, forward the requested project/files to it and exit — so a second
	// launch of the same project (UE "Open Source Code", Explorer "Open with",
	// a shell verb) reuses that window instead of spawning a duplicate. A launch
	// for a DIFFERENT project falls through and opens its own window. Opt out of
	// coalescing entirely with --new.
	{
		bool forceNew = false;
		for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--new") forceNew = true;
		if (!forceNew) {
			std::string mutexName = "Local\\ImGuiIDE_Instance_" + instanceKey;
			HANDLE inst = CreateMutexA(nullptr, FALSE, mutexName.c_str());
			if (inst && GetLastError() == ERROR_ALREADY_EXISTS) {
				std::error_code mec;
				auto dir = Editor::userConfigDir() / "open" / instanceKey;
				std::filesystem::create_directories(dir, mec);
				std::string body;
				if (!projectArg.empty()) body += "project|" + projectArg + "\n";
				for (auto& f : filesArg) body += "file|" + f + "\n";
				body += "raise|\n";
				auto name = dir / ("open_" + std::to_string((long long) std::time(nullptr)) + "_" +
					std::to_string((unsigned long) GetCurrentProcessId()) + ".txt");
				std::ofstream(name, std::ios::binary) << body;
				return 0; // the instance owning this project picks it up via pollOpenInbox()
			}
			// else: we are the primary for this project — hold `inst` for the process lifetime.
		}
	}
#endif

	// setup SDL
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		printf("Error: SDL_Init(): %s\n", SDL_GetError());
		return -1;
	}

	SDL_Window* window = SDL_CreateWindow("ImGui-IDE", 1280, 720,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY );
	if (!window) {
		printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
		return -1;
	}

	SDL_GPUDevice* gpu_device = SDL_CreateGPUDevice(
		SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
		true, nullptr);
	if (!gpu_device) {
		printf("Error: SDL_CreateGPUDevice(): %s\n", SDL_GetError());
		return -1;
	}

	if (!SDL_ClaimWindowForGPUDevice(gpu_device, window)) {
		printf("Error: SDL_ClaimWindowForGPUDevice(): %s\n", SDL_GetError());
		return -1;
	}

	// setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	auto& io = ImGui::GetIO();
	// Persist window layout, docking, viewport positions between runs.
	// imgui writes here automatically on a debounced timer (IniSavingRate).
	// Stored in the same absolute config dir as the favourites blob so
	// settings survive across launches regardless of cwd.
	static std::string iniPath = (Editor::userConfigDir() / "imgui.ini").string();
	io.IniFilename = iniPath.c_str();
	// Docking + multi-viewport on, set once at init (toggling ViewportsEnable
	// per-frame leaves viewport state inconsistent). Windows only leave the
	// main window when the user drags them out or uses the pop-out hotkeys.
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
	ImGui::StyleColorsDark();

	// A little extra breathing room so content (and full-width inputs that
	// stretch to -FLT_MIN) doesn't sit flush against the vertical scrollbar.
	{
		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowPadding.x = 12.0f;   // default 8 — adds a right-edge gap to the scrollbar
		style.ScrollbarSize   = 15.0f;   // slightly chunkier, easier to grab
		// Raise the floor on how narrow a docked pane (and thus its tab) can be
		// squeezed — stops document tabs collapsing into unreadable slivers when
		// several are split side by side. Default is 32x32.
		style.WindowMinSize   = ImVec2(140.0f, 80.0f);

		// ── ImGui-IDE theme ──────────────────────────────────────────────────
		// A calm, cool-gray dark theme so panels/tabs/menus read as one product
		// rather than raw StyleColorsDark. Rounded corners + softened borders.
		style.ItemSpacing  = ImVec2(8.0f, 5.0f);
		style.FramePadding = ImVec2(7.0f, 4.0f);
	}
	// Rounding + the colour palette live in Editor::applyTheme so themes are
	// switchable at runtime (View > Theme). Set the default now; the Editor
	// re-applies the user's saved theme (prefTheme) right after it loads settings.
	Editor::applyTheme(0);

	// setup platform/renderer backends
	ImGui_ImplSDL3_InitForSDLGPU(window);
	ImGui_ImplSDLGPU3_InitInfo init_info = {};
	init_info.Device = gpu_device;
	init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu_device, window);
	init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
	ImGui_ImplSDLGPU3_Init(&init_info);

	// setup our font
	ImFontConfig config;
	std::memcpy(config.Name, "DejaVu", 7);
	config.FontDataOwnedByAtlas = false;
	// Horizontal oversampling 2 (ImGui's default) so glyphs carry their full
	// left-side bearing in the atlas. With OversampleH=1 the leftmost pixel
	// column of a glyph can be shaved when text sits at a clip-rect edge —
	// visible as the first character of File-menu items being clipped on the
	// left. This is the UI font only; the editor grid uses its own font.
	config.OversampleH = 2;
	config.OversampleV = 1;
	io.Fonts->Clear();
	io.Fonts->AddFontFromMemoryCompressedTTF((void*) &dejavu, dejavuSize, 15.0f, &config);

	// main loop
	// Tell Editor to skip the demo doc when launched with files / project.
	Editor::sSkipDemo = (!projectArg.empty() || !filesArg.empty());
	Editor editor;
	editor.setInstanceKey(instanceKey);   // route this project's open-inbox to us
	updater::cleanupStaleUpdate(updater::runningExePath());   // drop any <exe>.old from a prior in-place update
	// Startup project/file opens run BEFORE the render loop, so an exception here
	// is uncaught and aborts the process (openFile guards itself, but this is a
	// belt-and-suspenders net for setProjectRoot / focus and any future path).
	try {
		if (!projectArg.empty()) editor.setProjectRoot(projectArg);
		for (auto& p : filesArg) editor.openFile(p);
		if (focusArg) editor.setFocusMode(true);
	}
	catch (const std::exception& ex) {
		std::fprintf(stderr, "startup open failed: %s\n", ex.what());
	}
	SDL_Event event;

	while (!editor.isDone()) {
		Uint64 frameStartNs = SDL_GetTicksNS();

		// poll and handle events (inputs, window resize, etc.)
		while (SDL_PollEvent(&event)) {
			ImGui_ImplSDL3_ProcessEvent(&event);
			if (event.type == SDL_EVENT_DROP_FILE) {
				// SDL3: event.drop.data is owned by SDL and lives until the next
				// SDL_EVENT_DROP_COMPLETE — copy it before using.
				const char* dropped_filedir = event.drop.data;
				if (dropped_filedir) {
					editor.openFile(std::string(dropped_filedir));
				}
			}
			if (event.type == SDL_EVENT_QUIT) {
				editor.tryToQuit();

			} else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) {
				editor.tryToQuit();
			}
		}

		if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
			SDL_Delay(10);
			continue;
		}

		// start the Dear ImGui frame
		ImGui_ImplSDLGPU3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();

		// render editor. A stray exception here — most often a std::filesystem
		// exception from a toolchain/include scan hitting a permission-denied,
		// too-long, or reparse-point path on a big tree (e.g. a UE engine dir) —
		// must NOT abort the whole editor and lose the user's unsaved work. Log
		// it and keep running; ImGui's error recovery closes any windows the
		// aborted frame left open, and the next frame renders clean.
		try {
			editor.render();
		}
		catch (const std::exception& ex) {
			static int loggedFrameErrors = 0;
			if (loggedFrameErrors++ < 50) {
				std::error_code lec;
				auto logPath = Editor::userConfigDir() / "crash.log";
				std::filesystem::create_directories(logPath.parent_path(), lec);
				std::ofstream(logPath, std::ios::app)
					<< "[render] recovered from exception: " << ex.what() << "\n";
				std::fprintf(stderr, "[render] recovered from exception: %s\n", ex.what());
			}
		}
		catch (...) {
			std::fprintf(stderr, "[render] recovered from non-std exception\n");
		}

		// A forwarded open request from a second launch (single-instance) asks us
		// to surface the window.
		if (editor.consumeRaiseRequest()) {
			SDL_ShowWindow(window);
			SDL_RaiseWindow(window);
			SDL_FlashWindow(window, SDL_FLASH_UNTIL_FOCUSED);
		}

		// render to the screen
		ImGui::Render();
		ImDrawData* draw_data = ImGui::GetDrawData();
		const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);

		// acquire a GPU command buffer and swapchain texture
		SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(gpu_device);
		SDL_GPUTexture* swapchain_texture;
		SDL_AcquireGPUSwapchainTexture(command_buffer, window, &swapchain_texture, nullptr, nullptr);

		if (swapchain_texture != nullptr && !is_minimized) {
			// setup and start a render pass
			ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);

			SDL_GPUColorTargetInfo target_info = {};
			target_info.texture = swapchain_texture;
			target_info.load_op = SDL_GPU_LOADOP_CLEAR;
			target_info.store_op = SDL_GPU_STOREOP_STORE;
			target_info.mip_level = 0;
			target_info.layer_or_depth_plane = 0;
			target_info.cycle = false;
			SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);

			// render ImGui
			ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, render_pass);
			SDL_EndGPURenderPass(render_pass);
		}

		// submit the command buffer
		SDL_SubmitGPUCommandBuffer(command_buffer);

		// render and present additional platform windows (multi-viewport)
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
		}

		// Frame pacing. Pick the active target FPS: the user's cap, or a low
		// idle rate when the editor is in the background. Sleep off the
		// remainder of the frame budget. 0 = unlimited and skips the sleep.
		//
		// Focus check must consider ALL of the app's windows, not just the main
		// one: with multi-viewport, a popped-out document lives in its own OS
		// window, so the main window loses INPUT_FOCUS even though the app is
		// active. Checking only the main window made the idle throttle drop us
		// to 10 FPS the moment a tab was popped out (the "multiviewport tanks
		// FPS" cliff). SDL_GetKeyboardFocus() returns a non-null window when ANY
		// of our windows (main or viewport) has focus, and null only when a
		// different application is focused.
		int targetFps = editor.fpsLimit();
		bool focused = (SDL_GetKeyboardFocus() != nullptr);
		if (editor.idleThrottle() && !focused) {
			targetFps = (targetFps == 0) ? 10 : (targetFps < 10 ? targetFps : 10);
		}
		if (targetFps > 0) {
			Uint64 budgetNs = 1000000000ull / (Uint64) targetFps;
			Uint64 elapsedNs = SDL_GetTicksNS() - frameStartNs;
			if (elapsedNs < budgetNs) SDL_DelayNS(budgetNs - elapsedNs);
		}
	}

	// cleanup
	SDL_WaitForGPUIdle(gpu_device);
	ImGui_ImplSDL3_Shutdown();
	ImGui_ImplSDLGPU3_Shutdown();
	ImGui::DestroyContext();

	SDL_ReleaseWindowFromGPUDevice(gpu_device, window);
	SDL_DestroyGPUDevice(gpu_device);
	SDL_DestroyWindow(window);
	SDL_Quit();


	return 0;
}
#endif