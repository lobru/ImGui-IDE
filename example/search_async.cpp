//
//  search_async.cpp — non-blocking project-wide text search (references +
//  find-in-files). Worker-thread scan into a mutex-guarded ProjectSearch;
//  the panels poll each frame and render hits through an ImGuiListClipper.
//  Ported from the performance-optimization-roadmap branch.
//

#include "editor.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "imgui.h"

// A code/text file worth scanning. Self-contained whitelist (the editor's own
// isCodeExtension is private); errs toward including source/markup/config text.
static bool pathIsCodeFile(const std::filesystem::path &p)
{
    static const std::unordered_set<std::string> kExts = {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp",
        ".cs", ".java", ".kt", ".go", ".rs", ".swift", ".m", ".mm",
        ".py", ".pyi", ".rb", ".pl", ".lua", ".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs",
        ".php", ".sh", ".bash", ".zsh", ".ps1", ".bat", ".cmd",
        ".html", ".htm", ".css", ".scss", ".sass", ".less", ".xml", ".xaml", ".svg",
        ".json", ".jsonc", ".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf", ".props",
        ".md", ".markdown", ".txt", ".rst", ".tex",
        ".glsl", ".hlsl", ".fx", ".fxh", ".hlsli", ".shader", ".compute",
        ".sql", ".cmake", ".gradle", ".make", ".mk", ".build", ".cs", ".verse",
        ".uproject", ".uplugin", ".target", ".vcxproj", ".csproj", ".sln",
    };
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char) std::tolower(c); });
    auto name = p.filename().string();
    if (ext.empty())
        return name == "CMakeLists.txt" || name == "Makefile" || name == "Dockerfile";
    return kExts.count(ext) != 0;
}

