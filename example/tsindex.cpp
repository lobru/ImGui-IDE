//	Tree-sitter-backed symbol extraction — implementation.

#include "tsindex.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <sstream>

#include <tree_sitter/api.h>

// Grammar entry points (defined in the generated parser.c files).
extern "C" const TSLanguage* tree_sitter_cpp(void);

#ifndef TS_CPP_TAGS_SCM
#define TS_CPP_TAGS_SCM ""
#endif

namespace ts {

namespace {

// Read the grammar's tags.scm once and augment it. The stock query only tags
// out-of-line definitions with a SINGLE namespace scope (ns::bar), missing
// nested ones (ns::Foo::bar) — i.e. the actual method body go-to-def wants. The
// extra patterns capture any qualified definition's final name, plus struct/
// class data members (for member-aware autocomplete later). Same-node double
// captures are removed by position dedup in extractSymbols.
const std::string& cppTagsQuery()
{
	static std::string query = [] {
		std::ifstream f(TS_CPP_TAGS_SCM);
		std::string base;
		if (f.is_open()) {
			std::stringstream ss;
			ss << f.rdbuf();
			base = ss.str();
		}
		if (base.empty()) return base;
		base +=
			"\n"
			"(function_definition declarator: (function_declarator declarator: (qualified_identifier) @name)) @definition.method\n"
			"(field_declaration declarator: (field_identifier) @name) @definition.field\n"
			"(enumerator name: (identifier) @name) @definition.constant\n";
		return base;
	}();
	return query;
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

} // namespace

Lang langForExtension(const std::string& extIn)
{
	std::string ext = extIn;
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char) std::tolower(c); });
	if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c" ||
		ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh" || ext == ".inl")
		return Lang::Cpp;
	return Lang::None;
}

bool available()
{
	return tree_sitter_cpp() != nullptr;
}

std::vector<Symbol> extractSymbols(Lang lang, const std::string& source)
{
	std::vector<Symbol> out;
	if (lang != Lang::Cpp || source.empty())
		return out;

	const std::string& queryStr = cppTagsQuery();
	if (queryStr.empty())
		return out;

	const TSLanguage* language = tree_sitter_cpp();
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

	uint32_t errOffset = 0;
	TSQueryError errType = TSQueryErrorNone;
	TSQuery* query = ts_query_new(language, queryStr.c_str(), (uint32_t) queryStr.size(), &errOffset, &errType);
	if (!query)
	{
		ts_tree_delete(tree);
		ts_parser_delete(parser);
		return out;
	}

	TSNode root = ts_tree_root_node(tree);
	TSQueryCursor* cursor = ts_query_cursor_new();
	ts_query_cursor_exec(cursor, query, root);

	std::vector<uint64_t> seen;   // (row<<20 | col) keys, to drop same-node double captures

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
					sym.name = source.substr(s, e - s);
					// Qualified out-of-line names (ns::Foo::bar) → last segment.
					if (auto pos = sym.name.rfind("::"); pos != std::string::npos)
						sym.name = sym.name.substr(pos + 2);
					sym.line = (int) p.row;
					sym.column = (int) p.column;
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
			if (std::find(seen.begin(), seen.end(), key) == seen.end())
			{
				seen.push_back(key);
				out.push_back(std::move(sym));
			}
		}
	}

	ts_query_cursor_delete(cursor);
	ts_query_delete(query);
	ts_tree_delete(tree);
	ts_parser_delete(parser);
	return out;
}

} // namespace ts
