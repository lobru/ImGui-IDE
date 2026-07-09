//
//  unreal_plugin.cpp — UnrealPlugin: wires the pure UE helpers (unreal.h) into
//  the editor through the plugin hooks. Talks to the editor only via PluginHost.
//

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

#include <imgui.h>

#include "unreal.h"
#include "unreal_plugin.h"

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
