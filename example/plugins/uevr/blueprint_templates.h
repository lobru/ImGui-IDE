//
//  blueprint_templates.h — starter graphs for the UEVR Blueprint editor.
//
//  A handful of ready-made graphs demonstrating common UEVR patterns (tick
//  logging, an on-screen ImGui panel, reading the player pawn). Each one
//  populates a BlueprintEditor from scratch so a user can start from a working
//  example instead of a blank canvas and adapt it — see BlueprintTemplates::All.
//

#pragma once

#include <string>
#include <vector>

class BlueprintEditor;

namespace BlueprintTemplates
{
struct Template
{
    std::string name;             // menu label
    std::string description;      // one-line tooltip
    void (*build)(BlueprintEditor &editor); // replaces the graph with this template
};

// The built-in templates, in menu order.
const std::vector<Template> &All();
} // namespace BlueprintTemplates
