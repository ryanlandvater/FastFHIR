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
#include "FF_AllTypes.hpp"

#include "FFHR_tests.hpp"
#include "FFHR_test_corpus.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace FastFHIR;

// ── Test framework (same header-only pattern as test_primitives.cpp) ───────


// ── Helpers ────────────────────────────────────────────────────────────────

// Ingest one FHIR JSON document through the real pipeline and seal it, so the
// stream under test is writer output rather than bytes arranged by hand (COV-1).
// A document may ingest with warnings (a resource outside the profile is kept
// as opaque JSON and reported), so only an outright failure refuses it.
static Memory ingest_json(const std::string &json, std::size_t arena_bytes,
                          FF_ExtensionFilterMode filter = FF_ExtensionFilterMode::FILTER_ALL_KNOWN)
{
    auto arena = Memory::create(arena_bytes);

    FF_BuilderCreateInfo builder_info;
    builder_info.arena = arena;
    builder_info.version = FHIR_VERSION_R5;
    FF_Builder builder;
    if (!FF_CreateBuilder(builder_info, builder))
        return nullptr;

    FF_IngestorCreateInfo ingestor_info;
    FF_Ingestor ingestor;
    if (!FF_CreateIngestor(ingestor_info, ingestor))
        return nullptr;

    Reflective::ObjectHandle bundle_handle;
    Size parsed_count = 0;
    const auto result = FF_Ingest(FF_IngestInfo{
        .ingestor = ingestor,
        .builder = builder,
        .source_type = FF_SOURCE_FHIR_JSON,
        .extension_filter = filter,
        .payload = json,
    }, bundle_handle, parsed_count);
    if (result.failed() || parsed_count == 0 || !bundle_handle)
        return nullptr;

    if (!FF_BuilderSetRoot(FF_BuilderSetRootInfo{
            .builder = builder,
            .root = bundle_handle,
        }))
        return nullptr;
    Memory::View view;
    if (!FF_BuilderFinalize(FF_BuilderFinalizeInfo{
            .builder = builder,
        }, view))
        return nullptr;
    return arena;
}

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
static Memory build_bundle()
{
    return ingest_json(R"({
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
    })", 64 * 1024 * 1024);
}

// RESOURCE TARGETS OUT OF PARENT ORDER, ON PURPOSE (TASKS.md COV-3).
//
// build_bundle() goes through the Ingestor, whose pool places each resource in
// worker-completion order. That order is not a guarantee, so the layout it
// produces is not one either: 200 ingests of that fixture gave five layouts,
// and a recovery rule that assumed parent-ordered placement passed on the
// common one and failed on the rest, 29 runs in 500 under load.
//
// A test that meets a layout only by scheduler luck does not cover it. This
// fixture builds the case directly through the Builder: three contained
// Patients appended BEFORE the Observations that name them, and in the order
// c2, c1, c3, so the middle Observation's target sits below both neighbours'.
// The intact neighbours then bracket a range that excludes the true child --
// exactly the geometry the ingest pool produced when the test failed.
static Memory build_out_of_order_contained()
{
    auto arena = Memory::create(4 * 1024 * 1024);
    FF_BuilderCreateInfo builder_info;
    builder_info.arena = arena;
    FF_Builder builder;
    if (!FF_CreateBuilder(builder_info, builder))
        return nullptr;

    const auto patient = [&](std::string_view id) {
        PatientData p;
        p.id = id;
        return builder->append_obj(p).offset();
    };
    const Offset c2 = patient("c2");
    const Offset c1 = patient("c1");
    const Offset c3 = patient("c3");

    const auto observation = [&](std::string_view id, Offset contained) {
        ObservationData o;
        o.id = id;
        o.status = FF_ObservationStatus::Final;
        o.contained.emplace_back(contained, RECOVER_FF_PATIENT);
        return builder->append_obj(o).offset();
    };
    const Offset o1 = observation("o1", c1);
    const Offset o2 = observation("o2", c2);
    const Offset o3 = observation("o3", c3);

    BundleData bundle;
    bundle.type = FF_BundleType::Collection;
    for (const Offset o : {o1, o2, o3}) {
        BundleentryData entry;
        entry.resource = ResourceReference(o, RECOVER_FF_OBSERVATION);
        bundle.entry.push_back(std::move(entry));
    }
    const auto root = builder->append_obj(bundle);
    if (!FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root}))
        return nullptr;
    Memory::View view;
    if (!FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, view))
        return nullptr;
    return arena;
}

