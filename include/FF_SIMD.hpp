/**
 * @file FF_SIMD.hpp
 * @brief FastFHIR SIMD intrinsics — platform-detected helpers shared by the
 *        Parser and Predigestion pipeline.
 *
 * Include this header instead of directly including <immintrin.h> or
 * <arm_neon.h>.  Every function is inline and gated on the appropriate
 * architecture macro.  A scalar fallback is always provided.
 *
 * Exported symbols
 * ─────────────────
 *  ff_sum_sizes_masked          — sum sizes[] entries selected by an 8-bit mask
 *  ff_compact_dense_offset      — compact-layout field byte offset calculator
 *  ff_match_mask_u64x8          — 8-bit match mask for an array of 8 uint64_t
 *
 * Architecture detection macros defined here (do not re-define externally):
 *  FF_HAS_AVX2   — AVX2 + AVX integer instructions available
 *  FF_HAS_BMI2   — BMI2 (_bzhi_u32, _pext_u32, …) available
 *  FF_HAS_SSE41  — SSE4.1 (_mm_cmpeq_epi64, …) available
 *  FF_HAS_NEON   — ARM NEON available
 */
#pragma once

#include <cstdint>
#include <cstddef>

// ─── Architecture detection ──────────────────────────────────────────────────

#if defined(__AVX2__)
#  define FF_HAS_AVX2 1
#endif

#if defined(__BMI2__)
#  define FF_HAS_BMI2 1
#endif

#if defined(__SSE4_1__) && !defined(FF_HAS_AVX2)
// Only activate the SSE4.1 path when AVX2 is absent (AVX2 is strictly better).
#  define FF_HAS_SSE41 1
#endif

#if defined(__ARM_NEON)
#  define FF_HAS_NEON 1
#endif

// ─── Platform headers ────────────────────────────────────────────────────────

#if defined(FF_HAS_AVX2) || defined(FF_HAS_BMI2) || defined(FF_HAS_SSE41)
#  include <immintrin.h>
#endif

#if defined(FF_HAS_NEON)
#  include <arm_neon.h>
#endif

// ─── MSVC __builtin_ctz shim ─────────────────────────────────────────────────
// MSVC lacks __builtin_ctz.  _BitScanForward is undefined when v==0; all
// call sites guard with `while (v != 0)` so this is safe.

#if defined(_MSC_VER) && !defined(__clang__) && !defined(__builtin_ctz)
#  include <intrin.h>
namespace FastFHIR { namespace detail {
static inline unsigned ff_ctz_u32(uint32_t v) noexcept {
    unsigned long idx;
    _BitScanForward(&idx, v);
    return static_cast<unsigned>(idx);
}
}} // namespace FastFHIR::detail
#  define __builtin_ctz(v) ::FastFHIR::detail::ff_ctz_u32(static_cast<uint32_t>(v))
#endif

// ─────────────────────────────────────────────────────────────────────────────
// ff_sum_sizes_masked
// ─────────────────────────────────────────────────────────────────────────────
// Sum the sizes[] entries whose corresponding bit is set in `mask`.
// sizes[i] = byte width of compact-layout field i (i in [0,7]).
// Used by ff_compact_dense_offset to accumulate field byte offsets.

static inline uint32_t ff_sum_sizes_masked_scalar(const uint8_t* sizes,
                                                   uint8_t mask) noexcept {
    uint32_t sum = 0;
    uint8_t  m   = mask;
    while (m != 0) {
        const unsigned idx = static_cast<unsigned>(__builtin_ctz(m));
        sum += sizes[idx];
        m   &= m - 1u;
    }
    return sum;
}

