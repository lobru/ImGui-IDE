//	ide_main.cpp - cloud IDE shell: GitHub-backed, runs entirely in the browser
//	Part of ImGui-IDE (github.com/lobotomy-x/ImGuiColorTextEdit)
//
//	Copyright (c) 2024-2026 Johan A. Goossens, Logan Brunet. All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.
//
//	The web-IDE track: where the desktop shell (example/editor.cpp) walks the
//	filesystem with std::filesystem and WinAPI, this shell talks to the GitHub
//	REST API instead - repo tree via the git/trees endpoint, file content via
//	the contents API (raw media type), saves as commits via PUT, search via
//	the code-search API. The editor itself is the same core, accessed through
//	the embed C API (texteditor_embed.h) like every other host.
//
//	Auth: a fine-grained PAT pasted in Settings, kept in localStorage by
//	choice (personal-tool tradeoff; anyone with the browser profile can read
//	it). Build: ./build_ide.sh -> single self-contained imgui_ide_cloud.html.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "texteditor_embed.h"
#include "gh_client.h"
#include "gh_parse.h"

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

static std::string lsGet(const char* key) {
	char* v = js_fetch(key);
	std::string out = v ? v : "";
	std::free(v);
	return out;
}

//
//	app state
//

struct Tab {
	std::string path;       // repo-relative
	std::string name;       // last component, for the tab label
	std::string sha;        // blob sha at load time (empty for new files)
	te_editor* ed = nullptr;
	bool isNew = false;     // first save creates the file
	bool loading = false;
	bool open = true;       // tab-bar close flag
};

enum class Conn { Disconnected, Branches, Tree, Ready };

struct App {
	GLFWwindow* window = nullptr;

	ghc::Config cfg;
	Conn conn = Conn::Disconnected;
	std::string connError;

	std::vector<std::string> branches;
	std::vector<ghp::TreeNode> tree;   // node 0 = root
	bool treeTruncated = false;

	std::vector<Tab> tabs;
	int activeTab = -1;
	int selectTab = -1;                // request focus on this tab next frame

	// pending branch switch (confirmation when dirty tabs exist)
	std::string pendingBranch;

	// save flow
	int savingTab = -1;
	char commitMsg[256] = "";
	bool openSavePopup = false;

	// new-file flow
	char newFilePath[256] = "";
	bool openNewFilePopup = false;

	// search
	char searchQuery[256] = "";
	std::vector<ghp::SearchHit> searchResults;
	bool searching = false;
	bool showSearch = false;
	std::string pendingLocate;         // fragment to locate after a search hit opens

	// settings popup scratch
	char inOwner[128] = "";
	char inRepo[128] = "";
	char inToken[256] = "";
	bool openSettings = false;

	// UI
	float treeWidth = 260.0f;
	bool darkTheme = true;
	std::string status;
	bool statusIsError = false;
	double statusUntil = 0.0;
};

static App app;

static void setStatus(const std::string& msg, bool error = false) {
	app.status = msg;
	app.statusIsError = error;
	app.statusUntil = emscripten_get_now() + (error ? 6000.0 : 3000.0);
}

static void persistConfig() {
	js_store("ide.owner", app.cfg.owner.c_str());
	js_store("ide.repo", app.cfg.repo.c_str());
	js_store("ide.branch", app.cfg.branch.c_str());
	js_store("ide.token", app.cfg.token.c_str());
}

//
//	repo loading
//

static void loadTree() {
	app.conn = Conn::Tree;
	ghc::getTree(app.cfg, [](int status, std::string body) {
		if (status != 200) {
			app.conn = Conn::Disconnected;
			app.connError = ghp::parseErrorMessage(body);
			setStatus("tree: HTTP " + std::to_string(status) + " " + app.connError, true);
			return;
		}

		auto entries = ghp::parseTree(body, &app.treeTruncated);
		app.tree = ghp::buildTreeModel(std::move(entries));
		app.conn = Conn::Ready;
		setStatus(app.cfg.owner + "/" + app.cfg.repo + " @ " + app.cfg.branch + " loaded");
	});
}