// A private copy of a sealed stream, so each damage case starts from clean bytes.
static Memory copy_of(const Memory &clean)
{
    Memory copy = Memory::create(std::max<size_t>(clean->size(), 1));
    copy->claim_space(clean->size());
    std::memcpy(copy->base(), clean->base(), clean->size());
    return copy;
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

    Recovery rec(arena);
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
    Recovery clean(arena);
    const auto clean_rep = clean.recover();
    CHECK(clean_rep.blocks_total > 0, "clean enumeration is non-empty");
    if (clean_rep.blocks.empty())
        return;
    const auto &r0 = clean_rep.blocks[0].block;
    CHECK(r0.child != FF_NULL_OFFSET, "first reference has a child");

    arena->base()[static_cast<size_t>(r0.child)] ^= 0x01;

    Recovery rec(arena);
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

    Recovery clean(arena);
    const auto clean_rep = clean.recover();
    CHECK(clean_rep.blocks_total > 0, "clean enumeration is non-empty");
    if (clean_rep.blocks.empty())
        return;
    const auto &r0 = clean_rep.blocks[0].block;

    // Flip one bit of the parent's stored offset: the child survives
    // self-consistent but unreachable -> an orphan (REC-12 case 2 / mode 1).
    const size_t slot_abs = static_cast<size_t>(r0.parent + r0.field);
    arena->base()[slot_abs] ^= 0x01;

    Recovery rec(arena);
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

    Recovery clean(arena);
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
    Recovery rec(arena);
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

    Recovery rec(arena);
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

    Recovery clean(arena);
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

    Recovery rec(arena);
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
    // no intact link reaches it. Absence is the only remaining evidence.
    // This is the two-corruption boundary P0-3 names -- gaps do not repair it,
    // but they stop it being silent.
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    Recovery clean(arena);
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

    Recovery rec(arena);
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

    Recovery clean(arena);
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

    Recovery rec(arena);
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

    Parser source(arena);
    auto dest = Memory::create(64 * 1024 * 1024);
    const Memory::View compact = Compactor::archive(Compactor::ArchiveInfo{
        .source = source, .destination = dest});
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

    Recovery clean(arena);
    const StreamMap base = clean.scan();
    CHECK(base.gaps.empty(), "baseline tiles with no gaps");

    const auto victims = one_victim_per_shape(base);
    CHECK(victims.size() >= 3, "fixture covers at least three entry shapes");

    for (const auto &[shape, off] : victims) {
        const Size expect = base.at(off).size;
        const std::string what = std::string(shape_name(shape)) + "@" + std::to_string(off);

        arena->base()[static_cast<size_t>(off)] ^= 0xFF;   // destroy VALIDATION

        Recovery rec(arena);
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

    Recovery after(arena);
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

    Recovery clean(arena);
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

        Recovery rec(arena);
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

    Recovery after(arena);
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

    Recovery clean(arena);
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

    Recovery rec(arena);
    const FF_RecoveryReport rep = rec.recover();
    CHECK_EQ(rep.blocks_total, clean_rep.blocks_total,
             "one damaged witness loses no references");
    CHECK_EQ(rep.holes, static_cast<size_t>(0),
             "the block the parent still vouches for is not left as a hole");
    CHECK_EQ(rep.unrecovered, static_cast<size_t>(0), "nothing is unrecovered");
    CHECK_EQ(rep.ambiguous, static_cast<size_t>(0), "nothing is ambiguous");
    CHECK(rep.position_repaired >= 1, "the damaged self-offset is reported repaired");
}

// REC-15 apply(): a repaired COPY that is strictly better than the damaged
// original, and an original that is byte-for-byte untouched.
//
// The copy is the point. A benchmark that mutates its own input cannot repeat a
// trial, and a before/after comparison needs both halves to still exist.
//
// "Strictly better" is asserted on the aggregate, not on the edge that was
// repaired, because a repair can verify locally and still make the stream worse
// -- rewriting a tag onto an innocent block relabels real data and takes its
// whole subtree out of the census. Measured that way, applying every tag
// rewrite bought 10 intact edges and cost 59 unrecovered ones. Only the
// aggregate catches that, so only the aggregate is trusted here.
static void test_apply_repairs_a_copy_and_improves_it()
{
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    // Damage one witness of a referenced block: the classic repairable edge.
    Recovery probe(arena);
    Offset victim = FF_NULL_OFFSET;
    for (const BlockRef &r : probe.reachable_blocks())
        if (r.child != FF_NULL_OFFSET && r.child != 0) { victim = r.child; break; }
    CHECK(victim != FF_NULL_OFFSET, "fixture has a referenced child to damage");
    if (victim == FF_NULL_OFFSET)
        return;
    arena->base()[static_cast<size_t>(victim)] ^= 0x01;  // VALIDATION damaged

    const std::vector<BYTE> before(arena->base(), arena->base() + arena->size());

    Recovery rec(arena);
    const FF_RecoveryReport dmg = rec.recover();

    std::vector<BYTE> fixed;
    const FF_ApplyReport ar = rec.apply(dmg, fixed);

    // 1. Every verdict is accounted for — nothing silently ignored. Both
    //    populations count: apply() walks the header verdicts (REC-24) and the
    //    block verdicts, and an outcome missing from the totals is exactly the
    //    silent skip this assertion exists to catch.
    CHECK_EQ(ar.applied + ar.declined + ar.failed, dmg.blocks.size() + dmg.header.size(),
             "apply accounts for every verdict");
    // 2. A failed write is reverted, never counted as applied.
    CHECK_EQ(ar.failed_edges.size(), ar.failed, "each failure names its edge");
    // 3. THE ORIGINAL IS UNTOUCHED.
    const std::vector<BYTE> after(arena->base(), arena->base() + arena->size());
    CHECK(before == after, "apply() does not mutate the arena it read");
    CHECK_EQ(fixed.size(), before.size(), "the repaired copy is the same length");

    // 4. The copy is better, and no worse anywhere that matters.
    auto copy = Memory::create(std::max<size_t>(fixed.size(), 1));
    copy->claim_space(fixed.size());
    std::memcpy(copy->base(), fixed.data(), fixed.size());
    Recovery rec2(copy);
    const FF_RecoveryReport rep = rec2.recover();
    CHECK(rep.intact >= dmg.intact, "repair does not reduce the intact edge count");
    CHECK(rep.unrecovered <= dmg.unrecovered, "repair does not create unrecoverable edges");
    CHECK(rep.holes <= dmg.holes, "repair does not open new holes");
    if (ar.applied > 0)
        CHECK(rep.intact > dmg.intact, "an applied repair shows up as an intact edge");
}

// GENERATIONAL RECOVERY, AGAINST A KNOWN DENOMINATOR.
//
// Every other measurement in this area has been taken against a corpus whose
// damage we did not enumerate first, so a number going UP read as a discovery
// ("the loop found 21 more broken references!") when it should have been an
// assertion. In a test we choose the corruption, so the answer is known before
// the run: this breaks exactly three links, and each chain either comes back
// whole or is reported at its top -- never half, and never wrongly.
//
// The shape is what the recovery engine is actually for. Take a chain
//
//      root ─► A ─► B ─► C
//
// and destroy BOTH witnesses of A, B and C: the parent slot that names each one
// AND the block's own VALIDATION word. Each becomes a true hole -- invisible to
// scan(), which finds blocks by their self-offset, and invisible to the walk,
// which cannot follow a broken slot. Nothing in the stream points at them and
// nothing in the stream identifies them.
//
// They must come back GENERATIONALLY. Nothing names B but A, and nothing names
// C but B, so a repair of A is only whole if B and C come with it. The rebuilt
// engine ranks A's branch down to its leaves, B and C included, and decides it
// as one. Six flips in a 1.7 KB fixture leave orphans densely enough that some
// one-bit repoints are not significant; those chains are reported at A instead,
// which is the other acceptable outcome.
// EVERY CHAIN, EVERY FIXTURE (TASKS.md COV-3). This used to damage only the
// FIRST qualifying chain in whichever layout ingest happened to produce, so which
// chain it tested was decided by the scheduler. It failed intermittently for
// exactly that reason: on some layouts the first chain ran through a resource
// tuple whose target sat below its parent. Every chain in both fixtures is cheap
// (a handful each), and it makes the property -- not the layout -- the subject.
static void check_generational_chains(const Memory &clean, const char *fixture)
{
    // The known denominator, taken BEFORE any damage.
    Recovery clean_rec(clean);
    const FF_RecoveryReport baseline = clean_rec.recover();
    CHECK_EQ(baseline.holes, static_cast<size_t>(0), fixture << ": baseline has no holes");
    const std::size_t expected_refs = baseline.blocks_total;

    // Find three-generation chains: a ref whose child is itself a parent whose
    // child is a parent. Indexed by parent so the descent is a lookup.
    const std::vector<BlockRef> refs = clean_rec.reachable_blocks();
    std::map<Offset, std::vector<BlockRef>> by_parent;
    for (const BlockRef &r : refs)
        if (r.child != FF_NULL_OFFSET)
            by_parent[r.parent].push_back(r);

    // TWO WITNESSES, NOT ONE. An inline-block array element lives AT the slot
    // that names it -- `parent + field == child`, because +0 of an inline entry
    // is the element's own offset. There is only one witness there, and
    // "destroy both" would flip the same byte twice and cancel. (That is not a
    // test artifact: a single flip in an inline element destroys its only
    // witness, which makes those elements strictly less recoverable than a
    // pointed-to block. Worth its own coverage; see REC-21.)
    const auto two_witnesses = [](const BlockRef &r) { return r.parent + r.field != r.child; };

    std::size_t chains = 0, recovered = 0;
    for (const BlockRef &a : refs) {
        if (a.child == FF_NULL_OFFSET || !by_parent.count(a.child) || !two_witnesses(a))
            continue;
        for (const BlockRef &b : by_parent[a.child]) {
            if (b.child == FF_NULL_OFFSET || !by_parent.count(b.child) || !two_witnesses(b))
                continue;
            for (const BlockRef &c : by_parent[b.child]) {
                if (c.child == FF_NULL_OFFSET || !two_witnesses(c))
                    continue;
                // Distinct blocks, so three separate holes rather than one
                // block damaged three times.
                if (a.child == b.child || b.child == c.child || a.child == c.child)
                    continue;
                ++chains;

                // FULL HOLES: both witnesses of each generation destroyed.
                //   - the parent's stored offset (the slot no longer names the child)
                //   - the child's VALIDATION word (scan can no longer find it)
                Memory damaged = copy_of(clean);
                BYTE *const base = damaged->base();
                for (const BlockRef *r : {&a, &b, &c}) {
                    base[static_cast<size_t>(r->parent) + static_cast<size_t>(r->field)] ^= 0x01;
                    base[static_cast<size_t>(r->child)] ^= 0x01;
                }
                const Recovery rec(damaged);
                const FF_RecoveryReport rep = rec.recover();
                std::vector<BYTE> repaired;
                rec.apply(rep, repaired);

                // Never wrong: every byte the repair changed moves back to clean.
                std::size_t invented = 0;
                for (std::size_t i = 0; i < clean->size() && i < repaired.size(); ++i)
                    invented += repaired[i] != base[i] && repaired[i] != clean->base()[i];
                CHECK_EQ(invented, std::size_t{0},
                         fixture << " chain ->" << c.child << ": no repair writes a byte that was not damaged");

                // Never silent: the top generation's slot is repaired or reported.
                const BlockVerdict *top = find_verdict(rep, a.parent, a.field);
                const bool repaired_top = top != nullptr && top->block.child == a.child;
                const bool reported_top = top != nullptr && (top->class_ == RepairClass::Ambiguous ||
                                                             top->class_ == RepairClass::Unrecovered);
                CHECK(repaired_top || reported_top,
                      fixture << " chain ->" << c.child << ": the top generation is repaired or reported");

                // Generational: once the top is repaired, every generation below it
                // comes back through its repaired parent -- the whole branch is
                // ranked down to the leaves, never one generation at a time. Where
                // the stream is too small for a repair to be significant (six
                // flips in 1.7 KB leave orphans densely enough that a one-bit
                // repoint would be a coincidence 2% of the time), the top is
                // reported instead and nothing below it is claimed.
                if (!repaired_top)
                    continue;
                ++recovered;
                CHECK(find_verdict(rep, b.parent, b.field) && find_verdict(rep, b.parent, b.field)->block.child == b.child,
                      fixture << " chain ->" << c.child << ": the child generation came back through its repaired parent");
                CHECK(find_verdict(rep, c.parent, c.field) && find_verdict(rep, c.parent, c.field)->block.child == c.child,
                      fixture << " chain ->" << c.child << ": the grandchild generation came back through its repaired child");
                CHECK_EQ(rep.blocks_total, expected_refs,
                         fixture << " chain ->" << c.child << ": a recovered chain accounts for every reference");
            }
        }
    }
    // P0-2: a sweep over zero chains is not a pass.
    CHECK(chains > 0, fixture << ": fixture contains a three-generation chain");
    CHECK(recovered > 0, fixture << ": at least one chain is recovered whole");
    std::cout << "    generational chains [" << fixture << "]: " << recovered << " of " << chains
              << " recovered whole, the rest reported\n";
}

static void test_generational_holes_recover_from_the_root()
{
    const auto ingested = build_bundle();
    REQUIRE(ingested != nullptr, "bundle build failed");
    check_generational_chains(ingested, "ingest");

    const auto ordered = build_out_of_order_contained();
    REQUIRE(ordered != nullptr, "out-of-order fixture build failed");
    check_generational_chains(ordered, "out-of-order");
}

// ONE FLIP IN EVERY RESOURCE TUPLE, IN A LAYOUT THE SCHEDULER DID NOT CHOOSE.
//
// The single-flip reduction of COV-3. A resource slot's target is appended on
// its own, so it can sit anywhere relative to its parent -- including below it,
// with intact neighbours whose children bracket a range that excludes it. The
// REC-23 locality rule read that as misattribution and demoted a correct 1-bit
// repoint to Unrecovered, even though the named block was intact and still
// vouched for itself. Every resource tuple in both fixtures, one at a time: the
// repoint must be Corroborated and must name the TRUE child.
static void check_resource_tuple_repoints(const Memory &clean, const char *fixture)
{
    const std::vector<BlockRef> refs = Recovery(clean).reachable_blocks();
    std::size_t tuples = 0;
    for (const BlockRef &r : refs) {
        if (r.kind != FF_FIELD_RESOURCE || r.child == FF_NULL_OFFSET ||
            r.parent + r.field == r.child)
            continue;
        ++tuples;
        Memory damaged = copy_of(clean);
        damaged->base()[static_cast<size_t>(r.parent) + static_cast<size_t>(r.field)] ^= 0x01;
        const FF_RecoveryReport rep = Recovery(damaged).recover();
        const BlockVerdict *v = find_verdict(rep, r.parent, r.field);
        CHECK(v != nullptr, fixture << " tuple " << r.parent << "+" << r.field << ": has a verdict");
        if (!v)
            continue;
        CHECK(v->class_ == RepairClass::Corroborated,
              fixture << " tuple " << r.parent << "+" << r.field << " -> " << r.child
                      << ": one flipped offset bit is Corroborated, got class "
                      << static_cast<int>(v->class_));
        CHECK(v->block.child == r.child,
              fixture << " tuple " << r.parent << "+" << r.field
                      << ": the repoint names the true child " << r.child);
        CHECK_EQ(rep.unrecovered, static_cast<size_t>(0),
                 fixture << " tuple " << r.parent << "+" << r.field << ": nothing left unrecovered");
    }
    // P0-2: both fixtures carry resource tuples (Bundle.entry.resource and
    // Observation.contained); a sweep that found none tested nothing.
    CHECK(tuples >= 4, fixture << ": fixture carries resource tuples, found " << tuples);
}

static void test_resource_tuple_repoint_independent_of_layout()
{
    const auto ingested = build_bundle();
    REQUIRE(ingested != nullptr, "bundle build failed");
    check_resource_tuple_repoints(ingested, "ingest");

    const auto ordered = build_out_of_order_contained();
    REQUIRE(ordered != nullptr, "out-of-order fixture build failed");
    check_resource_tuple_repoints(ordered, "out-of-order");
}


// REC-22.2 — TAG CONSENSUS, BOTH DIRECTIONS.
//
// A resource/choice reference is a 10-byte tuple {offset(8), tag(2)}, so the
// type is on the wire TWICE: beside the offset, and in the header of the block
// it names. One flipped bit in EITHER copy makes them disagree, and comparing
// them cannot say which one moved.
//
// The measured failure: on a Synthea bundle, seed 19 flipped the slot copy
// (0x1012 -> 0x1010) and seed 33 flipped the child's header identically. Both
// are real resource tags, so plausibility said nothing, and the ranker -- which
// scores both hypotheses at 1 bit -- broke the tie by preferring the parent.
// That is right exactly half the time. Believing the wrong copy is not a bad
// label but a bad SCHEMA: the V-Table gets decoded under another resource's
// field map, which cost 6 real fields and 3 invented ones on a single bit.
//
// So this asserts the two properties that matter, over every resource tuple in
// the bundle and in both directions:
//   1. it is never CONFIDENTLY WRONG -- a decisive verdict always names the
//      original tag, whichever half was damaged;
//   2. it is decisive at all -- a test that only checks (1) passes trivially by
//      never deciding anything.
static void test_tag_consensus_resolves_either_damaged_copy()
{
    auto arena = build_bundle();
    REQUIRE(arena != nullptr, "bundle build failed");

    Recovery clean(arena);
    const auto clean_rep = clean.recover();
    BYTE *const base = arena->base();

    std::size_t decisive_slot = 0, decisive_child = 0, wrong = 0, tried = 0;

    for (const auto &cv : clean_rep.blocks) {
        const BlockRef &r = cv.block;
        if (r.kind != FF_FIELD_RESOURCE && r.kind != FF_FIELD_CHOICE)
            continue;
        if (cv.class_ != RepairClass::Intact || r.child == FF_NULL_OFFSET)
            continue;
        if (++tried > 24)
            break;

        // The tuple's tag half and the child's header tag sit at the same
        // symbolic position -- a tuple has a DATA_BLOCK header's field layout.
        const std::size_t seats[2] = {
            static_cast<std::size_t>(r.parent) + r.field + DATA_BLOCK::RECOVERY,
            static_cast<std::size_t>(r.child) + DATA_BLOCK::RECOVERY,
        };
        for (int half = 0; half < 2; ++half) {
            const std::size_t at = seats[half];
            if (at + 2 > arena->size())
                continue;
            const uint16_t original = FF_GET_RECOVERY_TAG(base, static_cast<Offset>(at - DATA_BLOCK::RECOVERY));

            base[at] ^= 0x02;  // the flip both measured seeds made
            {
                // Judged by the writes: every tag the repair writes must be the
                // original, and the damaged copy must be among them.
                const auto rep = Recovery(arena).recover();
                const auto *v = find_verdict(rep, r.parent, r.field);
                bool restored = false;
                for (const PlannedWrite &w : v ? v->writes : std::vector<PlannedWrite>{}) {
                    if (w.width != 2)
                        continue;
                    wrong    += w.value != original;
                    restored |= w.seat == at && w.value == original;
                }
                if (restored)
                    ++(half == 0 ? decisive_slot : decisive_child);
            }
            base[at] ^= 0x02;  // restore -- the next probe needs a clean bundle
        }
    }

    CHECK(tried > 0, "the bundle contains resource tuples to damage");
    CHECK(wrong == 0, "a tag repair never writes anything but the original tag");
    CHECK(decisive_slot > 0, "a damaged SLOT copy is restored from the child's");
    CHECK(decisive_child > 0, "a damaged CHILD header is restored from the slot's");
    std::printf("    tag consensus: %zu tuples, slot=%zu child=%zu restored, %zu wrong\n",
                tried, decisive_slot, decisive_child, wrong);
}

// REC-21.3 regression -- a damaged INTERIOR element must not truncate the
// array.
//
// walk_array_extent used to return the index of the FIRST element that failed
// to validate. One flipped VALIDATION word inside a large inline-block array
// therefore read as "the array ends here": the classifier emitted
// ExtentDerived and apply() overwrote the INTACT entry count with the
// truncated extent. Measured on a real Synthea bundle: one flipped bit in
// entry 28 of the 1,473-entry Bundle.entry[] array derived an extent of 28,
// the count was rewritten 1473 -> 28, and the reparse lost 97% of the
// document (the bench leaf census dropped 99% -> 3% from that one 2-byte
// write -- isolated by single-write bisection). Element damage is work for
// the element's own reference verdict, never the end of the array: the
// extent is a geometry bound only.
static void test_interior_entry_damage_does_not_truncate_array()
{
    auto arena = build_bundle();
    CHECK(arena != nullptr, "bundle build failed");
    if (!arena)
        return;

    Recovery clean(arena);
    const auto clean_rep = clean.recover();

    // The Bundle.entry[] array: an inline array of FF_BUNDLE_ENTRY blocks
    // whose +0 is each element's own offset (the fixture has 2 entries).
    // Select the array whose element references are inline BLOCKS and that
    // has at least two of them (name[] has one; contained[] elements are
    // 10-byte resource tuples, not blocks).
    Offset entry_array = FF_NULL_OFFSET;
    std::size_t block_elems = 0;
    for (const auto &v : clean_rep.blocks)
    {
        if (v.block.kind != FF_FIELD_ARRAY)
            continue;
        std::size_t n = 0;
        for (const auto &e : clean_rep.blocks)
            if (e.block.parent == v.block.child && e.block.kind == FF_FIELD_BLOCK)
                ++n;
        if (n >= 2)
        {
            entry_array = v.block.child;
            block_elems = n;
            break;
        }
    }
    CHECK(entry_array != FF_NULL_OFFSET, "fixture has a multi-element inline-block array");
    if (entry_array == FF_NULL_OFFSET)
        return;

    // Damage entry[0]'s VALIDATION word (one bit) while every later entry
    // stays intact: provably interior damage, not truncation.
    const Offset elem0 = entry_array + FF_ARRAY::HEADER_SIZE;
    arena->base()[static_cast<size_t>(elem0)] ^= 0x01;

    Recovery rec(arena);
    const auto rep = rec.recover();

    // The array's count must not be extent-derived: the count is intact, the
    // damage is in an element, and elements sit at a fixed stride.
    bool array_extent_derived = false;
    for (const auto &v : rep.blocks)
        if (v.block.kind == FF_FIELD_ARRAY && v.block.child == entry_array &&
            v.class_ == RepairClass::ExtentDerived)
            array_extent_derived = true;
    CHECK(!array_extent_derived,
          "interior element damage must not derive a new array extent");

    // The damaged element itself is repaired by its own reference verdict.
    bool elem0_repaired = false;
    for (const auto &v : rep.blocks)
        if (v.block.kind == FF_FIELD_BLOCK && v.block.parent == entry_array &&
            v.block.child == static_cast<Offset>(elem0) &&
            v.class_ != RepairClass::Intact && v.class_ != RepairClass::Unrecovered &&
            v.class_ != RepairClass::Ambiguous)
            elem0_repaired = true;
    CHECK(elem0_repaired, "the damaged element is repaired by its own verdict");
    std::printf("    entry[] array: %zu inline elements, interior damage -> no extent rewrite\n",
                block_elems);
}

// A NULL ENTRY IS AN ABSENT ELEMENT, NOT THE END OF THE ARRAY.
//
// FHIR has no gaps in `contained[]`, but the public Builder API does: a
// default-constructed ResourceReference holds FF_NULL_OFFSET, and a caller can
// push one into any resource array. The reader treats it as an absent element
// and carries on -- print_json exports every entry after it. The recovery
// enumerator did not: it returned from the whole array at the first null, so
// every later entry vanished from reachable_blocks() (the clean baseline) and
// from recover(). Nothing reported it, because an edge that is never enumerated
// never gets a verdict, damaged or intact. On a damaged stream that edge could
// not be repaired, and the benchmark never chose its bytes to corrupt.
//
// Built through the Builder rather than by hand-editing bytes, so the fixture
// is writer output (COV-1).
static void test_null_array_entry_keeps_later_entries()
{
    auto arena = Memory::create(4 * 1024 * 1024);
    FF_BuilderCreateInfo builder_info;
    builder_info.arena = arena;
    FF_Builder builder;
    CHECK(FF_CreateBuilder(builder_info, builder), "builder created");
    if (!builder)
        return;

    const auto patient = [&](std::string_view id) {
        PatientData p;
        p.id = id;
        return builder->append_obj(p).offset();
    };
    const Offset before = patient("before");
    const Offset after  = patient("after");

    ObservationData o;
    o.id = "o1";
    o.status = FF_ObservationStatus::Final;
    o.contained.emplace_back(before, RECOVER_FF_PATIENT);
    o.contained.emplace_back();                               // null, mid-array
    o.contained.emplace_back(after, RECOVER_FF_PATIENT);
    const Offset obs = builder->append_obj(o).offset();

    BundleData bundle;
    bundle.type = FF_BundleType::Collection;
    BundleentryData entry;
    entry.resource = ResourceReference(obs, RECOVER_FF_OBSERVATION);
    bundle.entry.push_back(std::move(entry));
    const auto root = builder->append_obj(bundle);
    CHECK(FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root}), "root set");
    Memory::View view;
    CHECK(FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, view), "finalized");

    // The reader is the control: it already sees the entry after the null.
    std::ostringstream exported;
    Parser(arena).root().print_json(exported);
    CHECK(exported.str().find("\"after\"") != std::string::npos,
          "the reader exports the entry after the null (control)");

    Recovery rec(arena);
    bool baseline_before = false, baseline_after = false;
    for (const BlockRef &r : rec.reachable_blocks()) {
        baseline_before |= r.child == before;
        baseline_after  |= r.child == after;
    }
    CHECK(baseline_before, "the clean baseline enumerates the entry before the null");
    CHECK(baseline_after, "the clean baseline enumerates the entry after the null");

    const FF_RecoveryReport rep = rec.recover();
    const BlockVerdict *v_after = nullptr;
    for (const BlockVerdict &v : rep.blocks)
        if (v.block.child == after)
            v_after = &v;
    CHECK(v_after != nullptr, "recover() gives the entry after the null a verdict");
    CHECK(v_after && v_after->class_ == RepairClass::Intact,
          "on a clean stream that verdict is Intact");
}


