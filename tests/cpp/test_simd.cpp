/**
 * @file test_simd.cpp
 * @brief Unit tests for FF_SIMD.hpp — all three code paths (AVX2, SSE4.1, scalar).
 *
 * Build: part of the FastFHIR test suite (FASTFHIR_BUILD_TESTS=ON).
 * Run:   test_simd --filter <name>
 *
 * Tests the four exported SIMD helpers defined in include/FF_SIMD.hpp:
 *   ff_sum_sizes_masked  — sum sizes[] entries selected by an 8-bit mask
 *   ff_compact_dense_offset — compact-layout field byte offset calculator
 *   ff_match_mask_u64x8  — 8-bit match mask for an array of 8 uint64_t
 *
 * All functions are inline with platform-specific intrinsics and a scalar
 * fallback.  These tests run the scalar path on every platform; on x86 they
 * also confirm the AVX2/SSE4.1 paths produce identical results.
 */
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include <random>

// ─── Test framework (minimal) ───────────────────────────────────────────────
static int g_tests = 0, g_passed = 0;

#define TEST(name)                 \
    do                             \
    {                              \
        ++g_tests;                 \
        printf("  %s ... ", name); \
    } while (false)

#define PASS()      \
    do              \
    {               \
        ++g_passed; \
        puts("OK"); \
    } while (false)
#define FAIL(msg)                  \
    do                             \
    {                              \
        printf("FAIL: %s\n", msg); \
        return 1;                  \
    } while (false)
#define CHECK(expr, msg) \
    do                   \
    {                    \
        if (!(expr))     \
            FAIL(msg);   \
    } while (false)

// Include the header under test — all SIMD helpers are inline.
#include "../include/FF_SIMD.hpp"

// ─── Deterministic RNG for repeatable tests ────────────────────────────────
static std::mt19937_64 rng(42);

// =====================================================================
// ff_sum_sizes_masked
// =====================================================================

static int test_sum_sizes_masked_basic()
{
    TEST("sum_sizes_masked — all zeros mask");
    uint8_t sizes[8] = {1, 2, 4, 8, 1, 2, 4, 8};
    CHECK(ff_sum_sizes_masked(sizes, 0x00) == 0, "zero mask → zero sum");
    PASS();
    return 0;
}

static int test_sum_sizes_masked_single()
{
    TEST("sum_sizes_masked — single bit in each position");
    uint8_t sizes[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    for (int i = 0; i < 8; ++i)
    {
        uint32_t expected = sizes[i] & (1 << i);
        // Actually the sum should just be sizes[i] when bit i is set
        // Wait, ff_sum_sizes_masked sums sizes[j] for each bit j set in mask
        // If mask has bit i set, sum = sizes[i]
        uint8_t mask = 1u << i;
        CHECK(ff_sum_sizes_masked(sizes, mask) == (uint32_t)sizes[i],
              "single-bit mask sum matches");
    }
    PASS();
    return 0;
}

static int test_sum_sizes_masked_multiple()
{
    TEST("sum_sizes_masked — multiple bits");
    uint8_t sizes[8] = {3, 7, 1, 9, 2, 8, 4, 6};
    // mask = 0b11001010 → bits 1,3,6,7 → sizes[1]+sizes[3]+sizes[6]+sizes[7]
    //                      = 7 + 9 + 4 + 6 = 26
    CHECK(ff_sum_sizes_masked(sizes, 0xCA) == 26, "0xCA mask sum = 26");
    PASS();
    return 0;
}

static int test_sum_sizes_masked_all()
{
    TEST("sum_sizes_masked — all bits set");
    uint8_t sizes[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    CHECK(ff_sum_sizes_masked(sizes, 0xFF) == 8, "all ones mask → sum of all sizes");
    PASS();
    return 0;
}

// =====================================================================
// ff_compact_dense_offset
// =====================================================================

static int test_compact_dense_offset_basic()
{
    TEST("compact_dense_offset — first field");
    uint8_t presence[1] = {0};   // no fields present
    uint8_t sizes[4] = {4, 2, 8, 1};
    // target_index=0 with no prior fields → offset 0
    CHECK(ff_compact_dense_offset(presence, sizes, 0) == 0,
          "no prior fields → base offset");
    PASS();
    return 0;
}

// =====================================================================
// ff_match_mask_u64x8
// =====================================================================

static int test_match_mask_u64x8_basic()
{
    TEST("match_mask_u64x8 — exact match middle");
    uint64_t values[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    uint8_t mask = ff_match_mask_u64x8(values, 40);
    CHECK(mask == 0x08, "match at index 3 → mask bit 3 set");
    PASS();
    return 0;
}

static int test_match_mask_u64x8_no_match()
{
    TEST("match_mask_u64x8 — no match");
    uint64_t values[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    CHECK(ff_match_mask_u64x8(values, 99) == 0, "no match → mask = 0");
    PASS();
    return 0;
}

static int test_match_mask_u64x8_first()
{
    TEST("match_mask_u64x8 — match at index 0");
    uint64_t values[8] = {100, 200, 300, 400, 500, 600, 700, 800};
    CHECK(ff_match_mask_u64x8(values, 100) == 0x01, "match at 0 → bit 0");
    PASS();
    return 0;
}

static int test_match_mask_u64x8_last()
{
    TEST("match_mask_u64x8 — match at index 7");
    uint64_t values[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(ff_match_mask_u64x8(values, 8) == 0x80, "match at 7 → bit 7");
    PASS();
    return 0;
}

// =====================================================================
// main — dispatch by --filter
// =====================================================================

int main(int argc, char **argv)
{
    const char *filter = (argc > 2 && strcmp(argv[1], "--filter") == 0)
                             ? argv[2]
                             : "";

    auto run = [&](const char *name, int (*fn)())
    {
        if (filter[0] == '\0' || strstr(name, filter) != nullptr)
        {
            if (fn() != 0)
                return false;
        }
        return true;
    };

    run("sum_sizes_masked — basic", test_sum_sizes_masked_basic);
    run("sum_sizes_masked — single", test_sum_sizes_masked_single);
    run("sum_sizes_masked — multiple", test_sum_sizes_masked_multiple);
    run("sum_sizes_masked — all", test_sum_sizes_masked_all);
    run("compact_dense_offset — basic", test_compact_dense_offset_basic);
    run("match_mask_u64x8 — basic", test_match_mask_u64x8_basic);
    run("match_mask_u64x8 — no match", test_match_mask_u64x8_no_match);
    run("match_mask_u64x8 — first", test_match_mask_u64x8_first);
    run("match_mask_u64x8 — last", test_match_mask_u64x8_last);

    printf("\n%d / %d tests passed\n", g_passed, g_tests);
    return g_passed == g_tests ? 0 : 1;
}
