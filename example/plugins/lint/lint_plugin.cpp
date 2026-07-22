//
//  lint_plugin.cpp — see lint_plugin.h.
//

#include "lint_plugin.h"

#include <algorithm>
#include <cctype>

#include "imgui.h"

namespace
{
bool isCxx(const std::string &langName, const std::string &extLower)
{
    if (langName == "C" || langName == "C++")
        return true;
    return extLower == ".c" || extLower == ".cc" || extLower == ".cpp" || extLower == ".cxx" ||
           extLower == ".h" || extLower == ".hpp" || extLower == ".hxx" || extLower == ".inl";
}

// Replace all occurrences of `from` with `to` in `s`; returns count.
int replaceAll(std::string &s, const std::string &from, const std::string &to)
{
    int n = 0;
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
    {
        s.replace(p, from.size(), to);
        ++n;
    }
    return n;
}

// True if the position sits inside a // line comment or a string literal, so
// we don't lint inside comments/strings (cheap single-line heuristic).
bool inCommentOrString(const std::string &line, size_t pos)
{
    bool inStr = false, inChr = false;
    for (size_t i = 0; i < pos && i < line.size(); ++i)
    {
        char c = line[i];
        if (!inStr && !inChr && c == '/' && i + 1 < line.size() && line[i + 1] == '/')
            return true;
        if (!inChr && c == '"' && (i == 0 || line[i - 1] != '\\'))
            inStr = !inStr;
        else if (!inStr && c == '\'' && (i == 0 || line[i - 1] != '\\'))
            inChr = !inChr;
    }
    return inStr || inChr;
}
} // namespace

void LintPlugin::onRegister(PluginHost &host)
{
    enabled_ = host.hostGetFlag("lint.enabled", true);
}

void LintPlugin::refresh(PluginHost &host)
{
    // Memo key must be CHEAP: hostActiveText() serializes the whole document,
    // so testing "did anything change?" with it costs a full copy every frame
    // (this ran twice a frame — onFrame + contributeMarkers — and dropped a
    // large file to single-digit fps). Key on filename + undo index instead and
    // only pull the text on an actual change.
    std::string key = host.hostActiveFilename() + "\x1f" +
                      std::to_string(host.hostActiveDocVersion());
    if (key == cacheKey_)
        return;
    cacheKey_ = key;
    std::string text = host.hostActiveText();
    findings_.clear();
    docLines_.clear();

    // Split into lines (keep them for the Fix path).
    {
        size_t p = 0;
        while (p <= text.size())
        {
            size_t nl = text.find('\n', p);
            std::string ln = (nl == std::string::npos) ? text.substr(p) : text.substr(p, nl - p);
            if (!ln.empty() && ln.back() == '\r')
                ln.pop_back();
            docLines_.push_back(ln);
            if (nl == std::string::npos)
                break;
            p = nl + 1;
        }
    }

    for (int i = 0; i < (int) docLines_.size(); ++i)
    {
        const std::string &ln = docLines_[i];

        // Redundant boolean comparison: `== true` / `== false`.
        for (const char *pat : {"== true", "== false"})
        {
            size_t at = ln.find(pat);
            if (at != std::string::npos && !inCommentOrString(ln, at))
            {
                std::string fixed = ln;
                bool isTrue = std::string(pat) == "== true";
                replaceAll(fixed, isTrue ? " == true" : " == false", "");
                if (!isTrue)
                {
                    // `x == false` -> `!x` is not a safe pure string edit; only
                    // offer the fix for `== true` (drop it). Flag the false case
                    // without an auto-fix.
                    findings_.push_back({i, true, "redundant `== false` (use `!expr`)", {}});
                }
                else
                    findings_.push_back({i, true, "redundant `== true`", fixed});
                break;
            }
        }

        // C NULL in C++ -> nullptr.
        {
            size_t at = ln.find("NULL");
            // whole-word NULL, not inside an identifier like MY_NULL_THING
            bool whole = at != std::string::npos &&
                         (at == 0 || !(std::isalnum((unsigned char) ln[at - 1]) || ln[at - 1] == '_')) &&
                         (at + 4 >= ln.size() || !(std::isalnum((unsigned char) ln[at + 4]) || ln[at + 4] == '_'));
            if (whole && !inCommentOrString(ln, at))
            {
                std::string fixed = ln;
                replaceAll(fixed, "NULL", "nullptr");
                findings_.push_back({i, true, "prefer `nullptr` over `NULL`", fixed});
            }
        }

        // Trailing whitespace.
        if (!ln.empty() && (ln.back() == ' ' || ln.back() == '\t'))
        {
            std::string fixed = ln;
            while (!fixed.empty() && (fixed.back() == ' ' || fixed.back() == '\t'))
                fixed.pop_back();
            findings_.push_back({i, false, "trailing whitespace", fixed});
        }

        // TODO / FIXME / XXX notes.
        for (const char *tag : {"TODO", "FIXME", "XXX"})
            if (ln.find(tag) != std::string::npos)
            {
                findings_.push_back({i, false, std::string(tag) + " note", {}});
                break;
            }
    }
}

