/**
 * @file FF_Recovery.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * @version 0.1
 *
 * @brief FastFHIR Archive Recovery — implementation (TASKS.md REC-10…19)
 *
 * The two-sided reconciliation of P0-3: every parent→child block reference is
 * encoded twice (parent slot {expected tag, stored offset} + child header
 * {VALIDATION, RECOVERY}), and recover() turns the surviving half into a
 * restored reference, classified by which witness supplied the fix.
 *
 * Threat model: bit-flip only (REC-17). Every read is bounds-checked; nothing
 * here dereferences a header field at construction (CAPI-15).
 *
 * LAYOUT CONTRACT (REC-19.1): the call stack reads top→bottom. The inline
 * helpers are defined at the top so everything below can use them; the
 * entrance recover() sits at the top of the call stack; every callee is
 * defined below its caller; the leaf helpers end the file. Reading the file
 * top→bottom traces recover()'s execution, from entrance to the final
 * recovery of the stream.
 */

#include "FF_Recovery.hpp"
#include "FF_Ops.hpp"

#include <algorithm>
#include <bit>
#include <exception>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace FastFHIR {

// =====================================================================
// TOP — the inline helpers, predeclared and defined here (REC-19.1)
// =====================================================================
namespace {

// ---- leaf reads (free, not members: "what is at THIS offset" — the
// ---- members became free functions so the inline helpers above them can
// ---- call them; a member forces conjuring a block whose fabricated size
// ---- and version change what validate_full checks) ----

inline bool valid_validation(const BYTE* base, size_t size, Offset off) noexcept {
    if (!FF_BLOCK_IN_BOUNDS(off, size))
        return false;
    return FF_GET_VALIDATION(base, off) == static_cast<uint64_t>(off);
}

inline RECOVERY_TAG tag_at(const BYTE* base, size_t size, Offset off) noexcept {
    if (!FF_BLOCK_IN_BOUNDS(off, size))
        return FF_RECOVER_UNDEFINED;
    return FF_GET_RECOVERY_TAG(base, off);
}

inline bool header_is_readable(const BYTE* base, size_t size) noexcept {
    if (size < FF_HEADER::HEADER_SIZE)
        return false;
    return FF_HEADER(size).get_magic(base) == FF_MAGIC_BYTES;
}

// ---- slot-kind discriminators ----

// Loose "is this an array-shaped tag" test: the array bit means the block
// carries ENTRY_COUNT/KIND_AND_STEP after the DATA_BLOCK header.
inline constexpr bool IsArrayTagged(RECOVERY_TAG tag) {
    return (tag & RECOVER_ARRAY_BIT) != 0;
}

// The slot kinds whose parent half stores {offset | tag} as a 10-byte tuple.
inline constexpr bool IsTupleKind(FF_FieldKind kind) {
    return kind == FF_FIELD_CHOICE || kind == FF_FIELD_RESOURCE;
}

// The slot kinds whose parent half stores a bare 8-byte offset with a
// compile-time expected child type (1c is authoritative and uncorruptible).
inline constexpr bool IsTypedOffsetKind(FF_FieldKind kind) {
    return kind == FF_FIELD_BLOCK || kind == FF_FIELD_STRING ||
           kind == FF_FIELD_ARRAY || kind == FF_FIELD_DATETIME;
}

// FF_ARRAY element shapes, discriminated by element tag + entry kind. The
// discriminator matters: EntryKind alone cannot tell an inline scalar from an
// inline block (CLAUDE.md), and the two pointer shapes need target-walking,
// not entry-walking (TASKS.md F4).
// How to READ one array element. This is NOT the storage kind: every value
// below except OffsetPtr describes an array whose entries are stored INLINE
// (contiguous, fixed stride, no offset table). FF_ARRAY::OFFSET exists only for
// FF_STRING, which is the one variable-length element type -- 693 of the 693
// OFFSET arrays in a Synthea bundle are strings, and store.py emits that kind
// from exactly one branch. Do not read `InlineBlock` as "the array is inline";
// it means "this element's own bytes are a self-describing DATA_BLOCK header".
enum class ElementShape : uint8_t {
    None,        // raw scalars — inline, no header, extent not derivable
    InlineBlock, // inline, and +0 IS the element's own offset (walk in place)
    Tuple,       // inline 10-byte {value|tag} — +0 is the TARGET's offset (follow it)
    OffsetPtr,   // OFFSET table — 8-byte pointers to FF_STRING blocks (strings only)
};

inline ElementShape element_shape_of(RECOVERY_TAG element_tag, FF_ARRAY::EntryKind kind) noexcept {
    // The top two bits are the wire truth for the physical layout (FF_ARRAY
    // EntryKind, already pre-shifted): OFFSET and SCALAR need no tag help.
    if (kind == FF_ARRAY::EntryKind::OFFSET)
        return ElementShape::OffsetPtr;
    if (kind == FF_ARRAY::EntryKind::SCALAR)
        return ElementShape::None;  // raw values — no headers, extent not derivable
    // INLINE_BLOCK is stamped on scalars, block headers and resource tuples
    // alike (CLAUDE.md): the element TAG is the discriminator (F4).
    if (FF_IsScalarBlockTag(element_tag))
        return ElementShape::None;
    // RECOVER_FF_RESOURCE is the GENERIC polymorphic marker and lives in the
    // PRIMITIVE band (0x0003), so FF_IsResourceTag -- a 0x1000..0x1FFF range
    // test -- does not match it. Without this line a `contained[]` /
    // `Bundle.entry[]` array fell through to InlineBlock and walk_array_extent
    // asked a POINTER whether it equalled its own position; it never does, so
    // the walk bailed at element 0 and reported a bogus ExtentDerived on 46
    // arrays of every clean Synthea bundle. CLAUDE.md: "a resource tuple is
    // {offset(8), tag(2)} ... +0 is the TARGET's offset, so it must be
    // followed, never walked in place."
    if (element_tag == RECOVER_FF_RESOURCE || FF_IsResourceTag(element_tag))
        return ElementShape::Tuple;
    return ElementShape::InlineBlock;
}

// ---- REC-19.3 — the one routine every reference is judged by ----

// Judge one parent→child reference: does the child self-validate, and does its
// wire RECOVERY tag match what the slot expects? The expectation is 1c — the
// compiled V-Table child type, uncorruptible — for typed-offset kinds, and the
// STORED tag half of the {value|recovery} 10-byte tuple for choice/resource
// slots (F1: the compiled table is a decoy there, so the enumerator already
// resolved `declared` to the stored half).
//
// Returns true when the reference is damaged — the child fails to
// self-validate (the slot names a block whose VALIDATION word is not its own
// address: `InvalidSelfRef`), or the wire tag does not match the expected
// recovery (`VTableRecoveryMismatch`). Callers queue such refs for the
// classifier; a coherent ref is followed deeper. `failures` may be null when
// the caller only wants the verdict, not the audit record (the clean-stream
// baseline walks with no audit).
//
// This is deliberately NOT the walk's descent gate: the walk descends into a
// child whose self-offset is merely within the flip budget (one surviving
// witness, defect 1), while this routine reports any self-validation failure
// as damage. Recording and descent are different decisions with different
// evidence; REC-19.7's reapply loop re-uses this exact judgment after a
// repair to move deeper into the fixed block.
// Wrong-turn-2's self-verification, in one place because two call sites need
// it (the expansion and the REC-19.7 reapply).
//
// Re-reading a block under a corrected type is a HYPOTHESIS, and the ranker's
// preference is evidence, not proof. When the hypothesis is wrong the V-Table
// does not belong to that block, and every offset lifted through it is
// nonsense. That is cheap to detect: read under the right type, a block's
// slots yield children that are themselves well-formed; read under the wrong
// one, they yield children with no surviving witness at all. So enumerate into
// scratch, ask this, and discard the whole batch on a single incoherent child.
//
// Keeping garbage is strictly worse than dropping a subtree: a missing
// reference is reported and therefore honest, a fabricated one is believed.
// How strictly must a freshly enumerated batch of references hold up?
//
// Two questions, one predicate, because they differ only in strictness and a
// family of near-identical functions is how a caller ends up picking by vibe.
// Using the wrong strength does not fail loudly -- it silently disables the
// caller, which is how the generational cascade stayed broken.
enum class BatchTest : uint8_t {
    /// Every child must corroborate. The type is a HYPOTHESIS here, so a wrong
    /// V-Table -- which yields nothing but nonsense -- has to be thrown out
    /// whole before any of it is believed.
    EveryChildCorroborates,
    /// No child may be a wild pointer, and damage is expected. The type is
    /// already CORROBORATED by the tuple distance, so the V-Table is trusted
    /// and the only remaining question is whether the read produced nonsense.
    NoWildPointers,
};

// Demanding corroboration where only addressability is warranted is what
// silenced the generational cascade: a block whose one child is itself damaged
// has no corroborating child BY DEFINITION, so its batch was discarded and the
// chain stopped at the first repair. Parent broken, child broken, grandchild
// broken is exactly what that loop exists to unwind.
//
// A wrong V-Table still cannot survive NoWildPointers: it lifts offsets out of
// positions that hold none, and those are overwhelmingly outside the stream.
// One that happens to land in bounds yields references that classify as
// Unrecovered -- reported, never believed -- and repairing them would demand
// the same tuple evidence over again.
inline bool batch_passes(const BYTE* base, size_t size,
                         const std::vector<BlockRef>& refs, BatchTest test) noexcept {
    for (const BlockRef& c : refs) {
        if (c.child == FF_NULL_OFFSET)
            continue;
        if (!FF_BLOCK_IN_BOUNDS(c.child, static_cast<Size>(size)))
            return false;  // a wild pointer fails both tests
        if (test == BatchTest::NoWildPointers)
            continue;
        if (valid_validation(base, size, c.child))
            continue;
        const RECOVERY_TAG ct = tag_at(base, size, c.child);
        const RECOVERY_TAG cb = (c.kind == FF_FIELD_ARRAY) ? GetTypeFromTag(ct) : ct;
        if (c.declared == FF_RECOVER_UNDEFINED || cb != c.declared)
            return false;  // a child with no surviving witness — garbage
    }
    return true;
}

// The address a repaired reference should be FOLLOWED to.
//
// Not always the one the slot names. Corroborated means the REPOINT hypothesis
// won -- the ranker decided the parent's stored offset is the damaged half and
// the real child is the candidate it found -- so following r.child there walks
// the very address the ranker just rejected. TagRepaired and PositionRepaired
// are in-place repairs and keep their address. Following the rejected offset
// under the declared V-Table is what invented references in 5 of 40 single-bit
// trials, every one of them a flip in the PARENT's slot.
inline Offset repaired_target(const BlockVerdict& v) noexcept {
    if (v.class_ == RepairClass::Corroborated)
        return v.candidates.empty() ? FF_NULL_OFFSET : v.candidates.front();
    return v.block.child;
}

inline bool recover_follow_ref_chain(const BYTE* base, size_t size, const BlockRef& r,
                                     std::vector<ProducerFailure>* failures) noexcept {
    if (!FF_BLOCK_IN_BOUNDS(r.child, size))
        return false;  // absent or out of stream — no reference to judge
    const bool self_ok = valid_validation(base, size, r.child);
    if (self_ok) {
        const RECOVERY_TAG actual = tag_at(base, size, r.child);
        const RECOVERY_TAG actual_base =
            (r.kind == FF_FIELD_ARRAY) ? GetTypeFromTag(actual) : actual;
        if (r.declared == FF_RECOVER_UNDEFINED || actual_base == r.declared)
            return false;  // both witnesses agree — coherent
        if (failures)
            failures->push_back({ProducerFailureKind::VTableRecoveryMismatch, r.child,
                                 r.declared, actual,
                                 "wire tag does not match the slot's expected recovery"});
    } else if (failures) {
        failures->push_back({ProducerFailureKind::InvalidSelfRef, r.child, r.declared,
                             FF_RECOVER_UNDEFINED,
                             "slot names a block that does not self-validate"});
    }
    return true;
}

// The tag a walk should enumerate a child's V-Table under: the wire tag while
// it corroborates the slot, otherwise the slot's declared type re-armed with
// the array bit — the surviving witness when the child's tag is the damaged
// half. `usable` is false when BOTH witnesses are gone (a hole, not a
// reference) or the self-offset is beyond the flip budget (the parent's offset
// is the damaged half — this is not the child at all).
//
// WHY THE TAG MUST CORROBORATE, even though a valid self-offset already proves
// a block lives here: `self_ok && !tag_ok` is genuinely two situations wearing
// the same face — the child's TAG was flipped (a real child, mislabelled), or
// the PARENT's offset was flipped onto an innocent, perfectly valid block of
// another type. Nothing local separates them. Guessing "the tag was flipped"
// and enumerating that block under the slot's declared type measurably
// invented 13 references and 15 unrecovered verdicts from a single flipped bit
// (handoff wrong turn 1). The classifier below ranks those two hypotheses
// under the flip budget; the walk does not get to pre-empt it.
inline RECOVERY_TAG corroborated_tag(const BYTE* base, size_t size, const BlockRef& r,
                                     bool& usable) noexcept {
    usable = false;
    if (!FF_BLOCK_IN_BOUNDS(r.child, size))
        return FF_RECOVER_UNDEFINED;
    const RECOVERY_TAG actual = tag_at(base, size, r.child);
    const RECOVERY_TAG actual_base =
        (r.kind == FF_FIELD_ARRAY) ? GetTypeFromTag(actual) : actual;
    const bool self_ok = valid_validation(base, size, r.child);
    // Same budget as the walk: a self-offset that is not merely wrong but
    // UNRELATED to this address means the parent's offset is the damaged half.
    const bool self_repairable =
        self_ok || Recovery::hamming_cost(FF_GET_VALIDATION(base, r.child),
                                          static_cast<uint64_t>(r.child)) <= FF_RECOVERY_MAX_FLIPS;
    const bool tag_ok = r.declared != FF_RECOVER_UNDEFINED && actual_base == r.declared;
    if (!self_repairable || !tag_ok)
        return FF_RECOVER_UNDEFINED;
    usable = true;
    // Same rule as the walk: the wire tag wins only while it corroborates the
    // slot; otherwise it is the damaged half and the parent's declared type is
    // the witness that survived (re-armed with the array bit).
    return tag_ok ? actual
                  : (r.declared != FF_RECOVER_UNDEFINED
                         ? (r.kind == FF_FIELD_ARRAY ? ToArrayTag(r.declared) : r.declared)
                         : actual);
}

// ---- forward declarations: non-inline free helpers, defined below their
// ---- callers in call order (REC-19.1) ----

StreamMapEntry classify_block(const BYTE* base, size_t size, Offset off);
uint32_t walk_array_extent(const BYTE* base, size_t size, Offset array_off,
                           ElementShape shape, uint16_t stride, uint32_t stamped);

}  // namespace

