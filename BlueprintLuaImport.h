//	BlueprintLuaImport - turns hand-written UEVR Lua source into a BlueprintEditor graph.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


#pragma once


//
//	Include files
//

#include <string>

#include "BlueprintEditor.h"


//
//	BlueprintLuaImport
//
//	The inverse of BlueprintLua::GenerateScript: a best-effort STRUCTURAL importer, not a
//	Lua parser. It recognizes a handful of statement shapes (event/custom-event wrappers,
//	simple assignment, if/for/while, and calls matching a registry Function's metadata
//	template) and turns each into the corresponding node. Anything it doesn't recognize --
//	whole statements, or entire unrecognized blocks -- is wrapped VERBATIM in a Custom Lua
//	node spliced into the same position in the exec chain, so import can never drop or
//	corrupt source text; it just falls back to less decomposition.
//
//	Explicitly not solved (absorbed by the fallback): full Lua expression-precedence
//	grammar, metatables, coroutines, goto/labels, multiple-return/varargs unpacking,
//	closures beyond the two recognized wrapper shapes, and function-call ARGUMENTS that are
//	themselves calls (only literals and known bare identifiers resolve as arguments -- a
//	one-level-deep target/condition CAN be a matched call, but its own arguments cannot).
//	An `if` block followed by more statements in the same body also falls back whole,
//	since this editor's Branch node has no unified "continue either way" exec pin.
//

namespace BlueprintLuaImport {
	// Populates `editor`'s graph from `source` (replacing its current graph, like
	// LoadFromString). Returns false only on truly unrecoverable input (e.g. unbalanced
	// block/bracket nesting that prevents even statement splitting), with a human-readable
	// message in `error`; almost everything else succeeds via the verbatim fallback.
	bool ImportScript(BlueprintEditor& editor, const std::string& source, std::string& error);
}
