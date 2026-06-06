//	Tree-sitter-backed symbol extraction.
//
//	Pure logic (no ImGui/SDL): parse a source buffer with the matching grammar
//	and pull out definitions + references via the grammar's tags.scm query. This
//	is the foundation for accurate go-to-definition / declaration and member-aware
//	autocomplete, replacing the grep-based heuristics.

#pragma once

#include <string>
#include <vector>

namespace ts {

enum class Kind {
	Unknown,
	Function,
	Method,
	Class,
	Struct,
	Enum,
	Type,
	Field,
	Variable,
	Constant,
	Module,
	Macro,
};

struct Symbol {
	std::string name;
	Kind        kind = Kind::Unknown;
	int         line = 0;            // 0-based
	int         column = 0;         // 0-based
	bool        isDefinition = true; // definition (vs reference)
	std::string enclosingType;       // simple name of the containing type ("" if top-level)
};

// Languages we currently have a grammar + tags query for.
enum class Lang { None, Cpp, CSharp };

// Map a file extension (".cpp", ".h", …) to a supported grammar, or Lang::None.
Lang langForExtension(const std::string& ext);

// Parse `source` as `lang` and return its tagged symbols (definitions + refs).
// Empty result if the language is unsupported or the buffer fails to parse.
std::vector<Symbol> extractSymbols(Lang lang, const std::string& source);

// Curated member lists for common standard-library types (string, vector, map,
// optional, smart pointers, …). The project index can only see members of types
// it actually parses, so std::vector / std::string receivers would otherwise
// never complete. `simpleType` is the unqualified name ("vector", "string").
// Returns nullptr when the type isn't in the table.
const std::vector<std::string>* stlMembers(const std::string& simpleType);

// True if tree-sitter built in and a grammar is available.
bool available();

} // namespace ts