// ═══════════════════════════════════════════════════════════════════════════
// REC-25 / RA-1 — THE CENSUS (recovery_algorithm_handoff.md §6)
// ═══════════════════════════════════════════════════════════════════════════
//
// The rebuilt engine begins with a census. It reads every slot of every block,
// DECIDES each link whose witnesses all agree, and leaves every other slot as
// an open question for the task loop. Everything after it assumes two facts,
// and these cases pin both:
//
//   1. A clean stream leaves nothing open. Every block hangs from the header
//      through decided links, so there is one island, it is attached, and
//      there are no points and no holes.
//   2. One flipped bit opens exactly the question it damaged: the slot whose
//      word it hit, or the slot naming the block whose header it hit, and
//      nothing anywhere else. A census that opened unrelated points would send
//      the solver after damage that is not there; one that missed a point
//      would leave damage no later step ever looks at.

// Does byte `b` lie in the part of a slot's word that decides what it names?
// For a tuple holding a code, that is the low four bytes and the tag half: the
// code is a four-byte value in an eight-byte area, and nothing reads the rest.
static bool census_word_holds(const Recovery::Slot &s, std::size_t b)
{
    const std::size_t seat = static_cast<std::size_t>(s.seat);
    switch (s.repr) {
        case Recovery::SlotRepr::Absolute:
        case Recovery::SlotRepr::Relative63:
            return b >= seat && b < seat + 8;
        case Recovery::SlotRepr::Relative32:
            return b >= seat && b < seat + 4;
        case Recovery::SlotRepr::Tuple:
            if (b >= seat + 8 && b < seat + 10)
                return true;
            return b >= seat &&
                   b < seat + (Recovery_to_Kind(s.stored_tag) == FF_FIELD_CODE ? 4u : 8u);
        case Recovery::SlotRepr::InlineEntry:
            return false;  // no pointer word: the array's geometry names the entry
    }
    return false;
}

// The block a slot's stored word names, decoded in the slot's own encoding.
static Offset census_target_of(const Recovery::Slot &s)
{
    switch (s.repr) {
        case Recovery::SlotRepr::Absolute:
            return static_cast<Offset>(s.stored);
        case Recovery::SlotRepr::Relative32:
            return FF_ResolveCodeableConceptOffset(static_cast<uint32_t>(s.stored), s.parent);
        case Recovery::SlotRepr::Relative63:
            return FF_ResolveDateTimeOffset(s.stored, s.parent);
        case Recovery::SlotRepr::Tuple:
            if (Recovery_to_Kind(s.stored_tag) == FF_FIELD_CODE)
                return FF_ResolveCodeableConceptOffset(static_cast<uint32_t>(s.stored), s.parent);
            if (Recovery_to_Kind(s.stored_tag) == FF_FIELD_DATETIME)
                return FF_ResolveDateTimeOffset(s.stored, s.parent);
            return static_cast<Offset>(s.stored);
        case Recovery::SlotRepr::InlineEntry:
            return s.seat;
    }
    return FF_NULL_OFFSET;
}

static const Recovery::Edge *census_edge_at(const Recovery::Census &c, Offset seat)
{
    const auto it = std::lower_bound(c.edges.begin(), c.edges.end(), seat,
                                     [](const Recovery::Edge &e, Offset s) { return e.slot.seat < s; });
    return (it != c.edges.end() && it->slot.seat == seat) ? &*it : nullptr;
}

static void check_clean_census(const Memory &clean, const std::string &fixture)
{
    const Recovery rec(clean);
    const Recovery::Census c = rec.census();

    CHECK(c.anchors > 0, fixture << ": the census finds self-validating blocks");
    CHECK_EQ(c.islands.size(), std::size_t{1}, fixture << ": islands");
    if (!c.islands.empty()) {
        CHECK(c.islands[0].attached && c.islands[0].root == 0,
              fixture << ": the one island is the FF_HEADER's, attached");
        CHECK_EQ(c.islands[0].members.size(), c.anchors + 1,
                 fixture << ": every block hangs from the header (members, header included)");
    }
    CHECK_EQ(c.points.size(), std::size_t{0}, fixture << ": open points on a clean stream");
    CHECK_EQ(c.holes.size(), std::size_t{0}, fixture << ": holes on a clean stream");

    // The clean-stream baseline is the census's own reachable links, so the two
    // must agree reference for reference. They include the FF_HEADER's slots
    // and the URL directory's segments, which the old chain walk never read.
    const std::vector<BlockRef> refs = rec.reachable_blocks();
    std::size_t missing = 0, header_and_url = 0;
    for (const BlockRef &r : refs) {
        const Recovery::Edge *e = census_edge_at(c, r.parent + r.field);
        missing += e == nullptr || e->child != r.child;
        header_and_url += r.parent == 0 ||
                          FF_GET_RECOVERY_TAG(clean->base(), r.parent) == RECOVER_FF_URL_DIRECTORY;
    }
    CHECK_EQ(missing, std::size_t{0}, fixture << ": baseline references the census did not decide");
    CHECK_EQ(c.edges.size(), refs.size(), fixture << ": every decided link is a baseline reference");
    std::printf("    %s: %zu blocks, %zu decided links (%zu from the header or the URL directory)\n",
                fixture.c_str(), c.anchors, c.edges.size(), header_and_url);
}

static void test_census_clean_stream_is_one_attached_island()
{
    const auto bundle = build_bundle();
    REQUIRE(bundle != nullptr, "bundle build failed");
    check_clean_census(bundle, "bundle");

    const auto out_of_order = build_out_of_order_contained();
    REQUIRE(out_of_order != nullptr, "out-of-order build failed");
    check_clean_census(out_of_order, "out-of-order");

    // Real Synthea bundles carry the shapes the hand-written fixtures do not:
    // code and date/time fallbacks, extensions and their URL directory,
    // resources outside the profile kept as opaque JSON.
    const auto bundles = ff_test::find_bundles(2);
    if (bundles.empty())
        std::printf("    SKIP Synthea: no corpus configured (FASTFHIR_SYNTHEA_DIR)\n");
    for (const auto &path : bundles) {
        // FILTER_NONE, so every Extension.url reaches the URL directory.
        const auto stream = ingest_json(ff_test::read_file(path), std::size_t{1} << 30,
                                        FF_ExtensionFilterMode::FILTER_NONE);
        REQUIRE(stream != nullptr, "ingest " << path.filename().string());
        check_clean_census(stream, path.filename().string().substr(0, 32));
    }
}

