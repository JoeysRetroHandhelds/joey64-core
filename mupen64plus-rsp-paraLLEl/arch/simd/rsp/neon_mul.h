//
// arch/simd/rsp/neon_mul.h
//
// Native AArch64 versions of the multiply-accumulate ops that dominate RSP time (VMADN, VMADH,
// VMADM and their VMUD forms). Through SSE2NEON, each 16-bit high product costs its own pair of
// widening multiplies and every accumulator carry costs three ops (saturating add, two compares).
// Here one pair of widening multiplies gives both halves of the product, and a carry is a single
// unsigned compare of the sum against an operand. Results are bit-identical to the SSE versions
// (checked by a differential test over random inputs).
//
// This file is subject to the terms and conditions defined in
// 'LICENSE', which is part of this source code package.
//

#ifndef RSP_NEON_MUL_H
#define RSP_NEON_MUL_H

#include <arm_neon.h>

// Low and high 16 bits of the signed 32-bit products of vs and vt.
static inline void rsp_neon_smul(int16x8_t vs, int16x8_t vt, uint16x8_t *lo, int16x8_t *hi)
{
	int32x4_t p0 = vmull_s16(vget_low_s16(vs), vget_low_s16(vt));
	int32x4_t p1 = vmull_high_s16(vs, vt);
	*lo = vuzp1q_u16(vreinterpretq_u16_s32(p0), vreinterpretq_u16_s32(p1));
	*hi = vuzp2q_s16(vreinterpretq_s16_s32(p0), vreinterpretq_s16_s32(p1));
}

// acc_lo:acc_md:acc_hi += (hi:lo, with hi sign-extended), as the SSE versions of VMADN and VMADM do.
static inline void rsp_neon_acc_lo_md_hi(uint16x8_t lo, int16x8_t hi, __m128i *acc_lo, __m128i *acc_md,
                                         __m128i *acc_hi)
{
	uint16x8_t sum_lo = vaddq_u16(vreinterpretq_u16_s32(*acc_lo), lo);
	uint16x8_t carry = vcltq_u16(sum_lo, lo); // all ones on carry
	hi = vsubq_s16(hi, vreinterpretq_s16_u16(carry));

	uint16x8_t uhi = vreinterpretq_u16_s16(hi);
	uint16x8_t sum_md = vaddq_u16(vreinterpretq_u16_s32(*acc_md), uhi);
	carry = vcltq_u16(sum_md, uhi);

	int16x8_t sum_hi = vaddq_s16(vreinterpretq_s16_s32(*acc_hi), vshrq_n_s16(hi, 15));
	sum_hi = vsubq_s16(sum_hi, vreinterpretq_s16_u16(carry));

	*acc_lo = vreinterpretq_s32_u16(sum_lo);
	*acc_md = vreinterpretq_s32_u16(sum_md);
	*acc_hi = vreinterpretq_s32_s16(sum_hi);
}

// Signed saturation of acc_hi:acc_md to 16 bits.
static inline __m128i rsp_neon_sclamp_acc_tomd(__m128i acc_md, __m128i acc_hi)
{
	int16x8_t md = vreinterpretq_s16_s32(acc_md), hi = vreinterpretq_s16_s32(acc_hi);
	int32x4_t l = vreinterpretq_s32_s16(vzip1q_s16(md, hi));
	int32x4_t h = vreinterpretq_s32_s16(vzip2q_s16(md, hi));
	return vreinterpretq_s32_s16(vqmovn_high_s32(vqmovn_s32(l), h));
}

// Unsigned clamp of the accumulator to val (acc_lo), as rsp_uclamp_acc.
static inline __m128i rsp_neon_uclamp_acc(__m128i val, __m128i acc_md, __m128i acc_hi)
{
	int16x8_t md = vreinterpretq_s16_s32(acc_md), hi = vreinterpretq_s16_s32(acc_hi);
	int16x8_t hi_negative = vshrq_n_s16(hi, 15);
	uint16x8_t keep = vandq_u16(vceqq_s16(hi_negative, hi), vceqq_s16(hi_negative, vshrq_n_s16(md, 15)));
	uint16x8_t clamped = vreinterpretq_u16_s16(vmvnq_s16(hi_negative));
	return vreinterpretq_s32_u16(vbslq_u16(keep, vreinterpretq_u16_s32(val), clamped));
}

