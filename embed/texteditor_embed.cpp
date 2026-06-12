//	texteditor_embed.cpp - flat C API for embedding the TextEditor core as a DLL
//	Part of ImGui-IDE (github.com/lobotomy-x/ImGuiColorTextEdit)
//
//	Copyright (c) 2024-2026 Johan A. Goossens, Logan Brunet. All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.

#include <cstring>
#include <string>
#include <unordered_map>

#include "imgui.h"

#include "TextEditor.h"
#include "texteditor_embed.h"

//	The opaque handle. Wraps the C++ editor plus the saved-undo-index used by
//	te_is_dirty(); keeping it here means hosts get dirty tracking without
//	maintaining state of their own.
struct te_editor {
	TextEditor editor;
	size_t savedUndoIndex = 0;
};

//
//	versioning
//

const char* te_version(void) { return TE_EMBED_VERSION; }
const char* te_imgui_version(void) { return IMGUI_VERSION; }
int te_imgui_version_num(void) { return IMGUI_VERSION_NUM; }

//
//	host ImGui binding
//

void te_bind_imgui(void* ctx, te_alloc_fn alloc, te_free_fn free, void* user_data) {
	// install the host's allocators first so anything ImGui allocates from
	// this binary uses (and is freed by) the same heap as the host
	if (alloc && free) {
		ImGui::SetAllocatorFunctions(alloc, free, user_data);
	}

	ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));
}

//
//	editor lifetime
//

te_editor* te_create(void) { return new te_editor(); }
void te_destroy(te_editor* ed) { delete ed; }

//
//	text
//

void te_set_text(te_editor* ed, const char* utf8, size_t len) {
	if (!ed || !utf8) {
		return;
	}

	if (len == (size_t) -1) {
		len = std::strlen(utf8);
	}

	ed->editor.SetText(std::string_view(utf8, len));
	ed->savedUndoIndex = ed->editor.GetUndoIndex();
}

size_t te_get_text(te_editor* ed, char* buf, size_t cap) {
	if (!ed) {
		if (buf && cap) {
			buf[0] = '\0';
		}

		return 0;
	}

	std::string text = ed->editor.GetText();

	if (buf && cap) {
		size_t n = text.size() < cap - 1 ? text.size() : cap - 1;
		std::memcpy(buf, text.data(), n);
		buf[n] = '\0';
	}

	return text.size();
}

size_t te_undo_index(te_editor* ed) { return ed ? ed->editor.GetUndoIndex() : 0; }

void te_mark_saved(te_editor* ed) {
	if (ed) {
		ed->savedUndoIndex = ed->editor.GetUndoIndex();
	}
}

int te_is_dirty(te_editor* ed) {
	return (ed && ed->editor.GetUndoIndex() != ed->savedUndoIndex) ? 1 : 0;
}

//
//	language / appearance
//

void te_set_language(te_editor* ed, te_language lang) {
	if (!ed) {
		return;
	}

	const TextEditor::Language* l = nullptr;

	switch (lang) {
		case TE_LANG_NONE: break;
		case TE_LANG_C: l = TextEditor::Language::C(); break;
		case TE_LANG_CPP: l = TextEditor::Language::Cpp(); break;
		case TE_LANG_CSHARP: l = TextEditor::Language::Cs(); break;
		case TE_LANG_ANGELSCRIPT: l = TextEditor::Language::AngelScript(); break;
		case TE_LANG_LUA: l = TextEditor::Language::Lua(); break;
		case TE_LANG_PYTHON: l = TextEditor::Language::Python(); break;
		case TE_LANG_GLSL: l = TextEditor::Language::Glsl(); break;
		case TE_LANG_HLSL: l = TextEditor::Language::Hlsl(); break;
		case TE_LANG_JSON: l = TextEditor::Language::Json(); break;
		case TE_LANG_MARKDOWN: l = TextEditor::Language::Markdown(); break;
		case TE_LANG_SQL: l = TextEditor::Language::Sql(); break;
		case TE_LANG_INI: l = TextEditor::Language::Ini(); break;
	}

	ed->editor.SetLanguage(l);
}