// Single flips, one at a time: every bit of a small stream, or a seeded sample
// of the structural bytes of a large one (`sample` > 0). For each flip, every
// point the census opens must be one the flip explains, and every decided link
// whose word or whose child's header the flip hit must be open.
static void census_single_flip_sweep(const Memory &clean, const char *fixture,
                                     std::size_t sample = 0, uint64_t seed = 0)
{
    const Recovery::Census c0 = Recovery(clean).census();
    const std::size_t n = static_cast<std::size_t>(c0.extent);
    REQUIRE(!c0.edges.empty() && n > 0, fixture << ": the clean census decides links to sweep");

    // Which decided link each byte witnesses, from the clean census: the link
    // whose child's header holds the byte, and the link whose word holds it.
    std::vector<Offset> child_header_of(n, FF_NULL_OFFSET);
    std::vector<Offset> word_of(n, FF_NULL_OFFSET);
    for (const Recovery::Edge &e : c0.edges) {
        for (std::size_t j = 0; j < DATA_BLOCK::HEADER_SIZE &&
                                static_cast<std::size_t>(e.child) + j < n; ++j)
            child_header_of[static_cast<std::size_t>(e.child) + j] = e.slot.seat;
        for (std::size_t j = 0; j < 10 && static_cast<std::size_t>(e.slot.seat) + j < n; ++j)
            if (census_word_holds(e.slot, static_cast<std::size_t>(e.slot.seat) + j))
                word_of[static_cast<std::size_t>(e.slot.seat) + j] = e.slot.seat;
    }

    std::vector<std::pair<std::size_t, int>> sites;  // (byte, bit)
    if (sample == 0) {
        for (std::size_t b = 0; b < n; ++b)
            for (int bit = 0; bit < 8; ++bit)
                sites.emplace_back(b, bit);
    } else {
        // Structural bytes only. Everything else is payload the census never reads.
        std::vector<std::size_t> structural;
        for (std::size_t b = 0; b < n; ++b)
            if (child_header_of[b] != FF_NULL_OFFSET || word_of[b] != FF_NULL_OFFSET)
                structural.push_back(b);
        std::mt19937_64 rng(seed);
        std::printf("    %s: sampling %zu of %zu structural bytes, seed %llu\n", fixture, sample,
                    structural.size(), static_cast<unsigned long long>(seed));
        for (std::size_t i = 0; i < sample; ++i)
            sites.emplace_back(structural[rng() % structural.size()], static_cast<int>(rng() % 8));
    }

    Memory damaged = copy_of(clean);
    BYTE *const bytes = damaged->base();
    std::map<std::string, std::size_t> wrong;
    std::vector<std::string> examples;  // the first few, printed to localise a failure
    std::size_t opened = 0;
    const auto record = [&](const char *what, std::size_t b, int bit, const std::string &detail) {
        ++wrong[what];
        if (examples.size() < 12)
            examples.push_back("byte " + std::to_string(b) + " bit " + std::to_string(bit) + " " +
                               detail);
    };
    for (const auto &[b, bit] : sites) {
        bytes[b] ^= static_cast<BYTE>(1u << bit);
        const Recovery::Census c = Recovery(damaged).census();
        bytes[b] ^= static_cast<BYTE>(1u << bit);
        opened += c.points.size();

        // Every point must be explained by this flip.
        for (const Recovery::Point &p : c.points) {
            bool explained = false;
            if (p.kind == Recovery::PointKind::ArrayExtent) {
                // The array's KIND_AND_STEP or ENTRY_COUNT, or the header of one
                // of its inline entries: at the tail of an inline array a
                // flipped entry tag and a flipped count look alike.
                explained = b >= static_cast<std::size_t>(p.array) + FF_ARRAY::KIND_AND_STEP &&
                            b < static_cast<std::size_t>(p.array) + FF_ARRAY::HEADER_SIZE;
                const Recovery::Edge *e = child_header_of[b] != FF_NULL_OFFSET
                                              ? census_edge_at(c0, child_header_of[b]) : nullptr;
                if (e != nullptr && e->slot.repr == Recovery::SlotRepr::InlineEntry &&
                    e->slot.parent == p.array)
                    explained = true;
            } else {
                const Recovery::Edge *e = census_edge_at(c0, p.slot.seat);
                explained = census_word_holds(p.slot, b) ||
                            (e != nullptr && child_header_of[b] == p.slot.seat);
                // A flipped pointer that lands exactly on another slot's child
                // makes that child claimed twice, and both slots open.
                for (const Recovery::Point &q : c.points)
                    if (!explained && e != nullptr && q.kind == Recovery::PointKind::Open &&
                        q.slot.seat != p.slot.seat && census_word_holds(q.slot, b) &&
                        census_target_of(q.slot) == e->child)
                        explained = true;
            }
            if (!explained)
                record("an unexplained point", b, bit,
                       "opened seat " + std::to_string(p.slot.seat) +
                           (p.kind == Recovery::PointKind::ArrayExtent
                                ? " (array " + std::to_string(p.array) + ")" : ""));
        }

        // Every decided link the flip damaged must be open. At the tail of an
        // inline array the census asks about the array instead of the entry.
        const auto asked = [&c](const Recovery::Edge &e) {
            for (const Recovery::Point &p : c.points) {
                if (p.kind == Recovery::PointKind::Open && p.slot.seat == e.slot.seat)
                    return true;
                if (p.kind == Recovery::PointKind::ArrayExtent &&
                    e.slot.repr == Recovery::SlotRepr::InlineEntry && p.array == e.slot.parent)
                    return true;
            }
            return false;
        };
        // The FF_HEADER's own words are Phase 0's to restore, and it does.
        if (const Recovery::Edge *e = word_of[b] != FF_NULL_OFFSET ? census_edge_at(c0, word_of[b]) : nullptr;
            e != nullptr && e->slot.parent != 0 && !asked(*e))
            record("a damaged slot left closed", b, bit,
                   "left seat " + std::to_string(e->slot.seat) + " closed");
        if (const Recovery::Edge *e = child_header_of[b] != FF_NULL_OFFSET
                                          ? census_edge_at(c0, child_header_of[b]) : nullptr;
            e != nullptr && !asked(*e))
            record("a damaged child left closed", b, bit,
                   "left seat " + std::to_string(e->slot.seat) + " closed");
    }
    std::size_t total_wrong = 0;
    for (const auto &[what, count] : wrong) {
        std::printf("    %s: %zu x %s\n", fixture, count, what.c_str());
        total_wrong += count;
    }
    for (const std::string &e : examples)
        std::printf("      e.g. %s\n", e.c_str());
    std::printf("    %s: %zu single flips over %zu bytes, %zu points opened\n",
                fixture, sites.size(), n, opened);
    CHECK_EQ(total_wrong, std::size_t{0},
             fixture << ": flips whose points do not match the damage (first: "
                     << (examples.empty() ? std::string("none") : examples.front()) << ")");
}

// ── REC-25: the branch solver ─────────────────────────────────────────────

// Recover and apply, and count the bytes the repaired copy still gets wrong
// and the ones it invented. Exact means both are zero.
struct Outcome {
    std::size_t still_damaged = 0;
    std::size_t invented      = 0;
    FF_RecoveryReport report;
};

static Outcome recover_and_compare(const Memory &clean, const Memory &damaged)
{
    Outcome o;
    const Recovery rec(damaged);
    o.report = rec.recover();
    std::vector<BYTE> repaired;
    rec.apply(o.report, repaired);
    for (std::size_t i = 0; i < clean->size(); ++i) {
        const BYTE was = damaged->base()[i], now = i < repaired.size() ? repaired[i] : was;
        o.still_damaged += now != clean->base()[i];
        o.invented      += now != clean->base()[i] && now != was;
    }
    return o;
}

// F01, the reason for the rewrite. A resource tuple still says Observation
// (0x1012), and the Observation's own tag has flipped to Bundle (0x1002). Two
// copies of one type, one flip between them: at the tuple the two readings tie
// at 152 bits, and only the block's own slots can decide. A second, unrelated
// flip pair damages a child only the Observation reading names -- its
// self-offset and its tag -- which made the old engine's all-or-nothing test
// fail the Observation reading, pass the Bundle reading (a prefix of it), and
// overwrite the INTACT parent tag: measured on a Synthea bundle, 20 references
// destroyed by three flips. Ranked by corroboration, the Observation reading's
// children outweigh the Bundle reading's, and every flip is restored.
static void test_tag_consensus_never_overwrites_an_intact_parent()
{
    const auto clean = build_bundle();
    REQUIRE(clean != nullptr, "bundle build failed");
    const Recovery::Census c = Recovery(clean).census();

    // The Observation behind a resource tuple, and a block child of it at a
    // field the Bundle's V-Table does not reach.
    Offset obs = FF_NULL_OFFSET, tuple_tag = FF_NULL_OFFSET, only = FF_NULL_OFFSET;
    for (const Recovery::Edge &e : c.edges)
        if (e.slot.repr == Recovery::SlotRepr::Tuple && e.slot.stored_tag == RECOVER_FF_OBSERVATION) {
            obs       = e.child;
            tuple_tag = e.slot.seat + 8;
        }
    REQUIRE(obs != FF_NULL_OFFSET, "the fixture holds an Observation behind a tuple");
    for (const Recovery::Edge &e : c.edges)
        if (e.slot.parent == obs && e.slot.kind == FF_FIELD_BLOCK &&
            e.slot.seat - obs >= Recovery::derived_block_size(RECOVER_FF_BUNDLE))
            only = e.child;
    REQUIRE(only != FF_NULL_OFFSET, "the Observation names a block the Bundle reading cannot see");

    Memory damaged = copy_of(clean);
    BYTE *const bytes = damaged->base();
    bytes[obs + DATA_BLOCK::RECOVERY] ^= 0x10;   // 0x1012 -> 0x1002: the child's copy of its type
    bytes[only + DATA_BLOCK::VALIDATION] ^= 0x01; // the Observation-only child loses its self-offset
    bytes[only + DATA_BLOCK::RECOVERY] ^= 0x01;   // ... and one bit of its tag
    REQUIRE(FF_GET_RECOVERY_TAG(bytes, obs) == RECOVER_FF_BUNDLE, "the flip reads as a Bundle tag");

    const Outcome o = recover_and_compare(clean, damaged);
    bool parent_tag_written = false;
    for (const BlockVerdict &v : o.report.blocks)
        for (const PlannedWrite &w : v.writes)
            parent_tag_written |= w.seat == tuple_tag;
    CHECK(!parent_tag_written, "the intact parent tag is never written");
    CHECK_EQ(o.invented, std::size_t{0}, "no repair writes a byte the damage never touched");
    CHECK_EQ(o.still_damaged, std::size_t{0}, "all three flips are restored");
}

// A lost block whose self-offset took three flips is beyond SELF_RADIUS, so no
// word near its address marks it. The block before it still ends exactly where
// it starts, though, so chaining the hole from that point places it, and the
// parent's intact pointer and the block's residual tag agree with the place.
// 97-98% of the blocks lost at 2,048 flips have an intact predecessor.
static void test_lost_block_recovered_from_predecessor_extent()
{
    const auto clean = build_bundle();
    REQUIRE(clean != nullptr, "bundle build failed");
    const Recovery::Census c = Recovery(clean).census();
    std::size_t tried = 0, restored = 0, invented = 0;
    for (const Recovery::Edge &e : c.edges) {
        if (e.slot.parent == 0 || e.slot.repr != Recovery::SlotRepr::Absolute)
            continue;
        ++tried;
        Memory damaged = copy_of(clean);
        for (const int bit : {0, 9, 18})  // three flips across the self-offset's low bytes
            damaged->base()[e.child + bit / 8] ^= static_cast<BYTE>(1u << (bit % 8));
        const Outcome o = recover_and_compare(clean, damaged);
        restored += o.still_damaged == 0;
        invented += o.invented;
    }
    std::printf("    lost blocks: %zu of %zu restored from their predecessor's extent\n", restored, tried);
    CHECK(tried > 0, "the fixture holds absolute references to lose");
    CHECK_EQ(invented, std::size_t{0}, "no repair writes a byte the damage never touched");
    CHECK_EQ(restored, tried, "every lost block is restored");
}

static void test_census_single_flip_opens_exactly_its_point()
{
    const auto bundle = build_bundle();
    REQUIRE(bundle != nullptr, "bundle build failed");
    census_single_flip_sweep(bundle, "bundle");

    const auto out_of_order = build_out_of_order_contained();
    REQUIRE(out_of_order != nullptr, "out-of-order build failed");
    census_single_flip_sweep(out_of_order, "out-of-order");
}


// ═══════════════════════════════════════════════════════════════════════════
// WP1 — THE GATES (../FastFHIR-benchmark/recovery_handoff.md §6)
// ═══════════════════════════════════════════════════════════════════════════
//
// Everything above this line picks an edge, flips a known bit, and asserts the
// verdict. That shape proves the classifier agrees with whoever wrote the case,
// and it is how a green suite coexisted with a recovery pass that overwrote
// intact tags, could not enact its own hole matches, and dropped whole edges
// without reporting anything.
//
// These gates ask a different question over EVERY eligible byte rather than a
// chosen one, and they judge the BYTES rather than the verdict:
//
//   1. popcount(introduced) == 0, where
//          damage     = clean XOR damaged
//          remaining  = clean XOR repaired
//          introduced = remaining AND NOT damage
//      A repair may only move a damaged bit back toward clean. Writing onto a
//      byte the damage never touched is wrong by construction under the
//      single-flip model, whatever the verdict says about it.
//
//   2. Either the repair is exact, or the report SAYS SO. Every flipped byte
//      still differing from clean must carry an Ambiguous or Unrecovered
//      verdict naming an edge that byte witnesses, or a header verdict naming
//      the field that owns it. "No verdict" and "no damage" must not look
//      alike -- that resemblance IS the silent failure.
//
// Assertion 1 catches wrong writes; assertion 2 catches silent loss. Neither
// can be satisfied by making the ranker bolder, which is the point.
//
// MEASURED. Updated 2026-09-24 after the REC-25 rewrite (census, branch
// solver, transactional apply). Do not change a number here without the
// commit that moved it.
//
//   gate                     ingest        out-of-order   header(432 bits)
//   wrong writes             0  (was 1)    0  (was 0)     0  (was 0)
//   damage left silent       1  (was 43)   1  (was 9)     1  (was 1)
//   confident-but-declined   0             0              0
//   unaccounted verdicts     0             0              0
//   idempotence drift        0             0              --
//   paired flips: wrong 0, silent 0 on both fixtures (was 0/9 and 2/8).
//   ("was" is the engine before the rewrite, after its F29 and F04b fixes.)
//
// The one silent bit left is F31, and it is a limit of the format rather than
// of recovery: FHIR_VERSION_R4 (0x0400) and FHIR_VERSION_R5 (0x0500) are one
// bit apart, so the flip lands exactly on the other legal revision and no
// witness separates them. It keeps single_flip_oracle and
// header_single_bit_enumeration red (WILL_FAIL in tests/tests.cmake).
//
// Declining is an outcome these gates accept wherever it is REPORTED: on these
// 1.7 KB fixtures a few double-damaged links are Ambiguous or Unrecovered,
// because the orphans lie densely enough that a one-bit repoint would be a
// coincidence more often than MAX_CHANCE allows, or because two readings of a
// block tie (a Patient carrying only an id reads equally well as any resource
// type that fits the gap). Rule 7 of recovery_algorithm_handoff.md: recovering
// less is acceptable, recovering wrongly is not.
//
// The ingest fixture's counts can move between runs, because the Ingestor pool
// places resources in worker-completion order (COV-3). Every assertion here is
// "== 0", which holds on any layout. The out-of-order fixture is byte-stable.
//
// A note on what these fixtures CANNOT show. copy_of() sizes each arena to the
// payload exactly, so trusted_extent's ceiling is already the true length and
// an inflating STREAM_SIZE flip is clamped for free. The 4 GiB sweep F09
// describes needs an arena reserved wider than its payload, which is what the
// benchmark uses. Read a green StreamSize result here as "not reproduced on
// this fixture", never as "F09 is fixed".

// Gate tunables, set once from main so a gate widens without a rebuild. A
// randomised gate PINS its seed and prints it: a suite that flakes is a suite
// that gets ignored, and a red log has to name the command that reproduces it.
static uint64_t    g_gate_seed   = 20260922;
static std::size_t g_gate_probes = 4096;  ///< single-bit probes per fixture
static std::size_t g_gate_pairs  = 384;   ///< paired-bit probes per fixture
static std::size_t g_census_samples = 256;  ///< census single flips on a Synthea bundle