// VMADN / VMUDN: unsigned vs * signed vt.
template <bool VMADN>
static inline __m128i rsp_vmadn_vmudn(__m128i vs_, __m128i vt_, __m128i, __m128i *acc_lo, __m128i *acc_md,
                                      __m128i *acc_hi)
{
	int16x8_t vs = vreinterpretq_s16_s32(vs_), vt = vreinterpretq_s16_s32(vt_);
	uint16x8_t lo;
	int16x8_t hi;
	rsp_neon_smul(vs, vt, &lo, &hi);
	// The signed product is short by vt << 16 where vs has its top bit set.
	hi = vaddq_s16(hi, vandq_s16(vt, vshrq_n_s16(vs, 15)));

	if (VMADN)
	{
		rsp_neon_acc_lo_md_hi(lo, hi, acc_lo, acc_md, acc_hi);
		return rsp_neon_uclamp_acc(*acc_lo, *acc_md, *acc_hi);
	}

	*acc_lo = vreinterpretq_s32_u16(lo);
	*acc_md = vreinterpretq_s32_s16(hi);
	*acc_hi = vreinterpretq_s32_s16(vshrq_n_s16(hi, 15));
	return *acc_lo;
}

// VMADM / VMUDM: signed vs * unsigned vt.
template <bool VMADM>
static inline __m128i rsp_vmadm_vmudm(__m128i vs_, __m128i vt_, __m128i, __m128i *acc_lo, __m128i *acc_md,
                                      __m128i *acc_hi)
{
	int16x8_t vs = vreinterpretq_s16_s32(vs_), vt = vreinterpretq_s16_s32(vt_);
	uint16x8_t lo;
	int16x8_t hi;
	rsp_neon_smul(vs, vt, &lo, &hi);
	// The signed product is short by vs << 16 where vt has its top bit set.
	hi = vaddq_s16(hi, vandq_s16(vs, vshrq_n_s16(vt, 15)));

	if (VMADM)
	{
		rsp_neon_acc_lo_md_hi(lo, hi, acc_lo, acc_md, acc_hi);
		return rsp_neon_sclamp_acc_tomd(*acc_md, *acc_hi);
	}

	*acc_lo = vreinterpretq_s32_u16(lo);
	*acc_md = vreinterpretq_s32_s16(hi);
	*acc_hi = vreinterpretq_s32_s16(vshrq_n_s16(hi, 15));
	return *acc_md;
}

// VMADH / VMUDH: signed vs * signed vt, accumulated into acc_md:acc_hi.
template <bool VMADH>
static inline __m128i rsp_vmadh_vmudh(__m128i vs_, __m128i vt_, __m128i zero, __m128i *acc_lo, __m128i *acc_md,
                                      __m128i *acc_hi)
{
	uint16x8_t lo;
	int16x8_t hi;
	rsp_neon_smul(vreinterpretq_s16_s32(vs_), vreinterpretq_s16_s32(vt_), &lo, &hi);

	if (VMADH)
	{
		uint16x8_t sum_md = vaddq_u16(vreinterpretq_u16_s32(*acc_md), lo);
		uint16x8_t carry = vcltq_u16(sum_md, lo);
		hi = vsubq_s16(hi, vreinterpretq_s16_u16(carry));
		*acc_md = vreinterpretq_s32_u16(sum_md);
		*acc_hi = vreinterpretq_s32_s16(vaddq_s16(vreinterpretq_s16_s32(*acc_hi), hi));
	}
	else
	{
		*acc_lo = zero;
		*acc_md = vreinterpretq_s32_u16(lo);
		*acc_hi = vreinterpretq_s32_s16(hi);
	}

	return rsp_neon_sclamp_acc_tomd(*acc_md, *acc_hi);
}

#endif
