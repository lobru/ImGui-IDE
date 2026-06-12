//	texteditor_embed.h - flat C API for embedding the TextEditor core as a DLL
//	Part of ImGui-IDE (github.com/lobotomy-x/ImGuiColorTextEdit)
//
//	Copyright (c) 2024-2026 Johan A. Goossens, Logan Brunet. All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.
//
//	Design notes
//	------------
//	The DLL compiles its own copy of Dear ImGui (no backends) and renders into
//	the HOST's ImGui context. This is the documented ImGui cross-DLL pattern:
//	both binaries must compile the SAME ImGui version with the same config, and
//	the host's context + allocator functions must be installed in the DLL via
//	te_bind_imgui() before the first te_render() call. Verify the version match
//	at runtime with te_imgui_version_num() — refuse to render on mismatch
//	instead of crashing mysteriously.
//
//	The API is a flat C surface so the DLL is usable from any compiler/language
//	(MSVC host + MinGW DLL, Rust, C#, Lua FFI, ...). No C++ types cross the
//	boundary; strings are UTF-8; buffers use the "call twice" sizing pattern.
//
//	Thread safety: none. Call everything from the host's render thread.

#ifndef TEXTEDITOR_EMBED_H
#define TEXTEDITOR_EMBED_H

#include <stddef.h> /* size_t */
#include <stdint.h> /* uint32_t */

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
	#if defined(TE_EMBED_BUILD)
		#define TE_API __declspec(dllexport)
	#elif defined(TE_EMBED_STATIC)
		#define TE_API
	#else
		#define TE_API __declspec(dllimport)
	#endif
#else
	#if defined(TE_EMBED_BUILD)
		#define TE_API __attribute__((visibility("default")))
	#else
		#define TE_API
	#endif
#endif

/* ── versioning ─────────────────────────────────────────────────────── */

/* Semver of this embed API. Bump major on any breaking signature change. */
#define TE_EMBED_VERSION "1.0.0"

TE_API const char* te_version(void);           /* TE_EMBED_VERSION of the DLL  */
TE_API const char* te_imgui_version(void);     /* IMGUI_VERSION it was built with  */
TE_API int         te_imgui_version_num(void); /* IMGUI_VERSION_NUM, e.g. 19270 */

/* ── host ImGui binding ─────────────────────────────────────────────── */

/* Match ImGuiMemAllocFunc / ImGuiMemFreeFunc without including imgui.h. */
typedef void* (*te_alloc_fn)(size_t size, void* user_data);
typedef void  (*te_free_fn)(void* ptr, void* user_data);

/*	Install the host's ImGui context and allocators into this DLL.
	Call once after the host has created its context (and again if the host
	ever recreates it). `ctx` is the host's ImGuiContext*; the allocator
	functions are what the host passed to / gets from
	ImGui::GetAllocatorFunctions(). All four may be retrieved on the host side
	with a couple of lines — see embed/README.md for per-host recipes. */
TE_API void te_bind_imgui(void* ctx, te_alloc_fn alloc, te_free_fn free, void* user_data);

/* ── editor lifetime ────────────────────────────────────────────────── */

typedef struct te_editor te_editor; /* opaque */

TE_API te_editor* te_create(void);
TE_API void       te_destroy(te_editor* ed);

/* ── text ───────────────────────────────────────────────────────────── */

/*	Set the whole document. `len` is the byte length of `utf8`;
	pass (size_t)-1 to use strlen(). */
TE_API void te_set_text(te_editor* ed, const char* utf8, size_t len);

/*	Copy the document into `buf` (NUL-terminated when cap > 0) and return the
	full byte length excluding the NUL. Call with cap 0 to size a buffer. */
TE_API size_t te_get_text(te_editor* ed, char* buf, size_t cap);

/*	Undo-stack position; increases with every edit, decreases on undo. Track
	"dirty since save" by comparing against the value at save time, or use the
	pair below which stores the saved index inside the editor. */
TE_API size_t te_undo_index(te_editor* ed);
TE_API void   te_mark_saved(te_editor* ed);   /* remember current undo index  */
TE_API int    te_is_dirty(te_editor* ed);     /* undo index != saved index    */

/* ── language / appearance ──────────────────────────────────────────── */

typedef enum te_language {
	TE_LANG_NONE = 0,
	TE_LANG_C,
	TE_LANG_CPP,
	TE_LANG_CSHARP,
	TE_LANG_ANGELSCRIPT,
	TE_LANG_LUA,
	TE_LANG_PYTHON,
	TE_LANG_GLSL,
	TE_LANG_HLSL,
	TE_LANG_JSON,
	TE_LANG_MARKDOWN,
	TE_LANG_SQL,
	TE_LANG_INI
} te_language;

TE_API void te_set_language(te_editor* ed, te_language lang);

/*	Load a key=value .lang definition (same format the desktop IDE uses).
	Returns 1 on success, 0 on failure (file missing/unparsable). The returned
	definition is cached for the lifetime of the process. */
TE_API int te_set_language_from_file(te_editor* ed, const char* path);

typedef enum te_palette {
	TE_PALETTE_DARK = 0,
	TE_PALETTE_LIGHT
} te_palette;

TE_API void te_set_palette(te_editor* ed, te_palette palette);

TE_API void te_set_read_only(te_editor* ed, int enabled);
TE_API void te_set_tab_size(te_editor* ed, int size);          /* 1..8   */
TE_API void te_set_line_spacing(te_editor* ed, float spacing); /* 1..2   */
TE_API void te_set_show_line_numbers(te_editor* ed, int enabled);
TE_API void te_set_show_whitespace(te_editor* ed, int enabled);
TE_API void te_set_show_minimap(te_editor* ed, int enabled);
TE_API void te_set_show_matching_brackets(te_editor* ed, int enabled);

/* ── rendering ──────────────────────────────────────────────────────── */

/*	Draw the editor as a child region inside the host's current ImGui window.
	Call between the host's ImGui::Begin()/End() (for ReShade/UEVR overlays:
	inside the overlay/draw-ui callback). `width`/`height` of 0 fill the
	available content region. `title` must be unique per editor instance
	within the parent window (it is the ImGui child id). */
TE_API void te_render(te_editor* ed, const char* title, float width, float height, int border);

/* ── cursor / selection ─────────────────────────────────────────────── */

TE_API void   te_set_cursor(te_editor* ed, int line, int column); /* 0-based */
TE_API void   te_get_cursor(te_editor* ed, int* line, int* column);
TE_API void   te_select_all(te_editor* ed);
TE_API int    te_line_count(te_editor* ed);

/* ── markers (diagnostics, breakpoints, search hits...) ─────────────── */

/*	Colors are ImU32 RGBA as produced by IM_COL32(). Pass 0 to skip coloring
	that part; tooltips may be NULL. `line` is 0-based. */
TE_API void te_add_marker(te_editor* ed, int line,
                          uint32_t line_number_color, uint32_t text_color,
                          const char* line_number_tooltip, const char* text_tooltip);
TE_API void te_clear_markers(te_editor* ed);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TEXTEDITOR_EMBED_H */
