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
#include <set>
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

// Was this stream written by an engine newer than this reader? A newer writer
// may append V-Table slots, assign dictionary IDs this build has never seen,
// and emit encodings this build cannot read, all legitimately. So anything
// that is merely UNFAMILIAR is evidence of damage only when this answers
// false. FF_HEADER::VERSION carries the writer's version, composed below
// exactly as the writer composes it (src/FF_Primitives.cpp).
inline bool stream_is_newer(const BYTE* base, size_t size) noexcept {
    if (!header_is_readable(base, size))
        return false;
    constexpr uint32_t kThisEngine =
        ((static_cast<uint32_t>(FASTFHIR_VERSION_MAJOR) & 0x3FFFu) << 16) |
         (static_cast<uint32_t>(FASTFHIR_VERSION_MINOR) & 0xFFFFu);
    return FF_HEADER(size).get_engine_version(base) > FF_HEADER_ENGINE_VERSION(kThisEngine);
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

// A position inside a hole whose residual VALIDATION word is a near-neighbour
// of its own address — the y side of REC-20's assignment problem. Declared here
// rather than inside recover() because plan_repair() below needs to name it:
// the candidate's OWN residual damage is what the repair has to undo.
struct HoleCandidate {
    Offset   pos;
    uint32_t self_cost;     ///< hamming(word, pos) — the ranking key
    uint64_t word;          ///< the location's offset encoding — the block's
                            ///< own (possibly damaged) record of the true offset
    uint16_t tag;           ///< the 2 residual recovery bytes
    bool     corroborated;  ///< both producers called this run a hole
};

// THE BYTES ONE VERDICT MUST WRITE, DECIDED WHERE THE EVIDENCE IS.
//
// apply() used to re-derive the seats from the RepairClass label, and a label
// is not a plan. Deriving one loses everything the classifier knew that the
// label does not spell, and three defects lived in that gap -- each of them
// reporting a confident repair while leaving the damage on the wire:
//
//   * a Corroborated repoint onto a HOLE candidate wrote only the parent's
//     slot and then demanded the target validate. A hole candidate is admitted
//     precisely because its self-offset does NOT validate (self_cost is 1 or 2
//     by construction), so that check could never pass: all 36 hole matches in
//     the two fixtures verified false and reverted;
//   * PositionRepaired on a tuple priced `val_cost + tag_cost` and then wrote
//     only the VALIDATION word, so the tag it had charged for stayed damaged
//     and the verdict verified anyway, because the check asked only whether the
//     self-offset now held;
//   * an inline array element has no pointer word at all -- its child IS its
//     slot -- so a repoint writes an address over the element's own VALIDATION.
//
// So the classifier states the writes and apply() enacts exactly those. It is
// the rule `derived_extent` already followed, generalised: one owner per fact.
inline void plan_repair(BlockVerdict& v, const BYTE* base, size_t size,
                        const HoleCandidate* hole) {
    v.writes.clear();
    const BlockRef& r = v.block;
    if (r.child == FF_NULL_OFFSET)
        return;
    const RECOVERY_TAG want = (r.declared == FF_RECOVER_UNDEFINED) ? FF_RECOVER_UNDEFINED
                              : (r.kind == FF_FIELD_ARRAY)        ? ToArrayTag(r.declared)
                                                                  : r.declared;
    const auto plan = [&v](uint64_t seat, uint8_t width, uint64_t value) {
        v.writes.push_back({static_cast<Offset>(seat), width, value});
    };
    switch (v.class_) {
        case RepairClass::Corroborated: {
            if (v.candidates.empty())
                return;
            const Offset target = v.candidates.front();
            plan(static_cast<uint64_t>(r.parent) + r.field, 8, static_cast<uint64_t>(target));
            // THE TARGET'S OWN WITNESSES, when the winner was a hole candidate.
            // The parent's surviving copies determine both of them, so neither
            // is a guess: the address is the candidate's position, and the type
            // is the slot's declared expectation.
            if (hole != nullptr) {
                if (hole->self_cost != 0)
                    plan(static_cast<uint64_t>(target) + DATA_BLOCK::VALIDATION, 8,
                         static_cast<uint64_t>(target));
                if (want != FF_RECOVER_UNDEFINED && static_cast<RECOVERY_TAG>(hole->tag) != want)
                    plan(static_cast<uint64_t>(target) + DATA_BLOCK::RECOVERY, 2,
                         static_cast<uint64_t>(want));
            }
            break;
        }
        case RepairClass::TagRepaired: {
            const bool adjudicated =
                v.damaged_copy != TagCopy::Undecided && v.consensus_tag != FF_RECOVER_UNDEFINED;
            const uint64_t seat = (adjudicated && v.damaged_copy == TagCopy::ParentSlot)
                                      ? static_cast<uint64_t>(r.parent) + r.field
                                      : static_cast<uint64_t>(r.child);
            const RECOVERY_TAG value = adjudicated ? v.consensus_tag : want;
            if (value != FF_RECOVER_UNDEFINED)
                plan(seat + DATA_BLOCK::RECOVERY, 2, static_cast<uint64_t>(value));
            break;
        }
        case RepairClass::PositionRepaired: {
            plan(static_cast<uint64_t>(r.child) + DATA_BLOCK::VALIDATION, 8,
                 static_cast<uint64_t>(r.child));
            // The tag this verdict was PRICED for. A tuple's expected type lives
            // in the parent's tag half, so its in-place cost is val + tag, and
            // writing only the first half enacts half a verdict.
            const RECOVERY_TAG actual = tag_at(base, size, r.child);
            const RECOVERY_TAG actual_base =
                (r.kind == FF_FIELD_ARRAY) ? GetTypeFromTag(actual) : actual;
            if (want != FF_RECOVER_UNDEFINED && actual_base != r.declared)
                plan(static_cast<uint64_t>(r.child) + DATA_BLOCK::RECOVERY, 2,
                     static_cast<uint64_t>(want));
            break;
        }
        case RepairClass::ExtentDerived:
            plan(static_cast<uint64_t>(r.child) + FF_ARRAY::ENTRY_COUNT, 4, v.derived_extent);
            break;
        default:
            break;  // Intact, Ambiguous and Unrecovered are not repairs
    }
}

// THE WHOLE EDGE, NOT THE BYTE THAT WAS JUST STORED.
//
// Each repair class used to verify its own write by reading it back, which
// proves only that memory accepted a store. The question a repair has to answer
// is whether the EDGE now holds: the slot names the target, the target vouches
// for its own address, and its tag is the type the slot expects. One predicate
// for every class, so a class cannot quietly check less than its neighbours.
inline bool verify_edge(const BYTE* dst, size_t size, const BlockVerdict& v) noexcept {
    for (const PlannedWrite& w : v.writes) {
        const uint64_t got = w.width == 2   ? LOAD_U16(dst + w.seat)
                             : w.width == 4 ? LOAD_U32(dst + w.seat)
                                            : LOAD_U64(dst + w.seat);
        if (got != w.value)
            return false;
    }
    // An ENTRY_COUNT has no second witness anywhere on the wire (F4), so the
    // stored value is the only thing there is to check.
    if (v.class_ == RepairClass::ExtentDerived)
        return true;
    const BlockRef& r = v.block;
    const Offset target = (v.class_ == RepairClass::Corroborated && !v.candidates.empty())
                              ? v.candidates.front()
                              : r.child;
    if (!FF_BLOCK_IN_BOUNDS(target, static_cast<Size>(size)) ||
        !valid_validation(dst, size, target))
        return false;
    if (r.declared == FF_RECOVER_UNDEFINED)
        return true;
    // An adjudicated tag repair may legitimately name a type the slot's own
    // copy disagrees with -- that disagreement IS the repair -- so the
    // consensus is what the target is checked against.
    const RECOVERY_TAG want = (r.kind == FF_FIELD_ARRAY) ? ToArrayTag(r.declared) : r.declared;
    const RECOVERY_TAG expect =
        (v.class_ == RepairClass::TagRepaired && v.consensus_tag != FF_RECOVER_UNDEFINED)
            ? v.consensus_tag
            : want;
    return tag_at(dst, size, target) == expect;
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
    //
    // The walk is launched SPECULATIVELY, on the root exactly as the wire
    // states it, before the header has been reconciled. That is deliberate and
    // it costs nothing either way: when the root is intact -- four trials in
    // five -- the speculative walk is already the right one and nothing is
    // repeated; when it is damaged the walk bails on its first test and
    // returns an empty map, so re-running it against the restored root below
    // is as cheap as running it once. Reconciling first instead would
    // serialise the census behind a decision the census has to supply.
    RootAnchor root = wire_root();
    StreamMap chain_map;  // hierarchical producer: reachable blocks, sized
    StreamMap scan_map;   // scanned producer: the byte census
    std::exception_ptr chain_error;
    std::exception_ptr census_error;
    std::thread hierarchical([this, &chain_map, &chain_error, root] {
        try {
            chain_map = reachable_blocks_map(root);
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

    // REC-24 — THE HEADER FIRST, BECAUSE EVERYTHING BELOW STARTS AT ITS ROOT.
    //
    // Every pass after this one walks from the root the header names, so a
    // header that cannot be read is not a local loss but a total one. The
    // census is in hand now, which is what the reconciliation needs: the block
    // the root names, and the singleton blocks the metadata fields name, are
    // all in it. Re-walk only when the reconciliation actually moved the root,
    // so an intact header pays nothing at all.
    const RootAnchor reconciled = reconcile_header(scan_map, rep.header);
    for (const HeaderVerdict& v : rep.header)
        if (v.class_ == RepairClass::Corroborated)
            ++rep.header_repaired;
    if (reconciled.usable &&
        (reconciled.offset != root.offset || reconciled.tag != root.tag || !root.usable)) {
        root = reconciled;
        chain_map = reachable_blocks_map(root);
    }

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

    // The admission frontier starts at the RECONCILED root, like every other
    // walk in this function — reading the header again here is how a restored
    // root stops being the one the pass actually uses.
    std::vector<std::pair<Offset, RECOVERY_TAG>> frontier;
    if (root.usable && root.offset != FF_NULL_OFFSET && root.offset >= 0 &&
        static_cast<size_t>(root.offset) + DATA_BLOCK::HEADER_SIZE <= m_size &&
        plausible_tag(root.tag))
        frontier.emplace_back(root.offset, root.tag);
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
    const auto classify_core = [&](const BlockRef& r, BlockVerdict& v,
                                   const HoleCandidate*& chosen_hole) {
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
            // (F4), so it can only be DISPROVEN by geometry -- the count must
            // not conjure elements the buffer cannot hold. walk_array_extent
            // is that bound; it never derives a count from element damage
            // (a damaged element is repaired by its own reference verdict).
            // Measured regression: a walk that stopped at the first damaged
            // entry turned one flipped VALIDATION word in entry 28 into an
            // ExtentDerived that rewrote an intact 1,473-entry count to 28
            // and lost 97% of the reparse.
            if (r.kind == FF_FIELD_ARRAY &&
                static_cast<size_t>(r.child) + FF_ARRAY::HEADER_SIZE <= m_size) {
                const FF_ARRAY array(r.child, m_size, 0);
                const uint32_t stamped = array.entry_count(m_base);
                const RECOVERY_TAG element_tag = GetTypeFromTag(actual);
                const ElementShape shape = element_shape_of(element_tag, array.entry_kind(m_base));

                const uint16_t step = array.entry_step(m_base);

                if (shape == ElementShape::InlineBlock) {
                    // Geometry bound only: walked == stamped unless the stamped
                    // count overruns the buffer, which is the one case a
                    // derived extent may replace it.
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
        // TAG CONSENSUS (REC-22.2). For a TUPLE kind the type is on the wire
        // twice, so a disagreement is damage to one copy -- and the ranker
        // below cannot tell which. It costs the tag distance either way and
        // resolves the tie by preferring the parent, which is right exactly
        // half the time: a flip in the slot copy and a flip in the child's
        // header are the same one bit seen from two sides.
        //
        // So do not decide it by cost. Ask whether the block READS as each
        // candidate: a type is a hypothesis about a V-Table, and the wrong one
        // pulls offsets out of positions holding something else. Exactly one
        // coherent reading is an answer; anything else is Undecided and falls
        // through to be reported rather than guessed.
        if (IsTupleKind(r.kind) && valid_validation(m_base, m_size, r.child)) {
            const TagCopy damaged = adjudicate_tag(r.child, r.declared, actual);
            if (damaged != TagCopy::Undecided) {
                v.class_ = RepairClass::TagRepaired;
                v.damaged_copy = damaged;
                v.consensus_tag = (damaged == TagCopy::ParentSlot) ? actual : r.declared;
                v.bit_cost = hamming_cost(r.declared, actual);
                return;
            }
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
        // `hc` rides along because a hole candidate's OWN residual damage is
        // part of the repair: the plan has to restore the target's witnesses,
        // not only the parent's pointer at it. An orphan passes nullptr, having
        // no residual damage by construction.
        const auto consider_cost = [&](Offset p, uint32_t c, const HoleCandidate* hc = nullptr) {
            if (c < h_off) {
                h_off = c;
                off_unique = true;
                v.candidates.clear();
                v.candidates.push_back(p);
                chosen_hole = hc;
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
        // AN INLINE ARRAY ELEMENT HAS NO POINTER WORD, SO IT CANNOT BE
        // REPOINTED (F10). enumerate_array_entries emits such an element with
        // child == parent + field, because its address comes from array
        // geometry and nothing stores it. A repoint would write a target
        // address over the element's own VALIDATION, relabelling it as living
        // somewhere it does not. The only repair that means anything here is in
        // place, so the repoint hypothesis is never even formed.
        const bool inline_element = (r.parent + r.field == r.child);

        // A REPOINT WRITES THE SLOT, SO THE SLOT HAS TO BE A POINTER.
        //
        // Two of the three polymorphic slots hold a SIGNED offset RELATIVE to
        // the containing block, and one of those is only FOUR bytes wide
        // (CLAUDE.md, the three-polymorphic-slots table). Storing an absolute
        // 8-byte target into either one corrupts it and whatever follows it:
        // measured, a repoint onto an FF_FIELD_CODE fallback slot overwrote the
        // 4-byte code AND the 4 bytes after it, and the old applier did the
        // same thing for every class.
        //
        // The wire answers this without a schema guess, which matters because
        // the schema cannot: a choice variant carrying RECOVER_FF_STRING is a
        // plain string variant (absolute) or a date/time fallback (relative),
        // and the BlockRef records nothing that separates them.
        // enumerate_block_refs sets r.child from the RAW WORD for an absolute
        // arm and from a resolver for a relative one, so the stored word
        // equalling r.child is exactly the test -- and it holds whether or not
        // the slot is damaged, because r.child was read from those same bytes.
        const bool absolute_slot =
            (static_cast<uint64_t>(r.parent) + r.field + 8 <= m_size) &&
            LOAD_U64(m_base + r.parent + r.field) == static_cast<uint64_t>(r.child);

        if (r.declared != FF_RECOVER_UNDEFINED && !inline_element && absolute_slot) {
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
                consider_cost(hc.pos, cost, &hc);
            }
        }

        const bool in_place = h_inplace <= FF_RECOVERY_MAX_FLIPS;
        const bool in_off = off_unique && h_off <= FF_RECOVERY_MAX_FLIPS;
        if (in_place && in_off && h_inplace == h_off) {
            v.class_ = RepairClass::Ambiguous;  // tie — never guess
            v.bit_cost = h_inplace;
        } else if (in_place && (!in_off || h_inplace < h_off)) {
            // THE INNOCENT-BLOCK AMBIGUITY IS DECIDED HERE, NOT IN THE WRITER.
            //
            // `self_ok && !tag_ok` is two situations wearing one face: the
            // child's TAG was flipped, or the parent's OFFSET was flipped onto
            // an innocent, perfectly valid block of another type. Nothing local
            // separates them, and relabelling an innocent block destroys the
            // only surviving record of what it really was.
            //
            // apply() used to make that call, declining to write whenever the
            // child's wire tag was still a plausible type. The classifier never
            // applied the same test, so the verdict stayed TagRepaired,
            // rep.tag_repaired counted it, and the damage stayed on the wire
            // while the report claimed a repair -- 218 of 3,712 single-flip
            // probes on the ingest fixture, the largest single cluster the WP1
            // oracles found. The guard was right; its LOCATION was wrong.
            //
            // A tag that is not a plausible type cannot be an innocent block's
            // real type, so there is nothing to destroy and the repair is safe.
            // A plausible-but-different one is a genuine tie, and a tie is
            // Ambiguous -- reported with its alternatives, never guessed.
            if (!valid_validation(m_base, m_size, r.child))
                v.class_ = RepairClass::PositionRepaired;
            else
                v.class_ = plausible_tag(actual) ? RepairClass::Ambiguous
                                                 : RepairClass::TagRepaired;
            v.bit_cost = h_inplace;
        } else if (in_off) {
            v.class_ = RepairClass::Corroborated;
            v.bit_cost = h_off;
        } else {
            v.class_ = RepairClass::Unrecovered;
            v.bit_cost = (h_off != UINT32_MAX && h_off < h_inplace) ? h_off : h_inplace;
        }
    };

    // THE VERDICT, THEN ITS PLAN, IN THAT ORDER AND IN ONE PLACE.
    //
    // classify_core has several early returns -- the array extent cases settle
    // inside the Intact branch -- so the plan is built by the wrapper every
    // caller actually uses. That way a class decided on any path gets its
    // writes, and a path added later cannot forget to plan one.
    const auto classify_one = [&](const BlockRef& r, BlockVerdict& v) {
        const HoleCandidate* chosen_hole = nullptr;
        classify_core(r, v, chosen_hole);
        plan_repair(v, m_base, m_size, chosen_hole);
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
    //
    // It yields the winning CANDIDATE, not merely its position, because the
    // repair needs the candidate's own residual damage: a hole candidate does
    // not vouch for itself, so plan_repair has to restore the target's
    // witnesses as well as the parent's pointer at it.
    const auto match_ref = [&](const BlockRef& r, const std::vector<HoleCandidate>& cands,
                               uint32_t& best_cost) -> const HoleCandidate* {
        best_cost = UINT32_MAX;
        const HoleCandidate* winner = nullptr;
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
            if (cost < best_cost) { best_cost = cost; winner = &hc; unique = true; }
            else if (cost == best_cost) { unique = false; }
        }
        if (!unique || best_cost > FF_RECOVERY_MAX_FLIPS)
            return nullptr;  // no match, or a tie -- never guessed (P0-3)
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
                const HoleCandidate* hit = match_ref(r, cands, cost);
                if (hit == nullptr)
                    continue;
                const Offset winner = hit->pos;

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
                // PLAN IT HERE TOO. This is the SECOND hole matcher -- the
                // band-widening one -- and it promotes a verdict to
                // Corroborated without going through classify_one. A verdict
                // that never reaches plan_repair carries no writes, and apply()
                // declines a confident verdict with an empty plan rather than
                // inventing one, so every match this pass made was silently
                // dropped at the write step.
                plan_repair(rep.blocks[vi], m_base, m_size, hit);
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

    // REC-23 — A UNIQUE CANDIDATE IS NOT A CORROBORATED ONE.
    //
    // The repoint ranker accepts a candidate when it is the only one inside
    // the flip budget. That is a property of the SEARCH, not of the evidence:
    // when a reference's true child has itself been destroyed the byte scan
    // never lists it as an orphan, so the true answer is absent from the
    // candidate set and some innocent orphan of the same type is the only
    // thing left standing. `cands == 1` then makes off_unique trivially true
    // and the repoint is applied with full confidence.
    //
    // Measured at 512 flips: 11 of 206 repoints attached the WRONG block, and
    // in 10 of the 11 the true child was never a candidate. The cost is not a
    // lost reference but a plausible wrong one -- a real Observation hanging
    // off another entry, reading as valid FHIR. Misattribution is worse than
    // loss because nothing downstream can tell.
    //
    // Two witnesses the ranker never consulted, both already in hand:
    //
    //   LOCALITY. The writer appends children as it walks, so a field's
    //   targets ascend with parent order -- measured 1471/1472 on a clean
    //   bundle. A candidate outside the bracket set by the nearest INTACT
    //   references on either side is not a repair of this edge. Rejecting on
    //   that caught 10 of the 11 with zero false positives on the 195 correct.
    //
    //   NOT FOR RESOURCE TUPLES. Ascending order is a property of one append<T>
    //   laying out one subtree, and a resource slot's target is not in its
    //   parent's subtree: the Ingestor appends each contained/entry resource on
    //   its own, in worker-completion order, and writes the {offset, tag} tuple
    //   afterwards. Placement there is scheduler order, which is not a guarantee
    //   (TASKS.md COV-3). Measured on a concurrently ingested 1.15 MB Synthea
    //   bundle: 239/306 resource targets ascend against 15,971/16,107 for every
    //   other kind. Applied to them, the rule demoted a correct 1-bit repoint
    //   of Observation.contained[0] whenever the pool happened to place the
    //   contained Patient before its parent -- the intermittent
    //   ff_test_recovery failure. Exclusivity still applies to them.
    //
    //   EXCLUSIVITY. Two references cannot own the same child. Greedy per-ref
    //   picking let one steal the orphan another needed; that was the 11th.
    //
    // Demotion only: this pass can turn a repoint into Unrecovered, never the
    // reverse, so it cannot invent recovery -- it only declines to guess.
    {
        // Anchors: intact (parent -> child) per field, in parent order.
        std::map<Offset, std::vector<std::pair<Offset, Offset>>> anchors;
        for (const BlockVerdict& v : rep.blocks)
            if (v.class_ == RepairClass::Intact && v.block.child != FF_NULL_OFFSET)
                anchors[v.block.field].push_back({v.block.parent, v.block.child});
        for (auto& [field, a] : anchors)
            std::sort(a.begin(), a.end());

        // Exclusivity: how many repoints chose each address.
        std::map<Offset, int> claims;
        for (const BlockVerdict& v : rep.blocks)
            if (v.class_ == RepairClass::Corroborated && !v.candidates.empty())
                ++claims[v.candidates.front()];

        std::size_t demoted_locality = 0, demoted_contended = 0;
        for (BlockVerdict& v : rep.blocks) {
            if (v.class_ != RepairClass::Corroborated || v.candidates.empty())
                continue;
            const Offset chose = v.candidates.front();

            if (claims[chose] > 1) {
                // Contended: two references want one child and nothing here
                // says which is entitled to it. Neither may have it.
                v.class_ = RepairClass::Ambiguous;
                v.writes.clear();  // no longer a repair — it must not keep a plan
                ++demoted_contended;
                continue;
            }
            if (v.block.kind == FF_FIELD_RESOURCE)
                continue;  // placement is scheduler order — no locality witness
            const auto it = anchors.find(v.block.field);
            if (it == anchors.end() || it->second.size() < 2)
                continue;  // no neighbours to reason from — leave the ranker's call
            const auto& a = it->second;
            const auto hi = std::lower_bound(
                a.begin(), a.end(), std::make_pair(v.block.parent, Offset(0)));
            if (hi == a.begin() || hi == a.end())
                continue;  // outside the anchored run — no bracket to test against
            const Offset lo_child = (hi - 1)->second, hi_child = hi->second;
            if (lo_child >= hi_child)
                continue;  // anchors not ascending here — the rule does not hold
            if (chose <= lo_child || chose >= hi_child) {
                v.class_ = RepairClass::Unrecovered;
                v.candidates.clear();
                v.writes.clear();  // no longer a repair — it must not keep a plan
                ++demoted_locality;
            }
        }
        if (demoted_locality != 0 || demoted_contended != 0) {
            // Re-count: the tallies above were taken before these demotions.
            rep.intact = rep.corroborated = rep.tag_repaired = 0;
            rep.position_repaired = rep.extent_derived = 0;
            rep.ambiguous = rep.unrecovered = 0;
            for (const BlockVerdict& v : rep.blocks)
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
        switch (g.class_) {
            case GapClass::Hole:        ++rep.holes; break;
            case GapClass::VersionSkew: ++rep.version_skew; break;
            // Trailing: arena slack past the last entry — benign, uncounted.
            // Listed explicitly so a future GapClass member is triaged here.
            case GapClass::Trailing:    break;
        }
    }
    return rep;
}

// =====================================================================
// PRODUCERS — the two maps, in call order (REC-19.1)
// =====================================================================

StreamMap Recovery::reachable_blocks_map() const { return reachable_blocks_map(wire_root()); }

StreamMap Recovery::reachable_blocks_map(const RootAnchor& root) const {
    // The HIERARCHICAL producer (REC-19.4): the offset-chain walk, sized per
    // visited block. Sizes are the delta over the bare-offset walk — without
    // them the map cannot tile, and offset + entry.size is what maps holes.
    StreamMap map;
    map.file_size = static_cast<Size>(m_size);
    if (root.usable || header_is_readable(m_base, m_size))
        map[0] = {StreamMapEntryType::Header, 0, FF_HEADER::HEADER_SIZE};
    for (const Offset off : walk_chain(root, nullptr, &map.failures)) {
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
// REC-24 — THE HEADER, RECONCILED AGAINST THE CENSUS
// =====================================================================
// Fifty-four bytes with no self-offset of their own, and until this pass no
// repair at all: a single flipped bit in ROOT_OFFSET made the Parser refuse
// the whole stream, so a document whose 60,000 blocks were otherwise perfect
// scored nothing. Measured on a 3.3 MB Synthea artifact, the header is 54 of
// 495,882 corruptible positions, so at 2,048 flips it is hit in 1 - e^(-0.223)
// = 20% of trials -- which is exactly the 4-in-20 zero-scoring rate the
// benchmark recorded, and the whole distance between its 76.7% mean and its
// 95.6% median.
//
// Nothing here searches. Each field is compared against a witness that already
// exists elsewhere, and the rules differ because the witnesses do:
//
//   A CONSTANT the writer always stores. MAGIC and RECOVERY are compiled in,
//   so the true value is known outright and the only question is whether these
//   bytes are a FastFHIR stream at all. The census answers that: a position
//   whose eight bytes equal its own offset occurs by chance at 2^-64, so a
//   handful of them is proof, and a file that is not one produces none.
//
//   A CLOSED SET the writer chooses from. The FHIR revision is R4 or R5; the
//   stream layout is standard or compact. A value outside the set is damage,
//   and the unique member within the flip budget is the repair. When two
//   members are equally close the field stays Ambiguous -- R5 with bit 8
//   flipped IS R4, and no evidence in the header separates them.
//
//   AN IDENTITY WITH ANOTHER FIELD. The checksum footer is the last block a
//   sealed stream contains, so STREAM_SIZE equals CHECKSUM_OFFSET plus that
//   block's size on every stream the writer produces. Two fields, one fact,
//   each one a witness for the other.
//
//   A SINGLETON IN THE CENSUS. The checksum footer, the URL directory and the
//   module registry occur at most once per stream, so a field required to name
//   one has exactly one candidate whenever the block itself survived. That
//   candidate is not reached by proximity, so the flip budget does not govern
//   it: the budget bounds a SEARCH, and there is no search when the arena
//   contains one block of the type and no other. The Hamming distance is
//   recorded as a diagnostic.
//
//   THE BLOCK THE FIELD NAMES. ROOT_OFFSET and ROOT_RECOVERY witness each
//   other the way a resource tuple's two halves do: the header states the
//   root's address and its type, and the block at that address states its own
//   address and its own type. Whichever half survived identifies the other.
//
// Where a rule leaves more than one value supported, the verdict is Ambiguous
// and carries the alternatives. apply() writes the Corroborated ones and
// nothing else, so an ambiguity stays declared rather than becoming a silent
// choice.
Recovery::RootAnchor Recovery::wire_root() const noexcept {
    RootAnchor anchor;
    if (!header_is_readable(m_base, m_size))
        return anchor;
    anchor.offset = FF_HEADER(m_size).get_root(m_base);
    anchor.tag    = tag_at(m_base, m_size, anchor.offset);
    anchor.usable = true;
    return anchor;
}

Recovery::RootAnchor Recovery::reconcile_header(const StreamMap& census,
                                                std::vector<HeaderVerdict>& out) const {
    RootAnchor anchor;
    if (m_base == nullptr || m_size < FF_HEADER::HEADER_SIZE)
        return anchor;

    const auto record = [&out](HeaderField field, RepairClass cls, uint64_t stored,
                               uint64_t restored, uint32_t cost, const char* why) {
        out.push_back({field, cls, stored, restored, cost, why, {}});
    };

    // THE SUPPORT TEST, ONCE, FOR EVERY CONSTANT BELOW.
    //
    // Stamping a known constant over damaged bytes is only a repair when the
    // bytes are the thing the constant belongs to. A position holding its own
    // offset arises by chance at 2^-64 per position, so a file that is not a
    // FastFHIR arena produces none of them; requiring several is conclusive
    // without being a judgement call. This is what stops recovery declaring a
    // JPEG to be a damaged FastFHIR stream and writing a magic number into it.
    constexpr std::size_t kSupportingBlocks = 8;
    std::size_t anchored = 0;
    for (const auto& [off, entry] : census)
        if (entry.type != StreamMapEntryType::Header && ++anchored >= kSupportingBlocks)
            break;
    const bool supported = anchored >= kSupportingBlocks;

    // --- MAGIC and RECOVERY: compiled constants ---------------------------
    const auto reconcile_constant = [&](HeaderField field, uint64_t stored, uint64_t want,
                                        const char* name) {
        if (stored == want) {
            record(field, RepairClass::Intact, stored, want, 0, name);
            return;
        }
        const uint32_t cost = hamming_cost(stored, want);
        if (!supported)
            record(field, RepairClass::Unrecovered, stored, want, cost,
                   "no self-validating block corroborates a FastFHIR arena");
        else if (cost > FF_RECOVERY_MAX_FLIPS)
            record(field, RepairClass::Unrecovered, stored, want, cost,
                   "beyond the flip budget: these bytes were not this constant");
        else
            record(field, RepairClass::Corroborated, stored, want, cost, name);
    };
    reconcile_constant(HeaderField::Magic, LOAD_U32(m_base + FF_HEADER::MAGIC),
                       FF_MAGIC_BYTES, "compiled constant, corroborated by the census");
    reconcile_constant(HeaderField::Recovery, LOAD_U16(m_base + FF_HEADER::RECOVERY),
                       static_cast<uint64_t>(RECOVER_FF_HEADER),
                       "compiled constant, corroborated by the census");

    // --- FHIR_REV: a closed set of two ------------------------------------
    {
        constexpr uint16_t kRevisions[] = {FHIR_VERSION_R4, FHIR_VERSION_R5};
        const uint16_t stored = LOAD_U16(m_base + FF_HEADER::FHIR_REV);
        HeaderVerdict v{HeaderField::FhirRevision, RepairClass::Unrecovered, stored, stored,
                        0, "no legal revision within the flip budget", {}};
        uint32_t best = UINT32_MAX;
        for (const uint16_t rev : kRevisions) {
            const uint32_t cost = hamming_cost(stored, rev);
            if (cost < best) {
                best = cost;
                v.restored = rev;
                v.bit_cost = cost;
                v.candidates.assign(1, rev);
            } else if (cost == best) {
                v.candidates.push_back(rev);
            }
        }
        if (best == 0) {
            v.class_ = RepairClass::Intact;
            v.why    = "a revision this build knows";
            v.candidates.clear();
        } else if (v.candidates.size() > 1) {
            v.class_ = RepairClass::Ambiguous;
            v.why    = "equidistant from two legal revisions — nothing here separates them";
        } else if (best <= FF_RECOVERY_MAX_FLIPS) {
            v.class_ = RepairClass::Corroborated;
            v.why    = "the one legal revision within the flip budget";
            v.candidates.clear();
        }
        out.push_back(std::move(v));
    }

    // --- The three census singletons, and the root ------------------------
    //
    // A field required to name a block of type T has exactly one candidate
    // when the census holds one block of type T, and none when that block's
    // own header was destroyed. Absence is a legitimate value for all three of
    // the optional ones, so FF_NULL_OFFSET with an empty census reads Intact.
    const auto singletons_of = [&census](RECOVERY_TAG tag) {
        std::vector<Offset> found;
        for (const auto& [off, entry] : census)
            if (entry.type != StreamMapEntryType::Header && entry.recovery == tag)
                found.push_back(off);
        return found;
    };
    const auto reconcile_singleton = [&](HeaderField field, Size slot, RECOVERY_TAG tag,
                                         const char* name) -> Offset {
        const uint64_t stored = LOAD_U64(m_base + slot);
        const std::vector<Offset> found = singletons_of(tag);
        if (found.size() == 1 && stored == static_cast<uint64_t>(found.front())) {
            record(field, RepairClass::Intact, stored, stored, 0, name);
            return found.front();
        }
        if (found.empty()) {
            // Nothing of this type survives. An absent field is correct as it
            // stands; a present one names a block whose header is gone, and
            // rewriting it to "absent" would destroy the last record that the
            // block ever existed.
            record(field,
                   stored == static_cast<uint64_t>(FF_NULL_OFFSET) ? RepairClass::Intact
                                                                   : RepairClass::Unrecovered,
                   stored, stored, 0,
                   stored == static_cast<uint64_t>(FF_NULL_OFFSET)
                       ? "absent, and the census holds no such block"
                       : "the block this names no longer vouches for itself");
            return FF_NULL_OFFSET;
        }
        if (found.size() > 1) {
            HeaderVerdict v{field, RepairClass::Ambiguous, stored, stored, 0,
                            "more than one block of this type — the field names no unique block",
                            {}};
            for (const Offset o : found)
                v.candidates.push_back(static_cast<uint64_t>(o));
            out.push_back(std::move(v));
            return FF_NULL_OFFSET;
        }

        // A SINGLETON IS ONLY DECISIVE WHEN THE STORED VALUE AGREES WITH IT (F29).
        //
        // This used to read "one block of the type exists, therefore the field
        // names it", and that is wrong for a reason worth stating plainly: the
        // singleton itself can be MANUFACTURED BY THE DAMAGE. These three tags
        // are 0x0004, 0x0005 and 0x0006, so one flipped bit in any block's tag
        // anywhere in the arena creates a brand new "URL directory", the census
        // then holds exactly one, and a URL_DIR_OFFSET that was legitimately
        // absent was rewritten into a pointer at it. Measured by the WP1 gates:
        // roughly 60 invented bits, and the only cluster in the whole header
        // pass that wrote a WRONG byte rather than merely declining to write.
        //
        // The repair needs two things that agree, which is the same standard
        // every other field here is held to. The census supplies one. The
        // stored value supplies the other only when it is close enough to be a
        // damaged copy of that address: FF_NULL_OFFSET is all ones and a real
        // offset is small, so genuine absence is dozens of bits from any
        // candidate and can never be talked into pointing somewhere.
        const uint64_t candidate = static_cast<uint64_t>(found.front());
        const uint32_t cost = hamming_cost(stored, candidate);
        if (cost > FF_RECOVERY_MAX_FLIPS) {
            HeaderVerdict v{field,
                            stored == static_cast<uint64_t>(FF_NULL_OFFSET)
                                ? RepairClass::Intact
                                : RepairClass::Ambiguous,
                            stored, stored, cost,
                            stored == static_cast<uint64_t>(FF_NULL_OFFSET)
                                ? "absent, and the one block of this type is too far from "
                                  "absence to be a damaged copy of it — that block's tag is "
                                  "the likelier damage"
                                : "the one block of this type is beyond the flip budget of "
                                  "the stored address; the two do not corroborate",
                            {}};
            v.candidates.push_back(candidate);
            out.push_back(std::move(v));
            return FF_NULL_OFFSET;
        }
        record(field, RepairClass::Corroborated, stored, candidate, cost, name);
        return found.front();
    };

    const Offset checksum = reconcile_singleton(
        HeaderField::ChecksumOffset, FF_HEADER::CHECKSUM_OFFSET, RECOVER_FF_CHECKSUM,
        "the one self-validating checksum footer in the arena");
    reconcile_singleton(HeaderField::UrlDirectoryOffset, FF_HEADER::URL_DIR_OFFSET,
                        RECOVER_FF_URL_DIRECTORY,
                        "the one self-validating URL directory in the arena");
    reconcile_singleton(HeaderField::ModuleRegistryOffset, FF_HEADER::MODULE_REG_OFFSET,
                        RECOVER_FF_MODULE_REGISTRY,
                        "the one self-validating module registry in the arena");

    // --- STREAM_SIZE: the checksum footer is the last block ---------------
    {
        const uint64_t stored = LOAD_U64(m_base + FF_HEADER::STREAM_SIZE);
        if (checksum == FF_NULL_OFFSET) {
            record(HeaderField::StreamSize, RepairClass::Unrecovered, stored, stored, 0,
                   "no checksum footer to measure the end of the payload against");
        } else {
            const uint64_t sealed =
                static_cast<uint64_t>(checksum) + FF_CHECKSUM::HEADER_SIZE;
            record(HeaderField::StreamSize,
                   stored == sealed ? RepairClass::Intact : RepairClass::Corroborated, stored,
                   sealed, hamming_cost(stored, sealed),
                   "the checksum footer is the last block a sealed stream contains");
        }
    }

    // --- ROOT_OFFSET and ROOT_RECOVERY: two halves of one reference -------
    //
    // The same shape as a resource tuple. The header holds the root's address
    // and its type; the block holds its own address and its own type. A flip
    // in one half leaves the other standing, and which half moved is decided
    // by which one still agrees with the block.
    {
        const uint64_t     stored_off = LOAD_U64(m_base + FF_HEADER::ROOT_OFFSET);
        const RECOVERY_TAG stored_tag =
            static_cast<RECOVERY_TAG>(LOAD_U16(m_base + FF_HEADER::ROOT_RECOVERY));
        const bool         seat_ok = valid_validation(m_base, m_size,
                                                      static_cast<Offset>(stored_off));
        const RECOVERY_TAG seat_tag =
            seat_ok ? tag_at(m_base, m_size, static_cast<Offset>(stored_off))
                    : FF_RECOVER_UNDEFINED;

        if (seat_ok && seat_tag == stored_tag && plausible_tag(stored_tag)) {
            // Both halves agree with the block they describe.
            record(HeaderField::RootOffset, RepairClass::Intact, stored_off, stored_off, 0,
                   "the block here vouches for itself and for the stated type");
            record(HeaderField::RootRecovery, RepairClass::Intact, stored_tag, stored_tag, 0,
                   "the block here vouches for itself and for the stated type");
            anchor = {static_cast<Offset>(stored_off), stored_tag, true};
        } else if (seat_ok && plausible_tag(seat_tag)) {
            // The address lands on a real block, so the address survived and
            // the TYPE is the damaged half: the block's own tag is the witness.
            record(HeaderField::RootOffset, RepairClass::Intact, stored_off, stored_off, 0,
                   "the block here vouches for itself");
            record(HeaderField::RootRecovery, RepairClass::Corroborated, stored_tag, seat_tag,
                   hamming_cost(stored_tag, seat_tag),
                   "the root block's own tag, which the address still reaches");
            anchor = {static_cast<Offset>(stored_off), seat_tag, true};
        } else {
            // The address reaches no block. Search the census for the root the
            // header's surviving type half describes, then widen to any
            // resource if that half is gone too. Candidates must be within the
            // flip budget of the stored address: a block merely being of the
            // right type says nothing about it being THIS reference's target.
            std::vector<Offset> typed, resources;
            for (const auto& [off, entry] : census) {
                if (entry.type == StreamMapEntryType::Header)
                    continue;
                if (hamming_cost(stored_off, static_cast<uint64_t>(off)) > FF_RECOVERY_MAX_FLIPS)
                    continue;
                if (plausible_tag(stored_tag) && entry.recovery == stored_tag)
                    typed.push_back(off);
                else if (FF_IsResourceTag(entry.recovery))
                    resources.push_back(off);
            }
            const std::vector<Offset>& pool = !typed.empty() ? typed : resources;
            const char* why = !typed.empty()
                                  ? "the one block within the flip budget carrying the "
                                    "root type the header still states"
                                  : "both halves damaged — the one resource block within "
                                    "the flip budget of the stored address";
            if (pool.size() == 1) {
                const Offset found = pool.front();
                const RECOVERY_TAG found_tag = tag_at(m_base, m_size, found);
                record(HeaderField::RootOffset, RepairClass::Corroborated, stored_off,
                       static_cast<uint64_t>(found),
                       hamming_cost(stored_off, static_cast<uint64_t>(found)), why);
                record(HeaderField::RootRecovery,
                       found_tag == stored_tag ? RepairClass::Intact : RepairClass::Corroborated,
                       stored_tag, found_tag, hamming_cost(stored_tag, found_tag),
                       "the tag of the block the restored address reaches");
                anchor = {found, found_tag, true};
            } else {
                HeaderVerdict v{HeaderField::RootOffset, RepairClass::Unrecovered, stored_off,
                                stored_off, 0,
                                pool.empty()
                                    ? "no block within the flip budget of the stored address"
                                    : "several blocks within the flip budget — no unique root",
                                {}};
                if (pool.size() > 1)
                    v.class_ = RepairClass::Ambiguous;
                for (const Offset o : pool)
                    v.candidates.push_back(static_cast<uint64_t>(o));
                out.push_back(std::move(v));
                record(HeaderField::RootRecovery, RepairClass::Unrecovered, stored_tag,
                       stored_tag, 0, "no restored address to read a tag from");
            }
        }
    }

    // --- The 2-bit stream layout packed into VERSION ----------------------
    //
    // Only the layout bits are reconciled. The engine version beside them is a
    // free 30-bit word whose true value this reader cannot know: a stream may
    // legitimately have been written by an engine newer than this one, which
    // is precisely what find_gaps() uses to tell benign version skew from
    // damage. Guessing it would destroy that distinction, so it is left alone
    // and SAID SO, in its own verdict below. Leaving it unmentioned made a
    // flip in any of its 30 bits read as damage that recovery had neither
    // repaired nor reported, which is the one outcome the gates forbid.
    {
        const uint32_t            encoded = LOAD_U32(m_base + FF_HEADER::VERSION);
        const FF_StreamCompaction layout  = FF_HEADER_STREAM_LAYOUT(encoded);
        const bool legal = layout == FF_STREAM_COMPACTION_NONE || layout == FF_STREAM_COMPACTED;
        record(HeaderField::StreamLayout, legal ? RepairClass::Intact : RepairClass::Corroborated,
               static_cast<uint64_t>(layout),
               legal ? static_cast<uint64_t>(layout)
                     : static_cast<uint64_t>(FF_STREAM_COMPACTION_NONE),
               legal ? 0u : hamming_cost(static_cast<uint64_t>(layout), FF_STREAM_COMPACTION_NONE),
               legal ? "one of the two layouts the writer emits"
                     : "a layout the writer never emits; the census tiles as a standard stream");
        // The other 30 bits of the same word, declared uncertifiable rather
        // than left unmentioned. This is a REPORT, never a repair: there is no
        // witness for it, so the verdict says the field cannot be vouched for
        // and apply() writes nothing, since only Corroborated is ever written.
        record(HeaderField::EngineVersion, RepairClass::Unrecovered,
               FF_HEADER_ENGINE_VERSION(encoded), FF_HEADER_ENGINE_VERSION(encoded), 0,
               "no second witness anywhere in the arena: a stream written by a newer "
               "engine is legitimate, so this reader cannot tell a newer writer from "
               "a flipped bit and must not choose");
    }

    // The magic may have been restored above, and every field below it was
    // reconciled regardless — so a stream whose only damage was its magic
    // stamp still yields a usable root.
    if (!anchor.usable)
        return anchor;
    for (const HeaderVerdict& v : out)
        if (v.field == HeaderField::Magic && v.class_ != RepairClass::Intact &&
            v.class_ != RepairClass::Corroborated)
            anchor.usable = false;
    return anchor;
}

// =====================================================================
// REC-25 — THE CENSUS (recovery_algorithm_handoff.md §6)
// =====================================================================
// The first phase of the rebuilt engine. It reads every slot of every
// self-validating block, and every slot the FF_HEADER holds, and sorts the
// links those slots describe into two groups.
//
// A link whose every witness holds is DECIDED here. That means the parent's
// word names a position, the block at that position states its own offset
// there, and its tag is exactly the type the slot expects. Under the
// statistical model (handoff §5.3) such a link carries the full weight of all
// three witnesses and costs nothing, and no other explanation of the slot can
// cost less: any other position pays on the pointer, and because every block
// in the offset chain has exactly one parent (H5), no other slot can claim the
// block without the census seeing two claims.
//
// Every other slot is a question, and the census does not answer it. It
// records the slot as OPEN against the block that owns it. Blocks joined by
// decided links form ISLANDS. The FF_HEADER's island is attached from the
// start, because Phase 0 already reconciled the header, and the open slots of
// that island become the POINTS the task loop works through. Every other
// island is an orphaned subtree, and its open slots wait until the loop
// attaches it.
//
// Nothing here writes, and nothing here searches. On a clean stream the
// result is one attached island, no points and no holes.

namespace {

// The FF_HEADER sits at offset 0 and parents the root and the three metadata
// blocks. No self-validating block can start at 0, since the header is there,
// so the value can never collide with a real block's offset.
constexpr Offset HEADER_PARENT = 0;

// How far a residual tag may sit from the type it is read as, in bits. Used
// only where the tag is corroborated by something else (an array's geometry,
// or a word that names that exact block).
constexpr uint32_t TAG_RADIUS = 2;

// What a slot's bytes say, read on their own.
enum class Reading : uint8_t {
    Absent,    ///< the slot is empty
    Inline,    ///< the word is a value (a dictionary ID, a packed date/time, a scalar choice)
    Intact,    ///< every witness of the link holds: decided
    Dangling,  ///< the word names a position that is not a block of the expected type
    Suspect,   ///< the word reads as a value, but its bytes also name a block of the
               ///< type a damaged pointer would name there
};

struct Judged {
    Reading reading = Reading::Absent;
    Offset  target  = FF_NULL_OFFSET;  ///< Intact, Dangling and Suspect: the position the word names
};

// One self-validating block, as the scan found it.
struct Anchor {
    Offset       off         = FF_NULL_OFFSET;
    RECOVERY_TAG wire_tag    = FF_RECOVER_UNDEFINED;  ///< the tag on the wire, which may be the damage
    Size         prov_extent = 0;                     ///< provisional: under the wire tag
};

// Everything the census learns, and everything the task loop will extend.
struct Board {
    const BYTE* base = nullptr;
    size_t      size = 0;
    /// A newer writer may legitimately emit dictionary IDs and date/time
    /// packings this build does not know, so their unfamiliarity is evidence
    /// of damage only when the stream is not newer than this reader.
    bool        newer_stream = false;

    std::vector<Anchor>                  anchors;    ///< ascending by offset
    std::unordered_map<Offset, uint32_t> anchor_at;  ///< offset -> index into anchors
    std::vector<Gap>                     holes;      ///< ascending by start

    /// Each block's one parent (H5): the census fills it with decided links.
    std::unordered_map<Offset, Recovery::Slot>              owner;
    /// Per block: its slots whose link the census could not decide.
    std::unordered_map<Offset, std::vector<Recovery::Slot>> open_of;
    /// Arrays whose stamped geometry the surrounding bytes contradict.
    std::unordered_set<Offset>                              refuted;

    std::vector<Recovery::Island>             islands;
    std::unordered_map<Offset, uint32_t>      island_of;  ///< block -> index into islands
    /// Every unattached island's root, by its wire tag: the pools the task
    /// loop draws candidate children from.
    std::map<RECOVERY_TAG, std::set<Offset>>  orphans;
    std::vector<Recovery::Point>              points;
    std::vector<Recovery::Edge>               edges;      ///< every decided link, by seat
};

// ---- forward declarations: the census's callees, defined below it in
// ---- call order (REC-19.1) ----
std::vector<Recovery::Slot> header_slots(const BYTE* base, size_t size,
                                         const std::vector<HeaderVerdict>& verdicts,
                                         bool root_usable, Offset root_offset);
Board take_census(const BYTE* base, size_t size, const StreamMap& scan_map,
                  std::vector<Recovery::Slot> header);
void slots_of(const Board& B, Offset off, RECOVERY_TAG tag,
              std::vector<Recovery::Slot>& out, bool& refuted);
void array_slots(const Board& B, Offset off, RECOVERY_TAG tag,
                 std::vector<Recovery::Slot>& out, bool& refuted);
void url_directory_slots(const Board& B, Offset off, std::vector<Recovery::Slot>& out);
bool sizable_tag(RECOVERY_TAG tag);
uint64_t next_anchor_after(const Board& B, Offset off);
Judged judge(const Board& B, const Recovery::Slot& s);
Judged judge_tuple(const Board& B, const Recovery::Slot& s);
Judged judge_relative32(const Board& B, const Recovery::Slot& s, uint32_t raw);
Judged judge_relative63(const Board& B, const Recovery::Slot& s, uint64_t raw);

}  // namespace

Recovery::Census Recovery::census() const {
    // Phase 0 first. The header's slots take the values it reconciled wherever
    // it reconciled them exactly (header_slots() says which), so a flipped
    // ROOT_OFFSET is already restored by the time the census judges it.
    const StreamMap scan_map = scan();
    std::vector<HeaderVerdict> verdicts;
    const RootAnchor root = reconcile_header(scan_map, verdicts);
    Board B = take_census(m_base, m_size, scan_map,
                          header_slots(m_base, m_size, verdicts, root.usable, root.offset));
    Census c;
    c.extent  = static_cast<Size>(m_size);
    c.anchors = B.anchors.size();
    c.edges   = std::move(B.edges);
    c.islands = std::move(B.islands);
    c.holes   = std::move(B.holes);
    c.points  = std::move(B.points);
    return c;
}

namespace {

// The FF_HEADER's four slots, holding the values Phase 0 settled on. The root
// is a tuple, since ROOT_OFFSET followed by ROOT_RECOVERY is exactly a tuple's
// layout; the checksum footer, the URL directory and the module registry are
// absolute offsets whose type is fixed. A field reconcile_header() could not
// settle keeps its stored word, and the census judges that like any other.
std::vector<Recovery::Slot> header_slots(const BYTE* base, size_t size,
                                         const std::vector<HeaderVerdict>& verdicts,
                                         bool root_usable, Offset root_offset) {
    std::vector<Recovery::Slot> out;
    if (base == nullptr || size < FF_HEADER::HEADER_SIZE)
        return out;

    // The root takes Phase 0's address only where Phase 0 found the root by the
    // type the header still states, and it always keeps the header's own copy
    // of that type. When ROOT_RECOVERY disagrees with the block it names, the
    // two copies carry one flip between them and nothing in the header says
    // which one moved; reconcile_header() takes the block's copy, which is
    // wrong whenever the flip hit the block. That question belongs to the task
    // loop, which reads the block under both types, so here the slot keeps the
    // wire's tag, the disagreement leaves it open, and the loop decides.
    bool address_exact = false;
    for (const HeaderVerdict& v : verdicts)
        if (v.field == HeaderField::RootOffset && v.class_ == RepairClass::Corroborated)
            for (const HeaderVerdict& t : verdicts)
                if (t.field == HeaderField::RootRecovery && t.class_ == RepairClass::Intact)
                    address_exact = true;
    Recovery::Slot root;
    root.parent     = HEADER_PARENT;
    root.seat       = FF_HEADER::ROOT_OFFSET;
    root.kind       = FF_FIELD_RESOURCE;
    root.repr       = Recovery::SlotRepr::Tuple;
    root.stored     = (root_usable && address_exact) ? static_cast<uint64_t>(root_offset)
                                                     : LOAD_U64(base + FF_HEADER::ROOT_OFFSET);
    root.stored_tag = static_cast<RECOVERY_TAG>(LOAD_U16(base + FF_HEADER::ROOT_RECOVERY));
    out.push_back(root);

    struct Metadata { HeaderField field; Offset seat; RECOVERY_TAG tag; };
    constexpr Metadata metadata[] = {
        {HeaderField::ChecksumOffset,       FF_HEADER::CHECKSUM_OFFSET,   RECOVER_FF_CHECKSUM},
        {HeaderField::UrlDirectoryOffset,   FF_HEADER::URL_DIR_OFFSET,    RECOVER_FF_URL_DIRECTORY},
        {HeaderField::ModuleRegistryOffset, FF_HEADER::MODULE_REG_OFFSET, RECOVER_FF_MODULE_REGISTRY},
    };
    for (const Metadata& m : metadata) {
        Recovery::Slot s;
        s.parent = HEADER_PARENT;
        s.seat   = m.seat;
        s.kind   = FF_FIELD_BLOCK;
        s.repr   = Recovery::SlotRepr::Absolute;
        s.stored = LOAD_U64(base + m.seat);
        s.expect = m.tag;
        for (const HeaderVerdict& v : verdicts)
            if (v.field == m.field &&
                (v.class_ == RepairClass::Intact || v.class_ == RepairClass::Corroborated))
                s.stored = v.restored;
        out.push_back(s);
    }
    return out;
}

// The census proper (handoff §6), in five steps.
Board take_census(const BYTE* base, size_t size, const StreamMap& scan_map,
                  std::vector<Recovery::Slot> header) {
    Board B;
    B.base         = base;
    B.size         = size;
    B.newer_stream = stream_is_newer(base, size);

    // 0. The anchors and the holes, both straight from the scan. The holes do
    //    not depend on any judgement: they are the bytes no self-validating
    //    block's provisional extent covers.
    for (const auto& [off, entry] : scan_map) {
        if (entry.type == StreamMapEntryType::Header)
            continue;
        B.anchor_at.emplace(off, static_cast<uint32_t>(B.anchors.size()));
        B.anchors.push_back({off, entry.recovery, entry.size});
    }
    for (const Gap& g : scan_map.gaps)
        if (g.class_ == GapClass::Hole)
            B.holes.push_back(g);

    // 1. Judge every slot: the header's first, then each block's, each read
    //    under the block's own wire tag. Decided links are kept in this order,
    //    so every parent's children come out in the order of its slots.
    std::vector<Recovery::Edge>                    intact;
    std::unordered_map<Offset, std::size_t>        claim;      // child -> first claimant
    std::map<Offset, std::vector<std::size_t>>     contested;  // child -> every claimant
    std::vector<std::pair<Recovery::Slot, Offset>> suspects;
    const auto consider = [&](const Recovery::Slot& s) {
        const Judged j = judge(B, s);
        switch (j.reading) {
            case Reading::Absent:
            case Reading::Inline:
                break;
            case Reading::Intact: {
                const std::size_t at = intact.size();
                intact.push_back({s, j.target});
                const auto [it, first] = claim.emplace(j.target, at);
                if (!first) {
                    std::vector<std::size_t>& claimants = contested[j.target];
                    if (claimants.empty())
                        claimants.push_back(it->second);
                    claimants.push_back(at);
                }
                break;
            }
            case Reading::Dangling:
                B.open_of[s.parent].push_back(s);
                break;
            case Reading::Suspect:
                suspects.emplace_back(s, j.target);
                break;
        }
    };
    for (const Recovery::Slot& s : header)
        consider(s);
    std::vector<Recovery::Slot> slots;
    for (const Anchor& a : B.anchors) {
        slots.clear();
        bool refuted = false;
        slots_of(B, a.off, a.wire_tag, slots, refuted);
        for (const Recovery::Slot& s : slots)
            consider(s);
        if (refuted)
            B.refuted.insert(a.off);
    }

    // 2. A block claimed by two slots keeps neither claim, because at most one
    //    of the two words is right and the census cannot say which. Both slots
    //    open, and the task loop weighs them. The FF_HEADER is the exception:
    //    Phase 0 already decided what its slots name, so a second claim on the
    //    root is the other slot's damage.
    std::vector<bool> dropped(intact.size(), false);
    for (const auto& [child, claimants] : contested) {
        const bool header_holds = intact[claimants.front()].slot.parent == HEADER_PARENT;
        for (const std::size_t i : claimants) {
            if (header_holds && i == claimants.front())
                continue;
            dropped[i] = true;
            B.open_of[intact[i].slot.parent].push_back(intact[i].slot);
        }
    }
    std::unordered_map<Offset, std::vector<Offset>> kids;
    for (std::size_t i = 0; i < intact.size(); ++i) {
        if (dropped[i])
            continue;
        B.owner.emplace(intact[i].child, intact[i].slot);
        kids[intact[i].slot.parent].push_back(intact[i].child);
    }

    // 3. A suspect slot's word reads as a value and also names a block of the
    //    type a damaged pointer would name. That second reading is evidence
    //    only while the block has no parent: a block some other slot names
    //    intactly is accounted for, and the value is then just a value.
    for (const auto& [s, target] : suspects)
        if (!B.owner.contains(target))
            B.open_of[s.parent].push_back(s);

    // 4. The islands. The FF_HEADER roots the first, attached from the start;
    //    every block with no decided parent roots an unattached one. The walks
    //    use an explicit stack, so depth costs nothing.
    std::vector<bool> placed(B.anchors.size(), false);
    const auto grow = [&](Offset root, bool attached) {
        const uint32_t index = static_cast<uint32_t>(B.islands.size());
        Recovery::Island island{root, attached, {}};
        std::vector<Offset> stack{root};
        while (!stack.empty()) {
            const Offset b = stack.back();
            stack.pop_back();
            if (const auto it = B.anchor_at.find(b); it != B.anchor_at.end()) {
                if (placed[it->second])
                    continue;
                placed[it->second] = true;
            }
            island.members.push_back(b);
            B.island_of[b] = index;
            if (const auto k = kids.find(b); k != kids.end())
                stack.insert(stack.end(), k->second.rbegin(), k->second.rend());
        }
        B.islands.push_back(std::move(island));
    };
    if (!header.empty())
        grow(HEADER_PARENT, true);
    for (const Anchor& a : B.anchors)
        if (!B.owner.contains(a.off) && !placed[B.anchor_at.at(a.off)]) {
            grow(a.off, false);
            B.orphans[a.wire_tag].insert(a.off);
        }
    // Whatever is still unplaced lies on a loop of decided links: every block
    // on it has a parent, and that parent is on the loop too. The offset chain
    // is a tree, so only damage makes a loop -- a word that happens to name one
    // of its own ancestors. Break it at its lowest offset and open the slot
    // that closed it, since the census cannot tell which word on the loop is
    // the damaged one and the task loop can.
    for (const Anchor& a : B.anchors) {
        if (placed[B.anchor_at.at(a.off)])
            continue;
        const Recovery::Slot closing = B.owner.at(a.off);
        B.owner.erase(a.off);
        dropped[claim.at(a.off)] = true;
        B.open_of[closing.parent].push_back(closing);
        std::vector<Offset>& siblings = kids[closing.parent];
        siblings.erase(std::find(siblings.begin(), siblings.end(), a.off));
        grow(a.off, false);
        B.orphans[a.wire_tag].insert(a.off);
    }

    // 5. The points: the open slots and the refuted arrays of the attached
    //    island. An unattached island's questions wait until it is attached,
    //    because until then nothing says it belongs to the document at all.
    if (!B.islands.empty() && B.islands.front().attached) {
        for (const Offset m : B.islands.front().members) {
            if (const auto it = B.open_of.find(m); it != B.open_of.end())
                for (const Recovery::Slot& s : it->second)
                    B.points.push_back({Recovery::PointKind::Open, s, FF_NULL_OFFSET});
            if (B.refuted.contains(m))
                B.points.push_back({Recovery::PointKind::ArrayExtent, {}, m});
        }
    }
    const auto point_key = [](const Recovery::Point& p) {
        return p.kind == Recovery::PointKind::Open ? p.slot.seat
                                                   : p.array + FF_ARRAY::KIND_AND_STEP;
    };
    std::sort(B.points.begin(), B.points.end(),
              [&point_key](const Recovery::Point& a, const Recovery::Point& b) {
                  return point_key(a) < point_key(b);
              });
    for (std::size_t i = 0; i < intact.size(); ++i)
        if (!dropped[i])
            B.edges.push_back(intact[i]);
    std::sort(B.edges.begin(), B.edges.end(),
              [](const Recovery::Edge& a, const Recovery::Edge& b) { return a.slot.seat < b.slot.seat; });
    return B;
}

// Every slot of the block at `off`, read under `tag`. A data block's slots are
// its V-Table's; an array's are its entries; the URL directory's are its
// segment offsets. Everything else is a leaf.
void slots_of(const Board& B, Offset off, RECOVERY_TAG tag,
              std::vector<Recovery::Slot>& out, bool& refuted) {
    if (IsArrayTagged(tag)) {
        array_slots(B, off, tag, out, refuted);
        return;
    }
    if (tag == RECOVER_FF_URL_DIRECTORY) {
        url_directory_slots(B, off, out);
        return;
    }
    // A leaf (FF_STRING and everything sharing its layout, FF_CODED_VALUE, the
    // checksum footer, the module registry) has no reflected fields, and a tag
    // this build does not know has none either.
    const auto fields = reflected_fields_view(static_cast<uint16_t>(tag));
    if (fields.empty())
        return;
    // H3. A data block never contains another block's first byte, so a type
    // whose V-Table would reach the next self-validating block is not this
    // block's type, and its V-Table must not be read here. Reading it anyway
    // lifts the NEXT block's slots as this one's, and every child they name
    // is then claimed twice. A newer writer only ever makes a block larger
    // than this build's table says, so the true type is never refused by this.
    if (static_cast<uint64_t>(off) + Recovery::derived_block_size(tag) > next_anchor_after(B, off))
        return;
    for (const FF_FieldInfo& f : fields) {
        const uint64_t seat = static_cast<uint64_t>(off) + f.field_offset;
        if (seat + ff_slot_width(f.kind) > B.size)
            continue;
        Recovery::Slot s;
        s.parent = off;
        s.seat   = static_cast<Offset>(seat);
        s.kind   = f.kind;
        switch (f.kind) {
            case FF_FIELD_BLOCK:
            case FF_FIELD_STRING:
            case FF_FIELD_ARRAY:
                s.repr   = Recovery::SlotRepr::Absolute;
                s.stored = LOAD_U64(B.base + seat);
                s.expect = f.child_recovery;
                break;
            case FF_FIELD_RESOURCE:
            case FF_FIELD_CHOICE:
                s.repr       = Recovery::SlotRepr::Tuple;
                s.stored     = LOAD_U64(B.base + seat);
                s.stored_tag = static_cast<RECOVERY_TAG>(LOAD_U16(B.base + seat + 8));
                break;
            case FF_FIELD_CODE:
                s.repr   = Recovery::SlotRepr::Relative32;
                s.stored = LOAD_U32(B.base + seat);
                s.expect = RECOVER_FF_CODED_VALUE;
                break;
            case FF_FIELD_DATETIME:
                s.repr   = Recovery::SlotRepr::Relative63;
                s.stored = LOAD_U64(B.base + seat);
                s.expect = RECOVER_FF_STRING;
                break;
            // An inline value names nothing. A URL slot is an index into the
            // URL directory, not an offset. An identity slot is a
            // cross-reference between resources, which sits outside the
            // offset chain (H5) and takes no part in structural recovery.
            case FF_FIELD_UNKNOWN:
            case FF_FIELD_BOOL:
            case FF_FIELD_INT32:
            case FF_FIELD_UINT32:
            case FF_FIELD_INT64:
            case FF_FIELD_UINT64:
            case FF_FIELD_FLOAT64:
            case FF_FIELD_URL:
            case FF_FIELD_ID:
                continue;
        }
        out.push_back(s);
    }
}

// The entries of an array, bounded by the bytes around it rather than by the
// stamped count alone.
//
// An array's entries carry no witnesses of their own, so its stamped geometry
// (the entry kind, the stride and the count) is believed only as far as the
// arena agrees with it. The arena is dense: a block starts where the previous
// one ends, so the entries of a tuple or offset array end at the next
// self-validating block, and the entries of an inline array are themselves
// blocks of the element type, one stride apart. Where the stamped geometry
// runs past what the bytes support, the census reads only the entries the
// bytes do support and marks the array REFUTED, which becomes one point. That
// is what keeps a flipped count from opening a slot for every phantom entry.
//
// A damaged entry does not end the array. An inline entry whose self-offset
// was flipped still sits at its stride, inside a hole, with its tag intact, and
// it is read as an entry whose link is open.
void array_slots(const Board& B, Offset off, RECOVERY_TAG tag,
                 std::vector<Recovery::Slot>& out, bool& refuted) {
    const uint64_t entries = static_cast<uint64_t>(off) + FF_ARRAY::HEADER_SIZE;
    if (entries > B.size) {
        refuted = true;  // the header itself does not fit
        return;
    }
    const RECOVERY_TAG element  = GetTypeFromTag(tag);
    const uint16_t     packed   = LOAD_U16(B.base + off + FF_ARRAY::KIND_AND_STEP);
    const uint64_t     stride   = packed & FF_ARRAY::STEP_MASK;
    const uint32_t     stamped  = LOAD_U32(B.base + off + FF_ARRAY::ENTRY_COUNT);
    const ElementShape shape    = element_shape_of(
        element, static_cast<FF_ARRAY::EntryKind>(packed & FF_ARRAY::KIND_MASK));

    // The first self-validating block after the header: under the density
    // premise, where a tuple, offset or scalar array's entries end.
    const uint64_t limit = next_anchor_after(B, off);
    const auto in_hole = [&B](uint64_t p) {
        const auto h = std::upper_bound(B.holes.begin(), B.holes.end(), p,
                                        [](uint64_t q, const Gap& g) { return q < g.start; });
        return h != B.holes.begin() &&
               p < static_cast<uint64_t>(std::prev(h)->start) + std::prev(h)->length;
    };
    const auto anchor_tag = [&B](uint64_t p) {
        const auto it = B.anchor_at.find(static_cast<Offset>(p));
        return it != B.anchor_at.end() ? B.anchors[it->second].wire_tag : FF_RECOVER_UNDEFINED;
    };

    switch (shape) {
        case ElementShape::None: {
            // Raw scalars name nothing, but their count can still overrun.
            const uint64_t end = entries + static_cast<uint64_t>(stamped) * stride;
            if (end > limit)
                refuted = true;
            return;
        }
        case ElementShape::Tuple:
        case ElementShape::OffsetPtr: {
            const uint64_t width = shape == ElementShape::Tuple ? 10 : 8;
            const uint64_t room  = limit > entries ? (limit - entries) / width : 0;
            const uint64_t n     = std::min<uint64_t>(stamped, room);
            if (stamped > room)
                refuted = true;
            // A count flipped LOW leaves the rest of the entries unclaimed: a
            // hole begins exactly where the stamped entries end, and its first
            // bytes still read as an entry whose every witness holds.
            const uint64_t tail = entries + static_cast<uint64_t>(stamped) * width;
            if (!refuted && tail + width <= limit && in_hole(tail)) {
                const Offset t = static_cast<Offset>(LOAD_U64(B.base + tail));
                const RECOVERY_TAG want = shape == ElementShape::Tuple
                                              ? static_cast<RECOVERY_TAG>(LOAD_U16(B.base + tail + 8))
                                              : element;
                if (t != FF_NULL_OFFSET && anchor_tag(t) == want)
                    refuted = true;
            }
            for (uint64_t i = 0; i < n; ++i) {
                const uint64_t p = entries + i * width;
                Recovery::Slot s;
                s.parent = off;
                s.seat   = static_cast<Offset>(p);
                s.stored = LOAD_U64(B.base + p);
                if (shape == ElementShape::Tuple) {
                    s.kind       = FF_FIELD_RESOURCE;
                    s.repr       = Recovery::SlotRepr::Tuple;
                    s.stored_tag = static_cast<RECOVERY_TAG>(LOAD_U16(B.base + p + 8));
                } else {
                    s.kind   = FF_FIELD_STRING;
                    s.repr   = Recovery::SlotRepr::Absolute;
                    s.expect = element;
                }
                out.push_back(s);
            }
            return;
        }
        case ElementShape::InlineBlock: {
            if (stride < DATA_BLOCK::HEADER_SIZE) {
                refuted = true;  // no block fits a stride this short
                return;
            }
            // Each position the stamped count covers is one of three things.
            // EXACT: a block of the element type, or the unclaimed remains of
            // one (a hole position whose residual tag is still the element
            // type's, because only its self-offset was damaged). NEAR: a
            // block, or remains, whose tag is within TAG_RADIUS of the element
            // type. FOREIGN: anything else, which means the array has already
            // ended.
            //
            // A NEAR position is an entry whose tag was damaged only if the
            // array demonstrably continues after it. At the tail there is
            // nothing after it, and there a flipped entry tag and a flipped
            // count look exactly alike: one entry's tag moved a bit, or the
            // count moved and this is the next block, whose tag happens to lie
            // a bit from the element type (an FF_STRING, 0x0002, is one bit
            // from a Coding, 0x0202). The census does not choose. It reads the
            // entries up to the last EXACT one and refutes the array, which
            // asks the task loop one question about its extent.
            uint64_t last_exact = 0;  // one past the last EXACT position
            for (uint64_t i = 0; i < stamped; ++i) {
                const uint64_t p = entries + i * stride;
                if (p + stride > B.size)
                    break;
                const RECOVERY_TAG seen = anchor_tag(p);
                const RECOVERY_TAG residual =
                    seen != FF_RECOVER_UNDEFINED
                        ? seen
                        : (in_hole(p) ? FF_GET_RECOVERY_TAG(B.base, static_cast<Offset>(p))
                                      : FF_RECOVER_UNDEFINED);
                if (residual == FF_RECOVER_UNDEFINED ||
                    Recovery::hamming_cost(residual, element) > TAG_RADIUS)
                    break;
                if (residual == element)
                    last_exact = i + 1;
            }
            if (last_exact < stamped)
                refuted = true;
            for (uint64_t i = 0; i < last_exact; ++i) {
                const uint64_t p = entries + i * stride;
                Recovery::Slot s;
                s.parent = off;
                s.seat   = static_cast<Offset>(p);
                s.kind   = FF_FIELD_BLOCK;
                s.repr   = Recovery::SlotRepr::InlineEntry;
                s.expect = element;
                out.push_back(s);
            }
            // A count flipped LOW leaves more entries after the stamped ones:
            // a block of exactly the element type sits at the next stride, and
            // it fits there (H3). The fit is what separates a real entry from
            // the block that follows the array having had its own tag flipped
            // into the element type: the string after a Coding array, tagged
            // 0x0202 by one flip, would need 55 bytes and has 30.
            const uint64_t after = entries + static_cast<uint64_t>(stamped) * stride;
            if (anchor_tag(after) == element &&
                after + stride <= next_anchor_after(B, static_cast<Offset>(after)))
                refuted = true;
            return;
        }
    }
}

// The URL directory's segment offsets. Each 16-byte entry ends with the
// absolute offset of the FF_STRING holding that entry's segment.
void url_directory_slots(const Board& B, Offset off, std::vector<Recovery::Slot>& out) {
    const uint64_t table = static_cast<uint64_t>(off) + FF_URL_DIRECTORY::HEADER_SIZE;
    if (table > B.size)
        return;
    // The table ends where the next self-validating block begins, for the
    // same reason an array's entries do.
    const uint64_t limit = next_anchor_after(B, off);
    const uint64_t room  = limit > table ? (limit - table) / FF_URL_DIRECTORY::URL_ENTRY_SIZE : 0;
    const uint64_t n = std::min<uint64_t>(LOAD_U32(B.base + off + FF_URL_DIRECTORY::ENTRY_COUNT), room);
    for (uint64_t i = 0; i < n; ++i) {
        const uint64_t seat =
            table + i * FF_URL_DIRECTORY::URL_ENTRY_SIZE + FF_URL_DIRECTORY::URL_ENTRY_SEG_OFFSET;
        Recovery::Slot s;
        s.parent = off;
        s.seat   = static_cast<Offset>(seat);
        s.kind   = FF_FIELD_STRING;
        s.repr   = Recovery::SlotRepr::Absolute;
        s.stored = LOAD_U64(B.base + seat);
        s.expect = RECOVER_FF_STRING;
        out.push_back(s);
    }
}

// Does this build know the layout of a block carrying `tag`? Recovery's
// plausible_tag() cannot answer that: it maps every value from 0x0200 up to
// FF_FIELD_BLOCK, so 0xFFFF passes it. This asks the layouts themselves. A
// newer writer's tag fails it too, which only ever makes the census trust a
// position less, never more.
bool sizable_tag(RECOVERY_TAG tag) {
    if (IsArrayTagged(tag)) {
        const RECOVERY_TAG element = GetTypeFromTag(tag);
        if (element == RECOVER_FF_RESOURCE)
            return true;  // a resource array: 10-byte tuples
        if (FF_IsScalarBlockTag(element))
            return Recovery_to_Kind(element) != FF_FIELD_UNKNOWN;  // raw scalars
        return sizable_tag(element);
    }
    if (FF_IsStringLayoutTag(tag) || tag == RECOVER_FF_CODED_VALUE || tag == RECOVER_FF_CHECKSUM ||
        tag == RECOVER_FF_URL_DIRECTORY || tag == RECOVER_FF_MODULE_REGISTRY)
        return true;
    return !reflected_fields_view(static_cast<uint16_t>(tag)).empty();
}

// Where the first block after `off` starts, or the end of the trusted extent
// when there is none. The arena is dense, so this is where the block at `off`
// must end, and nothing it contains may lie past it.
//
// Only a self-validating position whose tag this build can size counts as a
// block here. A pointer word that a flip has turned into its own address
// self-validates as well -- 748 with bit 7 cleared is 620, and a slot at 620
// holding a pointer to a block 128 bytes away is one flip from that -- and the
// two bytes after it are whatever the next slot holds, almost never an
// assigned tag. Letting such a word end the block around it would refuse that
// block's true V-Table.
uint64_t next_anchor_after(const Board& B, Offset off) {
    auto next = std::upper_bound(B.anchors.begin(), B.anchors.end(), off,
                                 [](Offset o, const Anchor& a) { return o < a.off; });
    while (next != B.anchors.end() && !sizable_tag(next->wire_tag))
        ++next;
    return next != B.anchors.end() ? std::min<uint64_t>(next->off, B.size)
                                   : static_cast<uint64_t>(B.size);
}

// The type a slot's child must carry on the wire for the link to be decided.
// An ARRAY slot's compiled expectation is the element type, and the array
// block itself carries it with the array bit set, so the comparison is exact
// including that bit.
RECOVERY_TAG wire_expectation(const Recovery::Slot& s) noexcept {
    if (s.repr == Recovery::SlotRepr::Tuple)
        return s.stored_tag;
    if (s.kind == FF_FIELD_ARRAY)
        return ToArrayTag(s.expect);
    return s.expect;
}

// Which types a tuple slot may hold (H1). The FF_HEADER may be rooted at any
// block the Builder was given. A resource slot holds a resource, or an opaque
// resource kept verbatim from outside the profile. A choice slot admits every
// type this build knows until the generator emits each field's variant list
// (RA-2).
bool band_admits(const Recovery::Slot& s, RECOVERY_TAG tag) noexcept {
    if (!Recovery::plausible_tag(tag))
        return false;
    if (s.parent == HEADER_PARENT || s.kind == FF_FIELD_CHOICE)
        return true;
    return FF_IsResourceTag(tag) || tag == RECOVER_FF_OPAQUE_JSON;
}

// Does the block at `t` vouch for itself and carry exactly the type `want`?
Judged settle(const Board& B, uint64_t t, RECOVERY_TAG want) {
    const auto it = B.anchor_at.find(static_cast<Offset>(t));
    if (it != B.anchor_at.end() && B.anchors[it->second].wire_tag == want)
        return {Reading::Intact, static_cast<Offset>(t)};
    return {Reading::Dangling, static_cast<Offset>(t)};
}

// Is the block at `t` a self-validating block of exactly the type `want`?
bool names_a_block_of(const Board& B, uint64_t t, RECOVERY_TAG want) {
    const auto it = B.anchor_at.find(static_cast<Offset>(t));
    return it != B.anchor_at.end() && B.anchors[it->second].wire_tag == want;
}

Judged judge(const Board& B, const Recovery::Slot& s) {
    switch (s.repr) {
        case Recovery::SlotRepr::Absolute:
            if (s.stored == static_cast<uint64_t>(FF_NULL_OFFSET))
                return {};
            return settle(B, s.stored, wire_expectation(s));
        case Recovery::SlotRepr::InlineEntry:
            return settle(B, s.seat, s.expect);
        case Recovery::SlotRepr::Tuple:
            return judge_tuple(B, s);
        case Recovery::SlotRepr::Relative32:
            return judge_relative32(B, s, static_cast<uint32_t>(s.stored));
        case Recovery::SlotRepr::Relative63:
            return judge_relative63(B, s, s.stored);
    }
    return {};
}

// A tuple is read by its tag half: an offset-bearing type names a block at the
// absolute offset in its first eight bytes, a code or date/time names one only
// through a relative fallback, and a scalar names nothing.
//
// Its tag half can be the damaged witness, though, and a flip there can turn a
// pointer into what reads as a scalar, a code or a date/time. The word then
// still holds the child's exact offset, which a real value almost never does:
// the child is a self-validating block, and its tag sits within TAG_RADIUS of
// the stored one. That is read as Suspect, never as a value.
Judged judge_tuple(const Board& B, const Recovery::Slot& s) {
    if (s.stored == static_cast<uint64_t>(FF_NULL_OFFSET))
        return {};  // monostate: the slot is empty
    if (!band_admits(s, s.stored_tag))
        return {Reading::Suspect, static_cast<Offset>(s.stored)};
    const FF_FieldKind kind = Recovery_to_Kind(s.stored_tag);
    const bool names_by_offset = kind == FF_FIELD_STRING || kind == FF_FIELD_BLOCK ||
                                 kind == FF_FIELD_RESOURCE || kind == FF_FIELD_ARRAY ||
                                 kind == FF_FIELD_CHOICE || kind == FF_FIELD_URL;
    if (names_by_offset)
        return settle(B, s.stored, s.stored_tag);
    if (const auto it = B.anchor_at.find(static_cast<Offset>(s.stored)); it != B.anchor_at.end()) {
        const RECOVERY_TAG there = B.anchors[it->second].wire_tag;
        const FF_FieldKind there_kind = Recovery_to_Kind(there);
        const bool there_by_offset = there_kind == FF_FIELD_STRING || there_kind == FF_FIELD_BLOCK ||
                                     there_kind == FF_FIELD_RESOURCE;
        if (there_by_offset && band_admits(s, there) &&
            Recovery::hamming_cost(there, s.stored_tag) <= TAG_RADIUS)
            return {Reading::Suspect, static_cast<Offset>(s.stored)};
    }
    switch (kind) {
        case FF_FIELD_CODE:
            return judge_relative32(B, s, static_cast<uint32_t>(s.stored));
        case FF_FIELD_DATETIME:
            return judge_relative63(B, s, s.stored);
        case FF_FIELD_BOOL:
        case FF_FIELD_INT32:
        case FF_FIELD_UINT32:
        case FF_FIELD_INT64:
        case FF_FIELD_UINT64:
        case FF_FIELD_FLOAT64:
        case FF_FIELD_ID:
            return {Reading::Inline, FF_NULL_OFFSET};
        // Excluded above: band_admits refuses an unknown tag, and the
        // offset-bearing kinds were settled already.
        case FF_FIELD_UNKNOWN:
        case FF_FIELD_STRING:
        case FF_FIELD_BLOCK:
        case FF_FIELD_RESOURCE:
        case FF_FIELD_ARRAY:
        case FF_FIELD_CHOICE:
        case FF_FIELD_URL:
            break;
    }
    return {Reading::Suspect, static_cast<Offset>(s.stored)};
}

// A code slot. With bit 31 set, the word is an offset relative to the block
// that holds the slot, naming an FF_CODED_VALUE. With it clear, the word is a
// dictionary ID, which names nothing -- unless it is a fallback offset whose
// flag bit was the damage. Two things give that away: set the bit back and the
// word names an FF_CODED_VALUE exactly (which a real ID does only by a
// coincidence the H5 check in the census then removes), or the ID is not in
// this build's ledger although the stream is not newer than this build.
Judged judge_relative32(const Board& B, const Recovery::Slot& s, uint32_t raw) {
    if (raw == FF_CODE_NULL)
        return {};
    const Offset as_reference =
        FF_ResolveCodeableConceptOffset(raw | FF_CODED_VALUE_FLAG, s.parent);
    if (!(raw & FF_CODED_VALUE_FLAG)) {
        if (names_a_block_of(B, as_reference, RECOVER_FF_CODED_VALUE))
            return {Reading::Suspect, as_reference};
        if (!B.newer_stream && FF_ResolveCode(raw, 0) == nullptr)
            return {Reading::Suspect, as_reference};
        return {Reading::Inline, FF_NULL_OFFSET};
    }
    return settle(B, as_reference, RECOVER_FF_CODED_VALUE);
}

// A date/time slot: the 8-byte counterpart of the code slot. With bit 63
// clear the word is a packed civil date/time, and a packing no writer emits
// (a day past 9999-12-31, an hour past 23, an unused precision) is the tell
// that the flag bit was the damage.
Judged judge_relative63(const Board& B, const Recovery::Slot& s, uint64_t raw) {
    if (raw == FF_DATETIME_NULL)
        return {};
    const Offset as_reference =
        FF_ResolveDateTimeOffset(raw | FF_DATETIME_FALLBACK_FLAG, s.parent);
    if (!(raw & FF_DATETIME_FALLBACK_FLAG)) {
        if (names_a_block_of(B, as_reference, RECOVER_FF_STRING))
            return {Reading::Suspect, as_reference};
        const FF_DateTimeParts parts = FF_UNPACK_DATETIME(raw);
        const bool well_formed =
            ff_datetime_fits(parts) &&
            static_cast<uint8_t>(parts.precision) <= static_cast<uint8_t>(FF_DateTimePrecision::FRAC3);
        if (!B.newer_stream && !well_formed)
            return {Reading::Suspect, as_reference};
        return {Reading::Inline, FF_NULL_OFFSET};
    }
    return settle(B, as_reference, RECOVER_FF_STRING);
}

}  // namespace

// =====================================================================
// The walk — the hierarchical producer's engine, then its enumerators
// =====================================================================

// The root arrives as an argument rather than being read here, and that is the
// whole point of the RootAnchor type: reconcile_header() may have restored one
// or both halves out of the census, and a callee that re-reads the wire would
// walk the damaged value while the report says the good one. One root per
// recover(), decided once (REC-24).
std::vector<Offset> Recovery::walk_chain(const RootAnchor& root, std::vector<BlockRef>* out,
                                         std::vector<ProducerFailure>* failures) const {
    std::vector<Offset> reachable;
    if (!root.usable || !valid_validation(m_base, m_size, root.offset) ||
        !plausible_tag(root.tag))
        return reachable;

    // DFS through INTACT references only, depth- and cycle-bounded. Marking on
    // completion is unnecessary here (the verdict of a reference does not
    // depend on how it was reached), so a simple visited set bounds the work.
    struct Pending { Offset off; std::size_t depth; RECOVERY_TAG tag; };
    std::unordered_set<Offset> visited;
    std::vector<Pending> stack{{root.offset, 0, root.tag}};
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
    // Bound by GEOMETRY (REC-21.1): the stamped count when the arena has room
    // for it, and the largest fitting extent when it does not, so a corrupted
    // ENTRY_COUNT cannot conjure thousands of references out of a small array.
    // walk_array_extent never stops at a DAMAGED entry -- elements sit at a
    // fixed stride, so damage to one element is work for the matcher, not the
    // end of the array (stopping there turned one flipped VALIDATION word into
    // an ExtentDerived that overwrote an intact 1,473-entry count).
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
                    // An absent element, not the end of the array: the reader
                    // exports every entry after it, so the enumerator must too.
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
                        continue;  // an absent element; see the tuple branch above
                    out.push_back(BlockRef{array_off, static_cast<Offset>(pos - static_cast<uint64_t>(array_off)),
                                           shape == ElementShape::OffsetPtr ? FF_FIELD_STRING : FF_FIELD_BLOCK,
                                           child, element_tag, FF_RECOVER_UNDEFINED});
                }
            }

}

bool Recovery::reads_as(Offset off, RECOVERY_TAG tag) const {
    if (!plausible_tag(tag))
        return false;
    std::vector<BlockRef> kids;
    enumerate_block_refs(off, tag, kids);
    if (kids.empty())
        return false;  // no children to judge by — silence, not agreement
    return batch_passes(m_base, m_size, kids, BatchTest::EveryChildCorroborates);
}

TagCopy Recovery::adjudicate_tag(Offset child, RECOVERY_TAG slot_tag,
                                 RECOVERY_TAG child_tag) const {
    // Both copies are plausible types or there would be nothing to decide --
    // plausibility is what the ranker already used, and it is exactly what
    // fails here: 0x1012 and 0x1010 are both real resource tags, one bit apart.
    // So ask the bytes instead of the tag.
    const bool slot_reads  = reads_as(child, slot_tag);
    const bool child_reads = reads_as(child, child_tag);
    if (slot_reads == child_reads)
        return TagCopy::Undecided;  // both coherent, or neither — no opinion
    // The coherent reading names the true type; the OTHER copy is the damage.
    return slot_reads ? TagCopy::ChildHeader : TagCopy::ParentSlot;
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
                // No-default on purpose: Recovery_to_Kind's output set is closed
                // (see the mapping in FF_Primitives.hpp), so a kind added there
                // must be triaged here at compile time, never mis-read.
                switch (variant_kind) {
                    case FF_FIELD_UNKNOWN:
                        // The tag half names no kind this build knows.
                        continue;
                    case FF_FIELD_BOOL:
                    case FF_FIELD_INT32:
                    case FF_FIELD_UINT32:
                    case FF_FIELD_INT64:
                    case FF_FIELD_UINT64:
                    case FF_FIELD_FLOAT64:
                        continue;  // inline scalar value — no addressable child (D3)
                    case FF_FIELD_DATETIME: {
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
                    case FF_FIELD_CODE: {
                        // Code occupies the low 4 bytes of the 8-byte value area;
                        // MSB clear = packed dictionary ID, MSB set = signed
                        // relative offset to an FF_CODED_VALUE fallback block.
                        const uint32_t raw_code = static_cast<uint32_t>(raw);
                        if (!(raw_code & FF_CODED_VALUE_FLAG))
                            continue;
                        out.push_back(BlockRef{block_offset, static_cast<Offset>(f.field_offset), f.kind,
                                               FF_ResolveCodeableConceptOffset(raw_code, block_offset),
                                               RECOVER_FF_CODED_VALUE, FF_RECOVER_UNDEFINED});
                        break;
                    }
                    // Offset-bearing: the raw 8 bytes are an absolute child
                    // offset. Array-tagged tags reach here through their base
                    // kind — GetTypeFromTag strips the array bit before the
                    // mapping — and the array bit survives in `stored` for the
                    // walker. ARRAY/CHOICE/URL are never yielded by
                    // Recovery_to_Kind; they are listed anyway so -Wswitch stays
                    // live and a future widening of that mapping is triaged
                    // here instead of silently mis-reading a new inline form.
                    case FF_FIELD_STRING:
                    case FF_FIELD_BLOCK:
                    case FF_FIELD_RESOURCE:
                    case FF_FIELD_ARRAY:
                    case FF_FIELD_CHOICE:
                    case FF_FIELD_URL:
                        out.push_back(BlockRef{block_offset, static_cast<Offset>(f.field_offset), f.kind,
                                               static_cast<Offset>(raw), stored, FF_RECOVER_UNDEFINED});
                        break;
                    // An identity slot is 16 SELF-DESCRIBING bytes, not the bare
                    // 8-byte offset this group reads -- bytes 0-7 may be a trie
                    // index and a kind word, which read as an offset would be a
                    // plausible wrong answer. Recovery_to_Kind never yields it
                    // (no tag maps to it yet); the case exists so -Wswitch stays
                    // live, and when a tag does map to it the arm must be decoded
                    // via FF_Id::read_slot, never read as `raw`.
                    case FF_FIELD_ID:
                        continue;
                }
                break;  // end of the CHOICE/RESOURCE case
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
                // FF_CODED_VALUE fallback block (D5).
                if (slot + 4 > m_size)
                    continue;
                const uint32_t raw = LOAD_U32(m_base + slot);
                if (raw == FF_CODE_NULL || !(raw & FF_CODED_VALUE_FLAG))
                    continue;
                out.push_back(BlockRef{block_offset, static_cast<Offset>(f.field_offset), f.kind,
                                       FF_ResolveCodeableConceptOffset(raw, block_offset),
                                       RECOVER_FF_CODED_VALUE, FF_RECOVER_UNDEFINED});
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
    // HEADER_SIZE instead. Measured on one Synthea bundle: CODED_VALUE
    // x6593, URL_DIRECTORY x1, CHECKSUM x1 -- three tags cover every one.
    if (tag == RECOVER_FF_CODED_VALUE) {
        if (static_cast<size_t>(off) + FF_CODED_VALUE::HEADER_SIZE <= size) {
            const uint8_t len = FF_CODED_VALUE(off, size, 0).length(base);
            const uint64_t total = FF_CODED_VALUE::HEADER_SIZE + static_cast<uint64_t>(len);
            if (total <= size - static_cast<size_t>(off))
                return {StreamMapEntryType::Block, off, static_cast<Size>(total)};
        }
        return {StreamMapEntryType::Block, off, FF_CODED_VALUE::HEADER_SIZE};
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

// The walked extent of one array: how many elements the GEOMETRY can hold
// (fixed stride inside the buffer). Element validation is deliberately NOT
// part of this answer -- a damaged element is damage to one element, repaired
// by its own reference verdict, never the end of the array. Returns the
// stamped count when the shape makes geometry the only check, and the largest
// fitting index when the stamped count overruns the buffer (the
// anti-inflation bound: a corrupted count must not conjure elements).
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

    // The extent is a GEOMETRY question, never a validation score. Elements
    // sit at a fixed stride, so a failed element is damage to ONE element —
    // the reference verdicts repair it individually — not the end of the
    // array. Bailing at the first damaged entry read a mid-array flip as
    // truncation: one flipped VALIDATION word in entry 28 of a 1,473-entry
    // bundle array derived an extent of 28, ExtentDerived overwrote the
    // INTACT entry count (the flip was in the entry, not the count), and the
    // reparse lost 97% of the document (reproduced via bench recovery_probe:
    // the single 2-byte count rewrite 1473 -> 28 dropped the leaf census
    // 99% -> 3%). Only an element position the buffer cannot hold ends the
    // extent — that is the anti-inflation bound, and a count that overruns
    // the arena is the one case a derived extent may replace.
    for (uint32_t i = 0; i < stamped; ++i) {
        const uint64_t pos = entries + static_cast<uint64_t>(i) * step;
        if (pos + step > size)
            return i;
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
    //
    // The test names the compact layout exactly, because the field is two bits
    // wide and half its values are ones the writer never emits. Refusing on
    // "anything other than standard" meant one flipped bit in those two bits
    // disabled the whole hole analysis, so a damaged stream silently reported
    // no holes at all — a total loss of the evidence, caused by the damage the
    // evidence exists to find (REC-24).
    if (header_is_readable(m_base, m_size)) {
        if (FF_HEADER(m_size).get_stream_layout(m_base) == FF_STREAM_COMPACTED)
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
    const bool newer_stream = stream_is_newer(m_base, m_size);

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
    walk_chain(wire_root(), &out);
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

    // WHICH BYTES THIS CALL HAS ALREADY WRITTEN.
    //
    // Two verdicts planning the same seat is not a repair but a collision: the
    // second overwrites the first and BOTH verify, because each only re-reads
    // its own value. Nothing here can say which is right, so the second is
    // refused and counted failed -- the engine produced two incompatible plans
    // and that is the honest report of it.
    //
    // The header's 54 bytes start marked. They belong to the header pass below,
    // so a block verdict planning a write inside them is a collision by
    // definition rather than by coincidence.
    std::vector<bool> touched(n, false);
    for (std::size_t i = 0; i < static_cast<std::size_t>(FF_HEADER::HEADER_SIZE) && i < n; ++i)
        touched[i] = true;
    const auto collides = [&touched, n](const PlannedWrite& w) {
        for (uint64_t i = 0; i < w.width && w.seat + i < n; ++i)
            if (touched[static_cast<std::size_t>(w.seat + i)])
                return true;
        return false;
    };

    // REC-24 — THE HEADER FIRST, AND IT IS NOT SUBJECT TO THE FILTER.
    //
    // The ApplyFilter takes a BlockVerdict, so it has nothing to say about a
    // header field; passing header fields through a predicate that cannot see
    // them would silently apply or skip them on the strength of whichever
    // BlockVerdict happened to be fabricated for the call. A header repair is
    // the precondition for the stream opening at all, so a caller filtering
    // block repairs still wants it.
    //
    // Only Corroborated is written. Ambiguous carries alternatives the engine
    // declined to choose between, and writing one of them would convert a
    // declared uncertainty into a silent one — the same rule the block classes
    // follow, for the same reason.
    for (const HeaderVerdict& v : report.header) {
        if (v.class_ != RepairClass::Corroborated) {
            ++out.declined;
            continue;
        }
        Size     slot  = 0;
        unsigned width = 0;
        uint64_t value = v.restored;
        switch (v.field) {
            case HeaderField::Magic:        slot = FF_HEADER::MAGIC;         width = 4; break;
            case HeaderField::Recovery:     slot = FF_HEADER::RECOVERY;      width = 2; break;
            case HeaderField::FhirRevision: slot = FF_HEADER::FHIR_REV;      width = 2; break;
            case HeaderField::StreamSize:   slot = FF_HEADER::STREAM_SIZE;   width = 8; break;
            case HeaderField::RootOffset:   slot = FF_HEADER::ROOT_OFFSET;   width = 8; break;
            case HeaderField::RootRecovery: slot = FF_HEADER::ROOT_RECOVERY; width = 2; break;
            case HeaderField::ChecksumOffset:
                slot = FF_HEADER::CHECKSUM_OFFSET; width = 8; break;
            case HeaderField::UrlDirectoryOffset:
                slot = FF_HEADER::URL_DIR_OFFSET;  width = 8; break;
            case HeaderField::ModuleRegistryOffset:
                slot = FF_HEADER::MODULE_REG_OFFSET; width = 8; break;
            case HeaderField::StreamLayout:
                // The layout shares its 32-bit word with the engine version,
                // which this pass has no evidence about and must not disturb.
                // Re-encode rather than store: the two fields are packed.
                slot  = FF_HEADER::VERSION;
                width = 4;
                value = FF_ENCODE_HEADER_VERSION(
                    FF_HEADER_ENGINE_VERSION(LOAD_U32(dst + FF_HEADER::VERSION)),
                    static_cast<FF_StreamCompaction>(v.restored));
                break;
            case HeaderField::EngineVersion:
                // reconcile_header() never corroborates it, because nothing in
                // the arena witnesses it; a verdict that reaches here anyway
                // has nothing behind it to write.
                ++out.declined;
                continue;
        }
        if (!in_bounds(slot, width)) {
            ++out.declined;
            continue;
        }
        switch (width) {
            case 2: STORE_U16(dst + slot, static_cast<uint16_t>(value)); break;
            case 4: STORE_U32(dst + slot, static_cast<uint32_t>(value)); break;
            default: STORE_U64(dst + slot, value); break;
        }
        // Read it back from the copy, exactly as the block classes do. A write
        // that does not hold is not a repair.
        const uint64_t after = width == 2   ? LOAD_U16(dst + slot)
                               : width == 4 ? LOAD_U32(dst + slot)
                                            : LOAD_U64(dst + slot);
        if (after == value) {
            ++out.applied;
        } else {
            ++out.failed;
            out.failed_edges.push_back(static_cast<Offset>(slot));
        }
    }

    // ONE VERDICT, ONE GROUP OF WRITES, STAGED AND VERIFIED AS A UNIT.
    //
    // `before` is hoisted so the whole call allocates once; a repair plans at
    // most three writes, and there are tens of thousands of verdicts.
    std::vector<uint64_t> before;
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
        //
        // A confident verdict carrying an EMPTY plan is declined for the same
        // reason. apply() enacts a plan; it does not invent one from a label.
        if (!confident || v.writes.empty()) {
            ++out.declined;
            continue;
        }

        // ADMIT THE WHOLE GROUP OR NONE OF IT. A repair whose second write is
        // out of bounds must not leave its first one standing: a half-enacted
        // hypothesis is the state this whole path exists to make impossible.
        bool admissible = true;
        for (const PlannedWrite& w : v.writes)
            if (!in_bounds(w.seat, w.width) || collides(w))
                admissible = false;
        if (!admissible) {
            ++out.failed;
            out.failed_edges.push_back(v.block.child);
            continue;
        }

        before.clear();
        for (const PlannedWrite& w : v.writes) {
            before.push_back(w.width == 2   ? LOAD_U16(dst + w.seat)
                             : w.width == 4 ? LOAD_U32(dst + w.seat)
                                            : LOAD_U64(dst + w.seat));
            switch (w.width) {
                case 2:  STORE_U16(dst + w.seat, static_cast<uint16_t>(w.value)); break;
                case 4:  STORE_U32(dst + w.seat, static_cast<uint32_t>(w.value)); break;
                default: STORE_U64(dst + w.seat, w.value); break;
            }
        }

        if (verify_edge(dst, n, v)) {
            for (const PlannedWrite& w : v.writes)
                for (uint64_t i = 0; i < w.width && w.seat + i < n; ++i)
                    touched[static_cast<std::size_t>(w.seat + i)] = true;
            ++out.applied;
            continue;
        }
        // RESTORE EVERY BYTE OF THE GROUP. A write that does not verify is not
        // a repair, and leaving any part of one standing would make the copy
        // worse than the damaged original while reporting success -- the one
        // outcome this must never produce.
        for (std::size_t k = 0; k < v.writes.size(); ++k) {
            const PlannedWrite& w = v.writes[k];
            switch (w.width) {
                case 2:  STORE_U16(dst + w.seat, static_cast<uint16_t>(before[k])); break;
                case 4:  STORE_U32(dst + w.seat, static_cast<uint32_t>(before[k])); break;
                default: STORE_U64(dst + w.seat, before[k]); break;
            }
        }
        ++out.failed;
        out.failed_edges.push_back(v.block.child);
    }
    return out;
}

namespace {

// THE EXTENT, BOOTSTRAPPED FROM THE ONE IDENTITY THE HEADER CARRIES TWICE.
//
// `ceiling` is the furthest byte the arena itself vouches for, and nothing in
// the stream can influence it. Inside that ceiling there are two wire words
// that both claim to say where the payload ends:
//
//   STREAM_SIZE                              (FF_HEADER, bytes 8-15)
//   CHECKSUM_OFFSET + FF_CHECKSUM::HEADER_SIZE   (FF_HEADER, bytes 26-33)
//
// They agree on every sealed stream, because the checksum footer is the last
// block the writer lays down. That makes them two independent copies of one
// fact, so a flip in either is detectable, and the one that still lands on a
// block vouching for itself as a checksum footer is the one that survived.
//
// The test is decisive rather than merely suggestive: a candidate extent E is
// accepted only when the eight bytes at E - FF_CHECKSUM::HEADER_SIZE hold
// exactly that address and the two bytes after them read RECOVER_FF_CHECKSUM.
// Random bytes satisfy the address half with probability 2^-64.
//
// Without this, a single flipped bit in STREAM_SIZE set the extent to the
// arena's 4 GiB sparse reservation (an anonymous arena has no disk size to
// fall back on), and the byte census then swept 4 GiB looking for a 3 MB
// document. A flip the other way truncated the extent and hid every block past
// it. Neither failure could be repaired afterwards, because both happen before
// any of the repair machinery runs.
inline size_t trusted_extent(const BYTE* base, uint64_t claimed, uint64_t ceiling) noexcept {
    if (base != nullptr && ceiling >= FF_HEADER::HEADER_SIZE) {
        const auto seals_at = [base, ceiling](uint64_t end) {
            if (end < static_cast<uint64_t>(FF_HEADER::HEADER_SIZE) + FF_CHECKSUM::HEADER_SIZE ||
                end > ceiling)
                return false;
            const Offset seat = static_cast<Offset>(end - FF_CHECKSUM::HEADER_SIZE);
            return LOAD_U64(base + seat) == static_cast<uint64_t>(seat) &&
                   FF_GET_RECOVERY_TAG(base, seat) == RECOVER_FF_CHECKSUM;
        };
        if (seals_at(claimed))
            return static_cast<size_t>(claimed);
        const uint64_t implied =
            LOAD_U64(base + FF_HEADER::CHECKSUM_OFFSET) + FF_CHECKSUM::HEADER_SIZE;
        if (seals_at(implied))
            return static_cast<size_t>(implied);
    }
    // No checksum footer to corroborate either word — an unsealed stream has
    // none by design. Fall back to the ceiling, which is still never a wire value.
    return static_cast<size_t>(std::min(claimed, ceiling));
}

}  // namespace

Recovery::Recovery(const Memory& memory) noexcept
    : m_base(memory->base()),
      // THE DECLARED SIZE IS A WIRE VALUE, AND THIS CLASS TRUSTS NO WIRE VALUE.
      //
      // Memory_t::size() reads the arena's write head, and the head lives at
      // byte 8 of the arena (Memory_t::STREAM_CURSOR_OFFSET) -- the same 8 bytes
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
      //
      // That ceiling keeps every read inside mapped memory, and on an anonymous
      // arena it is 4 GiB wide, so it is a safety floor rather than an answer.
      // trusted_extent() narrows it to the payload by checking which of the
      // header's two claims about the end of the stream still lands on the
      // checksum footer (REC-24).
      m_size(trusted_extent(memory->base(), memory->size(),
                            memory->disk_size() != 0 ? memory->disk_size()
                                                     : memory->capacity())) {}

}  // namespace FastFHIR
