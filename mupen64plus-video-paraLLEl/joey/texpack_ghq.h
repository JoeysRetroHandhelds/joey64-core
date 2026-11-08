// Joey64 texture packs on parallel-RDP: GLideNHQ (GLideN64's pack reader) wrapped for rdp.cpp. Call from one thread.
#pragma once
#include <stdint.h>
#include <vector>

// dir holds <ROM name>_HIRESTEXTURES.hts (or a GLideN64 PNG pack folder named after the ROM). romname as GLideN64's
// RSP.romname: the header name with trailing spaces removed.
bool texpack_ghq_init(const char *dir, const char *romname);
void texpack_ghq_shutdown();
// The pack's texture for a GLideN64 key (Rice CRC: palette CRC << 32 | texture CRC; N64 format and size), as RGBA8
// (R in the low byte). palette: GLideN64's TexFilterPalette (512 entries), used for true CI pack textures.
void texpack_ghq_fmtstats(bool on);
void texpack_ghq_fmtstats_line(char *buf, size_t len);
bool texpack_ghq_lookup(uint64_t crc, unsigned fmt, unsigned siz, const uint16_t *palette, std::vector<uint32_t> &out,
                        unsigned &width, unsigned &height);