// Which half of this binary runs. Three of the gates are RED on purpose, so
// ctest registers them as their own entries and runs the legacy cases with
// `--gates off`: one red gate must not drown the 251 checks that guard
// everything else. `--gates only` is the gates alone, which is what each gate's
// own ctest entry runs. No argument at all runs both, because a developer
// invoking the binary by hand should see the whole picture.
enum class GateMode { All, Off, Only };
static GateMode g_gate_mode = GateMode::All;

static void gate_set_options(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--seed")
            g_gate_seed = std::strtoull(argv[i + 1], nullptr, 10);
        else if (flag == "--probes")
            g_gate_probes = std::strtoull(argv[i + 1], nullptr, 10);
        else if (flag == "--pairs")
            g_gate_pairs = std::strtoull(argv[i + 1], nullptr, 10);
        else if (flag == "--census-samples")
            g_census_samples = std::strtoull(argv[i + 1], nullptr, 10);
        else if (flag == "--gates")
            g_gate_mode = std::string_view(argv[i + 1]) == "off"    ? GateMode::Off
                          : std::string_view(argv[i + 1]) == "only" ? GateMode::Only
                                                                    : GateMode::All;
    }
}

// Dispatch that honours --gates. Both kinds of case are registered through one
// of these two, so a case added later cannot forget the switch.
template <class Fn>
static void run_case(const char *name, Fn fn)
{
    if (g_gate_mode != GateMode::Only)
        ff_test::run(name, fn);
}

template <class Fn>
static void run_gate(const char *name, Fn fn)
{
    if (g_gate_mode != GateMode::Off)
        ff_test::run(name, fn);
}

// A (byte, bit) site the gate flips. One bit per byte is the benchmark's
// damage model exactly (bench_test_5 selects distinct bytes, then XORs one bit
// in each), so a gate built on anything else would be measuring a threat the
// recovery pass was never designed against.
using GateSite = std::pair<std::size_t, int>;

// THE ELIGIBLE SET, mirroring the benchmark's structural_positions().
//
// These are the bytes carrying a structural witness, and they are exactly the
// ones bench_test_5 corrupts: the stream header, every parent slot naming a
// child, and every child's own 10-byte block header. Scalar payloads, string
// bytes and leaf-data references are excluded -- a broken leaf reference has no
// second witness, so no repair is owed for it and damaging one would measure
// the format's design rather than the recovery pass.
//
// Kept in step with the benchmark BY HAND, deliberately: linking the benchmark
// in would drag Bazel and its corpus behind it. If structural_positions()
// changes, this changes with it.
static std::vector<std::size_t> gate_eligible_positions(const Memory &clean,
                                                        const std::vector<BlockRef> &refs)
{
    const std::size_t n = clean->size();
    std::vector<std::size_t> pos;
    for (std::size_t i = 0; i < static_cast<std::size_t>(FF_HEADER::HEADER_SIZE) && i < n; ++i)
        pos.push_back(i);
    for (const BlockRef &r : refs) {
        if (r.child == FF_NULL_OFFSET || static_cast<std::size_t>(r.child) >= n)
            continue;
        const bool tuple = r.kind == FF_FIELD_CHOICE || r.kind == FF_FIELD_RESOURCE;
        const std::size_t slot =
            static_cast<std::size_t>(r.parent) + static_cast<std::size_t>(r.field);
        for (std::size_t j = 0; j < (tuple ? 10u : 8u) && slot + j < n; ++j)
            pos.push_back(slot + j);
        for (std::size_t j = 0;
             j < static_cast<std::size_t>(DATA_BLOCK::HEADER_SIZE) &&
             static_cast<std::size_t>(r.child) + j < n;
             ++j)
            pos.push_back(static_cast<std::size_t>(r.child) + j);
    }
    std::sort(pos.begin(), pos.end());
    pos.erase(std::unique(pos.begin(), pos.end()), pos.end());
    return pos;
}

// Which FF_HEADER field owns a byte. The VERSION word packs the 30-bit engine
// version together with the 2-bit stream layout, and this table is byte
// granular, so it cannot split the one byte they share. It attributes the whole
// word to EngineVersion because that is where 30 of the 32 bits live.
//
// Only the layout is ever REPAIRED. The engine version has no second witness --
// a stream written by a newer engine is legitimate, and find_gaps() depends on
// that being possible to tell version skew from damage -- so recovery declares
// it uncertifiable instead of guessing. That declaration is what this mapping
// looks for: F04b was a flip in those 30 bits landing on an Intact verdict,
// which read as damage neither repaired nor reported.
static bool gate_header_field(std::size_t at, HeaderField &out)
{
    struct Span { std::size_t begin, end; HeaderField field; };
    static const Span spans[] = {
        {FF_HEADER::MAGIC,             FF_HEADER::RECOVERY,          HeaderField::Magic},
        {FF_HEADER::RECOVERY,          FF_HEADER::FHIR_REV,          HeaderField::Recovery},
        {FF_HEADER::FHIR_REV,          FF_HEADER::STREAM_SIZE,       HeaderField::FhirRevision},
        {FF_HEADER::STREAM_SIZE,       FF_HEADER::ROOT_OFFSET,       HeaderField::StreamSize},
        {FF_HEADER::ROOT_OFFSET,       FF_HEADER::ROOT_RECOVERY,     HeaderField::RootOffset},
        {FF_HEADER::ROOT_RECOVERY,     FF_HEADER::CHECKSUM_OFFSET,   HeaderField::RootRecovery},
        {FF_HEADER::CHECKSUM_OFFSET,   FF_HEADER::URL_DIR_OFFSET,    HeaderField::ChecksumOffset},
        {FF_HEADER::URL_DIR_OFFSET,    FF_HEADER::MODULE_REG_OFFSET, HeaderField::UrlDirectoryOffset},
        {FF_HEADER::MODULE_REG_OFFSET, FF_HEADER::VERSION,           HeaderField::ModuleRegistryOffset},
        {FF_HEADER::VERSION,           FF_HEADER::HEADER_SIZE,       HeaderField::EngineVersion},
    };
    for (const Span &s : spans)
        if (at >= s.begin && at < s.end) { out = s.field; return true; }
    return false;
}

static const char *gate_header_name(HeaderField f)
{
    switch (f) {
        case HeaderField::Magic:                return "Magic";
        case HeaderField::Recovery:             return "Recovery";
        case HeaderField::FhirRevision:         return "FhirRevision";
        case HeaderField::StreamSize:           return "StreamSize";
        case HeaderField::RootOffset:           return "RootOffset";
        case HeaderField::RootRecovery:         return "RootRecovery";
        case HeaderField::ChecksumOffset:       return "ChecksumOffset";
        case HeaderField::UrlDirectoryOffset:   return "UrlDirectoryOffset";
        case HeaderField::ModuleRegistryOffset: return "ModuleRegistryOffset";
        case HeaderField::StreamLayout:         return "StreamLayout";
        case HeaderField::EngineVersion:        return "EngineVersion";
    }
    return "?";
}

static const char *gate_class_name(RepairClass c)
{
    switch (c) {
        case RepairClass::Intact:           return "Intact";
        case RepairClass::Corroborated:     return "Corroborated";
        case RepairClass::TagRepaired:      return "TagRepaired";
        case RepairClass::PositionRepaired: return "PositionRepaired";
        case RepairClass::ExtentDerived:    return "ExtentDerived";
        case RepairClass::Ambiguous:        return "Ambiguous";
        case RepairClass::Unrecovered:      return "Unrecovered";
    }
    return "?";
}

// One damage trial end to end: flip the sites in a private copy, recover,
// apply, and measure the repaired bytes against the clean ones.
struct GateProbe {
    FF_RecoveryReport report;
    FF_ApplyReport    written;
    std::vector<BYTE> repaired;
    std::size_t       introduced = 0;  ///< bits changed that the damage never touched
    std::size_t       residual   = 0;  ///< damaged bits still wrong after the repair
    bool              exact      = false;
    bool              same_size  = true;
};

static std::size_t gate_popcount(BYTE b)
{
    return static_cast<std::size_t>(std::popcount(static_cast<unsigned>(b)));
}

static GateProbe gate_probe(const Memory &clean, const std::vector<GateSite> &sites)
{
    GateProbe p;
    Memory damaged = copy_of(clean);
    for (const auto &[at, bit] : sites)
        damaged->base()[at] ^= static_cast<BYTE>(1u << bit);

    Recovery rec(damaged);
    p.report  = rec.recover();
    p.written = rec.apply(p.report, p.repaired);

    const BYTE *const c = clean->base();
    const BYTE *const d = damaged->base();
    const std::size_t n = clean->size();
    p.same_size = p.repaired.size() == n;
    p.exact     = p.same_size;
    for (std::size_t i = 0; i < n && i < p.repaired.size(); ++i) {
        const BYTE damage    = static_cast<BYTE>(c[i] ^ d[i]);
        const BYTE remaining = static_cast<BYTE>(c[i] ^ p.repaired[i]);
        p.introduced += gate_popcount(static_cast<BYTE>(remaining & ~damage));
        p.residual   += gate_popcount(static_cast<BYTE>(remaining & damage));
        if (remaining)
            p.exact = false;
    }
    return p;
}

// Assertion 2's predicate. Every flipped byte the repair did not restore must
// be named by the report: an Ambiguous or Unrecovered verdict on an edge that
// byte witnesses, or a header verdict on the field that owns it. That is the
// report saying "I could not fix this", which is the whole contract. Intact --
// or no verdict at all -- is the silent loss these gates exist to end.
static bool gate_explains(const GateProbe &p, const Memory &clean,
                          const std::vector<BlockRef> &refs,
                          const std::vector<GateSite> &sites, std::size_t &unexplained_at)
{
    const BYTE *const c = clean->base();
    for (const auto &[at, bit] : sites) {
        (void)bit;
        if (at < p.repaired.size() && p.repaired[at] == c[at])
            continue;  // restored — nothing to explain

        HeaderField field{};
        bool explained = false;
        if (gate_header_field(at, field)) {
            for (const HeaderVerdict &v : p.report.header)
                if (v.field == field && (v.class_ == RepairClass::Ambiguous ||
                                         v.class_ == RepairClass::Unrecovered))
                    explained = true;
        }
        // A byte can witness more than one edge -- an inline array element is
        // its own slot -- so any edge it belongs to may carry the report.
        for (const BlockRef &r : refs) {
            if (explained)
                break;
            if (r.child == FF_NULL_OFFSET)
                continue;
            const bool tuple = r.kind == FF_FIELD_CHOICE || r.kind == FF_FIELD_RESOURCE;
            const std::size_t slot =
                static_cast<std::size_t>(r.parent) + static_cast<std::size_t>(r.field);
            const std::size_t child = static_cast<std::size_t>(r.child);
            const bool in_slot  = at >= slot && at < slot + (tuple ? 10u : 8u);
            const bool in_child = at >= child &&
                                  at < child + static_cast<std::size_t>(DATA_BLOCK::HEADER_SIZE);
            if (!in_slot && !in_child)
                continue;
            for (const BlockVerdict &v : p.report.blocks)
                if (v.block.parent == r.parent && v.block.field == r.field &&
                    (v.class_ == RepairClass::Ambiguous || v.class_ == RepairClass::Unrecovered))
                    explained = true;
        }
        if (!explained) {
            unexplained_at = at;
            return false;
        }
    }
    return true;
}

// DIAGNOSTIC ONLY -- which verdict's seat covers a byte apply() changed.
//
// This mirrors the seat arithmetic inside Recovery::apply so a failure can name
// the culprit instead of printing a bare address. It is deliberately NOT an
// assertion: if apply's seats move, this annotation goes stale and gets less
// useful, and nothing silently starts passing because of it.
static std::string gate_blame(const FF_RecoveryReport &rep, std::size_t at,
                              std::string *key = nullptr)
{
    char buf[192];
    HeaderField hf{};
    if (gate_header_field(at, hf)) {
        for (const HeaderVerdict &v : rep.header)
            if (v.field == hf) {
                std::snprintf(buf, sizeof buf, "header %s/%s", gate_header_name(hf),
                              gate_class_name(v.class_));
                if (key) *key = buf;
                return buf;
            }
    }
    for (const BlockVerdict &v : rep.blocks) {
        const std::size_t parent = static_cast<std::size_t>(v.block.parent);
        const std::size_t field  = static_cast<std::size_t>(v.block.field);
        const std::size_t child  = static_cast<std::size_t>(v.block.child);
        const bool wrote_it = std::any_of(v.writes.begin(), v.writes.end(), [at](const PlannedWrite &w) {
            return at >= w.seat && at < w.seat + w.width;
        });
        if (wrote_it) {
            std::snprintf(buf, sizeof buf, "%s parent=%zu field=%zu child=%zu",
                          gate_class_name(v.class_), parent, field, child);
            if (key) *key = gate_class_name(v.class_);
            return buf;
        }
    }
    // Not a seat any repair class writes. Say which edge the byte witnesses
    // instead, because "nothing wrote here" is the interesting half of a
    // silent loss: the edge exists and no verdict claims it.
    for (const BlockVerdict &v : rep.blocks) {
        const std::size_t parent = static_cast<std::size_t>(v.block.parent);
        const std::size_t field  = static_cast<std::size_t>(v.block.field);
        const std::size_t child  = static_cast<std::size_t>(v.block.child);
        const bool tuple = v.block.kind == FF_FIELD_CHOICE || v.block.kind == FF_FIELD_RESOURCE;
        const bool here  = (at >= parent + field && at < parent + field + (tuple ? 10u : 8u)) ||
                          (at >= child &&
                           at < child + static_cast<std::size_t>(DATA_BLOCK::HEADER_SIZE));
        if (!here)
            continue;
        std::snprintf(buf, sizeof buf, "no-write/%s parent=%zu field=%zu child=%zu",
                      gate_class_name(v.class_), parent, field, child);
        if (key) *key = std::string("no-write/") + gate_class_name(v.class_);
        return buf;
    }
    if (key) *key = "no-verdict-at-all";
    return "no-verdict-at-all";
}