void Editor::startProjectSearch(std::shared_ptr<ProjectSearch> search,
                                std::string needle, bool caseSensitive, bool wholeWord,
                                size_t maxHits, bool skipDepsVendor,
                                std::filesystem::path root, std::string activeCanon,
                                std::string activeLabel, std::string activeText)
{
    int gen = ++search->gen;
    {
        std::lock_guard<std::mutex> lk(search->mutex);
        search->hits.clear();
        search->fileCount = 0;
        search->truncated = false;
        ++search->version;
    }
    if (needle.empty())
        return;
    search->running = true;

    std::thread([search, gen, needle = std::move(needle), caseSensitive, wholeWord, maxHits,
                 skipDepsVendor, root = std::move(root), activeCanon = std::move(activeCanon),
                 activeLabel = std::move(activeLabel), activeText = std::move(activeText)]() {
        // Only the pass that still OWNS the state may clear `running` — a
        // superseded pass unwinding late must not stomp its successor's flag.
        auto finish = [&]() {
            if (gen == search->gen.load())
                search->running = false;
        };
        auto lower = [](std::string s) {
            for (auto &c : s)
                c = (char) std::tolower((unsigned char) c);
            return s;
        };
        const std::string fold = caseSensitive ? needle : lower(needle);
        auto isBoundary = [](char c) { return !(std::isalnum((unsigned char) c) || c == '_'); };

        // Scan one stream into a per-file batch (one lock per file, not per hit).
        auto scanStream = [&](const std::string &file, std::istream &in, std::vector<RefHit> &batch) {
            std::string line;
            int ln = 0;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                const std::string hay = caseSensitive ? line : lower(line);
                size_t pos = 0;
                while ((pos = hay.find(fold, pos)) != std::string::npos)
                {
                    size_t end = pos + fold.size();
                    bool ok = !wholeWord ||
                              (((pos == 0) || isBoundary(line[pos - 1])) &&
                               ((end >= line.size()) || isBoundary(line[end])));
                    if (ok)
                    {
                        std::string trimmed = line;
                        size_t s = trimmed.find_first_not_of(" \t");
                        if (s != std::string::npos)
                            trimmed = trimmed.substr(s);
                        if (trimmed.size() > 400)
                            trimmed.resize(400);
                        batch.push_back({file, ln, std::move(trimmed)});
                        break; // one hit per line
                    }
                    pos = end;
                }
                ++ln;
            }
        };
        // Append a file's batch; false = stop (superseded or hit cap reached).
        auto publish = [&](std::vector<RefHit> &batch) {
            if (gen != search->gen.load())
                return false;
            if (batch.empty())
                return true;
            std::lock_guard<std::mutex> lk(search->mutex);
            if (search->hits.size() >= maxHits)
            {
                search->truncated = true;
                return false;
            }
            if (search->hits.size() + batch.size() > maxHits)
            {
                batch.resize(maxHits - search->hits.size());
                search->truncated = true;
            }
            search->hits.insert(search->hits.end(),
                                std::make_move_iterator(batch.begin()),
                                std::make_move_iterator(batch.end()));
            ++search->fileCount;
            ++search->version;
            return !search->truncated;
        };

        // Active doc's LIVE buffer first — unsaved edits count.
        if (!activeLabel.empty())
        {
            std::vector<RefHit> batch;
            std::istringstream ss(activeText);
            scanStream(activeLabel, ss, batch);
            if (!publish(batch))
            {
                finish();
                return;
            }
        }

        // Enumerate candidate files.
        std::vector<std::string> files;
        if (!root.empty())
        {
            std::error_code ec;
            int budget = 8000;
            for (auto it = std::filesystem::recursive_directory_iterator(
                     root, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            {
                if (gen != search->gen.load())
                {
                    finish();
                    return;
                }
                if (ec)
                {
                    ec.clear();
                    continue;
                }
                if (it->is_directory(ec))
                {
                    auto n = it->path().filename().string();
                    bool skip = n == ".git" || n == ".svn" || n == ".hg" || n == "node_modules" ||
                                n == "bin" || n == "obj" || n == "out" || n == "build" ||
                                n == "target" || n == ".vs" || n == ".vscode" || n == ".idea" ||
                                n == "__pycache__" ||
                                n == "Backup" || n == "backup" || n == "Backups" || n == "backups";
                    if (skipDepsVendor)
                        skip = skip || n == "deps" || n == "vendor";
                    if (skip)
                        it.disable_recursion_pending();
                    continue;
                }
                if (--budget < 0)
                {
                    std::lock_guard<std::mutex> lk(search->mutex);
                    search->truncated = true;
                    ++search->version;
                    break;
                }
                if (!pathIsCodeFile(it->path()))
                    continue;
                auto canon = std::filesystem::weakly_canonical(it->path(), ec);
                if (!activeCanon.empty() && canon.string() == activeCanon)
                    continue; // already scanned from the live buffer
                files.push_back(it->path().string());
            }
        }

        // Fan the file scans out. Workers pull candidates via an atomic cursor;
        // per-file batches keep each file's hits contiguous in the result list
        // (the panels group rows by consecutive file).
        unsigned hw = std::thread::hardware_concurrency();
        int nWorkers = (int) (std::min)(files.size(), (size_t) std::clamp((int) hw - 1, 1, 8));
        if (nWorkers < 1)
            nWorkers = 1;
        std::atomic<size_t> nextFile{0};
        std::atomic<bool> stop{false};
        auto workerBody = [&]() {
            for (;;)
            {
                if (stop.load() || gen != search->gen.load())
                    return;
                size_t i = nextFile.fetch_add(1);
                if (i >= files.size())
                    return;
                try
                {
                    std::ifstream f(files[i], std::ios::binary);
                    if (!f.is_open())
                        continue;
                    std::vector<RefHit> batch;
                    scanStream(files[i], f, batch);
                    if (!publish(batch))
                    {
                        stop = true;
                        return;
                    }
                }
                catch (...)
                {
                    // Unreadable/pathological file — skip it, keep searching.
                }
            }
        };
        std::vector<std::thread> pool;
        pool.reserve((size_t) (nWorkers - 1));
        for (int w = 1; w < nWorkers; ++w)
            pool.emplace_back(workerBody);
        workerBody();
        for (auto &th : pool)
            th.join();
        finish();
    }).detach();
}


void Editor::buildSearchRows(const std::vector<RefHit> &hits, std::vector<int> &rows)
{
    rows.clear();
    rows.reserve(hits.size() + hits.size() / 4 + 1);
    const std::string *lastFile = nullptr;
    for (size_t i = 0; i < hits.size(); ++i)
    {
        if (!lastFile || hits[i].file != *lastFile)
        {
            lastFile = &hits[i].file;
            rows.push_back(-(int) i - 1);   // header row for hit i's file
        }
        rows.push_back((int) i);
    }
}

// Copy fresh results out of the async search state — only when `version`
// moved, so an idle panel costs one mutex probe per frame, not a 5000-row copy.
bool Editor::pollProjectSearch(ProjectSearch &search, int &seenVersion,
                               std::vector<RefHit> &hits, int &fileCount, bool &truncated)
{
    std::lock_guard<std::mutex> lk(search.mutex);
    if (search.version == seenVersion)
        return false;
    hits = search.hits;
    fileCount = search.fileCount;
    truncated = search.truncated;
    seenVersion = search.version;
    return true;
}

// Clipper-driven render of a grouped hit list. Every row is one text line
// high (header rows are plain text, hit rows a Selectable + trailing text),
// so the clipper's row-height probe holds for the whole list.
void Editor::renderSearchHits(const std::vector<RefHit> &hits, const std::vector<int> &rows)
{
    ImGuiListClipper clipper;
    clipper.Begin((int) rows.size());
    while (clipper.Step())
    {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
        {
            int v = rows[(size_t) r];
            if (v < 0)
            {
                const auto &hit = hits[(size_t) (-v - 1)];
                auto leaf = std::filesystem::path(hit.file).filename().string();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::TextUnformatted(leaf.empty() ? hit.file.c_str() : leaf.c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", hit.file.c_str());
                continue;
            }
            const auto &hit = hits[(size_t) v];
            ImGui::PushID(r);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%5d", hit.line + 1);
            if (ImGui::Selectable(buf, false, 0, ImVec2(48.0f, 0.0f)))
            {
                navHistory.record(currentNavLocation());   // so Back returns here
                openFile(hit.file);
                if (!tabs.empty())
                {
                    auto &ed = doc().editor;
                    ed.SetCursor(hit.line, 0);
                    ed.SelectLine(hit.line); // highlight the whole line, not just the gutter
                    ed.ScrollToLine(hit.line, TextEditor::Scroll::alignMiddle);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", hit.text.c_str());
            ImGui::PopID();
        }
    }
}