// =====================================================================
// ENTRANCE — recover(), the top of the call stack (REC-19.1)
// =====================================================================

FF_RecoveryReport Recovery::recover() const {
    FF_RecoveryReport rep;

    // The two producers on two threads (REC-19.4/.5): the hierarchical
    // offset-chain walk and the parallel byte census share no state — scan()
    // follows no pointers, the walk scans no bytes — so a damaged stream pays
    // both costs concurrently. An uncaught exception in a std::thread would
    // std::terminate the process, so each worker captures its failure and it
    // is rethrown on the calling thread after join. Specific catch, not
    // `...`: anything that is not a std::exception is a bug worth crashing on.
    StreamMap chain_map;  // hierarchical producer: reachable blocks, sized
    StreamMap scan_map;   // scanned producer: the byte census
    std::exception_ptr chain_error;
    std::exception_ptr census_error;
    std::thread hierarchical([this, &chain_map, &chain_error] {
        try {
            chain_map = reachable_blocks_map();
        } catch (const std::exception&) {
            chain_error = std::current_exception();
        }
    });
    std::thread scanned([this, &scan_map, &census_error] {
        try {
            scan_map = scan();
        } catch (const std::exception&) {
            census_error = std::current_exception();
        }
    });
    hierarchical.join();
    scanned.join();
    if (chain_error)
        std::rethrow_exception(chain_error);
    if (census_error)
        std::rethrow_exception(census_error);

    // ADMIT PARENT-ATTESTED BLOCKS BEFORE ENUMERATING, AND ENUMERATE UNDER
    // THE CORROBORATED TYPE.
    //
    // This enumeration used to be driven by the scan census alone, with each
    // block enumerated under whatever tag was on the wire. Both halves of that
    // lose subtrees to a single bit flip:
    //
    //   * scan() finds a block by its self-offset, so a block whose VALIDATION
    //     was the damaged half is ABSENT from the census. The reference TO it
    //     was still classified and repaired -- which is why the report showed
    //     no failure -- but every reference FROM it was never enumerated.
    //   * a block whose TAG was the damaged half is present, but enumerating it
    //     under that tag names the wrong V-Table (or none), so it yields no
    //     children and the subtree leaves just as quietly.
    //
    // Measured on a 1.05 MB Synthea artifact: one flipped bit anywhere in a
    // block's 10-byte header cost 3 block references while recover() reported
    // zero ambiguous and zero unrecovered. Finding the hole and leaving it is
    // not recovery -- the parent slot still names the child's address AND its
    // type, and that second witness is the whole reason the format stores it.
    //
    // So walk from the root, carrying the corroborated type down: a child is
    // admitted (and enumerated) while EITHER witness still agrees with the
    // slot, and skipped only when both are gone -- the genuine no-witness hole
    // REC-18 reports, where inventing a block would be a guess (P0-3: never
    // guessed). Census blocks the walk never reaches are then enumerated under
    // their own tag, exactly as before: an orphaned parent's outgoing
    // references are still real.
    // REC-20.1 — CROSS-REFERENCE THE TWO PRODUCERS' HOLES BEFORE MERGING.
    //
    // They fail in opposite directions, which is the whole reason for running
    // both: the hierarchy walk cannot see a block nothing points at, and the
    // scan cannot see a block whose VALIDATION is damaged. So a run of bytes
    // that BOTH call unattributed is a hole on two independent grounds, while
    // one only the walk reports may simply be an orphan the scan is holding.
    // Capture the scan's list before the move takes it.
    std::vector<Gap> scan_holes;
    for (const Gap& g : scan_map.gaps)
        if (g.class_ == GapClass::Hole)
            scan_holes.push_back(g);
    const auto corroborated_by_scan = [&scan_holes](Offset pos) {
        for (const Gap& g : scan_holes)
            if (pos >= g.start && pos < g.start + static_cast<Offset>(g.length))
                return true;
        return false;
    };

    StreamMap map = std::move(scan_map);
    std::vector<BlockRef> all;
    std::unordered_set<Offset> enumerated;
    std::size_t admitted = 0;

    // The reachable set comes from the hierarchical producer's map: the walk
    // visited exactly these offsets. (The header entry at 0 rides along; the
    // orphan test below skips Header entries, so it cannot mislead.)
    std::unordered_set<Offset> reached;
    for (const auto& [off, entry] : chain_map)
        reached.insert(off);

    std::vector<std::pair<Offset, RECOVERY_TAG>> frontier;
    if (header_is_readable(m_base, m_size)) {
        const Offset root = FF_HEADER(m_size).get_root(m_base);
        if (root != FF_NULL_OFFSET && root >= 0 &&
            static_cast<size_t>(root) + DATA_BLOCK::HEADER_SIZE <= m_size &&
            plausible_tag(tag_at(m_base, m_size, root)))
            frontier.emplace_back(root, tag_at(m_base, m_size, root));
    }
    while (!frontier.empty()) {
        std::vector<std::pair<Offset, RECOVERY_TAG>> next;
        for (const auto& [off, use] : frontier) {
            if (!enumerated.insert(off).second)
                continue;
            const auto existing = map.find(off);
            if (existing == map.end()) {
                StreamMapEntry e = classify_block(m_base, m_size, off);
                e.offset = off;
                if (e.type == StreamMapEntryType::Undefined)
                    e.type = StreamMapEntryType::Block;
                if (e.size == 0)
                    e.size = derived_block_size(use);
                e.recovery = tag_at(m_base, m_size, off);
                map[off] = e;
                ++admitted;
            } else if (tag_at(m_base, m_size, off) != use) {
                // The block IS in the census, but scan() sized it from the tag
                // on the wire and that tag was the damaged half -- so its
                // extent is wrong and its own bytes still tile as a hole. Now
                // that the parent has supplied the real type, re-derive it.
                const Size sized = derived_block_size(use);
                if (sized != 0 && sized != existing->second.size) {
                    existing->second.size = sized;
                    ++admitted;
                }
            }
            const std::size_t before = all.size();
            enumerate_block_refs(off, use, all);
            for (std::size_t i = before; i < all.size(); ++i) {
                bool usable = false;
                const RECOVERY_TAG child_tag = corroborated_tag(m_base, m_size, all[i], usable);
                if (usable && !enumerated.contains(all[i].child))
                    next.emplace_back(all[i].child, child_tag);
            }
        }
        frontier.swap(next);
    }

    // The tiling was computed over the pre-admission census, so every block the
    // walk just recovered still reads as a hole in it. Re-run the sweep over
    // the repaired map -- which is what find_gaps() is exposed for -- so `holes`
    // counts what is still missing AFTER recovery, not what recovery found.
    if (admitted != 0)
        find_gaps(map);

    // ORPHAN SET LAST, NOT FIRST — the ordering is load-bearing.
    //
    // Orphans are self-consistent ∧ ¬reachable (REC-11), bucketed by tag so a
    // corrupt slot's search is one bucket lookup, not a per-reference arena
    // sweep. The array bit is STRIPPED: an array block must land in the bucket
    // of its element type, which is what the slot declares (an entry-array slot
    // declaring BUNDLE_ENTRY must find its orphaned array, not just loose entry
    // blocks).
    //
    // This used to be built straight off the raw census, BEFORE the admission
    // pass above. That is the wrong order for the repair that consumes it: the
    // repoint hypothesis (H_off, below) looks for a unique unclaimed orphan of
    // the declared type, and a block whose VALIDATION was the damaged half is
    // invisible to scan() -- so it was missing from the very pool meant to
    // receive a re-pointed parent. A corrupt parent offset AND a corrupt child
    // self-offset therefore failed to reconcile even though each had a witness
    // the other side could supply. Closing the self-offset holes first makes
    // the census complete, and only then is the pool the ranker searches the
    // real one.
    std::unordered_map<RECOVERY_TAG, std::vector<Offset>> orphans;
    for (const auto& [off, entry] : map) {
        if (entry.type == StreamMapEntryType::Header)
            continue;
        if (reached.contains(off))
            continue;
        const RECOVERY_TAG t = GetTypeFromTag(tag_at(m_base, m_size, off));
        if (plausible_tag(t))
            orphans[t].push_back(off);
    }

    // REC-20.2 — THE RANKED CANDIDATE LIST, BUILT ONCE.
    //
    // A hole is not an absence, it is an under-determined block. It opens when
    // a block loses BOTH witnesses -- its own VALIDATION word and the parent
    // slot that named it -- so neither survivor is clean, but neither is gone.
    // What is left is two independently damaged copies of the same ten bytes:
    // the parent still holds {offset|recovery}, and the block still holds
    // {self-offset|recovery} where it sits. That is what makes this an
    // assignment problem rather than a search.
    //
    // This pass supplies the y side and runs EXACTLY ONCE: every position
    // inside a hole whose residual word is a near-neighbour of its own address.
    // A block encodes its own offset, so a damaged VALIDATION word still reads
    // close to where it sits, and the positions whose word most nearly encodes
    // their own offset rank top -- that is the least likely thing to happen by
    // accident.
    struct HoleCandidate {
        Offset   pos;
        uint32_t self_cost;     ///< hamming(word, pos) — the ranking key
        uint64_t word;          ///< the location's offset encoding — the block's
                                ///< own (possibly damaged) record of the true offset
        uint16_t tag;           ///< the 2 residual recovery bytes
        bool     corroborated;  ///< both producers called this run a hole
    };
    std::vector<HoleCandidate> hole_candidates;
    for (const Gap& g : map.gaps) {
        if (g.class_ != GapClass::Hole || g.length < DATA_BLOCK::HEADER_SIZE)
            continue;
        const uint64_t last = static_cast<uint64_t>(g.start) + g.length - DATA_BLOCK::HEADER_SIZE;
        for (uint64_t pos = static_cast<uint64_t>(g.start); pos <= last; ++pos) {
            if (pos + DATA_BLOCK::HEADER_SIZE > m_size)
                break;
            // A TIGHT band, not the full flip budget. In random bytes even 8
            // bits would be astronomically unlikely, but hole bytes are not
            // random: the arena is full of 8-byte OFFSET words, and an offset
            // naturally shares most of its high bits with its own position, so
            // a loose threshold harvests structure rather than signal. Measured
            // on a 512-flip artifact, the self-cost histogram over 12,227 hole
            // bytes was 1:35  2:14  3:7  4:15  5:14  6:15  7:10  8:10 -- a
            // sharp spike at 1-2 (the real lost blocks; a hole needs ~1 flip on
            // its VALIDATION) on a flat coincidence floor from 3 up. Admitting
            // that floor cost real repairs: it tied against correct orphan
            // repoints and turned 11 clean verdicts Ambiguous.
            constexpr uint32_t kHoleSignatureFlips = 2;
            const uint64_t word = FF_GET_VALIDATION(m_base, static_cast<Offset>(pos));
            const uint32_t self_cost = hamming_cost(word, pos);
            if (self_cost > kHoleSignatureFlips)
                continue;  // not a near-validation signature — just bytes
            hole_candidates.push_back(
                {static_cast<Offset>(pos), self_cost, word,
                 static_cast<uint16_t>(tag_at(m_base, m_size, static_cast<Offset>(pos))),
                 corroborated_by_scan(static_cast<Offset>(pos))});
        }
    }
    // Ranked: strongest self-similarity first, and a position both producers
    // agree on ahead of one only the walk reports (REC-20.1). The order is what
    // makes the driver below cheap -- it can stop caring about the tail.
    std::sort(hole_candidates.begin(), hole_candidates.end(),
              [](const HoleCandidate& a, const HoleCandidate& b) {
                  if (a.self_cost != b.self_cost) return a.self_cost < b.self_cost;
                  if (a.corroborated != b.corroborated) return a.corroborated;
                  return a.pos < b.pos;
              });

    // EXPANSION: a block whose TAG was the damaged half still owns its subtree.
    //
    // The walk above descends only when the child's tag CORROBORATES the slot,
    // because `self-offset valid && tag disagrees` is two situations wearing
    // one face: the child's tag was flipped, or the parent's offset was flipped
    // onto an innocent valid block of another type. Guessing there invented 13
    // references and 15 unrecovered verdicts from one flipped bit.
    //
    // But the choice is not the walk's to guess -- it is exactly what the
    // ranker below decides, under the flip budget, with ties left Ambiguous.
    // So ask it FIRST, and enumerate the child under the slot's declared type
    // only where the in-place tag repair wins outright. That is deference to
    // the ranker, not a second opinion: same inputs, same rule, and where it
    // declines (a cheaper unique repoint exists, or a tie) nothing is walked.
    //
    // Without this, a single flipped tag byte silently cost the ~3 references
    // hanging below that block, and 1-bit corruption could not be lossless.
    const auto tag_flip_wins = [&](const BlockRef& r) {
        if (r.declared == FF_RECOVER_UNDEFINED || r.child == FF_NULL_OFFSET)
            return false;
        if (!valid_validation(m_base, m_size, r.child))
            return false;  // handled by the admission pass, not here
        const RECOVERY_TAG actual = tag_at(m_base, m_size, r.child);
        const RECOVERY_TAG actual_base =
            (r.kind == FF_FIELD_ARRAY) ? GetTypeFromTag(actual) : actual;
        if (actual_base == r.declared)
            return false;  // nothing to repair
        const uint32_t h_inplace = hamming_cost(actual_base, r.declared);
        if (h_inplace > FF_RECOVERY_MAX_FLIPS)
            return false;
        // Cheapest unique repoint, ranked exactly as the classifier ranks it.
        uint32_t h_off = UINT32_MAX;
        bool off_unique = false;
        const auto consider = [&](Offset cand) {
            const uint32_t c =
                hamming_cost(static_cast<uint64_t>(r.child), static_cast<uint64_t>(cand));
            if (c < h_off) { h_off = c; off_unique = true; }
            else if (c == h_off) { off_unique = false; }
        };
        const auto it = orphans.find(r.declared);
        if (it != orphans.end())
            for (const Offset cand : it->second)
                consider(cand);
        const Size want = derived_block_size(r.declared);
        for (const Gap& g : map.gaps)
            if (g.class_ == GapClass::Hole && g.length == want)
                consider(g.start);
        const bool in_off = off_unique && h_off <= FF_RECOVERY_MAX_FLIPS;
        return !in_off || h_inplace < h_off;  // strict: a tie stays Ambiguous
    };

    // THE EXPANSION VERIFIES ITSELF BEFORE IT IS KEPT.
    //
    // The ranker's preference is evidence, not proof. When it is wrong -- the
    // parent's OFFSET was the damaged half and the child is an innocent block
    // of another type -- enumerating that child under the slot's declared type
    // reads a V-Table the block does not have, and every offset it lifts out is
    // nonsense. Trusting the preference alone invented 13 references from one
    // flipped bit, which is a worse failure than the loss it was avoiding: a
    // missing reference is reported, a fabricated one is believed.
    //
    // But a wrong V-Table is CHEAP TO DETECT. Read under the correct type, a
    // block's slots yield children that are themselves well-formed; read under
    // the wrong one, they yield offsets with no witness at all. So enumerate
    // into scratch, require every child it produces to still corroborate
    // something, and discard the whole expansion otherwise. The block then
    // simply keeps its reported TagRepaired verdict and its subtree waits for
    // the reapply loop (REC-19.7) -- the honest outcome.
    std::vector<BlockRef> probe;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const BlockRef r = all[i];  // by value: `all` grows inside this loop
        if (!tag_flip_wins(r) || enumerated.contains(r.child))
            continue;
        probe.clear();
        enumerate_block_refs(r.child,
                             r.kind == FF_FIELD_ARRAY ? ToArrayTag(r.declared) : r.declared,
                             probe);
        if (!batch_passes(m_base, m_size, probe, BatchTest::EveryChildCorroborates))
            continue;
        enumerated.insert(r.child);
        all.insert(all.end(), probe.begin(), probe.end());
    }

    // ONLY NOW the census sweep: whatever neither the walk nor the expansion
    // reached is enumerated under its own tag — unchanged behaviour for
    // orphans. This runs LAST on purpose. It used to run before the expansion,
    // which meant a tag-damaged block had already been enumerated under the
    // damaged tag (yielding nothing) and was marked done, so the expansion
    // skipped the very block it existed to rescue.
    for (const auto& [off, entry] : map) {
        if (entry.type == StreamMapEntryType::Header)
            continue;
        if (!enumerated.insert(off).second)
            continue;
        enumerate_block_refs(off, tag_at(m_base, m_size, off), all);
    }

    // THE CLASSIFIER — one reference, one verdict (REC-19.6 two-word rank).
    //
    // `classify_one` is shared by the main sweep and the reapply loop below so
    // a repaired block's subtree is judged by exactly the same rule as the
    // first pass. The ranker costs BOTH wire words when both are damaged (the
    // self-offset word AND the recovery word); each must stay within the flip
    // budget (D1/D2 discipline is per flip site) and the SUM orders the
    // hypotheses, with ties left Ambiguous — never guessed (P0-3).
    rep.blocks.reserve(all.size());
    const auto classify_one = [&](const BlockRef& r, BlockVerdict& v) {
        v.block = r;

        if (r.declared == FF_RECOVER_UNDEFINED) {
            // Choice/resource slot with BOTH halves corrupt — undecidable
            // without a type to match. Never guessed (P0-3).
            v.class_ = (valid_validation(m_base, m_size, r.child) &&
                        plausible_tag(tag_at(m_base, m_size, r.child)))
                           ? RepairClass::Ambiguous
                           : RepairClass::Unrecovered;
            return;
        }
        // The block at the parent-named address is self-consistent AND its tag
        // corroborates the slot — nothing to repair. (The VALIDATION guard is
        // load-bearing: a broken VALIDATION with an intact tag must NOT read
        // as Intact.)
        const RECOVERY_TAG actual = tag_at(m_base, m_size, r.child);
        // Array children carry the array bit; the slot declares the element
        // type — compare the stripped forms.
        const RECOVERY_TAG actual_base =
            (r.kind == FF_FIELD_ARRAY) ? GetTypeFromTag(actual) : actual;
        if (valid_validation(m_base, m_size, r.child) && actual_base == r.declared) {
            v.class_ = RepairClass::Intact;
            // Array extent: the stamped ENTRY_COUNT has no second witness
            // (F4), but the element walk derives it — a disagreement means the
            // count is the damaged half.
            if (r.kind == FF_FIELD_ARRAY &&
                static_cast<size_t>(r.child) + FF_ARRAY::HEADER_SIZE <= m_size) {
                const FF_ARRAY array(r.child, m_size, 0);
                const uint32_t stamped = array.entry_count(m_base);
                const RECOVERY_TAG element_tag = GetTypeFromTag(actual);
                const ElementShape shape = element_shape_of(element_tag, array.entry_kind(m_base));

                const uint16_t step = array.entry_step(m_base);

                if (shape == ElementShape::InlineBlock) {
                    // The elements ARE blocks, sitting inside the entry region
                    // and carrying their own self-offset witness. Walking them
                    // in place derives the count directly, and it is the only
                    // shape that can be walked.
                    const uint32_t walked =
                        walk_array_extent(m_base, m_size, r.child, shape, step, stamped);
                    if (walked != stamped) {
                        v.class_ = RepairClass::ExtentDerived;
                        v.bit_cost = hamming_cost(walked, stamped);
                        v.derived_extent = walked;
                    }
                    return;
                }

                // IT MUST FIT IN THE HOLE, NOT MERELY IN THE ARENA.
                //
                // Every other shape stores offsets, 10-byte tuples, or raw
                // scalars -- nothing inside the entry region is a block start.
                // So the first block the census knows about after this header
                // is a NEIGHBOUR, and the array may not reach it. That run of
                // bytes is the hole this array has to fit inside; the size of
                // the file has nothing to do with it.
                //
                // Measured: one flipped bit made an ABSENT `Observation.
                // triggeredBy` read as a present OFFSET array of 29,303
                // entries. Honouring that count needs 234,440 bytes -- which
                // fits a 1 MB arena comfortably and overruns its neighbours
                // immediately. The recovery report said zero invented
                // REFERENCES while the document gained 29,303 fabricated leaf
                // values, because reference integrity is not content integrity.
                //
                // For raw scalars (None) this is the ONLY bound there is:
                // headerless elements leave nothing to walk, so before this
                // their stamped count was accepted unconditionally.
                const uint64_t entries_at =
                    static_cast<uint64_t>(r.child) + FF_ARRAY::HEADER_SIZE;
                const auto next = map.upper_bound(r.child);
                const uint64_t boundary = (next != map.end())
                                              ? static_cast<uint64_t>(next->first)
                                              : static_cast<uint64_t>(m_size);
                const uint64_t room =
                    (step != 0 && boundary > entries_at) ? (boundary - entries_at) / step : 0;

                if (static_cast<uint64_t>(stamped) > room) {
                    v.class_ = RepairClass::ExtentDerived;
                    v.bit_cost = hamming_cost(room, static_cast<uint64_t>(stamped));
                    v.derived_extent = static_cast<uint32_t>(room);
                }
            }
            return;
        }
        // Damaged reference: rank the repair hypotheses under the flip budget
        // (D1/D2). H_inplace repairs the child in place — a tag mismatch
        // (child validates) costs the recovery-word distance; a broken
        // VALIDATION costs the self-offset distance, only when the target is
        // otherwise coherent as the declared type, PLUS the recovery-word
        // distance when an expectation exists (REC-19.6 — for typed-offset
        // kinds coherence forces that second term to zero, so this is
        // byte-identical to the old single-word rank; choice/resource tuples
        // now pay it because their expected type lives in the stored tag half,
        // F1). H_off repoints to a unique unclaimed orphan of the declared
        // type — orphan candidates are bucketed BY the declared type, so their
        // recovery-word distance is 0 by construction. Cheapest under budget
        // wins; equal costs are ambiguous — never guessed. The repoint
        // hypothesis was previously never computed on the child-validates
        // path, so a 1-bit offset flip onto an innocent valid block was
        // misrepaired as a tag rewrite.
        uint32_t h_inplace = UINT32_MAX;
        if (valid_validation(m_base, m_size, r.child)) {
            h_inplace = hamming_cost(actual_base, r.declared);
        } else {
            const bool addressable = r.child >= 0 &&
                                     static_cast<size_t>(r.child) + DATA_BLOCK::HEADER_SIZE <= m_size;
            const RECOVERY_TAG target_tag =
                addressable ? tag_at(m_base, m_size, r.child) : FF_RECOVER_UNDEFINED;
            const RECOVERY_TAG target_base =
                (r.kind == FF_FIELD_ARRAY) ? GetTypeFromTag(target_tag) : target_tag;
            const bool coherent = IsTupleKind(r.kind) ? plausible_tag(target_tag)
                                                      : target_base == r.declared;
            if (addressable && coherent) {
                const uint32_t val_cost = hamming_cost(
                    FF_GET_VALIDATION(m_base, r.child), static_cast<uint64_t>(r.child));
                // No expectation (both tuple halves corrupt) carries no
                // recovery-word cost — there is nothing to match it against.
                const uint32_t tag_cost =
                    (r.declared != FF_RECOVER_UNDEFINED) ? hamming_cost(target_base, r.declared)
                                                         : 0;
                if (val_cost <= FF_RECOVERY_MAX_FLIPS && tag_cost <= FF_RECOVERY_MAX_FLIPS)
                    h_inplace = val_cost + tag_cost;
            }
        }

        // The repoint cost: cheapest unique candidate (mode 1), if any.
        uint32_t h_off = UINT32_MAX;
        bool off_unique = false;
        // `extra` is the candidate's OWN residual cost: 0 for an orphan, whose
        // self-offset validates and whose tag is exact by bucket construction,
        // and self+tag distance for a hole candidate, which validates neither.
        // Adding it keeps a single comparable metric across both kinds, so a
        // clean orphan always outranks a hole that needs the same offset
        // correction — which is the right precedence.
        const auto consider_cost = [&](Offset p, uint32_t c) {
            if (c < h_off) {
                h_off = c;
                off_unique = true;
                v.candidates.clear();
                v.candidates.push_back(p);
            } else if (c == h_off) {
                off_unique = false;
                v.candidates.push_back(p);
            }
        };
        // An orphan is scored on the offset distance alone: its self-offset
        // validates and its tag is exact by bucket construction, so those two
        // distances are zero and adding them would be theatre.
        const auto consider = [&](Offset p) {
            consider_cost(p, hamming_cost(static_cast<uint64_t>(r.child),
                                          static_cast<uint64_t>(p)));
        };
        if (r.declared != FF_RECOVER_UNDEFINED) {
            const auto it = orphans.find(r.declared);
            if (it != orphans.end())
                for (const Offset p : it->second)
                    consider(p);

            // REC-20.4 — THE HOLE MATCH, INSIDE THE CLASSIFIER, ON ONE METRIC.
            //
            // This loop IS the ref-driven driver: classify_one is called once
            // per reference, and the ranked candidate list it walks was built
            // once, outside. Refs are few and hole bytes are many, so this is
            // O(refs x candidates), never O(bytes x refs).
            //
            // Each hole candidate is dual-indexed: `pos` is the location — the
            // noise-free expected address — and `word` is the offset encoding
            // actually stored there, the block's own (possibly damaged) record
            // of the same true offset the parent's slot also holds; self_cost
            // = hamming(word, pos) is the distance between the two indexes.
            // The corrupted parent value is hammed against the LOCATION
            // (item 14: comparing one noisy observation to a known value beats
            // comparing two noisy observations — tuple-against-tuple measured
            // strictly worse, 15,944 refs vs 15,946), while the location's own
            // damage is carried as separate evidence and the tag term stays a
            // direct comparison (neither side has a noise-free version).
            // `word` rides along so a corroboration pass can still ask how
            // well the two damaged copies of the one value agree.
            //
            // It must stay HERE, competing with the orphan repoint on one
            // metric. Moved to a pass after classification it saw only the refs
            // nothing else could fix -- 4 of them against 44 holes -- and
            // closed one hole where competing inline closes 27.
            for (const HoleCandidate& hc : hole_candidates) {
                const RECOVERY_TAG cand_tag = static_cast<RECOVERY_TAG>(hc.tag);
                const RECOVERY_TAG cand_base =
                    (r.kind == FF_FIELD_ARRAY) ? GetTypeFromTag(cand_tag) : cand_tag;
                const uint32_t cost =
                    hamming_cost(static_cast<uint64_t>(r.child),
                                 static_cast<uint64_t>(hc.pos)) +
                    hc.self_cost + hamming_cost(cand_base, r.declared);
                if (cost > FF_RECOVERY_MAX_FLIPS)
                    continue;  // the residue does not describe this reference
                consider_cost(hc.pos, cost);
            }
        }

        const bool in_place = h_inplace <= FF_RECOVERY_MAX_FLIPS;
        const bool in_off = off_unique && h_off <= FF_RECOVERY_MAX_FLIPS;
        if (in_place && in_off && h_inplace == h_off) {
            v.class_ = RepairClass::Ambiguous;  // tie — never guess
            v.bit_cost = h_inplace;
        } else if (in_place && (!in_off || h_inplace < h_off)) {
            v.class_ = valid_validation(m_base, m_size, r.child) ? RepairClass::TagRepaired
                                                                 : RepairClass::PositionRepaired;
            v.bit_cost = h_inplace;
        } else if (in_off) {
            v.class_ = RepairClass::Corroborated;
            v.bit_cost = h_off;
        } else {
            v.class_ = RepairClass::Unrecovered;
            v.bit_cost = (h_off != UINT32_MAX && h_off < h_inplace) ? h_off : h_inplace;
        }
    };
    const auto count = [&rep](const BlockVerdict& v) {
        switch (v.class_) {
            case RepairClass::Intact:            ++rep.intact; break;
            case RepairClass::Corroborated:      ++rep.corroborated; break;
            case RepairClass::TagRepaired:       ++rep.tag_repaired; break;
            case RepairClass::PositionRepaired:  ++rep.position_repaired; break;
            case RepairClass::ExtentDerived:     ++rep.extent_derived; break;
            case RepairClass::Ambiguous:         ++rep.ambiguous; break;
            case RepairClass::Unrecovered:       ++rep.unrecovered; break;
        }
    };

    for (const BlockRef& r : all) {
        BlockVerdict v;
        classify_one(r, v);
        count(v);
        rep.blocks.push_back(std::move(v));
    }

    // REC-19.7 — REAPPLY: repair → follow the fixed block deeper.
    //
    // A repair restores the child's readability, but the subtree BELOW it was
    // never enumerated under the corrected type: a tag-damaged block's
    // V-Table was wrong (or absent), a position-repaired block's address was
    // never the one the parent named. handoff §3.3: the report was incomplete
    // until apply() wrote the repair back and the stream was re-scanned. This
    // loop closes that gap on the report side — every repaired block is
    // re-enumerated under the CORRECTED type, each ref judged by
    // recover_follow_ref_chain: coherent ones descend, damaged ones are
    // classified right here by the same rule as the first pass.
    //
    // This is also wrong-turn-2's self-verification made continuous: a wrong
    // repair (the ranker preferred a repoint that is not the truth) yields
    // children with no surviving witness, and they classify Unrecovered
    // instead of being believed. The verdicts a repair produces are never
    // trusted blindly — only the ones whose children corroborate something
    // keep the walk alive. `enumerated` bounds total work (each block is
    // enumerated at most once), so cycles and re-walks are impossible; the
    // depth bound matches the walk's FF_RECOVERY_MAX_DEPTH discipline.
    struct ReapplyStep { Offset off; RECOVERY_TAG tag; std::size_t depth; };
    std::vector<ReapplyStep> repaired;
    for (const BlockVerdict& v : rep.blocks) {
        if (v.class_ != RepairClass::Corroborated &&
            v.class_ != RepairClass::TagRepaired &&
            v.class_ != RepairClass::PositionRepaired)
            continue;
        const RECOVERY_TAG corrected =
            v.block.kind == FF_FIELD_ARRAY ? ToArrayTag(v.block.declared) : v.block.declared;
        const Offset target = repaired_target(v);
        if (corrected != FF_RECOVER_UNDEFINED && target != FF_NULL_OFFSET)
            repaired.push_back({target, corrected, 0});
    }
    while (!repaired.empty()) {
        std::vector<ReapplyStep> next;
        for (const ReapplyStep& step : repaired) {
            if (!enumerated.insert(step.off).second)
                continue;  // already enumerated (walk/expansion/census) — no dupes
            if (step.depth >= FF_RECOVERY_MAX_DEPTH)
                continue;
            // Admit it to the census. A block reconstructed out of a hole is
            // not recovered until the map says it is there: the tiling is what
            // reports holes, so leaving the map untouched means the hole this
            // repair just filled is still counted as damage.
            if (!map.contains(step.off)) {
                StreamMapEntry e = classify_block(m_base, m_size, step.off);
                e.offset = step.off;
                if (e.type == StreamMapEntryType::Undefined)
                    e.type = StreamMapEntryType::Block;
                if (e.size == 0)
                    e.size = derived_block_size(step.tag);
                e.recovery = step.tag;
                map[step.off] = e;
                ++admitted;
            }
            std::vector<BlockRef> scratch;
            enumerate_block_refs(step.off, step.tag, scratch);
            // THE ADDRESSABLE TEST, NOT THE COHERENT ONE — and the difference
            // is the whole generational cascade.
            //
            // This once kept whatever the corrected type produced, so a wrong
            // repair became believed verdicts (handoff wrong-turn 2). The fix
            // was to verify first. But verifying with EveryChildCorroborates,
            // demands that EVERY child corroborate, over-corrects: a block whose
            // one child is itself damaged has no corroborating child by
            // definition, so its batch was discarded and the chain stopped dead
            // at the first repair. Parent broken, child broken, grandchild
            // broken is precisely what this loop exists to unwind, and the
            // guard was refusing it.
            //
            // The type here is not a hypothesis -- it came from a classified
            // repair, corroborated under the flip budget -- so the V-Table is
            // trusted and the only question left is whether the read produced
            // wild pointers. A wrong V-Table lifts offsets out of positions
            // that hold none, and those are overwhelmingly outside the stream.
            // One that lands in bounds yields references that classify as
            // Unrecovered: reported, not believed, and repairing them would
            // demand the same evidence over again.
            if (!batch_passes(m_base, m_size, scratch, BatchTest::NoWildPointers))
                continue;
            for (const BlockRef& r : scratch) {
                if (r.child == FF_NULL_OFFSET)
                    continue;
                // EVERY reference this block owns gets a verdict, damaged or
                // not. It used to record only the damaged ones, so a block
                // reconstructed out of a hole was walked and its healthy
                // children silently went uncounted -- the subtree was recovered
                // and then not reported, which is the same invisible loss this
                // whole pass exists to end. `enumerated` still bounds each
                // block to one enumeration, so nothing is double-counted.
                const bool still_damaged =
                    recover_follow_ref_chain(m_base, m_size, r, &map.failures);
                {
                    BlockVerdict v;
                    classify_one(r, v);
                    count(v);
                    rep.blocks.push_back(std::move(v));
                }
                if (still_damaged) {
                    const BlockVerdict& v = rep.blocks.back();
                    if (v.class_ == RepairClass::Corroborated ||
                        v.class_ == RepairClass::TagRepaired ||
                        v.class_ == RepairClass::PositionRepaired) {
                        const RECOVERY_TAG ct =
                            r.kind == FF_FIELD_ARRAY ? ToArrayTag(r.declared) : r.declared;
                        const Offset deeper = repaired_target(rep.blocks.back());
                        if (ct != FF_RECOVER_UNDEFINED && deeper != FF_NULL_OFFSET &&
                            !enumerated.contains(deeper))
                            next.push_back({deeper, ct, step.depth + 1});
                    }
                } else {
                    // Coherent under the corrected type — walk deeper.
                    bool usable = false;
                    const RECOVERY_TAG ct = corroborated_tag(m_base, m_size, r, usable);
                    if (usable && !enumerated.contains(r.child))
                        next.push_back({r.child, ct, step.depth + 1});
                }
            }
        }
        repaired.swap(next);
    }

    // REC-19.7 — the gap sweep re-runs on the augmented map: a repoint that
    // claimed bytes shrank the holes, and the reapply may have admitted
    // nothing new to the map (repairs are reported, not written — REC-15's
    // apply() mutates), so only re-tile when the map changed.
    if (!map.failures.empty() || admitted != 0)
        find_gaps(map);

    // =================================================================
    // REC-20.5/.6 — PROGRESSIVE BAND EXPANSION, DRIVEN BY THE BROKEN REFS
    // =================================================================
    // The signature band has to start tight and cannot stay there.
    //
    // A candidate is a position whose residual word is within `band` bits of
    // its own address. At 2 bits that is a clean signal across the whole arena
    // -- but it is also BLIND to a block whose VALIDATION took 3 or more flips,
    // and measured on a 512-flip artifact that is 6 of the 8 surviving holes.
    // Widening the band up front is not the answer: over 12,227 hole bytes the
    // 3+ band is a flat coincidence floor, and admitting it turned 11 clean
    // verdicts Ambiguous by tying against correct orphan repoints.
    //
    // What makes widening safe is doing it LAST, against a pool that earlier
    // rounds have already emptied. Eight holes is a different proposition from
    // twelve thousand bytes: the same 4-bit band that is noise across the arena
    // is decisive across a handful of runs nothing else could claim.
    //
    // So: match at the tightest band, follow every repaired block INTO the
    // structure to pick up the references it now exposes, and only when that
    // stalls with references still broken widen by one bit and try the
    // remainder. Capped at FF_RECOVERY_MAX_FLIPS so one budget governs the
    // whole engine.
    const auto build_candidates = [&](uint32_t band) {
        std::vector<HoleCandidate> out;
        for (const Gap& g : map.gaps) {
            if (g.class_ != GapClass::Hole || g.length < DATA_BLOCK::HEADER_SIZE)
                continue;
            const uint64_t last =
                static_cast<uint64_t>(g.start) + g.length - DATA_BLOCK::HEADER_SIZE;
            for (uint64_t pos = static_cast<uint64_t>(g.start); pos <= last; ++pos) {
                if (pos + DATA_BLOCK::HEADER_SIZE > m_size)
                    break;
                const uint64_t word = FF_GET_VALIDATION(m_base, static_cast<Offset>(pos));
                const uint32_t self_cost = hamming_cost(word, pos);
                if (self_cost > band)
                    continue;
                out.push_back(
                    {static_cast<Offset>(pos), self_cost, word,
                     static_cast<uint16_t>(tag_at(m_base, m_size, static_cast<Offset>(pos))),
                     corroborated_by_scan(static_cast<Offset>(pos))});
            }
        }
        std::sort(out.begin(), out.end(),
                  [](const HoleCandidate& a, const HoleCandidate& b) {
                      if (a.self_cost != b.self_cost) return a.self_cost < b.self_cost;
                      if (a.corroborated != b.corroborated) return a.corroborated;
                      return a.pos < b.pos;
                  });
        return out;
    };

    // One reference against the current candidate pool. Same metric as the
    // classifier: the offset term is scored against the candidate's EXACT
    // position (noise-free) with its self_cost carried as separate evidence,
    // because comparing one noisy observation to a known value beats comparing
    // two noisy observations -- measured, tuple-against-tuple lost 2 refs.
    const auto match_ref = [&](const BlockRef& r, const std::vector<HoleCandidate>& cands,
                               uint32_t& best_cost) -> Offset {
        best_cost = UINT32_MAX;
        Offset winner = FF_NULL_OFFSET;
        bool unique = false;
        for (const HoleCandidate& hc : cands) {
            if (map.contains(hc.pos))
                continue;  // already claimed by an earlier round
            const RECOVERY_TAG cand_tag = static_cast<RECOVERY_TAG>(hc.tag);
            const RECOVERY_TAG cand_base =
                (r.kind == FF_FIELD_ARRAY) ? GetTypeFromTag(cand_tag) : cand_tag;
            const uint32_t cost = hamming_cost(static_cast<uint64_t>(r.child),
                                               static_cast<uint64_t>(hc.pos)) +
                                  hc.self_cost + hamming_cost(cand_base, r.declared);
            if (cost < best_cost) { best_cost = cost; winner = hc.pos; unique = true; }
            else if (cost == best_cost) { unique = false; }
        }
        if (!unique || best_cost > FF_RECOVERY_MAX_FLIPS)
            return FF_NULL_OFFSET;  // no match, or a tie -- never guessed (P0-3)
        return winner;
    };

    std::size_t rec20_matched = 0;
    for (uint32_t band = 2; band <= FF_RECOVERY_MAX_FLIPS; ++band) {
        // Any references still looking? If not, no band needs trying.
        bool any_broken = false;
        for (const BlockVerdict& v : rep.blocks)
            if ((v.class_ == RepairClass::Unrecovered || v.class_ == RepairClass::Ambiguous) &&
                v.block.declared != FF_RECOVER_UNDEFINED && v.block.child != FF_NULL_OFFSET) {
                any_broken = true;
                break;
            }
        if (!any_broken)
            break;

        std::vector<HoleCandidate> cands = build_candidates(band);
        if (cands.empty())
            continue;

        bool progress = true;
        while (progress) {
            progress = false;
            // Index-based: repairing a block appends the references it exposes,
            // and those are themselves candidates for the next sweep.
            for (std::size_t vi = 0; vi < rep.blocks.size(); ++vi) {
                BlockVerdict& v = rep.blocks[vi];
                if (v.class_ != RepairClass::Unrecovered && v.class_ != RepairClass::Ambiguous)
                    continue;
                const BlockRef r = v.block;  // by value: rep.blocks may reallocate
                if (r.declared == FF_RECOVER_UNDEFINED || r.child == FF_NULL_OFFSET)
                    continue;

                uint32_t cost = UINT32_MAX;
                const Offset winner = match_ref(r, cands, cost);
                if (winner == FF_NULL_OFFSET)
                    continue;

                // Admit the reconstructed block: the reference chain, the
                // self-offset and the recovery tag now agree again.
                StreamMapEntry e = classify_block(m_base, m_size, winner);
                e.offset = winner;
                if (e.type == StreamMapEntryType::Undefined)
                    e.type = StreamMapEntryType::Block;
                if (e.size == 0)
                    e.size = derived_block_size(r.declared);
                e.recovery = r.declared;
                map[winner] = e;
                ++admitted;

                rep.blocks[vi].candidates.assign(1, winner);
                rep.blocks[vi].bit_cost = cost;
                rep.blocks[vi].class_ = RepairClass::Corroborated;
                ++rec20_matched;
                progress = true;

                // FOLLOW THE REPAIRED BLOCK IMMEDIATELY. THIS IS THE DESIGN.
                //
                // The moment a reference is repaired, the block it names is
                // back in the hierarchy chain — and it is not merely reachable,
                // it is now a PARENT. Its own outgoing references have never
                // been seen by anything: the walk could not reach it, the scan
                // could not identify it, so its slots were never enumerated and
                // any damage in them was never counted, let alone repaired.
                //
                // So assess it right here, through the same router every other
                // producer uses (recover_follow_ref_chain), and let whatever it
                // finds broken join THIS work list. One repair exposes a parent,
                // that parent exposes its children, and a chain of losses
                // unwinds from a single recovered edge. Deferring this to a
                // later pass would mean matching against a hole set that no
                // longer describes the stream.
                //
                // The batch test is deliberately the weak one.
                // EveryChildCorroborates
                // refuses a batch containing any witness-less child, which is
                // correct for "did I read this under the right type" and exactly
                // wrong here: a damaged child is what the search is hunting, and
                // rejecting the batch discards it. NoWildPointers asks
                // only whether anything corroborates, which still catches a
                // wrong V-Table (uniformly nonsense) without throwing away the
                // broken references this loop exists to consume.
                if (enumerated.insert(winner).second) {
                    const RECOVERY_TAG use = r.kind == FF_FIELD_ARRAY
                                                 ? ToArrayTag(r.declared)
                                                 : r.declared;
                    std::vector<BlockRef> exposed;
                    enumerate_block_refs(winner, use, exposed);
                    if (batch_passes(m_base, m_size, exposed, BatchTest::NoWildPointers)) {
                        for (const BlockRef& child : exposed) {
                            if (child.child == FF_NULL_OFFSET)
                                continue;
                            // The single router: judges the reference and
                            // records which witness failed, so a break inside a
                            // just-recovered block is reported the same way as
                            // one the hierarchy walk found itself.
                            recover_follow_ref_chain(m_base, m_size, child, &map.failures);
                            BlockVerdict cv;
                            classify_one(child, cv);
                            count(cv);
                            rep.blocks.push_back(std::move(cv));
                        }
                    }
                }
            }
            if (progress) {
                // Re-tile: a hole that held more than one lost block now shows
                // its remainder, and the next sweep sees it.
                find_gaps(map);
                cands = build_candidates(band);
            }
        }
    }

    // The counters were tallied as verdicts were produced, so re-derive them
    // rather than letting `unrecovered` keep reporting edges this pass repaired.
    if (rec20_matched != 0) {
        rep.intact = rep.corroborated = rep.tag_repaired = rep.position_repaired = 0;
        rep.extent_derived = rep.ambiguous = rep.unrecovered = 0;
        for (const BlockVerdict& v : rep.blocks) {
            switch (v.class_) {
                case RepairClass::Intact:            ++rep.intact; break;
                case RepairClass::Corroborated:      ++rep.corroborated; break;
                case RepairClass::TagRepaired:       ++rep.tag_repaired; break;
                case RepairClass::PositionRepaired:  ++rep.position_repaired; break;
                case RepairClass::ExtentDerived:     ++rep.extent_derived; break;
                case RepairClass::Ambiguous:         ++rep.ambiguous; break;
                case RepairClass::Unrecovered:       ++rep.unrecovered; break;
            }
        }
    }

    rep.blocks_total = rep.blocks.size();

    // REC-19.2 — the merged producer audits ride the report (scan tag audit +
    // hierarchical reference judgment + the reapply's findings).
    rep.failures = std::move(map.failures);
    rep.failures.insert(rep.failures.end(), chain_map.failures.begin(), chain_map.failures.end());

    // REC-18.5 — carry the gaps through, counted by class. A hole is the one
    // finding neither the scan nor the reachability walk can produce.
    // RE-INDEX LAST. Every earlier tiling predates the repairs above, so a
    // hole this pass filled would still be reported as one. Re-running the
    // sweep over the final census is what makes `holes` mean "still missing
    // after recovery" instead of "was missing before it ran" -- and a hole that
    // held more than one lost block correctly shows its remainder.
    if (admitted != 0)
        find_gaps(map);
    rep.gaps = map.gaps;
    for (const Gap& g : rep.gaps) {
        if (g.class_ == GapClass::Hole)             ++rep.holes;
        else if (g.class_ == GapClass::VersionSkew) ++rep.version_skew;
    }
    return rep;
}

