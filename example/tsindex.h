//	Tree-sitter-backed symbol extraction.
//
//	Pure logic (no ImGui/SDL): parse a source buffer with the matching grammar
//	and pull out definitions + references via the grammar's tags.scm query. This
//	is the foundation for accurate go-to-definition / declaration and member-aware
//	autocomplete, replacing the grep-based heuristics.

#pragma once

#include <string>
#include <unordered_map>
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
enum class Lang { None, Cpp, CSharp, Python, JavaScript, Lua, Go, Rust };

// Map a file extension (".cpp", ".h", …) to a supported grammar, or Lang::None.
Lang langForExtension(const std::string& ext);

// Parse `source` as `lang` and return its tagged symbols (definitions + refs).
// Empty result if the language is unsupported or the buffer fails to parse.
std::vector<Symbol> extractSymbols(Lang lang, const std::string& source);

// Scope-aware receiver-type resolver for member autocomplete. Parses `source`
// and finds the declaration of `receiver` visible at the cursor, respecting
// scope + declaration-before-use (so an inner shadow beats an outer decl).
//   line : 0-based row (matches TSPoint / Symbol.line)
//   col  : 0-based BYTE column (not codepoint — caller converts first)
// Handles locals, parameters, fields, `this`, range/foreach vars, and best-
// effort auto/var (from `new T()` / `T()`); returns the simple type name
// ("Foo", "vector") or "" when it can't resolve (caller then falls back).
std::string resolveLocalType(Lang lang, const std::string& source,
                             int line, int col, const std::string& receiver);

// type name -> (member name -> member's declared type). Lets member-chain
// resolution hop into types defined in OTHER files (the per-doc parse only sees
// the current file). Built project-wide by extractMemberTypes per file.
using MemberTypeMap = std::unordered_map<std::string, std::unordered_map<std::string, std::string>>;

// Extract every type's data members and their declared types from one source
// buffer (pointer members reduced to the pointee). C++ / C# only; empty for
// other languages. The complement to extractSymbols, used to build the project's
// MemberTypeMap so chained completion works across files.
MemberTypeMap extractMemberTypes(Lang lang, const std::string& source);

// Resolve a dotted member chain to its final type, for member-of-member
// completion (`a.b.c` -> type of c). `chain` is the receiver segments in order
// ({"a","b","c"}); (line,col) locates the base receiver for scope resolution.
// The base is resolved like resolveLocalType; each subsequent hop descends into
// the prior type's definition — first IN THIS DOCUMENT, then (if `index` is
// given) via the project-wide MemberTypeMap so types in other files resolve too.
// Returns "" if any hop can't be resolved. A single-element chain is exactly
// resolveLocalType. C++ / C# only.
std::string resolveMemberChain(Lang lang, const std::string& source,
                               int line, int col, const std::vector<std::string>& chain,
                               const MemberTypeMap* index = nullptr);

// Curated member lists for common standard-library types (string, vector, map,
// optional, smart pointers, …). The project index can only see members of types
// it actually parses, so std::vector / std::string receivers would otherwise
// never complete. `simpleType` is the unqualified name ("vector", "string").
// Returns nullptr when the type isn't in the table.
const std::vector<std::string>* stlMembers(const std::string& simpleType);

// Augment the member table at runtime from a pregenerated symbol pack (loaded off
// disk — see Editor::loadSymbolPacks). The project index can only see types it
// parses, and the compiled table above only covers hand-curated STL/UE types; a
// pack teaches it members for a whole framework (STL by C++ standard, .NET BCL,
// Python builtins, Lua stdlib, Unreal, …) without a recompile.
//
// Merges into the type's member list (deduped, order-preserving). The registry is
// a node-based unordered_map, so pointers returned by stlMembers() stay valid
// across later registrations. NOT thread-safe against a concurrent stlMembers()
// read — register everything during startup, before the render loop queries it.
void registerTypeMembers(const std::string& simpleType, const std::vector<std::string>& members);

// How many types the runtime registry currently holds (for a status line / tests).
size_t registeredTypeCount();

// True if tree-sitter built in and a grammar is available.
bool available();

// ── On-disk symbol cache ────────────────────────────────────────────────────
// One entry per indexed source file: its tree-sitter symbols plus the mtime/size
// used to detect staleness on the next project load. Lets the index persist
// across sessions and rebuild incrementally (skip unchanged files' parses).
struct FileSyms {
	long long          mtime = 0;   // last-write time (filesystem clock ticks)
	unsigned long long size  = 0;   // byte size
	std::vector<Symbol> symbols;
	MemberTypeMap       memberTypes; // types defined in THIS file -> members -> type (for cross-file chains)
};

// Binary, host-endian (the cache is machine-local). Return false on any I/O or
// format error — the caller then just rebuilds from scratch.
bool writeIndexCache(const std::string& path, const std::unordered_map<std::string, FileSyms>& files);
bool readIndexCache(const std::string& path, std::unordered_map<std::string, FileSyms>& out);

} // namespace ts
