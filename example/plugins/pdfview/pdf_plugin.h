//
//  pdf_plugin.h — in-app PDF viewer as an ImGui-IDE plugin.
//
//  Wraps the pure rendering backend (pdfview.{cpp,h}, OS PDF engine via
//  C++/WinRT) behind the EditorPlugin hooks:
//    - openFile: claims .pdf so it opens in a dockable in-app window instead of
//      the external default app
//    - onFrame:  renders the open PDF windows (lazy page textures, LRU-capped)
//
//  Compiled only when IMGUIIDE_PLUGIN_PDF is defined (see example/CMakeLists).
//

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

#include "plugin_api.h"

class PdfViewPlugin : public EditorPlugin
{
public:
    const char *id() const override { return "pdfview"; }
    const char *displayName() const override { return "PDF viewer"; }

    bool openFile(PluginHost &host, const std::filesystem::path &path) override;
    void onFrame(PluginHost &host) override;

private:
    // One open document — pages rendered lazily (visible pages only) into GPU
    // textures; an LRU cap keeps a long document from ballooning VRAM.
    struct PdfDoc
    {
        std::string path;
        std::string windowTitle;
        bool        open = true;
        bool        wantFocus = false;
        bool        fitted = false;
        float       zoom = 1.0f;
        float       renderScale = 1.5f; // texture supersampling vs natural 96-dpi size
        int         pageCount = 0;
        std::string error;              // load error (shown in-window)
        std::vector<std::pair<float, float>> pageSizes; // natural DIP sizes (placeholders)
        struct PageTex
        {
            ImTextureData *tex = nullptr;
            int    w = 0, h = 0;
            bool   failed = false;      // render failed — don't retry every frame
            double lastVisible = 0.0;   // for LRU eviction
        };
        std::vector<PageTex> pages;
    };
    std::vector<std::unique_ptr<PdfDoc>> pdfs;

    void openPdf(const std::string &path);
};

// Factory used by the DLL ABI shim (plugin_dll_main.cpp).
std::unique_ptr<EditorPlugin> createPdfViewPlugin();
