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

#include <FF_Compactor.hpp>
#include <FF_Ingestor.hpp>
#include <FastFHIR.hpp>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
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


// ── REC-18: gaps ───────────────────────────────────────────────────────────

static void test_clean_stream_tiles_with_no_gaps()
{
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    Recovery rec(*arena);
    const StreamMap map = rec.scan();

    // P0-2 floor: a tiling assertion over an empty map is vacuous.
    CHECK(!map.empty(), "scan must locate blocks before tiling means anything");
    CHECK(map.file_size > 0, "stream has a size");

    // Every entry must carry a real extent, or the sweep below is measuring
    // nothing. size == 0 was the pre-REC-18 state for every plain block.
    size_t unsized = 0;
    for (const auto &[off, e] : map)
        if (e.size == 0)
            ++unsized;
    CHECK_EQ(unsized, static_cast<size_t>(0), "every map entry has a derived extent");

    // THE assertion: a clean arena tiles. Any run of bytes nothing claims on an
    // undamaged stream is a defect in the extent model, not damage.
    CHECK_EQ(map.gaps.size(), static_cast<size_t>(0), "clean stream has zero gaps");
}

static void test_broken_validation_leaves_a_hole()
{
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    Recovery clean(*arena);
    const StreamMap clean_map = clean.scan();
    CHECK(clean_map.gaps.empty(), "baseline is gap-free");

    // Find a plain block with a known tag, and destroy ONLY its VALIDATION word.
    // scan() locates blocks by `LOAD_U64(o) == o`, so the block becomes
    // invisible to it -- but its bytes are still there, so the tiling opens up.
    Offset victim = FF_NULL_OFFSET;
    for (const auto &[off, e] : clean_map)
        if (e.type == StreamMapEntryType::Block && off != 0 && e.size >= DATA_BLOCK::HEADER_SIZE) {
            victim = off;
            break;
        }
    CHECK(victim != FF_NULL_OFFSET, "fixture contains a plain block to damage");
    if (victim == FF_NULL_OFFSET)
        return;
    const Size victim_size = clean_map.at(victim).size;

    arena->base()[static_cast<size_t>(victim)] ^= 0xFF;  // VALIDATION destroyed

    Recovery rec(*arena);
    const StreamMap map = rec.scan();
    CHECK(!map.contains(victim), "a broken VALIDATION hides the block from scan()");

    bool found = false;
    for (const Gap &g : map.gaps)
        if (g.start == victim && g.length == victim_size && g.class_ == GapClass::Hole)
            found = true;
    CHECK(found, "the vanished block is reported as a Hole of its own size");
}

static void test_both_witnesses_broken_is_still_found()
{
    // THE REC-18 case. Break the child's VALIDATION *and* the parent's
    // reference to it: neither witness survives, so scan() cannot see it and
    // walk_chain() cannot reach it. Absence is the only remaining evidence.
    // This is the two-corruption boundary P0-3 names -- gaps do not repair it,
    // but they stop it being silent.
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    Recovery clean(*arena);
    const auto clean_rep = clean.recover();
    CHECK(clean_rep.blocks_total > 0, "clean enumeration is non-empty");
    CHECK_EQ(clean_rep.holes, static_cast<size_t>(0), "clean stream reports no holes");
    if (clean_rep.blocks.empty())
        return;

    const StreamMap clean_map = clean.scan();
    const BlockRef *target = nullptr;
    for (const auto &v : clean_rep.blocks) {
        const auto it = clean_map.find(v.block.child);
        if (it == clean_map.end() || it->second.type != StreamMapEntryType::Block ||
            it->second.size < DATA_BLOCK::HEADER_SIZE)
            continue;
        // The two damage sites must be DISTINCT bytes. For some references the
        // slot address and the child's own offset coincide, and two `^= 0xFF`
        // writes to one byte cancel -- which silently produced an undamaged
        // stream and a green-looking test.
        if (static_cast<size_t>(v.block.parent + v.block.field) ==
            static_cast<size_t>(v.block.child))
            continue;
        target = &v.block;
        break;
    }
    CHECK(target != nullptr, "fixture has a reference to a plain block");
    if (!target)
        return;
    const Offset child = target->child;
    const Size   child_size = clean_map.at(child).size;

    arena->base()[static_cast<size_t>(child)] ^= 0xFF;                    // witness 2 gone
    arena->base()[static_cast<size_t>(target->parent + target->field)] ^= 0xFF;  // witness 1 gone

    Recovery rec(*arena);
    const StreamMap damaged = rec.scan();
    // Precondition, asserted rather than assumed: if the block did not actually
    // vanish, everything below passes for the wrong reason.
    CHECK(!damaged.contains(child), "both-witness damage really did hide the block");

    const auto rep = rec.recover();
    CHECK(rep.holes > 0, "a block with NEITHER witness is still reported (as a hole)");
    bool sized = false;
    for (const Gap &g : rep.gaps)
        if (g.class_ == GapClass::Hole && g.start == child && g.length == child_size)
            sized = true;
    CHECK(sized, "the hole is located at the lost block and sized to it");
}


