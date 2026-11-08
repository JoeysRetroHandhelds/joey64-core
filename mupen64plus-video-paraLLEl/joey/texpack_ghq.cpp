// Joey64 texture packs on parallel-RDP: the GLideNHQ side (GLideN64's pack reader, GPLv2-or-later; Joey64 is
// GPL-3.0-or-later). Kept apart from rdp.cpp so GLideNHQ's headers stay out of it. Only the pack worker thread
// (rdp.cpp, TexPack) calls these: GLideNHQ is not thread-safe.
#include "texpack_ghq.h"
#include "../../GLideN64/src/GLideNHQ/TxFilterExport.h"
#include <string.h>
#include <wchar.h>
#include <stdlib.h>
#include <string>
#include <atomic>
#include <stdio.h>

namespace
{
// GL values as GLideNHQ stores them in GHQTexInfo (texture_format, pixel_type).
enum : unsigned
{
	GL_RGB_ = 0x1907, GL_RGBA_ = 0x1908, GL_LUMINANCE_ = 0x1909, GL_LUMINANCE_ALPHA_ = 0x190A,
	GL_UNSIGNED_BYTE_ = 0x1401, GL_UNSIGNED_SHORT_4_4_4_4_ = 0x8033, GL_UNSIGNED_SHORT_5_5_5_1_ = 0x8034,
	GL_UNSIGNED_SHORT_5_6_5_ = 0x8363,
};
bool inited;

std::wstring widen(const char *s)
{
	std::wstring w;
	for (; *s; s++)
		w += wchar_t((unsigned char)*s);
	return w;
}

inline uint32_t rgba(unsigned r, unsigned g, unsigned b, unsigned a)
{
	return r | (g << 8) | (b << 16) | (a << 24);
}
inline unsigned x5(unsigned v) { return (v << 3) | (v >> 2); }
inline unsigned x6(unsigned v) { return (v << 2) | (v >> 4); }
inline unsigned x4(unsigned v) { return v * 17; }

// Dev counters (N64_RDP_TEXPACK_FMTSTATS=1, order item 10): texels looked up, by the smallest GPU format that holds
// them exactly: 0 L8 (grey, opaque), 1 LA8 (grey), 2 RGB565 (opaque), 3 RGBA5551 (alpha 0 or 255), 4 RGBA4444, 5 RGBA8;
// a replacement counts as a whole in the smallest format all its texels fit. src16: stored as 16-bit in the pack.
std::atomic<uint64_t> fmt_texels[6], src16_texels, all_texels;
bool fmt_stats = false;
inline bool e5(unsigned v) { return x5(v >> 3) == v; }
inline bool e6(unsigned v) { return x6(v >> 2) == v; }
inline bool e4(unsigned v) { return x4(v >> 4) == v; }
void count_formats(const std::vector<uint32_t> &px)
{
	bool l8 = true, la8 = true, c565 = true, c5551 = true, c4444 = true;
	for (uint32_t c : px)
	{
		const unsigned r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff, a = c >> 24;
		const bool grey = r == g && g == b;
		l8 = l8 && grey && a == 255;
		la8 = la8 && grey;
		c565 = c565 && a == 255 && e5(r) && e6(g) && e5(b);
		c5551 = c5551 && (a == 0 || a == 255) && e5(r) && e5(g) && e5(b);
		c4444 = c4444 && e4(r) && e4(g) && e4(b) && e4(a);
		if (!la8 && !c5551 && !c4444 && !c565)
			break;
	}
	const int k = l8 ? 0 : la8 ? 1 : c565 ? 2 : c5551 ? 3 : c4444 ? 4 : 5;
	fmt_texels[k] += px.size();
	all_texels += px.size();
}
}

