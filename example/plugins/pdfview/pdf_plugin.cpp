//
//  pdf_plugin.cpp — in-app PDF viewer plugin. Window/texture logic moved
//  verbatim from the editor's openPdfFile/renderPdfWindows; the editor core no
//  longer carries any PDF code.
//

#include "pdf_plugin.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "imgui_internal.h"   // RegisterUserTexture / UnregisterUserTexture

#include "pdfview.h"

bool PdfViewPlugin::openFile(PluginHost &host, const std::filesystem::path &path)
{
    (void) host;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".pdf")
        return false;
    openPdf(path.string());
    return true;
}

void PdfViewPlugin::openPdf(const std::string &path)
{
    for (auto &p : pdfs)
    {
        if (p->path == path)
        {
            p->wantFocus = true;
            p->open = true;
            return;
        }
    }
    auto doc = std::make_unique<PdfDoc>();
    doc->path = path;
    doc->windowTitle = std::filesystem::path(path).filename().string() + "##pdf:" + path;
    pdfview::Info inf;
    std::string err;
    if (!pdfview::info(path, inf, err))
    {
        doc->error = err;
    }
    else
    {
        doc->pageCount = inf.pageCount;
        doc->pageSizes = std::move(inf.pageSizes);
        doc->pages.resize((size_t) doc->pageCount);
    }
    doc->wantFocus = true;
    pdfs.push_back(std::move(doc));
}

void PdfViewPlugin::onFrame(PluginHost &host)
{
    auto dropTexture = [](PdfDoc::PageTex &pt) {
        if (pt.tex)
        {
            pt.tex->WantDestroyNextFrame = true;
            ImGui::UnregisterUserTexture(pt.tex);
            pt.tex = nullptr;
        }
    };

    for (auto it = pdfs.begin(); it != pdfs.end();)
    {
        auto &p = **it;
        if (!p.open)
        {
            for (auto &pt : p.pages)
                dropTexture(pt);
            it = pdfs.erase(it);
            continue;
        }
        if (p.wantFocus)
        {
            ImGui::SetNextWindowFocus();
            p.wantFocus = false;
        }
        ImGui::SetNextWindowSize(ImVec2(760.0f, 900.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(p.windowTitle.c_str(), &p.open))
        {
            if (!p.error.empty())
            {
                ImGui::TextWrapped("Could not open PDF: %s", p.error.c_str());
                ImGui::End();
                ++it;
                continue;
            }
            ImGui::Text("%d page%s", p.pageCount, p.pageCount == 1 ? "" : "s");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderFloat("zoom", &p.zoom, 0.25f, 4.0f, "%.2fx");
            ImGui::SameLine();
            if (ImGui::SmallButton("Fit width") && !p.pageSizes.empty())
            {
                float natW = p.pageSizes[0].first;
                if (natW > 1.0f)
                    p.zoom = (ImGui::GetContentRegionAvail().x - 24.0f) / natW;
            }
            ImGui::Separator();
            ImGui::BeginChild("##pdfScroll", ImVec2(0, 0),
                              ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);

            // Auto fit-width on first display (natural size can overflow the window).
            if (!p.fitted && !p.pageSizes.empty())
            {
                float availX = ImGui::GetContentRegionAvail().x;
                float natW = p.pageSizes[0].first;
                if (availX > 1.0f && natW > 1.0f)
                {
                    p.zoom = (std::min)(1.5f, availX / natW);
                    p.fitted = true;
                }
            }

            double now = ImGui::GetTime();
            int liveTextures = 0;
            for (auto &pt : p.pages)
                if (pt.tex)
                    ++liveTextures;

            for (int i = 0; i < p.pageCount; ++i)
            {
                auto &pt = p.pages[(size_t) i];
                float natW = p.pageSizes[(size_t) i].first;
                float natH = p.pageSizes[(size_t) i].second;
                ImVec2 dispSize(natW * p.zoom, natH * p.zoom);

                bool visible = ImGui::IsRectVisible(dispSize);
                if (visible)
                {
                    pt.lastVisible = now;
                    // Lazy render: only when scrolled into view. Blocking (~50-150ms
                    // once per page) — v1 tradeoff, no partial-page flicker.
                    if (!pt.tex && !pt.failed)
                    {
                        pdfview::Page page;
                        std::string err;
                        if (pdfview::renderPage(p.path, i, p.renderScale, page, err) &&
                            page.w > 0 && page.h > 0)
                        {
                            pt.tex = IM_NEW(ImTextureData)();
                            pt.tex->Create(ImTextureFormat_RGBA32, page.w, page.h);
                            std::memcpy(pt.tex->GetPixels(), page.rgba.data(), page.rgba.size());
                            pt.tex->Status = ImTextureStatus_WantCreate;
                            pt.tex->UseColors = true;
                            ImGui::RegisterUserTexture(pt.tex);
                            pt.w = page.w;
                            pt.h = page.h;
                            ++liveTextures;
                        }
                        else
                        {
                            pt.failed = true;
                        }
                    }
                }

                if (pt.tex && pt.tex->Status == ImTextureStatus_OK && pt.tex->TexID != ImTextureID_Invalid)
                    ImGui::Image(pt.tex->GetTexRef(), dispSize);
                else if (pt.failed)
                {
                    ImGui::Dummy(dispSize);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - dispSize.y * 0.5f);
                    ImGui::TextDisabled("   page %d failed to render", i + 1);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + dispSize.y * 0.5f - ImGui::GetTextLineHeight());
                }
                else
                    ImGui::Dummy(dispSize); // placeholder keeps scroll extent stable
                ImGui::Spacing();
            }

            // LRU eviction: cap live page textures so a 500-page doc can't hold
            // hundreds of MB of VRAM. Never evict currently-visible pages.
            const int kMaxLive = 10;
            while (liveTextures > kMaxLive)
            {
                int oldest = -1;
                double oldestTime = 1e300;
                for (int i = 0; i < (int) p.pages.size(); ++i)
                {
                    auto &pt = p.pages[(size_t) i];
                    if (pt.tex && pt.lastVisible < now && pt.lastVisible < oldestTime)
                    {
                        oldestTime = pt.lastVisible;
                        oldest = i;
                    }
                }
                if (oldest < 0)
                    break;
                dropTexture(p.pages[(size_t) oldest]);
                --liveTextures;
            }

            host.hostMiddleMousePanScroll(111); // pdf viewport — honors pan-invert (plugin keys 100+)
            ImGui::EndChild();
        }
        ImGui::End();
        ++it;
    }
}

std::unique_ptr<EditorPlugin> createPdfViewPlugin()
{
    return std::make_unique<PdfViewPlugin>();
}
