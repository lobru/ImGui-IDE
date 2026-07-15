//
//  cppgen.cpp — see cppgen.h.
//
#include "cppgen.h"

#include <cctype>

namespace cppgen
{
    namespace
    {
        std::string trim(const std::string &s)
        {
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos)
                return {};
            size_t b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        }

        bool isIdentChar(char c)
        {
            return std::isalnum((unsigned char)c) || c == '_';
        }

        // Advance past a leading whole-word keyword; returns true and consumes it
        // (plus trailing spaces) if `s` starts with `kw` followed by a non-ident.
        bool eatKeyword(std::string &s, const char *kw)
        {
            size_t n = 0;
            while (kw[n])
                ++n;
            if (s.size() < n)
                return false;
            if (s.compare(0, n, kw) != 0)
                return false;
            if (s.size() > n && isIdentChar(s[n]))
                return false;
            s.erase(0, n);
            s = trim(s);
            return true;
        }

        // Strip a leading [[...]] attribute if present.
        bool eatAttribute(std::string &s)
        {
            if (s.size() < 2 || s[0] != '[' || s[1] != '[')
                return false;
            int depth = 0;
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '[')
                    ++depth;
                else if (s[i] == ']')
                {
                    --depth;
                    if (depth == 0)
                    {
                        s.erase(0, i + 1);
                        s = trim(s);
                        return true;
                    }
                }
            }
            return false;
        }

        // Index of the matching ')' for the '(' at `open`, honouring nested
        // (), <>, [], {} and skipping string/char literals. -1 if unbalanced.
        int matchParen(const std::string &s, int open)
        {
            int depth = 0;
            for (int i = open; i < (int)s.size(); ++i)
            {
                char c = s[i];
                if (c == '"' || c == '\'')
                {
                    char q = c;
                    ++i;
                    while (i < (int)s.size() && s[i] != q)
                    {
                        if (s[i] == '\\')
                            ++i;
                        ++i;
                    }
                    continue;
                }
                if (c == '(')
                    ++depth;
                else if (c == ')')
                {
                    --depth;
                    if (depth == 0)
                        return i;
                }
            }
            return -1;
        }

        // Locate the parameter list's '(' .. ')'. Handles operator overloads,
        // whose name may itself contain "()" / "[]". Returns false on failure.
        bool findParamParens(const std::string &s, int &openOut, int &closeOut)
        {
            // operator special case: params are the parens AFTER the operator symbol.
            size_t scanFrom = 0;
            size_t opPos = std::string::npos;
            for (size_t i = 0; i + 8 <= s.size(); ++i)
            {
                if (s.compare(i, 8, "operator") == 0 &&
                    (i == 0 || !isIdentChar(s[i - 1])) &&
                    (i + 8 >= s.size() || !isIdentChar(s[i + 8])))
                {
                    opPos = i;
                    break;
                }
            }
            if (opPos != std::string::npos)
            {
                size_t j = opPos + 8;
                while (j < s.size() && (s[j] == ' ' || s[j] == '\t'))
                    ++j;
                // operator() and operator[] — the symbol is a bracket pair.
                if (j + 1 < s.size() && ((s[j] == '(' && s[j + 1] == ')') || (s[j] == '[' && s[j + 1] == ']')))
                    j += 2;
                else if (j < s.size() && (isIdentChar(s[j]) || s[j] == ':'))
                {
                    // conversion operator: operator Type — skip the type name.
                    while (j < s.size() && s[j] != '(')
                        ++j;
                }
                else
                {
                    // punctuation operator (+, ==, <=>, ->, ...). Skip the run.
                    const std::string punct = "+-*/%^&|~!=<>,.?";
                    while (j < s.size() && (punct.find(s[j]) != std::string::npos))
                        ++j;
                }
                scanFrom = j;
            }

            // First '(' at angle-bracket depth 0 (so a templated return type or a
            // trailing-return default isn't mistaken for the param list).
            int angle = 0;
            for (size_t i = scanFrom; i < s.size(); ++i)
            {
                char c = s[i];
                if (c == '<')
                    ++angle;
                else if (c == '>' && angle > 0)
                    --angle;
                else if (c == '(' && angle == 0)
                {
                    int close = matchParen(s, (int)i);
                    if (close < 0)
                        return false;
                    openOut = (int)i;
                    closeOut = close;
                    return true;
                }
            }
            return false;
        }

        // Strip top-level default arguments ("= expr") from a param list body,
        // preserving parameter names and types.
        std::string stripDefaults(const std::string &params)
        {
            std::string out;
            int depth = 0; // () [] {} <>
            bool skipping = false;
            for (size_t i = 0; i < params.size(); ++i)
            {
                char c = params[i];
                if (c == '(' || c == '[' || c == '{' || c == '<')
                    ++depth;
                else if (c == ')' || c == ']' || c == '}' || c == '>')
                    --depth;
                if (!skipping && c == '=' && depth == 0)
                {
                    // Not ==, <=, >=, != : those don't appear at depth 0 in a
                    // parameter type, and a real default starts a value.
                    skipping = true;
                    continue;
                }
                if (skipping)
                {
                    if (depth == 0 && c == ',')
                    {
                        skipping = false;
                        out += c;
                    }
                    continue;
                }
                out += c;
            }
            return trim(out);
        }

        // Parse the qualifiers after the parameter list into the pieces we keep
        // on a definition, and detect =0 / =default / =delete / inline body.
        void parseTrailing(const std::string &after, MemberDecl &m)
        {
            std::string s = trim(after);
            std::string keep;
            auto append = [&](const std::string &tok) {
                if (!keep.empty())
                    keep += ' ';
                keep += tok;
            };
            size_t i = 0;
            while (i < s.size())
            {
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
                    ++i;
                if (i >= s.size())
                    break;
                char c = s[i];
                if (c == '{')
                {
                    m.hasInlineBody = true;
                    break;
                }
                if (c == ';')
                    break;
                if (c == '=')
                {
                    ++i;
                    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
                        ++i;
                    if (i < s.size() && s[i] == '0')
                        m.isPureVirtual = true;
                    else if (s.compare(i, 7, "default") == 0)
                        m.isDefaulted = true;
                    else if (s.compare(i, 6, "delete") == 0)
                        m.isDeleted = true;
                    break;
                }
                if (c == '-' && i + 1 < s.size() && s[i + 1] == '>')
                {
                    // trailing return type — everything up to ; = {
                    size_t j = i + 2;
                    size_t end = s.size();
                    for (size_t k = j; k < s.size(); ++k)
                        if (s[k] == '{' || s[k] == ';' || (s[k] == '=' && (k == 0 || s[k - 1] != '>')))
                        {
                            end = k;
                            break;
                        }
                    append("-> " + trim(s.substr(j, end - j)));
                    i = end;
                    continue;
                }
                if (c == '&')
                {
                    if (i + 1 < s.size() && s[i + 1] == '&')
                    {
                        append("&&");
                        i += 2;
                    }
                    else
                    {
                        append("&");
                        ++i;
                    }
                    continue;
                }
                // a word: const / volatile / noexcept / override / final
                if (isIdentChar(c))
                {
                    size_t j = i;
                    while (j < s.size() && isIdentChar(s[j]))
                        ++j;
                    std::string word = s.substr(i, j - i);
                    i = j;
                    if (word == "const" || word == "volatile")
                        append(word);
                    else if (word == "noexcept")
                    {
                        std::string tok = "noexcept";
                        // optional noexcept(expr)
                        size_t k = i;
                        while (k < s.size() && (s[k] == ' ' || s[k] == '\t'))
                            ++k;
                        if (k < s.size() && s[k] == '(')
                        {
                            int close = matchParen(s, (int)k);
                            if (close > 0)
                            {
                                tok += s.substr(k, close - k + 1);
                                i = close + 1;
                            }
                        }
                        append(tok);
                    }
                    // override / final: intentionally dropped from the definition.
                    continue;
                }
                ++i; // skip anything unrecognised
            }
            m.trailing = keep;
        }
    } // namespace

    MemberDecl parseMemberDecl(const std::string &declText)
    {
        MemberDecl m;
        std::string s = trim(declText);
        if (s.empty())
            return m;

        // Strip leading access-specifier labels — the class-body walk glues the
        // label to the first member after it ("public:\n    Widget()").
        for (;;)
        {
            bool stripped = false;
            for (const char *lbl : {"public:", "private:", "protected:", "signals:", "slots:"})
            {
                size_t n = 0;
                while (lbl[n])
                    ++n;
                if (s.compare(0, n, lbl) == 0)
                {
                    s.erase(0, n);
                    s = trim(s);
                    stripped = true;
                    break;
                }
            }
            if (!stripped)
                break;
        }

        // Peel leading attributes and specifiers.
        for (;;)
        {
            if (eatAttribute(s))
                continue;
            if (eatKeyword(s, "virtual") || eatKeyword(s, "explicit") || eatKeyword(s, "inline") ||
                eatKeyword(s, "friend") || eatKeyword(s, "static") || eatKeyword(s, "constexpr") ||
                eatKeyword(s, "consteval") || eatKeyword(s, "force_inline") || eatKeyword(s, "FORCEINLINE"))
            {
                // Track the ones that change generation.
                continue;
            }
            break;
        }
        // Re-scan the ORIGINAL (trimmed) text for the flags we need — eatKeyword
        // above consumed them, so record via a cheap prefix check on the raw.
        {
            std::string raw = trim(declText);
            auto has = [&](const char *kw) {
                size_t n = 0;
                while (kw[n])
                    ++n;
                size_t p = raw.find(kw);
                while (p != std::string::npos)
                {
                    bool lok = (p == 0 || !isIdentChar(raw[p - 1]));
                    bool rok = (p + n >= raw.size() || !isIdentChar(raw[p + n]));
                    if (lok && rok)
                        return true;
                    p = raw.find(kw, p + 1);
                }
                return false;
            };
            m.isStatic = has("static");
            m.isConstexpr = has("constexpr") || has("consteval");
            m.isFriend = has("friend");
        }

        // Reject obvious non-functions early.
        if (s.rfind("using ", 0) == 0 || s.rfind("typedef ", 0) == 0 ||
            s.rfind("static_assert", 0) == 0 || s.rfind("public:", 0) == 0 ||
            s.rfind("private:", 0) == 0 || s.rfind("protected:", 0) == 0)
            return m;

        int po = 0, pc = 0;
        if (!findParamParens(s, po, pc))
            return m; // no parameter list → data member, macro, etc.

        std::string head = trim(s.substr(0, po));                 // return type + name
        std::string params = stripDefaults(trim(s.substr(po + 1, pc - po - 1)));
        std::string after = s.substr(pc + 1);

        if (head.empty())
            return m; // "(...)" with no callee — not a member decl

        // Split head into return type + name. The name is the trailing token,
        // except for operators (name = "operator" + symbol) and destructors.
        std::string retType, name;
        {
            size_t opPos = head.rfind("operator");
            bool opWord = opPos != std::string::npos &&
                          (opPos == 0 || !isIdentChar(head[opPos - 1]));
            if (opWord)
            {
                name = trim(head.substr(opPos));
                retType = trim(head.substr(0, opPos));
            }
            else
            {
                // Trailing identifier (may be preceded by '~' for a destructor).
                int e = (int)head.size();
                int b = e;
                while (b > 0 && isIdentChar(head[b - 1]))
                    --b;
                if (b == e)
                    return m; // doesn't end in an identifier
                if (b > 0 && head[b - 1] == '~')
                    --b;
                name = head.substr(b);
                retType = trim(head.substr(0, b));
            }
        }

        m.name = trim(name);
        m.returnType = retType;
        m.params = params;
        m.isCtorOrDtor = retType.empty() && m.name.rfind("operator", 0) != 0;
        parseTrailing(after, m);
        m.valid = !m.name.empty();
        return m;
    }

    std::string memberDefinition(const std::string &className, const MemberDecl &m)
    {
        if (!m.valid || m.hasInlineBody || m.isPureVirtual || m.isDeleted ||
            m.isDefaulted || m.isFriend)
            return {};
        std::string out;
        if (!m.returnType.empty())
            out += m.returnType + " ";
        out += className + "::" + m.name + "(" + m.params + ")";
        if (!m.trailing.empty())
            out += " " + m.trailing;
        out += "\n{\n}\n";
        return out;
    }

    // Scan `text` skipping string/char literals and //, /* */ comments; for each
    // class/struct, find its body braces and the innermost one enclosing the
    // char offset `pos`. Returns that class's name (and, via out-params, its
    // brace range) or "".
    static std::string enclosingClassImpl(const std::string &text, size_t pos,
                                          size_t *bodyOpen, size_t *bodyClose)
    {
        struct Frame
        {
            std::string name;
            size_t open;
            bool isClass;
        };
        std::vector<Frame> stack;
        std::string best;
        size_t bestOpen = 0, bestClose = 0;
        bool haveBest = false;

        // Pending class header seen but brace not yet opened.
        bool pendingClass = false;
        std::string pendingName;

        for (size_t i = 0; i < text.size(); ++i)
        {
            char c = text[i];
            // Skip literals / comments.
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
            {
                i = text.find('\n', i);
                if (i == std::string::npos)
                    break;
                continue;
            }
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
            {
                i = text.find("*/", i + 2);
                if (i == std::string::npos)
                    break;
                ++i;
                continue;
            }
            if (c == '"' || c == '\'')
            {
                char q = c;
                ++i;
                while (i < text.size() && text[i] != q)
                {
                    if (text[i] == '\\')
                        ++i;
                    ++i;
                }
                continue;
            }

            // Detect a class/struct header start at a word boundary.
            if ((c == 'c' || c == 's') && (i == 0 || !isIdentChar(text[i - 1])))
            {
                const char *kw = nullptr;
                size_t klen = 0;
                if (text.compare(i, 5, "class") == 0)
                {
                    kw = "class";
                    klen = 5;
                }
                else if (text.compare(i, 6, "struct") == 0)
                {
                    kw = "struct";
                    klen = 6;
                }
                if (kw && (i + klen >= text.size() || !isIdentChar(text[i + klen])))
                {
                    // Read the name; then require a '{' before ';' (else it's a
                    // forward decl or a variable of that type).
                    size_t j = i + klen;
                    while (j < text.size() && (text[j] == ' ' || text[j] == '\t' || text[j] == '\n' || text[j] == '\r'))
                        ++j;
                    // skip attributes / final / alignas etc up to name
                    size_t nb = j;
                    while (j < text.size() && isIdentChar(text[j]))
                        ++j;
                    std::string nm = text.substr(nb, j - nb);
                    // Look ahead for { vs ; at top level.
                    size_t k = j;
                    bool opens = false;
                    while (k < text.size())
                    {
                        char d = text[k];
                        if (d == ';')
                            break;
                        if (d == '{')
                        {
                            opens = true;
                            break;
                        }
                        ++k;
                    }
                    if (opens && !nm.empty() && nm != "final")
                    {
                        pendingClass = true;
                        pendingName = nm;
                    }
                    i = j - 1;
                    continue;
                }
            }

            if (c == '{')
            {
                if (pendingClass)
                {
                    stack.push_back({pendingName, i, true});
                    pendingClass = false;
                }
                else
                {
                    stack.push_back({"", i, false});
                }
            }
            else if (c == '}')
            {
                if (!stack.empty())
                {
                    Frame f = stack.back();
                    stack.pop_back();
                    if (f.isClass && pos > f.open && pos <= i)
                    {
                        // Innermost wins: prefer the tightest enclosing range.
                        if (!haveBest || (f.open >= bestOpen && i <= bestClose))
                        {
                            best = f.name;
                            bestOpen = f.open;
                            bestClose = i;
                            haveBest = true;
                        }
                    }
                }
            }
        }
        if (haveBest)
        {
            if (bodyOpen)
                *bodyOpen = bestOpen;
            if (bodyClose)
                *bodyClose = bestClose;
        }
        return best;
    }

    static size_t offsetOfLine(const std::string &text, int line)
    {
        if (line <= 0)
            return 0;
        size_t off = 0;
        int cur = 0;
        while (cur < line && off < text.size())
        {
            size_t nl = text.find('\n', off);
            if (nl == std::string::npos)
                return text.size();
            off = nl + 1;
            ++cur;
        }
        return off;
    }

    std::string enclosingClass(const std::string &text, int line)
    {
        size_t pos = offsetOfLine(text, line);
        return enclosingClassImpl(text, pos, nullptr, nullptr);
    }

    // Join from the char offset `start` forward until the parameter parens are
    // balanced and we hit a ';' or '{' at top level — the full declaration.
    static std::string joinDeclarationAt(const std::string &text, size_t start, size_t *endOut)
    {
        int depth = 0;
        bool sawParen = false;
        size_t i = start;
        for (; i < text.size(); ++i)
        {
            char c = text[i];
            if (c == '"' || c == '\'')
            {
                char q = c;
                ++i;
                while (i < text.size() && text[i] != q)
                {
                    if (text[i] == '\\')
                        ++i;
                    ++i;
                }
                continue;
            }
            if (c == '(')
            {
                ++depth;
                sawParen = true;
            }
            else if (c == ')')
                --depth;
            else if (depth == 0 && sawParen && (c == ';' || c == '{'))
            {
                ++i;
                break;
            }
        }
        if (endOut)
            *endOut = i;
        return text.substr(start, i - start);
    }

    std::string generateOneDefinition(const std::string &text, int line, std::string *classNameOut)
    {
        std::string cls = enclosingClass(text, line);
        if (cls.empty())
            return {};
        size_t start = offsetOfLine(text, line);
        // Skip leading blank lines / access labels to the declaration proper.
        std::string decl = joinDeclarationAt(text, start, nullptr);
        MemberDecl m = parseMemberDecl(decl);
        if (!m.valid)
            return {};
        std::string def = memberDefinition(cls, m);
        if (!def.empty() && classNameOut)
            *classNameOut = cls;
        return def;
    }

    std::string generateClassDefinitions(const std::string &text, int line, std::string *classNameOut)
    {
        size_t bodyOpen = 0, bodyClose = 0;
        size_t pos = offsetOfLine(text, line);
        std::string cls = enclosingClassImpl(text, pos, &bodyOpen, &bodyClose);
        if (cls.empty())
            return {};
        if (classNameOut)
            *classNameOut = cls;

        std::string out;
        // Walk the body at depth 1, cutting declarations at ';' and skipping any
        // nested { } (inline bodies, nested types). bodyOpen points at the '{'.
        size_t i = bodyOpen + 1;
        std::string cur;
        int depth = 0;
        auto flush = [&]() {
            std::string decl = trim(cur);
            cur.clear();
            if (decl.empty())
                return;
            MemberDecl m = parseMemberDecl(decl);
            if (!m.valid || m.isConstexpr)
                return;
            std::string def = memberDefinition(cls, m);
            if (!def.empty())
            {
                if (!out.empty())
                    out += "\n";
                out += def;
            }
            else if (m.isPureVirtual)
            {
                // A pure virtual has no out-of-line body to generate, but SILENTLY
                // dropping it made whole-class generation look like it "lost" the
                // virtual members. Leave a one-line marker so the override surface
                // is visible in the generated file.
                if (!out.empty())
                    out += "\n";
                out += "// " + (m.returnType.empty() ? std::string() : m.returnType + " ") +
                       m.name + "(" + m.params + ")" +
                       (m.trailing.empty() ? std::string() : " " + m.trailing) +
                       " = 0;  // pure virtual - override in a derived class\n";
            }
        };
        while (i < bodyClose)
        {
            char c = text[i];
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
            {
                size_t nl = text.find('\n', i);
                i = (nl == std::string::npos) ? bodyClose : nl;
                continue;
            }
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
            {
                size_t e = text.find("*/", i + 2);
                i = (e == std::string::npos) ? bodyClose : e + 2;
                continue;
            }
            if (c == '"' || c == '\'')
            {
                char q = c;
                cur += c;
                ++i;
                while (i < bodyClose && text[i] != q)
                {
                    if (text[i] == '\\')
                    {
                        cur += text[i++];
                    }
                    cur += text[i++];
                }
                if (i < bodyClose)
                    cur += text[i++];
                continue;
            }
            if (c == '{')
            {
                // Inline body or nested type: keep the '{' so parseMemberDecl
                // sees hasInlineBody, then skip the balanced block.
                cur += c;
                int d = 1;
                ++i;
                while (i < bodyClose && d > 0)
                {
                    if (text[i] == '{')
                        ++d;
                    else if (text[i] == '}')
                        --d;
                    ++i;
                }
                flush(); // an inline-defined member ends here
                continue;
            }
            if (c == ';' && depth == 0)
            {
                flush();
                ++i;
                continue;
            }
            cur += c;
            ++i;
        }
        flush();
        return out;
    }

    std::string declarationFromDefinition(const std::string &defText)
    {
        std::string s = trim(defText);
        // Cut off the body if present.
        int po = 0, pc = 0;
        if (!findParamParens(s, po, pc))
            return {};
        std::string head = trim(s.substr(0, po));
        std::string params = trim(s.substr(po + 1, pc - po - 1));
        std::string after = s.substr(pc + 1);

        // head must contain a qualified name "Class::method" (or "Class::~Class").
        size_t colons = head.rfind("::");
        if (colons == std::string::npos)
            return {};
        // return type = everything before the last top-level qualifier chain.
        // Find the start of the qualified-id (walk back over identifier / :: / ~).
        int b = (int)colons + 2;
        // include the method name after ::
        int e = (int)head.size();
        std::string methodName = trim(head.substr(b, e - b));
        // walk back from `colons` to the beginning of the qualified id
        int q = (int)colons;
        while (q > 0)
        {
            char ch = head[q - 1];
            // Walk back over the qualified-id only — NOT the space that separates
            // it from the return type (that would swallow the return type).
            if (isIdentChar(ch) || ch == ':' || ch == '~' || ch == '<' || ch == '>' || ch == ',')
                --q;
            else
                break;
        }
        std::string retType = trim(head.substr(0, q));

        MemberDecl m;
        m.returnType = retType;
        m.name = methodName;
        m.params = params;
        parseTrailing(after, m);

        std::string out;
        if (!m.returnType.empty())
            out += m.returnType + " ";
        out += m.name + "(" + m.params + ")";
        if (!m.trailing.empty())
            out += " " + m.trailing;
        out += ";";
        return out;
    }
}
