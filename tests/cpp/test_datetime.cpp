/**
 * @file test_datetime.cpp
 * @brief Serialization/deserialization tests for the packed date/time slot
 *        (TASKS.md DT-1.3): FF_PACK_DATETIME / FF_UNPACK_DATETIME, the
 *        FF_PARSE_DATETIME / FF_FORMAT_DATETIME text codec, and the
 *        SIZE_/STORE_/ENCODE_FF_DATETIME emitter triple.
 *
 * The contract under test is byte-exactness: whatever text goes in comes back
 * out unchanged, whether it packed into the 8-byte slot or spilled to an
 * FF_STRING. Every case therefore runs through the SAME path a real writer and
 * reader use -- encode into an arena slot, then decode that slot -- rather than
 * calling the parser and formatter back to back, which would not exercise the
 * discriminator, the relative offset, or the fallback block at all.
 *
 * THE EPOCH STRADDLE IS THE INTERESTING BOUNDARY, and it is a SIGNEDNESS
 * boundary, not a branch. Days are stored UNSIGNED from 0001-01-01, while the
 * civil conversion underneath is Hinnant's, which counts from 1970-01-01. The
 * shift between the two therefore crosses zero: `ff_days_from_civil` returns a
 * negative count for every date before 1970, and `ff_datetime_civil_from_days`
 * hands `ff_civil_from_days` a negative argument. Roughly 78% of the
 * representable range is on that side, so it is not an edge case.
 *
 * Hinnant's own negative-era branches are NOT what is at risk here: the
 * algorithm adds 719,468 before dividing, so within years 0001..9999 its
 * internal `z` is always positive and those branches are unreachable. Deleting
 * them changes nothing, which was confirmed by mutation. What IS at risk is the
 * epoch shift itself -- computing it in unsigned arithmetic wraps for every
 * pre-1970 date. That mutation was also run: it turns 0001-01-01 into
 * 9222-01-21 and fails 22 assertions below while every post-1970 case stays
 * green. The tests in [CivilEpoch] exist for that bug specifically.
 *
 * Dates are SAMPLED, not enumerated: the calendar is regular, so a few thousand
 * random dates either side of 1970 buy what walking all 3,652,059 days buys.
 * Precision is not regular and IS enumerated -- every sample runs at every
 * precision its tag admits, YEAR through FRAC3, because each level renders a
 * different slice of the same packed word.
 *
 * Run: ff_test_datetime [--filter <name>] [--seed <n>]
 *   The seed is fixed by default and printed on every run, so the suite cannot
 *   flake and a red log always names the command that reproduces it.
 * Exit code: 0 = all pass, non-zero = failures.
 */

#include <FF_Primitives.hpp>
#include <FF_Ops.hpp>
#include <FF_Utilities.hpp>

#include "FFHR_tests.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace FastFHIR;

// ── Test framework (matches the other standalone suites) ───────────────────

// Fixed by default so a failure is reproducible and CI cannot flake; --seed
// varies it deliberately. The seed is printed on every run, so a red CI log
// always carries the command that reproduces it.
static uint32_t g_seed = 20260820u;

// ── The round trip under test ──────────────────────────────────────────────

// A block sitting well inside the arena, so a fallback FF_STRING can be placed
// either after it (positive relative offset) or before it (negative).
static constexpr Offset TEST_BLOCK_OFFSET = 4096;

struct Encoded
{
    uint64_t slot = FF_DATETIME_NULL;
    bool packed = false;   ///< inline in the slot rather than an FF_STRING
    std::string decoded;   ///< what a reader gets back out
};

/// Decode exactly as a reader must: null, then the discriminator, then either
/// the packed value or the FF_STRING the relative offset names. This mirrors
/// what DT-3's export path will do, so a bug here is a bug there.
static std::string decode_slot(const BYTE *base, Offset block_offset, uint64_t slot,
                               RECOVERY_TAG tag)
{
    if (slot == FF_DATETIME_NULL) return {};
    if (FF_DATETIME_IS_FALLBACK(slot))
    {
        const Offset str = FF_ResolveDateTimeOffset(slot, block_offset);
        const uint32_t len = FF_GET_STRING_LENGTH(base, str);
        return std::string(reinterpret_cast<const char *>(base + str + FF_STRING::STRING_DATA),
                           len);
    }
    return FF_FORMAT_DATETIME(FF_UNPACK_DATETIME(slot), tag);
}

/// Serialize `text` into a slot, then deserialize that slot back to text.
/// `child_off` walks forward through the arena exactly as the Builder's does.
static Encoded roundtrip(std::vector<BYTE> &arena, Offset &child_off, std::string_view text,
                         RECOVERY_TAG tag)
{
    Encoded e;
    e.slot = ENCODE_FF_DATETIME(arena.data(), TEST_BLOCK_OFFSET, child_off, text, tag);
    e.packed = (e.slot != FF_DATETIME_NULL) && !FF_DATETIME_IS_FALLBACK(e.slot);
    e.decoded = decode_slot(arena.data(), TEST_BLOCK_OFFSET, e.slot, tag);
    return e;
}

// Shared arena for the bulk cases. Big enough that no test exhausts it; each
// helper below asserts its own child_off stayed in bounds.
static std::vector<BYTE> g_arena(1u << 20, 0);
static Offset g_child = TEST_BLOCK_OFFSET + 512;

static Encoded rt(std::string_view text, RECOVERY_TAG tag)
{
    if (g_child + 1024 >= g_arena.size()) g_child = TEST_BLOCK_OFFSET + 512;   // recycle
    return roundtrip(g_arena, g_child, text, tag);
}

/// Text that must pack inline AND come back identical.
static void expect_packed(std::string_view text, RECOVERY_TAG tag)
{
    const Encoded e = rt(text, tag);
    CHECK(e.packed, "'" << text << "' should pack inline, not spill to an FF_STRING");
    CHECK_EQ(e.decoded, std::string(text), "'" << text << "' round trip");
}

