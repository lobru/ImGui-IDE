//
//  texttools.h — pure text transforms behind the Text Tools plugin.
//
//  All functions are string -> string with an `err` out-param ("" = success),
//  no ImGui/editor dependencies — selftest-covered directly.
//

#pragma once

#include <string>

namespace texttools {

// ── JSON ────────────────────────────────────────────────────────────────
std::string jsonPretty(const std::string& in, std::string& err);   // 2-space indent
std::string jsonMinify(const std::string& in, std::string& err);
// JSON -> XML: objects become elements, arrays repeat the element per item
// (<key>...</key> per entry), primitives become text. Root element <root>.
std::string jsonToXml(const std::string& in, std::string& err);
// XML -> JSON: elements become objects, attributes "@name", pure text becomes
// the value directly, repeated sibling names become arrays. Declarations and
// comments are skipped. Minimal by design — not a validating parser.
std::string xmlToJson(const std::string& in, std::string& err);

// ── Lines ───────────────────────────────────────────────────────────────
// Sort the lines of `in`. numeric: compare by the first number on each line
// (lines without one sort after, alphabetically). Preserves the presence or
// absence of a trailing newline.
std::string sortLines(const std::string& in, bool numeric, bool ascending);
// Remove duplicate lines, keeping each line's FIRST occurrence in place
// (no sorting). Preserves the trailing-newline state.
std::string uniqueLines(const std::string& in);
// Prefix every line with its 1-based number ("1. foo"). Width-padded so the
// numbers align. Preserves the trailing-newline state.
std::string numberLines(const std::string& in);

// ── Case ────────────────────────────────────────────────────────────────
enum class Case { Upper, Lower, Title, Camel, Snake };
// Upper/Lower/Title preserve the text's structure (per character / per word);
// Camel/Snake re-tokenize on separators AND camelCase boundaries, then join.
std::string convertCase(const std::string& in, Case mode);

} // namespace texttools