static void test_same_version_stream_never_reports_skew()
{
    // The version GATE, asserted in the safety-critical direction: on a stream
    // written by THIS engine, no gap may be excused as version skew. Getting
    // this backwards would silently reclassify real damage as benign, which is
    // strictly worse than reporting nothing.
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    Recovery clean(*arena);
    const StreamMap clean_map = clean.scan();
    Offset victim = FF_NULL_OFFSET;
    for (const auto &[off, e] : clean_map)
        if (e.type == StreamMapEntryType::Block && off != 0 && e.size >= DATA_BLOCK::HEADER_SIZE) {
            victim = off;
            break;
        }
    CHECK(victim != FF_NULL_OFFSET, "fixture contains a plain block to damage");
    if (victim == FF_NULL_OFFSET)
        return;

    arena->base()[static_cast<size_t>(victim)] ^= 0xFF;

    Recovery rec(*arena);
    const StreamMap map = rec.scan();
    CHECK(!map.gaps.empty(), "damage opened a gap at all");
    size_t skew = 0;
    for (const Gap &g : map.gaps)
        if (g.class_ == GapClass::VersionSkew)
            ++skew;
    CHECK_EQ(skew, static_cast<size_t>(0),
             "a same-version stream must never excuse a gap as version skew");
}

static void test_compact_archive_is_refused()
{
    // REC-18.7 — the compact layout is a presence-bitmask rewrite with entirely
    // different geometry. Gap analysis must decline rather than emit nonsense.
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    Parser source(*arena);
    auto dest = Memory::create(64 * 1024 * 1024);
    const Memory::View compact = Compactor::archive(source, dest);
    CHECK(!compact.empty(), "compaction produced a stream");
    if (compact.empty())
        return;

    Recovery rec(dest);
    const StreamMap map = rec.scan();
    CHECK(!map.empty(), "the compact stream still contains locatable blocks");
    CHECK(map.gaps.empty(), "gap analysis declines a compact archive");
}


static const char *shape_name(StreamMapEntryType t)
{
    switch (t) {
        case StreamMapEntryType::Header: return "Header";
        case StreamMapEntryType::Block:  return "Block";
        case StreamMapEntryType::Array:  return "Array";
        case StreamMapEntryType::String: return "String";
        default:                         return "Undefined";
    }
}

// One victim per entry shape that is not the last entry (a run after the final
// entry classifies as Trailing, not Hole). Offset-ordered, so `next` exists.
static std::vector<std::pair<StreamMapEntryType, Offset>> one_victim_per_shape(const StreamMap &map)
{
    std::vector<std::pair<StreamMapEntryType, Offset>> out;
    for (auto it = map.begin(); it != map.end(); ++it) {
        if (std::next(it) == map.end())
            break;                                   // no successor: would be Trailing
        if (it->first == 0 || it->second.size == 0)
            continue;                                // header / unsized
        bool seen = false;
        for (const auto &v : out)
            if (v.first == it->second.type)
                seen = true;
        if (!seen)
            out.emplace_back(it->second.type, it->first);
    }
    return out;
}