static void connect() {
	if (app.cfg.owner.empty() || app.cfg.repo.empty()) {
		app.openSettings = true;
		return;
	}

	app.conn = Conn::Branches;
	app.connError.clear();
	ghc::listBranches(app.cfg, [](int status, std::string body) {
		if (status != 200) {
			app.conn = Conn::Disconnected;
			app.connError = ghp::parseErrorMessage(body);
			setStatus("branches: HTTP " + std::to_string(status) + " " + app.connError, true);
			return;
		}

		app.branches = ghp::parseBranches(body);

		// keep the configured branch if it exists, else fall back sensibly
		bool found = false;

		for (auto& b : app.branches) {
			if (b == app.cfg.branch) { found = true; break; }
		}

		if (!found && !app.branches.empty()) {
			app.cfg.branch = app.branches[0];

			for (auto& b : app.branches) {
				if (b == "main" || b == "master") { app.cfg.branch = b; break; }
			}
		}

		persistConfig();
		loadTree();
	});
}

//
//	tabs
//

static int findTab(const std::string& path) {
	for (int i = 0; i < (int) app.tabs.size(); i++) {
		if (app.tabs[(size_t) i].path == path) {
			return i;
		}
	}

	return -1;
}

static bool anyDirtyTabs() {
	for (auto& t : app.tabs) {
		if (t.ed && (te_is_dirty(t.ed) || t.isNew)) {
			return true;
		}
	}

	return false;
}

static void closeAllTabs() {
	for (auto& t : app.tabs) {
		if (t.ed) {
			te_destroy(t.ed);
		}
	}

	app.tabs.clear();
	app.activeTab = -1;
}

//	place the cursor at the first line containing `needle` (post-search jump)
static void locateInTab(Tab& tab, const std::string& needle) {
	if (needle.empty() || !tab.ed) {
		return;
	}

	size_t n = te_get_text(tab.ed, nullptr, 0);
	std::string text(n + 1, '\0');
	te_get_text(tab.ed, text.data(), n + 1);
	text.resize(n);
	auto pos = text.find(needle);

	if (pos == std::string::npos) {
		return;
	}

	int line = 0;

	for (size_t i = 0; i < pos; i++) {
		if (text[i] == '\n') {
			line++;
		}
	}

	te_set_cursor(tab.ed, line, 0);
}

static void openFile(const std::string& path, const std::string& sha) {
	int existing = findTab(path);

	if (existing >= 0) {
		app.selectTab = existing;
		return;
	}

	Tab tab;
	tab.path = path;
	auto slash = path.find_last_of('/');
	tab.name = slash == std::string::npos ? path : path.substr(slash + 1);
	tab.sha = sha;
	tab.loading = true;
	tab.ed = te_create();
	te_set_palette(tab.ed, app.darkTheme ? TE_PALETTE_DARK : TE_PALETTE_LIGHT);
	te_set_show_line_numbers(tab.ed, 1);
	te_set_show_minimap(tab.ed, 1);
	te_set_language(tab.ed, ghp::languageForPath(path));
	te_set_text(tab.ed, "// loading...", (size_t) -1);
	te_set_read_only(tab.ed, 1);
	app.tabs.push_back(std::move(tab));
	app.selectTab = (int) app.tabs.size() - 1;

	std::string p = path; // captured copies for the async hop
	ghc::getFileRaw(app.cfg, p, [p](int status, std::string body) {
		int idx = findTab(p);

		if (idx < 0) {
			return; // tab closed while loading
		}

		Tab& t = app.tabs[(size_t) idx];
		t.loading = false;

		if (status != 200) {
			te_set_text(t.ed, ("// failed to load: HTTP " + std::to_string(status) + "\n// " +
				ghp::parseErrorMessage(body) + "\n").c_str(), (size_t) -1);
			setStatus(p + ": HTTP " + std::to_string(status), true);
			return;
		}

		te_set_read_only(t.ed, 0);
		te_set_text(t.ed, body.c_str(), body.size());
		te_mark_saved(t.ed);

		if (!app.pendingLocate.empty()) {
			locateInTab(t, app.pendingLocate);
			app.pendingLocate.clear();
		}
	});
}

//
//	save / commit
//

