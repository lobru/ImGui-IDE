//	Unreal Engine project support — implementation.
//
//	Copyright (c) 2026 Logan Brunet (ImGui-IDE). All rights reserved.
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.

#include "unreal.h"

#include <algorithm>
#include <cctype>

namespace unreal {

namespace {

// Per-kind template facts: prefix, base class, base-class include, and which
// engine-template overrides the generated pair carries.
struct KindInfo {
	const char* label;
	const char* prefix;        // A / U / I
	const char* base;          // AActor, UActorComponent, …
	const char* include;       // engine header for the base class
	bool        hasBeginPlay;
	bool        canTick;       // Tick (actors) or TickComponent (components)
	bool        isComponent;   // TickComponent + PrimaryComponentTick spelling
};

const KindInfo& infoFor(ClassKind kind)
{
	static const KindInfo table[] = {
		// label                        pre  base                        include                                  begin  tick   comp
		{"Actor",                       "A", "AActor",                    "GameFramework/Actor.h",                 true,  true,  false},
		{"Character",                   "A", "ACharacter",                "GameFramework/Character.h",             true,  true,  false},
		{"Pawn",                        "A", "APawn",                     "GameFramework/Pawn.h",                  true,  true,  false},
		{"Actor Component",             "U", "UActorComponent",           "Components/ActorComponent.h",           true,  true,  true},
		{"Scene Component",             "U", "USceneComponent",           "Components/SceneComponent.h",           true,  true,  true},
		{"Object",                      "U", "UObject",                   "UObject/Object.h",                      false, false, false},
		{"Interface",                   "I", "UInterface",                "UObject/Interface.h",                   false, false, false},
		{"Blueprint Function Library",  "U", "UBlueprintFunctionLibrary", "Kismet/BlueprintFunctionLibrary.h",     false, false, false},
	};
	return table[(size_t) kind];
}

std::string trim(const std::string& s)
{
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return {};
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

std::string joinParams(const FunctionSpec& fn)
{
	std::string out;
	for (size_t i = 0; i < fn.params.size(); ++i)
	{
		if (i)
			out += ", ";
		out += fn.params[i].first + " " + fn.params[i].second;
	}
	return out;
}

} // namespace

const char* kindLabel(ClassKind kind)
{
	return infoFor(kind).label;
}

std::string prefixedClassName(const ClassSpec& spec)
{
	return std::string(infoFor(spec.kind).prefix) + spec.name;
}

std::string baseClassFor(ClassKind kind)
{
	return infoFor(kind).base;
}

std::string headerFileName(const ClassSpec& spec)
{
	return spec.name + ".h";
}

std::string sourceFileName(const ClassSpec& spec)
{
	return spec.name + ".cpp";
}

std::string moduleApiMacro(const std::string& moduleName)
{
	if (moduleName.empty())
		return {};
	std::string up = moduleName;
	std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return (char) std::toupper(c); });
	// Non-identifier characters (spaces, dashes) can't appear in a macro name.
	for (auto& c : up)
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
			c = '_';
	return up + "_API";
}

std::string generateHeader(const ClassSpec& spec)
{
	const KindInfo& k = infoFor(spec.kind);
	std::string cls = prefixedClassName(spec);
	std::string api = moduleApiMacro(spec.moduleName);
	std::string apiSp = api.empty() ? "" : api + " ";

	std::string s;
	s += "#pragma once\n\n";
	s += "#include \"CoreMinimal.h\"\n";
	s += "#include \"" + std::string(k.include) + "\"\n";
	s += "#include \"" + spec.name + ".generated.h\"\n\n";

	if (spec.kind == ClassKind::Interface)
	{
		// UE interfaces are a pair: a UINTERFACE shell UObject + the pure I-class
		// implementers actually inherit.
		s += "UINTERFACE(MinimalAPI" + std::string(spec.blueprintable ? ", Blueprintable" : "") + ")\n";
		s += "class U" + spec.name + " : public UInterface\n";
		s += "{\n\tGENERATED_BODY()\n};\n\n";
		s += "class " + apiSp + "I" + spec.name + "\n";
		s += "{\n";
		s += "\tGENERATED_BODY()\n\n";
		s += "public:\n";
		s += "\t// Add interface functions here. BlueprintNativeEvent functions can be\n";
		s += "\t// implemented in both C++ and Blueprint:\n";
		s += "\t// UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = \"" + spec.name + "\")\n";
		s += "\t// void OnInteract(AActor* Caller);\n";
		s += "};\n";
		return s;
	}

	// UCLASS specifiers: components advertise themselves to the Add Component
	// menu; other kinds take the Blueprintable pair when requested.
	if (k.isComponent)
		s += "UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))\n";
	else if (spec.blueprintable)
		s += "UCLASS(Blueprintable, BlueprintType)\n";
	else
		s += "UCLASS()\n";

	s += "class " + apiSp + cls + " : public " + std::string(k.base) + "\n";
	s += "{\n";
	s += "\tGENERATED_BODY()\n\n";
	s += "public:\n";
	s += "\t" + cls + "();\n";

	if (spec.kind == ClassKind::Character || spec.kind == ClassKind::Pawn)
	{
		s += "\n\t// Called to bind functionality to input\n";
		s += "\tvirtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;\n";
	}

	if (k.hasBeginPlay)
	{
		s += "\nprotected:\n";
		s += "\t// Called when the game starts" + std::string(k.isComponent ? "" : " or when spawned") + "\n";
		s += "\tvirtual void BeginPlay() override;\n";
	}

	if (spec.tick && k.canTick)
	{
		s += "\npublic:\n";
		s += "\t// Called every frame\n";
		if (k.isComponent)
			s += "\tvirtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;\n";
		else
			s += "\tvirtual void Tick(float DeltaTime) override;\n";
	}

	s += "};\n";
	return s;
}

