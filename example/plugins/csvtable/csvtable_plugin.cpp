//
//  csvtable_plugin.cpp — see csvtable_plugin.h.
//

#include "csvtable_plugin.h"

#include <algorithm>
#include <cctype>

#include "imgui.h"

bool CsvTablePlugin::isCsvName(const std::string &name, char &delimOut)
{
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });
    if (ext == ".csv") { delimOut = ','; return true; }
    if (ext == ".tsv") { delimOut = '\t'; return true; }
    return false;
}

// RFC-4180-ish: fields split on `delim`, "quoted" fields may hold delim,
// newlines, and "" escaped quotes.
void CsvTablePlugin::reparse(const std::string &text)
{
    grid_.clear();
    std::vector<std::string> row;
    std::string field;
    bool inQuotes = false;
    auto endField = [&]() { row.push_back(std::move(field)); field.clear(); };
    auto endRow = [&]() { endField(); grid_.push_back(std::move(row)); row.clear(); };
    for (size_t i = 0; i < text.size(); ++i)
    {
        char c = text[i];
        if (inQuotes)
        {
            if (c == '"')
            {
                if (i + 1 < text.size() && text[i + 1] == '"') { field += '"'; ++i; }
                else inQuotes = false;
            }
            else
                field += c;
        }
        else if (c == '"')
            inQuotes = true;
        else if (c == delim_)
            endField();
        else if (c == '\n')
            endRow();
        else if (c == '\r')
            ; // swallow — CRLF handled by the \n
        else
            field += c;
    }
    if (!field.empty() || !row.empty())
        endRow();
    // Normalize column count to the widest row so the table is rectangular.
    size_t cols = 0;
    for (auto &r : grid_) cols = std::max(cols, r.size());
    for (auto &r : grid_) r.resize(cols);
}

std::string CsvTablePlugin::serialize() const
{
    std::string out;
    for (const auto &r : grid_)
    {
        for (size_t c = 0; c < r.size(); ++c)
        {
            if (c) out += delim_;
            const std::string &f = r[c];
            bool needQuote = f.find(delim_) != std::string::npos ||
                             f.find('"') != std::string::npos ||
                             f.find('\n') != std::string::npos;
            if (needQuote)
            {
                out += '"';
                for (char ch : f) { if (ch == '"') out += '"'; out += ch; }
                out += '"';
            }
            else
                out += f;
        }
        out += '\n';
    }
    return out;
}

void CsvTablePlugin::onMenu(PluginHost &host, PluginMenu which)
{
    if (which != PluginMenu::Tools)
        return;
    char d;
    bool isCsv = isCsvName(host.hostActiveFilename(), d);
    if (ImGui::MenuItem("CSV Table", nullptr, visible_, isCsv || visible_))
        visible_ = !visible_;
}

void CsvTablePlugin::onFrame(PluginHost &host)
{
    if (!visible_)
        return;
    std::string fname = host.hostActiveFilename();
    char d;
    if (!isCsvName(fname, d))
        return; // only for CSV/TSV documents
    delim_ = d;

    ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CSV Table###csvTable", &visible_))
    {
        ImGui::End();
        return;
    }

    // Reparse when the document (or its length) changed and there's no pending
    // local edit to write back.
    std::string text = host.hostActiveText();
    std::string key = fname + "\x1f" + std::to_string(text.size());
    if (key != cacheKey_ && !dirty_)
    {
        cacheKey_ = key;
        reparse(text);
    }

    ImGui::Text("%zu rows x %zu cols", grid_.empty() ? 0 : grid_.size() - 1,
                grid_.empty() ? 0 : grid_.front().size());
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty_);
    if (ImGui::SmallButton("Apply to document"))
    {
        host.hostSetActiveText(serialize());
        dirty_ = false;
        cacheKey_.clear(); // force a reparse of the written-back text next frame
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Revert"))
    {
        dirty_ = false;
        cacheKey_.clear();
    }
    if (dirty_)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(unapplied edits)");
    }
    ImGui::Separator();

    if (grid_.empty() || grid_.front().empty())
    {
        ImGui::TextDisabled("(empty)");
        ImGui::End();
        return;
    }

    int cols = (int) grid_.front().size();
    if (cols > 64) cols = 64; // ImGui table column cap
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##csv", cols, flags))
    {
        // Header row from grid_[0]. Labels are held in a stable local vector so
        // the const char* stays valid until TableHeadersRow copies them.
        std::vector<std::string> labels;
        labels.reserve((size_t) cols);
        for (int c = 0; c < cols; ++c)
        {
            const std::string &h = grid_[0][(size_t) c];
            labels.push_back(h.empty() ? ("col " + std::to_string(c + 1)) : h);
        }
        for (int c = 0; c < cols; ++c)
            ImGui::TableSetupColumn(labels[(size_t) c].c_str());
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int) grid_.size() - 1); // data rows (skip header)
        while (clipper.Step())
        {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
            {
                auto &row = grid_[(size_t) r + 1];
                ImGui::TableNextRow();
                for (int c = 0; c < cols; ++c)
                {
                    ImGui::TableSetColumnIndex(c);
                    ImGui::PushID(r * 1000 + c);
                    char buf[512];
                    std::snprintf(buf, sizeof(buf), "%s", (size_t) c < row.size() ? row[(size_t) c].c_str() : "");
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputText("##cell", buf, sizeof(buf)))
                    {
                        if ((size_t) c < row.size())
                            row[(size_t) c] = buf;
                        dirty_ = true;
                    }
                    ImGui::PopID();
                }
            }
        }
        clipper.End();
        ImGui::EndTable();
    }
    ImGui::End();
}

void CsvTablePlugin::contributePaletteCommands(PluginHost &host, const PluginDocInfo &,
                                               const std::function<void(const std::string &,
                                                                        std::function<void()>)> &add)
{
    char d;
    if (isCsvName(host.hostActiveFilename(), d))
        add("View: CSV Table", [this]() { visible_ = true; });
}

std::unique_ptr<EditorPlugin> createCsvTablePlugin()
{
    return std::make_unique<CsvTablePlugin>();
}
