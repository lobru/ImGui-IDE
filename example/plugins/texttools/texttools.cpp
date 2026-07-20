//
//  texttools.cpp — pure text transforms (see texttools.h).
//

#include "texttools.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <nlohmann/json.hpp>

using ojson = nlohmann::ordered_json;

namespace texttools {

// ── JSON ────────────────────────────────────────────────────────────────

static ojson parseJson(const std::string& in, std::string& err)
{
	err.clear();
	ojson j = ojson::parse(in, nullptr, /*allow_exceptions*/ false);
	if (j.is_discarded())
		err = "invalid JSON";
	return j;
}

std::string jsonPretty(const std::string& in, std::string& err)
{
	ojson j = parseJson(in, err);
	return err.empty() ? j.dump(2) : std::string();
}

std::string jsonMinify(const std::string& in, std::string& err)
{
	ojson j = parseJson(in, err);
	return err.empty() ? j.dump() : std::string();
}

static void xmlEscape(const std::string& s, std::string& out)
{
	for (char c : s)
	{
		switch (c)
		{
			case '&': out += "&amp;"; break;
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			case '"': out += "&quot;"; break;
			default: out += c; break;
		}
	}
}

// A JSON key isn't necessarily a valid XML element name — sanitize the easy way.
static std::string xmlName(const std::string& key)
{
	std::string n;
	for (char c : key)
		n += (std::isalnum((unsigned char) c) || c == '_' || c == '-' || c == '.') ? c : '_';
	if (n.empty() || std::isdigit((unsigned char) n[0]))
		n = "_" + n;
	return n;
}

static void jsonNodeToXml(const std::string& name, const ojson& v, std::string& out, int depth)
{
	std::string ind((size_t) depth * 2, ' ');
	if (v.is_object())
	{
		out += ind + "<" + name + ">\n";
		for (auto it = v.begin(); it != v.end(); ++it)
			jsonNodeToXml(xmlName(it.key()), it.value(), out, depth + 1);
		out += ind + "</" + name + ">\n";
	}
	else if (v.is_array())
	{
		// Repeat the element once per entry, like most JSON<->XML mappings.
		for (const auto& item : v)
			jsonNodeToXml(name, item, out, depth);
	}
	else
	{
		std::string text = v.is_string() ? v.get<std::string>() : v.dump();
		out += ind + "<" + name + ">";
		xmlEscape(text, out);
		out += "</" + name + ">\n";
	}
}

std::string jsonToXml(const std::string& in, std::string& err)
{
	ojson j = parseJson(in, err);
	if (!err.empty())
		return {};
	std::string out;
	jsonNodeToXml("root", j, out, 0);
	return out;
}

// ── Minimal XML reader (elements, attributes, text; skips <?...?> <!-- -->) ──

namespace {
struct XmlCursor
{
	const std::string& s;
	size_t i = 0;
	bool fail = false;

