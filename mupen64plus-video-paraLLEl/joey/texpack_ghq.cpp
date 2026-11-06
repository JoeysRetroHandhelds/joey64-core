// Joey64 texture packs on parallel-RDP: the GLideNHQ side (GLideN64's pack reader, GPLv2-or-later; Joey64 is
// GPL-3.0-or-later). Kept apart from rdp.cpp so GLideNHQ's headers stay out of it. Only the pack worker thread
// (rdp.cpp, TexPack) calls these: GLideNHQ is not thread-safe.
#include "texpack_ghq.h"
#include "../../GLideN64/src/GLideNHQ/TxFilterExport.h"
#include <string.h>
#include <wchar.h>
#include <stdlib.h>
#include <string>

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
	return true;
}