// =====================================================================
// PRODUCERS — the two maps, in call order (REC-19.1)
// =====================================================================

StreamMap Recovery::reachable_blocks_map() const {
    // The HIERARCHICAL producer (REC-19.4): the offset-chain walk, sized per
    // visited block. Sizes are the delta over the bare-offset walk — without
    // them the map cannot tile, and offset + entry.size is what maps holes.
    StreamMap map;
    map.file_size = static_cast<Size>(m_size);
    if (header_is_readable(m_base, m_size))
        map[0] = {StreamMapEntryType::Header, 0, FF_HEADER::HEADER_SIZE};
    for (const Offset off : walk_chain(nullptr, &map.failures)) {
        if (map.contains(off))
            continue;
        map[off] = classify_block(m_base, m_size, off);
        map[off].recovery = tag_at(m_base, m_size, off);
    }
    // The hierarchical producer's own coverage gaps: regions the reference
    // chain did not attribute. On a clean stream the walk reaches every block
    // and this tiles exactly like the census; on a damaged one the unreachable
    // regions show here and resolve against the census at recover()'s join.
    find_gaps(map);
    return map;
}

StreamMap Recovery::scan() const {
    // The SCANNED producer (REC-19.5): the byte census. Byte-wise, not
    // block-wise: the arena is not aligned to block boundaries and a block can
    // start anywhere the bump allocator left it. A u64 equal to its own
    // position has a false-positive rate of ~2^-64 per position, so the
    // VALIDATION word alone is a reliable block detector. The arena is split
    // across hardware-concurrency chunks when it is large enough to amortize
    // the spawn; each worker emits its candidates locally (no shared state),
    // merged after join. Chunks overlap by HEADER_SIZE-1 so a block straddling
    // a boundary is seen by the next chunk; duplicates are dropped at the
    // merge. Every entry offers up the wire uint16 recovery tag — a SINGLE
    // witness that may itself be corrupted: it is recorded on the entry and
    // audited below, but never trusted to decide a type (defect 5).
    StreamMap map;
    map.file_size = static_cast<Size>(m_size);
    if (header_is_readable(m_base, m_size))
        map[0] = {StreamMapEntryType::Header, 0, FF_HEADER::HEADER_SIZE};

    const auto audit = [&](Offset off) {
        if (map.contains(off))
            return;  // the header (and any overlap) is already claimed
        map[off] = classify_block(m_base, m_size, off);
        map[off].recovery = FF_GET_RECOVERY_TAG(m_base, off);
        if (!plausible_tag(map[off].recovery))
            map.failures.push_back({ProducerFailureKind::ScanTagInvalid, off,
                                    FF_RECOVER_UNDEFINED, map[off].recovery,
                                    "self-offset is consistent but the recovery tag is not a known type"});
    };

    const auto emit_candidates = [&](size_t begin, size_t end,
                                     std::vector<Offset>& out) {
        for (size_t off = begin; off + DATA_BLOCK::HEADER_SIZE <= end; ++off)
            if (LOAD_U64(m_base + off) == static_cast<uint64_t>(off))
                out.push_back(static_cast<Offset>(off));
    };

    const unsigned hw = std::thread::hardware_concurrency();
    const size_t workers = (m_size >= (1u << 20) && hw > 1) ? std::min<size_t>(hw, 8u) : 1;
    const size_t chunk = (m_size + workers - 1) / workers;
    const size_t overlap = DATA_BLOCK::HEADER_SIZE - 1;
    std::vector<std::vector<Offset>> found(workers);
    if (workers == 1) {
        emit_candidates(0, m_size, found[0]);  // small arena — single pass
    } else {
        std::vector<std::exception_ptr> errors(workers);
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (size_t w = 0; w < workers; ++w) {
            const size_t begin = w * chunk;
            const size_t end =
                std::min(m_size, begin + chunk + (w + 1 < workers ? overlap : 0));
            threads.emplace_back([this, &emit_candidates, begin, end, &found, &errors, w] {
                try {
                    emit_candidates(begin, end, found[w]);
                } catch (const std::exception&) {
                    errors[w] = std::current_exception();
                }
            });
        }
        for (std::thread& t : threads)
            t.join();
        for (const auto& e : errors)
            if (e)
                std::rethrow_exception(e);
    }
    for (const auto& v : found)
        for (const Offset off : v)
            audit(off);
    find_gaps(map);
    return map;
}

