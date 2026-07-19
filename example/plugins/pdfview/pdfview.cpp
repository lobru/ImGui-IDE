//	ImGui-IDE — PDF rendering backend. See pdfview.h.
//
//	Windows implementation uses the OS PDF engine (Windows.Data.Pdf) through
//	C++/WinRT: load the document, render a page into an in-memory PNG stream,
//	decode with stb_image to RGBA. Every entry point runs its WinRT work on a
//	fresh worker thread initialized as MTA and blocks on completion — the
//	blocking .get() calls are illegal on an STA thread, and the app's main
//	thread makes no apartment guarantees.

#include "pdfview.h"

#ifdef _WIN32

// stb_image — static-linkage copy for this TU (mirrors editor.cpp).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505) // unreferenced static functions
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#if defined(_MSC_VER)
#pragma warning(push, 3)
#endif
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.UI.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <filesystem>
#include <thread>

namespace pdfview {

// Run `fn` on a worker thread with an MTA apartment; capture any error text.
template <typename F>
static bool runWorker(F&& fn, std::string& err)
{
	bool ok = false;
	std::string workerErr;
	std::thread worker([&]() {
		try
		{
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
		}
		catch (...)
		{
			// Already initialized on this thread in some mode — fine either way.
		}
		try
		{
			fn();
			ok = true;
		}
		catch (const winrt::hresult_error& e)
		{
			workerErr = winrt::to_string(e.message());
		}
		catch (const std::exception& e)
		{
			workerErr = e.what();
		}
		catch (...)
		{
			workerErr = "unknown PDF engine error";
		}
	});
	worker.join();
	if (!ok)
		err = workerErr.empty() ? "PDF engine failed" : workerErr;
	return ok;
}

static winrt::Windows::Data::Pdf::PdfDocument loadDocument(const std::string& path)
{
	using namespace winrt::Windows::Storage;
	using namespace winrt::Windows::Data::Pdf;
	auto abs = std::filesystem::absolute(std::filesystem::path(path)).string();
	auto file = StorageFile::GetFileFromPathAsync(winrt::to_hstring(abs)).get();
	return PdfDocument::LoadFromFileAsync(file).get();
}

bool info(const std::string& path, Info& out, std::string& err)
{
	out = {};
	return runWorker(
		[&]() {
			auto doc = loadDocument(path);
			out.pageCount = static_cast<int>(doc.PageCount());
			out.pageSizes.reserve(static_cast<size_t>(out.pageCount));
			for (int i = 0; i < out.pageCount; ++i)
			{
				auto page = doc.GetPage(static_cast<uint32_t>(i));
				auto size = page.Size();
				out.pageSizes.emplace_back(size.Width, size.Height);
			}
		},
		err);
}

bool renderPage(const std::string& path, int pageIndex, float scale, Page& out, std::string& err)
{
	out = {};
	if (pageIndex < 0)
	{
		err = "bad page index";
		return false;
	}
	std::vector<uint8_t> png;
	bool ok = runWorker(
		[&]() {
			using namespace winrt::Windows::Data::Pdf;
			using namespace winrt::Windows::Storage::Streams;
			auto doc = loadDocument(path);
			if (static_cast<uint32_t>(pageIndex) >= doc.PageCount())
				throw std::runtime_error("page index out of range");
			auto page = doc.GetPage(static_cast<uint32_t>(pageIndex));
			auto size = page.Size();

			PdfPageRenderOptions opts;
			opts.DestinationWidth(static_cast<uint32_t>(size.Width * scale + 0.5f));
			opts.DestinationHeight(static_cast<uint32_t>(size.Height * scale + 0.5f));
			opts.BackgroundColor(winrt::Windows::UI::Color{ 255, 255, 255, 255 }); // opaque white

			InMemoryRandomAccessStream stream;
			page.RenderToStreamAsync(stream, opts).get(); // encodes PNG

			auto len = static_cast<uint32_t>(stream.Size());
			DataReader reader(stream.GetInputStreamAt(0));
			reader.LoadAsync(len).get();
			png.resize(len);
			reader.ReadBytes(png);
		},
		err);
	if (!ok)
		return false;

	int w = 0, h = 0, n = 0;
	stbi_uc* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &n, 4);
	if (!pixels || w <= 0 || h <= 0)
	{
		if (pixels)
			stbi_image_free(pixels);
		err = "PNG decode of rendered page failed";
		return false;
	}
	out.w = w;
	out.h = h;
	out.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
	stbi_image_free(pixels);
	return true;
}

} // namespace pdfview

#else // !_WIN32 — the desktop PDF viewer rides the OS engine; POSIX says so clearly.

namespace pdfview {

bool info(const std::string&, Info&, std::string& err)
{
	err = "The PDF viewer is Windows-only for now (uses the OS PDF engine).";
	return false;
}

bool renderPage(const std::string&, int, float, Page&, std::string& err)
{
	err = "The PDF viewer is Windows-only for now (uses the OS PDF engine).";
	return false;
}

} // namespace pdfview

#endif