/// Text that must spill to an FF_STRING AND still come back identical -- the
/// fallback is not a data-loss path, it is the byte-exact path for values the
/// 63 bits cannot hold or that are not legal for the tag.
static void expect_fallback(std::string_view text, RECOVERY_TAG tag)
{
    const Encoded e = rt(text, tag);
    CHECK(!e.packed, "'" << text << "' should take the FF_STRING fallback");
    CHECK(FF_DATETIME_IS_FALLBACK(e.slot), "'" << text << "' slot should have bit 63 set");
    CHECK_EQ(e.decoded, std::string(text), "'" << text << "' fallback preserves the text");
}

// ── Precision ladder ───────────────────────────────────────────────────────

static void test_precision_levels()
{
    expect_packed("2024", RECOVER_FF_DATETIME);
    expect_packed("2024-01", RECOVER_FF_DATETIME);
    expect_packed("2024-01-15", RECOVER_FF_DATETIME);
    expect_packed("2024-01-15T13:45:30Z", RECOVER_FF_DATETIME);
    expect_packed("2024-01-15T13:45:30.5Z", RECOVER_FF_DATETIME);
    expect_packed("2024-01-15T13:45:30.50Z", RECOVER_FF_DATETIME);
    expect_packed("2024-01-15T13:45:30.500Z", RECOVER_FF_DATETIME);

    // 'date' admits the three date precisions and nothing finer.
    expect_packed("2024", RECOVER_FF_DATE);
    expect_packed("2024-01", RECOVER_FF_DATE);
    expect_packed("2024-01-15", RECOVER_FF_DATE);

    // 'instant' admits only second-or-finer, always with a timezone.
    expect_packed("2024-01-15T13:45:30Z", RECOVER_FF_INSTANT);
    expect_packed("2024-01-15T13:45:30.123Z", RECOVER_FF_INSTANT);

    // 'time' is a duration since midnight: no date, no timezone.
    expect_packed("13:45:30", RECOVER_FF_TIME);
    expect_packed("13:45:30.2", RECOVER_FF_TIME);
    expect_packed("13:45:30.25", RECOVER_FF_TIME);
    expect_packed("13:45:30.250", RECOVER_FF_TIME);
}

static void test_precision_is_preserved_not_normalised()
{
    // "Born in 1970" is not "born 1 Jan 1970". If precision were dropped these
    // would all decode identically, which is the defect the 3-bit field exists
    // to prevent -- so assert the SLOTS differ, not merely the text.
    const uint64_t year = rt("1970", RECOVER_FF_DATETIME).slot;
    const uint64_t month = rt("1970-01", RECOVER_FF_DATETIME).slot;
    const uint64_t day = rt("1970-01-01", RECOVER_FF_DATETIME).slot;
    CHECK(year != month, "'1970' and '1970-01' must not share a packed value");
    CHECK(month != day, "'1970-01' and '1970-01-01' must not share a packed value");
    CHECK(year != day, "'1970' and '1970-01-01' must not share a packed value");

    // Same for trailing fractional zeros: one instant, three texts.
    const uint64_t f1 = rt("2024-01-15T13:45:30.5Z", RECOVER_FF_DATETIME).slot;
    const uint64_t f2 = rt("2024-01-15T13:45:30.50Z", RECOVER_FF_DATETIME).slot;
    const uint64_t f3 = rt("2024-01-15T13:45:30.500Z", RECOVER_FF_DATETIME).slot;
    CHECK(f1 != f2 && f2 != f3 && f1 != f3, "fractional digit count must survive");
}

// ── The epoch straddle ─────────────────────────────────────────────────────

static void test_epoch_shift_crosses_zero()
{
    // Pin the signedness the dates below depend on, rather than assuming they
    // reach it. If these ever come back unsigned, every case in this group is
    // still "passing" against arithmetic that silently wrapped.
    CHECK(ff_days_from_civil(1969, 12, 31) < 0, "1969-12-31 must count negative from 1970");
    CHECK(ff_days_from_civil(1, 1, 1) < 0, "0001-01-01 must count negative from 1970");
    CHECK(ff_days_from_civil(1970, 1, 1) == 0, "1970-01-01 is the civil-helper origin");
    CHECK(ff_days_from_civil(1970, 1, 2) > 0, "1970-01-02 must count positive");

    // ...while the STORED day number is unsigned and 0 only at 0001-01-01.
    CHECK_EQ(ff_datetime_days_from_civil(1, 1, 1), 0u, "stored epoch is 0001-01-01");
    CHECK(ff_datetime_days_from_civil(1970, 1, 1) > 0, "1970 is not the stored epoch");
    CHECK_EQ(ff_datetime_days_from_civil(9999, 12, 31), FF_DATETIME_MAX_DAYS, "last day");
}

