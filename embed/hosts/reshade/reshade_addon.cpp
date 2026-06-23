//	reshade_addon.cpp - TEMPLATE: a ReShade add-on that embeds the TextEditor DLL
//	as an in-overlay shader (.fx) editor.
//	Part of ImGui-IDE (github.com/lobotomy-x/ImGuiColorTextEdit)
//
//	Copyright (c) 2026 Logan Brunet. All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.
//
//	STATUS: integration template, not a drop-in build (same convention as
//	hosts/uevr/). It can only be validated against a real ReShade install + a
//	running game, so the SDK-specific bits are marked TODO. The non-TODO scaffold
//	(add-on registration, overlay, editor binding, load/save) is what every
//	ReShade add-on looks like; fill the TODOs against the ReShade version you ship
//	for.
//
//	── Why ReShade is different from the desktop/UEVR hosts ────────────────────
//	A ReShade add-on does NOT get the host's ImGuiContext*. Overlay drawing goes
//	through ReShade's imgui *function table* (reshade_overlay.hpp), which exposes
//	the PUBLIC ImGui API for the ONE IMGUI_VERSION_NUM that the target ReShade
//	release pins. Two consequences:
//
//	  1. The editor core (TextEditor.cpp + texteditor_embed.cpp) must be compiled
//	     against the PUBLIC ImGui API only — build the embed DLL/static lib with
//	     TE_EMBED_NO_IMGUI_INTERNAL=ON (CI proves this variant builds), and supply
//	     ImGui via reshade_overlay.hpp instead of a bundled imgui.cpp. Graceful
//	     feature cost is documented in embed/README.md (minimap, IME, diff's synced
//	     scrollbars, autocomplete z-order nudge).
//	  2. The ImGui version MUST match ReShade's exactly. Verify at runtime with
//	     te_imgui_version_num() == IMGUI_VERSION_NUM and refuse to render on a
//	     mismatch — see below.
//
//	── Build (Windows, x64) ────────────────────────────────────────────────────
//	  - Get the ReShade SDK headers (crosire/reshade: include/reshade.hpp,
//	    reshade_overlay.hpp, reshade_api*.hpp) at the version you target.
//	  - Compile this file + TextEditor.cpp + TextDiff.cpp + texteditor_embed.cpp
//	    with TE_NO_IMGUI_INTERNAL=1 and the ReShade include dir on the path, into a
//	    single DLL renamed to <name>.addon64 (ReShade loads *.addon64 next to it).
//	  - Do NOT compile a bundled imgui.cpp: reshade_overlay.hpp routes ImGui:: to
//	    ReShade's table. Include reshade_overlay.hpp in every TU that calls ImGui::.

#if 0 // TODO: flip to 1 once the ReShade SDK headers are on the include path

#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_OBSOLETE_KEYIO

#include <imgui.h>            // ReShade's pinned ImGui version (types/enums)
#include <reshade.hpp>        // pulls in reshade_overlay.hpp -> ImGui:: via the table

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "texteditor_embed.h"

namespace {

te_editor*  g_editor = nullptr;
bool        g_bound = false;
std::string g_currentFile;     // .fx path currently loaded in the editor
std::string g_status;

//	Lazily bind the editor to ReShade's ImGui context (set up by the time the
//	overlay callback runs). Refuse on a version mismatch rather than crashing.
bool ensureBound() {
	if (g_bound) {
		return true;
	}

	if (te_imgui_version_num() != IMGUI_VERSION_NUM) {
		reshade::log::message(reshade::log::level::error,
			"Shader Editor disabled: editor DLL ImGui != ReShade ImGui");
		return false;
	}

	ImGuiMemAllocFunc allocFn; ImGuiMemFreeFunc freeFn; void* userData;
	ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &userData);
	te_bind_imgui(ImGui::GetCurrentContext(), allocFn, freeFn, userData);

