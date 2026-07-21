//
//  build_picker.cpp — graphical build/run target discovery + picker.
//
//  Multilevel projects rarely have ONE obvious build entry point; the old
//  first-match walk picked something arbitrary or gave up with a message. This
//  enumerates every plausible target across the project tree and lets the user
//  build/run any of them with one click — or pin one as the project's F6 (build)
//  / F5 (run) default, persisted in settings ([build] / [run]). runProjectBuild
//  opens this picker with the candidates when it can't resolve on its own.
//

#include "editor.h"

#include <algorithm>
#include <cctype>

#include "imgui.h"

std::vector<Editor::BuildTarget> Editor::discoverBuildTargets() const
{
    std::vector<BuildTarget> out;
    if (projectRoot.empty())
        return out;

    std::error_code ec;
    auto addTarget = [&](std::string label, std::string cmd, std::filesystem::path dir, bool runnable) {
        for (auto &t : out)
            if (t.command == cmd && t.dir == dir)
                return; // discovered twice via different walks
        out.push_back({std::move(label), std::move(cmd), std::move(dir), runnable});
    };

    // Inspect one directory for every target type we understand.
    auto scanDir = [&](const std::filesystem::path &dir) {
        auto rel = std::filesystem::relative(dir, projectRoot, ec).string();
        if (rel == ".")
            rel.clear();
        auto tag = [&](const std::string &what) {
            return rel.empty() ? what : (what + "  [" + rel + "]");
        };

        // Build scripts.
#ifdef _WIN32
        for (const char *s : {"build.bat", "build.cmd"})
            if (std::filesystem::is_regular_file(dir / s, ec))
                addTarget(tag(s), "\"" + (dir / s).string() + "\"", dir, false);
        if (std::filesystem::is_regular_file(dir / "build.ps1", ec))
            addTarget(tag("build.ps1"),
                      "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + (dir / "build.ps1").string() + "\"",
                      dir, false);
#else
        if (std::filesystem::is_regular_file(dir / "build.sh", ec))
            addTarget(tag("build.sh"), "bash \"" + (dir / "build.sh").string() + "\"", dir, false);
#endif
        if (std::filesystem::is_regular_file(dir / "Makefile", ec))
            addTarget(tag("make"), "make", dir, false);

        // A project-type plugin (e.g. Unreal) claiming this dir.
        if (auto pc = const_cast<PluginRegistry &>(pluginRegistry).buildCommand(dir))
        {
            std::filesystem::path rd = pc->path;
            std::string cmd = pc->command;
            if (std::filesystem::is_regular_file(rd, ec))
            {
                cmd = cmd.empty() ? ("\"" + rd.string() + "\"") : (cmd + " \"" + rd.string() + "\"");
                rd = rd.parent_path();
            }
            addTarget(tag("Project plugin build"), cmd, rd, false);
        }

        // Solution / project files — EVERY one, not just the first.
        for (auto it = std::filesystem::directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file(ec))
                continue;
            auto p = it->path();
            auto ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char) std::tolower(c); });
            auto leaf = p.filename().string();
            if (ext == ".sln")
                addTarget(tag("dotnet build " + leaf), "dotnet build \"" + p.string() + "\"", dir, false);
            else if (ext == ".csproj")
            {
                addTarget(tag("dotnet build " + leaf), "dotnet build \"" + p.string() + "\"", dir, false);
                addTarget(tag("dotnet run " + leaf), "dotnet run --project \"" + p.string() + "\"", dir, true);
            }
            else if (ext == ".vcxproj")
                addTarget(tag("msbuild " + leaf), "msbuild \"" + p.string() + "\"", dir, false);
        }

        // CMake: prefer an existing build dir; offer configure+build otherwise.
        if (std::filesystem::is_regular_file(dir / "CMakeLists.txt", ec))
        {
            bool haveBuildDir = false;
            for (const char *sub : {"build", "out/build/x64-Debug", "out/build/x64-Release", "out/build"})
            {
                auto bd = dir / sub;
                if (std::filesystem::is_regular_file(bd / "CMakeCache.txt", ec))
                {
                    addTarget(tag(std::string("cmake --build ") + sub), "cmake --build . --config %CONFIG%", bd, false);
                    haveBuildDir = true;
                }
            }
            if (!haveBuildDir)
                addTarget(tag("cmake configure + build"), "cmake -B build && cmake --build build", dir, false);
        }

        // npm / cargo.
        if (std::filesystem::is_regular_file(dir / "package.json", ec))
        {
            addTarget(tag("npm run build"), "npm run build", dir, false);
            addTarget(tag("npm start"), "npm start", dir, true);
        }
        if (std::filesystem::is_regular_file(dir / "Cargo.toml", ec))
        {
            addTarget(tag("cargo build"), "cargo build", dir, false);
            addTarget(tag("cargo run"), "cargo run", dir, true);
        }
    };

    // Root + nested dirs (depth <= 3, budgeted, junk trees pruned) — multilevel
    // repos keep their real projects a level or two down.
    scanDir(projectRoot);
    int budget = 4000;
    for (auto it = std::filesystem::recursive_directory_iterator(
             projectRoot, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (--budget < 0)
            break;
        if (!it->is_directory(ec))
            continue;
        auto name = it->path().filename().string();
        bool junk = name == ".git" || name == "node_modules" || name == "build" || name == "out" ||
                    name == "bin" || name == "obj" || name == ".vs" || name == ".vscode" ||
                    name == "Intermediate" || name == "Binaries" || name == "DerivedDataCache" ||
                    name == "Saved" || name == "deps" || name == "vendor" || name == "target";
        if (junk || it.depth() >= 3)
        {
            it.disable_recursion_pending();
            if (junk)
                continue;
        }
        scanDir(it->path());
    }

    // The freshest built exe is the classic F5 target.
    if (auto exe = findBuiltExe(); !exe.empty())
        addTarget("Run built exe: " + exe.filename().string(),
                  "\"" + exe.string() + "\"", exe.parent_path(), true);

    return out;
}

