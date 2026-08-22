/**
 * @file test_dictionary.cpp
 * @brief Lookup-integrity tests for FF_GetDictionaryCode over the permanent
 *        code ledger (dictionaries/master_codes.json -> FF_*_DICTIONARY).
 *
 * ONE INVARIANT CARRIES THIS SUITE: every label in a dictionary must resolve to
 * its OWN code. That sounds tautological and is not -- the lookup is built by
 * inserting labels into an unordered_map, and unordered_map::emplace does not
 * overwrite, so any construction that can put a second entry under an existing
 * key silently wins or loses on table order.
 *
 * That is precisely what happened. The UCUM map inserted each label together
 * with a lowercased alias for case-insensitive convenience matching, in one
 * pass. 'A' (ampere) therefore seeded the alias key 'a' before the real 'a'
 * (year, a completely different unit) was reached, and the real entry no-opped.
 * Every lookup of "a" returned ampere. 55 of the 1,384 UCUM labels differ from
 * another label only by case, and which of each pair broke came down purely to
 * table order -- 'd' (day) happens to precede 'D' (deci-) and so looked
 * correct, which is most of why the bug survived.
 *
 * UCUM is case-SENSITIVE by specification: a/A year/ampere, m/M metre/mega,
 * t/T tonne/tesla. A silently substituted unit in a dose or a lab result is a
 * clinical error that no downstream consumer can detect, so the case-pair test
 * below is the regression that matters, not the aggregate count.
 *
 * The space is ENUMERATED, not sampled: a few thousand labels is small enough
 * to walk completely, so there is no seed and nothing to flake.
 *
 * DEDUP PRIORITY IS PART OF THE CONTRACT, not an exception to it. The lookup
 * consults UCUM, then R4 or R5 by version. A label present in both UCUM and a
 * FHIR dictionary therefore resolves to the UCUM code BY DESIGN, so the R4/R5
 * checks below assert self-resolution only for labels UCUM does not claim --
 * asserting more would encode the wrong contract and fail on correct code.
 *
 * Run: ff_test_dictionary [--filter <name>]
 * Exit code: 0 = all pass, non-zero = failures.
 */

#include <FF_Dictionary.hpp>
#include <FF_Primitives.hpp>

#include <cctype>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace FastFHIR;

// ── Test framework (matches the other standalone suites) ───────────────────

static int g_failures = 0;
static int g_tests = 0;
static const char *g_current_group = "";

#define TEST_GROUP(name)                     \
    do                                       \
    {                                        \
        g_current_group = name;              \
        std::cout << "\n[" << name << "]\n"; \
    } while (0)
#define CHECK(cond, msg)                                                              \
    do                                                                                \
    {                                                                                 \
        ++g_tests;                                                                    \
        if (!(cond))                                                                  \
        {                                                                             \
            ++g_failures;                                                             \
            std::cerr << "  FAIL " << g_current_group << "::" << __func__ << " line " \
                      << __LINE__ << ": " << msg << "\n";                             \
        }                                                                             \
    } while (0)
#define CHECK_EQ(a, b, msg) CHECK((a) == (b), msg << " expected " << (b) << " got " << (a))

// ── Helpers ────────────────────────────────────────────────────────────────