// =====================================================================
// The walk — the hierarchical producer's engine, then its enumerators
// =====================================================================

std::vector<Offset> Recovery::walk_chain(std::vector<BlockRef>* out,
                                         std::vector<ProducerFailure>* failures) const {
    std::vector<Offset> reachable;
    if (!header_is_readable(m_base, m_size))
        return reachable;
    const Offset root = FF_HEADER(m_size).get_root(m_base);
    if (!valid_validation(m_base, m_size, root))
        return reachable;
    const RECOVERY_TAG root_tag = tag_at(m_base, m_size, root);
    if (!plausible_tag(root_tag))
        return reachable;

    // DFS through INTACT references only, depth- and cycle-bounded. Marking on
    // completion is unnecessary here (the verdict of a reference does not
    // depend on how it was reached), so a simple visited set bounds the work.
    struct Pending { Offset off; std::size_t depth; RECOVERY_TAG tag; };
    std::unordered_set<Offset> visited;
    std::vector<Pending> stack{{root, 0, root_tag}};
    std::vector<BlockRef> scratch;
    while (!stack.empty()) {
        const auto [off, depth, tag] = stack.back();
        stack.pop_back();
        if (!visited.insert(off).second)
            continue;
        reachable.push_back(off);

        // One enumeration per block feeds both the walk (child discovery) and,
        // when requested, the baseline output — no second pass. The tag comes
        // down the stack rather than off the wire: for a child whose own tag
        // was the damaged half, the parent's declared type is the surviving
        // witness and the only one that names the right V-Table.
        scratch.clear();
        enumerate_block_refs(off, tag, scratch);
        if (out)
            out->insert(out->end(), scratch.begin(), scratch.end());
        if (depth >= FF_RECOVERY_MAX_DEPTH)
            continue;

        for (const BlockRef& r : scratch) {
            if (r.child == FF_NULL_OFFSET)
                continue;
            // REC-19.3 — the hierarchical producer's audit: every reference is
            // judged by the same routine the reapply loop uses, so a damaged
            // ref is recorded exactly here, at the moment the walk sees it.
            // This recording is not a gate — the descent rule below is
            // deliberately unchanged.
            recover_follow_ref_chain(m_base, m_size, r, failures);
            // ONE SURVIVING WITNESS IS ENOUGH TO KEEP WALKING.
            //
            // This used to require the child to be fully intact -- self-offset
            // valid AND tag equal to the slot's declared type -- and abandoned
            // the subtree otherwise. That threw away the whole point of storing
            // the type twice. A single bit flipped in a child's 10-byte header
            // left the PARENT still naming both its address and its type, and
            // the walk stopped there anyway, so every block below it went
            // unreached and unenumerated. Measured on a 1.05 MB Synthea
            // artifact: one flip in a block header cost 3 block references, in
            // a stream where recover() reported zero failures -- the loss never
            // appeared as a failed repair because the references were never
            // enumerated to be repaired.
            //
            // The rule now matches the redundancy the format actually has: the
            // pair (child's self-offset, child's tag) is corroborated by the
            // parent's slot, so descend while EITHER half still agrees, and
            // stop only when both are gone -- which is the genuine no-witness
            // hole REC-18 exists to report and no walk can cross.
            // A BROKEN SELF-OFFSET MUST STILL BE WITHIN THE FLIP BUDGET.
            //
            // "the tag corroborates but the self-offset does not" describes two
            // very different situations, and they must not be treated alike:
            //   * the child is real and its VALIDATION word took the flip --
            //     the stored word is then a Hamming neighbour of the address;
            //   * the PARENT's offset took the flip and now names arbitrary
            //     bytes whose two tag bytes happen to match -- the stored word
            //     there is unrelated to the address, so the distance is large.
            // Descending on the second one enumerates garbage under a real
            // V-Table. Measured: it invented 13 references and 15 unrecovered
            // verdicts on a stream with a single bit flipped. The budget is the
            // same D1/D2 discipline the classifier already applies, so the walk
            // and the verdict cannot disagree about what is repairable.
            const bool self_ok = valid_validation(m_base, m_size, r.child);
            const bool self_repairable =
                self_ok || (r.child >= 0 &&
                            static_cast<size_t>(r.child) + DATA_BLOCK::HEADER_SIZE <= m_size &&
                            hamming_cost(FF_GET_VALIDATION(m_base, r.child),
                                         static_cast<uint64_t>(r.child)) <= FF_RECOVERY_MAX_FLIPS);
            const RECOVERY_TAG actual = tag_at(m_base, m_size, r.child);
            const bool declared_known = r.declared != FF_RECOVER_UNDEFINED;
            const RECOVERY_TAG actual_base =
                (r.kind == FF_FIELD_ARRAY) ? GetTypeFromTag(actual) : actual;
            const bool tag_ok = declared_known && actual_base == r.declared;
            if (!self_repairable && !tag_ok)
                continue;  // both witnesses gone — a hole, not a reference
            if (!self_repairable)
                continue;  // tag matches by coincidence at an unrelated address
            // Enumerate under the corroborated type. When the child's own tag
            // is the damaged half, its V-Table would be wrong (or absent), and
            // enumerating under it silently yields no children at all — the
            // same subtree loss by another route.
            // Which tag names the V-Table to walk this child under.
            //
            // The wire tag wins when it CORROBORATES the slot, because it is
            // the one that carries the array bit (`declared` is an ELEMENT
            // type, and enumerating an array under its element's V-Table reads
            // the wrong shape entirely). But "plausible" is not "correct": a
            // flipped tag frequently lands on another live tag, and trusting it
            // walks the wrong V-Table and silently yields no children. When the
            // slot declares a type and the wire disagrees, the tag is the
            // damaged half by definition -- the parent is the surviving witness
            // -- so follow the parent, restoring the array bit the slot's
            // element type does not carry.
            const RECOVERY_TAG use =
                tag_ok ? actual
                       : (declared_known
                              ? (r.kind == FF_FIELD_ARRAY ? ToArrayTag(r.declared) : r.declared)
                              : actual);
            stack.emplace_back(r.child, depth + 1, use);
        }
    }
    return reachable;
}