static void test_epoch_boundary_dates()
{
    static const char *const AROUND[] = {
        "1969-12-30", "1969-12-31", "1970-01-01", "1970-01-02", "1970-01-03",
    };
    for (const char *d : AROUND)
    {
        expect_packed(d, RECOVER_FF_DATE);
        expect_packed(d, RECOVER_FF_DATETIME);
    }

    static const char *const BEFORE[] = {
        "0001-01-01",   // the stored epoch itself
        "0001-01-02",
        "0001-12-31",
        "0004-02-29",   // first leap year
        "0100-02-28",   // century, not a leap year
        "0400-02-29",   // century divisible by 400 -- is a leap year
        "1000-06-15",
        "1582-10-15",   // Gregorian adoption; proleptic here, must not be special
        "1899-12-31",
        "1900-02-28",   // 1900 is NOT a leap year
        "1900-03-01",
        "1969-07-20",
    };
    for (const char *d : BEFORE)
    {
        CHECK(ff_days_from_civil(std::atoi(d), 1, 1) < 0, d << " should be pre-1970");
        expect_packed(d, RECOVER_FF_DATE);
    }

    static const char *const AFTER[] = {
        "1970-01-01", "1999-12-31", "2000-02-29",   // 2000 IS a leap year
        "2024-02-29", "2100-02-28",                 // 2100 is not
        "2038-01-19",                               // the 32-bit time_t cliff, irrelevant here
        "9999-12-30", "9999-12-31",
    };
    for (const char *d : AFTER) expect_packed(d, RECOVER_FF_DATE);

    expect_packed("1969-07-20T20:17:40Z", RECOVER_FF_INSTANT);
    expect_packed("1969-07-20T20:17:40.000Z", RECOVER_FF_INSTANT);
    expect_packed("1969-07-20T15:17:40-05:00", RECOVER_FF_INSTANT);
    expect_packed("0001-01-01T00:00:00Z", RECOVER_FF_INSTANT);
    expect_packed("9999-12-31T23:59:59.999+14:00", RECOVER_FF_INSTANT);
}

// ── Calendar sweeps ────────────────────────────────────────────────────────

/// The test's OWN calendar. A sampled date must not be derived from the code
/// under test, or the sample and the implementation agree by construction and
/// the round trip proves nothing.
static uint32_t ref_days_in_month(uint32_t y, uint32_t m)
{
    static const uint32_t DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return (m == 2 && leap) ? 29u : DAYS[m - 1];
}

struct Sample
{
    uint32_t y = 1, m = 1, d = 1;
    uint32_t hh = 0, mm = 0, ss = 0;
    char frac[4] = {'0', '0', '0', '\0'};   ///< three digits; a prefix is taken per precision
    int16_t offset = 0;                     ///< minutes
    bool use_z = false;                     ///< spell a zero offset as 'Z'
};