static std::string to_lower(std::string s)
{
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Dict
{
    const char *name;
    const FF_CodeEntry *entries;
    size_t count;
    uint32_t version;
};

static Dict ucum() { return {"UCUM", FF_UCUM_DICTIONARY, FF_UCUM_DICTIONARY_SIZE, FHIR_VERSION_R5}; }
static Dict r4()   { return {"R4",   FF_R4_DICTIONARY,   FF_R4_DICTIONARY_SIZE,   FHIR_VERSION_R4}; }
static Dict r5()   { return {"R5",   FF_R5_DICTIONARY,   FF_R5_DICTIONARY_SIZE,   FHIR_VERSION_R5}; }

/// Labels UCUM claims. The FHIR dictionaries are consulted after UCUM, so any
/// label in here legitimately resolves to the UCUM code regardless of version.
static const std::unordered_set<std::string> &ucum_labels()
{
    static const std::unordered_set<std::string> s = [] {
        std::unordered_set<std::string> out;
        for (size_t i = 0; i < FF_UCUM_DICTIONARY_SIZE; ++i) out.emplace(FF_UCUM_DICTIONARY[i].label);
        return out;
    }();
    return s;
}

// ── The invariant ──────────────────────────────────────────────────────────

/// Every UCUM label resolves to its own code. UCUM is consulted first, so this
/// admits no exceptions -- a miss here is a shadowed entry.
static void test_ucum_labels_resolve_to_own_code()
{
    const Dict d = ucum();
    size_t bad = 0;
    for (size_t i = 0; i < d.count; ++i)
    {
        const std::string label = d.entries[i].label;
        const auto want = static_cast<uint32_t>(d.entries[i].code);
        const uint32_t got = FF_GetDictionaryCode(label, d.version);
        if (got != want && ++bad <= 10)
            CHECK(false, "UCUM '" << label << "' resolves to " << got << ", not its own " << want);
    }
    CHECK_EQ(bad, size_t{0}, "UCUM labels shadowed by another entry");
}

/// THE REGRESSION. Two labels differing only by case are two different units,
/// so they must resolve to two different codes. A convenience alias that
/// lowercases labels must never be able to occupy a key that is itself a real
/// code -- the failure mode is silent and clinical.
static void test_case_variant_labels_stay_distinct()
{
    std::unordered_map<std::string, std::vector<std::string>> by_lower;
    for (size_t i = 0; i < FF_UCUM_DICTIONARY_SIZE; ++i)
    {
        const std::string label = FF_UCUM_DICTIONARY[i].label;
        by_lower[to_lower(label)].push_back(label);
    }

    size_t groups = 0;
    for (const auto &[lo, variants] : by_lower)
    {
        if (variants.size() < 2) continue;
        ++groups;
        for (size_t a = 0; a < variants.size(); ++a)
            for (size_t b = a + 1; b < variants.size(); ++b)
            {
                if (variants[a] == variants[b]) continue;
                const uint32_t ca = FF_GetDictionaryCode(variants[a], FHIR_VERSION_R5);
                const uint32_t cb = FF_GetDictionaryCode(variants[b], FHIR_VERSION_R5);
                CHECK(ca != cb, "case-variant UCUM labels '"
                                    << variants[a] << "' and '" << variants[b]
                                    << "' both resolve to " << ca
                                    << " -- one is shadowing the other");
            }
    }
    // Guards the guard: if the table ever stops containing case pairs this
    // test silently proves nothing, and the alias hazard would return unnoticed.
    CHECK(groups > 0, "expected UCUM labels differing only by case; found none");
}

/// The specific pair that broke, spelled out. 'a' is a year and 'A' an ampere;
/// a stream that confuses them is wrong in a way no reader can detect.
static void test_ucum_year_is_not_ampere()
{
    const uint32_t year = FF_GetDictionaryCode("a", FHIR_VERSION_R5);
    const uint32_t ampere = FF_GetDictionaryCode("A", FHIR_VERSION_R5);
    CHECK(year != FF_CODE_NULL, "UCUM 'a' (year) must be in the dictionary");
    CHECK(ampere != FF_CODE_NULL, "UCUM 'A' (ampere) must be in the dictionary");
    CHECK(year != ampere, "UCUM 'a' (year) and 'A' (ampere) both resolve to " << year);
}

/// R4/R5 labels resolve to their own code unless UCUM claims the label first,
/// which is the documented dedup priority rather than a defect.
static void test_fhir_labels_resolve_to_own_code()
{
    for (const Dict d : {r4(), r5()})
    {
        size_t bad = 0, deferred = 0;
        for (size_t i = 0; i < d.count; ++i)
        {
            const std::string label = d.entries[i].label;
            if (ucum_labels().count(label)) { ++deferred; continue; }
            const auto want = static_cast<uint32_t>(d.entries[i].code);
            const uint32_t got = FF_GetDictionaryCode(label, d.version);
            if (got != want && ++bad <= 10)
                CHECK(false, d.name << " '" << label << "' resolves to " << got
                                    << ", not its own " << want);
        }
        CHECK_EQ(bad, size_t{0}, d.name << " labels shadowed by another entry");
        std::cout << "    " << d.name << ": " << d.count << " labels, " << deferred
                  << " deferred to UCUM by dedup priority\n";
    }
}

/// NO FUZZY MATCHING. UCUM is case-sensitive by specification, so a
/// wrong-case spelling is not a forgivable typo -- it is a different unit or
/// no unit. The lookup must refuse it rather than guess.
///
/// Refusing costs nothing: FF_CODE_NULL sends the writer to an
/// FF_CODEABLE_CONCEPT block holding the original text, so a non-conformant
/// unit round-trips verbatim instead of being silently rewritten into another.
/// A case-insensitive fallback previously accepted 640 such spellings.
static void test_wrong_case_ucum_is_refused()
{
    size_t admitted = 0, examined = 0;
    for (size_t i = 0; i < FF_UCUM_DICTIONARY_SIZE; ++i)
    {
        const std::string label = FF_UCUM_DICTIONARY[i].label;
        const std::string lo = to_lower(label);
        if (lo == label) continue;   // no wrong-case spelling to test
        // Skip labels whose lowercase IS itself a real code in some dictionary:
        // resolving those is exact matching, not fuzziness.
        if (ucum_labels().count(lo)) continue;
        ++examined;
        const uint32_t got = FF_GetDictionaryCode(lo, FHIR_VERSION_R5);
        if (got == static_cast<uint32_t>(FF_UCUM_DICTIONARY[i].code) && ++admitted <= 5)
            CHECK(false, "wrong-case '" << lo << "' was accepted as UCUM '" << label
                                        << "' -- UCUM is case-sensitive");
    }
    CHECK_EQ(admitted, size_t{0}, "wrong-case UCUM spellings accepted");
    CHECK(examined > 0, "expected UCUM labels with a distinct lowercase form");
}

/// A label absent from every dictionary must report absent, not collide with
/// something. FF_CODE_NULL is what makes the writer fall back to an
/// FF_CODEABLE_CONCEPT block instead of asserting a wrong code.
static void test_unknown_labels_report_null()
{
    for (const char *s : {"__no_such_code__", "zzzz-not-a-unit", " "})
        CHECK_EQ(FF_GetDictionaryCode(s, FHIR_VERSION_R5), FF_CODE_NULL,
                 "unknown label '" << s << "' must resolve to FF_CODE_NULL");
    CHECK_EQ(FF_GetDictionaryCode("", FHIR_VERSION_R5), FF_CODE_NULL,
             "empty label must resolve to FF_CODE_NULL");
}

int main(int argc, char **argv)
{
    const char *filter = "";
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) filter = argv[++i];

    auto run = [&](const char *name, auto fn)
    {
        if (filter[0] == '\0' || std::strstr(name, filter)) fn();
    };

    std::cout << "UCUM " << FF_UCUM_DICTIONARY_SIZE << " labels, R4 " << FF_R4_DICTIONARY_SIZE
              << ", R5 " << FF_R5_DICTIONARY_SIZE << " (enumerated, no sampling)\n";

    TEST_GROUP("SelfResolution");
    run("test_ucum_labels_resolve_to_own_code", test_ucum_labels_resolve_to_own_code);
    run("test_fhir_labels_resolve_to_own_code", test_fhir_labels_resolve_to_own_code);

    TEST_GROUP("CaseSensitivity");
    run("test_case_variant_labels_stay_distinct", test_case_variant_labels_stay_distinct);
    run("test_ucum_year_is_not_ampere", test_ucum_year_is_not_ampere);
    run("test_wrong_case_ucum_is_refused", test_wrong_case_ucum_is_refused);

    TEST_GROUP("Absent");
    run("test_unknown_labels_report_null", test_unknown_labels_report_null);

    std::cout << "\n────────────────────────────────────────────────\n"
              << g_tests << " checks, " << g_failures << " failures\n";
    return g_failures > 0 ? 1 : 0;
}
