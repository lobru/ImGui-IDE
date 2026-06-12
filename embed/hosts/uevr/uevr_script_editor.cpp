//	uevr_script_editor.cpp - TEMPLATE: embed the TextEditor DLL as a Lua
//	script editor inside a UEVR C++ plugin.
//	Part of ImGui-IDE (github.com/lobotomy-x/ImGuiColorTextEdit)
//
//	Copyright (c) 2024-2026 Johan A. Goossens, Logan Brunet. All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.
//
//	STATUS: integration template, not a drop-in build. It follows the layout of
//	praydog/UEVR-ExamplePlugin (PluginV1 C++ API), where the PLUGIN owns the
//	whole ImGui lifecycle: it compiles ImGui, creates the context, runs the
//	DX11/DX12 backend against the game's device, and calls NewFrame/Render.
//	The editor DLL just draws into that context. Search for "TODO" before
//	building; the UEVR SDK side must come from the ExamplePlugin you start from.
//
//	Build requirements:
//	- texteditor_embed.dll built with TE_EMBED_IMGUI_DIR pointing at the SAME
//	  Dear ImGui checkout this plugin compiles (verify with
//	  te_imgui_version_num() below - it refuses to render on mismatch).
//	- Link nothing else from this repo; the C API is the whole contract.

#if 0 // TODO: flip to 1 inside a real UEVR plugin project

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// TODO: these come from the UEVR ExamplePlugin project
#include "uevr/Plugin.hpp"
#include "imgui.h"

#include "texteditor_embed.h"

class ScriptEditorPlugin : public uevr::Plugin {
public:
	// called once the plugin's ImGui context + render backend exist
	// (in the ExamplePlugin layout that is the end of on_initialize / the
	// first device callback - wherever ImGui::CreateContext() has run)
	void bindEditor() {
		// refuse mismatched ImGui builds instead of crashing inside ImGui
		if (te_imgui_version_num() != IMGUI_VERSION_NUM) {
			API::get()->log_error(
				"script editor disabled: editor DLL ImGui %d != plugin ImGui %d",
				te_imgui_version_num(), IMGUI_VERSION_NUM);
			return;
		}

		ImGuiMemAllocFunc allocFn; ImGuiMemFreeFunc freeFn; void* userData;
		ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &userData);
		te_bind_imgui(ImGui::GetCurrentContext(), allocFn, freeFn, userData);

		editor = te_create();
		te_set_language(editor, TE_LANG_LUA);
		te_set_palette(editor, TE_PALETTE_DARK);
		te_set_show_line_numbers(editor, 1);
		loadScript();
		bound = true;
	}

	// if the plugin recreates its ImGui context on device reset, re-bind:
	void on_device_reset() override {
		bound = false; // bindEditor() again after the new context exists
	}

	// the plugin's normal ImGui draw callback
	void on_draw_ui() override {
		if (!bound) {
			bindEditor();

			if (!bound) {
				return;
			}
		}

		if (ImGui::Begin("Lua Script Editor")) {
			if (ImGui::Button("Save & Reload")) {
				saveScript();
				// TODO: trigger UEVR lua reload (restart the script subsystem
				// or call the reload the ExamplePlugin exposes)
			}

			ImGui::SameLine();
			ImGui::TextUnformatted(te_is_dirty(editor) ? "*modified*" : "saved");

			te_render(editor, "##script_source", 0.0f, 0.0f, 0);
		}

		ImGui::End();
	}

private:
	void loadScript() {
		std::ifstream in(scriptPath, std::ios::binary);

		if (in) {
			std::ostringstream ss;
			ss << in.rdbuf();
			auto text = ss.str();
			te_set_text(editor, text.c_str(), text.size());
		}
	}

	void saveScript() {
		size_t n = te_get_text(editor, nullptr, 0);
		std::string buf(n, '\0');
		te_get_text(editor, buf.data(), n + 1);
		std::ofstream(scriptPath, std::ios::binary).write(buf.data(), (std::streamsize) n);
		te_mark_saved(editor);
	}

	// TODO: resolve from the UEVR profile directory for the current game
	std::filesystem::path scriptPath = "scripts/main.lua";
	te_editor* editor = nullptr;
	bool bound = false;
};

// TODO: the ExamplePlugin's registration macro / instance
std::unique_ptr<ScriptEditorPlugin> g_plugin{new ScriptEditorPlugin()};

#endif