/// Render a sample at one precision, in the spelling the tag requires. This is
/// the EXPECTED text: the round trip must reproduce it byte for byte.
static std::string make_text(const Sample &s, FF_DateTimePrecision prec, RECOVERY_TAG tag)
{
    const uint8_t rank = static_cast<uint8_t>(prec);
    const uint8_t second_rank = static_cast<uint8_t>(FF_DateTimePrecision::SECOND);
    char buf[48];
    int n = 0;

    if (tag == RECOVER_FF_TIME)
    {
        n = std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u", s.hh, s.mm, s.ss);
    }
    else
    {
        n = std::snprintf(buf, sizeof(buf), "%04u", s.y);
        if (rank >= static_cast<uint8_t>(FF_DateTimePrecision::YEAR_MONTH))
            n += std::snprintf(buf + n, sizeof(buf) - n, "-%02u", s.m);
        if (rank >= static_cast<uint8_t>(FF_DateTimePrecision::DATE))
            n += std::snprintf(buf + n, sizeof(buf) - n, "-%02u", s.d);
        if (rank < second_rank) return std::string(buf, n);   // no time, no zone
        n += std::snprintf(buf + n, sizeof(buf) - n, "T%02u:%02u:%02u", s.hh, s.mm, s.ss);
    }

    if (rank > second_rank)
    {
        n += std::snprintf(buf + n, sizeof(buf) - n, ".%.*s", rank - second_rank, s.frac);
    }
    if (tag == RECOVER_FF_TIME) return std::string(buf, n);   // 'time' carries no zone

    if (s.use_z)
    {
        n += std::snprintf(buf + n, sizeof(buf) - n, "Z");
    }
    else
    {
        const int magnitude = s.offset < 0 ? -s.offset : s.offset;
        n += std::snprintf(buf + n, sizeof(buf) - n, "%c%02d:%02d",
                           s.offset < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
    }
    return std::string(buf, n);
}

// Which precisions each tag legally admits. 'date' stops at DATE; 'instant' and
// 'time' start at SECOND; 'dateTime' spans the union, which is the whole point
// of it being a union type.
static const FF_DateTimePrecision ALL_PRECISIONS[] = {
    FF_DateTimePrecision::YEAR,   FF_DateTimePrecision::YEAR_MONTH,
    FF_DateTimePrecision::DATE,   FF_DateTimePrecision::SECOND,
    FF_DateTimePrecision::FRAC1,  FF_DateTimePrecision::FRAC2,
    FF_DateTimePrecision::FRAC3,
};

static bool tag_admits(RECOVERY_TAG tag, FF_DateTimePrecision p)
{
    const uint8_t rank = static_cast<uint8_t>(p);
    const uint8_t date_rank = static_cast<uint8_t>(FF_DateTimePrecision::DATE);
    const uint8_t second_rank = static_cast<uint8_t>(FF_DateTimePrecision::SECOND);
    if (tag == RECOVER_FF_DATE) return rank <= date_rank;
    if (tag == RECOVER_FF_INSTANT || tag == RECOVER_FF_TIME) return rank >= second_rank;
    return true;   // dateTime spans YEAR..FRAC3
}

static const char *precision_name(FF_DateTimePrecision p)
{
    switch (p)
    {
        case FF_DateTimePrecision::YEAR:       return "YEAR";
        case FF_DateTimePrecision::YEAR_MONTH: return "YEAR_MONTH";
        case FF_DateTimePrecision::DATE:       return "DATE";
        case FF_DateTimePrecision::SECOND:     return "SECOND";
        case FF_DateTimePrecision::FRAC1:      return "FRAC1";
        case FF_DateTimePrecision::FRAC2:      return "FRAC2";
        case FF_DateTimePrecision::FRAC3:      return "FRAC3";
    }
    return "?";
}

/// Randomised coverage. Dates are sampled either side of 1970 rather than
/// enumerated -- the calendar is regular, so a few thousand samples across both
/// signs of the epoch shift buy what walking all 3.6M days buys, in a fraction
/// of the time. Every sample is exercised at EVERY precision the tag admits,
/// because precision is the axis that is NOT regular: each level renders a
/// different amount of the same packed word, and a bug at FRAC2 is invisible at
/// FRAC3.
static void test_random_dates_all_precisions()
{
    static constexpr size_t SAMPLES_PER_BUCKET = 1500;
    struct Bucket { const char *name; uint32_t lo_year, hi_year; };
    static const Bucket BUCKETS[] = {
        {"pre-1970", 1, 1969},      // the epoch shift is negative here
        {"post-1970", 1970, 9999},  // and non-negative here
    };
    static const RECOVERY_TAG TAGS[] = {
        RECOVER_FF_DATE, RECOVER_FF_DATETIME, RECOVER_FF_INSTANT, RECOVER_FF_TIME,
    };

    std::mt19937 rng(g_seed);
    size_t covered[4][7] = {};

    for (const Bucket &bucket : BUCKETS)
    {
        size_t bad = 0, checked = 0;
        std::string first_failure;

        for (size_t i = 0; i < SAMPLES_PER_BUCKET; ++i)
        {
            Sample s;
            s.y = std::uniform_int_distribution<uint32_t>(bucket.lo_year, bucket.hi_year)(rng);
            s.m = std::uniform_int_distribution<uint32_t>(1, 12)(rng);
            s.d = std::uniform_int_distribution<uint32_t>(1, ref_days_in_month(s.y, s.m))(rng);
            s.hh = std::uniform_int_distribution<uint32_t>(0, 23)(rng);
            s.mm = std::uniform_int_distribution<uint32_t>(0, 59)(rng);
            s.ss = std::uniform_int_distribution<uint32_t>(0, 60)(rng);   // 60 = leap second
            for (int f = 0; f < 3; ++f)
                s.frac[f] = static_cast<char>('0' + std::uniform_int_distribution<int>(0, 9)(rng));
            s.offset = static_cast<int16_t>(std::uniform_int_distribution<int>(
                FF_DATETIME_OFFSET_MIN, FF_DATETIME_OFFSET_MAX)(rng));
            s.use_z = std::uniform_int_distribution<int>(0, 3)(rng) == 0;

            for (size_t t = 0; t < 4; ++t)
            {
                for (size_t p = 0; p < 7; ++p)
                {
                    if (!tag_admits(TAGS[t], ALL_PRECISIONS[p])) continue;
                    ++covered[t][p];
                    ++checked;

                    const std::string text = make_text(s, ALL_PRECISIONS[p], TAGS[t]);
                    const Encoded e = rt(text, TAGS[t]);
                    if (e.packed && e.decoded == text) continue;
                    if (bad == 0)
                    {
                        first_failure = text + " (tag " + std::to_string(TAGS[t]) + ", " +
                                        precision_name(ALL_PRECISIONS[p]) + ") -> " +
                                        (e.packed ? e.decoded : "FALLBACK");
                    }
                    ++bad;
                }
            }
        }
        CHECK_EQ(bad, size_t{0}, bucket.name << ": " << checked
                                             << " round trips, first failure: " << first_failure);
    }

    // The sampler is only as good as its coverage: assert every legal
    // (tag, precision) cell was actually reached, so a hole cannot hide behind
    // a green run.
    for (size_t t = 0; t < 4; ++t)
    {
        for (size_t p = 0; p < 7; ++p)
        {
            const bool legal = tag_admits(TAGS[t], ALL_PRECISIONS[p]);
            CHECK(legal == (covered[t][p] > 0),
                  "tag " << TAGS[t] << " x " << precision_name(ALL_PRECISIONS[p])
                         << (legal ? " was never exercised" : " was exercised but is illegal"));
        }
    }
}

static void test_named_boundary_days()
{
    // The specific days a random sampler is unlikely to land on, and where a
    // calendar bug would actually live. Each is run at every precision its tag
    // admits, via the same path as the random cases.
    static const Sample DAYS[] = {
        {1, 1, 1, 0, 0, 0, {'0', '0', '0', '\0'}, 0, true},        // stored epoch
        {1, 12, 31, 23, 59, 59, {'9', '9', '9', '\0'}, 0, false},
        {4, 2, 29, 12, 0, 0, {'5', '0', '0', '\0'}, -300, false},  // first leap year
        {100, 2, 28, 12, 0, 0, {'1', '2', '3', '\0'}, 0, true},    // century, not leap
        {400, 2, 29, 12, 0, 0, {'1', '2', '3', '\0'}, 0, true},    // /400, is leap
        {1900, 2, 28, 12, 0, 0, {'0', '0', '1', '\0'}, 60, false}, // 1900 not leap
        {1969, 12, 31, 23, 59, 60, {'9', '9', '9', '\0'}, 0, true},// last pre-epoch day
        {1970, 1, 1, 0, 0, 0, {'0', '0', '0', '\0'}, 0, true},     // civil epoch
        {2000, 2, 29, 12, 0, 0, {'5', '0', '0', '\0'}, 840, false},// /400, is leap
        {2024, 2, 29, 12, 0, 0, {'5', '0', '0', '\0'}, -840, false},
        {2100, 2, 28, 12, 0, 0, {'0', '0', '0', '\0'}, 0, true},   // century, not leap
        {9999, 12, 31, 23, 59, 59, {'9', '9', '9', '\0'}, 0, true},// last stored day
    };
    static const RECOVERY_TAG TAGS[] = {
        RECOVER_FF_DATE, RECOVER_FF_DATETIME, RECOVER_FF_INSTANT, RECOVER_FF_TIME,
    };

    for (const Sample &s : DAYS)
    {
        for (RECOVERY_TAG tag : TAGS)
        {
            for (FF_DateTimePrecision p : ALL_PRECISIONS)
            {
                if (!tag_admits(tag, p)) continue;
                expect_packed(make_text(s, p, tag), tag);
            }
        }
    }
}

static void test_time_of_day_sweep()
{
    size_t bad = 0;
    for (uint32_t h = 0; h < 24; ++h)
    {
        for (uint32_t m = 0; m < 60; ++m)
        {
            for (uint32_t s = 0; s < 61; ++s)   // 60 is a legal FHIR leap second
            {
                FF_DateTimeParts p;
                p.hour = static_cast<uint8_t>(h);
                p.minute = static_cast<uint8_t>(m);
                p.second = static_cast<uint8_t>(s);
                p.precision = FF_DateTimePrecision::SECOND;

                const uint64_t slot = FF_PACK_DATETIME(p);
                const std::string text = FF_FORMAT_DATETIME(FF_UNPACK_DATETIME(slot),
                                                            RECOVER_FF_TIME);
                const auto back = FF_PARSE_DATETIME(text, RECOVER_FF_TIME);
                if (!back || back->hour != h || back->minute != m || back->second != s) ++bad;
                if (text.size() != 8) ++bad;   // hh:mm:ss, always zero-padded
            }
        }
    }
    CHECK_EQ(bad, size_t{0}, "24*60*61 = 87,840 times round-tripped");

    bad = 0;
    for (uint32_t ms = 0; ms < 1000; ++ms)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "12:00:00.%03u", ms);
        const Encoded e = rt(buf, RECOVER_FF_TIME);
        if (!e.packed || e.decoded != buf) ++bad;
    }
    CHECK_EQ(bad, size_t{0}, "1,000 millisecond values round-tripped");
}

