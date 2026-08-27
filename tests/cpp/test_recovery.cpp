/**
 * @file test_recovery.cpp
 * @brief Recovery subsystem tests — TASKS.md REC-12 + review-round-2 regressions.
 *
 * Builds a real Bundle through the ingest API, then drives FastFHIR::Recovery
 * over the same Memory:
 *   - a CLEAN stream must report zero damage (D3/D4/D5 regression: inline
 *     scalar choice variants, packed codes and legal null array entries must
 *     not fabricate references or extents);
 *   - a 1-bit VALIDATION flip  -> PositionRepaired at bit_cost 1;
 *   - a 1-bit parent offset flip -> Corroborated at bit_cost 1 (mode 1);
 *   - both halves damaged -> never a silent repair.
 *
 * Run: ff_test_recovery [--filter name]
 * Exit code: 0 = all pass, non-zero = failures.
 */

#include <FF_Ingestor.hpp>
#include <FastFHIR.hpp>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace FastFHIR;

// ── Test framework (same header-only pattern as test_primitives.cpp) ───────

static int g_failures = 0;
static int g_tests = 0;
static const char *g_current_group = "";

#define TEST_GROUP(name)                     \
    do                                       \
    {                                        \
        g_current_group = name;              \
        std::cout << "\n[" << name << "]\n"; \
    } while (0)
#define TEST(name) \
    do             \
    {              \
        ++g_tests; \
    } while (0)
#define CHECK(cond, msg)                                   \
    do                                                     \
    {                                                      \
        if (!(cond))                                       \
        {                                                  \
            ++g_failures;                                  \
            std::cerr << "  FAIL " << g_current_group      \
                      << "::" << __func__ << " line "      \
                      << __LINE__ << ": " << msg << "\n";  \
        }                                                  \
    } while (0)
#define CHECK_EQ(a, b, msg) CHECK((a) == (b), msg << " expected " << (b) << " got " << (a))

// ── Helpers ────────────────────────────────────────────────────────────────

// A finalized Memory holding a two-resource Bundle that exercises the shapes
// recovery must NOT misread: boolean choice variants (deceased[x]),
// inline-block arrays (name[]), a Quantity choice variant, packed codes,
// string offsets, and -- critically -- a RESOURCE-TUPLE array.
//
// `Bundle.entry[]` is NOT that array: entry is a BackboneElement, so it is an
// inline array of FF_BUNDLE_ENTRY *blocks* whose +0 IS each element's own
// offset. The fixture claimed entry[] gave tuple coverage and it does not,
// which is why the clean-stream assertions below passed while every real
// Synthea bundle reported 46 bogus ExtentDerived arrays.
//
// `contained[]` is the real thing: an array whose elements carry
// RECOVER_FF_RESOURCE (0x0003) and are 10-byte {target_offset, target_tag}
// tuples stored inline -- +0 points AWAY, so walking it in place always fails.
// Keep at least one `contained` resource here or fix #1 loses its regression
// test (TASKS.md P0-3).
static std::shared_ptr<Memory> build_bundle()
{
    auto arena = std::make_shared<Memory>(Memory::create(64 * 1024 * 1024));

    FF_StreamCreateInfo stream_info;
    stream_info.arena = arena;
    stream_info.version = FHIR_VERSION_R5;
    FF_Stream stream;
    if (!FF_CreateStream(stream_info, stream))
        return nullptr;

    FF_IngestorCreateInfo ingestor_info;
    FF_Ingestor ingestor;
    if (!FF_CreateIngestor(ingestor_info, ingestor))
        return nullptr;

    const std::string json = R"({
        "resourceType": "Bundle",
        "type": "collection",
        "entry": [
            {"resource": {"resourceType": "Patient",
                          "id": "p1",
                          "active": false,
                          "deceasedBoolean": false,
                          "name": [{"family": "Smith", "given": ["John"]}]}},
            {"resource": {"resourceType": "Observation",
                          "id": "o1",
                          "status": "final",
                          "contained": [
                              {"resourceType": "Patient", "id": "c1", "active": true},
                              {"resourceType": "Patient", "id": "c2", "active": false}
                          ],
                          "code": {"coding": [{"system": "http://loinc.org",
                                               "code": "2085-9"}]},
                          "valueQuantity": {"value": 4.5, "unit": "mmol/L"}}}
        ]
    })";

    Reflective::ObjectHandle bundle_handle;
    Size parsed_count = 0;
    const auto result = FF_Ingest(FF_IngestInfo{
        .ingestor = ingestor,
        .stream = stream,
        .source_type = FF_SOURCE_FHIR_JSON,
        .payload = json,
    }, bundle_handle, parsed_count);
    if (result.code != FF_SUCCESS || parsed_count == 0 || !bundle_handle)
        return nullptr;

    if (!FF_StreamSetRoot(FF_StreamSetRootInfo{
            .stream = stream,
            .root = bundle_handle,
        }))
        return nullptr;
    Memory::View view;
    if (!FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream,
        }, view))
        return nullptr;
    return arena;
}

static const BlockVerdict *find_verdict(const FF_RecoveryReport &rep,
                                        Offset parent, Offset field)
{
    for (const auto &v : rep.blocks)
        if (v.block.parent == parent && v.block.field == field)
            return &v;
    return nullptr;
}

// ── Tests ──────────────────────────────────────────────────────────────────