// The running tally one oracle sweep produces. Counts, plus the first example
// of each kind: a count says how bad, an example says what to go and look at.
struct GateTally {
    std::size_t probes = 0, wrong_writes = 0, silent = 0, size_drift = 0, unaccounted = 0;
    /// Verdicts in a CONFIDENT repair class that apply() then declined to
    /// write. recover() and apply() disagreeing about what is repairable is
    /// the one disagreement a driver cannot see: the report still counts the
    /// verdict under tag_repaired or corroborated, so the summary claims a
    /// repair that never reached a byte.
    std::size_t declined_confident = 0;
    std::string first_declined;
    std::string first_wrong, first_silent, first_unaccounted;
    /// Where the failures cluster. A count tells you how bad, an example tells
    /// you what to look at, and this tells you which defect you are looking at
    /// -- 292 silent probes is one bug or fifteen, and only this says which.
    std::map<std::string, std::size_t> silent_by, wrong_by;
};

static void gate_histogram(const char *label, const std::map<std::string, std::size_t> &h)
{
    if (h.empty())
        return;
    std::vector<std::pair<std::size_t, std::string>> rows;
    for (const auto &[k, n] : h)
        rows.push_back({n, k});
    std::sort(rows.rbegin(), rows.rend());
    std::printf("      %s by seat:", label);
    for (std::size_t i = 0; i < rows.size() && i < 6; ++i)
        std::printf(" [%s x%zu]", rows[i].second.c_str(), rows[i].first);
    std::printf("\n");
}

static void gate_score(const Memory &clean, const std::vector<BlockRef> &refs,
                       const std::vector<GateSite> &sites, GateTally &t)
{
    const GateProbe p = gate_probe(clean, sites);
    ++t.probes;
    char buf[512];
    const std::size_t at  = sites.front().first;
    const int         bit = sites.front().second;

    if (!p.same_size)
        ++t.size_drift;

    if (p.introduced != 0) {
        ++t.wrong_writes;
        if (t.first_wrong.empty()) {
            // Name the first byte the repair invented, not merely the one that
            // was damaged: they are usually different addresses, and the
            // invented one is where the wrong decision landed.
            std::size_t culprit = at;
            for (std::size_t i = 0; i < clean->size() && i < p.repaired.size(); ++i) {
                const BYTE damage    = static_cast<BYTE>(clean->base()[i] ^ p.repaired[i]);
                bool       flipped   = false;
                for (const auto &[site_at, site_bit] : sites)
                    if (site_at == i) flipped = true;
                if (damage && !flipped) { culprit = i; break; }
            }
            std::snprintf(buf, sizeof buf,
                          "flip byte=%zu bit=%d introduced %zu bit(s); first invented byte=%zu "
                          "clean=%02x repaired=%02x by %s",
                          at, bit, p.introduced, culprit,
                          culprit < clean->size() ? clean->base()[culprit] : 0,
                          culprit < p.repaired.size() ? p.repaired[culprit] : 0,
                          gate_blame(p.report, culprit).c_str());
            t.first_wrong = buf;
        }
        std::size_t culprit = at;
        for (std::size_t i = 0; i < clean->size() && i < p.repaired.size(); ++i) {
            bool flipped = false;
            for (const auto &[site_at, site_bit] : sites)
                if (site_at == i) flipped = true;
            if ((clean->base()[i] ^ p.repaired[i]) && !flipped) { culprit = i; break; }
        }
        std::string key;
        gate_blame(p.report, culprit, &key);
        ++t.wrong_by[key];
    }

    std::size_t unexplained = 0;
    if (!p.exact && !gate_explains(p, clean, refs, sites, unexplained)) {
        ++t.silent;
        if (t.first_silent.empty()) {
            std::snprintf(buf, sizeof buf,
                          "flip byte=%zu bit=%d left byte %zu damaged (clean=%02x repaired=%02x) "
                          "with no Ambiguous/Unrecovered verdict naming it; nearest seat: %s",
                          at, bit, unexplained,
                          unexplained < clean->size() ? clean->base()[unexplained] : 0,
                          unexplained < p.repaired.size() ? p.repaired[unexplained] : 0,
                          gate_blame(p.report, unexplained).c_str());
            t.first_silent = buf;
        }
        std::string key;
        gate_blame(p.report, unexplained, &key);
        ++t.silent_by[key];
    }

    // Expected declines are exactly the verdicts that are not a repair:
    // Intact, Ambiguous and Unrecovered among the blocks, and every header
    // verdict apply does not write. Anything declined beyond that population
    // is a confident class apply refused on a guard the classifier never ran.
    std::size_t confident = 0;
    for (const BlockVerdict &v : p.report.blocks)
        if (v.class_ == RepairClass::Corroborated || v.class_ == RepairClass::TagRepaired ||
            v.class_ == RepairClass::PositionRepaired || v.class_ == RepairClass::ExtentDerived)
            ++confident;
    // The header's root and metadata offsets are written by their references'
    // verdicts, which are already counted above; apply declines their header
    // verdicts by design, so only the other fields are owed a write.
    for (const HeaderVerdict &v : p.report.header)
        if (v.class_ == RepairClass::Corroborated && v.field != HeaderField::RootOffset &&
            v.field != HeaderField::RootRecovery && v.field != HeaderField::ChecksumOffset &&
            v.field != HeaderField::UrlDirectoryOffset && v.field != HeaderField::ModuleRegistryOffset)
            ++confident;
    if (p.written.applied + p.written.failed < confident) {
        const std::size_t missed = confident - p.written.applied - p.written.failed;
        t.declined_confident += missed;
        if (t.first_declined.empty()) {
            std::snprintf(buf, sizeof buf,
                          "flip byte=%zu bit=%d: %zu of %zu confident verdicts were declined by "
                          "apply (applied=%zu failed=%zu declined=%zu) -- the report still "
                          "counts them as repairs",
                          at, bit, missed, confident, p.written.applied, p.written.failed,
                          p.written.declined);
            t.first_declined = buf;
        }
    }

    const std::size_t accounted = p.written.applied + p.written.declined + p.written.failed;
    const std::size_t verdicts  = p.report.blocks.size() + p.report.header.size();
    if (accounted != verdicts) {
        ++t.unaccounted;
        if (t.first_unaccounted.empty()) {
            std::snprintf(buf, sizeof buf,
                          "flip byte=%zu bit=%d: apply accounted for %zu of %zu verdicts "
                          "(applied=%zu declined=%zu failed=%zu)",
                          at, bit, accounted, verdicts, p.written.applied, p.written.declined,
                          p.written.failed);
            t.first_unaccounted = buf;
        }
    }
}

static void gate_report(const GateTally &t, const char *gate, const char *fixture)
{
    CHECK_EQ(t.wrong_writes, static_cast<std::size_t>(0),
             gate << " [" << fixture << "]: repairs that changed an undamaged byte, over "
                  << t.probes << " probes -- " << t.first_wrong);
    CHECK_EQ(t.silent, static_cast<std::size_t>(0),
             gate << " [" << fixture << "]: damage left unrepaired AND unreported, over "
                  << t.probes << " probes -- " << t.first_silent);
    CHECK_EQ(t.unaccounted, static_cast<std::size_t>(0),
             gate << " [" << fixture << "]: applies whose outcomes do not sum to the verdict "
                  << "count, over " << t.probes << " probes -- " << t.first_unaccounted);
    CHECK_EQ(t.size_drift, static_cast<std::size_t>(0),
             gate << " [" << fixture << "]: repaired copies whose length differs from the "
                  << "clean stream, over " << t.probes << " probes");
    CHECK_EQ(t.declined_confident, static_cast<std::size_t>(0),
             gate << " [" << fixture << "]: confident verdicts apply() declined to write, over "
                  << t.probes << " probes -- " << t.first_declined);
}

// ── Gate: a clean stream is never written to (F26) ────────────────────────
static void gate_clean_zero_writes(const Memory &clean, const char *fixture)
{
    Recovery rec(clean);
    const FF_RecoveryReport rep = rec.recover();
    std::vector<BYTE> out;
    const FF_ApplyReport ar = rec.apply(rep, out);

    CHECK_EQ(ar.applied, static_cast<std::size_t>(0),
             fixture << ": apply writes nothing to an undamaged stream");
    CHECK_EQ(ar.failed, static_cast<std::size_t>(0),
             fixture << ": an undamaged stream produces no failed write");
    CHECK_EQ(ar.applied + ar.declined + ar.failed, rep.blocks.size() + rep.header.size(),
             fixture << ": every verdict is accounted for");
    const std::vector<BYTE> before(clean->base(), clean->base() + clean->size());
    CHECK_EQ(out.size(), before.size(), fixture << ": the copy is the same length");
    CHECK(out == before, fixture << ": apply on a clean stream changes no byte");
}

static void test_clean_stream_zero_writes()
{
    const auto ingested = build_bundle();
    REQUIRE(ingested != nullptr, "bundle build failed");
    gate_clean_zero_writes(ingested, "ingest");

    const auto ordered = build_out_of_order_contained();
    REQUIRE(ordered != nullptr, "out-of-order fixture build failed");
    gate_clean_zero_writes(ordered, "out-of-order");
}

// ── Gate: the single-flip oracle ──────────────────────────────────────────
//
// The central gate. Every eligible structural byte, every bit, both fixtures.
// Enumerated completely while the space fits the budget; sampled from a printed
// seed when it does not, so a larger fixture degrades to a reproducible subset
// rather than to a twenty-minute test.
static void gate_single_flip(const Memory &clean, const char *fixture)
{
    const std::vector<BlockRef>    refs = Recovery(clean).reachable_blocks();
    const std::vector<std::size_t> pos  = gate_eligible_positions(clean, refs);
    REQUIRE(!pos.empty(), fixture << ": the fixture has eligible structural bytes");

    std::vector<GateSite> all;
    all.reserve(pos.size() * 8);
    for (const std::size_t at : pos)
        for (int bit = 0; bit < 8; ++bit)
            all.push_back({at, bit});
    const bool sampled = all.size() > g_gate_probes;
    if (sampled) {
        std::mt19937_64 rng(g_gate_seed);
        std::shuffle(all.begin(), all.end(), rng);
        all.resize(g_gate_probes);
    }

    GateTally t;
    for (const GateSite &s : all)
        gate_score(clean, refs, {s}, t);

    std::printf("    single-flip [%s]: %zu refs, %zu eligible bytes, %zu probes (%s seed=%llu)"
                " -> wrong=%zu silent=%zu declined=%zu unaccounted=%zu\n",
                fixture, refs.size(), pos.size(), t.probes,
                sampled ? "sampled" : "exhaustive",
                static_cast<unsigned long long>(g_gate_seed),
                t.wrong_writes, t.silent, t.declined_confident, t.unaccounted);
    gate_histogram("wrong", t.wrong_by);
    gate_histogram("silent", t.silent_by);
    gate_report(t, "single-flip oracle", fixture);
}

static void test_single_flip_oracle()
{
    const auto ingested = build_bundle();
    REQUIRE(ingested != nullptr, "bundle build failed");
    gate_single_flip(ingested, "ingest");

    const auto ordered = build_out_of_order_contained();
    REQUIRE(ordered != nullptr, "out-of-order fixture build failed");
    gate_single_flip(ordered, "out-of-order");
}

// ── Gate: the paired-flip oracle ──────────────────────────────────────────
//
// Two flips, and the pairs that matter are BUILT rather than drawn: both
// witnesses of one edge (the classic hole), and a parent slot with its
// grandchild's header (the cascade). Random pairs alone would reach those
// shapes only by luck, and they are precisely where a repair has to stage more
// than one write or decline.
static std::vector<std::vector<GateSite>> gate_pairs(const Memory &clean,
                                                     const std::vector<BlockRef> &refs)
{
    const std::size_t n = clean->size();
    std::mt19937_64 rng(g_gate_seed);
    std::vector<std::vector<GateSite>> pairs;

    const auto slot_of = [](const BlockRef &r) {
        return static_cast<std::size_t>(r.parent) + static_cast<std::size_t>(r.field);
    };
    const auto bit = [&rng] { return static_cast<int>(rng() & 7u); };

    // Same edge: the parent's offset word and the child's self-offset word.
    for (const BlockRef &r : refs) {
        if (r.child == FF_NULL_OFFSET || static_cast<std::size_t>(r.child) >= n)
            continue;
        if (slot_of(r) + 8 > n)
            continue;
        pairs.push_back({{slot_of(r) + (rng() & 7u), bit()},
                         {static_cast<std::size_t>(r.child) + (rng() & 7u), bit()}});
    }
    // Cascade: a slot naming a block that is itself a parent, damaged together
    // with one of its own children's headers.
    for (const BlockRef &a : refs) {
        if (a.child == FF_NULL_OFFSET)
            continue;
        for (const BlockRef &b : refs) {
            if (b.parent != a.child || b.child == FF_NULL_OFFSET ||
                static_cast<std::size_t>(b.child) >= n)
                continue;
            if (slot_of(a) + 8 > n)
                continue;
            pairs.push_back({{slot_of(a) + (rng() & 7u), bit()},
                             {static_cast<std::size_t>(b.child) + (rng() & 7u), bit()}});
            break;  // one cascade per parent edge is enough to cover the shape
        }
    }
    // The rest drawn at random over the eligible set, so shapes nobody thought
    // of still get hit.
    const std::vector<std::size_t> pos = gate_eligible_positions(clean, refs);
    while (pairs.size() < g_gate_pairs && pos.size() >= 2) {
        const std::size_t i = pos[rng() % pos.size()];
        const std::size_t j = pos[rng() % pos.size()];
        if (i != j)
            pairs.push_back({{i, bit()}, {j, bit()}});
    }
    if (pairs.size() > g_gate_pairs) {
        std::shuffle(pairs.begin(), pairs.end(), rng);
        pairs.resize(g_gate_pairs);
    }
    return pairs;
}