	void skipWs()
	{
		while (i < s.size() && std::isspace((unsigned char) s[i]))
			++i;
	}
	bool starts(const char* lit) const
	{
		return s.compare(i, std::strlen(lit), lit) == 0;
	}
	void skipMisc()
	{
		for (;;)
		{
			skipWs();
			if (starts("<?"))
			{
				auto e = s.find("?>", i);
				if (e == std::string::npos) { fail = true; return; }
				i = e + 2;
			}
			else if (starts("<!--"))
			{
				auto e = s.find("-->", i);
				if (e == std::string::npos) { fail = true; return; }
				i = e + 3;
			}
			else if (starts("<!"))
			{
				auto e = s.find('>', i);
				if (e == std::string::npos) { fail = true; return; }
				i = e + 1;
			}
			else
				return;
		}
	}
};

std::string xmlUnescape(const std::string& t)
{
	std::string out;
	for (size_t i = 0; i < t.size();)
	{
		if (t[i] == '&')
		{
			if (t.compare(i, 5, "&amp;") == 0) { out += '&'; i += 5; continue; }
			if (t.compare(i, 4, "&lt;") == 0) { out += '<'; i += 4; continue; }
			if (t.compare(i, 4, "&gt;") == 0) { out += '>'; i += 4; continue; }
			if (t.compare(i, 6, "&quot;") == 0) { out += '"'; i += 6; continue; }
			if (t.compare(i, 6, "&apos;") == 0) { out += '\''; i += 6; continue; }
		}
		out += t[i++];
	}
	return out;
}

// If the text looks like a number/bool/null, store it typed; else as a string.
ojson typedText(const std::string& t)
{
	if (t == "true") return true;
	if (t == "false") return false;
	if (t == "null") return nullptr;
	if (!t.empty())
	{
		char* end = nullptr;
		double d = std::strtod(t.c_str(), &end);
		if (end && *end == '\0')
		{
			if (d == (long long) d && t.find_first_of(".eE") == std::string::npos)
				return (long long) d;
			return d;
		}
	}
	return t;
}

// Insert child under key; repeated names become arrays.
void addChild(ojson& obj, const std::string& key, ojson value)
{
	auto it = obj.find(key);
	if (it == obj.end())
		obj[key] = std::move(value);
	else if (it->is_array())
		it->push_back(std::move(value));
	else
	{
		ojson arr = ojson::array();
		arr.push_back(std::move(*it));
		arr.push_back(std::move(value));
		obj[key] = std::move(arr);
	}
}

ojson parseElement(XmlCursor& c, std::string& tagOut);

// Parse the content between <tag ...> and </tag> into a json value.
ojson parseContent(XmlCursor& c, const std::string& tag)
{
	ojson obj = ojson::object();
	std::string text;
	for (;;)
	{
		if (c.i >= c.s.size()) { c.fail = true; return obj; }
		if (c.starts("</"))
		{
			auto e = c.s.find('>', c.i);
			if (e == std::string::npos) { c.fail = true; return obj; }
			std::string close = c.s.substr(c.i + 2, e - c.i - 2);
			while (!close.empty() && std::isspace((unsigned char) close.back()))
				close.pop_back();
			if (close != tag)
				c.fail = true;
			c.i = e + 1;
			break;
		}
		if (c.s[c.i] == '<')
		{
			c.skipMisc();
			if (c.fail) return obj;
			if (c.s[c.i] != '<' || c.starts("</"))
				continue;
			std::string childTag;
			ojson child = parseElement(c, childTag);
			if (c.fail) return obj;
			addChild(obj, childTag, std::move(child));
		}
		else
		{
			auto e = c.s.find('<', c.i);
			if (e == std::string::npos) { c.fail = true; return obj; }
			text += c.s.substr(c.i, e - c.i);
			c.i = e;
		}
	}
	// Trim whitespace-only text.
	size_t a = text.find_first_not_of(" \t\r\n");
	size_t b = text.find_last_not_of(" \t\r\n");
	std::string trimmed = a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
	if (obj.empty())
		return trimmed.empty() ? ojson(nullptr) : typedText(xmlUnescape(trimmed));
	if (!trimmed.empty())
		obj["#text"] = typedText(xmlUnescape(trimmed));
	return obj;
}

ojson parseElement(XmlCursor& c, std::string& tagOut)
{
	// c.i at '<'
	++c.i;
	size_t start = c.i;
	while (c.i < c.s.size() && !std::isspace((unsigned char) c.s[c.i]) &&
	       c.s[c.i] != '>' && c.s[c.i] != '/')
		++c.i;
	tagOut = c.s.substr(start, c.i - start);
	if (tagOut.empty()) { c.fail = true; return nullptr; }

	ojson obj = ojson::object();
	// Attributes.
	for (;;)
	{
		c.skipWs();
		if (c.i >= c.s.size()) { c.fail = true; return obj; }
		if (c.starts("/>")) { c.i += 2; return obj.empty() ? ojson(nullptr) : obj; }
		if (c.s[c.i] == '>') { ++c.i; break; }
		size_t as = c.i;
		while (c.i < c.s.size() && c.s[c.i] != '=' && !std::isspace((unsigned char) c.s[c.i]) &&
		       c.s[c.i] != '>' && c.s[c.i] != '/')
			++c.i;
		std::string aname = c.s.substr(as, c.i - as);
		c.skipWs();
		if (c.i < c.s.size() && c.s[c.i] == '=')
		{
			++c.i;
			c.skipWs();
			if (c.i < c.s.size() && (c.s[c.i] == '"' || c.s[c.i] == '\''))
			{
				char q = c.s[c.i++];
				size_t vs = c.i;
				auto e = c.s.find(q, vs);
				if (e == std::string::npos) { c.fail = true; return obj; }
				obj["@" + aname] = typedText(xmlUnescape(c.s.substr(vs, e - vs)));
				c.i = e + 1;
				continue;
			}
			c.fail = true;
			return obj;
		}
		if (!aname.empty())
			obj["@" + aname] = true; // bare attribute
	}

	ojson content = parseContent(c, tagOut);
	if (c.fail) return obj;
	if (obj.empty())
		return content;
	// Merge attributes with element content.
	if (content.is_object())
	{
		for (auto it = content.begin(); it != content.end(); ++it)
			addChild(obj, it.key(), it.value());
	}
	else if (!content.is_null())
		obj["#text"] = std::move(content);
	return obj;
}
} // namespace

std::string xmlToJson(const std::string& in, std::string& err)
{
	err.clear();
	XmlCursor c{in};
	c.skipMisc();
	if (c.fail || c.i >= in.size() || in[c.i] != '<')
	{
		err = "invalid XML";
		return {};
	}
	std::string tag;
	ojson content = parseElement(c, tag);
	if (c.fail)
	{
		err = "invalid XML";
		return {};
	}
	ojson root = ojson::object();
	root[tag] = std::move(content);
	return root.dump(2);
}

// ── Lines ───────────────────────────────────────────────────────────────

std::string sortLines(const std::string& in, bool numeric, bool ascending)
{
	bool trailingNl = !in.empty() && in.back() == '\n';
	std::vector<std::string> lines;
	std::string cur;
	for (char ch : in)
	{
		if (ch == '\n')
		{
			if (!cur.empty() && cur.back() == '\r')
				cur.pop_back();
			lines.push_back(std::move(cur));
			cur.clear();
		}
		else
			cur += ch;
	}
	if (!cur.empty())
		lines.push_back(std::move(cur));

	auto firstNumber = [](const std::string& l, double& out) {
		const char* p = l.c_str();
		while (*p && !(std::isdigit((unsigned char) *p) ||
		               ((*p == '-' || *p == '+' || *p == '.') && std::isdigit((unsigned char) p[1]))))
			++p;
		if (!*p)
			return false;
		char* end = nullptr;
		out = std::strtod(p, &end);
		return end != p;
	};

	std::stable_sort(lines.begin(), lines.end(), [&](const std::string& a, const std::string& b) {
		if (numeric)
		{
			double na, nb;
			bool ha = firstNumber(a, na), hb = firstNumber(b, nb);
			if (ha && hb && na != nb)
				return na < nb;
			if (ha != hb)
				return ha; // numbered lines before unnumbered
		}
		return a < b;
	});
	if (!ascending)
		std::reverse(lines.begin(), lines.end());

	std::string out;
	for (size_t i = 0; i < lines.size(); ++i)
	{
		out += lines[i];
		if (i + 1 < lines.size() || trailingNl)
			out += '\n';
	}
	return out;
}

// ── Case ────────────────────────────────────────────────────────────────

// Tokenize on separators AND camelCase boundaries; words come back lowercase.
static std::vector<std::string> splitWords(const std::string& in)
{
	std::vector<std::string> words;
	std::string cur;
	unsigned char prev = 0;   // previous ORIGINAL char (cur stores lowercase)
	auto flush = [&]() {
		if (!cur.empty())
		{
			words.push_back(cur);
			cur.clear();
		}
	};
	for (size_t i = 0; i < in.size(); ++i)
	{
		unsigned char ch = (unsigned char) in[i];
		if (!std::isalnum(ch))
		{
			flush();
			prev = 0;
			continue;
		}
		// lower->Upper or letter<->digit boundaries start a new word.
		if (!cur.empty())
		{
			bool boundary =
			    (std::islower(prev) && std::isupper(ch)) ||
			    (std::isalpha(prev) && std::isdigit(ch)) ||
			    (std::isdigit(prev) && std::isalpha(ch)) ||
			    // ABBRWord: keep "XML" together but split before "Parser" in XMLParser.
			    (std::isupper(prev) && std::isupper(ch) && i + 1 < in.size() &&
			     std::islower((unsigned char) in[i + 1]));
			if (boundary)
				flush();
		}
		cur += (char) std::tolower(ch);
		prev = ch;
	}
	flush();
	return words;
}

std::string convertCase(const std::string& in, Case mode)
{
	if (mode == Case::Upper || mode == Case::Lower)
	{
		std::string out = in;
		for (auto& c : out)
			c = mode == Case::Upper ? (char) std::toupper((unsigned char) c)
			                        : (char) std::tolower((unsigned char) c);
		return out;
	}
	if (mode == Case::Title)
	{
		// Structure-preserving: uppercase each letter that follows a non-letter.
		std::string out = in;
		bool newWord = true;
		for (auto& c : out)
		{
			if (std::isalpha((unsigned char) c))
			{
				c = newWord ? (char) std::toupper((unsigned char) c)
				            : (char) std::tolower((unsigned char) c);
				newWord = false;
			}
			else
				newWord = true;
		}
		return out;
	}

	auto words = splitWords(in);
	std::string out;
	if (mode == Case::Snake)
	{
		for (size_t i = 0; i < words.size(); ++i)
		{
			if (i)
				out += '_';
			out += words[i];
		}
	}
	else // Camel
	{
		for (size_t i = 0; i < words.size(); ++i)
		{
			if (i == 0)
				out += words[i];
			else
			{
				out += (char) std::toupper((unsigned char) words[i][0]);
				out += words[i].substr(1);
			}
		}
	}
	return out;
}

} // namespace texttools