// =====================================================================
// An ARRAY is not a datablock, and is not recovered like one.
// =====================================================================
// A datablock is a V-Table of slots; an array is a stride and a count over
// inline entries. The entries carry no witnesses of their own -- that is the
// format's design, not an omission: they are not pointers to distant objects,
// so the array's VALIDATION (where it is) and RECOVERY (what is inside) cover
// all of them at once.
//
// Which means the array is the ONLY way back to its contents, and walking it
// with the V-Table walker finds nothing at all: reflected_fields_view of an
// array tag is empty, so enumerate_block_refs returns immediately. That is
// exactly what happened when a repaired array reference was followed -- the
// address was recovered correctly and every entry stayed lost, because the
// only code that reads entries was a branch inside the PARENT's walk, run
// earlier against the corrupted address.
//
// So it lives here, callable, and both paths use it: the parent's walk when it
// reaches an FF_FIELD_ARRAY slot, and the repair path when the thing repaired
// IS the array. The concentration of witnesses at the array is what makes this
// matter -- lose the pointer and the array's self-offset together and every
// entry goes with them, N references from a two-bit event.
void Recovery::enumerate_array_entries(Offset array_off, RECOVERY_TAG array_tag,
                                       std::vector<BlockRef>& out) const {
    if (array_off == FF_NULL_OFFSET)
        return;
            // Element references. The array header's self-offset may be the
            // damaged half -- the slot above still names this address and
            // this element type -- so require only that it be REPAIRABLE
            // under the flip budget, not already correct. Demanding a clean
            // self-offset here dropped every element of the array from the
            // census even after the walk had recovered the array itself,
            // which on the test fixture cost 2 of 24 references from one
            // flipped bit. Bytes that are not a Hamming neighbour of their
            // own address are still refused: that is not this array.
            if (!valid_validation(m_base, m_size, array_off) &&
                (static_cast<size_t>(array_off) + DATA_BLOCK::HEADER_SIZE > m_size ||
                 hamming_cost(FF_GET_VALIDATION(m_base, array_off),
                              static_cast<uint64_t>(array_off)) > FF_RECOVERY_MAX_FLIPS))
                return;
            if (static_cast<size_t>(array_off) + FF_ARRAY::HEADER_SIZE > m_size)
                return;
            const RECOVERY_TAG element_tag = GetTypeFromTag(array_tag);
            const FF_ARRAY array(array_off, m_size, 0);
            const uint16_t stride = array.entry_step(m_base);
            const uint32_t stamped = array.entry_count(m_base);
            const ElementShape shape = element_shape_of(element_tag, array.entry_kind(m_base));
            if (shape == ElementShape::None)
                return;
    // EMITTING REFERENCES IS NOT DERIVING AN EXTENT, and conflating them is
    // what stopped the generational recovery dead.
    //
    // walk_array_extent answers "how many entries are demonstrably intact" by
    // walking until one fails to validate. That is the right answer for SIZING
    // the array in the census. It is the wrong bound for emitting references,
    // because it stops at the first DAMAGED entry -- and a damaged entry is
    // precisely what recovery is hunting. An array whose first element is the
    // broken one emitted no references at all, so nothing was looking for that
    // element and its hole survived every round.
    //
    // Bound by GEOMETRY instead (REC-21.1): the stamped count when the arena
    // has room for it, and the walked extent when it does not, so a corrupted
    // ENTRY_COUNT cannot conjure thousands of references out of a small array.
    // Each emitted reference is then judged on its own merits -- an intact one
    // classifies Intact, a damaged one becomes work for the matcher.
    const uint64_t entries = static_cast<uint64_t>(array_off) + FF_ARRAY::HEADER_SIZE;
    const uint64_t width = (shape == ElementShape::Tuple)       ? 10
                           : (shape == ElementShape::OffsetPtr) ? 8
                                                                : stride;
    const uint64_t room = (width != 0 && m_size > entries)
                              ? (static_cast<uint64_t>(m_size) - entries) / width
                              : 0;
    const uint32_t walked = walk_array_extent(m_base, m_size, array_off,
                                              shape, stride, stamped);
    const uint32_t extent = (static_cast<uint64_t>(stamped) <= room) ? stamped : walked;
            for (uint32_t i = 0; i < extent; ++i) {
                const uint64_t pos = entries + static_cast<uint64_t>(i) * (shape == ElementShape::Tuple ? 10
                                                                  : shape == ElementShape::OffsetPtr ? 8
                                                                  : stride);
                if (pos + (shape == ElementShape::Tuple ? 10 : 8) > m_size)
                    break;
                if (shape == ElementShape::Tuple) {
                    const Offset child = static_cast<Offset>(LOAD_U64(m_base + pos));
                    if (child == FF_NULL_OFFSET)
                        return;
                    const RECOVERY_TAG stored = FF_GET_RECOVERY_TAG(m_base, static_cast<Offset>(pos));
                    out.push_back(BlockRef{array_off, static_cast<Offset>(pos - static_cast<uint64_t>(array_off)),
                                           FF_FIELD_RESOURCE, child,
                                           plausible_tag(stored) ? stored : FF_RECOVER_UNDEFINED,
                                           FF_RECOVER_UNDEFINED});
                } else {
                    // OFFSET entries point at string blocks; inline blocks
                    // sit at `pos` itself. Both are single-witness-ish
                    // references whose declared type is the array's element tag.
                    const Offset child = (shape == ElementShape::OffsetPtr)
                                             ? static_cast<Offset>(LOAD_U64(m_base + pos))
                                             : static_cast<Offset>(pos);
                    if (child == FF_NULL_OFFSET)
                        return;
                    out.push_back(BlockRef{array_off, static_cast<Offset>(pos - static_cast<uint64_t>(array_off)),
                                           shape == ElementShape::OffsetPtr ? FF_FIELD_STRING : FF_FIELD_BLOCK,
                                           child, element_tag, FF_RECOVER_UNDEFINED});
                }
            }

}