static void gate_paired_flip(const Memory &clean, const char *fixture)
{
    const std::vector<BlockRef> refs = Recovery(clean).reachable_blocks();
    const auto pairs = gate_pairs(clean, refs);
    REQUIRE(!pairs.empty(), fixture << ": the fixture yields damage pairs");

    GateTally t;
    for (const auto &sites : pairs)
        gate_score(clean, refs, sites, t);

    std::printf("    paired-flip [%s]: %zu probes (seed=%llu) -> wrong=%zu silent=%zu "
                "declined=%zu unaccounted=%zu\n",
                fixture, t.probes, static_cast<unsigned long long>(g_gate_seed),
                t.wrong_writes, t.silent, t.declined_confident, t.unaccounted);
    gate_histogram("wrong", t.wrong_by);
    gate_histogram("silent", t.silent_by);
    gate_report(t, "paired-flip oracle", fixture);
}

static void test_paired_flip_oracle()
{
    const auto ingested = build_bundle();
    REQUIRE(ingested != nullptr, "bundle build failed");
    gate_paired_flip(ingested, "ingest");

    const auto ordered = build_out_of_order_contained();
    REQUIRE(ordered != nullptr, "out-of-order fixture build failed");
    gate_paired_flip(ordered, "out-of-order");
}

// ── Gate: every header bit, and the stream still opens (F04, F04b, F09) ───
//
// The header is the one structure with no self-offset of its own, and a single
// flip in it used to cost the WHOLE document: four of twenty benchmark trials
// scored zero, which is the entire distance between the 76.7% mean and the
// 95.6% median. 54 bytes is small enough to enumerate completely, so it is.
static Memory gate_wrap(const std::vector<BYTE> &bytes)
{
    Memory m = Memory::create(std::max<std::size_t>(bytes.size(), 1));
    m->claim_space(bytes.size());
    std::memcpy(m->base(), bytes.data(), bytes.size());
    return m;
}

static void test_header_single_bit_enumeration()
{
    const auto clean = build_bundle();
    REQUIRE(clean != nullptr, "bundle build failed");
    const std::vector<BlockRef> refs = Recovery(clean).reachable_blocks();

    GateTally   t;
    std::size_t unopenable = 0;
    std::string first_unopenable;
    for (std::size_t at = 0; at < static_cast<std::size_t>(FF_HEADER::HEADER_SIZE); ++at) {
        for (int bit = 0; bit < 8; ++bit) {
            const std::vector<GateSite> sites{{at, bit}};
            gate_score(clean, refs, sites, t);

            // The header's own gate: a repaired stream that will not open has
            // recovered nothing, whatever its block verdicts say.
            const GateProbe p = gate_probe(clean, sites);
            HeaderField     field{};
            gate_header_field(at, field);
            try {
                Parser parser(gate_wrap(p.repaired));
                (void)parser.root();
            } catch (const std::exception &e) {
                ++unopenable;
                if (first_unopenable.empty()) {
                    char buf[512];
                    std::snprintf(buf, sizeof buf, "byte=%zu bit=%d field=%s: %s", at, bit,
                                  gate_header_name(field), e.what());
                    first_unopenable = buf;
                }
            }
        }
    }
    std::printf("    header bits: %zu probes -> wrong=%zu silent=%zu declined=%zu "
                "unopenable=%zu\n",
                t.probes, t.wrong_writes, t.silent, t.declined_confident, unopenable);
    gate_histogram("wrong", t.wrong_by);
    gate_histogram("silent", t.silent_by);
    gate_report(t, "header enumeration", "ingest");
    CHECK_EQ(unopenable, static_cast<std::size_t>(0),
             "header enumeration: repaired streams the Parser refuses to open, over "
                 << t.probes << " probes -- " << first_unopenable);
}

// ── Gate: repair is idempotent (F25) ──────────────────────────────────────
//
// A second recovery over an already-repaired stream must write nothing. Any
// byte it changes is structure the first pass invented and the second believed,
// which is how a repair loop drifts away from the original document while every
// individual write verifies.
static void gate_idempotent(const Memory &clean, const char *fixture)
{
    const std::vector<BlockRef>    refs = Recovery(clean).reachable_blocks();
    const std::vector<std::size_t> pos  = gate_eligible_positions(clean, refs);
    REQUIRE(!pos.empty(), fixture << ": the fixture has eligible structural bytes");

    std::mt19937_64 rng(g_gate_seed);
    std::size_t     drifted = 0, probes = 0;
    std::string     first;
    for (std::size_t i = 0; i < pos.size() && probes < g_gate_pairs; ++i) {
        const std::size_t at  = pos[rng() % pos.size()];
        const int         bit = static_cast<int>(rng() & 7u);
        const GateProbe   p   = gate_probe(clean, {{at, bit}});
        ++probes;

        const Memory      once = gate_wrap(p.repaired);
        Recovery          rec(once);
        std::vector<BYTE> twice;
        const FF_ApplyReport ar = rec.apply(rec.recover(), twice);
        if (twice != p.repaired) {
            ++drifted;
            if (first.empty()) {
                std::size_t where = 0;
                for (std::size_t k = 0; k < twice.size() && k < p.repaired.size(); ++k)
                    if (twice[k] != p.repaired[k]) { where = k; break; }
                char buf[512];
                std::snprintf(buf, sizeof buf,
                              "flip byte=%zu bit=%d: the second pass rewrote byte %zu "
                              "(%02x -> %02x), applied=%zu",
                              at, bit, where,
                              where < p.repaired.size() ? p.repaired[where] : 0,
                              where < twice.size() ? twice[where] : 0, ar.applied);
                first = buf;
            }
        }
    }
    std::printf("    idempotence [%s]: %zu probes (seed=%llu) -> drifted=%zu\n", fixture, probes,
                static_cast<unsigned long long>(g_gate_seed), drifted);
    CHECK_EQ(drifted, static_cast<std::size_t>(0),
             "idempotence [" << fixture << "]: repaired streams a second pass changed again, over "
                             << probes << " probes -- " << first);
}

static void test_repair_is_idempotent()
{
    const auto ingested = build_bundle();
    REQUIRE(ingested != nullptr, "bundle build failed");
    gate_idempotent(ingested, "ingest");

    const auto ordered = build_out_of_order_contained();
    REQUIRE(ordered != nullptr, "out-of-order fixture build failed");
    gate_idempotent(ordered, "out-of-order");
}

// ── WP2 gates: apply must enact the WHOLE hypothesis ──────────────────────
//
// The three shapes the WP1 oracles could not separate, because neither fixture
// reaches them by a single flip. Each one is a case where the CLASSIFIER is
// right and the WRITER enacts only part of what it decided, so the report
// claims a repair that never reached the bytes.
//
// All three have the same root cause: a BlockVerdict carries a RepairClass
// LABEL, and apply() re-derives the seats from that label. A label is not a
// plan. Deriving one loses whatever the classifier knew that the label does not
// spell -- which witness was damaged, whether the target needs its own
// self-offset rewritten, whether the slot is a pointer at all.

// Every reference whose slot is an ABSOLUTE 8-BYTE POINTER: the child lives
// elsewhere and the slot names it outright, so the slot and the child are two
// distinct, independently repairable seats.
//
// Two shapes are excluded, and both for the same reason -- their slot is not a
// pointer word, so "restore the 8 bytes at the slot" is not what a repair of
// them even means:
//
//   * an INLINE ARRAY ELEMENT, whose slot IS the element (its own gate below);
//   * a RELATIVE reference. FF_FIELD_CODE stores a 4-byte block-relative
//     fallback and FF_FIELD_DATETIME an 8-byte one, so an absolute write
//     corrupts the slot and, for the 4-byte case, its neighbour too. Those
//     edges are covered by the single-flip oracle's "restored or reported"
//     rule instead, and the repoint gap is recorded as F32.
//
// The test is taken from the WIRE, not the schema, because the schema cannot
// answer it: a choice variant tagged RECOVER_FF_STRING is either a plain string
// variant (absolute) or a date/time fallback (relative). The census sets
// r.child from the raw word for an absolute arm and from a resolver for a
// relative one, so the stored word equalling r.child is decisive.
static std::vector<BlockRef> wp2_pointer_refs(const Memory &clean)
{
    std::vector<BlockRef> out;
    for (const BlockRef &r : Recovery(clean).reachable_blocks()) {
        if (r.child == FF_NULL_OFFSET || r.child == 0)
            continue;
        if (r.parent + r.field == r.child)
            continue;  // inline element — no pointer word to damage
        if (static_cast<std::size_t>(r.child) + DATA_BLOCK::HEADER_SIZE > clean->size())
            continue;
        const std::size_t slot = static_cast<std::size_t>(r.parent + r.field);
        if (slot + 8 > clean->size() ||
            LOAD_U64(clean->base() + slot) != static_cast<uint64_t>(r.child))
            continue;  // relative reference — its slot is not an absolute pointer
        out.push_back(r);
    }
    return out;
}

// Both 8-byte words of an edge, compared against clean.
static bool wp2_word_restored(const Memory &clean, const GateProbe &p, std::size_t seat)
{
    for (std::size_t i = 0; i < 8; ++i)
        if (seat + i >= p.repaired.size() || p.repaired[seat + i] != clean->base()[seat + i])
            return false;
    return true;
}

// F03a — THE HOLE MATCH MUST BE ENACTABLE, NOT MERELY DIAGNOSABLE.
//
// Damage BOTH witnesses of one edge: one bit in the parent's offset word and
// one bit in the child's VALIDATION. Neither witness now names the child, so
// scan() loses it and its bytes become a hole. REC-20's hole matcher is built
// for exactly this: the position whose residual VALIDATION word is a Hamming
// neighbour of its own address is the lost block, and the parent's damaged
// offset is a neighbour of that same address.
//
// The classifier finds it and reports Corroborated. apply() then writes ONLY
// the parent's slot and verifies valid_validation() on the target -- which a
// hole candidate NEVER satisfies, because it was admitted precisely for having
// a damaged self-offset (self_cost is 1 or 2 by construction). So every hole
// match verifies false, reverts, and is counted failed. The diagnosis is right
// and nothing reaches the wire.
static void wp2_hole_repoint(const Memory &clean, const char *fixture)
{
    const std::vector<BlockRef> refs = wp2_pointer_refs(clean);
    REQUIRE(!refs.empty(), fixture << ": the fixture has pointer references");

    std::size_t tried = 0, diagnosed = 0, slot_ok = 0, child_ok = 0, applied = 0;
    std::string first;
    for (const BlockRef &r : refs) {
        const std::size_t slot  = static_cast<std::size_t>(r.parent + r.field);
        const std::size_t child = static_cast<std::size_t>(r.child);
        ++tried;

        // One bit in each witness: the parent now names child±1, and the child
        // vouches for child±1 instead of itself.
        const GateProbe p = gate_probe(clean, {{slot, 0}, {child, 0}});
        const BlockVerdict *v = find_verdict(p.report, r.parent, r.field);
        if (v && v->class_ == RepairClass::Corroborated)
            ++diagnosed;
        const bool s = wp2_word_restored(clean, p, slot);
        const bool c = wp2_word_restored(clean, p, child);
        slot_ok  += s;
        child_ok += c;
        applied  += p.written.applied;
        if (!(s && c) && first.empty()) {
            // Name the PLAN, not just the outcome. A Corroborated verdict that
            // restored nothing is either a wrong candidate or an empty plan,
            // and those need opposite fixes.
            std::size_t duplicates = 0;
            for (const BlockVerdict &bv : p.report.blocks)
                if (bv.block.parent == r.parent && bv.block.field == r.field)
                    ++duplicates;
            char buf[512];
            std::snprintf(buf, sizeof buf,
                          "slot=%zu child=%zu kind=%d verdict=%s verdicts_for_edge=%zu "
                          "writes=%zu cand=%llu slot_clean=%llu slot_repaired=%llu "
                          "slot_restored=%d child_restored=%d applied=%zu "
                          "declined=%zu failed=%zu",
                          slot, child, static_cast<int>(r.kind),
                          v ? gate_class_name(v->class_) : "none", duplicates,
                          v ? v->writes.size() : 0,
                          (v && !v->candidates.empty())
                              ? static_cast<unsigned long long>(v->candidates.front())
                              : 0ULL,
                          static_cast<unsigned long long>(LOAD_U64(clean->base() + slot)),
                          slot + 8 <= p.repaired.size()
                              ? static_cast<unsigned long long>(LOAD_U64(p.repaired.data() + slot))
                              : 0ULL,
                          s, c, p.written.applied, p.written.declined, p.written.failed);
            first = buf;
        }
    }
    std::printf("    hole repoint [%s]: %zu edges, %zu diagnosed Corroborated, "
                "slot restored %zu, child restored %zu\n",
                fixture, tried, diagnosed, slot_ok, child_ok);
    CHECK_EQ(slot_ok, tried, "hole repoint [" << fixture
                                 << "]: the parent's offset is restored -- " << first);
    CHECK_EQ(child_ok, tried, "hole repoint [" << fixture
                                  << "]: the child's self-offset is restored too -- " << first);
}