void texpack_ghq_fmtstats(bool on) { fmt_stats = on; }
void texpack_ghq_fmtstats_line(char *buf, size_t len)
{
	const uint64_t n = all_texels.load();
	const double mb = 1.0 / (1 << 20);
	const uint64_t now = n * 4;
	const uint64_t small = fmt_texels[0] + fmt_texels[1] * 2 + (fmt_texels[2] + fmt_texels[3] + fmt_texels[4]) * 2 +
	                       fmt_texels[5] * 4;
	snprintf(buf, len, "%.1f MB as RGBA8, %.1f MB in the smallest exact formats (%.0f%%); texels L8 %.1f%%, LA8 %.1f%%, "
	         "565 %.1f%%, 5551 %.1f%%, 4444 %.1f%%, RGBA8 %.1f%%; stored 16-bit in the pack %.1f%%",
	         now * mb, small * mb, n ? 100.0 * small / now : 0.0,
	         n ? 100.0 * fmt_texels[0] / n : 0.0, n ? 100.0 * fmt_texels[1] / n : 0.0, n ? 100.0 * fmt_texels[2] / n : 0.0,
	         n ? 100.0 * fmt_texels[3] / n : 0.0, n ? 100.0 * fmt_texels[4] / n : 0.0, n ? 100.0 * fmt_texels[5] / n : 0.0,
	         n ? 100.0 * src16_texels.load() / n : 0.0);
}

bool texpack_ghq_init(const char *dir, const char *romname)
{
	if (inited)
		return true;
	const std::wstring wdir = widen(dir), wrom = widen(romname);
	// As GLideN64 with its defaults (hi-res packs on, full alpha channel, compressed cache) and file storage, so a
	// .hts of any size is read entry by entry from disk instead of loaded whole.
	const int options = RICE_HIRESTEXTURES | FILE_HIRESTEXCACHE | DUMP_HIRESTEXCACHE | GZ_HIRESTEXCACHE |
	                    LET_TEXARTISTS_FLY;
	inited = txfilter_init(4096, 4096, 32, options, 0, wdir.c_str(), wdir.c_str(), wdir.c_str(), wrom.c_str(),
	                       nullptr) != 0;
	return inited;
}

void texpack_ghq_shutdown()
{
	if (inited)
		txfilter_shutdown();
	inited = false;
}

bool texpack_ghq_lookup(uint64_t crc, unsigned fmt, unsigned siz, const uint16_t *palette, std::vector<uint32_t> &out,
                        unsigned &width, unsigned &height)
{
	if (!inited)
		return false;
	GHQTexInfo info;
	if (!txfilter_hirestex(0, Checksum(crc), const_cast<uint16 *>(palette), N64FormatSize(uint16(fmt), uint16(siz)),
	                       &info) || !info.data || info.width == 0 || info.height == 0)
		return false;
	width = info.width;
	height = info.height;
	const size_t n = size_t(width) * height;
	out.resize(n);
	const uint8_t *src = info.data;
	const uint16_t *src16 = reinterpret_cast<const uint16_t *>(info.data);
	switch (info.pixel_type)
	{
	case GL_UNSIGNED_BYTE_:
		if (info.texture_format == GL_RGBA_)
			memcpy(out.data(), src, n * 4);
		else if (info.texture_format == GL_RGB_)
			for (size_t i = 0; i < n; i++)
				out[i] = rgba(src[3 * i], src[3 * i + 1], src[3 * i + 2], 255);
		else if (info.texture_format == GL_LUMINANCE_ALPHA_)
			for (size_t i = 0; i < n; i++)
				out[i] = rgba(src[2 * i], src[2 * i], src[2 * i], src[2 * i + 1]);
		else if (info.texture_format == GL_LUMINANCE_)
			for (size_t i = 0; i < n; i++)
				out[i] = rgba(src[i], src[i], src[i], 255);
		else
			return false;
		break;
	case GL_UNSIGNED_SHORT_5_5_5_1_:
		for (size_t i = 0; i < n; i++)
		{
			unsigned v = src16[i];
			out[i] = rgba(x5(v >> 11), x5((v >> 6) & 31), x5((v >> 1) & 31), (v & 1) ? 255 : 0);
		}
		break;
	case GL_UNSIGNED_SHORT_4_4_4_4_:
		for (size_t i = 0; i < n; i++)
		{
			unsigned v = src16[i];
			out[i] = rgba(x4(v >> 12), x4((v >> 8) & 15), x4((v >> 4) & 15), x4(v & 15));
		}
		break;
	case GL_UNSIGNED_SHORT_5_6_5_:
		for (size_t i = 0; i < n; i++)
		{
			unsigned v = src16[i];
			out[i] = rgba(x5(v >> 11), x6((v >> 5) & 63), x5(v & 31), 255);
		}
		break;
	default:
		return false;
	}
	if (fmt_stats)
	{
		if (info.pixel_type != GL_UNSIGNED_BYTE_)
			src16_texels += n;
		count_formats(out);
	}
	return true;
}