#if defined(FF_HAS_AVX2)
// AVX2: zero-branch mask expansion + horizontal sum of 8 × int32.
static inline uint32_t ff_sum_sizes_masked_avx2(const uint8_t* sizes,
                                                 uint8_t mask) noexcept {
    const __m128i xmm_sizes   = _mm_loadl_epi64(
                                     reinterpret_cast<const __m128i*>(sizes));
    const __m256i sizes_epi32 = _mm256_cvtepu8_epi32(xmm_sizes);

    const __m256i mask_broad  = _mm256_set1_epi32(static_cast<int>(mask));
    const __m256i bit_ids     = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
    const __m256i selected    = _mm256_and_si256(mask_broad, bit_ids);
    const __m256i is_zero     = _mm256_cmpeq_epi32(selected,
                                                    _mm256_setzero_si256());
    const __m256i keep        = _mm256_xor_si256(is_zero,
                                                  _mm256_set1_epi32(-1));
    const __m256i active      = _mm256_and_si256(sizes_epi32, keep);

    __m128i lo = _mm256_castsi256_si128(active);
    __m128i hi = _mm256_extracti128_si256(active, 1);
    __m128i s  = _mm_add_epi32(lo, hi);
    s = _mm_hadd_epi32(s, s);
    s = _mm_hadd_epi32(s, s);
    return static_cast<uint32_t>(_mm_cvtsi128_si32(s));
}
#endif // FF_HAS_AVX2

