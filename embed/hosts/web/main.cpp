//	main.cpp - web host: the TextEditor embed API compiled to WebAssembly
//	Part of ImGui-IDE (github.com/lobotomy-x/ImGuiColorTextEdit)
//
//	Copyright (c) 2024-2026 Johan A. Goossens, Logan Brunet. All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.
//
//	Browser test host for the embed C API and first step of the web IDE track.
//	The editor core + C API are statically compiled into the wasm module (the
//	"DLL" boundary doesn't exist on the web), but ALL editor access goes
//	through texteditor_embed.h - so what works here is exactly the surface a
//	native host gets. ImGui renders via GLFW (emscripten's JS implementation)
//	+ WebGL2. The document persists to localStorage.
//
//	Build: ./build_web.sh (needs emsdk + an imgui checkout, see script).

#include <cstdlib>
#include <cstring>
#include <string>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "texteditor_embed.h"

//
//	localStorage bridge
//

EM_JS(void, js_store, (const char* key, const char* text), {
	try { localStorage.setItem(UTF8ToString(key), UTF8ToString(text)); } catch (e) {}
});

EM_JS(char*, js_fetch, (const char* key), {
	try {
		var s = localStorage.getItem(UTF8ToString(key));
		if (s === null) { return 0; }
		var n = lengthBytesUTF8(s) + 1;
		var p = _malloc(n);
		stringToUTF8(s, p, n);
		return p;
	} catch (e) { return 0; }
});

static const char* kStorageKey = "imgui-ide-web-document";
static const char* kStorageLang = "imgui-ide-web-language";

//
//	sample documents
//

static const char* kSampleHlsl =
	"// HLSL sample - the ReShade shader-editor use case\n"
	"Texture2D backBuffer : register(t0);\n"
	"SamplerState samp : register(s0);\n"
	"\n"
	"float4 PS_Vignette(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target\n"
	"{\n"
	"\tfloat3 color = backBuffer.Sample(samp, uv).rgb;\n"
	"\tfloat2 d = uv - 0.5;\n"
	"\tfloat vignette = 1.0 - dot(d, d) * 1.5;\n"
	"\treturn float4(color * saturate(vignette), 1.0);\n"
	"}\n";

static const char* kSampleLua =
	"-- Lua sample - the UEVR script-editor use case\n"
	"local api = uevr.api\n"
	"\n"
	"uevr.sdk.callbacks.on_pre_engine_tick(function(engine, delta)\n"
	"\tlocal pawn = api:get_local_pawn(0)\n"
	"\tif pawn ~= nil then\n"
	"\t\t-- print(pawn:get_full_name())\n"
	"\tend\n"
	"end)\n";

static const char* kSampleCpp =
	"// C++ sample\n"
	"#include <vector>\n"
	"#include <string>\n"
	"\n"
	"template <typename T>\n"
	"struct Pool {\n"
	"\tstd::vector<T> items;\n"
	"\n"
	"\tT& acquire() {\n"
	"\t\titems.emplace_back();\n"
	"\t\treturn items.back();\n"
	"\t}\n"
	"};\n";

//
//	app state
//

static GLFWwindow* window = nullptr;
static te_editor* editor = nullptr;
static int currentLang = TE_LANG_HLSL;
static bool darkTheme = true;
static double statusUntil = 0.0;
static char statusMsg[128] = "";

struct LangChoice { const char* name; te_language lang; };

static const LangChoice kLangs[] = {
	{ "None", TE_LANG_NONE }, { "C", TE_LANG_C }, { "C++", TE_LANG_CPP },
	{ "C#", TE_LANG_CSHARP }, { "AngelScript", TE_LANG_ANGELSCRIPT },
	{ "Lua", TE_LANG_LUA }, { "Python", TE_LANG_PYTHON }, { "GLSL", TE_LANG_GLSL },
	{ "HLSL", TE_LANG_HLSL }, { "JSON", TE_LANG_JSON },
	{ "Markdown", TE_LANG_MARKDOWN }, { "SQL", TE_LANG_SQL }, { "INI", TE_LANG_INI },
};

static void setStatus(const char* msg) {
	std::snprintf(statusMsg, sizeof(statusMsg), "%s", msg);
	statusUntil = emscripten_get_now() + 2500.0;
}

static void saveDocument() {
	size_t n = te_get_text(editor, nullptr, 0);
	std::string buf(n + 1, '\0');
	te_get_text(editor, buf.data(), n + 1);
	js_store(kStorageKey, buf.c_str());
	js_store(kStorageLang, std::to_string(currentLang).c_str());
	te_mark_saved(editor);
	setStatus("saved to localStorage");
}

static void loadSample(const char* text, te_language lang) {
	te_set_text(editor, text, (size_t) -1);
	currentLang = lang;
	te_set_language(editor, lang);
}