void Recovery::enumerate_block_refs(Offset block_offset, RECOVERY_TAG block_tag,
                                    std::vector<BlockRef>& out) const {
    // Was `> m_size`, which let an offset within a header's width of the end
    // through to code that immediately reads that header.
    if (!FF_BLOCK_IN_BOUNDS(block_offset, m_size))
        return;

    // DISPATCH ON SHAPE. There are three, and they are recovered differently:
    //
    //   ARRAY       stride + count over inline entries. No V-Table at all --
    //               reflected_fields_view of an array tag is empty -- so the
    //               walk below would return nothing and silently lose every
    //               entry. Its own routine.
    //   BYTE ARRAY  a length and an opaque payload (FF_STRING and everything
    //               sharing its layout, opaque JSON included). It has an
    //               extent but no children, so there is nothing to enumerate;
    //               saying so here is the difference between "no references"
    //               and "unknown tag", which is not the same answer.
    //   DATABLOCK   a V-Table of slots. The walk below.
    //
    // Callers ask for "the references of the block at X" and get the right
    // answer for what X actually is. Leaving the array case as a branch inside
    // the datablock walk meant only the parent could reach it, and the repair
    // path -- which needs exactly that walk against a corrected address --
    // could not.
    if (IsArrayTagged(block_tag)) {
        enumerate_array_entries(block_offset, block_tag, out);
        return;
    }
    if (FF_IsStringLayoutTag(block_tag))
        return;  // byte array: an extent, no references

    const auto fields = reflected_fields_view(static_cast<uint16_t>(block_tag));
    if (fields.empty())
        return;  // unknown tag — nothing the compiled table can walk

    for (const FF_FieldInfo& f : fields) {
        const uint64_t slot = static_cast<uint64_t>(block_offset) + f.field_offset;
        switch (f.kind) {
            case FF_FIELD_BLOCK:
            case FF_FIELD_STRING: {
                if (slot + 8 > m_size)
                    continue;
                const Offset child = static_cast<Offset>(LOAD_U64(m_base + slot));
                if (child == FF_NULL_OFFSET)
                    continue;  // legitimately absent — no reference
                out.push_back(BlockRef{block_offset, static_cast<Offset>(f.field_offset),
                                       f.kind, child, f.child_recovery, FF_RECOVER_UNDEFINED});
                break;
            }
            case FF_FIELD_CHOICE:
            case FF_FIELD_RESOURCE: {
                // 10-byte tuple: the tag half at slot+8 mirrors the child's
                // RECOVERY for OFFSET-BEARING variants (F1: the compiled table
                // is a decoy here). Inline-scalar variants (bool/int/double)
                // put the VALUE in the first 8 bytes, packed date/times and
                // codes are inline too — only flagged fallbacks and
                // block/string/resource variants are references (D3).
                if (slot + 10 > m_size)
                    continue;
                const uint64_t raw = LOAD_U64(m_base + slot);
                if (raw == static_cast<uint64_t>(FF_NULL_OFFSET))
                    continue;  // absent (monostate) — not a reference
                const RECOVERY_TAG stored = FF_GET_RECOVERY_TAG(m_base, static_cast<Offset>(slot));
                const FF_FieldKind variant_kind = Recovery_to_Kind(stored);
                if (variant_kind == FF_FIELD_UNKNOWN)
                    continue;  // the tag half names no kind this build knows
                if (variant_kind == FF_FIELD_BOOL || variant_kind == FF_FIELD_INT32 ||
                    variant_kind == FF_FIELD_UINT32 || variant_kind == FF_FIELD_INT64 ||
                    variant_kind == FF_FIELD_UINT64 || variant_kind == FF_FIELD_FLOAT64)
                    continue;  // inline scalar value — no addressable child (D3)
                if (variant_kind == FF_FIELD_DATETIME) {
                    // Packed civil value unless the fallback flag is set; the
                    // flagged form is a SIGNED offset RELATIVE to this block
                    // (CLAUDE.md hard invariant) naming an FF_STRING fallback.
                    if (raw == FF_DATETIME_NULL || !(raw & FF_DATETIME_FALLBACK_FLAG))
                        continue;
                    out.push_back(BlockRef{block_offset, static_cast<Offset>(f.field_offset), f.kind,
                                           FF_ResolveDateTimeOffset(raw, block_offset),
                                           RECOVER_FF_STRING, FF_RECOVER_UNDEFINED});
                    break;
                }
                if (variant_kind == FF_FIELD_CODE) {
                    // Code occupies the low 4 bytes of the 8-byte value area;
                    // MSB clear = packed dictionary ID, MSB set = signed
                    // relative offset to an FF_CODEABLE_CONCEPT fallback block.
                    const uint32_t raw_code = static_cast<uint32_t>(raw);
                    if (!(raw_code & FF_CODEABLE_CONCEPT_FLAG))
                        continue;
                    out.push_back(BlockRef{block_offset, static_cast<Offset>(f.field_offset), f.kind,
                                           FF_ResolveCodeableConceptOffset(raw_code, block_offset),
                                           RECOVER_FF_CODEABLE_CONCEPT, FF_RECOVER_UNDEFINED});
                    break;
                }
                // Offset-bearing variant: STRING / BLOCK / RESOURCE / ARRAY.
                out.push_back(BlockRef{block_offset, static_cast<Offset>(f.field_offset), f.kind,
                                       static_cast<Offset>(raw), stored, FF_RECOVER_UNDEFINED});
                break;
            }
            case FF_FIELD_ARRAY: {
                if (slot + 8 > m_size)
                    continue;
                const Offset array_off = static_cast<Offset>(LOAD_U64(m_base + slot));
                if (array_off == FF_NULL_OFFSET)
                    continue;
                // The parent→array reference itself...
                out.push_back(BlockRef{block_offset, static_cast<Offset>(f.field_offset), f.kind,
                                       array_off, f.child_recovery, FF_RECOVER_UNDEFINED});
                // ...and NOTHING ELSE. The entries belong to the array, not to
                // this parent: the walk descends into the array like any other
                // child and enumerate_block_refs dispatches to the array
                // routine there. Emitting them from both places counted every
                // array entry twice -- measured, it inflated a clean stream
                // from 16,071 references to 21,566.
                //
                // One owner per fact: the parent owns the parent→array edge,
                // the array owns what is inside it.
                break;
            }
            case FF_FIELD_DATETIME: {
                // Packed inline unless the fallback flag is set; only the
                // flagged form is a reference. The offset is SIGNED and
                // RELATIVE to the containing block (CLAUDE.md hard invariant),
                // and the fallback it names is an FF_STRING block (D6).
                if (slot + 8 > m_size)
                    continue;
                const uint64_t raw = LOAD_U64(m_base + slot);
                if (raw == FF_DATETIME_NULL || !(raw & FF_DATETIME_FALLBACK_FLAG))
                    continue;
                out.push_back(BlockRef{block_offset, static_cast<Offset>(f.field_offset), f.kind,
                                       FF_ResolveDateTimeOffset(raw, block_offset),
                                       RECOVER_FF_STRING, FF_RECOVER_UNDEFINED});
                break;
            }
            case FF_FIELD_CODE: {
                // Packed dictionary ID unless the fallback flag is set; the
                // flagged form is a signed relative offset to an
                // FF_CODEABLE_CONCEPT fallback block (D5).
                if (slot + 4 > m_size)
                    continue;
                const uint32_t raw = LOAD_U32(m_base + slot);
                if (raw == FF_CODE_NULL || !(raw & FF_CODEABLE_CONCEPT_FLAG))
                    continue;
                out.push_back(BlockRef{block_offset, static_cast<Offset>(f.field_offset), f.kind,
                                       FF_ResolveCodeableConceptOffset(raw, block_offset),
                                       RECOVER_FF_CODEABLE_CONCEPT, FF_RECOVER_UNDEFINED});
                break;
            }
            default:
                break;  // scalars, codes, URL-directory refs: no addressable child
        }
    }
}

