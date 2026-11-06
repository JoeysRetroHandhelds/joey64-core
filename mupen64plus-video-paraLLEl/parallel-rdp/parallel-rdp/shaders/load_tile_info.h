/* Copyright (c) 2020 Themaister
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef LOAD_TILE_INFO_H_
#define LOAD_TILE_INFO_H_

#ifdef TEXPACK
// Joey64 texture packs: a tile with a replacement carries it in the upper 20 bits of slo/shi/tlo/thi (the RDP's tile
// coordinates are 12 bits). Layout (Renderer::draw_shaded_primitive):
//   slo[31:12] texel offset bits 0-19      shi[19:12] texel offset bits 20-27   shi[31:20] width - 1
//   tlo[23:12] height - 1                  tlo[31:24] original width - 1, bits 0-7
//   thi[13:12] original width - 1, bits 8-9   thi[23:14] original height - 1      thi[24] present
// Returns (offset, width | height << 16, original width | original height << 16, present).
uvec4 load_tile_replace(uint index)
{
	uvec4 raw = uvec4(tile_infos.elems[index].slo, tile_infos.elems[index].shi,
	                  tile_infos.elems[index].tlo, tile_infos.elems[index].thi) >> 12u;
	uint offset = raw.x | ((raw.y & 0xffu) << 20u);
	uint w = (raw.y >> 8u) + 1u;
	uint h = (raw.z & 0xfffu) + 1u;
	uint ow = ((raw.z >> 12u) | ((raw.w & 3u) << 8u)) + 1u;
	uint oh = ((raw.w >> 2u) & 0x3ffu) + 1u;
	return uvec4(offset, w | (h << 16u), ow | (oh << 16u), (raw.w >> 12u) & 1u);
}
#endif

#ifdef TEXPACK
#define load_tile_info load_tile_info_raw
#endif
TileInfo load_tile_info(uint index)
{
#if SMALL_TYPES
	return tile_infos.elems[index];
#else
	return TileInfo(
			tile_infos.elems[index].slo,
			tile_infos.elems[index].shi,
			tile_infos.elems[index].tlo,
			tile_infos.elems[index].thi,
			tile_infos.elems[index].offset,
			tile_infos.elems[index].stride,
			u8(tile_infos.elems[index].fmt),
			u8(tile_infos.elems[index].size),
			u8(tile_infos.elems[index].palette),
			u8(tile_infos.elems[index].mask_s),
			u8(tile_infos.elems[index].shift_s),
			u8(tile_infos.elems[index].mask_t),
			u8(tile_infos.elems[index].shift_t),
			u8(tile_infos.elems[index].flags));
#endif
}

#ifdef TEXPACK
#undef load_tile_info
// The tile without the replacement bits (shaders use slo..thi in arithmetic).
TileInfo load_tile_info(uint index)
{
	TileInfo t = load_tile_info_raw(index);
	t.slo &= 0xfffu;
	t.shi &= 0xfffu;
	t.tlo &= 0xfffu;
	t.thi &= 0xfffu;
	return t;
}
#endif

#endif