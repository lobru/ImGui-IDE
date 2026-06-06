//	Tree-sitter-backed symbol extraction — implementation.

#include "tsindex.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <tree_sitter/api.h>

// Grammar entry points (defined in the generated parser.c files).
extern "C" const TSLanguage* tree_sitter_cpp(void);
extern "C" const TSLanguage* tree_sitter_c_sharp(void);
extern "C" const TSLanguage* tree_sitter_python(void);
extern "C" const TSLanguage* tree_sitter_javascript(void);
extern "C" const TSLanguage* tree_sitter_lua(void);
extern "C" const TSLanguage* tree_sitter_go(void);
extern "C" const TSLanguage* tree_sitter_rust(void);

#ifndef TS_CPP_TAGS_SCM
#define TS_CPP_TAGS_SCM ""
#endif
#ifndef TS_CSHARP_TAGS_SCM
#define TS_CSHARP_TAGS_SCM ""
#endif
#ifndef TS_PYTHON_TAGS_SCM
#define TS_PYTHON_TAGS_SCM ""
#endif
#ifndef TS_JS_TAGS_SCM
#define TS_JS_TAGS_SCM ""
#endif
#ifndef TS_LUA_TAGS_SCM
#define TS_LUA_TAGS_SCM ""
#endif
#ifndef TS_GO_TAGS_SCM
#define TS_GO_TAGS_SCM ""
#endif
#ifndef TS_RUST_TAGS_SCM
#define TS_RUST_TAGS_SCM ""
#endif