static void test_clean_stream_zero_false_positives()
{
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    Recovery rec(*arena);
    const auto rep = rec.recover();

    // P0-2 floor: the comparison is meaningless on an empty set.
    CHECK(rep.blocks_total > 0, "clean stream must enumerate references");
    CHECK_EQ(rep.blocks_total, rep.intact, "clean stream: every reference intact");
    CHECK(rep.corroborated == 0 && rep.tag_repaired == 0 &&
              rep.position_repaired == 0 && rep.extent_derived == 0 &&
              rep.ambiguous == 0 && rep.unrecovered == 0,
          "clean stream: zero false positives (D3/D4/D5)");
}

static void test_validation_flip_position_repaired()
{
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    // Pick the first real reference from a clean run and break its child's
    // VALIDATION word with a single bit (REC-12 case 1).
    Recovery clean(*arena);
    const auto clean_rep = clean.recover();
    CHECK(clean_rep.blocks_total > 0, "clean enumeration is non-empty");
    if (clean_rep.blocks.empty())
        return;
    const auto &r0 = clean_rep.blocks[0].block;
    CHECK(r0.child != FF_NULL_OFFSET, "first reference has a child");

    arena->base()[static_cast<size_t>(r0.child)] ^= 0x01;

    Recovery rec(*arena);
    const auto rep = rec.recover();
    const auto *v = find_verdict(rep, r0.parent, r0.field);
    CHECK(v != nullptr, "damaged reference is still reported");
    CHECK(v && v->class_ == RepairClass::PositionRepaired,
          "1-bit VALIDATION flip -> PositionRepaired");
    CHECK(v && v->bit_cost == 1, "1-bit VALIDATION flip costs 1");
}

static void test_offset_flip_corroborated()
{
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    Recovery clean(*arena);
    const auto clean_rep = clean.recover();
    CHECK(clean_rep.blocks_total > 0, "clean enumeration is non-empty");
    if (clean_rep.blocks.empty())
        return;
    const auto &r0 = clean_rep.blocks[0].block;

    // Flip one bit of the parent's stored offset: the child survives
    // self-consistent but unreachable -> an orphan (REC-12 case 2 / mode 1).
    const size_t slot_abs = static_cast<size_t>(r0.parent + r0.field);
    arena->base()[slot_abs] ^= 0x01;

    Recovery rec(*arena);
    const auto rep = rec.recover();
    const auto *v = find_verdict(rep, r0.parent, r0.field);
    CHECK(v != nullptr, "damaged reference is still reported");
    CHECK(v && v->class_ == RepairClass::Corroborated,
          "1-bit parent offset flip -> Corroborated (mode 1)");
    CHECK(v && v->bit_cost == 1, "1-bit parent offset flip costs 1");
}

static void test_both_halves_never_silent()
{
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    Recovery clean(*arena);
    const auto clean_rep = clean.recover();
    CHECK(clean_rep.blocks_total > 0, "clean enumeration is non-empty");
    if (clean_rep.blocks.empty())
        return;
    const auto &r0 = clean_rep.blocks[0].block;
    const size_t slot_abs = static_cast<size_t>(r0.parent + r0.field);

    // Damage BOTH halves: the parent's offset AND the child's VALIDATION.
    arena->base()[static_cast<size_t>(r0.child)] ^= 0x01;
    arena->base()[slot_abs] ^= 0x01;

    // Contract for the two-corruptions boundary (P0-3: "two independent
    // records defeat one corruption, not two"): the damage must be VISIBLE —
    // never reported Intact, never silently dropped. The ranker may still pick
    // the cheapest under-budget hypothesis (the true child is invisible with
    // its VALIDATION broken); whether that repair attached the RIGHT child is
    // the benchmark's baseline check (F3), not this unit's job.
    Recovery rec(*arena);
    const auto rep = rec.recover();
    const auto *v = find_verdict(rep, r0.parent, r0.field);
    CHECK(v != nullptr, "double-damaged reference is still reported");
    CHECK(v && v->class_ != RepairClass::Intact,
          "both halves damaged -> never reported Intact (got "
              << (v ? std::to_string(static_cast<int>(v->class_)) : std::string("null")) << ")");
}

// ── main ───────────────────────────────────────────────────────────────────

int main(int argc, char **argv)
{
    const char *filter = (argc > 2 && strcmp(argv[1], "--filter") == 0) ? argv[2] : "";

    auto run = [&](const char *name, auto fn)
    {
        if (filter[0] != '\0' && !strstr(name, filter))
            return;
        // Count here, not inside the bodies: a suite that runs nothing must not
        // be indistinguishable from a suite that passes everything. Before this,
        // deleting all four run() lines below produced byte-identical output and
        // the same green ctest verdict (TASKS.md P0-2).
        TEST(name);
        fn();
    };

    TEST_GROUP("Recovery");
    run("clean_stream_zero_false_positives", test_clean_stream_zero_false_positives);
    run("validation_flip_position_repaired", test_validation_flip_position_repaired);
    run("offset_flip_corroborated", test_offset_flip_corroborated);
    run("both_halves_never_silent", test_both_halves_never_silent);

    std::cout << "\n" << g_tests << " test(s), " << g_failures << " failure(s)\n";
    if (g_tests == 0)
    {
        std::cerr << "  FAIL no tests ran -- a pass on zero coverage is not a pass\n";
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
