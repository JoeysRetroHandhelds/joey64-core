/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - cic.c                                                   *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2014 Bobby Smiles                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "cic.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "api/callbacks.h"
#include "api/m64p_types.h"

#define __STDC_FORMAT_MACROS
#include <inttypes.h>


/* Joey64 (2026-11-12): some dumps keep the cartridge's header CRC1/CRC2 although the code under it changed (four Wii
 * Virtual Console copies: Mario Tennis USA/Europe, Mario Tennis 64, Mario Party 2 Japan). The boot code (IPL3) sums
 * ROM 0x1000-0x101000 and loops for good when the sum isn't the header's, so they hung before their first frame, as they
 * would on a console. The header's CRCs are read only by the boot code here (the ini was matched by MD5/CRC before this
 * runs), so the loaded copy gets the computed sum. Only for a CIC told by its boot code: the sum (the n64crc tool's
 * algorithm) depends on it, and a good dump always matches, so it never changes. */
void fix_boot_checksum(void* rom, size_t size)
{
    uint32_t* w = (uint32_t*)rom;
    uint64_t sum = 0;
    uint32_t seed, t1, t2, t3, t4, t5, t6, i, c1, c2;
    int cic;

    if (size < 0x101000)
        return;
    for (i = 0; i < 0xfc0/4; i++)
        sum += w[0x40/4 + i];
    switch (sum)
    {
    case UINT64_C(0x000000D0027FDF31):
    case UINT64_C(0x000000CFFB631223):
    case UINT64_C(0x000000D057C85244): cic = 6102; seed = 0xF8CA4DDC; break; /* X101, X102 */
    case UINT64_C(0x000000D6497E414B): cic = 6103; seed = 0xA3886759; break;
    case UINT64_C(0x0000011A49F60E96): cic = 6105; seed = 0xDF26F436; break;
    case UINT64_C(0x000000D6D5BE5580): cic = 6106; seed = 0x1FEA617A; break;
    default: return;
    }
    t1 = t2 = t3 = t4 = t5 = t6 = seed;
    for (i = 0x1000; i < 0x101000; i += 4) {
        uint32_t d = w[i/4], r;
        if ((uint32_t)(t6 + d) < t6)
            t4++;
        t6 += d;
        t3 ^= d;
        r = (d << (d & 0x1F)) | (d >> ((32 - (d & 0x1F)) & 0x1F));
        if ((d & 0x1F) == 0)
            r = d;
        t5 += r;
        if (t2 > d)
            t2 ^= r;
        else
            t2 ^= t6 ^ d;
        if (cic == 6105)
            t1 += w[(0x0750 + (i & 0xFF))/4] ^ d;
        else
            t1 += t5 ^ d;
    }
    if (cic == 6103)      { c1 = (t6 ^ t4) + t3; c2 = (t5 ^ t2) + t1; }
    else if (cic == 6106) { c1 = (t6 * t4) + t3; c2 = (t5 * t2) + t1; }
    else                  { c1 = t6 ^ t4 ^ t3;   c2 = t5 ^ t2 ^ t1; }
    if (w[0x10/4] == c1 && w[0x14/4] == c2)
        return;
    DebugMessage(M64MSG_WARNING, "ROM header CRC %08" PRIX32 " %08" PRIX32 " isn't its boot checksum %08" PRIX32 " %08" PRIX32
        " (CIC %d): booting with the computed one", w[0x10/4], w[0x14/4], c1, c2, cic);
    w[0x10/4] = c1;
    w[0x14/4] = c2;
}

void init_cic_using_ipl3(struct cic* cic, const void* ipl3)
{
    size_t i;
    uint64_t crc = 0;

    static const struct cic cics[] =
    {
        { "5101", CIC_5101, 0xac },
        { "X101", CIC_X101, 0x3f },
        { "X102", CIC_X102, 0x3f },
        { "X103", CIC_X103, 0x78 },
        { "X105", CIC_X105, 0x91 },
        { "X106", CIC_X106, 0x85 },
        { "5167", CIC_5167, 0xdd },
        { "8303", CIC_8303, 0xdd },
        { "8401", CIC_8401, 0xdd },
        { "8501", CIC_8501, 0xde }
    };

    for (i = 0; i < 0xfc0/4; i++)
        crc += ((uint32_t*)ipl3)[i];

    switch(crc)
    {
        default:
            DebugMessage(M64MSG_WARNING, "Unknown CIC type (%016" PRIX64 ")! using CIC 6102.", crc);
            /* fall through */
        case UINT64_C(0x000000D057C85244): i = 2; break; /* CIC_X102 */
        case UINT64_C(0x000000D0027FDF31):               /* CIC_X101 */
        case UINT64_C(0x000000CFFB631223): i = 1; break; /* CIC_X101 */
        case UINT64_C(0x000000D6497E414B): i = 3; break; /* CIC_X103 */
        case UINT64_C(0x0000011A49F60E96): i = 4; break; /* CIC_X105 */
        case UINT64_C(0x000000D6D5BE5580): i = 5; break; /* CIC_X106 */
        case UINT64_C(0x000001053BC19870): i = 6; break; /* CIC 5167 */
        case UINT64_C(0x000000A5F80BF620): i = 0; break; /* CIC 5101 */
        case UINT64_C(0x000000D2E53EF008): i = 7; break; /* CIC 8303 */
        case UINT64_C(0x000000D2E53EF39F): i = 8; break; /* CIC 8401 */
        case UINT64_C(0x000000D2E53E5DDA): i = 9; break; /* CIC 8501 */
    }

    memcpy(cic, &cics[i], sizeof(*cic));

    DebugMessage(M64MSG_INFO, "Using CIC type %s", cic->name);
}

