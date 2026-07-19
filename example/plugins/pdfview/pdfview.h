//	ImGui-IDE — PDF rendering backend (Windows.Data.Pdf via C++/WinRT).
//
//	Pure data layer: opens a PDF, reports page count + natural page sizes (DIPs,
//	96 dpi), and renders single pages to RGBA buffers. No ImGui/SDL — the app
//	turns the buffers into textures. Windows-only (the OS PDF engine); the POSIX
//	stubs fail with a clear message.

#pragma once

#include <string>
#include <vector>

namespace pdfview {

struct Info {
	int pageCount = 0;
	std::vector<std::pair<float, float>> pageSizes; // natural DIP size per page
};

struct Page {
	int w = 0, h = 0;
	std::vector<unsigned char> rgba; // w*h*4
};

// Page count + natural sizes. False + err on failure.
bool info(const std::string& path, Info& out, std::string& err);

// Render one page at `scale` (1.0 = natural 96-dpi size) to RGBA (white background).
bool renderPage(const std::string& path, int pageIndex, float scale, Page& out, std::string& err);

} // namespace pdfview