std::string generateSource(const ClassSpec& spec)
{
	const KindInfo& k = infoFor(spec.kind);
	std::string cls = prefixedClassName(spec);

	std::string s;
	s += "#include \"" + spec.name + ".h\"\n\n";

	if (spec.kind == ClassKind::Interface)
	{
		s += "// Interfaces carry no state; default implementations of\n";
		s += "// BlueprintNativeEvent functions go here as <Name>_Implementation.\n";
		return s;
	}

	// Constructor — set up ticking the way the engine templates do.
	s += cls + "::" + cls + "()\n{\n";
	if (k.canTick)
	{
		const char* tickVar = k.isComponent ? "PrimaryComponentTick" : "PrimaryActorTick";
		s += "\t// Set this to true if this ";
		s += k.isComponent ? "component" : "actor";
		s += " needs per-frame updates. Leaving\n\t// ticking off when unused is a free performance win.\n";
		s += "\t" + std::string(tickVar) + ".bCanEverTick = " + (spec.tick ? "true" : "false") + ";\n";
	}
	s += "}\n";

	if (k.hasBeginPlay)
	{
		s += "\nvoid " + cls + "::BeginPlay()\n{\n\tSuper::BeginPlay();\n}\n";
	}

	if (spec.kind == ClassKind::Character || spec.kind == ClassKind::Pawn)
	{
		s += "\nvoid " + cls + "::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)\n";
		s += "{\n\tSuper::SetupPlayerInputComponent(PlayerInputComponent);\n}\n";
	}

	if (spec.tick && k.canTick)
	{
		if (k.isComponent)
		{
			s += "\nvoid " + cls + "::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)\n";
			s += "{\n\tSuper::TickComponent(DeltaTime, TickType, ThisTickFunction);\n}\n";
		}
		else
		{
			s += "\nvoid " + cls + "::Tick(float DeltaTime)\n";
			s += "{\n\tSuper::Tick(DeltaTime);\n}\n";
		}
	}

	return s;
}

std::vector<std::pair<std::string, std::string>> parseParamList(const std::string& text)
{
	std::vector<std::pair<std::string, std::string>> out;
	size_t start = 0;
	int depth = 0;   // ignore commas inside template args: TMap<FName, int32> Counts
	for (size_t i = 0; i <= text.size(); ++i)
	{
		char c = i < text.size() ? text[i] : ',';
		if (c == '<' || c == '(')
			++depth;
		else if (c == '>' || c == ')')
			--depth;
		if (c != ',' || depth > 0)
			continue;
		std::string piece = trim(text.substr(start, i - start));
		start = i + 1;
		if (piece.empty())
			continue;
		// The name is the last identifier run; the type is everything before it.
		size_t e = piece.size();
		while (e > 0 && (std::isalnum((unsigned char) piece[e - 1]) || piece[e - 1] == '_'))
			--e;
		std::string name = piece.substr(e);
		std::string type = trim(piece.substr(0, e));
		if (name.empty())
		{
			// No name given ("float") — synthesize one so the stub compiles.
			type = piece;
			name = "Param" + std::to_string(out.size() + 1);
		}
		if (type.empty())
		{
			// Only one token ("Amount") — treat it as a name with a float type,
			// the most common Blueprint parameter.
			type = "float";
		}
		out.push_back({type, name});
	}
	return out;
}

std::string generateFunctionDecl(const FunctionSpec& fn)
{
	std::string s = "UFUNCTION(";
	s += fn.pure ? "BlueprintPure" : "BlueprintCallable";
	s += ", Category = \"" + (fn.category.empty() ? std::string("Default") : fn.category) + "\")\n";
	s += fn.returnType + " " + fn.name + "(" + joinParams(fn) + ");\n";
	return s;
}

std::string generateFunctionDef(const FunctionSpec& fn, const std::string& className)
{
	std::string s = fn.returnType + " " + className + "::" + fn.name + "(" + joinParams(fn) + ")\n{\n";
	s += "\t// TODO: implement\n";
	if (fn.returnType != "void" && !fn.returnType.empty())
		s += "\treturn " + fn.returnType + "{};\n";
	s += "}\n";
	return s;
}

std::string generatePropertyDecl(const PropertySpec& prop)
{
	std::string s = "UPROPERTY(";
	s += prop.editAnywhere ? "EditAnywhere" : "VisibleAnywhere";
	s += prop.readOnly ? ", BlueprintReadOnly" : ", BlueprintReadWrite";
	s += ", Category = \"" + (prop.category.empty() ? std::string("Default") : prop.category) + "\")\n";
	s += prop.type + " " + prop.name + ";\n";
	return s;
}

bool findUProject(const std::filesystem::path& root, std::filesystem::path& out)
{
	std::error_code ec;
	if (root.empty() || !std::filesystem::is_directory(root, ec))
		return false;
	std::vector<std::filesystem::path> found;
	for (auto& entry : std::filesystem::directory_iterator(root, ec))
	{
		if (ec)
			break;
		if (!entry.is_regular_file(ec))
			continue;
		auto ext = entry.path().extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
		if (ext == ".uproject")
			found.push_back(entry.path());
	}
	if (found.empty())
		return false;
	std::sort(found.begin(), found.end());
	out = found.front();
	return true;
}

std::filesystem::path defaultClassDir(const std::filesystem::path& root, const std::string& moduleName)
{
	std::error_code ec;
	if (!moduleName.empty())
	{
		auto dir = root / "Source" / moduleName;
		if (std::filesystem::is_directory(dir, ec))
			return std::filesystem::path("Source") / moduleName;
	}
	if (std::filesystem::is_directory(root / "Source", ec))
		return "Source";
	return {};
}

} // namespace unreal