//
//	frame
//

static void mainLoop() {
	// match the canvas to its CSS size every frame (handles window resizes)
	double cssW, cssH;
	emscripten_get_element_css_size("#canvas", &cssW, &cssH);
	int w = (int) cssW, h = (int) cssH;
	int curW, curH;
	glfwGetWindowSize(window, &curW, &curH);

	if ((w != curW || h != curH) && w > 0 && h > 0) {
		glfwSetWindowSize(window, w, h);
	}

	glfwPollEvents();
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(io.DisplaySize);

	ImGui::Begin("##app", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

	// ── toolbar ──────────────────────────────────────────────────────
	if (ImGui::Button(te_is_dirty(editor) ? "Save*" : "Save")) {
		saveDocument();
	}

	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);

	if (ImGui::BeginCombo("##lang", kLangs[currentLang].name)) {
		for (int i = 0; i < (int) (sizeof(kLangs) / sizeof(kLangs[0])); i++) {
			if (ImGui::Selectable(kLangs[i].name, i == currentLang)) {
				currentLang = i;
				te_set_language(editor, kLangs[i].lang);
			}
		}

		ImGui::EndCombo();
	}

	ImGui::SameLine();

	if (ImGui::Button(darkTheme ? "Light" : "Dark")) {
		darkTheme = !darkTheme;
		te_set_palette(editor, darkTheme ? TE_PALETTE_DARK : TE_PALETTE_LIGHT);

		if (darkTheme) { ImGui::StyleColorsDark(); } else { ImGui::StyleColorsLight(); }
	}

	ImGui::SameLine();
	ImGui::TextUnformatted("|");
	ImGui::SameLine();

	if (ImGui::Button("HLSL sample")) { loadSample(kSampleHlsl, TE_LANG_HLSL); }

	ImGui::SameLine();

	if (ImGui::Button("Lua sample")) { loadSample(kSampleLua, TE_LANG_LUA); }

	ImGui::SameLine();

	if (ImGui::Button("C++ sample")) { loadSample(kSampleCpp, TE_LANG_CPP); }

	// keep language index in sync with what loadSample set
	for (int i = 0; i < (int) (sizeof(kLangs) / sizeof(kLangs[0])); i++) {
		if (kLangs[i].lang == currentLang) { currentLang = i; break; }
	}

	// ── editor fills everything but the status line ──────────────────
	float statusH = ImGui::GetFrameHeightWithSpacing();
	te_render(editor, "##source", 0.0f, -statusH, 0);

	// ── status line ──────────────────────────────────────────────────
	int line = 0, column = 0;
	te_get_cursor(editor, &line, &column);
	ImGui::Text("Ln %d, Col %d  %s  %d lines  %s",
		line + 1, column + 1,
		te_is_dirty(editor) ? "[modified]" : "[saved]",
		te_line_count(editor),
		emscripten_get_now() < statusUntil ? statusMsg : "");

	ImGui::End();

	// ── render ───────────────────────────────────────────────────────
	ImGui::Render();
	int fbW, fbH;
	glfwGetFramebufferSize(window, &fbW, &fbH);
	glViewport(0, 0, fbW, fbH);
	glClearColor(darkTheme ? 0.06f : 0.85f, darkTheme ? 0.06f : 0.85f, darkTheme ? 0.07f : 0.86f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	glfwSwapBuffers(window);
}

//
//	entry
//

int main() {
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	window = glfwCreateWindow(1280, 720, "ImGui-IDE web", nullptr, nullptr);
	glfwMakeContextCurrent(window);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef __EMSCRIPTEN__
	ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
#endif
	ImGui_ImplOpenGL3_Init("#version 300 es");

	// the host owns the context; the editor binds to it - same contract as
	// the native DLL even though the web build links statically
	ImGuiMemAllocFunc allocFn; ImGuiMemFreeFunc freeFn; void* allocUd;
	ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &allocUd);
	te_bind_imgui(ImGui::GetCurrentContext(), allocFn, freeFn, allocUd);

	editor = te_create();
	te_set_palette(editor, TE_PALETTE_DARK);
	te_set_show_line_numbers(editor, 1);
	te_set_show_minimap(editor, 1);

	// restore the saved document, or start with the HLSL sample
	char* saved = js_fetch(kStorageKey);

	if (saved && saved[0]) {
		te_set_text(editor, saved, (size_t) -1);
		char* lang = js_fetch(kStorageLang);

		if (lang) {
			currentLang = std::atoi(lang);
			std::free(lang);
		}

		te_set_language(editor, kLangs[currentLang].lang);
	} else {
		loadSample(kSampleHlsl, TE_LANG_HLSL);
	}

	if (saved) {
		std::free(saved);
	}

	emscripten_set_main_loop(mainLoop, 0, true);
	return 0;
}
