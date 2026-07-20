//	Screenshot — write the app's own framebuffer to a PNG.
//
//	The app renders through SDL3's GPU (D3D12) swapchain, and OS-level screen
//	capture reads that back as a black rectangle: GPU-composed content isn't in
//	the desktop compositor's capture path. So don't screen-capture the window at
//	all — read back the buffer we drew, which is authoritative and doesn't care
//	what's on top of the window (or whether the window is even on screen).
//
//	writePng has no dependencies: a PNG's zlib stream is allowed to use STORED
//	(uncompressed) deflate blocks, so we can emit a real, universally-readable
//	.png with nothing but CRC-32 and Adler-32. Bigger on disk than a compressed
//	PNG; free of a compression library.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace screenshot {

// rgba: `height` rows of `width` RGBA8 pixels, `strideBytes` apart. Alpha is
// forced opaque — the swapchain's alpha channel is meaningless once composited,
// and a transparent PNG of the UI is not what anyone wants.
bool writePng(const std::string& path, const uint8_t* rgba, int width, int height, int strideBytes);

// Swap R and B in place, for a BGRA swapchain (the common D3D12 format).
void bgraToRgba(uint8_t* pixels, size_t bytes);

// <dir>/imgui-ide-YYYYmmdd-HHMMSS.png
std::string timestampedPath(const std::string& dir);

} // namespace screenshot