#if defined(FF_HAS_NEON)
// NEON: zero-branch mask expansion using vtst; widening sum.
static inline uint32_t ff_sum_sizes_masked_neon(const uint8_t* sizes,
                                                 uint8_t mask) noexcept {
    const uint8x8_t  sizes_u8  = vld1_u8(sizes);
    const uint16x8_t sizes_u16 = vmovl_u8(sizes_u8);
    const uint32x4_t sizes_lo  = vmovl_u16(vget_low_u16(sizes_u16));
    const uint32x4_t sizes_hi  = vmovl_u16(vget_high_u16(sizes_u16));

    static const uint8_t bit_ids[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x8_t  keep_u8  = vtst_u8(vdup_n_u8(mask), vld1_u8(bit_ids));
    const int8x8_t   keep_s8  = vreinterpret_s8_u8(keep_u8);
    const int16x8_t  keep_s16 = vmovl_s8(keep_s8);
    const uint32x4_t keep_lo  = vreinterpretq_u32_s32(
                                     vmovl_s16(vget_low_s16(keep_s16)));
    const uint32x4_t keep_hi  = vreinterpretq_u32_s32(
                                     vmovl_s16(vget_high_s16(keep_s16)));

    const uint32x4_t active_lo = vandq_u32(sizes_lo, keep_lo);
    const uint32x4_t active_hi = vandq_u32(sizes_hi, keep_hi);
    return static_cast<uint32_t>(vaddvq_u32(active_lo) + vaddvq_u32(active_hi));
}
#endif // FF_HAS_NEON

// Dispatch: choose the fastest available implementation at compile time.
static inline uint32_t ff_sum_sizes_masked(const uint8_t* sizes,
                                            uint8_t mask) noexcept {
#if defined(FF_HAS_AVX2)
    return ff_sum_sizes_masked_avx2(sizes, mask);
#elif defined(FF_HAS_NEON)
    return ff_sum_sizes_masked_neon(sizes, mask);
#else
    return ff_sum_sizes_masked_scalar(sizes, mask);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// ff_compact_dense_offset
// ─────────────────────────────────────────────────────────────────────────────
// Compute the byte distance from the start of a compact-layout dense payload
// to field slot `target_index`.  Equivalent to summing sizes_table[i] for all
// i < target_index where bit i is set in the presence bitmap.

// Scalar reference (always compiled, also used as the fallback tail loop).
static inline uint64_t ff_compact_dense_offset_scalar(
    const uint8_t* presence,
    const uint8_t* sizes_table,
    size_t         target_index) noexcept
{
    uint64_t rel          = 0;
    const size_t full_bytes = target_index / 8;
    for (size_t byte_index = 0; byte_index < full_bytes; ++byte_index) {
        uint8_t mask = presence[byte_index];
        while (mask != 0) {
            const unsigned bit_index = static_cast<unsigned>(__builtin_ctz(mask));
            rel  += sizes_table[byte_index * 8 + bit_index];
            mask &= mask - 1u;
        }
    }
    const size_t tail_bits = target_index % 8;
    if (tail_bits == 0) return rel;
    const uint8_t tail_mask = static_cast<uint8_t>((1u << tail_bits) - 1u);
    uint8_t mask = static_cast<uint8_t>(presence[full_bytes] & tail_mask);
    while (mask != 0) {
        const unsigned bit_index = static_cast<unsigned>(__builtin_ctz(mask));
        rel  += sizes_table[full_bytes * 8 + bit_index];
        mask &= mask - 1u;
    }
    return rel;
}

// BMI2 fast-path: handles target_index ≤ 32 with a single _bzhi_u32.
static inline uint64_t ff_compact_dense_offset_bmi2(
    const uint8_t* presence,
    const uint8_t* sizes_table,
    size_t         target_index,
    bool&          used) noexcept
{
    used = false;
#if defined(FF_HAS_BMI2)
    if (target_index <= 32) {
        uint32_t present_word = 0;
        const size_t bytes = (target_index + 7) / 8;
        for (size_t i = 0; i < bytes; ++i)
            present_word |= static_cast<uint32_t>(presence[i]) << (8 * i);

        const uint32_t masked = _bzhi_u32(present_word,
                                           static_cast<unsigned>(target_index));
        uint64_t rel = 0;
        uint32_t bits = masked;
        while (bits != 0) {
            const unsigned idx = static_cast<unsigned>(__builtin_ctz(bits));
            rel  += sizes_table[idx];
            bits &= bits - 1u;
        }
        used = true;
        return rel;
    }
#endif
    return 0;
}

// Main dispatch: tries BMI2, then SIMD chunk-sum, then scalar tail.
static inline uint64_t ff_compact_dense_offset(
    const uint8_t* presence,
    const uint8_t* sizes_table,
    size_t         target_index) noexcept
{
    bool used_bmi2 = false;
    const uint64_t bmi2_rel = ff_compact_dense_offset_bmi2(
        presence, sizes_table, target_index, used_bmi2);
    if (used_bmi2) return bmi2_rel;

    uint64_t rel = 0;
    const size_t full_chunks = target_index / 8;
    for (size_t chunk = 0; chunk < full_chunks; ++chunk) {
        const uint8_t mask = presence[chunk];
        if (mask == 0) continue;
        rel += ff_sum_sizes_masked(&sizes_table[chunk * 8], mask);
    }

    const size_t tail_bits = target_index % 8;
    if (tail_bits == 0) return rel;

    const size_t  tail_base = full_chunks * 8;
    const uint8_t tail_mask = static_cast<uint8_t>(
        presence[full_chunks] &
        static_cast<uint8_t>((1u << tail_bits) - 1u));
    if (tail_mask != 0)
        rel += ff_sum_sizes_masked(&sizes_table[tail_base], tail_mask);

    return rel;
}

// ─────────────────────────────────────────────────────────────────────────────
// ff_match_mask_u64x8
// ─────────────────────────────────────────────────────────────────────────────
// Compare a fixed array of exactly 8 uint64_t values against `needle`.
// Returns an 8-bit bitmask: bit i is set if haystack[i] == needle.
//
// All lanes are always compared (no early-exit); callers mask the result
// with `(child_count < 8) ? ((1u << child_count) - 1u) : 0xFFu` to exclude
// uninitialised slots.
//
// Used by the predigestion radix-trie child-hash search.  SIMD paths batch
// all 8 comparisons into a single instruction group; the scalar fallback is
// a straight unrolled loop that the compiler can vectorise at -O2.

#if defined(FF_HAS_AVX2)
static inline uint8_t ff_match_mask_u64x8_avx2(const uint64_t* haystack,
                                                uint64_t needle) noexcept {
    const __m256i target = _mm256_set1_epi64x(static_cast<int64_t>(needle));

    // Low 4 lanes (indices 0–3)
    const __m256i lo  = _mm256_loadu_si256(
                            reinterpret_cast<const __m256i*>(haystack));
    const uint32_t mlo = static_cast<uint32_t>(
                            _mm256_movemask_epi8(_mm256_cmpeq_epi64(lo, target)));

    // High 4 lanes (indices 4–7)
    const __m256i hi  = _mm256_loadu_si256(
                            reinterpret_cast<const __m256i*>(haystack + 4));
    const uint32_t mhi = static_cast<uint32_t>(
                            _mm256_movemask_epi8(_mm256_cmpeq_epi64(hi, target)));

    // _mm256_movemask_epi8: per-lane match produces 8 consecutive set bits.
    // Extract one bit per lane from bit position 7 (MSB of the first matching byte).
    uint8_t r = 0;
    r |= static_cast<uint8_t>(((mlo >>  7) & 1u) << 0);
    r |= static_cast<uint8_t>(((mlo >> 15) & 1u) << 1);
    r |= static_cast<uint8_t>(((mlo >> 23) & 1u) << 2);
    r |= static_cast<uint8_t>(((mlo >> 31) & 1u) << 3);
    r |= static_cast<uint8_t>(((mhi >>  7) & 1u) << 4);
    r |= static_cast<uint8_t>(((mhi >> 15) & 1u) << 5);
    r |= static_cast<uint8_t>(((mhi >> 23) & 1u) << 6);
    r |= static_cast<uint8_t>(((mhi >> 31) & 1u) << 7);
    return r;
}
#endif // FF_HAS_AVX2

#if defined(FF_HAS_SSE41)
// SSE4.1: process pairs of uint64 lanes.
static inline uint8_t ff_match_mask_u64x8_sse41(const uint64_t* haystack,
                                                  uint64_t needle) noexcept {
    const __m128i target = _mm_set1_epi64x(static_cast<int64_t>(needle));
    uint8_t r = 0;
    for (uint32_t i = 0; i < 8; i += 2) {
        const __m128i v    = _mm_loadu_si128(
                                 reinterpret_cast<const __m128i*>(haystack + i));
        const int     mask = _mm_movemask_epi8(_mm_cmpeq_epi64(v, target));
        if (mask & 0x00FF) r |= static_cast<uint8_t>(1u << i);
        if (mask & 0xFF00) r |= static_cast<uint8_t>(1u << (i + 1));
    }
    return r;
}
#endif // FF_HAS_SSE41

#if defined(FF_HAS_NEON)
// NEON: process pairs of uint64 lanes with vceqq_u64.
static inline uint8_t ff_match_mask_u64x8_neon(const uint64_t* haystack,
                                                uint64_t needle) noexcept {
    const uint64x2_t target = vdupq_n_u64(needle);
    uint8_t r = 0;
    for (uint32_t i = 0; i < 8; i += 2) {
        const uint64x2_t v  = vld1q_u64(haystack + i);
        const uint64x2_t eq = vceqq_u64(v, target);
        if (vgetq_lane_u64(eq, 0) != 0) r |= static_cast<uint8_t>(1u << i);
        if (vgetq_lane_u64(eq, 1) != 0) r |= static_cast<uint8_t>(1u << (i + 1));
    }
    return r;
}
#endif // FF_HAS_NEON

// Scalar fallback: 8-iteration unrolled loop (compiler-vectorisable at -O2).
static inline uint8_t ff_match_mask_u64x8_scalar(const uint64_t* haystack,
                                                   uint64_t needle) noexcept {
    uint8_t r = 0;
    for (uint32_t i = 0; i < 8; ++i)
        if (haystack[i] == needle)
            r |= static_cast<uint8_t>(1u << i);
    return r;
}

// Main dispatch.
static inline uint8_t ff_match_mask_u64x8(const uint64_t* haystack,
                                            uint64_t needle) noexcept {
#if defined(FF_HAS_AVX2)
    return ff_match_mask_u64x8_avx2(haystack, needle);
#elif defined(FF_HAS_SSE41)
    return ff_match_mask_u64x8_sse41(haystack, needle);
#elif defined(FF_HAS_NEON)
    return ff_match_mask_u64x8_neon(haystack, needle);
#else
    return ff_match_mask_u64x8_scalar(haystack, needle);
#endif
}