int te_set_language_from_file(te_editor* ed, const char* path) {
	if (!ed || !path) {
		return 0;
	}

	// Language::FromFile returns pointers that must stay alive as long as any
	// editor references them; cache per path for the process lifetime (same
	// policy the desktop IDE uses for its languages/ directory)
	static std::unordered_map<std::string, const TextEditor::Language*> cache;
	auto it = cache.find(path);

	if (it == cache.end()) {
		it = cache.emplace(path, TextEditor::Language::FromFile(path)).first;
	}

	if (!it->second) {
		return 0;
	}

	ed->editor.SetLanguage(it->second);
	return 1;
}

void te_set_palette(te_editor* ed, te_palette palette) {
	if (!ed) {
		return;
	}

	switch (palette) {
		case TE_PALETTE_DARK: ed->editor.SetPalette(TextEditor::GetDarkPalette()); break;
		case TE_PALETTE_LIGHT: ed->editor.SetPalette(TextEditor::GetLightPalette()); break;
	}
}

void te_set_read_only(te_editor* ed, int enabled) {
	if (ed) {
		ed->editor.SetReadOnlyEnabled(enabled != 0);
	}
}

void te_set_tab_size(te_editor* ed, int size) {
	if (ed) {
		ed->editor.SetTabSize(size);
	}
}

void te_set_line_spacing(te_editor* ed, float spacing) {
	if (ed) {
		ed->editor.SetLineSpacing(spacing);
	}
}

void te_set_show_line_numbers(te_editor* ed, int enabled) {
	if (ed) {
		ed->editor.SetShowLineNumbersEnabled(enabled != 0);
	}
}

void te_set_show_whitespace(te_editor* ed, int enabled) {
	if (ed) {
		ed->editor.SetShowWhitespacesEnabled(enabled != 0);
	}
}

void te_set_show_minimap(te_editor* ed, int enabled) {
	if (ed) {
		ed->editor.SetShowScrollbarMiniMapEnabled(enabled != 0);
	}
}

void te_set_show_matching_brackets(te_editor* ed, int enabled) {
	if (ed) {
		ed->editor.SetShowMatchingBrackets(enabled != 0);
	}
}

//
//	rendering
//

void te_render(te_editor* ed, const char* title, float width, float height, int border) {
	if (!ed || !title) {
		return;
	}

	// refuse to render without a bound context: ImGui::GetCurrentContext()
	// is DLL-local, so a missing te_bind_imgui() shows up here, not as a
	// mysterious crash inside ImGui
	if (ImGui::GetCurrentContext() == nullptr) {
		return;
	}

	ed->editor.Render(title, ImVec2(width, height), border != 0);
}

//
//	cursor / selection
//

void te_set_cursor(te_editor* ed, int line, int column) {
	if (ed) {
		ed->editor.SetCursor(line, column);
	}
}

void te_get_cursor(te_editor* ed, int* line, int* column) {
	int l = 0, c = 0;

	if (ed) {
		ed->editor.GetMainCursor(l, c);
	}

	if (line) {
		*line = l;
	}

	if (column) {
		*column = c;
	}
}

void te_select_all(te_editor* ed) {
	if (ed) {
		ed->editor.SelectAll();
	}
}

int te_line_count(te_editor* ed) {
	return ed ? ed->editor.GetLineCount() : 0;
}

//
//	markers
//

void te_add_marker(te_editor* ed, int line, uint32_t line_number_color, uint32_t text_color,
                   const char* line_number_tooltip, const char* text_tooltip) {
	if (ed) {
		ed->editor.AddMarker(
			line,
			line_number_color,
			text_color,
			line_number_tooltip ? std::string_view(line_number_tooltip) : std::string_view(),
			text_tooltip ? std::string_view(text_tooltip) : std::string_view());
	}
}

void te_clear_markers(te_editor* ed) {
	if (ed) {
		ed->editor.ClearMarkers();
	}
}