	g_editor = te_create();
	te_set_language(g_editor, TE_LANG_HLSL);   // ReShade FX is HLSL-like
	te_set_palette(g_editor, TE_PALETTE_DARK);
	te_set_show_line_numbers(g_editor, 1);
	te_set_show_matching_brackets(g_editor, 1);
	g_bound = true;
	return true;
}

void loadFile(const std::string& path) {
	std::ifstream in(path, std::ios::binary);

	if (!in) {
		g_status = "open failed: " + path;
		return;
	}

	std::ostringstream ss;
	ss << in.rdbuf();
	std::string text = ss.str();
	te_set_text(g_editor, text.c_str(), text.size());
	te_mark_saved(g_editor);
	g_currentFile = path;
	g_status = "loaded " + path;
}

void saveFile(reshade::api::effect_runtime* runtime) {
	if (g_currentFile.empty()) {
		return;
	}

	size_t n = te_get_text(g_editor, nullptr, 0);
	std::string buf(n + 1, '\0');
	te_get_text(g_editor, buf.data(), n + 1);
	buf.resize(n);
	std::ofstream(g_currentFile, std::ios::binary).write(buf.data(), (std::streamsize) n);
	te_mark_saved(g_editor);
	g_status = "saved " + g_currentFile;

	// Force ReShade to recompile the edited effect so the change is live.
	// TODO: exact call depends on the ReShade SDK version, e.g.
	//   runtime->reload_effect(...) or the global "reload all effects" path.
	(void) runtime;
}

//	Enumerate the loaded effect files for the file picker.
//	TODO: the exact effect_runtime enumeration API is version-specific. Sketch:
//	  runtime->enumerate_techniques(nullptr, [](effect_runtime*, effect_technique tech, void* ud){
//	      char fx[260]; runtime->get_technique_effect_name(tech, fx); ... collect unique paths ...
//	  }, &out);
//	Resolve each to an absolute path under the preset's shader search dirs.
std::vector<std::string> enumerateEffectFiles(reshade::api::effect_runtime* runtime) {
	std::vector<std::string> out;
	(void) runtime;
	// TODO: fill via the effect_runtime API (see sketch above).
	return out;
}

//	ReShade overlay callback — registered as "Shader Editor".
void drawOverlay(reshade::api::effect_runtime* runtime) {
	if (!ensureBound()) {
		ImGui::TextUnformatted("Shader Editor unavailable (ImGui version mismatch).");
		return;
	}

	// file picker over the active preset's effects
	if (ImGui::BeginCombo("Effect", g_currentFile.empty() ? "(pick an .fx)" : g_currentFile.c_str())) {
		for (const std::string& fx : enumerateEffectFiles(runtime)) {
			if (ImGui::Selectable(fx.c_str(), fx == g_currentFile)) {
				loadFile(fx);
			}
		}

		ImGui::EndCombo();
	}

	ImGui::SameLine();

	if (ImGui::Button("Save & reload") && !g_currentFile.empty()) {
		saveFile(runtime);
	}

	ImGui::SameLine();
	ImGui::TextUnformatted(g_editor && te_is_dirty(g_editor) ? "*modified*" : "");

	if (!g_status.empty()) {
		ImGui::TextDisabled("%s", g_status.c_str());
	}

	// the editor fills the rest of the overlay window
	te_render(g_editor, "##fx_source", 0.0f, 0.0f, 0);
}

} // namespace

//	ReShade discovers add-ons by their DllMain calling register_addon, and reads
//	these two exported strings for the add-ons list.
extern "C" __declspec(dllexport) const char* NAME = "ImGui-IDE Shader Editor";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
	"Edit the active preset's .fx files in-overlay (ImGui-IDE TextEditor).";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule)) {
			return FALSE;
		}

		reshade::register_overlay("Shader Editor", &drawOverlay);
		break;

	case DLL_PROCESS_DETACH:
		if (g_editor) {
			te_destroy(g_editor);
			g_editor = nullptr;
		}

		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}

#endif
