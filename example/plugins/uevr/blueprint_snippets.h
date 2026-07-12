//
//  blueprint_snippets.h — macro-style snippets for the UEVR Blueprint editor.
//
//  Unlike templates (which replace the whole graph), a snippet INSERTS a small
//  cluster of pre-wired nodes into the CURRENT graph at the view cursor, so you
//  can drop a loop/branch pattern into an existing script and keep working. See
//  BlueprintSnippets::All.
//

#pragma once

#include <string>
#include <vector>

class BlueprintEditor;

namespace BlueprintSnippets
{
struct Snippet
{
    std::string name;        // menu label
    std::string description; // one-line tooltip
    void (*insert)(BlueprintEditor &editor); // adds nodes at NextSpawnPos (no clear)
};

const std::vector<Snippet> &All();
} // namespace BlueprintSnippets
