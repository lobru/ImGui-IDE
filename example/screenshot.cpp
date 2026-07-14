#include "screenshot.h"

#include <cstdio>
#include <ctime>
#include <fstream>

namespace screenshot {

namespace {

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu)
{
	static uint32_t table[256];
	static bool built = false;

	if (!built)
	{
		for (uint32_t n = 0; n < 256; n++)
		{
			uint32_t c = n;

			for (int k = 0; k < 8; k++)
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);

			table[n] = c;
		}

		built = true;
	}

	for (size_t i = 0; i < len; i++)
		crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);

	return crc;
}

void put32(std::vector<uint8_t>& out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v >> 24));
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

void chunk(std::vector<uint8_t>& out, const char tag[4], const std::vector<uint8_t>& data)
{
	put32(out, static_cast<uint32_t>(data.size()));

	const size_t start = out.size();
	out.insert(out.end(), tag, tag + 4);
	out.insert(out.end(), data.begin(), data.end());

	// the CRC covers the type tag AND the data, but not the length
	const uint32_t crc = crc32(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu;
	put32(out, crc);
}

} // namespace

void bgraToRgba(uint8_t* pixels, size_t bytes)
{
	for (size_t i = 0; i + 3 < bytes; i += 4)
	{
		const uint8_t b = pixels[i];
		pixels[i] = pixels[i + 2];
		pixels[i + 2] = b;
	}
}

bool writePng(const std::string& path, const uint8_t* rgba, int width, int height, int strideBytes)
{
	if (!rgba || width <= 0 || height <= 0)
		return false;

	// ── raw scanlines: each row is prefixed with filter type 0 (None) ──
	std::vector<uint8_t> raw;
	raw.reserve(static_cast<size_t>(height) * (static_cast<size_t>(width) * 4 + 1));

	for (int y = 0; y < height; y++)
	{
		raw.push_back(0);
		const uint8_t* row = rgba + static_cast<size_t>(y) * static_cast<size_t>(strideBytes);

		for (int x = 0; x < width; x++)
		{
			raw.push_back(row[x * 4 + 0]);
			raw.push_back(row[x * 4 + 1]);
			raw.push_back(row[x * 4 + 2]);
			raw.push_back(0xFF); // opaque: a composited UI has no meaningful alpha
		}
	}

	// ── zlib stream with STORED deflate blocks (no compressor needed) ──
	std::vector<uint8_t> z;
	z.push_back(0x78); // CMF: deflate, 32K window
	z.push_back(0x01); // FLG: no dict, check bits make (0x78<<8|0x01) % 31 == 0

	constexpr size_t kBlock = 65535; // max STORED block payload

	for (size_t off = 0; off < raw.size(); off += kBlock)
	{
		const size_t n = (raw.size() - off < kBlock) ? (raw.size() - off) : kBlock;
		const bool last = (off + n) >= raw.size();

		z.push_back(last ? 1 : 0);                        // BFINAL, BTYPE=00 (stored)
		z.push_back(static_cast<uint8_t>(n & 0xFF));      // LEN (little endian)
		z.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
		z.push_back(static_cast<uint8_t>(~n & 0xFF));     // NLEN = ~LEN
		z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
		z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
	}

	// Adler-32 of the UNCOMPRESSED data, appended to the zlib stream
	uint32_t a = 1, b = 0;
	for (uint8_t v : raw)
	{
		a = (a + v) % 65521;
		b = (b + a) % 65521;
	}
	put32(z, (b << 16) | a);

	// ── assemble the PNG ──
	std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

	std::vector<uint8_t> ihdr;
	put32(ihdr, static_cast<uint32_t>(width));
	put32(ihdr, static_cast<uint32_t>(height));
	ihdr.push_back(8); // bit depth
	ihdr.push_back(6); // colour type 6 = RGBA
	ihdr.push_back(0); // deflate
	ihdr.push_back(0); // adaptive filtering
	ihdr.push_back(0); // no interlace
	chunk(png, "IHDR", ihdr);
	chunk(png, "IDAT", z);
	chunk(png, "IEND", {});

	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f.is_open())
		return false;

	f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
	return f.good();
}

std::string timestampedPath(const std::string& dir)
{
	std::time_t now = std::time(nullptr);
	std::tm tm {};

#ifdef _WIN32
	localtime_s(&tm, &now);
#else
	localtime_r(&now, &tm);
#endif

	char stamp[64];
	std::snprintf(stamp, sizeof(stamp), "imgui-ide-%04d%02d%02d-%02d%02d%02d.png",
	              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	              tm.tm_hour, tm.tm_min, tm.tm_sec);

	return dir.empty() ? std::string(stamp) : (dir + "/" + stamp);
}

} // namespace screenshot