void Editor::openBuildPicker()
{
    buildPickerTargets = discoverBuildTargets();
    buildPickerVisible = true;
    buildPickerCustom[0] = 0;
}

void Editor::renderBuildPicker()
{
    if (!buildPickerVisible)
        return;
    ImGui::SetNextWindowSize(ImVec2(760.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Build / Run Targets###buildPicker", &buildPickerVisible))
    {
        if (projectRoot.empty())
        {
            ImGui::TextWrapped("No project open. Open a project folder first.");
            ImGui::End();
            return;
        }
        std::string key = dbgProjectKey();
        ImGui::TextDisabled("%s", projectRoot.string().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Rescan"))
            buildPickerTargets = discoverBuildTargets();

        // The active project — what the status bar shows next to the repo name.
        {
            auto a = projectActiveName.find(key);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Active project: %s",
                        a != projectActiveName.end() ? a->second.c_str() : "(none — pick one below)");
            if (a != projectActiveName.end())
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("clear##active"))
                {
                    projectActiveName.erase(key);
                    saveSettings();
                }
            }
        }

        // Current pins.
        {
            auto b = projectBuildOverrides.find(key);
            auto r = projectRunOverrides.find(key);
            ImGui::TextDisabled("F6 build: %s", b != projectBuildOverrides.end() ? b->second.c_str() : "(auto)");
            ImGui::SameLine();
            if (b != projectBuildOverrides.end() && ImGui::SmallButton("clear##b"))
            {
                projectBuildOverrides.erase(key);
                saveSettings();
            }
            ImGui::TextDisabled("F5 run:   %s", r != projectRunOverrides.end() ? r->second.c_str() : "(auto)");
            ImGui::SameLine();
            if (r != projectRunOverrides.end() && ImGui::SmallButton("clear##r"))
            {
                projectRunOverrides.erase(key);
                saveSettings();
            }
        }
        ImGui::Separator();

        ImGui::BeginChild("##targets", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f));
        if (buildPickerTargets.empty())
            ImGui::TextDisabled("Nothing discovered — use a custom command below.");
        for (size_t i = 0; i < buildPickerTargets.size(); ++i)
        {
            auto &t = buildPickerTargets[i];
            ImGui::PushID((int) i);
            if (ImGui::Button(t.runnable ? "Run" : "Build"))
            {
                runCommandInOutputPanel(t.command, t.dir);
                buildPickerVisible = false;
            }
            ImGui::SameLine();
            // One click makes this THE project: pins it (F6 for build rows, F5
            // for runnable rows) and names it on the status bar.
            if (ImGui::SmallButton("Set Active"))
            {
                projectActiveName[key] = t.label;
                if (t.runnable)
                    projectRunOverrides[key] = t.command + "|" + t.dir.string();
                else
                    projectBuildOverrides[key] = t.command + "|" + t.dir.string();
                saveSettings();
                pushToast("Active project: " + t.label, IM_COL32(120, 200, 120, 255));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Set F6"))
            {
                projectBuildOverrides[key] = t.command + "|" + t.dir.string();
                saveSettings();
                pushToast("F6 now builds: " + t.label, IM_COL32(120, 200, 120, 255));
            }
            if (t.runnable)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Set F5"))
                {
                    projectRunOverrides[key] = t.command + "|" + t.dir.string();
                    saveSettings();
                    pushToast("F5 now runs: " + t.label, IM_COL32(120, 200, 120, 255));
                }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(t.label.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\nin %s", t.command.c_str(), t.dir.string().c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();

        // Custom command escape hatch — still available, never required.
        ImGui::SetNextItemWidth(-260.0f);
        ImGui::InputTextWithHint("##custom", "custom command (runs in the project root)…",
                                 buildPickerCustom, sizeof(buildPickerCustom));
        ImGui::SameLine();
        bool haveCustom = buildPickerCustom[0] != 0;
        if (ImGui::Button("Run##custom") && haveCustom)
            runCommandInOutputPanel(buildPickerCustom, projectRoot);
        ImGui::SameLine();
        if (ImGui::Button("Set F6##custom") && haveCustom)
        {
            projectBuildOverrides[key] = std::string(buildPickerCustom) + "|" + projectRoot.string();
            saveSettings();
        }
        ImGui::SameLine();
        if (ImGui::Button("Set F5##custom") && haveCustom)
        {
            projectRunOverrides[key] = std::string(buildPickerCustom) + "|" + projectRoot.string();
            saveSettings();
        }
    }
    ImGui::End();
}