static void test_utc_offset_sweep()
{
    size_t bad = 0, checked = 0;
    for (int minutes = FF_DATETIME_OFFSET_MIN; minutes <= FF_DATETIME_OFFSET_MAX;
         ++minutes, ++checked)
    {
        const int magnitude = minutes < 0 ? -minutes : minutes;
        char buf[40];
        std::snprintf(buf, sizeof(buf), "2024-01-15T13:45:30%c%02d:%02d",
                      minutes < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
        const Encoded e = rt(buf, RECOVER_FF_INSTANT);
        if (!e.packed || e.decoded != buf) ++bad;
    }
    CHECK_EQ(bad, size_t{0}, checked << " UTC offsets round-tripped");

    const Encoded z = rt("2024-01-15T13:45:30Z", RECOVER_FF_INSTANT);
    const Encoded plus = rt("2024-01-15T13:45:30+00:00", RECOVER_FF_INSTANT);
    CHECK(z.packed && plus.packed, "both spellings must pack");
    CHECK(z.slot != plus.slot, "the spare offset code must keep them distinct");
    CHECK_EQ(z.decoded, std::string("2024-01-15T13:45:30Z"), "Z stays Z");
    CHECK_EQ(plus.decoded, std::string("2024-01-15T13:45:30+00:00"), "+00:00 stays +00:00");
    CHECK_EQ(FF_UNPACK_DATETIME(z.slot).utc_offset, FF_DATETIME_OFFSET_Z, "Z sentinel");
    CHECK_EQ(FF_UNPACK_DATETIME(plus.slot).utc_offset, int16_t{0}, "+00:00 is numeric zero");
}

// ── Canonical form ─────────────────────────────────────────────────────────

static void test_equal_values_have_equal_bits()
{
    // The claim that comparison becomes an integer compare only holds if fields
    // the tag does not use are zero. Two independent encodes of the same text
    // must therefore produce the identical word.
    CHECK_EQ(rt("2024-01-15", RECOVER_FF_DATE).slot, rt("2024-01-15", RECOVER_FF_DATE).slot,
             "same date, same bits");
    CHECK_EQ(rt("13:45:30", RECOVER_FF_TIME).slot, rt("13:45:30", RECOVER_FF_TIME).slot,
             "same time, same bits");

    const FF_DateTimeParts date = FF_UNPACK_DATETIME(rt("2024-01-15", RECOVER_FF_DATE).slot);
    CHECK_EQ(date.hour, uint8_t{0}, "'date' carries no hour");
    CHECK_EQ(date.minute, uint8_t{0}, "'date' carries no minute");
    CHECK_EQ(date.second, uint8_t{0}, "'date' carries no second");
    CHECK_EQ(date.millisecond, uint16_t{0}, "'date' carries no fraction");
    CHECK_EQ(date.utc_offset, int16_t{0}, "'date' never carries a timezone");

    const FF_DateTimeParts time = FF_UNPACK_DATETIME(rt("13:45:30", RECOVER_FF_TIME).slot);
    CHECK_EQ(time.days, uint32_t{0}, "'time' carries no date");
    CHECK_EQ(time.utc_offset, int16_t{0}, "'time' never carries a timezone");

    // Days sit at the top of the payload, so this holds for same-offset values.
    // It deliberately does NOT claim chronological ordering across offsets.
    CHECK(rt("1969-12-31", RECOVER_FF_DATE).slot < rt("1970-01-01", RECOVER_FF_DATE).slot,
          "the epoch boundary must not invert the ordering");
    CHECK(rt("0001-01-01", RECOVER_FF_DATE).slot < rt("9999-12-31", RECOVER_FF_DATE).slot,
          "first day sorts before last day");
}

// ── Fallback ───────────────────────────────────────────────────────────────

static void test_fallback_preserves_text()
{
    expect_fallback("2024-01-15T13:45:30.5000Z", RECOVER_FF_DATETIME);      // 4 digits
    expect_fallback("2024-01-15T13:45:30.123456Z", RECOVER_FF_DATETIME);    // 6 digits
    expect_fallback("2024-01-15T13:45:30.123456789Z", RECOVER_FF_INSTANT);  // 9 digits

    expect_fallback("2024-01-15T13:45:30Z", RECOVER_FF_DATE);   // 'date' has no time
    expect_fallback("2024-01-15", RECOVER_FF_INSTANT);          // 'instant' needs one
    expect_fallback("2024-01-15T13:45:30", RECOVER_FF_DATETIME);// T without a timezone
    expect_fallback("13:45:30Z", RECOVER_FF_TIME);              // 'time' has no timezone
    expect_fallback("2024-01-15T13:45:30Z", RECOVER_FF_TIME);   // 'time' has no date

    expect_fallback("2024-02-31", RECOVER_FF_DATE);   // would normalise to 03-02
    expect_fallback("2023-02-29", RECOVER_FF_DATE);   // not a leap year
    expect_fallback("1900-02-29", RECOVER_FF_DATE);   // century, not a leap year
    expect_fallback("2024-1-5", RECOVER_FF_DATE);     // unpadded
    expect_fallback("2024-13-01", RECOVER_FF_DATE);
    expect_fallback("2024-00-01", RECOVER_FF_DATE);
    expect_fallback("2024-01-00", RECOVER_FF_DATE);
    expect_fallback("0000-01-01", RECOVER_FF_DATE);   // year 0 is not legal FHIR
    expect_fallback("2024-01-15T25:00:00Z", RECOVER_FF_DATETIME);
    expect_fallback("2024-01-15T13:60:00Z", RECOVER_FF_DATETIME);
    expect_fallback("2024-01-15T13:45:61Z", RECOVER_FF_DATETIME);   // 61 > leap second
    expect_fallback("2024-01-15T13:45:30+15:00", RECOVER_FF_DATETIME);
    expect_fallback("2024-01-15T13:45:30+14:01", RECOVER_FF_DATETIME);
    expect_fallback("2024-01-15T13:45:30.Z", RECOVER_FF_DATETIME);  // bare '.'
    expect_fallback("not-a-date", RECOVER_FF_DATETIME);
    expect_fallback("2024-01-15T13:45:30ZZ", RECOVER_FF_DATETIME);  // trailing junk
}

// ── The arena: what actually reaches the wire ──────────────────────────────

static void test_slot_serialization()
{
    std::vector<BYTE> arena(8192, 0);
    Offset child = TEST_BLOCK_OFFSET + 512;
    const Offset before = child;
    const Encoded e = roundtrip(arena, child, "2024-01-15T13:45:30Z", RECOVER_FF_INSTANT);
    CHECK(e.packed, "should pack");
    CHECK_EQ(child, before, "child_off must not move");
    CHECK_EQ(SIZE_FF_DATETIME("2024-01-15T13:45:30Z", RECOVER_FF_INSTANT), Size{0},
             "SIZE must agree that no child block is needed");
    CHECK_EQ(STORE_FF_DATETIME(arena.data(), child, "2024-01-15T13:45:30Z", RECOVER_FF_INSTANT),
             Size{0}, "STORE must write nothing");

    const char *text = "2024-01-15T13:45:30.123456Z";
    child = TEST_BLOCK_OFFSET + 512;
    const Size need = SIZE_FF_DATETIME(text, RECOVER_FF_INSTANT);
    CHECK_EQ(need, SIZE_FF_STRING(text), "SIZE must reserve exactly the FF_STRING");

    const Offset str_at = child;
    const uint64_t slot = ENCODE_FF_DATETIME(arena.data(), TEST_BLOCK_OFFSET, child, text,
                                             RECOVER_FF_INSTANT);
    CHECK(FF_DATETIME_IS_FALLBACK(slot), "bit 63 set");
    CHECK(slot != FF_DATETIME_NULL, "a fallback must never look like the null sentinel");
    CHECK_EQ(child, str_at + need, "child_off advances by exactly the block size");
    CHECK_EQ(FF_ResolveDateTimeOffset(slot, TEST_BLOCK_OFFSET), str_at, "offset resolves back");
    CHECK_EQ(static_cast<RECOVERY_TAG>(LOAD_U16(arena.data() + str_at + DATA_BLOCK::RECOVERY)),
             RECOVER_FF_STRING, "target block is an FF_STRING");
    CHECK_EQ(LOAD_U64(arena.data() + str_at + DATA_BLOCK::VALIDATION), str_at,
             "target block's self-offset word");
    CHECK_EQ(decode_slot(arena.data(), TEST_BLOCK_OFFSET, slot, RECOVER_FF_INSTANT),
             std::string(text), "text survives");

    // The relative offset is signed; a negative one is the case a mistaken
    // unsigned cast would silently break.
    Offset back_child = TEST_BLOCK_OFFSET - 2048;
    const Offset back_at = back_child;
    const uint64_t back = ENCODE_FF_DATETIME(arena.data(), TEST_BLOCK_OFFSET, back_child, text,
                                             RECOVER_FF_INSTANT);
    CHECK(FF_DATETIME_IS_FALLBACK(back), "bit 63 set");
    CHECK_EQ(FF_ResolveDateTimeOffset(back, TEST_BLOCK_OFFSET), back_at,
             "negative offset resolves back");
    CHECK_EQ(decode_slot(arena.data(), TEST_BLOCK_OFFSET, back, RECOVER_FF_INSTANT),
             std::string(text), "text survives a backward offset");

    child = TEST_BLOCK_OFFSET + 512;
    const Offset untouched = child;
    CHECK_EQ(ENCODE_FF_DATETIME(arena.data(), TEST_BLOCK_OFFSET, child, "", RECOVER_FF_DATE),
             FF_DATETIME_NULL, "empty -> FF_DATETIME_NULL");
    CHECK_EQ(child, untouched, "empty text claims no child space");
    CHECK_EQ(SIZE_FF_DATETIME("", RECOVER_FF_DATE), Size{0}, "empty needs no bytes");
}

static void test_layout_constants()
{
    CHECK_EQ(static_cast<int>(FF_DT_FLAG), 63, "discriminator sits at bit 63");
    CHECK_EQ(static_cast<int>(FF_DT_PRECISION), 0, "precision is the low field");
    CHECK_EQ(static_cast<int>(FF_DT_DAYS + FF_DT_DAYS_S), static_cast<int>(FF_DT_FLAG),
             "days must end exactly where the flag begins");

    // Set every field to its maximum at once: nothing may bleed into a
    // neighbour, and the discriminator must stay clear.
    FF_DateTimeParts p;
    p.days = FF_DATETIME_MAX_DAYS;
    p.hour = 23;
    p.minute = 59;
    p.second = 60;
    p.millisecond = 999;
    p.utc_offset = FF_DATETIME_OFFSET_MAX;
    p.precision = FF_DateTimePrecision::FRAC3;

    const uint64_t slot = FF_PACK_DATETIME(p);
    CHECK(!FF_DATETIME_IS_FALLBACK(slot), "a packed value never sets bit 63");
    CHECK(slot != FF_DATETIME_NULL, "a packed value is never the null sentinel");

    const FF_DateTimeParts back = FF_UNPACK_DATETIME(slot);
    CHECK_EQ(back.days, p.days, "days");
    CHECK_EQ(back.hour, p.hour, "hour");
    CHECK_EQ(back.minute, p.minute, "minute");
    CHECK_EQ(back.second, p.second, "second");
    CHECK_EQ(back.millisecond, p.millisecond, "millisecond");
    CHECK_EQ(back.utc_offset, p.utc_offset, "utc offset");
    CHECK(back.precision == p.precision, "precision");

    p.utc_offset = FF_DATETIME_OFFSET_MIN;
    CHECK_EQ(FF_UNPACK_DATETIME(FF_PACK_DATETIME(p)).utc_offset, FF_DATETIME_OFFSET_MIN,
             "-14:00 sign-extends");
    p.utc_offset = FF_DATETIME_OFFSET_Z;
    CHECK_EQ(FF_UNPACK_DATETIME(FF_PACK_DATETIME(p)).utc_offset, FF_DATETIME_OFFSET_Z,
             "the Z sentinel sign-extends");

    FF_DateTimeParts bad;
    bad.days = FF_DATETIME_MAX_DAYS + 1;
    CHECK(!ff_datetime_fits(bad), "a day past 9999-12-31");
    bad = {};
    bad.hour = 24;
    CHECK(!ff_datetime_fits(bad), "hour 24");
    bad = {};
    bad.second = 61;
    CHECK(!ff_datetime_fits(bad), "second 61");
    bad = {};
    bad.millisecond = 1000;
    CHECK(!ff_datetime_fits(bad), "millisecond 1000");
    bad = {};
    bad.utc_offset = FF_DATETIME_OFFSET_MAX + 1;
    CHECK(!ff_datetime_fits(bad), "offset past +14:00");
}

// ── The kind (DT-1.2) ──────────────────────────────────────────────────────

static void test_field_kind()
{
    CHECK_EQ(static_cast<int>(FF_FIELD_DATETIME), 13, "appended value is ABI-pinned");
    CHECK_EQ(static_cast<int>(ff_slot_width(FF_FIELD_DATETIME)),
             static_cast<int>(TYPE_SIZE_UINT64), "8 bytes");
    // The slot did not widen: routing these types off STRING_TYPES in DT-2 moves
    // no V-Table offset, which is what makes the change confined to a field's
    // interpretation.
    CHECK_EQ(static_cast<int>(ff_slot_width(FF_FIELD_DATETIME)),
             static_cast<int>(ff_slot_width(FF_FIELD_STRING)),
             "must match the string-offset slot it replaces");

    CHECK(Recovery_to_Kind(RECOVER_FF_DATE) == FF_FIELD_DATETIME, "date");
    CHECK(Recovery_to_Kind(RECOVER_FF_DATETIME) == FF_FIELD_DATETIME, "dateTime");
    CHECK(Recovery_to_Kind(RECOVER_FF_TIME) == FF_FIELD_DATETIME, "time");
    CHECK(Recovery_to_Kind(RECOVER_FF_INSTANT) == FF_FIELD_DATETIME, "instant");

    // Pinned so nobody "completes" the table later: one kind names four tags, so
    // any single answer here is wrong three times in four, and the wrong answer
    // would show up as a `date` exported as valueDateTime.
    CHECK(Kind_to_Recovery(FF_FIELD_DATETIME) == FF_RECOVER_UNDEFINED,
          "Kind_to_Recovery must not guess which of the four tags this is");

    // FF_IsFieldEmpty's default arm returns true, so a missing case would report
    // every date/time field as absent and silently drop it on export.
    std::vector<BYTE> buf(32, 0);
    STORE_U64(buf.data(), FF_DATETIME_NULL);
    CHECK(FF_IsFieldEmpty(buf.data(), 0, FF_FIELD_DATETIME), "all-ones reads as absent");

    const auto packed = FF_PARSE_DATETIME("2024-01-15", RECOVER_FF_DATE);
    CHECK(packed.has_value(), "fixture parses");
    STORE_U64(buf.data(), FF_PACK_DATETIME(*packed));
    CHECK(!FF_IsFieldEmpty(buf.data(), 0, FF_FIELD_DATETIME), "a packed value is present");

    // A pre-epoch date packs to a small day count; its high bytes are zero, and
    // zero must not read as absent either.
    const auto early = FF_PARSE_DATETIME("0001-01-01", RECOVER_FF_DATE);
    CHECK(early.has_value(), "epoch fixture parses");
    STORE_U64(buf.data(), FF_PACK_DATETIME(*early));
    CHECK(!FF_IsFieldEmpty(buf.data(), 0, FF_FIELD_DATETIME),
          "0001-01-01 packs to mostly zero bits and must still read as present");
}

// ChoiceEntry::to_string -- text on demand. A PACKED date/time choice stores
// the civil value in the slot and has no text on the wire; the text is
// FORMATTED when a caller asks (heap std::string, caller owns). The FALLBACK
// form's text is preserved verbatim in the arena and returned as-is. This is
// the CAPI-11 resolution: the representation stays packed, the conversion to
// text is one call away.
static void test_choice_datetime_to_string()
{
    const auto parsed = FF_PARSE_DATETIME("2024-01-15T13:45:30+00:00", RECOVER_FF_DATETIME);
    CHECK(parsed.has_value(), "probe text must parse");
    if (!parsed) return;
    const uint64_t packed = FF_PACK_DATETIME(*parsed);

    ChoiceEntry e;  // packed civil value -> formatted text on demand
    e.tag = RECOVER_FF_DATETIME;
    e.value = packed;
    CHECK_EQ(e.to_string(), FF_FORMAT_DATETIME(*parsed, RECOVER_FF_DATETIME),
             "packed date/time choice formats to text on demand");

    std::ostringstream oss;
    oss << e;
    CHECK_EQ(oss.str(), std::string(FF_RecoveryName(e.tag)) + ": " + e.to_string(),
             "operator<< prints the tag name and the value");

    ChoiceEntry f;  // fallback: original text preserved verbatim, no formatting
    f.tag = RECOVER_FF_INSTANT;
    f.value = std::string_view("2024-01-15T13:45:30.1234567+05:30");
    CHECK_EQ(f.to_string(), "2024-01-15T13:45:30.1234567+05:30",
             "fallback date/time choice returns its verbatim text");

    ChoiceEntry g;  // any arm stringifies -- an integer is digits, not empty
    g.tag = RECOVER_FF_INT32;
    g.value = int32_t{7};
    CHECK_EQ(g.to_string(), "7", "integer choice formats its digits");

    ChoiceEntry h;  // absent
    CHECK(h.to_string().empty(), "absent choice formats to nothing");
}

// to_string() is generic -- every arm has a text form: strings as-is,
// booleans/integers/decimals formatted (shortest-round-trip for doubles, the
// same answer print_decimal_json gives a decimal choice variant). Block
// alternatives are structured and format to nothing.
static void test_choice_to_string_generic()
{
    ChoiceEntry b;
    b.tag = RECOVER_FF_BOOL;
    b.value = true;
    CHECK_EQ(b.to_string(), "true", "bool choice formats true");

    ChoiceEntry d;
    d.tag = RECOVER_FF_FLOAT64;
    d.value = 42.142567166419695;
    CHECK_EQ(d.to_string(), "42.142567166419695", "decimal choice uses shortest round-trip");

    ChoiceEntry s;
    s.tag = RECOVER_FF_STRING;
    s.value = std::string_view("hello");
    CHECK_EQ(s.to_string(), "hello", "string choice returns text as-is");

    ChoiceEntry u;
    u.tag = RECOVER_FF_UINT64;
    u.value = uint64_t{18446744073709551615ull};
    CHECK_EQ(u.to_string(), "18446744073709551615", "u64 choice formats full width");

    ChoiceEntry i64;
    i64.tag = RECOVER_FF_INT64;
    i64.value = int64_t{-7};
    CHECK_EQ(i64.to_string(), "-7", "i64 choice formats negatives");

    ChoiceEntry q;  // block alternative: structured, no scalar text
    q.tag = RECOVER_FF_QUANTITY;
    CHECK(q.to_string().empty(), "block choice formats to nothing");
}

int main(int argc, char **argv)
{
    const char *filter = "";
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) filter = argv[++i];
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            g_seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    }
    std::cout << "seed " << g_seed << " (reproduce with --seed " << g_seed << ")\n";


    TEST_GROUP("Layout");
    ff_test::run("test_layout_constants", test_layout_constants);
    ff_test::run("test_field_kind", test_field_kind);

    TEST_GROUP("Precision");
    ff_test::run("test_precision_levels", test_precision_levels);
    ff_test::run("test_precision_is_preserved_not_normalised", test_precision_is_preserved_not_normalised);

    TEST_GROUP("CivilEpoch");
    ff_test::run("test_epoch_shift_crosses_zero", test_epoch_shift_crosses_zero);
    ff_test::run("test_epoch_boundary_dates", test_epoch_boundary_dates);

    TEST_GROUP("Sweeps");
    ff_test::run("test_named_boundary_days", test_named_boundary_days);
    ff_test::run("test_random_dates_all_precisions", test_random_dates_all_precisions);
    ff_test::run("test_time_of_day_sweep", test_time_of_day_sweep);
    ff_test::run("test_utc_offset_sweep", test_utc_offset_sweep);

    TEST_GROUP("CanonicalForm");
    ff_test::run("test_equal_values_have_equal_bits", test_equal_values_have_equal_bits);

    TEST_GROUP("Fallback");
    ff_test::run("test_fallback_preserves_text", test_fallback_preserves_text);

    TEST_GROUP("Serialization");
    ff_test::run("test_slot_serialization", test_slot_serialization);

    TEST_GROUP("ChoiceText");
    ff_test::run("test_choice_datetime_to_string", test_choice_datetime_to_string);
    ff_test::run("test_choice_to_string_generic", test_choice_to_string_generic);

    std::cout << "\n────────────────────────────────────────────────\n"
              << ::ff_test::g_checks << " tests, " << ::ff_test::g_failures << " failures\n";
    return ::ff_test::g_failures > 0 ? 1 : 0;
}