namespace ts {

namespace {

const TSLanguage* languageFor(Lang lang)
{
	switch (lang) {
		case Lang::Cpp:        return tree_sitter_cpp();
		case Lang::CSharp:     return tree_sitter_c_sharp();
		case Lang::Python:     return tree_sitter_python();
		case Lang::JavaScript: return tree_sitter_javascript();
		case Lang::Lua:        return tree_sitter_lua();
		case Lang::Go:         return tree_sitter_go();
		case Lang::Rust:       return tree_sitter_rust();
		default:               return nullptr;
	}
}

std::string readFile(const char* path)
{
	std::ifstream f(path);
	if (!f.is_open()) return {};
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// The grammar's tags.scm, loaded once per language and (for C++) augmented. The
// stock C++ query only tags single-namespace out-of-line defs (ns::bar), missing
// nested ones (ns::Foo::bar) — the actual method body go-to-def wants — so we add
// patterns for qualified definitions + data members + enum constants. Same-node
// double captures are removed by position dedup in extractSymbols.
const std::string& tagsQueryFor(Lang lang)
{
	static const std::string cpp = [] {
		std::string base = readFile(TS_CPP_TAGS_SCM);
		if (base.empty()) return base;
		base +=
			"\n"
			"(function_definition declarator: (function_declarator declarator: (qualified_identifier) @name)) @definition.method\n"
			"(field_declaration declarator: (field_identifier) @name) @definition.field\n"
			"(enumerator name: (identifier) @name) @definition.constant\n";
		return base;
	}();
	static const std::string csharp = [] {
		std::string base = readFile(TS_CSHARP_TAGS_SCM);
		if (base.empty()) return base;
		// Stock C# tags capture class/interface/method/namespace only — add data
		// members so member completion isn't methods-only.
		base +=
			"\n"
			"(field_declaration (variable_declaration (variable_declarator (identifier) @name))) @definition.field\n"
			"(property_declaration name: (identifier) @name) @definition.field\n";
		return base;
	}();
	// Python + JavaScript: stock tags.scm is enough (class/function/method
	// captures); no augmentation needed.
	static const std::string python     = readFile(TS_PYTHON_TAGS_SCM);
	static const std::string javascript = readFile(TS_JS_TAGS_SCM);
	static const std::string lua        = readFile(TS_LUA_TAGS_SCM);
	static const std::string go         = readFile(TS_GO_TAGS_SCM);
	static const std::string rust       = readFile(TS_RUST_TAGS_SCM);
	static const std::string none;

	switch (lang) {
		case Lang::Cpp:        return cpp;
		case Lang::CSharp:     return csharp;
		case Lang::Python:     return python;
		case Lang::JavaScript: return javascript;
		case Lang::Lua:        return lua;
		case Lang::Go:         return go;
		case Lang::Rust:       return rust;
		default:               return none;
	}
}

Kind kindFromCapture(const std::string& tail)
{
	if (tail == "function")            return Kind::Function;
	if (tail == "method")              return Kind::Method;
	if (tail == "class")               return Kind::Class;
	if (tail == "struct")              return Kind::Struct;
	if (tail == "enum")                return Kind::Enum;
	if (tail == "type" || tail == "interface") return Kind::Type;
	if (tail == "field" || tail == "member")   return Kind::Field;
	if (tail == "constant")            return Kind::Constant;
	if (tail == "variable")            return Kind::Variable;
	if (tail == "module" || tail == "namespace") return Kind::Module;
	if (tail == "macro")               return Kind::Macro;
	return Kind::Unknown;
}

std::string nodeText(TSNode n, const std::string& src)
{
	uint32_t s = ts_node_start_byte(n), e = ts_node_end_byte(n);
	if (e > s && e <= src.size()) return src.substr(s, e - s);
	return {};
}

std::string lastSegment(std::string s)
{
	if (auto p = s.rfind("::"); p != std::string::npos) s = s.substr(p + 2);
	if (auto p = s.rfind('.'); p != std::string::npos) s = s.substr(p + 1);
	return s;
}

// Walk up from a name node to the tightest enclosing type declaration (C++ class/
// struct/enum, C# class/struct/interface/enum/record) and return its simple name.
// Empty if the symbol is top-level (e.g. an out-of-line C++ definition).
std::string enclosingTypeOf(TSNode nameNode, const std::string& src)
{
	for (TSNode n = ts_node_parent(nameNode); !ts_node_is_null(n); n = ts_node_parent(n))
	{
		const char* t = ts_node_type(n);
		std::string ty = t ? t : "";
		if (ty == "class_specifier" || ty == "struct_specifier" || ty == "enum_specifier" ||
			ty == "class_declaration" || ty == "struct_declaration" || ty == "interface_declaration" ||
			ty == "enum_declaration" || ty == "record_declaration" ||
			ty == "class_definition" ||                       // Python
			ty == "class")                                    // JavaScript class expression
		{
			TSNode nameField = ts_node_child_by_field_name(n, "name", 4);
			// If this container's own name IS our node, it's the type's own
			// declaration — keep walking so a type's enclosing is its OUTER type.
			if (!ts_node_is_null(nameField) && ts_node_eq(nameField, nameNode))
				continue;
			if (!ts_node_is_null(nameField))
				return lastSegment(nodeText(nameField, src));
			return {};
		}
	}
	return {};
}

} // namespace

Lang langForExtension(const std::string& extIn)
{
	std::string ext = extIn;
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
	if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c" ||
		ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh" || ext == ".inl")
		return Lang::Cpp;
	if (ext == ".cs")
		return Lang::CSharp;
	if (ext == ".py" || ext == ".pyw" || ext == ".pyi")
		return Lang::Python;
	if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs")
		return Lang::JavaScript;
	if (ext == ".lua")
		return Lang::Lua;
	if (ext == ".go")
		return Lang::Go;
	if (ext == ".rs")
		return Lang::Rust;
	return Lang::None;
}

bool available()
{
	return languageFor(Lang::Cpp) != nullptr;
}

const std::vector<std::string>* stlMembers(const std::string& simpleType)
{
	// Member NAMES only (no signatures) — completion inserts the name and the
	// user types the call. Curated for the everyday containers/wrappers; common
	// members listed, not the exhaustive interface.
	static const std::unordered_map<std::string, std::vector<std::string>> table = {
		{"string", {"size", "length", "empty", "clear", "append", "push_back", "pop_back",
			"substr", "find", "rfind", "find_first_of", "find_last_of", "replace", "insert",
			"erase", "c_str", "data", "at", "front", "back", "begin", "end", "rbegin", "rend",
			"compare", "resize", "reserve", "capacity", "shrink_to_fit", "swap", "assign",
			"starts_with", "ends_with", "contains"}},
		{"wstring", {"size", "length", "empty", "clear", "append", "push_back", "substr",
			"find", "replace", "insert", "erase", "c_str", "data", "at", "front", "back",
			"begin", "end", "compare", "resize", "reserve", "capacity"}},
		{"string_view", {"size", "length", "empty", "substr", "find", "rfind", "data", "at",
			"front", "back", "begin", "end", "remove_prefix", "remove_suffix", "compare",
			"starts_with", "ends_with", "contains"}},
		{"vector", {"push_back", "pop_back", "emplace_back", "size", "empty", "clear",
			"resize", "reserve", "capacity", "shrink_to_fit", "at", "front", "back", "data",
			"begin", "end", "rbegin", "rend", "insert", "emplace", "erase", "assign", "swap"}},
		{"deque", {"push_back", "push_front", "pop_back", "pop_front", "emplace_back",
			"emplace_front", "size", "empty", "clear", "resize", "at", "front", "back",
			"begin", "end", "insert", "erase", "swap"}},
		{"list", {"push_back", "push_front", "pop_back", "pop_front", "emplace_back",
			"emplace_front", "size", "empty", "clear", "front", "back", "begin", "end",
			"insert", "erase", "remove", "remove_if", "sort", "reverse", "unique", "splice",
			"merge", "swap"}},
		{"forward_list", {"push_front", "pop_front", "emplace_front", "empty", "clear",
			"front", "begin", "end", "insert_after", "erase_after", "remove", "sort",
			"reverse", "splice_after"}},
		{"array", {"size", "at", "front", "back", "data", "begin", "end", "rbegin", "rend",
			"fill", "empty", "swap"}},
		{"map", {"insert", "insert_or_assign", "emplace", "try_emplace", "find", "count",
			"contains", "at", "erase", "clear", "size", "empty", "begin", "end", "lower_bound",
			"upper_bound", "equal_range", "swap"}},
		{"multimap", {"insert", "emplace", "find", "count", "contains", "erase", "clear",
			"size", "empty", "begin", "end", "lower_bound", "upper_bound", "equal_range"}},
		{"unordered_map", {"insert", "insert_or_assign", "emplace", "try_emplace", "find",
			"count", "contains", "at", "erase", "clear", "size", "empty", "begin", "end",
			"reserve", "rehash", "bucket_count", "load_factor", "swap"}},
		{"set", {"insert", "emplace", "find", "count", "contains", "erase", "clear", "size",
			"empty", "begin", "end", "lower_bound", "upper_bound", "equal_range", "swap"}},
		{"multiset", {"insert", "emplace", "find", "count", "contains", "erase", "clear",
			"size", "empty", "begin", "end", "lower_bound", "upper_bound", "equal_range"}},
		{"unordered_set", {"insert", "emplace", "find", "count", "contains", "erase", "clear",
			"size", "empty", "begin", "end", "reserve", "rehash", "bucket_count", "swap"}},
		{"pair", {"first", "second", "swap"}},
		{"tuple", {"swap"}},
		{"optional", {"value", "has_value", "value_or", "reset", "emplace", "swap"}},
		{"unique_ptr", {"get", "reset", "release", "swap", "operator bool"}},
		{"shared_ptr", {"get", "reset", "swap", "use_count", "unique", "operator bool"}},
		{"weak_ptr", {"lock", "reset", "expired", "use_count", "swap"}},
		{"stack", {"push", "pop", "top", "emplace", "size", "empty", "swap"}},
		{"queue", {"push", "pop", "front", "back", "emplace", "size", "empty", "swap"}},
		{"priority_queue", {"push", "pop", "top", "emplace", "size", "empty", "swap"}},
	};
	auto it = table.find(simpleType);
	return it == table.end() ? nullptr : &it->second;
}

std::vector<Symbol> extractSymbols(Lang lang, const std::string& source)
{
	std::vector<Symbol> out;
	if (lang == Lang::None || source.empty())
		return out;

	const std::string& queryStr = tagsQueryFor(lang);
	if (queryStr.empty())
		return out;

	const TSLanguage* language = languageFor(lang);
	if (!language)
		return out;

	TSParser* parser = ts_parser_new();
	ts_parser_set_language(parser, language);
	TSTree* tree = ts_parser_parse_string(parser, nullptr, source.c_str(), (uint32_t) source.size());
	if (!tree)
	{
		ts_parser_delete(parser);
		return out;
	}

	// Compile the tags query ONCE per language and reuse it — it's immutable and
	// built from invariant inputs, so recompiling per file was pure waste on the
	// background reindex. extractSymbols is only called from the serialized index
	// build thread + the startup selftest, so the static cache needs no extra lock.
	static std::unordered_map<int, TSQuery*> queryCache;
	TSQuery* query = nullptr;
	if (auto cit = queryCache.find((int) lang); cit != queryCache.end())
	{
		query = cit->second;
	}
	else
	{
		uint32_t errOffset = 0;
		TSQueryError errType = TSQueryErrorNone;
		query = ts_query_new(language, queryStr.c_str(), (uint32_t) queryStr.size(), &errOffset, &errType);
		queryCache[(int) lang] = query;   // cache even nullptr so a broken query isn't retried
		if (!query)
			std::fprintf(stderr, "[ts] tags query compile failed: lang=%d errType=%d at byte %u\n",
			             (int) lang, (int) errType, errOffset);
	}
	if (!query)
	{
		ts_tree_delete(tree);
		ts_parser_delete(parser);
		return out;
	}

	TSNode root = ts_tree_root_node(tree);
	TSQueryCursor* cursor = ts_query_cursor_new();
	ts_query_cursor_exec(cursor, query, root);

	std::unordered_set<uint64_t> seen;   // (row<<20 | col) keys, drops same-node double captures (O(1))

	TSQueryMatch match;
	while (ts_query_cursor_next_match(cursor, &match))
	{
		Symbol sym;
		bool haveName = false;
		bool haveKind = false;
		for (uint16_t i = 0; i < match.capture_count; ++i)
		{
			const TSQueryCapture& cap = match.captures[i];
			uint32_t len = 0;
			const char* cn = ts_query_capture_name_for_id(query, cap.index, &len);
			std::string capName(cn ? cn : "", len);

			if (capName == "name")
			{
				TSPoint p = ts_node_start_point(cap.node);
				uint32_t s = ts_node_start_byte(cap.node);
				uint32_t e = ts_node_end_byte(cap.node);
				if (e > s && e <= source.size())
				{
					std::string raw = source.substr(s, e - s);   // may be qualified (ns::Foo::bar)
					sym.name = lastSegment(raw);
					sym.line = (int) p.row;
					sym.column = (int) p.column;
					// Enclosing type: tightest container in the tree, else the
					// second-to-last segment of an out-of-line qualified name.
					sym.enclosingType = enclosingTypeOf(cap.node, source);
					if (sym.enclosingType.empty())
					{
						auto last = raw.rfind("::");
						if (last != std::string::npos)
						{
							auto prev = (last >= 2) ? raw.rfind("::", last - 2) : std::string::npos;
							sym.enclosingType = (prev == std::string::npos)
								? raw.substr(0, last)
								: raw.substr(prev + 2, last - (prev + 2));
						}
					}
					haveName = true;
				}
			}
			else if (capName.rfind("definition.", 0) == 0)
			{
				sym.isDefinition = true;
				sym.kind = kindFromCapture(capName.substr(sizeof("definition.") - 1));
				haveKind = true;
			}
			else if (capName.rfind("reference.", 0) == 0)
			{
				sym.isDefinition = false;
				sym.kind = kindFromCapture(capName.substr(sizeof("reference.") - 1));
				haveKind = true;
			}
		}
		if (haveName && haveKind)
		{
			uint64_t key = ((uint64_t) sym.line << 20) | (uint32_t) sym.column;
			if (seen.insert(key).second)
				out.push_back(std::move(sym));
		}
	}

	ts_query_cursor_delete(cursor);
	// query is cached per-language (queryCache) — do NOT delete it.
	ts_tree_delete(tree);
	ts_parser_delete(parser);
	return out;
}

// ── Scope-aware receiver-type resolver ───────────────────────────────────────
// Position-anchored: land at the cursor, walk enclosing scopes nearest-first,
// return the simple type name of the receiver's declaration. A manual cursor walk
// (not a tags query) because resolution must respect scope + declaration-before-
// use — a query has no position anchor and would replicate the old scope-blindness.

namespace {

inline std::string tyOf(TSNode n) { const char* t = ts_node_type(n); return t ? t : std::string(); }

// Reduce a C++ type node to its simple name ("vector" from std::vector<int>).
std::string reduceCppType(TSNode t, const std::string& src)
{
	if (ts_node_is_null(t)) return {};
	std::string ty = tyOf(t);
	if (ty == "type_identifier" || ty == "primitive_type" ||
		ty == "sized_type_specifier" || ty == "namespace_identifier")
		return nodeText(t, src);
	if (ty == "qualified_identifier")
		return reduceCppType(ts_node_child_by_field_name(t, "name", 4), src);
	if (ty == "template_type")
		return reduceCppType(ts_node_child_by_field_name(t, "name", 4), src);
	if (ty == "class_specifier" || ty == "struct_specifier" ||
		ty == "union_specifier" || ty == "enum_specifier")
	{
		TSNode name = ts_node_child_by_field_name(t, "name", 4);
		return ts_node_is_null(name) ? std::string() : nodeText(name, src);
	}
	// placeholder_type_specifier (auto), decltype, dependent_type → unresolvable
	return {};
}

// Reduce a C# type node to its simple name ("List" from System.List<T>).
std::string reduceCsType(TSNode t, const std::string& src)
{
	if (ts_node_is_null(t)) return {};
	std::string ty = tyOf(t);
	if (ty == "identifier" || ty == "predefined_type") return nodeText(t, src);
	if (ty == "qualified_name")
		return reduceCsType(ts_node_child_by_field_name(t, "name", 4), src);
	if (ty == "generic_name")
	{
		uint32_t n = ts_node_named_child_count(t);
		for (uint32_t i = 0; i < n; ++i)
		{
			TSNode c = ts_node_named_child(t, i);
			if (tyOf(c) == "identifier") return nodeText(c, src);
		}
		return {};
	}
	if (ty == "nullable_type" || ty == "array_type" || ty == "pointer_type")
		return reduceCsType(ts_node_child_by_field_name(t, "type", 4), src);
	// implicit_type (var) → unresolvable here (caller infers from initializer)
	return {};
}

// Descend a C++ declarator chain to the declared identifier's text.
std::string cppDeclName(TSNode d, const std::string& src)
{
	if (ts_node_is_null(d)) return {};
	std::string ty = tyOf(d);
	if (ty == "identifier" || ty == "field_identifier") return nodeText(d, src);
	if (ty == "pointer_declarator" || ty == "array_declarator" ||
		ty == "function_declarator" || ty == "init_declarator")
		return cppDeclName(ts_node_child_by_field_name(d, "declarator", 10), src);
	if (ty == "reference_declarator" || ty == "parenthesized_declarator" ||
		ty == "attributed_declarator")
	{
		// No `declarator` field — inner is a positional named child.
		uint32_t n = ts_node_named_child_count(d);
		for (uint32_t i = 0; i < n; ++i)
		{
			std::string r = cppDeclName(ts_node_named_child(d, i), src);
			if (!r.empty()) return r;
		}
		return {};
	}
	if (ty == "qualified_identifier")
		return cppDeclName(ts_node_child_by_field_name(d, "name", 4), src);
	return {};
}

// Walk pointer/reference wrappers to the function_declarator (null node if none).
TSNode cppFuncDeclarator(TSNode d)
{
	if (ts_node_is_null(d)) return TSNode{};
	std::string ty = tyOf(d);
	if (ty == "function_declarator") return d;
	if (ty == "pointer_declarator" || ty == "array_declarator" ||
		ty == "reference_declarator" || ty == "parenthesized_declarator")
	{
		TSNode inner = ts_node_child_by_field_name(d, "declarator", 10);
		if (!ts_node_is_null(inner)) return cppFuncDeclarator(inner);
		uint32_t n = ts_node_named_child_count(d);
		for (uint32_t i = 0; i < n; ++i)
		{
			TSNode r = cppFuncDeclarator(ts_node_named_child(d, i));
			if (!ts_node_is_null(r)) return r;
		}
	}
	return TSNode{};
}

// Match a C++ `declaration` (one type, possibly several declarators) to recv;
// resolves `auto` from a new-expression or constructor call initializer.
std::string matchCppDecl(TSNode decl, const std::string& src, const std::string& recv)
{
	TSNode typeNode = ts_node_child_by_field_name(decl, "type", 4);
	if (ts_node_is_null(typeNode)) return {};
	bool isAuto = (tyOf(typeNode) == "placeholder_type_specifier");
	uint32_t n = ts_node_named_child_count(decl);
	for (uint32_t i = 0; i < n; ++i)
	{
		TSNode child = ts_node_named_child(decl, i);
		if (ts_node_eq(child, typeNode)) continue;
		std::string cty = tyOf(child);
		if (cty == "init_declarator")
		{
			if (cppDeclName(ts_node_child_by_field_name(child, "declarator", 10), src) != recv)
				continue;
			if (!isAuto) return reduceCppType(typeNode, src);
			TSNode val = ts_node_child_by_field_name(child, "value", 5);
			if (ts_node_is_null(val)) return {};
			std::string vty = tyOf(val);
			if (vty == "new_expression")
				return reduceCppType(ts_node_child_by_field_name(val, "type", 4), src);
			if (vty == "call_expression")
			{
				TSNode fn = ts_node_child_by_field_name(val, "function", 8);
				if (ts_node_is_null(fn)) return {};
				std::string fty = tyOf(fn);
				if (fty == "identifier") return nodeText(fn, src);
				if (fty == "qualified_identifier" || fty == "template_function")
				{
					TSNode nm = ts_node_child_by_field_name(fn, "name", 4);
					if (!ts_node_is_null(nm)) return nodeText(nm, src);
				}
			}
			return {};   // auto with unresolvable initializer
		}
		if (isAuto) continue;
		if (cppDeclName(child, src) == recv) return reduceCppType(typeNode, src);
	}
	return {};
}

std::string searchCppBlock(TSNode block, const std::string& src, const std::string& recv, TSPoint cur)
{
	uint32_t n = ts_node_named_child_count(block);
	for (uint32_t i = 0; i < n; ++i)
	{
		TSNode child = ts_node_named_child(block, i);
		TSPoint sp = ts_node_start_point(child);
		if (sp.row > cur.row || (sp.row == cur.row && sp.column >= cur.column))
			break;   // declaration-before-use
		if (tyOf(child) == "declaration")
		{
			std::string r = matchCppDecl(child, src, recv);
			if (!r.empty()) return r;
		}
	}
	return {};
}

std::string searchCppParams(TSNode funcDef, const std::string& src, const std::string& recv)
{
	TSNode fd = cppFuncDeclarator(ts_node_child_by_field_name(funcDef, "declarator", 10));
	if (ts_node_is_null(fd)) return {};
	TSNode params = ts_node_child_by_field_name(fd, "parameters", 10);
	if (ts_node_is_null(params)) return {};
	uint32_t n = ts_node_named_child_count(params);
	for (uint32_t i = 0; i < n; ++i)
	{
		TSNode p = ts_node_named_child(params, i);
		std::string pty = tyOf(p);
		if (pty != "parameter_declaration" && pty != "optional_parameter_declaration") continue;
		TSNode pdecl = ts_node_child_by_field_name(p, "declarator", 10);
		if (ts_node_is_null(pdecl) || cppDeclName(pdecl, src) != recv) continue;
		TSNode typeNode = ts_node_child_by_field_name(p, "type", 4);
		if (ts_node_is_null(typeNode) || tyOf(typeNode) == "placeholder_type_specifier") return {};
		return reduceCppType(typeNode, src);
	}
	return {};
}

std::string searchCppClassMembers(TSNode cls, const std::string& src, const std::string& recv)
{
	TSNode body = ts_node_child_by_field_name(cls, "body", 4);
	if (ts_node_is_null(body)) return {};
	uint32_t n = ts_node_named_child_count(body);
	for (uint32_t i = 0; i < n; ++i)
	{
		TSNode child = ts_node_named_child(body, i);
		if (tyOf(child) != "field_declaration") continue;
		TSNode typeNode = ts_node_child_by_field_name(child, "type", 4);
		if (ts_node_is_null(typeNode)) continue;
		uint32_t m = ts_node_named_child_count(child);
		for (uint32_t j = 0; j < m; ++j)
		{
			TSNode fd = ts_node_named_child(child, j);
			if (ts_node_eq(fd, typeNode)) continue;
			if (cppDeclName(fd, src) == recv) return reduceCppType(typeNode, src);
		}
	}
	return {};
}

std::string searchCppForRange(TSNode f, const std::string& src, const std::string& recv)
{
	TSNode typeNode = ts_node_child_by_field_name(f, "type", 4);
	TSNode decl = ts_node_child_by_field_name(f, "declarator", 10);
	if (ts_node_is_null(typeNode) || ts_node_is_null(decl)) return {};
	if (cppDeclName(decl, src) != recv) return {};
	if (tyOf(typeNode) == "placeholder_type_specifier") return {};   // `for (auto& x : ...)`
	return reduceCppType(typeNode, src);
}

std::string searchCppForInit(TSNode f, const std::string& src, const std::string& recv)
{
	TSNode init = ts_node_child_by_field_name(f, "initializer", 11);
	if (ts_node_is_null(init)) return {};
	if (tyOf(init) == "init_statement")
	{
		uint32_t n = ts_node_named_child_count(init);
		for (uint32_t i = 0; i < n; ++i)
		{
			TSNode c = ts_node_named_child(init, i);
			if (tyOf(c) == "declaration") { init = c; break; }
		}
	}
	if (tyOf(init) != "declaration") return {};
	return matchCppDecl(init, src, recv);
}

std::string resolveThisCpp(TSNode start, const std::string& src)
{
	for (TSNode n = start; !ts_node_is_null(n); n = ts_node_parent(n))
	{
		std::string ty = tyOf(n);
		if (ty == "class_specifier" || ty == "struct_specifier" || ty == "union_specifier")
		{
			TSNode name = ts_node_child_by_field_name(n, "name", 4);
			return ts_node_is_null(name) ? std::string() : lastSegment(nodeText(name, src));
		}
		if (ty == "function_definition")
		{
			TSNode fd = cppFuncDeclarator(ts_node_child_by_field_name(n, "declarator", 10));
			if (!ts_node_is_null(fd))
			{
				TSNode inner = ts_node_child_by_field_name(fd, "declarator", 10);
				if (!ts_node_is_null(inner) && tyOf(inner) == "qualified_identifier")
				{
					// Out-of-line def A::B::method — enclosing type is the
					// second-to-last "::" segment (handles arbitrary nesting).
					std::string full = nodeText(inner, src);
					std::vector<std::string> segs;
					size_t pos = 0;
					while (true)
					{
						size_t c = full.find("::", pos);
						if (c == std::string::npos) { segs.push_back(full.substr(pos)); break; }
						segs.push_back(full.substr(pos, c - pos));
						pos = c + 2;
					}
					if (segs.size() >= 2) return segs[segs.size() - 2];
				}
			}
			// in-class method: fall through, keep walking to class_specifier
		}
	}
	return {};
}

// C# variable_declaration -> match recv -> type (or var-inferred from new T()).
std::string csVarDeclMatch(TSNode varDecl, const std::string& src, const std::string& recv)
{
	TSNode typeNode = ts_node_child_by_field_name(varDecl, "type", 4);
	bool isVar = !ts_node_is_null(typeNode) && tyOf(typeNode) == "implicit_type";
	uint32_t n = ts_node_named_child_count(varDecl);
	for (uint32_t i = 0; i < n; ++i)
	{
		TSNode d = ts_node_named_child(varDecl, i);
		if (tyOf(d) != "variable_declarator") continue;
		TSNode name = ts_node_child_by_field_name(d, "name", 4);
		if (ts_node_is_null(name) || nodeText(name, src) != recv) continue;
		if (!isVar) return reduceCsType(typeNode, src);
		uint32_t m = ts_node_named_child_count(d);   // var: infer from new T()
		for (uint32_t j = 0; j < m; ++j)
		{
			TSNode c = ts_node_named_child(d, j);
			if (tyOf(c) == "object_creation_expression")
				return reduceCsType(ts_node_child_by_field_name(c, "type", 4), src);
		}
		return {};
	}
	return {};
}

std::string searchCsBlock(TSNode block, const std::string& src, const std::string& recv, TSPoint cur)
{
	uint32_t n = ts_node_named_child_count(block);
	for (uint32_t i = 0; i < n; ++i)
	{
		TSNode child = ts_node_named_child(block, i);
		TSPoint sp = ts_node_start_point(child);
		if (sp.row > cur.row || (sp.row == cur.row && sp.column >= cur.column))
			break;
		if (tyOf(child) != "local_declaration_statement") continue;
		uint32_t m = ts_node_named_child_count(child);
		for (uint32_t j = 0; j < m; ++j)
		{
			TSNode vd = ts_node_named_child(child, j);
			if (tyOf(vd) == "variable_declaration")
			{
				std::string r = csVarDeclMatch(vd, src, recv);
				if (!r.empty()) return r;
			}
		}
	}
	return {};
}

std::string searchCsParams(TSNode method, const std::string& src, const std::string& recv)
{
	TSNode params = ts_node_child_by_field_name(method, "parameters", 10);
	if (ts_node_is_null(params)) return {};
	uint32_t n = ts_node_named_child_count(params);
	for (uint32_t i = 0; i < n; ++i)
	{
		TSNode p = ts_node_named_child(params, i);
		if (tyOf(p) != "parameter") continue;
		TSNode name = ts_node_child_by_field_name(p, "name", 4);
		if (ts_node_is_null(name) || nodeText(name, src) != recv) continue;
		return reduceCsType(ts_node_child_by_field_name(p, "type", 4), src);
	}
	return {};
}

std::string searchCsClassMembers(TSNode cls, const std::string& src, const std::string& recv)
{
	TSNode body = ts_node_child_by_field_name(cls, "body", 4);
	if (ts_node_is_null(body)) return {};
	uint32_t n = ts_node_named_child_count(body);
	for (uint32_t i = 0; i < n; ++i)
	{
		TSNode child = ts_node_named_child(body, i);
		std::string cty = tyOf(child);
		if (cty == "field_declaration")
		{
			uint32_t m = ts_node_named_child_count(child);
			for (uint32_t j = 0; j < m; ++j)
			{
				TSNode vd = ts_node_named_child(child, j);
				if (tyOf(vd) == "variable_declaration")
				{
					std::string r = csVarDeclMatch(vd, src, recv);
					if (!r.empty()) return r;
				}
			}
		}
		else if (cty == "property_declaration")
		{
			TSNode name = ts_node_child_by_field_name(child, "name", 4);
			if (!ts_node_is_null(name) && nodeText(name, src) == recv)
				return reduceCsType(ts_node_child_by_field_name(child, "type", 4), src);
		}
	}
	return {};
}

std::string searchCsForeach(TSNode f, const std::string& src, const std::string& recv)
{
	TSNode left = ts_node_child_by_field_name(f, "left", 4);
	if (ts_node_is_null(left) || nodeText(left, src) != recv) return {};
	TSNode typeNode = ts_node_child_by_field_name(f, "type", 4);
	if (ts_node_is_null(typeNode) || tyOf(typeNode) == "implicit_type") return {};
	return reduceCsType(typeNode, src);
}

std::string resolveThisCs(TSNode start, const std::string& src)
{
	for (TSNode n = start; !ts_node_is_null(n); n = ts_node_parent(n))
	{
		std::string ty = tyOf(n);
		if (ty == "class_declaration" || ty == "struct_declaration" ||
			ty == "record_declaration" || ty == "interface_declaration")
		{
			TSNode name = ts_node_child_by_field_name(n, "name", 4);
			return ts_node_is_null(name) ? std::string() : nodeText(name, src);
		}
	}
	return {};
}

std::string walkScopes(Lang lang, TSNode start, const std::string& src,
					   const std::string& recv, TSPoint cur)
{
	for (TSNode n = start; !ts_node_is_null(n); n = ts_node_parent(n))
	{
		std::string ty = tyOf(n);
		std::string r;
		if (lang == Lang::Cpp)
		{
			if (ty == "compound_statement")       r = searchCppBlock(n, src, recv, cur);
			else if (ty == "function_definition") r = searchCppParams(n, src, recv);
			else if (ty == "for_range_loop")      r = searchCppForRange(n, src, recv);
			else if (ty == "for_statement")       r = searchCppForInit(n, src, recv);
			else if (ty == "class_specifier" || ty == "struct_specifier" || ty == "union_specifier")
				r = searchCppClassMembers(n, src, recv);
		}
		else if (lang == Lang::CSharp)
		{
			if (ty == "block")                    r = searchCsBlock(n, src, recv, cur);
			else if (ty == "method_declaration" || ty == "constructor_declaration" ||
					 ty == "local_function_statement" || ty == "lambda_expression" ||
					 ty == "accessor_declaration") r = searchCsParams(n, src, recv);
			else if (ty == "foreach_statement")   r = searchCsForeach(n, src, recv);
			else if (ty == "class_declaration" || ty == "struct_declaration" ||
					 ty == "record_declaration" || ty == "interface_declaration")
				r = searchCsClassMembers(n, src, recv);
		}
		if (!r.empty()) return r;
	}
	return {};
}

// Single-entry parse cache. Keyed by a VALUE COPY of the source (never the
// pointer of the GetText() temporary — that would be a use-after-free). The size
// short-circuit avoids a full string compare on a miss.
struct ResolveCache
{
	Lang        lang = Lang::None;
	std::string src;
	TSTree*     tree = nullptr;
	~ResolveCache() { if (tree) ts_tree_delete(tree); }
	TSTree* get(Lang l, const std::string& s)
	{
		if (tree && lang == l && src.size() == s.size() && src == s)
			return tree;
		if (tree) { ts_tree_delete(tree); tree = nullptr; }
		const TSLanguage* language = languageFor(l);
		if (!language) return nullptr;
		TSParser* parser = ts_parser_new();
		ts_parser_set_language(parser, language);
		tree = ts_parser_parse_string(parser, nullptr, s.c_str(), (uint32_t) s.size());
		ts_parser_delete(parser);
		lang = l;
		src = s;   // own copy so next call's value-compare is safe
		return tree;
	}
};
ResolveCache sResolveCache;

} // namespace

std::string resolveLocalType(Lang lang, const std::string& source,
							 int line, int col, const std::string& receiver)
{
	if (lang == Lang::None || source.empty() || receiver.empty()) return {};
	if (source.size() > 512 * 1024) return {};                 // keystroke-path size cap
	if (receiver[0] >= '0' && receiver[0] <= '9') return {};   // numeric receiver

	TSTree* tree = sResolveCache.get(lang, source);
	if (!tree) return {};
	TSNode root = ts_tree_root_node(tree);

	TSPoint pt;
	pt.row    = (uint32_t) (line < 0 ? 0 : line);
	pt.column = (uint32_t) (col  < 0 ? 0 : col);
	TSNode cursor = ts_node_descendant_for_point_range(root, pt, pt);
	if (ts_node_is_null(cursor)) cursor = root;

	if (receiver == "this")
		return lang == Lang::Cpp ? resolveThisCpp(cursor, source) : resolveThisCs(cursor, source);

	return walkScopes(lang, cursor, source, receiver, pt);
}

} // namespace ts