static void saveTab(int idx) {
	if (idx < 0 || idx >= (int) app.tabs.size()) {
		return;
	}

	Tab& t = app.tabs[(size_t) idx];

	if (t.loading || !t.ed) {
		return;
	}

	size_t n = te_get_text(t.ed, nullptr, 0);
	std::string content(n + 1, '\0');
	te_get_text(t.ed, content.data(), n + 1);
	content.resize(n);

	std::string path = t.path;
	std::string msg = app.commitMsg[0] ? app.commitMsg : ("Update " + path + " via ImGui-IDE web");
	setStatus("committing " + path + "...");

	ghc::putFile(app.cfg, path, content, t.sha, msg, [path](int status, std::string body) {
		int i = findTab(path);

		if (i < 0) {
			return;
		}

		Tab& tab = app.tabs[(size_t) i];

		if (status == 200 || status == 201) {
			std::string newSha = ghp::parsePutResponse(body);

			if (!newSha.empty()) {
				tab.sha = newSha;
			}

			tab.isNew = false;
			te_mark_saved(tab.ed);
			setStatus("committed " + path + " to " + app.cfg.branch);
			loadTree(); // pick up the new blob sha / new file in the tree
		} else if (status == 409 || status == 422) {
			setStatus(path + ": conflict - file changed on " + app.cfg.branch +
				" since it was opened (" + ghp::parseErrorMessage(body) + ")", true);
		} else {
			setStatus(path + ": commit failed, HTTP " + std::to_string(status) + " " +
				ghp::parseErrorMessage(body), true);
		}
	});
}

//
//	search
//

static void runSearch() {
	if (!app.searchQuery[0] || app.searching) {
		return;
	}

	app.searching = true;
	app.searchResults.clear();
	ghc::searchCode(app.cfg, app.searchQuery, [](int status, std::string body) {
		app.searching = false;

		if (status != 200) {
			setStatus("search: HTTP " + std::to_string(status) + " " +
				ghp::parseErrorMessage(body), true);
			return;
		}

		app.searchResults = ghp::parseSearchResults(body);
		setStatus(std::to_string(app.searchResults.size()) + " result(s) (default branch only)");
	});
}

//
//	UI pieces
//

static void drawTreeNode(int nodeIdx) {
	const ghp::TreeNode& n = app.tree[(size_t) nodeIdx];

	if (n.isDir) {
		bool openNode = nodeIdx == 0 ||
			ImGui::TreeNodeEx(n.name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);

		if (openNode) {
			for (int c : n.children) {
				drawTreeNode(c);
			}

			if (nodeIdx != 0) {
				ImGui::TreePop();
			}
		}
	} else {
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
			ImGuiTreeNodeFlags_SpanAvailWidth;

		if (findTab(n.path) >= 0) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}

		ImGui::TreeNodeEx(n.name.c_str(), flags);

		if (ImGui::IsItemClicked()) {
			openFile(n.path, n.sha);
		}

		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", n.path.c_str());
		}
	}
}

static void drawTopBar() {
	if (ImGui::Button("Settings")) {
		std::snprintf(app.inOwner, sizeof(app.inOwner), "%s", app.cfg.owner.c_str());
		std::snprintf(app.inRepo, sizeof(app.inRepo), "%s", app.cfg.repo.c_str());
		std::snprintf(app.inToken, sizeof(app.inToken), "%s", app.cfg.token.c_str());
		app.openSettings = true;
	}

	ImGui::SameLine();

	if (app.conn == Conn::Ready) {
		ImGui::Text("%s/%s", app.cfg.owner.c_str(), app.cfg.repo.c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160.0f);

		if (ImGui::BeginCombo("##branch", app.cfg.branch.c_str())) {
			for (auto& b : app.branches) {
				if (ImGui::Selectable(b.c_str(), b == app.cfg.branch) && b != app.cfg.branch) {
					if (anyDirtyTabs()) {
						app.pendingBranch = b; // confirm first
					} else {
						app.cfg.branch = b;
						persistConfig();
						closeAllTabs();
						loadTree();
					}
				}
			}

			ImGui::EndCombo();
		}

		ImGui::SameLine();

		if (ImGui::Button("New file")) {
			app.newFilePath[0] = '\0';
			app.openNewFilePopup = true;
		}

		ImGui::SameLine();

		if (ImGui::Button(app.showSearch ? "Hide search" : "Search")) {
			app.showSearch = !app.showSearch;
		}

		ImGui::SameLine();

		if (ImGui::Button("Refresh")) {
			loadTree();
		}
	} else if (app.conn == Conn::Disconnected) {
		if (ImGui::Button("Connect")) {
			connect();
		}

		if (!app.connError.empty()) {
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", app.connError.c_str());
		}
	} else {
		ImGui::TextUnformatted(app.conn == Conn::Branches ? "loading branches..." : "loading tree...");
	}

	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 50.0f);

	if (ImGui::Button(app.darkTheme ? "Light" : "Dark")) {
		app.darkTheme = !app.darkTheme;

		if (app.darkTheme) { ImGui::StyleColorsDark(); } else { ImGui::StyleColorsLight(); }

		for (auto& t : app.tabs) {
			te_set_palette(t.ed, app.darkTheme ? TE_PALETTE_DARK : TE_PALETTE_LIGHT);
		}
	}
}

