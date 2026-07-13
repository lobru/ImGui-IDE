//
//  unreal_plugin.cpp — UnrealPlugin: wires the pure UE helpers (unreal.h) into
//  the editor through the plugin hooks. Talks to the editor only via PluginHost.
//

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include <imgui.h>

#include "unreal.h"
#include "unreal_codegen.h"
#include "unreal_plugin.h"
#include "unreal_uasset.h"

// Unreal / COM-style type-name convention: a prefix in {F,U,A,T,E,I,S} followed
// by an uppercase letter, and CamelCase (has a lowercase char — so SCREAMING_CASE
// macros like FILE/TRUE aren't miscolored). Covers the thousands of UE types that
// can't be enumerated (FMyStruct, UMyComponent, AMyActor, TMyTemplate, EMyEnum).
static bool unrealTypeLike(const std::string &id)
{
    if (id.size() < 3)
        return false;
    char c0 = id[0], c1 = id[1];
    if (c0 != 'F' && c0 != 'U' && c0 != 'A' && c0 != 'T' && c0 != 'E' && c0 != 'I' && c0 != 'S')
        return false;
    if (c1 < 'A' || c1 > 'Z')
        return false;
    for (char c : id)
        if (c >= 'a' && c <= 'z')
            return true; // CamelCase → a type, not an all-caps macro
    return false;
}

std::unique_ptr<EditorPlugin> createUnrealPlugin()
{
    return std::make_unique<UnrealPlugin>();
}

void UnrealPlugin::onRegister(PluginHost &host)
{
    // Augment the shared C++ language once with Unreal's vocabulary. Harmless in
    // non-UE C++ (these names simply don't appear there). Applied through the host
    // so this works identically whether the plugin is compiled in or DLL-loaded.

    // UE fundamental types, containers, smart pointers, math + string types —
    // colored as types (declarations). Aliases like int32/FString/TArray that
    // the base C++ set doesn't know, so "int32 X;" used to go uncolored.
    static const char *const ueTypes[] = {
        "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64",
        "TCHAR", "ANSICHAR", "WIDECHAR", "UTF8CHAR", "UCS2CHAR", "CHAR8", "CHAR16", "CHAR32",
        "SIZE_T", "SSIZE_T", "PTRINT", "UPTRINT",
        "FString", "FName", "FText", "FStringView", "FAnsiStringView",
        "TArray", "TArrayView", "TMap", "TMultiMap", "TSet", "TQueue", "TStaticArray",
        "TSharedPtr", "TSharedRef", "TWeakPtr", "TUniquePtr", "TWeakObjectPtr", "TStrongObjectPtr",
        "TObjectPtr", "TSoftObjectPtr", "TSoftClassPtr", "TSubclassOf", "TScriptInterface",
        "TOptional", "TFunction", "TUniqueFunction", "TTuple", "TPair", "TVariant", "TEnumAsByte",
        "FVector", "FVector2D", "FVector4", "FRotator", "FQuat", "FTransform", "FMatrix",
        "FColor", "FLinearColor", "FIntPoint", "FIntVector", "FBox", "FBox2D", "FGuid",
        "FDateTime", "FTimespan", "FDelegateHandle"};
    // UE reflection + logging + assertion macros — colored as keywords.
    static const char *const ueMacros[] = {
        "UCLASS", "UPROPERTY", "UFUNCTION", "USTRUCT", "UENUM", "UINTERFACE", "UDELEGATE", "UPARAM",
        "UMETA", "GENERATED_BODY", "GENERATED_UCLASS_BODY", "GENERATED_USTRUCT_BODY",
        "DECLARE_DYNAMIC_MULTICAST_DELEGATE", "DECLARE_MULTICAST_DELEGATE", "DECLARE_DELEGATE",
        "UE_LOG", "UE_LOGFMT", "UE_CLOG", "checkf", "checkNoEntry", "checkNoReentry", "checkSlow",
        "ensureMsgf", "ensureAlways", "verifyf", "unimplemented",
        "FORCEINLINE", "FORCENOINLINE", "UE_DEPRECATED", "UE_NODISCARD",
        "LOCTEXT", "NSLOCTEXT", "TEXT", "INVTEXT", "PURE_VIRTUAL"};

    host.hostAugmentCppLanguage(
        std::vector<std::string>(std::begin(ueTypes), std::end(ueTypes)),   // → declarations
        std::vector<std::string>(std::begin(ueMacros), std::end(ueMacros)), // → keywords
        unrealTypeLike); // catch unlisted F*/U*/A*/T*/E*/I*/S* types
}