static void test_holes_locate_and_size_every_entry_shape()
{
    // The extent rules differ per shape -- a String is header+LENGTH, an
    // INLINE_BLOCK Array is charged header-only so its elements are not
    // double-counted, a Block is its V-Table. A hole must land on the exact
    // range for ALL of them, so test one of each rather than whichever the map
    // happens to yield first.
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    Recovery clean(*arena);
    const StreamMap base = clean.scan();
    CHECK(base.gaps.empty(), "baseline tiles with no gaps");

    const auto victims = one_victim_per_shape(base);
    CHECK(victims.size() >= 3, "fixture covers at least three entry shapes");

    for (const auto &[shape, off] : victims) {
        const Size expect = base.at(off).size;
        const std::string what = std::string(shape_name(shape)) + "@" + std::to_string(off);

        arena->base()[static_cast<size_t>(off)] ^= 0xFF;   // destroy VALIDATION

        Recovery rec(*arena);
        const StreamMap m = rec.scan();
        CHECK(!m.contains(off), what + ": broken VALIDATION hides it from scan()");

        size_t holes = 0;
        bool exact = false;
        for (const Gap &g : m.gaps)
            if (g.class_ == GapClass::Hole) {
                ++holes;
                if (g.start == off && g.length == expect)
                    exact = true;
            }
        CHECK_EQ(holes, static_cast<size_t>(1), what + ": one lost block -> exactly one hole");
        CHECK(exact, what + ": hole is at the block's offset and its exact size (" +
                         std::to_string(expect) + " bytes)");

        arena->base()[static_cast<size_t>(off)] ^= 0xFF;   // restore
    }

    // Printed, not merely asserted: "it passed" and "it covered every shape"
    // are different claims, and a fixture that stops producing a shape must be
    // visible rather than silently narrowing the test.
    std::cout << "    covered shapes:";
    for (const auto &[shape, off] : victims)
        std::cout << " " << shape_name(shape) << "(" << base.at(off).size << "B)";
    std::cout << "\n";

    Recovery after(*arena);
    CHECK(after.scan().gaps.empty(), "restoring every victim leaves the arena tiled again");
}

static void test_broken_blockref_still_locates_and_sizes_the_orphan()
{
    // Ryan, 2026-08-27: break the parent->child REFERENCE as well as the child,
    // per shape, and require the gap to still name the exact byte range.
    //
    // Breaking the reference ALONE leaves the child self-consistent, so scan()
    // still finds it and it is an orphan, not a hole -- that path is covered by
    // test_offset_flip_corroborated. A hole needs the block to leave the map,
    // which is the reference AND the VALIDATION: no witness survives, and the
    // byte range is the only thing left to identify it by.
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    Recovery clean(*arena);
    const StreamMap base = clean.scan();
    const auto clean_rep = clean.recover();
    CHECK(base.gaps.empty(), "baseline tiles with no gaps");
    CHECK(clean_rep.blocks_total > 0, "clean enumeration is non-empty");

    // child offset -> the slot that references it
    std::map<Offset, std::pair<Offset, Offset>> ref_of;
    for (const auto &v : clean_rep.blocks)
        if (v.block.child != FF_NULL_OFFSET)
            ref_of.emplace(v.block.child, std::make_pair(v.block.parent, v.block.field));

    // EVERY referenced block, not one per shape: the claim is that the byte
    // range identifies the orphan, and that has to hold for all of them.
    size_t covered = 0, skipped_last = 0, skipped_collide = 0;
    std::map<std::string, size_t> by_shape;
    for (auto mit = base.begin(); mit != base.end(); ++mit) {
        const Offset off = mit->first;
        if (off == 0 || mit->second.size == 0)
            continue;
        if (std::next(mit) == base.end()) {             // its run would be Trailing
            ++skipped_last;
            continue;
        }
        const auto it = ref_of.find(off);
        if (it == ref_of.end())
            continue;                                   // not referenced; nothing to break
        const size_t slot = static_cast<size_t>(it->second.first + it->second.second);
        if (slot == static_cast<size_t>(off)) {         // same byte: the flips would cancel
            ++skipped_collide;
            continue;
        }

        const StreamMapEntryType shape = mit->second.type;
        const Size expect = base.at(off).size;
        const std::string what = std::string(shape_name(shape)) + "@" + std::to_string(off);
        by_shape[shape_name(shape)]++;

        arena->base()[static_cast<size_t>(off)] ^= 0xFF;  // witness 2: VALIDATION
        arena->base()[slot] ^= 0xFF;                     // witness 1: the blockref

        Recovery rec(*arena);
        const StreamMap m = rec.scan();
        CHECK(!m.contains(off), what + ": neither witness survives, so scan() loses it");

        bool exact = false;
        for (const Gap &g : m.gaps)
            if (g.class_ == GapClass::Hole && g.start == off && g.length == expect)
                exact = true;
        CHECK(exact, what + ": with the blockref broken too, the gap still gives the exact "
                             "offset and size (" + std::to_string(expect) + " bytes)");
        ++covered;

        arena->base()[static_cast<size_t>(off)] ^= 0xFF;  // restore both
        arena->base()[slot] ^= 0xFF;
    }

    // P0-2 floor: if no shape was reachable through a reference the loop above
    // asserted nothing at all, and this test would "pass" having tested nothing.
    std::cout << "    blockref breaks exercised: " << covered << " (";
    for (const auto &[name, n] : by_shape)
        std::cout << name << "=" << n << " ";
    std::cout << "| skipped: last=" << skipped_last
              << " slot-collides-with-child=" << skipped_collide << ")\n";
    CHECK(covered > 0, "at least one referenced block was exercised");

    Recovery after(*arena);
    CHECK(after.scan().gaps.empty(), "restoring leaves the arena tiled again");
}