static void drawPopups() {
	// ── settings ─────────────────────────────────────────────────────
	if (app.openSettings) {
		ImGui::OpenPopup("Settings");
		app.openSettings = false;
	}

	if (ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::InputText("Owner", app.inOwner, sizeof(app.inOwner));
		ImGui::InputText("Repo", app.inRepo, sizeof(app.inRepo));
		ImGui::InputText("Token", app.inToken, sizeof(app.inToken), ImGuiInputTextFlags_Password);
		ImGui::TextDisabled("fine-grained PAT with Contents read/write;\nstored in this browser's localStorage");

		if (ImGui::Button("Connect", ImVec2(120.0f, 0.0f))) {
			app.cfg.owner = app.inOwner;
			app.cfg.repo = app.inRepo;
			app.cfg.token = app.inToken;
			persistConfig();
			closeAllTabs();
			ImGui::CloseCurrentPopup();
			connect();
		}

		ImGui::SameLine();

		if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	// ── commit message ───────────────────────────────────────────────
	if (app.openSavePopup) {
		ImGui::OpenPopup("Commit");
		app.openSavePopup = false;
	}

	if (ImGui::BeginPopupModal("Commit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (app.savingTab >= 0 && app.savingTab < (int) app.tabs.size()) {
			ImGui::Text("Commit %s to %s", app.tabs[(size_t) app.savingTab].path.c_str(),
				app.cfg.branch.c_str());
			ImGui::SetNextItemWidth(420.0f);

			bool enter = ImGui::InputText("##msg", app.commitMsg, sizeof(app.commitMsg),
				ImGuiInputTextFlags_EnterReturnsTrue);

			if (ImGui::IsWindowAppearing()) {
				ImGui::SetKeyboardFocusHere(-1);
			}

			if (ImGui::Button("Commit", ImVec2(120.0f, 0.0f)) || enter) {
				saveTab(app.savingTab);
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();

			if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
				ImGui::CloseCurrentPopup();
			}
		} else {
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	// ── new file ─────────────────────────────────────────────────────
	if (app.openNewFilePopup) {
		ImGui::OpenPopup("New file");
		app.openNewFilePopup = false;
	}

	if (ImGui::BeginPopupModal("New file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::SetNextItemWidth(420.0f);

		bool enter = ImGui::InputText("path", app.newFilePath, sizeof(app.newFilePath),
			ImGuiInputTextFlags_EnterReturnsTrue);

		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere(-1);
		}

		ImGui::TextDisabled("repo-relative, e.g. docs/notes.md - created on first commit");

		if ((ImGui::Button("Create", ImVec2(120.0f, 0.0f)) || enter) && app.newFilePath[0]) {
			std::string path = app.newFilePath;

			if (findTab(path) < 0) {
				Tab tab;
				tab.path = path;
				auto slash = path.find_last_of('/');
				tab.name = slash == std::string::npos ? path : path.substr(slash + 1);
				tab.isNew = true;
				tab.ed = te_create();
				te_set_palette(tab.ed, app.darkTheme ? TE_PALETTE_DARK : TE_PALETTE_LIGHT);
				te_set_show_line_numbers(tab.ed, 1);
				te_set_language(tab.ed, ghp::languageForPath(path));
				app.tabs.push_back(std::move(tab));
			}

			app.selectTab = findTab(path);
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	// ── branch switch with dirty tabs ────────────────────────────────
	if (!app.pendingBranch.empty() && !ImGui::IsPopupOpen("Switch branch?")) {
		ImGui::OpenPopup("Switch branch?");
	}

	if (ImGui::BeginPopupModal("Switch branch?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("You have unsaved changes. Switching to '%s'\ncloses all tabs and discards them.",
			app.pendingBranch.c_str());

		if (ImGui::Button("Switch anyway", ImVec2(140.0f, 0.0f))) {
			app.cfg.branch = app.pendingBranch;
			app.pendingBranch.clear();
			persistConfig();
			closeAllTabs();
			loadTree();
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Stay", ImVec2(140.0f, 0.0f))) {
			app.pendingBranch.clear();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}

static void drawSidePanel() {
	ImGui::BeginChild("##side", ImVec2(app.treeWidth, 0.0f), ImGuiChildFlags_Borders);

	if (app.showSearch) {
		ImGui::SetNextItemWidth(-60.0f);

		bool enter = ImGui::InputTextWithHint("##q", "search code...", app.searchQuery,
			sizeof(app.searchQuery), ImGuiInputTextFlags_EnterReturnsTrue);

		ImGui::SameLine();

		if ((ImGui::Button("Go") || enter) && !app.searching) {
			runSearch();
		}

		if (app.searching) {
			ImGui::TextDisabled("searching...");
		}

		ImGui::Separator();

		for (int i = 0; i < (int) app.searchResults.size(); i++) {
			const auto& hit = app.searchResults[(size_t) i];
			ImGui::PushID(i);

			if (ImGui::Selectable(hit.path.c_str())) {
				// find the blob sha from the tree so saves work afterwards
				std::string sha;

				for (auto& node : app.tree) {
					if (!node.isDir && node.path == hit.path) { sha = node.sha; break; }
				}

				// first non-empty fragment line is what we scroll to
				std::string needle = hit.fragment;
				auto nl = needle.find('\n');

				while (nl == 0) {
					needle.erase(0, 1);
					nl = needle.find('\n');
				}

				if (nl != std::string::npos) {
					needle.resize(nl);
				}

				app.pendingLocate = needle;
				openFile(hit.path, sha);
			}

			if (!hit.fragment.empty() && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%.300s", hit.fragment.c_str());
			}

			ImGui::PopID();
		}

		ImGui::Separator();
	}

	if (app.conn == Conn::Ready && !app.tree.empty()) {
		if (app.treeTruncated) {
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "tree truncated (repo too large)");
		}

		drawTreeNode(0);
	} else if (app.conn != Conn::Ready) {
		ImGui::TextDisabled(app.conn == Conn::Disconnected
			? "not connected\n\nSettings -> owner/repo/token,\nthen Connect"
			: "loading...");
	}

	ImGui::EndChild();

	// splitter
	ImGui::SameLine(0.0f, 0.0f);
	ImGui::InvisibleButton("##split", ImVec2(6.0f, -1.0f));

	if (ImGui::IsItemActive()) {
		app.treeWidth += ImGui::GetIO().MouseDelta.x;
		app.treeWidth = app.treeWidth < 140.0f ? 140.0f : (app.treeWidth > 600.0f ? 600.0f : app.treeWidth);
	}

	if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
	}

	ImGui::SameLine(0.0f, 0.0f);
}

static void drawEditorArea() {
	ImGui::BeginChild("##main", ImVec2(0.0f, 0.0f));

	float statusH = ImGui::GetFrameHeightWithSpacing();

	if (app.tabs.empty()) {
		ImGui::Dummy(ImVec2(0.0f, 8.0f));
		ImGui::TextDisabled("   open a file from the tree on the left");
	} else if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_Reorderable |
		ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
		for (int i = 0; i < (int) app.tabs.size(); i++) {
			Tab& t = app.tabs[(size_t) i];
			ImGuiTabItemFlags flags = 0;

			if (t.ed && (te_is_dirty(t.ed) || t.isNew)) {
				flags |= ImGuiTabItemFlags_UnsavedDocument;
			}

			if (app.selectTab == i) {
				flags |= ImGuiTabItemFlags_SetSelected;
			}

			ImGui::PushID(t.path.c_str());

			if (ImGui::BeginTabItem(t.name.c_str(), &t.open, flags)) {
				app.activeTab = i;

				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", t.path.c_str());
				}

				te_render(t.ed, "##src", 0.0f, -statusH, 0);
				ImGui::EndTabItem();
			} else if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", t.path.c_str());
			}

			ImGui::PopID();
		}

		ImGui::EndTabBar();
	}

	app.selectTab = -1;

	// process closed tabs (after the loop; destroys editors)
	for (int i = (int) app.tabs.size() - 1; i >= 0; i--) {
		if (!app.tabs[(size_t) i].open) {
			te_destroy(app.tabs[(size_t) i].ed);
			app.tabs.erase(app.tabs.begin() + i);

			if (app.activeTab >= (int) app.tabs.size()) {
				app.activeTab = (int) app.tabs.size() - 1;
			}
		}
	}

	// ── status line ──────────────────────────────────────────────────
	if (app.activeTab >= 0 && app.activeTab < (int) app.tabs.size()) {
		Tab& t = app.tabs[(size_t) app.activeTab];
		int line = 0, column = 0;
		te_get_cursor(t.ed, &line, &column);
		ImGui::Text("Ln %d, Col %d  %d lines  %s", line + 1, column + 1, te_line_count(t.ed),
			t.isNew ? "[new file]" : (te_is_dirty(t.ed) ? "[modified]" : "[committed]"));
		ImGui::SameLine();

		if (ImGui::SmallButton("Commit...")) {
			app.savingTab = app.activeTab;
			app.commitMsg[0] = '\0';
			app.openSavePopup = true;
		}
	} else {
		ImGui::TextUnformatted(" ");
	}

	if (!app.status.empty() && emscripten_get_now() < app.statusUntil) {
		ImGui::SameLine();
		ImGui::TextColored(app.statusIsError ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
			: ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "| %s", app.status.c_str());
	}

	ImGui::EndChild();
}

//
//	frame
//

static void mainLoop() {
	double cssW, cssH;
	emscripten_get_element_css_size("#canvas", &cssW, &cssH);
	int w = (int) cssW, h = (int) cssH;
	int curW, curH;
	glfwGetWindowSize(app.window, &curW, &curH);

	if ((w != curW || h != curH) && w > 0 && h > 0) {
		glfwSetWindowSize(app.window, w, h);
	}

	glfwPollEvents();
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	// Ctrl+S commits the active tab
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && app.activeTab >= 0) {
		app.savingTab = app.activeTab;
		app.commitMsg[0] = '\0';
		app.openSavePopup = true;
	}

	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::Begin("##app", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

	drawTopBar();
	ImGui::Separator();
	drawSidePanel();
	drawEditorArea();
	drawPopups();

	ImGui::End();

	ImGui::Render();
	int fbW, fbH;
	glfwGetFramebufferSize(app.window, &fbW, &fbH);
	glViewport(0, 0, fbW, fbH);
	glClearColor(app.darkTheme ? 0.06f : 0.85f, app.darkTheme ? 0.06f : 0.85f,
		app.darkTheme ? 0.07f : 0.86f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	glfwSwapBuffers(app.window);
}

//
//	entry
//

int main() {
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	app.window = glfwCreateWindow(1280, 720, "ImGui-IDE cloud", nullptr, nullptr);
	glfwMakeContextCurrent(app.window);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui_ImplGlfw_InitForOpenGL(app.window, true);
#ifdef __EMSCRIPTEN__
	ImGui_ImplGlfw_InstallEmscriptenCallbacks(app.window, "#canvas");
#endif
	ImGui_ImplOpenGL3_Init("#version 300 es");

	ImGuiMemAllocFunc allocFn; ImGuiMemFreeFunc freeFn; void* allocUd;
	ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &allocUd);
	te_bind_imgui(ImGui::GetCurrentContext(), allocFn, freeFn, allocUd);

	// restore connection settings; auto-connect if we have a target
	app.cfg.owner = lsGet("ide.owner");
	app.cfg.repo = lsGet("ide.repo");
	app.cfg.branch = lsGet("ide.branch");
	app.cfg.token = lsGet("ide.token");

	if (app.cfg.owner.empty()) {
		app.cfg.owner = "lobotomy-x";
		app.cfg.repo = "ImGuiColorTextEdit";
	}

	if (!app.cfg.owner.empty() && !app.cfg.repo.empty()) {
		connect();
	}

	emscripten_set_main_loop(mainLoop, 0, true);
	return 0;
}
