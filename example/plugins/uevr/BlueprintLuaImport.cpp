//	BlueprintLuaImport - turns hand-written UEVR Lua source into a BlueprintEditor graph.
//
//	This work is licensed under the terms of the MIT license.
//	For a copy, see <https://opensource.org/licenses/MIT>.


//
//	Include files
//

#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "BlueprintLuaImport.h"


//
//	Local helpers
//

using PinKind = BlueprintEditor::PinKind;
using PinType = BlueprintEditor::PinType;
using ID = BlueprintEditor::ID;

namespace {

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool isBlankChar(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool isBlank(const std::string& text) {
	for (auto c : text) {
		if (!isBlankChar(c)) {
			return false;
		}
	}
	return true;
}

std::string trim(const std::string& text) {
	size_t a = text.find_first_not_of(" \t\r\n");

	if (a == std::string::npos) {
		return "";
	}

	size_t b = text.find_last_not_of(" \t\r\n");
	return text.substr(a, b - a + 1);
}

bool startsWith(const std::string& text, const std::string& prefix) {
	return text.compare(0, prefix.size(), prefix) == 0;
}

bool isBareIdentifier(const std::string& text) {
	if (text.empty() || !isIdentStart(text[0])) {
		return false;
	}

	for (auto c : text) {
		if (!isIdentChar(c)) {
			return false;
		}
	}

	return true;
}


//
//	Statement splitting: find newline positions where block-depth (if/for/while/function
//	vs end) and bracket-depth ((),{},[]) are both zero, treating strings/comments as
//	opaque spans so their contents never interfere with the count. This deliberately does
//	NOT try to recognize a bare standalone "do ... end" block (rare, and not one of the
//	recognized shapes below) -- because splits are just byte-range boundaries and every
//	byte of the source ends up in exactly one span either way, a miscount there can only
//	ever shift where a span boundary falls, never drop or duplicate text.
//

std::vector<size_t> findTopLevelSplits(const std::string& src) {
	std::vector<size_t> splits;
	int blockDepth = 0;
	int bracketDepth = 0;
	size_t i = 0;

	while (i < src.size()) {
		char c = src[i];

		// line comment / long comment
		if (c == '-' && i + 1 < src.size() && src[i + 1] == '-') {
			size_t j = i + 2;

			if (j < src.size() && src[j] == '[') {
				size_t k = j + 1;
				int eq = 0;

				while (k < src.size() && src[k] == '=') {
					eq++;
					k++;
				}

				if (k < src.size() && src[k] == '[') {
					std::string close = "]" + std::string(static_cast<size_t>(eq), '=') + "]";
					size_t end = src.find(close, k + 1);
					i = (end == std::string::npos) ? src.size() : end + close.size();
					continue;
				}
			}

			size_t end = src.find('\n', i);
			i = (end == std::string::npos) ? src.size() : end;
			continue;
		}

		// long string [[...]] / [=[...]=]
		if (c == '[') {
			size_t k = i + 1;
			int eq = 0;

			while (k < src.size() && src[k] == '=') {
				eq++;
				k++;
			}

			if (k < src.size() && src[k] == '[') {
				std::string close = "]" + std::string(static_cast<size_t>(eq), '=') + "]";
				size_t end = src.find(close, k + 1);
				i = (end == std::string::npos) ? src.size() : end + close.size();
				continue;
			}
		}

		// quoted string
		if (c == '"' || c == '\'') {
			char q = c;
			i++;

			while (i < src.size() && src[i] != q) {
				if (src[i] == '\\' && i + 1 < src.size()) {
					i++;
				}

				i++;
			}

			i++;
			continue;
		}

		if (c == '(' || c == '{' || c == '[') {
			bracketDepth++;
			i++;
			continue;
		}

		if (c == ')' || c == '}' || c == ']') {
			bracketDepth = std::max(0, bracketDepth - 1);
			i++;
			continue;
		}

		if (isIdentStart(c)) {
			size_t start = i;

			while (i < src.size() && isIdentChar(src[i])) {
				i++;
			}

			std::string word = src.substr(start, i - start);

			if (word == "if" || word == "for" || word == "while" || word == "function") {
				blockDepth++;

			} else if (word == "end") {
				blockDepth = std::max(0, blockDepth - 1);
			}

			continue;
		}

		if (c == '\n') {
			if (blockDepth == 0 && bracketDepth == 0) {
				splits.push_back(i);
			}

			i++;
			continue;
		}

		i++;
	}

	return splits;
}

// Finds the first occurrence of `keyword` as a whole-word token at block-depth 0 within
// `src` (same string/comment-skipping and if/for/while/function-vs-end depth tracking as
// findTopLevelSplits, just hunting for a keyword instead of newlines). Used to locate an
// if-statement's own top-level "else"/"elseif" without being fooled by a nested if's.
size_t findTopLevelKeyword(const std::string& src, const char* keyword) {
	int blockDepth = 0;
	size_t i = 0;
	size_t kwLen = std::strlen(keyword);

	while (i < src.size()) {
		char c = src[i];

		if (c == '-' && i + 1 < src.size() && src[i + 1] == '-') {
			size_t j = i + 2;

			if (j < src.size() && src[j] == '[') {
				size_t k = j + 1;
				int eq = 0;

				while (k < src.size() && src[k] == '=') {
					eq++;
					k++;
				}

				if (k < src.size() && src[k] == '[') {
					std::string close = "]" + std::string(static_cast<size_t>(eq), '=') + "]";
					size_t end = src.find(close, k + 1);
					i = (end == std::string::npos) ? src.size() : end + close.size();
					continue;
				}
			}

			size_t end = src.find('\n', i);
			i = (end == std::string::npos) ? src.size() : end;
			continue;
		}

		if (c == '[') {
			size_t k = i + 1;
			int eq = 0;

			while (k < src.size() && src[k] == '=') {
				eq++;
				k++;
			}

			if (k < src.size() && src[k] == '[') {
				std::string close = "]" + std::string(static_cast<size_t>(eq), '=') + "]";
				size_t end = src.find(close, k + 1);
				i = (end == std::string::npos) ? src.size() : end + close.size();
				continue;
			}
		}

		if (c == '"' || c == '\'') {
			char q = c;
			i++;

			while (i < src.size() && src[i] != q) {
				if (src[i] == '\\' && i + 1 < src.size()) {
					i++;
				}

				i++;
			}

			i++;
			continue;
		}

		if (isIdentStart(c)) {
			size_t start = i;

			while (i < src.size() && isIdentChar(src[i])) {
				i++;
			}

			std::string word = src.substr(start, i - start);

			if (blockDepth == 0 && word.size() == kwLen && word == keyword) {
				return start;
			}

			if (word == "if" || word == "for" || word == "while" || word == "function") {
				blockDepth++;

			} else if (word == "end") {
				blockDepth = std::max(0, blockDepth - 1);
			}

			continue;
		}

		i++;
	}

	return std::string::npos;
}

// Splits `src` into trimmed, non-empty top-level statement spans.
std::vector<std::string> splitStatements(const std::string& src) {
	auto splits = findTopLevelSplits(src);
	std::vector<std::string> result;
	size_t start = 0;

	auto flush = [&](size_t end) {
		std::string piece = trim(src.substr(start, end - start));

		// a comment-only line (this splitter only isolates one as its own span when
		// nothing else shares the line, so a leading "--" here can't be a trailing
		// comment after real code) carries no runtime meaning -- drop it rather than
		// spawn a pointless fallback node for it
		if (!piece.empty() && !startsWith(piece, "--")) {
			result.push_back(piece);
		}

		start = end + 1;
	};

	for (auto pos : splits) {
		flush(pos);
	}

	flush(src.size());
	return result;
}

// Removes the generator's own safety scaffolding from a statement list so imports of
// GENERATED scripts decompose into real nodes again: the __bp_gen counter lines, the
// per-callback generation guard, and the per-callback pcall shell (whose INNER body
// is spliced back in as ordinary statements). Hand-written code is left untouched.
std::vector<std::string> stripGeneratorScaffolding(std::vector<std::string> statements) {
	std::vector<std::string> result;

	for (auto& stmt : statements) {
		if (stmt == "__bp_gen = (__bp_gen or 0) + 1" || stmt == "local __gen = __bp_gen" ||
			stmt == "if __gen ~= __bp_gen then return end") {
			continue;
		}

		if (startsWith(stmt, "if not __ok then") && stmt.back() == 'd') {
			continue; // the pcall error-report line
		}

		const std::string shellOpen = "local __ok, __err = pcall(function()";

		if (startsWith(stmt, shellOpen) && stmt.size() > shellOpen.size() + 4 &&
			stmt.compare(stmt.size() - 4, 4, "end)") == 0) {
			std::string inner = stmt.substr(shellOpen.size(), stmt.size() - shellOpen.size() - 4);

			for (auto& unwrapped : stripGeneratorScaffolding(splitStatements(inner))) {
				result.push_back(std::move(unwrapped));
			}

			continue;
		}

		result.push_back(std::move(stmt));
	}

	return result;
}


//
//	Template shape matching: the inverse of Generator::substituteTokens (BlueprintLua.cpp)
//	-- turns a "{target}:is_a({0})"-style metadata template into literal delimiters, then
//	matches candidate call text against them to recover the captured substrings. Only the
//	first '|'-segment is used (see BlueprintLua.cpp's Generator::emitNode -- that's the
//	only segment an impure/statement-shaped call ever actually evaluates).
//

struct TemplateShape {
	std::vector<std::string> literals;    // N+1 literal segments around N captures
	std::vector<std::string> captureKeys; // "target" or a decimal index, per capture
};

TemplateShape parseTemplateShape(const std::string& tmpl) {
	std::string first = tmpl.substr(0, tmpl.find('|'));
	TemplateShape shape;
	std::string literal;
	size_t i = 0;

	while (i < first.size()) {
		if (first[i] == '{') {
			size_t close = first.find('}', i);

			if (close == std::string::npos) {
				literal += first[i++];
				continue;
			}

			shape.literals.push_back(literal);
			literal.clear();
			shape.captureKeys.push_back(first.substr(i + 1, close - i - 1));
			i = close + 1;
			continue;
		}

		literal += first[i++];
	}

	shape.literals.push_back(literal);
	return shape;
}

std::optional<std::vector<std::string>> matchShape(const TemplateShape& shape, const std::string& text) {
	if (shape.captureKeys.empty()) {
		return (text == shape.literals[0]) ? std::optional<std::vector<std::string>>(std::vector<std::string>{}) : std::nullopt;
	}

	if (text.rfind(shape.literals[0], 0) != 0) {
		return std::nullopt;
	}

	size_t pos = shape.literals[0].size();
	std::vector<std::string> captures;

	for (size_t c = 0; c < shape.captureKeys.size(); c++) {
		const std::string& nextLiteral = shape.literals[c + 1];
		size_t end;

		if (!nextLiteral.empty()) {
			end = text.find(nextLiteral, pos);

			if (end == std::string::npos) {
				return std::nullopt;
			}

		} else if (c + 1 == shape.captureKeys.size()) {
			end = text.size(); // last capture, nothing trailing: take the rest

		} else {
			// two adjacent captures with no separating literal: not present in this
			// template corpus by construction -- bail out rather than guess a split
			return std::nullopt;
		}

		captures.push_back(text.substr(pos, end - pos));
		pos = end + nextLiteral.size();
	}

	if (pos != text.size()) {
		return std::nullopt; // trailing text after the last literal
	}

	return captures;
}


//
//	Importer
//

class Importer {
public:
	explicit Importer(BlueprintEditor& e) : editor(e), registry(e.GetRegistry()) { buildTemplateIndex(); }

	void run(const std::vector<std::string>& topLevelStatements);

private:
	struct EventWrapperMatch {
		std::string className, eventName, args, body;
	};

	struct TemplateEntry {
		std::string className;
		const BlueprintEditor::Function* function = nullptr;
		TemplateShape shape;
	};

	struct Resolved {
		bool isLiteral = false;
		std::string literalText;
		ID sourcePin = 0;
	};

	struct CallMatch {
		std::string className;
		const BlueprintEditor::Function* function = nullptr;
		bool hasTarget = false;
		Resolved target;
		std::vector<Resolved> args;
	};

	BlueprintEditor& editor;
	const BlueprintEditor::TypeRegistry& registry;
	std::vector<TemplateEntry> templates;
	std::unordered_map<std::string, PinType> knownVariables;
	float layoutY = 0.0f;

	struct Scope {
		std::unordered_map<std::string, ID> valuePins; // event-arg / loop-index name -> its data output pin
	};

	void buildTemplateIndex();
	ImVec2 nextPos(float depthX);

	bool tryVariableDecl(const std::string& stmt);
	std::optional<EventWrapperMatch> tryEventWrapper(const std::string& stmt) const;
	std::optional<std::pair<std::string, std::string>> tryCustomEvent(const std::string& stmt) const;

	std::optional<Resolved> resolveArgument(const std::string& text, Scope& scope);
	std::optional<CallMatch> tryMatchCall(const std::string& text, Scope& scope);
	std::optional<Resolved> resolveExpression(const std::string& text, Scope& scope);

	ID addCustomLuaFallback(const std::string& text, ID entryPin, float depthX);
	ID wireCall(const CallMatch& match, ID entryPin, float depthX); // returns the created node's id (0 on failure)
	ID processStatement(const std::string& stmt, ID entryPin, Scope& scope, float depthX);
	ID processBody(const std::vector<std::string>& statements, ID entryPin, Scope& scope, float depthX);

	bool looksLikeIfBlock(const std::string& stmt) const;
	bool looksLikeForBlock(const std::string& stmt) const;
	bool looksLikeWhileBlock(const std::string& stmt) const;
};

void Importer::buildTemplateIndex() {
	for (auto& cls : registry.GetClasses()) {
		// the generic "Call (N Args)" escape hatches match almost anything positionally
		// (they're deliberately unconstrained wildcard args) -- excluding them from
		// reverse-matching avoids spurious matches that would out-compete a real,
		// specific template for the same call text
		bool isEscapeHatchClass = cls->name == "ImGui" || cls->name == "UObject";

		for (auto& fn : cls->functions) {
			if (fn.metadata.empty()) {
				continue;
			}

			if (isEscapeHatchClass && startsWith(fn.name, "Call (")) {
				continue;
			}

			TemplateEntry entry;
			entry.className = cls->name;
			entry.function = &fn;
			entry.shape = parseTemplateShape(fn.metadata);
			templates.push_back(std::move(entry));
		}
	}
}

ImVec2 Importer::nextPos(float depthX) {
	layoutY += 190.0f;
	return ImVec2(depthX, layoutY);
}

bool Importer::tryVariableDecl(const std::string& stmt) {
	// "local NAME = EXPR" at script scope, not "local function ..."
	if (!startsWith(stmt, "local ") && !startsWith(stmt, "local\t")) {
		return false;
	}

	std::string rest = trim(stmt.substr(5));

	if (startsWith(rest, "function")) {
		return false;
	}

	size_t eq = rest.find('=');

	if (eq == std::string::npos) {
		return false;
	}

	std::string name = trim(rest.substr(0, eq));

	if (!isBareIdentifier(name)) {
		return false;
	}

	std::string value = trim(rest.substr(eq + 1));
	PinType type(PinKind::Wildcard);
	std::string defaultValue;

	if (value == "true" || value == "false") {
		type = PinType(PinKind::Boolean);
		defaultValue = value;

	} else if (!value.empty() && (std::isdigit(static_cast<unsigned char>(value[0])) || value[0] == '-')) {
		type = PinType(value.find('.') != std::string::npos ? PinKind::Float : PinKind::Integer);
		defaultValue = value;

	} else if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
		type = PinType(PinKind::String);
		defaultValue = value.substr(1, value.size() - 2);

	} else if (value == "nil") {
		type = PinType(PinKind::Wildcard);

	} else {
		return false; // an initializer that isn't a simple literal -- not a safe variable decl match
	}

	if (!editor.AddVariable(name, type, defaultValue)) {
		return false;
	}

	knownVariables[name] = type;
	return true;
}

std::optional<Importer::EventWrapperMatch> Importer::tryEventWrapper(const std::string& stmt) const {
	size_t fnPos = stmt.find("function");

	if (fnPos == std::string::npos) {
		return std::nullopt;
	}

	size_t callParen = stmt.rfind('(', fnPos);

	if (callParen == std::string::npos) {
		return std::nullopt;
	}

	std::string callbackPath = trim(stmt.substr(0, callParen));

	if (!isBlank(stmt.substr(callParen + 1, fnPos - callParen - 1))) {
		return std::nullopt;
	}

	size_t paramOpen = stmt.find('(', fnPos);

	if (paramOpen == std::string::npos) {
		return std::nullopt;
	}

	size_t paramClose = stmt.find(')', paramOpen);

	if (paramClose == std::string::npos) {
		return std::nullopt;
	}

	size_t lastClose = stmt.find_last_of(')');

	if (lastClose == std::string::npos || lastClose <= paramClose) {
		return std::nullopt;
	}

	if (!trim(stmt.substr(lastClose + 1)).empty()) {
		return std::nullopt;
	}

	size_t endKw = stmt.rfind("end", lastClose);

	if (endKw == std::string::npos || endKw <= paramClose) {
		return std::nullopt;
	}

	if (!isBlank(stmt.substr(endKw + 3, lastClose - endKw - 3))) {
		return std::nullopt;
	}

	// resolve the callback path to a registered (className, eventName) pair
	for (auto& cls : registry.GetClasses()) {
		for (auto& ev : cls->events) {
			if (ev.metadata != callbackPath) {
				continue;
			}

			EventWrapperMatch match;
			match.className = cls->name;
			match.eventName = ev.name;
			match.args = trim(stmt.substr(paramOpen + 1, paramClose - paramOpen - 1));
			match.body = stmt.substr(paramClose + 1, endKw - paramClose - 1);
			return match;
		}
	}

	return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> Importer::tryCustomEvent(const std::string& stmt) const {
	if (!startsWith(stmt, "local")) {
		return std::nullopt;
	}

	size_t p = 5;

	while (p < stmt.size() && isBlankChar(stmt[p])) {
		p++;
	}

	if (stmt.compare(p, 8, "function") != 0) {
		return std::nullopt;
	}

	p += 8;

	while (p < stmt.size() && isBlankChar(stmt[p])) {
		p++;
	}

	size_t nameStart = p;

	while (p < stmt.size() && isIdentChar(stmt[p])) {
		p++;
	}

	std::string name = stmt.substr(nameStart, p - nameStart);

	if (name.empty()) {
		return std::nullopt;
	}

	while (p < stmt.size() && isBlankChar(stmt[p])) {
		p++;
	}

	if (p >= stmt.size() || stmt[p] != '(') {
		return std::nullopt;
	}

	size_t parenClose = stmt.find(')', p);

	if (parenClose == std::string::npos || !isBlank(stmt.substr(p + 1, parenClose - p - 1))) {
		return std::nullopt; // zero-arg only, matching AddCustomEventNode
	}

	std::string tail = trim(stmt.substr(parenClose + 1));

	if (tail.size() < 3 || tail.compare(tail.size() - 3, 3, "end") != 0) {
		return std::nullopt;
	}

	return std::make_pair(name, tail.substr(0, tail.size() - 3));
}

std::optional<Importer::Resolved> Importer::resolveArgument(const std::string& text, Scope& scope) {
	std::string t = trim(text);

	if (t.empty()) {
		return std::nullopt;
	}

	if (t == "true" || t == "false" || t == "nil" ||
		(t.front() == '-' && t.size() > 1 && std::isdigit(static_cast<unsigned char>(t[1]))) ||
		std::isdigit(static_cast<unsigned char>(t.front())) ||
		((t.front() == '"' || t.front() == '\'') && t.size() >= 2 && t.back() == t.front())) {
		Resolved r;
		r.isLiteral = true;
		r.literalText = t;
		return r;
	}

	if (isBareIdentifier(t)) {
		auto sv = scope.valuePins.find(t);

		if (sv != scope.valuePins.end()) {
			Resolved r;
			r.sourcePin = sv->second;
			return r;
		}

		if (knownVariables.count(t)) {
			ID node = editor.AddVariableGetNode(t, nextPos(-400.0f));
			ID outPin = editor.FindPinID(node, "", true);

			if (outPin != 0) {
				Resolved r;
				r.sourcePin = outPin;
				return r;
			}
		}
	}

	return std::nullopt; // too complex for a single argument (nested call, operator, ...)
}

std::optional<Importer::CallMatch> Importer::tryMatchCall(const std::string& text, Scope& scope) {
	for (auto& entry : templates) {
		auto captures = matchShape(entry.shape, text);

		if (!captures) {
			continue;
		}

		CallMatch match;
		match.className = entry.className;
		match.function = entry.function;
		bool ok = true;

		for (size_t i = 0; i < entry.shape.captureKeys.size() && ok; i++) {
			auto resolved = resolveArgument((*captures)[i], scope);

			if (!resolved) {
				ok = false;
				break;
			}

			if (entry.shape.captureKeys[i] == "target") {
				match.hasTarget = true;
				match.target = *resolved;

			} else {
				match.args.push_back(*resolved);
			}
		}

		if (ok) {
			return match;
		}
	}

	return std::nullopt;
}

std::optional<Importer::Resolved> Importer::resolveExpression(const std::string& text, Scope& scope) {
	if (auto simple = resolveArgument(text, scope)) {
		return simple;
	}

	// one level of call-matching: the condition/RHS itself may be a matched call, but that
	// call's OWN arguments only resolve as literals/known identifiers (resolveArgument) --
	// chained/nested calls inside arguments are an explicit non-goal, see the header comment
	auto call = tryMatchCall(trim(text), scope);

	if (!call) {
		return std::nullopt;
	}

	ID node = wireCall(*call, 0, -400.0f);

	if (node == 0) {
		return std::nullopt;
	}

	ID pin = editor.FindPinID(node, "Return Value", true);

	if (pin == 0) {
		// a multi-output pure function used in a single-value context: fall back to its
		// first declared data output
		if (auto* n = editor.GetNode(node)) {
			for (auto& p : n->pins) {
				if (p.isOutput && p.type.kind != PinKind::Exec) {
					pin = p.id;
					break;
				}
			}
		}
	}

	if (pin == 0) {
		return std::nullopt;
	}

	Resolved r;
	r.sourcePin = pin;
	return r;
}

ID Importer::addCustomLuaFallback(const std::string& text, ID entryPin, float depthX) {
	ID node = editor.AddCustomLuaNode(nextPos(depthX));
	editor.SetCustomLuaSource(node, text);
	ID execIn = editor.FindPinID(node, "", false);
	ID execOut = editor.FindPinID(node, "", true);

	if (entryPin != 0 && execIn != 0) {
		editor.AddLink(entryPin, execIn);
	}

	return execOut;
}

ID Importer::wireCall(const CallMatch& match, ID entryPin, float depthX) {
	ID node = editor.AddCallFunctionNode(match.className, match.function->name,
		nextPos(match.function->isPure ? depthX - 400.0f : depthX));

	if (node == 0) {
		return 0;
	}

	if (match.hasTarget) {
		ID targetPin = editor.FindPinID(node, "Target", false);

		if (targetPin != 0) {
			if (match.target.isLiteral) {
				editor.SetPinDefaultValue(targetPin, match.target.literalText);

			} else if (match.target.sourcePin != 0) {
				editor.AddLink(match.target.sourcePin, targetPin);
			}
		}
	}

	for (size_t i = 0; i < match.args.size() && i < match.function->parameters.size(); i++) {
		if (match.function->parameters[i].isOutput) {
			continue;
		}

		ID argPin = editor.FindPinID(node, match.function->parameters[i].name, false);

		if (argPin == 0) {
			continue;
		}

		if (match.args[i].isLiteral) {
			editor.SetPinDefaultValue(argPin, match.args[i].literalText);

		} else if (match.args[i].sourcePin != 0) {
			editor.AddLink(match.args[i].sourcePin, argPin);
		}
	}

	if (!match.function->isPure) {
		ID execIn = editor.FindPinID(node, "", false);

		if (entryPin != 0 && execIn != 0) {
			editor.AddLink(entryPin, execIn);
		}
	}

	return node;
}

bool Importer::looksLikeIfBlock(const std::string& stmt) const {
	return startsWith(stmt, "if ") || startsWith(stmt, "if\t") || startsWith(stmt, "if(");
}

bool Importer::looksLikeForBlock(const std::string& stmt) const {
	return startsWith(stmt, "for ") || startsWith(stmt, "for\t");
}

bool Importer::looksLikeWhileBlock(const std::string& stmt) const {
	return startsWith(stmt, "while ") || startsWith(stmt, "while\t");
}

ID Importer::processStatement(const std::string& stmt, ID entryPin, Scope& scope, float depthX) {
	// if/then/[else/]end
	if (looksLikeIfBlock(stmt) && stmt.size() > 3) {
		size_t thenPos = stmt.find(" then");
		size_t lastEnd = stmt.rfind("end");

		if (thenPos != std::string::npos && lastEnd != std::string::npos && lastEnd > thenPos && trim(stmt.substr(lastEnd + 3)).empty()) {
			std::string cond = trim(stmt.substr(3, thenPos - 3));
			auto resolved = resolveExpression(cond, scope);

			// an elseif chain has no single "False" branch this Branch node can represent
			// cleanly -- bail out to the verbatim fallback rather than mis-decompose it
			bool hasElseif = findTopLevelKeyword(stmt.substr(thenPos + 5, lastEnd - (thenPos + 5)), "elseif") != std::string::npos;

			if (resolved && (resolved->isLiteral || resolved->sourcePin != 0) && !hasElseif) {
				std::string body = stmt.substr(thenPos + 5, lastEnd - (thenPos + 5));
				std::string trueBody = body, falseBody;
				size_t elsePos = findTopLevelKeyword(body, "else");

				if (elsePos != std::string::npos) {
					trueBody = body.substr(0, elsePos);
					falseBody = body.substr(elsePos + 4);
				}

				ID node = editor.AddFlowControlNode("Branch", nextPos(depthX));
				ID execIn = editor.FindPinID(node, "", false);
				ID condPin = editor.FindPinID(node, "Condition", false);
				ID truePin = editor.FindPinID(node, "True", true);
				ID falsePin = editor.FindPinID(node, "False", true);

				if (entryPin != 0 && execIn != 0) {
					editor.AddLink(entryPin, execIn);
				}

				if (condPin != 0) {
					if (resolved->isLiteral) {
						editor.SetPinDefaultValue(condPin, resolved->literalText);

					} else {
						editor.AddLink(resolved->sourcePin, condPin);
					}
				}

				Scope trueScope = scope, falseScope = scope;
				processBody(splitStatements(trueBody), truePin, trueScope, depthX + 400.0f);

				if (!falseBody.empty()) {
					processBody(splitStatements(falseBody), falsePin, falseScope, depthX + 400.0f);
				}

				return node; // caller only uses this as "non-zero" to know a node exists; see processBody
			}
		}
	}

	// for VAR = A, B do ... end
	if (looksLikeForBlock(stmt)) {
		size_t doPos = stmt.rfind(" do");
		size_t lastEnd = stmt.rfind("end");

		if (doPos != std::string::npos && lastEnd != std::string::npos && lastEnd > doPos && trim(stmt.substr(lastEnd + 3)).empty()) {
			std::string header = trim(stmt.substr(4, doPos - 4));
			size_t eq = header.find('=');
			size_t comma = header.find(',');

			if (eq != std::string::npos && comma != std::string::npos && comma > eq) {
				std::string varName = trim(header.substr(0, eq));
				std::string firstExpr = trim(header.substr(eq + 1, comma - eq - 1));
				std::string lastExpr = trim(header.substr(comma + 1));
				auto first = resolveExpression(firstExpr, scope);
				auto last = resolveExpression(lastExpr, scope);

				if (isBareIdentifier(varName) && first && last) {
					ID node = editor.AddFlowControlNode("For Loop", nextPos(depthX));
					ID execIn = editor.FindPinID(node, "", false);
					ID firstPin = editor.FindPinID(node, "First Index", false);
					ID lastPin = editor.FindPinID(node, "Last Index", false);
					ID loopBodyPin = editor.FindPinID(node, "Loop Body", true);
					ID indexPin = editor.FindPinID(node, "Index", true);
					ID completedPin = editor.FindPinID(node, "Completed", true);

					if (entryPin != 0 && execIn != 0) {
						editor.AddLink(entryPin, execIn);
					}

					if (firstPin != 0) {
						first->isLiteral ? (void)editor.SetPinDefaultValue(firstPin, first->literalText)
										  : (void)editor.AddLink(first->sourcePin, firstPin);
					}

					if (lastPin != 0) {
						last->isLiteral ? (void)editor.SetPinDefaultValue(lastPin, last->literalText)
										 : (void)editor.AddLink(last->sourcePin, lastPin);
					}

					std::string body = stmt.substr(doPos + 3, lastEnd - (doPos + 3));
					Scope loopScope = scope;

					if (indexPin != 0) {
						loopScope.valuePins[varName] = indexPin;
					}

					processBody(splitStatements(body), loopBodyPin, loopScope, depthX + 400.0f);
					return completedPin;
				}
			}
		}
	}

	// while COND do ... end
	if (looksLikeWhileBlock(stmt)) {
		size_t doPos = stmt.rfind(" do");
		size_t lastEnd = stmt.rfind("end");

		if (doPos != std::string::npos && lastEnd != std::string::npos && lastEnd > doPos && trim(stmt.substr(lastEnd + 3)).empty()) {
			std::string cond = trim(stmt.substr(6, doPos - 6));
			auto resolved = resolveExpression(cond, scope);

			if (resolved) {
				ID node = editor.AddFlowControlNode("While Loop", nextPos(depthX));
				ID execIn = editor.FindPinID(node, "", false);
				ID condPin = editor.FindPinID(node, "Condition", false);
				ID loopBodyPin = editor.FindPinID(node, "Loop Body", true);
				ID completedPin = editor.FindPinID(node, "Completed", true);

				if (entryPin != 0 && execIn != 0) {
					editor.AddLink(entryPin, execIn);
				}

				if (condPin != 0) {
					resolved->isLiteral ? (void)editor.SetPinDefaultValue(condPin, resolved->literalText)
										 : (void)editor.AddLink(resolved->sourcePin, condPin);
				}

				std::string body = stmt.substr(doPos + 3, lastEnd - (doPos + 3));
				Scope loopScope = scope;
				processBody(splitStatements(body), loopBodyPin, loopScope, depthX + 400.0f);
				return completedPin;
			}
		}
	}

	// VARNAME = EXPR  (assignment to an already-known variable)
	{
		size_t eq = stmt.find('=');

		if (eq != std::string::npos && (eq + 1 >= stmt.size() || stmt[eq + 1] != '=') && (eq == 0 || stmt[eq - 1] != '=')) {
			std::string name = trim(stmt.substr(0, eq));

			if (isBareIdentifier(name) && knownVariables.count(name)) {
				auto resolved = resolveExpression(trim(stmt.substr(eq + 1)), scope);

				if (resolved) {
					ID node = editor.AddVariableSetNode(name, nextPos(depthX));
					ID execIn = editor.FindPinID(node, "", false);
					ID execOut = editor.FindPinID(node, "", true);
					ID valuePin = editor.FindPinID(node, name, false);

					if (entryPin != 0 && execIn != 0) {
						editor.AddLink(entryPin, execIn);
					}

					if (valuePin != 0) {
						resolved->isLiteral ? (void)editor.SetPinDefaultValue(valuePin, resolved->literalText)
											 : (void)editor.AddLink(resolved->sourcePin, valuePin);
					}

					return execOut;
				}
			}
		}
	}

	// a bare call matching a registered Function's template
	{
		auto call = tryMatchCall(stmt, scope);

		if (call && !call->function->isPure) {
			ID node = wireCall(*call, entryPin, depthX);
			ID execOut = (node != 0) ? editor.FindPinID(node, "", true) : 0;

			if (execOut != 0) {
				return execOut;
			}
			// wireCall succeeded creating the node but something about wiring the exec
			// pin failed (shouldn't happen for a non-pure CallFunction node) -- fall
			// through to the verbatim fallback rather than return a dead-end 0 that
			// would silently sever the rest of the exec chain
		}
	}

	return addCustomLuaFallback(stmt, entryPin, depthX);
}

// splits a comma-separated parameter list into trimmed names, ignoring blanks
std::vector<std::string> splitParamNames(const std::string& list) {
	std::vector<std::string> names;
	size_t start = 0;

	while (start <= list.size()) {
		size_t comma = list.find(',', start);
		std::string name = trim(list.substr(start, (comma == std::string::npos ? list.size() : comma) - start));

		if (!name.empty()) {
			names.push_back(name);
		}

		if (comma == std::string::npos) {
			break;
		}

		start = comma + 1;
	}

	return names;
}

void Importer::run(const std::vector<std::string>& topLevelStatements) {
	ID looseChainExec = 0; // lazily-created catch-all CustomEvent's running exec-out
	Scope looseScope;

	for (auto& stmt : topLevelStatements) {
		if (tryVariableDecl(stmt)) {
			continue;
		}

		if (auto ev = tryEventWrapper(stmt)) {
			ID node = editor.AddEventNode(ev->className, ev->eventName, nextPos(0.0f));

			if (node == 0) {
				looseChainExec = addCustomLuaFallback(stmt, looseChainExec, 400.0f);
				continue;
			}

			ID execOut = editor.FindPinID(node, "", true);

			// map the script's own chosen parameter names to the event's declared data
			// output pins POSITIONALLY (the script author's names rarely match the
			// registry's "Engine"/"Delta Seconds"-style display names)
			std::vector<ID> dataOutputs;

			if (auto* n = editor.GetNode(node)) {
				for (auto& pin : n->pins) {
					if (pin.isOutput && pin.type.kind != PinKind::Exec) {
						dataOutputs.push_back(pin.id);
					}
				}
			}

			Scope eventScope;
			auto argNames = splitParamNames(ev->args);

			for (size_t i = 0; i < argNames.size() && i < dataOutputs.size(); i++) {
				eventScope.valuePins[argNames[i]] = dataOutputs[i];
			}

			processBody(stripGeneratorScaffolding(splitStatements(ev->body)), execOut, eventScope, 400.0f);
			continue;
		}

		if (auto ce = tryCustomEvent(stmt)) {
			ID node = editor.AddCustomEventNode(ce->first, nextPos(0.0f));
			ID execOut = editor.FindPinID(node, "", true);
			Scope customScope;
			processBody(stripGeneratorScaffolding(splitStatements(ce->second)), execOut, customScope, 400.0f);
			continue;
		}

		// A loose top-level statement (written outside any event) has no natural home in
		// this exec-graph model -- GenerateScript itself never emits bare top-level
		// statements, only variable decls and event registrations. Rather than drop it,
		// collect all such statements into one synthetic catch-all Custom Event so the
		// data is always visible in the graph. Re-generating won't auto-run this code the
		// way the original top-level statement did (it now has to be triggered like any
		// other custom event) -- a known, documented import-only limitation.
		if (looseChainExec == 0) {
			ID node = editor.AddCustomEventNode("ImportedTopLevel", nextPos(0.0f));
			looseChainExec = editor.FindPinID(node, "", true);
		}

		looseChainExec = processStatement(stmt, looseChainExec, looseScope, 400.0f);
	}
}

ID Importer::processBody(const std::vector<std::string>& statements, ID entryPin, Scope& scope, float depthX) {
	ID current = entryPin;

	for (size_t i = 0; i < statements.size(); i++) {
		bool isLast = (i + 1 == statements.size());

		// An if-block followed by more statements can't be safely decomposed: this
		// editor's Branch node has no unified "continue either way" exec pin, so wiring
		// a downstream node from both True and False would need multi-source fan-in this
		// importer doesn't assume works. Fold the if-block and everything after it into
		// one verbatim Custom Lua node instead of guessing.
		if (!isLast && looksLikeIfBlock(statements[i])) {
			std::string tail;

			for (size_t j = i; j < statements.size(); j++) {
				if (!tail.empty()) {
					tail += "\n";
				}

				tail += statements[j];
			}

			return addCustomLuaFallback(tail, current, depthX);
		}

		ID result = processStatement(statements[i], current, scope, depthX);

		if (result != 0) {
			current = result;
		}
		// result == 0 happens for a Branch whose id we returned as a sentinel (non-exec)
		// or a pure/no-op case; `current` intentionally stays put in those cases since
		// there is nothing further to chain from at this level (the Branch's own
		// True/False sub-bodies already got their own continuations wired above).
	}

	return current;
}

} // namespace


//
//	BlueprintLuaImport::ImportScript
//

bool BlueprintLuaImport::ImportScript(BlueprintEditor& editor, const std::string& source, std::string& error) {
	auto topLevel = stripGeneratorScaffolding(splitStatements(source));

	if (topLevel.empty() && !trim(source).empty()) {
		error = "could not find any top-level statements (unbalanced block or bracket nesting?)";
		return false;
	}

	editor.ClearGraph();

	Importer importer(editor);
	importer.run(topLevel);

	error.clear();
	return true;
}