// A single damaged witness must cost NOTHING. The format stores a block's
// identity twice -- the block's own self-offset, and the parent slot that names
// its address and its type -- so one flip is exactly the case the redundancy
// exists to absorb.
//
// It did not absorb it. recover() enumerated references only from the scan
// census, and scan() finds a block by its self-offset, so a block whose
// VALIDATION took the flip was ABSENT: the reference TO it was still classified
// and repaired (which is why the report showed zero failures), while every
// reference FROM it was never enumerated. On a 1.05 MB Synthea artifact one
// flipped bit silently cost 3 block references and opened a hole, with
// ambiguous=0 and unrecovered=0 throughout -- a loss that could not be seen in
// the report it was absent from.
//
// So this asserts the whole-report shape, not just the verdict on the damaged
// edge: same reference count as clean, no holes, nothing unrecovered.
static void test_one_damaged_witness_costs_nothing()
{
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    Recovery clean(*arena);
    const FF_RecoveryReport clean_rep = clean.recover();
    CHECK_EQ(clean_rep.holes, static_cast<size_t>(0), "clean stream reports no holes");

    // A block with children, so the subtree below the damage is what is at
    // stake -- damaging a leaf would pass even with the walk truncated.
    std::map<Offset, size_t> children;
    for (const BlockRef &r : clean.reachable_blocks())
        if (r.child != FF_NULL_OFFSET)
            ++children[r.child];  // how many blocks does this one hang below
    std::map<Offset, size_t> outgoing;
    for (const BlockRef &r : clean.reachable_blocks())
        if (r.child != FF_NULL_OFFSET)
            ++outgoing[r.parent];

    Offset victim = FF_NULL_OFFSET;
    for (const auto &[off, n] : outgoing)
        if (n >= 2 && off != 0 && children.contains(off)) {
            victim = off;  // has a parent that vouches for it AND children to lose
            break;
        }
    CHECK(victim != FF_NULL_OFFSET, "fixture has a block with children to damage");
    if (victim == FF_NULL_OFFSET)
        return;

    // ONE witness: the block's own self-offset. The parent slot still names
    // this address and this type.
    arena->base()[static_cast<size_t>(victim)] ^= 0x01;

    Recovery rec(*arena);
    const FF_RecoveryReport rep = rec.recover();
    CHECK_EQ(rep.blocks_total, clean_rep.blocks_total,
             "one damaged witness loses no references");
    CHECK_EQ(rep.holes, static_cast<size_t>(0),
             "the block the parent still vouches for is not left as a hole");
    CHECK_EQ(rep.unrecovered, static_cast<size_t>(0), "nothing is unrecovered");
    CHECK_EQ(rep.ambiguous, static_cast<size_t>(0), "nothing is ambiguous");
    CHECK(rep.position_repaired >= 1, "the damaged self-offset is reported repaired");
}

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
    run("clean_stream_tiles_with_no_gaps", test_clean_stream_tiles_with_no_gaps);
    run("broken_validation_leaves_a_hole", test_broken_validation_leaves_a_hole);
    run("both_witnesses_broken_is_still_found", test_both_witnesses_broken_is_still_found);
    run("same_version_stream_never_reports_skew", test_same_version_stream_never_reports_skew);
    run("compact_archive_is_refused", test_compact_archive_is_refused);
    run("holes_locate_and_size_every_entry_shape", test_holes_locate_and_size_every_entry_shape);
    run("broken_blockref_still_locates_and_sizes_the_orphan",
        test_broken_blockref_still_locates_and_sizes_the_orphan);
    run("one_damaged_witness_costs_nothing", test_one_damaged_witness_costs_nothing);

    std::cout << "\n" << g_tests << " test(s), " << g_failures << " failure(s)\n";
    if (g_tests == 0)
    {
        std::cerr << "  FAIL no tests ran -- a pass on zero coverage is not a pass\n";
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
