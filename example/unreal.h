//	Unreal Engine project support — Blueprint-style C++ code generation.
//
//	Pure logic (no ImGui/SDL): detect a .uproject, and generate UE-idiomatic
//	class scaffolding (UCLASS/UPROPERTY/UFUNCTION boilerplate) the way the
//	engine's own "New C++ Class" wizard does — Actor/Character/Component/etc.
//	header+source pairs, Blueprint-callable function stubs, and Blueprint-exposed
//	property declarations. The editor wraps this in a wizard; keeping the
//	generation itself string-pure makes it testable in the headless selftest.
//
//	Copyright (c) 2026 Logan Brunet (ImGui-IDE). All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.

#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace unreal {

// Class templates offered by the wizard — mirrors UE5's "New C++ Class" dialog.
enum class ClassKind {
	Actor,
	Character,
	Pawn,
	ActorComponent,
	SceneComponent,
	Object,
	Interface,
	BlueprintFunctionLibrary,
};

// Everything the generator needs for one class. `name` is the bare name
// ("Health") — the UE prefix (A/U/I) is added per kind. `moduleName` feeds the
// MODULE_API export macro; empty emits no API macro (header-only prototyping).
struct ClassSpec {
	ClassKind   kind = ClassKind::Actor;
	std::string name;
	std::string moduleName;
	bool        blueprintable = true;   // UCLASS(Blueprintable, BlueprintType)
	bool        tick = false;           // override Tick / TickComponent
};

const char* kindLabel(ClassKind kind);            // "Actor", "Actor Component", …
std::string prefixedClassName(const ClassSpec&);  // AHealth / UHealthComponent / IHealth
std::string baseClassFor(ClassKind kind);         // "AActor", "UActorComponent", …
std::string headerFileName(const ClassSpec&);     // "Health.h"
std::string sourceFileName(const ClassSpec&);     // "Health.cpp"
std::string moduleApiMacro(const std::string& moduleName);   // "MyGame" -> "MYGAME_API"

// Full file contents, UE-style: CoreMinimal + base-class include + the
// <Name>.generated.h include last, UCLASS specifiers per kind, GENERATED_BODY,
// constructor/BeginPlay/Tick overrides where the engine template has them.
std::string generateHeader(const ClassSpec&);
std::string generateSource(const ClassSpec&);

// ── Blueprint function stubs ─────────────────────────────────────────────────
struct FunctionSpec {
	std::string name;
	std::string returnType = "void";
	std::vector<std::pair<std::string, std::string>> params;   // (type, name)
	bool        pure = false;           // BlueprintPure instead of BlueprintCallable
	std::string category = "Default";
};

// Parse a comma-separated parameter list ("float Amount, const FString& Who")
// into (type, name) pairs — the last whitespace-separated token is the name,
// everything before it the type. Empty input → empty list.
std::vector<std::pair<std::string, std::string>> parseParamList(const std::string& text);

std::string generateFunctionDecl(const FunctionSpec&);   // UFUNCTION(...) + signature, for the .h
std::string generateFunctionDef(const FunctionSpec&, const std::string& className);  // for the .cpp

// ── Blueprint-exposed properties ─────────────────────────────────────────────
struct PropertySpec {
	std::string type = "float";
	std::string name;
	std::string category = "Default";
	bool        readOnly = false;       // BlueprintReadOnly instead of ReadWrite
	bool        editAnywhere = true;    // EditAnywhere vs VisibleAnywhere
};

std::string generatePropertyDecl(const PropertySpec&);   // UPROPERTY(...) + member

// ── Project detection ────────────────────────────────────────────────────────
// Look for a *.uproject directly in `root` (the project root the editor has
// open). Returns true and sets `out` when found; picks the first alphabetically
// if several exist.
bool findUProject(const std::filesystem::path& root, std::filesystem::path& out);

// Default folder for new classes: Source/<Module>/ when it exists under root,
// else the root itself. Returned relative to root.
std::filesystem::path defaultClassDir(const std::filesystem::path& root, const std::string& moduleName);

} // namespace unreal