void UnrealPlugin::onFrame(PluginHost &host)
{
    // The wizards are modals: a menu click sets the request, the popup opens here
    // (OpenPopup and BeginPopupModal must share an ID scope, and menu items close
    // their own popup stack the frame they're clicked).
    if (requestClassWizard)
    {
        requestClassWizard = false;
        ImGui::OpenPopup("New UE C++ Class###ueClassWizard");
    }
    if (requestVerseWizard)
    {
        requestVerseWizard = false;
        ImGui::OpenPopup("New Verse Device###ueVerseWizard");
    }
    renderClassWizard(host);
    renderVerseWizard(host);
}

void UnrealPlugin::renderClassWizard(PluginHost &host)
{
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("New UE C++ Class###ueClassWizard", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    auto &parents = unreal::codegen::parentClasses();
    ImGui::TextDisabled("Generates the same boilerplate as the UE editor's class wizard.");
    ImGui::Separator();

    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("Name", "MyActor (no A/U prefix)", classNameBuf, sizeof(classNameBuf));

    if (ImGui::BeginCombo("Parent", parents[static_cast<size_t>(classParentIdx)].name))
    {
        for (int i = 0; i < static_cast<int>(parents.size()); i++)
        {
            if (ImGui::Selectable(parents[static_cast<size_t>(i)].name, i == classParentIdx))
                classParentIdx = i;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", parents[static_cast<size_t>(i)].blurb);
        }
        ImGui::EndCombo();
    }

    std::string modulePreview = classModules.empty()
                                    ? std::string("(no Source/ modules found)")
                                    : classModules[static_cast<size_t>(classModuleIdx)].filename().string();
    if (ImGui::BeginCombo("Module", modulePreview.c_str()))
    {
        for (int i = 0; i < static_cast<int>(classModules.size()); i++)
        {
            std::string label = classModules[static_cast<size_t>(i)].filename().string();
            if (ImGui::Selectable(label.c_str(), i == classModuleIdx))
                classModuleIdx = i;
        }
        ImGui::EndCombo();
    }

    // keep the name a valid bare identifier
    std::string name;
    for (char c : std::string(classNameBuf))
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            name += c;
    bool nameOk = !name.empty() && !std::isdigit(static_cast<unsigned char>(name[0]));

    ImGui::Separator();
    if (!nameOk || classModules.empty())
        ImGui::BeginDisabled();
    if (ImGui::Button("Create"))
    {
        auto moduleDir = classModules[static_cast<size_t>(classModuleIdx)];
        unreal::codegen::ClassSpec spec{name, parents[static_cast<size_t>(classParentIdx)].name,
                                        moduleDir.filename().string()};
        auto [headerPath, sourcePath] = unreal::codegen::classFilePaths(moduleDir, name);
        std::error_code ec;
        if (std::filesystem::exists(headerPath, ec) || std::filesystem::exists(sourcePath, ec))
            host.hostError(name + ".h/.cpp already exists in " + moduleDir.filename().string());
        else
        {
            std::ofstream h(headerPath, std::ios::binary), c(sourcePath, std::ios::binary);
            if (!h || !c)
                host.hostError("Could not write into " + moduleDir.string());
            else
            {
                h << unreal::codegen::generateClassHeader(spec);
                c << unreal::codegen::generateClassSource(spec);
                h.close();
                c.close();
                host.hostOpenFile(sourcePath.string());
                host.hostOpenFile(headerPath.string());
                host.hostToast("Created " + unreal::codegen::prefixedClassName(spec) +
                               " \xe2\x80\x94 rebuild + regenerate the compile DB to index it");
                classNameBuf[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
        }
    }
    if (!nameOk || classModules.empty())
        ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void UnrealPlugin::renderVerseWizard(PluginHost &host)
{
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("New Verse Device###ueVerseWizard", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextDisabled("Scaffolds a UEFN creative_device class (.verse).");
    ImGui::Separator();
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("Device", "my_device", verseNameBuf, sizeof(verseNameBuf));

    std::string name = unreal::codegen::sanitizeVerseName(verseNameBuf);
    ImGui::TextDisabled("-> %s.verse", name.c_str());

    ImGui::Separator();
    if (ImGui::Button("Create"))
    {
        std::error_code ec;
        std::filesystem::path dir = menuProj.parent_path();
        if (std::filesystem::is_directory(dir / "Content", ec))
            dir /= "Content";
        std::filesystem::path path = dir / (name + ".verse");
        if (std::filesystem::exists(path, ec))
            host.hostError(path.filename().string() + " already exists");
        else
        {
            std::ofstream f(path, std::ios::binary);
            if (!f)
                host.hostError("Could not write " + path.string());
            else
            {
                f << unreal::codegen::generateVerseDevice(name);
                f.close();
                host.hostOpenFile(path.string());
                host.hostToast("Created " + path.filename().string());
                verseNameBuf[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void UnrealPlugin::inspectUAssetPath(PluginHost &host, const std::filesystem::path &asset)
{
    auto summary = unreal::uasset::parseFile(asset);
    // Emit JSON and open it with a .json extension: the editor colours + folds it,
    // gives smart navigation, and the tab is a ready-to-Save-As export.
    std::string text = unreal::uasset::toJson(summary, asset.filename().string());

    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "ImGuiIDE";
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path out = dir / (asset.stem().string() + ".uasset.json");
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (f)
    {
        f << text;
        f.close();
        host.hostOpenFile(out.string());
    }
    else
        host.hostError("Could not write the report to " + out.string());
}

void UnrealPlugin::inspectActiveUAsset(PluginHost &host)
{
    inspectUAssetPath(host, host.hostActiveFilename());
}

bool UnrealPlugin::openFile(PluginHost &host, const std::filesystem::path &path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".uasset" && ext != ".umap")
        return false;
    inspectUAssetPath(host, path); // report opens in a tab; no external app
    return true;
}

void UnrealPlugin::onMenu(PluginHost &host, PluginMenu which)
{
    if (which != PluginMenu::Project)
        return;

    std::filesystem::path projectRoot = host.hostProjectRoot();
    if (menuCachedRoot != projectRoot)
    {
        menuCachedRoot = projectRoot;
        menuAssoc.clear();
        menuProj = projectRoot.empty() ? std::filesystem::path{}
                                       : unreal::findUProject(projectRoot);
        menuEngine = menuProj.empty() ? std::filesystem::path{}
                                      : unreal::findEngineRoot(menuProj, menuAssoc);
    }
    if (menuProj.empty() || !ImGui::BeginMenu("Unreal Engine"))
        return;

    ImGui::TextDisabled("%s  \xc2\xb7  UE %s%s", menuProj.filename().string().c_str(),
                        menuAssoc.empty() ? "(source build)" : menuAssoc.c_str(),
                        menuEngine.empty() ? "  \xe2\x80\x94 engine NOT found" : "");
    ImGui::Separator();
    bool haveEngine = !menuEngine.empty();
    bool cpp = unreal::hasCppSource(menuProj);
    if (ImGui::MenuItem("Build Editor Target", "F6", false, haveEngine && cpp))
        host.hostRunProjectBuild(); // the build resolver picks the UBT command
    if (ImGui::MenuItem("Generate IntelliSense DB (clangd)", nullptr, false, haveEngine && cpp))
    {
        // compile_commands.json lands in the project root; restart C/C++
        // IntelliSense afterwards to pick it up.
        host.hostRunInDir(unreal::generateClangDbCommand(menuEngine, menuProj), menuProj.parent_path());
        host.hostToast("Generating UE compile database \xe2\x80\x94 toggle C/C++ IntelliSense off/on when it finishes");
    }
    if (!cpp)
        ImGui::TextDisabled("(Blueprint-only project \xe2\x80\x94 no C++ Source/)");
    ImGui::Separator();
    if (ImGui::MenuItem("New C++ Class...", nullptr, false, cpp))
    {
        classModules = unreal::codegen::moduleDirs(menuProj);
        classModuleIdx = 0;
        requestClassWizard = true; // the modal opens in onFrame
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Generate a UCLASS header/source pair into a Source/ module (like the UE class wizard)");
    if (ImGui::MenuItem("New Verse Device..."))
        requestVerseWizard = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scaffold a UEFN creative_device .verse file");
    {
        std::filesystem::path active = host.hostActiveFilename();
        std::string ext = active.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        bool isPackage = ext == ".uasset" || ext == ".umap";
        if (ImGui::MenuItem("Inspect Package / Blueprint (.uasset)", nullptr, false, isPackage))
            inspectActiveUAsset(host);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(isPackage ? "Report the package header + import map; detects Blueprints (generated class, parent, referenced classes)"
                                        : "Open a .uasset/.umap file first (its tab must be active)");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Launch Unreal Editor", nullptr, false,
                        haveEngine && !unreal::editorBinary(menuEngine).empty()))
    {
        host.hostRunInDir("\"" + unreal::editorBinary(menuEngine).string() + "\" \"" + menuProj.string() + "\"",
                          menuProj.parent_path());
    }
    if (ImGui::MenuItem("Open .uproject"))
        host.hostOpenFile(menuProj.string());
    ImGui::Separator();
    if (ImGui::MenuItem("Install IDE plugin into project"))
    {
        // Copy the bundled source-code-accessor plugin into the game's Plugins/ —
        // UE compiles it on next editor start, then ImGui-IDE is selectable in
        // Editor Prefs > Source Code.
        std::error_code cec;
        std::filesystem::path src = host.hostExeDir() / "ue-plugins" / "ImGuiIDESourceCodeAccess";
        if (!std::filesystem::is_directory(src, cec))
        {
            auto self = host.hostRepoRoot(); // dev tree fallback
            if (!self.empty())
                src = self / "tools" / "ue-plugins" / "ImGuiIDESourceCodeAccess";
        }
        auto dst = menuProj.parent_path() / "Plugins" / "ImGuiIDESourceCodeAccess";
        if (!std::filesystem::is_directory(src, cec))
            host.hostError("Bundled UE plugin not found next to the executable (ue-plugins/).");
        else
        {
            std::filesystem::create_directories(dst, cec);
            std::filesystem::copy(src, dst,
                                  std::filesystem::copy_options::recursive |
                                      std::filesystem::copy_options::overwrite_existing,
                                  cec);
            if (cec)
                host.hostError("Plugin copy failed: " + cec.message());
            else
                host.hostToast("UE plugin installed \xe2\x80\x94 restart Unreal Editor, then pick "
                               "ImGui-IDE under Editor Prefs > Source Code");
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Copies the ImGuiIDESourceCodeAccess plugin into %s",
                          (menuProj.parent_path() / "Plugins").string().c_str());
    ImGui::EndMenu();
}

void UnrealPlugin::contributeAutocomplete(PluginHost &, const PluginDocInfo &doc,
                                          const std::function<void(const std::string &)> &addWord)
{
    // .uproject / .uplugin descriptors: complete the UE schema (keys, module
    // Type / LoadingPhase values) plus DISCOVERED plugin + module names from the
    // project and its engine, so dependency/plugin fields complete against what
    // actually exists.
    if (doc.extLower != ".uproject" && doc.extLower != ".uplugin")
        return;
    auto docDir = std::filesystem::path(doc.filename).parent_path();
    auto uproj = unreal::findUProject(docDir);
    std::filesystem::path engine;
    std::string assoc;
    if (!uproj.empty())
        engine = unreal::findEngineRoot(uproj, assoc);
    auto projDir = uproj.empty() ? docDir : uproj.parent_path();
    for (const auto &word : unreal::descriptorWords(engine, projDir))
        addWord(word);
}

std::optional<PluginBuildCommand> UnrealPlugin::projectBuildCommand(const std::filesystem::path &startDir)
{
    // A .uproject directly in startDir claims this level: UE projects also carry
    // a GENERATED .sln (C++, which `dotnet build` can't build), so this must take
    // precedence. Build the editor target through UnrealBuildTool — the same
    // invocation the VS project generator emits.
    std::error_code ec;
    std::filesystem::path uproject;
    for (auto &entry : std::filesystem::directory_iterator(startDir, ec))
    {
        std::error_code fec;
        if (!entry.is_regular_file(fec) || fec)
            continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (ext == ".uproject")
        {
            uproject = entry.path();
            break;
        }
    }
    if (uproject.empty())
        return std::nullopt;

    std::string assoc;
    auto engine = unreal::findEngineRoot(uproject, assoc);
    std::string ucmd = engine.empty() ? std::string()
                                      : unreal::buildEditorCommand(engine, uproject);
    if (ucmd.empty())
        ucmd = "echo Unreal build unavailable: " +
               std::string(engine.empty() ? "engine not found (EngineAssociation=" + assoc + ")"
                                          : "Blueprint-only project (no Source/ directory)");
    return PluginBuildCommand{uproject.parent_path(), ucmd}; // directory form → runner cd's there
}

std::optional<std::filesystem::path> UnrealPlugin::resolveInclude(const std::filesystem::path &docDir,
                                                                  const std::string &include)
{
    auto uproj = unreal::findUProject(docDir);
    if (uproj.empty())
        return std::nullopt;
    std::string assoc;
    auto engine = unreal::findEngineRoot(uproj, assoc);
    auto hit = unreal::resolveInclude(engine, uproj, include);
    if (hit.empty())
        return std::nullopt;
    return hit;
}

std::optional<PluginSourceRoot> UnrealPlugin::extraSourceRoot(const std::filesystem::path &projectRoot)
{
    auto uproj = unreal::findUProject(projectRoot);
    if (uproj.empty())
        return std::nullopt;
    std::string assoc;
    auto engine = unreal::findEngineRoot(uproj, assoc);
    if (engine.empty())
        return std::nullopt;
    return PluginSourceRoot{"Unreal Engine Source", engine / "Engine" / "Source"};
}