void LintPlugin::applyFix(PluginHost &host, int line0, const std::string &newText)
{
    if (line0 < 0 || line0 >= (int) docLines_.size())
        return;
    docLines_[(size_t) line0] = newText;
    std::string out;
    for (size_t i = 0; i < docLines_.size(); ++i)
    {
        out += docLines_[i];
        if (i + 1 < docLines_.size())
            out += '\n';
    }
    host.hostSetActiveText(out);
    cacheKey_.clear(); // force a re-scan of the written-back text
}

void LintPlugin::onFrame(PluginHost &host)
{
    if (!panelVisible_)
        return;
    if (enabled_)
        refresh(host);
    ImGui::SetNextWindowSize(ImVec2(460.0f, 300.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Static Analysis###lintPanel", &panelVisible_))
    {
        ImGui::End();
        return;
    }
    if (ImGui::Checkbox("Enabled", &enabled_))
        host.hostSetFlag("lint.enabled", enabled_);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu finding%s (active document)", findings_.size(),
                        findings_.size() == 1 ? "" : "s");
    ImGui::Separator();
    if (!enabled_)
        ImGui::TextDisabled("(disabled)");
    else if (findings_.empty())
        ImGui::TextDisabled("No issues found.");
    ImGui::BeginChild("##lintList");
    std::string curFile = host.hostActiveFilename();
    for (size_t i = 0; i < findings_.size(); ++i)
    {
        const auto &f = findings_[i];
        ImGui::PushID((int) i);
        ImU32 col = f.warn ? IM_COL32(240, 200, 90, 255) : IM_COL32(120, 170, 255, 255);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        std::string lbl = "L" + std::to_string(f.line0 + 1) + "  " + f.message;
        if (ImGui::Selectable(lbl.c_str()))
            host.hostJumpTo(curFile, f.line0);
        ImGui::PopStyleColor();
        if (!f.fixedLine.empty())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Fix"))
            {
                applyFix(host, f.line0, f.fixedLine);
                ImGui::PopID();
                break; // findings_ rebuilt on next refresh — stop iterating now
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}

void LintPlugin::onMenu(PluginHost &host, PluginMenu which)
{
    (void) host;
    if (which != PluginMenu::Tools)
        return;
    ImGui::MenuItem("Static Analysis", nullptr, &panelVisible_);
}

void LintPlugin::contributeMarkers(PluginHost &host, const PluginDocInfo &doc,
                                   const std::function<void(int, unsigned, unsigned,
                                                            const std::string &, const std::string &)> &add)
{
    if (!enabled_ || doc.filename.empty())
        return;
    // Only the active doc has text exposed to us; skip others.
    if (doc.filename != host.hostActiveFilename())
        return;
    if (!isCxx(doc.languageName, doc.extLower))
        return;
    refresh(host);
    for (const auto &f : findings_)
    {
        ImU32 col = f.warn ? IM_COL32(240, 200, 90, 255) : IM_COL32(120, 170, 255, 255);
        add(f.line0, col, 0, "lint: " + f.message, f.message);
    }
}

void LintPlugin::contributePaletteCommands(PluginHost &host, const PluginDocInfo &,
                                           const std::function<void(const std::string &,
                                                                    std::function<void()>)> &add)
{
    (void) host;
    add("View: Static Analysis", [this]() { panelVisible_ = true; });
}

std::unique_ptr<EditorPlugin> createLintPlugin()
{
    return std::make_unique<LintPlugin>();
}
