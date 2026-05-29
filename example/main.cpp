//	TextEditor - A syntax highlighting text editor for ImGui
//	Copyright (c) 2024-2026 Johan A. Goossens. All rights reserved.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


//
//	Include files
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "imgui.h"

#if STANDALONE

#include <SDL3/SDL.h>

#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"



#include "editor.h"
#include "dejavu.h"


//
//	main
//

int main(int argc, char** argv) {
	// Parse --project <dir> and positional args. Positionals are sorted into
	// (a) project root (last folder OR project file we see — supports shell
	// context-menu integration: right-click a .sln in Explorer → opens here)
	// and (b) plain files to open as documents.
	std::string projectArg;
	std::vector<std::string> filesArg;
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
	// setup SDL
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		printf("Error: SDL_Init(): %s\n", SDL_GetError());
		return -1;
	}

	SDL_Window* window = SDL_CreateWindow("TextEditor Example", 1280, 720,
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
	}

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
	config.OversampleH = 1;
	config.OversampleV = 1;
	io.Fonts->Clear();
	io.Fonts->AddFontFromMemoryCompressedTTF((void*) &dejavu, dejavuSize, 15.0f, &config);

	// main loop
	// Tell Editor to skip the demo doc when launched with files / project.
	Editor::sSkipDemo = (!projectArg.empty() || !filesArg.empty());
	Editor editor;
	if (!projectArg.empty()) editor.setProjectRoot(projectArg);
	for (auto& p : filesArg) editor.openFile(p);
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

		// render editor
		editor.render();

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