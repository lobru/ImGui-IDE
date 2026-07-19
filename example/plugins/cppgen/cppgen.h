//
//  cppgen.h — lightweight, lexical C++ member-function code generation.
//
//  No ImGui / editor dependencies so the headless selftest can link it. All
//  functions are pure string transforms:
//
//    * parseMemberDecl      — parse one (already line-joined) member declaration
//    * memberDefinition     — out-of-line definition stub for a parsed member
//    * enclosingClass       — class/struct whose body encloses a given line
//    * generateOneDefinition— clicked declaration line → single definition stub
//    * generateClassDefinitions — every declared-but-not-defined member → stubs
//    * declarationFromDefinition — reverse: "R C::m(a) const {" → "R m(a) const;"
//
//  The parser is deliberately lexical (like the include / log resolvers): it
//  covers the common real-world shapes — normal methods, ctors/dtors, const /
//  noexcept / ref-qualifiers, virtual/static/explicit, pure/defaulted/deleted,
//  trailing return types, templates in return/params, operator overloads and
//  default arguments — and degrades gracefully on the exotic rest.
//
#pragma once

#include <string>
#include <vector>

namespace cppgen
{
    struct MemberDecl
    {
        std::string returnType; // empty for ctor/dtor/conversion operator
        std::string name;       // "foo", "~Widget", "operator=", "operator int"
        std::string params;     // param list WITHOUT parens, default args stripped
        std::string trailing;   // qualifiers kept on the definition: "const", "noexcept", "&", "-> T"...
        bool isCtorOrDtor = false;
        bool isPureVirtual = false; // = 0
        bool isDeleted = false;     // = delete
        bool isDefaulted = false;   // = default
        bool isStatic = false;
        bool isConstexpr = false; // constexpr/consteval — usually must stay inline
        bool isFriend = false;
        bool hasInlineBody = false; // already { ... } in the declaration
        bool valid = false;         // parsed as a function declaration
    };

    // Parse a single member function declaration (line-joined). Returns
    // valid=false for anything that isn't a function declaration (data members,
    // using/typedef, access specifiers, nested types, ...).
    MemberDecl parseMemberDecl(const std::string &declText);

    // Out-of-line definition stub for one member, qualified with className.
    // Returns "" for members that can't/shouldn't get one (inline body,
    // pure-virtual, defaulted, deleted, friend). Ends with a trailing newline.
    std::string memberDefinition(const std::string &className, const MemberDecl &m);

    // Name of the class/struct whose body encloses `line` (0-based) in `text`,
    // or "" if `line` is not inside a class body. Skips strings/comments.
    std::string enclosingClass(const std::string &text, int line);

    // Clicked declaration → its single definition stub. `line` is 0-based and
    // may point at the first line of a multi-line declaration; the declaration
    // is joined forward until balanced. Returns "" if the line is not a
    // generatable member declaration. On success, *classNameOut (if non-null)
    // gets the enclosing class name.
    std::string generateOneDefinition(const std::string &text, int line, std::string *classNameOut);

    // Every declared-but-not-defined member of the class body containing `line`
    // → concatenated definition stubs (blank line between them). Skips inline,
    // pure-virtual, defaulted, deleted, friend, and constexpr members. Returns
    // "" if no class body contains `line`. *classNameOut gets the class name.
    std::string generateClassDefinitions(const std::string &text, int line, std::string *classNameOut);

    // Reverse: an out-of-line definition ("R Class::method(args) const {" or a
    // full definition body) → the in-class declaration "R method(args) const;".
    // Returns "" if `defText` isn't a qualified out-of-line definition.
    std::string declarationFromDefinition(const std::string &defText);
}
