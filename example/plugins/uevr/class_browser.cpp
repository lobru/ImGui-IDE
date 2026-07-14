//	Graphical class browser — a view of the imported reflection hierarchy.
//
//	The SDK index (sdkIndex) is a flat list of classes, each naming its parent. This
//	turns that into the TREE it really is, and makes it a way INTO the graph: click a
//	function or property and the matching node lands on the Blueprint canvas (lazily
//	exposing the class, exactly like the contextual member menus — so browsing 10k
//	classes still never floods the flat palette).
//
//	Names are folded through TypeRegistry::normalizeClassName before they're linked
//	up, because a dump says "Actor" where the built-in registry says "AActor"; without
//	folding, half the hierarchy would hang off phantom roots.

#include "uevr_plugin.h"

#include <algorithm>
#include <cctype>
#include <set>

void UevrPlugin::rebuildClassTree()
{
    classChildren.clear();
    classRoots.clear();

    using Registry = BlueprintEditor::TypeRegistry;

    // Every class we actually have, keyed by its folded name — a parent we don't
    // have a definition for isn't a node in the tree, it's a root boundary.
    std::set<std::string> known;

    for (const auto &cls : sdkIndex.GetClasses())
        known.insert(Registry::normalizeClassName(cls->name));

    for (const auto &cls : sdkIndex.GetClasses())
    {
        const std::string parent = Registry::normalizeClassName(cls->parentName);

        if (!cls->parentName.empty() && known.count(parent) &&
            parent != Registry::normalizeClassName(cls->name))
        {
            classChildren[parent].push_back(cls->name);
        }
        else
        {
            // no parent, or a parent whose definition isn't in the dump: it's a root
            classRoots.push_back(cls->name);
        }
    }

    auto byName = [](const std::string &a, const std::string &b) {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
                return std::tolower((unsigned char)x) < std::tolower((unsigned char)y);
            });
    };

    std::sort(classRoots.begin(), classRoots.end(), byName);

    for (auto &entry : classChildren)
        std::sort(entry.second.begin(), entry.second.end(), byName);
}

void UevrPlugin::renderClassBrowserNode(PluginHost &host, const std::string &className,
                                        const std::map<std::string, std::vector<std::string>> &children,
                                        int depth)
{
    using Registry = BlueprintEditor::TypeRegistry;

    // Depth guard: a malformed dump can name a parent cycle, and a cycle here is an
    // infinite recursion into the stack rather than a mildly wrong tree.
    if (depth > 64)
        return;

    const auto kids = children.find(Registry::normalizeClassName(className));
    const bool hasKids = kids != children.end() && !kids->second.empty();

    const BlueprintEditor::Class *cls = sdkIndex.FindClass(className);
    const bool hasMembers = cls && (!cls->functions.empty() || !cls->properties.empty());

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;

    if (!hasKids && !hasMembers)
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (classBrowserSelected == className)
        flags |= ImGuiTreeNodeFlags_Selected;

    const bool open = ImGui::TreeNodeEx(className.c_str(), flags, "%s", className.c_str());

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        classBrowserSelected = className;

    if (cls && !cls->tooltip.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", cls->tooltip.c_str());

    if (!open)
        return;

    if (cls)
    {
        BlueprintEditor &bp = ensureBlueprintEditor();

        if (!cls->properties.empty() && ImGui::TreeNodeEx("##props", ImGuiTreeNodeFlags_SpanAvailWidth,
                                                          "Properties (%d)", (int)cls->properties.size()))
        {
            for (const auto &prop : cls->properties)
            {
                ImGui::PushID(prop.name.c_str());
                if (ImGui::Selectable(prop.name.c_str()))
                {
                    bp.ensureClassAvailable(className);
                    bp.AddPropertyGetNode(className, prop.name, bp.NextSpawnPos());
                    blueprintVisible = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Click to drop a Get %s node on the canvas", prop.name.c_str());
                ImGui::PopID();
            }

            ImGui::TreePop();
        }

        if (!cls->functions.empty() && ImGui::TreeNodeEx("##funcs", ImGuiTreeNodeFlags_SpanAvailWidth,
                                                         "Functions (%d)", (int)cls->functions.size()))
        {
            for (const auto &fn : cls->functions)
            {
                ImGui::PushID(fn.name.c_str());
                if (ImGui::Selectable(fn.name.c_str()))
                {
                    bp.ensureClassAvailable(className);
                    bp.AddCallFunctionNode(className, fn.name, bp.NextSpawnPos());
                    blueprintVisible = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\nClick to drop a call node on the canvas",
                                      fn.tooltip.empty() ? fn.name.c_str() : fn.tooltip.c_str());
                ImGui::PopID();
            }

            ImGui::TreePop();
        }
    }

    if (hasKids)
    {
        for (const auto &child : kids->second)
            renderClassBrowserNode(host, child, children, depth + 1);
    }

    ImGui::TreePop();
}

void UevrPlugin::renderClassBrowser(PluginHost &host)
{
    if (!classBrowserVisible)
        return;

    ImGui::SetNextWindowSize(ImVec2(420, 560), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Class Browser (UEVR)", &classBrowserVisible))
    {
        ImGui::End();
        return;
    }

    if (sdkIndex.GetClasses().empty())
    {
        ImGui::TextWrapped("No SDK loaded.\n\nImport one from a running game (UEVR Live \xe2\x96\xb8 "
                           "Import SDK from Game), or drop a dump in the sdk/ folder next to the exe.");
        ImGui::End();
        return;
    }

    if (classRoots.empty() && classChildren.empty())
        rebuildClassTree();

    ImGui::SetNextItemWidth(-70.0f);
    ImGui::InputTextWithHint("##classFilter", "filter classes…", classBrowserFilter,
                             sizeof(classBrowserFilter));
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload"))
    {
        loadSdkDefinitions();
        rebuildClassTree();
    }

    ImGui::TextDisabled("%d classes", (int)sdkIndex.GetClasses().size());
    ImGui::Separator();

    std::string filter = classBrowserFilter;
    std::transform(filter.begin(), filter.end(), filter.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    ImGui::BeginChild("##classTree");
    host.hostMiddleMousePanScroll(108); // drag-scroll, routed through the host so it honors invert-pan

    if (filter.empty())
    {
        // the real hierarchy
        for (const auto &root : classRoots)
            renderClassBrowserNode(host, root, classChildren, 0);
    }
    else
    {
        // A filtered view is a FLAT list on purpose: showing a filtered tree would
        // either hide matches whose parents don't match, or drag in whole ancestor
        // chains that don't. A flat hit list is what you actually want when searching.
        int shown = 0;

        for (const auto &cls : sdkIndex.GetClasses())
        {
            std::string name = cls->name;
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });

            if (name.find(filter) == std::string::npos)
                continue;

            if (++shown > 200)
            {
                ImGui::TextDisabled("… more matches (narrow the filter)");
                break;
            }

            renderClassBrowserNode(host, cls->name, classChildren, 0);
        }

        if (shown == 0)
            ImGui::TextDisabled("No class matches \"%s\".", classBrowserFilter);
    }

    ImGui::EndChild();
    ImGui::End();
}
