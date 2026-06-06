//	Tree-sitter-backed symbol extraction — implementation.

#include "tsindex.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <tree_sitter/api.h>

// Grammar entry points (defined in the generated parser.c files).
extern "C" const TSLanguage* tree_sitter_cpp(void);
extern "C" const TSLanguage* tree_sitter_c_sharp(void);

#ifndef TS_CPP_TAGS_SCM
#define TS_CPP_TAGS_SCM ""
#endif
#ifndef TS_CSHARP_TAGS_SCM
#define TS_CSHARP_TAGS_SCM ""
#endif

namespace ts {

namespace {

const TSLanguage* languageFor(Lang lang)
{
	switch (lang) {
		case Lang::Cpp:    return tree_sitter_cpp();
		case Lang::CSharp: return tree_sitter_c_sharp();
		default:           return nullptr;
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
	static const std::string none;

	switch (lang) {
		case Lang::Cpp:    return cpp;
		case Lang::CSharp: return csharp;
		default:           return none;
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
			ty == "enum_declaration" || ty == "record_declaration")
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

} // namespace ts
