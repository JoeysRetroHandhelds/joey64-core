#pragma once
#include "Types.h"

// Single-return recursive forms keep these usable as constexpr under C++11 (libretro builds).

constexpr u32 twopow(u32 dim) noexcept
{
	return dim == 0 ? 0 : (1 << dim);
}

constexpr u32 pow2_from(u32 i, u32 dim) noexcept
{
	return i < dim ? pow2_from(i << 1, dim) : i;
}

constexpr u32 pow2(u32 dim) noexcept
{
	return dim == 0 ? 0 : (dim >= 0x80000000 ? 0x80000000 : pow2_from(1, dim));
}

constexpr u32 powof_from(u32 num, u32 i, u32 dim) noexcept
{
	return num < dim ? powof_from(num << 1, i + 1, dim) : i;
}

constexpr u32 powof(u32 dim) noexcept
{
	return dim == 0 ? 0 : (dim >= 0x80000000 ? 0x1F : powof_from(2, 1, dim));
}