static void test_hole_repoint_applies_both_witnesses()
{
    const auto ingested = build_bundle();
    REQUIRE(ingested != nullptr, "bundle build failed");
    wp2_hole_repoint(ingested, "ingest");

    const auto ordered = build_out_of_order_contained();
    REQUIRE(ordered != nullptr, "out-of-order fixture build failed");
    wp2_hole_repoint(ordered, "out-of-order");
}

// F03b — A TUPLE'S PRICED TAG REPAIR MUST ACTUALLY BE WRITTEN.
//
// Damage both of the CHILD's witnesses and leave the parent alone: one bit in
// the child's VALIDATION and one in the child's recovery tag. The parent's
// tuple still holds the address and a copy of the type, so a repair must write
// the self-offset and the tag together (F03b: the old applier wrote the
// VALIDATION word and stopped). When the block's own slots cannot separate its
// type from a neighbouring one, the honest answer is to decline and say so.
static void wp2_tuple_pair(const Memory &clean, const char *fixture)
{
    std::size_t tried = 0, whole = 0, reported = 0;
    std::string first;
    for (const BlockRef &r : wp2_pointer_refs(clean)) {
        if (r.kind != FF_FIELD_RESOURCE && r.kind != FF_FIELD_CHOICE)
            continue;
        const std::size_t child = static_cast<std::size_t>(r.child);
        const std::size_t tag   = child + static_cast<std::size_t>(DATA_BLOCK::RECOVERY);
        if (tag + 2 > clean->size())
            continue;
        ++tried;

        const GateProbe p = gate_probe(clean, {{child, 0}, {tag, 1}});
        const BlockVerdict *v = find_verdict(p.report, r.parent, r.field);
        const bool s = wp2_word_restored(clean, p, child);
        const bool t = tag + 1 < p.repaired.size() &&
                       p.repaired[tag] == clean->base()[tag] &&
                       p.repaired[tag + 1] == clean->base()[tag + 1];
        // Both witnesses restored, or neither and the verdict SAYS so. When the
        // two readings of the child tie -- a Patient carrying only an id reads
        // as well under any resource type that fits the gap -- declining is
        // the right answer, and half a repair never is.
        const bool declined = !s && !t && v != nullptr &&
                              (v->class_ == RepairClass::Ambiguous || v->class_ == RepairClass::Unrecovered);
        whole    += (s && t) || declined;
        reported += declined;
        if (!((s && t) || declined) && first.empty()) {
            char buf[384];
            std::snprintf(buf, sizeof buf,
                          "child=%zu verdict=%s cost=%u self_restored=%d tag_restored=%d "
                          "(clean tag %02x%02x, repaired %02x%02x)",
                          child, v ? gate_class_name(v->class_) : "none", v ? v->bit_cost : 0, s,
                          t, clean->base()[tag], clean->base()[tag + 1],
                          tag < p.repaired.size() ? p.repaired[tag] : 0,
                          tag + 1 < p.repaired.size() ? p.repaired[tag + 1] : 0);
            first = buf;
        }
    }
    std::printf("    tuple pair [%s]: %zu tuples, %zu repaired whole, %zu declined and reported\n", fixture,
                tried, whole - reported, reported);
    CHECK(tried > 0, fixture << ": the fixture carries resource or choice tuples");
    CHECK_EQ(whole, tried,
             "tuple pair [" << fixture << "]: self-offset and tag restored together, or neither and reported -- "
                            << first);
}

static void test_tuple_self_and_tag_repair_together()
{
    const auto ingested = build_bundle();
    REQUIRE(ingested != nullptr, "bundle build failed");
    wp2_tuple_pair(ingested, "ingest");

    const auto ordered = build_out_of_order_contained();
    REQUIRE(ordered != nullptr, "out-of-order fixture build failed");
    wp2_tuple_pair(ordered, "out-of-order");
}

// F10 — AN INLINE ELEMENT HAS NO POINTER, SO IT CAN NEVER BE REPOINTED.
//
// An inline array element is stored AT its slot: the census reads it with
// child == parent + field, because the element's address comes from array
// geometry and nothing stores it. There is therefore no pointer word to
// repair, and the only meaningful repair is in place.
//
// apply()'s Corroborated arm does not know that. It writes an 8-byte target
// offset at `parent + field`, which for these references IS the element's own
// VALIDATION word -- so a repoint overwrites the element's self-offset with
// another block's address. It happens to be harmless when the winning candidate
// is the element itself, since the value written is then the address it should
// hold anyway, but that is luck and not design: any other candidate silently
// relabels the element as living somewhere it does not.
//
// So this asserts the STRUCTURAL claim, not the lucky byte: no verdict on an
// inline element may ever be Corroborated, whatever value it would have written.
static void wp2_inline_elements(const Memory &clean, const char *fixture)
{
    std::vector<BlockRef> inlines;
    for (const BlockRef &r : Recovery(clean).reachable_blocks())
        if (r.child != FF_NULL_OFFSET && r.parent + r.field == r.child &&
            static_cast<std::size_t>(r.child) + DATA_BLOCK::HEADER_SIZE <= clean->size())
            inlines.push_back(r);
    CHECK(!inlines.empty(), fixture << ": the fixture carries inline array elements");

    std::size_t repointed = 0, restored = 0;
    std::string first;
    for (const BlockRef &r : inlines) {
        const std::size_t at = static_cast<std::size_t>(r.child);
        const GateProbe   p  = gate_probe(clean, {{at, 0}});

        // The damaged element itself, and every other verdict in the report: an
        // inline element must not be repointed no matter which edge is damaged.
        for (const BlockVerdict &bv : p.report.blocks)
            if (bv.class_ == RepairClass::Corroborated &&
                bv.block.child != FF_NULL_OFFSET &&
                bv.block.parent + bv.block.field == bv.block.child) {
                ++repointed;
                if (first.empty()) {
                    char buf[320];
                    std::snprintf(buf, sizeof buf,
                                  "damaged element %zu produced a Corroborated verdict on inline "
                                  "element parent=%llu field=%llu child=%llu",
                                  at, static_cast<unsigned long long>(bv.block.parent),
                                  static_cast<unsigned long long>(bv.block.field),
                                  static_cast<unsigned long long>(bv.block.child));
                    first = buf;
                }
            }
        restored += wp2_word_restored(clean, p, at);
    }
    std::printf("    inline elements [%s]: %zu elements, %zu restored, %zu repoint verdicts\n",
                fixture, inlines.size(), restored, repointed);
    CHECK_EQ(repointed, static_cast<std::size_t>(0),
             "inline elements [" << fixture
                                 << "]: an inline element is never repointed -- " << first);
    CHECK_EQ(restored, inlines.size(),
             "inline elements [" << fixture << "]: a damaged inline element is repaired in place");
}

static void test_inline_element_is_never_repointed()
{
    const auto ingested = build_bundle();
    REQUIRE(ingested != nullptr, "bundle build failed");
    wp2_inline_elements(ingested, "ingest");

    const auto ordered = build_out_of_order_contained();
    REQUIRE(ordered != nullptr, "out-of-order fixture build failed");
    wp2_inline_elements(ordered, "out-of-order");
}

// ── REC-25 gate: the census on a real stream, single flips sampled ─────────
//
// The exhaustive census sweep runs on the two small fixtures, which hold no
// date/time fallback and no URL directory, and only one code fallback. A
// Synthea bundle holds all of them, and it is far too large to sweep bit by
// bit, so this samples its structural bytes under the pinned gate seed.
static void test_census_single_flip_sample_synthea()
{
    const auto bundles = ff_test::find_bundles(1);
    if (bundles.empty()) {
        // Without the corpus there is nothing new to learn, but a gate that
        // runs no check at all reports as a failure, and rightly. So the
        // sampler runs over the hand-built fixture instead and says so.
        std::printf("    SKIP Synthea: no corpus configured (FASTFHIR_SYNTHEA_DIR); "
                    "sampling the ingest fixture instead\n");
        const auto bundle = build_bundle();
        REQUIRE(bundle != nullptr, "bundle build failed");
        census_single_flip_sweep(bundle, "bundle", g_census_samples, g_gate_seed);
        return;
    }
    const auto stream = ingest_json(ff_test::read_file(bundles.front()), std::size_t{1} << 30,
                                    FF_ExtensionFilterMode::FILTER_NONE);
    REQUIRE(stream != nullptr, "ingest " << bundles.front().filename().string());
    // copy_of() sizes the arena to the payload, as every other gate does.
    census_single_flip_sweep(copy_of(stream), "synthea", g_census_samples, g_gate_seed);
}

int main(int argc, char **argv)
{
    ff_test::set_filter(argc, argv);
    gate_set_options(argc, argv);


    TEST_GROUP("Recovery");
    run_case("clean_stream_zero_false_positives", test_clean_stream_zero_false_positives);
    run_case("validation_flip_position_repaired", test_validation_flip_position_repaired);
    run_case("offset_flip_corroborated", test_offset_flip_corroborated);
    run_case("both_halves_never_silent", test_both_halves_never_silent);
    run_case("clean_stream_tiles_with_no_gaps", test_clean_stream_tiles_with_no_gaps);
    run_case("broken_validation_leaves_a_hole", test_broken_validation_leaves_a_hole);
    run_case("both_witnesses_broken_is_still_found", test_both_witnesses_broken_is_still_found);
    run_case("same_version_stream_never_reports_skew", test_same_version_stream_never_reports_skew);
    run_case("compact_archive_is_refused", test_compact_archive_is_refused);
    run_case("holes_locate_and_size_every_entry_shape", test_holes_locate_and_size_every_entry_shape);
    run_case("broken_blockref_still_locates_and_sizes_the_orphan",
        test_broken_blockref_still_locates_and_sizes_the_orphan);
    run_case("one_damaged_witness_costs_nothing", test_one_damaged_witness_costs_nothing);
    run_case("apply_repairs_a_copy_and_improves_it", test_apply_repairs_a_copy_and_improves_it);
    run_case("generational_holes_recover_from_the_root",
        test_generational_holes_recover_from_the_root);
    run_case("resource_tuple_repoint_independent_of_layout",
        test_resource_tuple_repoint_independent_of_layout);
    run_case("interior_entry_damage_does_not_truncate_array",
        test_interior_entry_damage_does_not_truncate_array);
    run_case("null_array_entry_keeps_later_entries",
        test_null_array_entry_keeps_later_entries);
    run_case("tag_consensus_resolves_either_damaged_copy",
        test_tag_consensus_resolves_either_damaged_copy);

    // REC-25: the rebuilt engine, one work package at a time
    // (recovery_algorithm_handoff.md §12).
    run_case("census_clean_stream_is_one_attached_island",
        test_census_clean_stream_is_one_attached_island);
    run_case("census_single_flip_opens_exactly_its_point",
        test_census_single_flip_opens_exactly_its_point);
    run_case("tag_consensus_never_overwrites_an_intact_parent",
        test_tag_consensus_never_overwrites_an_intact_parent);
    run_case("lost_block_recovered_from_predecessor_extent",
        test_lost_block_recovered_from_predecessor_extent);

    // WP1 gates (recovery_handoff.md §6) -- byte-level oracles over every
    // eligible structural byte, not a hand-picked edge. Several are expected
    // RED until their work package lands; the list is in the block comment
    // above gate_set_options().
    run_gate("clean_stream_zero_writes", test_clean_stream_zero_writes);
    run_gate("single_flip_oracle", test_single_flip_oracle);
    run_gate("paired_flip_oracle", test_paired_flip_oracle);
    run_gate("header_single_bit_enumeration", test_header_single_bit_enumeration);
    run_gate("repair_is_idempotent", test_repair_is_idempotent);

    // WP2 gates: apply must enact the whole hypothesis, not the part its
    // RepairClass label happens to spell.
    run_gate("hole_repoint_applies_both_witnesses", test_hole_repoint_applies_both_witnesses);
    run_gate("tuple_self_and_tag_repair_together", test_tuple_self_and_tag_repair_together);
    run_gate("inline_element_is_never_repointed", test_inline_element_is_never_repointed);

    // REC-25 gates.
    run_gate("census_single_flip_sample_synthea", test_census_single_flip_sample_synthea);

    std::cout << "\n" << ::ff_test::g_checks << " test(s), " << ::ff_test::g_failures << " failure(s)\n";
    if (::ff_test::g_checks == 0)
    {
        std::cerr << "  FAIL no tests ran -- a pass on zero coverage is not a pass\n";
        return 1;
    }
    return ::ff_test::g_failures == 0 ? 0 : 1;
}