// =====================================================================
// CALLEES — the free helpers, in call order (REC-19.1)
// =====================================================================
namespace {

// Classify one self-consistent block start: which StreamMapEntryType it is and
// how big it is.
//
// REC-18.2 — CONTAINMENT. An entry is charged only the bytes NO OTHER ENTRY
// OWNS. An inline array of self-describing block headers has each element in
// the map already, so charging the array its whole extent double-counts: doing
// that produced 11,374 "overlaps" totalling 54 MB in a 3.3 MB stream. Arrays
// whose elements are their own entries are charged HEADER-ONLY; every other
// array (raw scalars, resource tuples, the string OFFSET table) is charged its
// full extent, because nothing else claims those bytes.
StreamMapEntry classify_block(const BYTE* base, size_t size, Offset off) {
    const RECOVERY_TAG tag = FF_GET_RECOVERY_TAG(base, off);
    if (IsArrayTagged(tag)) {
        // size = 16 + count x stride, when the header fits and the math lands
        // inside the arena; otherwise the header itself is suspect.
        if (static_cast<size_t>(off) + FF_ARRAY::HEADER_SIZE <= size) {
            const FF_ARRAY array(off, size, 0);
            const uint32_t count  = array.entry_count(base);
            const uint64_t stride = array.entry_step(base);
            const uint64_t total  = FF_ARRAY::HEADER_SIZE + static_cast<uint64_t>(count) * stride;
            // Elements that are themselves self-describing blocks appear in the
            // map on their own; charging them twice is the overlap above.
            const ElementShape shape = element_shape_of(GetTypeFromTag(tag), array.entry_kind(base));
            if (shape == ElementShape::InlineBlock)
                return {StreamMapEntryType::Array, off, FF_ARRAY::HEADER_SIZE};
            if (total <= size - static_cast<size_t>(off))
                return {StreamMapEntryType::Array, off, static_cast<Size>(total)};
        }
        return {StreamMapEntryType::Array, off, FF_ARRAY::HEADER_SIZE};
    }
    if (FF_IsStringLayoutTag(tag)) {
        // FF_STRING stamps its LENGTH at +10; size = STRING_DATA + LENGTH.
        if (static_cast<size_t>(off) + FF_STRING::STRING_DATA <= size) {
            const uint32_t len = FF_GET_STRING_LENGTH(base, off);
            if (static_cast<uint64_t>(len) <= size - static_cast<size_t>(off) - FF_STRING::STRING_DATA)
                return {StreamMapEntryType::String, off, static_cast<Size>(FF_STRING::STRING_DATA + len)};
        }
        return {StreamMapEntryType::String, off, 0};
    }
    // REC-18.1 — the three hand-written primitive blocks have no GENERATED
    // reflection, so reflected_fields_view() is empty for them and the derived
    // size would collapse to DATA_BLOCK::HEADER_SIZE. Each carries a compiled
    // HEADER_SIZE instead. Measured on one Synthea bundle: CODEABLE_CONCEPT
    // x6593, URL_DIRECTORY x1, CHECKSUM x1 -- three tags cover every one.
    if (tag == RECOVER_FF_CODEABLE_CONCEPT) {
        if (static_cast<size_t>(off) + FF_CODEABLE_CONCEPT::HEADER_SIZE <= size) {
            const uint8_t len = FF_CODEABLE_CONCEPT(off, size, 0).length(base);
            const uint64_t total = FF_CODEABLE_CONCEPT::HEADER_SIZE + static_cast<uint64_t>(len);
            if (total <= size - static_cast<size_t>(off))
                return {StreamMapEntryType::Block, off, static_cast<Size>(total)};
        }
        return {StreamMapEntryType::Block, off, FF_CODEABLE_CONCEPT::HEADER_SIZE};
    }
    if (tag == RECOVER_FF_URL_DIRECTORY) {
        if (static_cast<size_t>(off) + FF_URL_DIRECTORY::HEADER_SIZE <= size) {
            const uint32_t n = FF_URL_DIRECTORY(off, size, 0).entry_count(base);
            const uint64_t total = FF_URL_DIRECTORY::HEADER_SIZE +
                                   static_cast<uint64_t>(n) * FF_URL_DIRECTORY::URL_ENTRY_SIZE;
            if (total <= size - static_cast<size_t>(off))
                return {StreamMapEntryType::Block, off, static_cast<Size>(total)};
        }
        return {StreamMapEntryType::Block, off, FF_URL_DIRECTORY::HEADER_SIZE};
    }
    if (tag == RECOVER_FF_CHECKSUM) {
        // FF_CHECKSUM is a hand-written primitive with no GENERATED reflection;
        // derived_block_size would collapse it to DATA_BLOCK::HEADER_SIZE and
        // every sealed stream would trail a gap after its checksum block.
        return {StreamMapEntryType::Block, off, FF_CHECKSUM::HEADER_SIZE};
    }
    return {StreamMapEntryType::Block, off, Recovery::derived_block_size(tag)};
}

// The walked extent of one array: how many elements actually validate. Returns
// the stamped count when the shape makes verification impossible.
uint32_t walk_array_extent(const BYTE* base, size_t size, Offset array_off,
                           ElementShape shape, uint16_t stride, uint32_t stamped) {
    if (shape == ElementShape::None)
        return stamped;  // raw scalars have no headers to walk
    const uint64_t entries = static_cast<uint64_t>(array_off) + FF_ARRAY::HEADER_SIZE;
    const uint64_t step    = (shape == ElementShape::Tuple)  ? 10
                           : (shape == ElementShape::OffsetPtr) ? 8
                           : stride;
    if (step < 8)
        return stamped;  // implausible stride — the header is suspect

    for (uint32_t i = 0; i < stamped; ++i) {
        const uint64_t pos = entries + static_cast<uint64_t>(i) * step;
        if (pos + DATA_BLOCK::HEADER_SIZE > size)
            return i;
        if (shape == ElementShape::InlineBlock) {
            // Elements are blocks at fixed positions: walk their VALIDATION words.
            if (LOAD_U64(base + pos) != pos)
                return i;
            if (!Recovery::plausible_tag(FF_GET_RECOVERY_TAG(base, static_cast<Offset>(pos))))
                return i;
        } else {
            // Tuple / OFFSET entries are POINTERS: validate each target.
            const uint64_t target = LOAD_U64(base + pos);
            if (target == FF_NULL_OFFSET)
                continue;  // legal absent element — does not end the extent (D4)
            if (target + DATA_BLOCK::HEADER_SIZE > size)
                return i;
            if (LOAD_U64(base + target) != target)
                return i;
        }
    }
    return stamped;
}

}  // namespace (free helpers)

