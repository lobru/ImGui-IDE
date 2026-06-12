//	render_test.cpp - headless integration test for the TextEditor embed DLL
//	Part of ImGui-IDE (github.com/lobotomy-x/ImGuiColorTextEdit)
//
//	Copyright (c) 2024-2026 Johan A. Goossens, Logan Brunet. All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.
//
//	This executable plays the HOST role: it compiles its OWN copy of Dear ImGui
//	(the null platform/render backends - no window, no GPU), creates the
//	context, and hands it to the editor DLL with te_bind_imgui(). That
//	exercises the exact cross-binary contract ReShade/UEVR embeds rely on:
//	two ImGui compilations, one context, one allocator set.
//
//	Run via ctest (added by embed/CMakeLists.txt when TE_EMBED_BUILD_TESTS=ON).

#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_impl_null.h"

#include "texteditor_embed.h"

static int failures = 0;

#define CHECK(cond, ...) \
	do { \
		if (!(cond)) { \
			failures++; \
			std::printf("FAIL %s:%d: %s\n      ", __FILE__, __LINE__, #cond); \
			std::printf(__VA_ARGS__); \
			std::printf("\n"); \
		} \
	} while (0)

//	one frame: host drives ImGui, editor DLL draws into the host's context
static int renderFrame(te_editor* ed, bool focusEditor = false) {
	ImGui_ImplNullPlatform_NewFrame();
	ImGui_ImplNullRender_NewFrame();
	ImGui::NewFrame();

	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(1024.0f, 768.0f));

	if (ImGui::Begin("host window", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
		if (focusEditor) {
			ImGui::SetNextWindowFocus();
		}

		te_render(ed, "##editor", 0.0f, 0.0f, 0);
	}

	ImGui::End();
	ImGui::Render();
	return ImGui::GetDrawData() ? ImGui::GetDrawData()->TotalVtxCount : 0;
}

int main(int, char**) {
	IMGUI_CHECKVERSION();

	// 1. version contract: host and DLL must agree before anything renders
	CHECK(te_imgui_version_num() == IMGUI_VERSION_NUM,
		"DLL imgui %d vs host imgui %d", te_imgui_version_num(), IMGUI_VERSION_NUM);
	CHECK(std::strcmp(te_version(), TE_EMBED_VERSION) == 0,
		"DLL embed api %s vs header %s", te_version(), TE_EMBED_VERSION);

	if (failures) { // mismatched builds would crash below; stop here
		return 1;
	}

	// 2. te_render before any bind must be a safe no-op (returns, no crash)
	te_editor* early = te_create();
	te_render(early, "##early", 0.0f, 0.0f, 0);
	te_destroy(early);

	// 3. host owns the context; DLL gets it via te_bind_imgui
	ImGui::CreateContext();
	ImGui_ImplNullPlatform_Init();
	ImGui_ImplNullRender_Init();

	ImGuiMemAllocFunc allocFn; ImGuiMemFreeFunc freeFn; void* allocUd;
	ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &allocUd);
	te_bind_imgui(ImGui::GetCurrentContext(), allocFn, freeFn, allocUd);

	te_editor* ed = te_create();
	te_set_language(ed, TE_LANG_HLSL);
	te_set_palette(ed, TE_PALETTE_DARK);

	const char* shortText = "float4 main() : SV_Target { return 0; }\n";
	te_set_text(ed, shortText, (size_t) -1);

	// 4. drawing across the DLL boundary produces real geometry
	int vtxShort = 0;

	for (int i = 0; i < 3; i++) {
		vtxShort = renderFrame(ed);
	}

	CHECK(vtxShort > 50, "editor drew %d vertices for one line of HLSL", vtxShort);

	// 5. more text => more vertices (the editor really is drawing the document)
	std::string longText;

	for (int i = 0; i < 100; i++) {
		longText += "float v" + std::to_string(i) + " = " + std::to_string(i) + ".0; // comment\n";
	}

	te_set_text(ed, longText.c_str(), longText.size());
	int vtxLong = 0;

	for (int i = 0; i < 3; i++) {
		vtxLong = renderFrame(ed);
	}

	CHECK(vtxLong > vtxShort * 2, "long doc drew %d vs short %d", vtxLong, vtxShort);

	// 6. SetCursor is deferred to Render; after a frame it must be applied
	te_set_cursor(ed, 5, 8);
	renderFrame(ed);
	int line = -1, column = -1;
	te_get_cursor(ed, &line, &column);
	CHECK(line == 5 && column == 8, "cursor at %d,%d after set(5,8)+render", line, column);

	// 7. dirty tracking: set_text resets, typing marks dirty.
	//    Type through the host's input queue: focus the editor child, queue a
	//    character, render - the editor consumes it like any ImGui widget.
	CHECK(te_is_dirty(ed) == 0, "fresh document reports dirty");
	renderFrame(ed, true);            // focus the editor child
	ImGui::GetIO().AddInputCharacter('x');
	renderFrame(ed);                  // editor consumes the queued char
	size_t after = te_get_text(ed, nullptr, 0);
	CHECK(after == longText.size() + 1, "typed char changed doc size %zu -> %zu",
		longText.size(), after);
	CHECK(te_is_dirty(ed) == 1, "dirty flag after typing");
	te_mark_saved(ed);
	CHECK(te_is_dirty(ed) == 0, "dirty flag after mark_saved");

	// 8. markers render without blowing up
	te_add_marker(ed, 2, 0xFF3333FFu, 0x553333FFu, "error", "synthetic error");
	renderFrame(ed);
	te_clear_markers(ed);
	renderFrame(ed);

	// 9. second editor instance coexists in the same context
	te_editor* ed2 = te_create();
	te_set_text(ed2, "x = 1\n", (size_t) -1);
	te_set_language(ed2, TE_LANG_LUA);
	renderFrame(ed2);
	te_destroy(ed2);

	te_destroy(ed);

	ImGui_ImplNullRender_Shutdown();
	ImGui_ImplNullPlatform_Shutdown();
	ImGui::DestroyContext();

	if (failures == 0) {
		std::printf("render_test: all checks passed\n");
	}

	return failures == 0 ? 0 : 1;
}
