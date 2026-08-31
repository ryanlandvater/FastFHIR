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
    if (off < 0 || static_cast<size_t>(off) > size ||
        size - static_cast<size_t>(off) < DATA_BLOCK::HEADER_SIZE)
        return false;
    return FF_GET_VALIDATION(base, off) == static_cast<uint64_t>(off);
}

inline RECOVERY_TAG tag_at(const BYTE* base, size_t size, Offset off) noexcept {
    if (off < 0 || static_cast<size_t>(off) > size ||
        size - static_cast<size_t>(off) < DATA_BLOCK::HEADER_SIZE)
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
inline bool refs_are_coherent(const BYTE* base, size_t size,
                              const std::vector<BlockRef>& refs) noexcept {
    for (const BlockRef& c : refs) {
        if (c.child == FF_NULL_OFFSET)
            continue;
        if (c.child < 0 || static_cast<size_t>(c.child) + DATA_BLOCK::HEADER_SIZE > size)
            return false;
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
    if (r.child == FF_NULL_OFFSET || r.child < 0 ||
        static_cast<size_t>(r.child) + DATA_BLOCK::HEADER_SIZE > size)
        return false;  // absent or out of arena — no reference to judge
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
    if (r.child == FF_NULL_OFFSET || r.child < 0 ||
        static_cast<size_t>(r.child) + DATA_BLOCK::HEADER_SIZE > size)
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

    // HOLE CANDIDATES — a hole is not an absence, it is an under-determined
    // block, and it carries more evidence than its position.
    //
    // A hole opens when a block loses BOTH witnesses: its own VALIDATION word
    // and the parent slot that named it. That is two independent corruptions,
    // so neither survivor is clean -- but neither is gone either, and what is
    // left is heavily constrained. Three quantities must agree, and each is a
    // separate bit-distance:
    //
    //   1. the block encodes its OWN offset, so the damaged word at position p
    //      still reads close to p;
    //   2. the 2 bytes after it are the RECOVERY tag, which still reads close
    //      to what the parent expects (the V-Table's compiled child type for a
    //      typed-offset slot, the stored tag half for a choice/resource tuple);
    //   3. the parent's damaged offset still reads close to p.
    //
    // Matching y holes against x broken references is therefore not a guess
    // over an unstructured space -- it is an over-determined assignment, and
    // in practice x == y. This pass supplies the y side: every position inside
    // a hole whose residual word is a Hamming neighbour of its own address, a
    // "near-validation signature". The classifier below scores it against the
    // x side using all three distances at once.
    //
    // This replaces a filter that admitted a hole only when its LENGTH equalled
    // the declared type's size and then ranked on offset distance alone. Size
    // equality is weak evidence and fails outright when a hole holds more than
    // one lost block; the residual bits are strong evidence and localise the
    // block inside the hole rather than assuming it starts at the first byte.
    struct HoleCandidate {
        Offset       pos;
        uint32_t     self_cost;  // distance from the residual word to `pos`
        RECOVERY_TAG tag;        // residual recovery tag at pos + RECOVERY
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
            // loose thresholds harvest structure rather than signal. Measured
            // on a 512-flip artifact, the self-cost histogram over 12,227 hole
            // bytes was 1:35  2:14  3:7  4:15  5:14  6:15  7:10  8:10 -- a
            // sharp spike at 1-2 (the real lost blocks; a hole needs ~1 flip on
            // its VALIDATION) sitting on a flat coincidence floor from 3 up.
            // Admitting that floor cost real repairs: it produced ties against
            // correct orphan repoints and turned 11 clean verdicts Ambiguous.
            constexpr uint32_t kHoleSignatureFlips = 2;
            const uint32_t self_cost =
                hamming_cost(FF_GET_VALIDATION(m_base, static_cast<Offset>(pos)), pos);
            if (self_cost > kHoleSignatureFlips)
                continue;  // not a near-validation signature — just bytes
            hole_candidates.push_back({static_cast<Offset>(pos), self_cost,
                                       tag_at(m_base, m_size, static_cast<Offset>(pos))});
        }
    }

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
        if (!refs_are_coherent(m_base, m_size, probe))
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
                if (shape != ElementShape::None) {
                    const uint32_t walked = walk_array_extent(
                        m_base, m_size, r.child, shape, array.entry_step(m_base), stamped);
                    if (walked != stamped) {
                        v.class_ = RepairClass::ExtentDerived;
                        v.bit_cost = hamming_cost(walked, stamped);
                    }
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
        const auto consider = [&](Offset p, uint32_t extra = 0) {
            const uint32_t c = hamming_cost(static_cast<uint64_t>(r.child),
                                            static_cast<uint64_t>(p)) + extra;
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
        if (r.declared != FF_RECOVER_UNDEFINED) {
            const auto it = orphans.find(r.declared);
            if (it != orphans.end())
                for (const Offset p : it->second)
                    consider(p);

            // REC-18.6 — a HOLE is a candidate position, and the ONLY one
            // available when the child's VALIDATION is broken too: such a block
            // is in no orphan bucket because scan() never found it.
            //
            // Scored on all three distances at once (see the hole-candidate
            // pass above): how far the parent's stored offset is from the
            // candidate, how far the candidate's residual word is from its own
            // address, and how far its residual tag is from the type this slot
            // expects. Each is independent evidence about the same block, so
            // their sum under one flip budget is a genuine reconstruction
            // rather than a guess — and the budget stays the SAME total, which
            // makes a hole match strictly harder to earn than an orphan
            // repoint, as it should be.
            for (const HoleCandidate& hc : hole_candidates) {
                const RECOVERY_TAG hc_base =
                    (r.kind == FF_FIELD_ARRAY) ? GetTypeFromTag(hc.tag) : hc.tag;
                const uint32_t tag_cost = hamming_cost(hc_base, r.declared);
                if (hc.self_cost + tag_cost > FF_RECOVERY_MAX_FLIPS)
                    continue;  // the residue does not describe this type
                consider(hc.pos, hc.self_cost + tag_cost);
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
            // The comment above claimed this loop was wrong-turn-2's
            // self-verification made continuous. It was not: it classified and
            // KEPT whatever the corrected type produced, so a wrong repair
            // turned into believed verdicts. Verify first, exactly as the
            // expansion does, and drop the batch whole when the V-Table plainly
            // does not belong to this block.
            if (!refs_are_coherent(m_base, m_size, scratch))
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

void Recovery::enumerate_block_refs(Offset block_offset, RECOVERY_TAG block_tag,
                                    std::vector<BlockRef>& out) const {
    if (block_offset < 0 || static_cast<size_t>(block_offset) > m_size)
        return;
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
                // The parent→array reference itself.
                out.push_back(BlockRef{block_offset, static_cast<Offset>(f.field_offset), f.kind,
                                       array_off, f.child_recovery, FF_RECOVER_UNDEFINED});
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
                    continue;
                if (static_cast<size_t>(array_off) + FF_ARRAY::HEADER_SIZE > m_size)
                    continue;
                const RECOVERY_TAG ah_tag = tag_at(m_base, m_size, array_off);
                const RECOVERY_TAG element_tag = GetTypeFromTag(ah_tag);
                const FF_ARRAY array(array_off, m_size, 0);
                const uint16_t stride = array.entry_step(m_base);
                const uint32_t stamped = array.entry_count(m_base);
                const ElementShape shape = element_shape_of(element_tag, array.entry_kind(m_base));
                if (shape == ElementShape::None)
                    continue;
                const uint32_t extent = walk_array_extent(m_base, m_size, array_off,
                                                          shape, stride, stamped);
                const uint64_t entries = static_cast<uint64_t>(array_off) + FF_ARRAY::HEADER_SIZE;
                for (uint32_t i = 0; i < extent; ++i) {
                    const uint64_t pos = entries + static_cast<uint64_t>(i) * (shape == ElementShape::Tuple ? 10
                                                                      : shape == ElementShape::OffsetPtr ? 8
                                                                      : stride);
                    if (pos + (shape == ElementShape::Tuple ? 10 : 8) > m_size)
                        break;
                    if (shape == ElementShape::Tuple) {
                        const Offset child = static_cast<Offset>(LOAD_U64(m_base + pos));
                        if (child == FF_NULL_OFFSET)
                            continue;
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
                            continue;
                        out.push_back(BlockRef{array_off, static_cast<Offset>(pos - static_cast<uint64_t>(array_off)),
                                               shape == ElementShape::OffsetPtr ? FF_FIELD_STRING : FF_FIELD_BLOCK,
                                               child, element_tag, FF_RECOVER_UNDEFINED});
                    }
                }
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

Recovery::Recovery(const Memory& memory) noexcept
    : m_base(memory.base()), m_size(memory.size()) {}

}  // namespace FastFHIR