// REC-18.3/.4/.7 — tile the arena; every run of bytes no entry claims is a gap.
//
// The map is already offset-ordered (std::map), so this is one O(n) sweep with
// no sort. Overlap is impossible by construction once REC-18.2's containment
// rule holds -- an entry is charged only the bytes nothing else owns -- so a
// cursor that only moves forward is sufficient.
//
// Classification, cheapest discriminator first:
//   1. VERSION GATE. A gap can only be benign skew if the stream was written by
//      a NEWER engine than this reader. FF_HEADER::VERSION carries it; Parser
//      has always read that field and never acted on it, and this is the first
//      place that changes.
//   2. SYSTEMATICITY. A version gap trails EVERY instance of a tag at the SAME
//      size; a hole is a one-off. That is self-calibrating -- the reader cannot
//      know the newer layout, but it can observe that 6,593 CodeableConcepts all
//      trail exactly 6 bytes and conclude the delta rather than 6,593 holes.
//   3. SIZE FLOOR. A hole must be at least DATA_BLOCK::HEADER_SIZE; less than
//      that cannot have been a block. Weak alone -- a large version delta can
//      exceed a small block -- so it never decides on its own.
void Recovery::find_gaps(StreamMap& map) const {
    map.gaps.clear();
    if (map.empty())
        return;

    // REC-18.7 — the compact layout is a presence-bitmask rewrite with entirely
    // different geometry. Refuse rather than emit nonsense.
    if (header_is_readable(m_base, m_size)) {
        if (FF_HEADER(m_size).get_stream_layout(m_base) != FF_STREAM_COMPACTION_NONE)
            return;  // compact archive: gap analysis does not apply
    }

    // Pass 1 — collect the raw runs, remembering which tag each one trails.
    struct Raw { Offset start; Size len; RECOVERY_TAG after; Offset after_off; };
    std::vector<Raw> raw;
    uint64_t cursor = 0;
    RECOVERY_TAG prev_tag = FF_RECOVER_UNDEFINED;
    Offset prev_off = FF_NULL_OFFSET;
    for (const auto& [off, e] : map) {
        const uint64_t o = static_cast<uint64_t>(off);
        if (o > cursor)
            raw.push_back({static_cast<Offset>(cursor), static_cast<Size>(o - cursor),
                           prev_tag, prev_off});
        cursor   = std::max(cursor, o + static_cast<uint64_t>(e.size));
        prev_tag = tag_at(m_base, m_size, off);
        prev_off = off;
    }
    if (static_cast<uint64_t>(map.file_size) > cursor)
        raw.push_back({static_cast<Offset>(cursor),
                       static_cast<Size>(map.file_size - cursor), prev_tag, prev_off});

    // Pass 2 — could this reader be under-sizing blocks at all?
    bool newer_stream = false;
    if (header_is_readable(m_base, m_size)) {
        // This engine's version, composed exactly as the writer composes it
        // (src/FF_Primitives.cpp, the STORE_U32 into FF_HEADER::VERSION).
        constexpr uint32_t kThisEngine =
            ((static_cast<uint32_t>(FASTFHIR_VERSION_MAJOR) & 0x3FFFu) << 16) |
             (static_cast<uint32_t>(FASTFHIR_VERSION_MINOR) & 0xFFFFu);
        const uint32_t stream_engine = FF_HEADER(m_size).get_engine_version(m_base);
        newer_stream = stream_engine > FF_HEADER_ENGINE_VERSION(kThisEngine);
    }

    // Pass 3 — per-tag systematicity. A tag whose every instance trails the same
    // non-zero run is a version delta, not N separate holes.
    std::unordered_map<uint16_t, std::size_t> instances;
    for (const auto& [off, e] : map)
        instances[static_cast<uint16_t>(tag_at(m_base, m_size, off))]++;
    std::unordered_map<uint16_t, std::unordered_map<Size, std::size_t>> trail;
    for (const Raw& r : raw)
        if (r.after != FF_RECOVER_UNDEFINED)
            trail[static_cast<uint16_t>(r.after)][r.len]++;

    for (const Raw& r : raw) {
        Gap g{r.start, r.len, r.after, GapClass::Hole, "unattributed bytes"};
        if (static_cast<uint64_t>(r.start) + r.len >= static_cast<uint64_t>(map.file_size)) {
            g.class_ = GapClass::Trailing;
            g.why    = "arena slack past the last entry";
        } else if (newer_stream && r.after != FF_RECOVER_UNDEFINED) {
            const uint16_t t = static_cast<uint16_t>(r.after);
            const auto it = trail.find(t);
            const std::size_t same = (it != trail.end() && it->second.count(r.len))
                                         ? it->second.at(r.len) : 0;
            // Every instance of the tag trails this exact run -> a V-Table the
            // writer knows about and this reader does not.
            if (same > 1 && same == instances[t]) {
                g.class_ = GapClass::VersionSkew;
                g.why    = "constant trailing run after every block of this tag; "
                           "stream engine is newer than this reader";
            }
        }
        if (g.class_ == GapClass::Hole && r.len < DATA_BLOCK::HEADER_SIZE)
            g.why = "unattributed, but too small to have held a block header";
        map.gaps.push_back(g);
    }
}

// =====================================================================
// LEAF HELPERS
// =====================================================================

// The clean-stream baseline path: offset-chain walk only, no census, no
// classification. On bytes the caller vouches for, reachable blocks are all
// blocks — so this yields exactly the reference set recover() would classify
// Intact, at O(blocks) instead of O(bytes). recover() is for damaged streams;
// a baseline must never run the byte scan (TASKS.md REC-10).
std::vector<BlockRef> Recovery::reachable_blocks() const {
    std::vector<BlockRef> out;
    walk_chain(&out);
    return out;
}

// REC-18.1 — a generated block's V-Table extent, derived from the COMPILED
// reflection table. No generator change is needed: ff_slot_width() is constexpr
// and total over FF_FieldKind, so the widest (field_offset + width) is the
// header size. Verified exact against the compiled constants -- FF_OBSERVATION
// 288, FF_PATIENT 191, FF_CODING 55.
//
// This is also where an OLD parser under-sizes a NEWER stream: a future engine
// appends slots, so this reader's table is short and every block of that tag
// trails a small gap. That is benign and classify_gaps() must not call it
// damage (REC-18.4).
Size Recovery::derived_block_size(RECOVERY_TAG tag) noexcept {
    const auto fields = reflected_fields_view(static_cast<uint16_t>(tag));
    Size widest = DATA_BLOCK::HEADER_SIZE;
    for (const FF_FieldInfo& f : fields) {
        const Size end = static_cast<Size>(f.field_offset) + ff_slot_width(f.kind);
        if (end > widest)
            widest = end;
    }
    return widest;
}

uint32_t Recovery::hamming_cost(uint64_t a, uint64_t b) noexcept {
    return static_cast<uint32_t>(std::popcount(a ^ b));
}

bool Recovery::plausible_tag(RECOVERY_TAG tag) noexcept {
    if (tag == FF_RECOVER_UNDEFINED)
        return false;
    // Assigned-ness, not band membership: the bands tile 0x0001-0x7FFF, so a
    // range test reduces to "non-zero" and lets thousands of unassigned values
    // pollute the orphan buckets (pushing real corruption into Ambiguous).
    // Recovery_to_Kind's scalar-band switch has no default — unassigned scalar
    // values map to FF_FIELD_UNKNOWN — while every assigned tag maps to a real
    // kind (review finding D8). Loose enough that a false positive costs a
    // rejected candidate, strict enough that garbage tags stop polluting.
    return Recovery_to_Kind(tag) != FF_FIELD_UNKNOWN;
}

// =====================================================================
// REC-15 — apply(): the only mutating entry point
// =====================================================================
FF_ApplyReport Recovery::apply(const FF_RecoveryReport& report, std::vector<BYTE>& repaired,
                               const ApplyFilter& filter) const {
    FF_ApplyReport out;
    repaired.assign(m_base, m_base + m_size);   // the copy; the arena is untouched
    BYTE* const dst = repaired.data();
    const size_t n = repaired.size();

    const auto in_bounds = [n](uint64_t off, size_t width) {
        return off + width <= n;
    };

    for (const BlockVerdict& v : report.blocks) {
        const bool confident = v.class_ == RepairClass::Corroborated ||
                               v.class_ == RepairClass::TagRepaired ||
                               v.class_ == RepairClass::PositionRepaired ||
                               v.class_ == RepairClass::ExtentDerived;
        if (!(filter ? filter(v) : confident)) {
            ++out.declined;
            continue;
        }
        // Ambiguous and Unrecovered are never applied even when a filter asks
        // for them: the engine reported those because it declined to choose,
        // and writing a guess would turn a declared uncertainty into a silent
        // one. That is the failure mode the whole class exists to avoid.
        if (!confident) {
            ++out.declined;
            continue;
        }

        const BlockRef& r = v.block;
        const RECOVERY_TAG want =
            r.kind == FF_FIELD_ARRAY ? ToArrayTag(r.declared) : r.declared;
        bool wrote = false, ok = false;
        uint64_t slot = 0, before = 0;

        switch (v.class_) {
            case RepairClass::Corroborated: {
                // The parent's stored offset was the damaged half.
                if (v.candidates.empty())
                    break;
                const Offset target = v.candidates.front();
                slot = static_cast<uint64_t>(r.parent) + static_cast<uint64_t>(r.field);
                if (!in_bounds(slot, 8))
                    break;
                before = LOAD_U64(dst + slot);
                STORE_U64(dst + slot, static_cast<uint64_t>(target));
                wrote = true;
                ok = LOAD_U64(dst + slot) == static_cast<uint64_t>(target) &&
                     valid_validation(dst, n, target) &&
                     (r.declared == FF_RECOVER_UNDEFINED || tag_at(dst, n, target) == want);
                break;
            }
            case RepairClass::TagRepaired: {
                // The child's tag was the damaged half; the slot's declared
                // type is the surviving witness.
                //
                // BUT ONLY WHEN THE TAG ON THE WIRE IS ALREADY NONSENSE.
                //
                // TagRepaired is decided on `self_ok && !tag_ok`, and that is
                // two situations wearing one face: the child's tag was flipped,
                // or the PARENT's offset was flipped onto an innocent, perfectly
                // valid block of another type. The ranker picks whichever is
                // cheaper in bits and is sometimes wrong -- which costs nothing
                // while merely READING (the reader mis-types one block), but is
                // destructive when WRITING: relabelling an innocent block
                // erases the only surviving record of what it really was, and
                // its own references stop being enumerable at all.
                //
                // Measured on a 512-flip artifact, applying all 61 tag rewrites
                // bought +10 intact edges and created 59 unrecovered ones and 7
                // new holes -- strictly worse than not repairing. Applying the
                // position and repoint classes alone was clean.
                //
                // A tag that is not a plausible type cannot be an innocent
                // block's real type, so there is nothing to destroy: that is the
                // case this writes. A plausible-but-different tag stays a
                // report, and the caller may still opt in through the filter.
                if (r.declared == FF_RECOVER_UNDEFINED)
                    break;
                if (plausible_tag(tag_at(dst, n, r.child)))
                    break;  // could be an innocent block — report, do not rewrite
                slot = static_cast<uint64_t>(r.child) + DATA_BLOCK::RECOVERY;
                if (!in_bounds(slot, 2))
                    break;
                before = LOAD_U16(dst + slot);
                STORE_U16(dst + slot, static_cast<uint16_t>(want));
                wrote = true;
                ok = tag_at(dst, n, r.child) == want && valid_validation(dst, n, r.child);
                break;
            }
            case RepairClass::PositionRepaired: {
                // The child's VALIDATION was the damaged half. A block's
                // self-offset IS its own address, so the corrected value needs
                // no candidate — it is the address the parent named.
                slot = static_cast<uint64_t>(r.child) + DATA_BLOCK::VALIDATION;
                if (!in_bounds(slot, 8))
                    break;
                before = LOAD_U64(dst + slot);
                STORE_U64(dst + slot, static_cast<uint64_t>(r.child));
                wrote = true;
                ok = valid_validation(dst, n, r.child);
                break;
            }
            case RepairClass::ExtentDerived: {
                // Write the count the CLASSIFIER settled on. This arm used to
                // re-run walk_array_extent, which quietly enacted a different
                // verdict than the one reported: the walk cannot see the
                // distance to the next block, so for an OFFSET array it left a
                // 29,303-entry count in place and the repaired document kept
                // every fabricated leaf. One owner per fact.
                slot = static_cast<uint64_t>(r.child) + FF_ARRAY::ENTRY_COUNT;
                if (!in_bounds(slot, 4))
                    break;
                before = LOAD_U32(dst + slot);
                STORE_U32(dst + slot, v.derived_extent);
                wrote = true;
                ok = LOAD_U32(dst + slot) == v.derived_extent;
                break;
            }
            default:
                break;
        }

        if (!wrote) {
            ++out.declined;
            continue;
        }
        if (ok) {
            ++out.applied;
            continue;
        }
        // REVERT. A write that does not verify is not a repair, and leaving it
        // in place would make the copy worse than the damaged original while
        // reporting success -- the one outcome this must never produce.
        switch (v.class_) {
            case RepairClass::TagRepaired:   STORE_U16(dst + slot, static_cast<uint16_t>(before)); break;
            case RepairClass::ExtentDerived: STORE_U32(dst + slot, static_cast<uint32_t>(before)); break;
            default:                         STORE_U64(dst + slot, before); break;
        }
        ++out.failed;
        out.failed_edges.push_back(r.child);
    }
    return out;
}

Recovery::Recovery(const Memory& memory) noexcept
    : m_base(memory.base()),
      // THE DECLARED SIZE IS A WIRE VALUE, AND THIS CLASS TRUSTS NO WIRE VALUE.
      //
      // Memory::size() reads the arena's write head, and the head lives at
      // byte 8 of the arena (Memory::STREAM_CURSOR_OFFSET) -- the same 8 bytes
      // FF_HEADER::STREAM_SIZE occupies. That identity is the design: sealing a
      // stream parks the head at the payload size, so it becomes the declared
      // file size. It also means that on a DAMAGED stream those 8 bytes are
      // just corrupted bytes, and size() reports whatever they say.
      //
      // Measured: a Synthea artifact with 256 bits flipped reported a size of
      // 8,591,006,582 for a 1,071,990-byte file, and the byte census walked
      // 8 GiB of unmapped sparse address space -- SIGSEGV, in the one class
      // whose entire purpose is surviving bytes it does not trust, and whose
      // header promises every read is bounds-checked. It killed test 5's sweep
      // at 256 bits and predates the REC-19 rewrite.
      //
      // So bound it by an extent the stream cannot influence. disk_size() is
      // what the OS reports for the backing file and is the real answer when
      // there is one; capacity() is only the sparse RESERVATION (4 GiB by
      // default), so it is the weaker fallback used for an anonymous arena.
      // Neither is read from the stream, which is the whole point.
      m_size(std::min<uint64_t>(memory.size(),
                                memory.disk_size() != 0 ? memory.disk_size()
                                                        : memory.capacity())) {}

}  // namespace FastFHIR
