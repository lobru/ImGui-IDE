# Context Menus

The text editor provides an option to create context menus when the user right clicks on a line number or anywhere in the text. The editor creates these menus and a provided callback needs to populate them.

In the example below, the line number context menu is used to control breakpoints and the text context menu is used to report the current position in the text. Only the first is depicted in the screenshot.

```c++
```c++
		constexpr auto command = [](const char* name, const char* shortcut, bool* selected = false, int shortcut_key = 0) -> bool {
			if (shortcut_key != 0 && ImGui::IsKeyPressed(static_cast<ImGuiKey>(shortcut_key), false) && ImGui::IsKeyPressed(ImGuiKey_LeftCtrl, false))
				*selected = true;
			bool item = ImGui::MenuItem(name, shortcut, selected, true);
			return item;
			};

		if (ImGui::IsItemClicked(0)) ImGui::CloseCurrentPopup();
		if (command("Copy", "Ctrl-C", nullptr, ImGuiKey_C)) { clipboard_copy(); ImGui::CloseCurrentPopup(); }
		else if (command("Cut", "Ctrl-X", nullptr, ImGuiKey_X)) { clipboard_cut(); ImGui::CloseCurrentPopup(); }
		else if (command("Paste", "Ctrl-V", nullptr, ImGuiKey_V)) { clipboard_paste(); ImGui::CloseCurrentPopup(); }
		else if (command("Select All", "Ctrl-A", nullptr, ImGuiKey_A)) { select_all(); ImGui::CloseCurrentPopup(); }



editor.SetLineNumberContextMenuCallback([](int line) {
	if (ImGui::MenuItem("Set Breakpoint")) { /* handle click */ }
	if (ImGui::MenuItem("Remove Breakpoint")) { /* handle click */ }
});

editor.SetTextContextMenuCallback([](int line, int column) {
	ImGui::Text("Line %d, column %d", line + 1, column + 1);
});
```

![Context Menu](contextMenus.png)
