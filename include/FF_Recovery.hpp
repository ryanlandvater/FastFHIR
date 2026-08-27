/**
 * @file FF_Recovery.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * @version 0.1
 *
 * @brief FastFHIR Archive Recovery — StreamMap, block reconciliation, repair report
 *
 * Prefer including FastFHIR.hpp instead of this header directly.
 *
 * The recovery subsystem is the P0-3 two-sided reconciliation: every parent→child
 * block reference in the arena is encoded twice (the parent's slot {expected
 * RECOVERY_TAG, stored offset} and the child's block header {VALIDATION == own
 * offset, RECOVERY_TAG}), so a single-site corruption leaves the other half as
 * evidence. This header declares the archive-level machinery that turns that
 * redundancy into restored references:
 *
 *   StreamMap   — one map type, two producers, mirroring the Iris File
 *                 Extension's FileMap (generate_file_map / recover_file_structure):
 *                 reachable_blocks() walks intact parents (baseline/clean
 *                 producer); scan() is the byte-wise signature walk (recovery
 *                 producer).
 *   BlockRef    — a slot in a parent datablock that references a child
 *                 datablock, with both wire witnesses plus the compiled
 *                 expectation. The atom recovery counts (IN-G2).
 *   Recovery    — reachable_blocks() enumerates the offset-chain references
 *                 (clean-stream baseline); recover() reconciles both witnesses
 *                 of every reference on a damaged stream and reports blocks
 *                 restored per repair class, never silently.
 *
 * THREAT MODEL — bit-flip only (TASKS.md REC-17). The Hamming ranker assumes a
 * corrupted value stays within a small number of bit flips of the truth
 * (FF_RECOVERY_MAX_FLIPS). Truncation, memmove, or overwrite damage defeats it;
 * those cases fall back to type + reachability ranking and are reported as
 * ambiguous, never guessed. Integrity, not authenticity: a repaired stream is
 * not the same object as an intact one — every repair is reported, and apply()
 * is the only path that mutates.
 *
 * The "no second witness" boundary (TASKS.md P0-3): inline scalar slots, string
 * payload bytes, packed date/time with bit 63 clear, and both witnesses damaged
 * on the same reference are NOT covered by the redundancy. The checksum footer
 * proves *something* changed; it localizes nothing.
 */

#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "FF_Memory.hpp"
#include "FF_Primitives.hpp"
#include "FF_Reflection.hpp"
#include "FF_Utilities.hpp"

namespace FastFHIR {

/// The bit-flip budget of the Hamming ranker (TASKS.md REC-12). A candidate
/// whose repair costs more than this is not a hypothesis, it is a guess.
inline constexpr uint32_t FF_RECOVERY_MAX_FLIPS = 8;

/// Same 64 the compactor's MAX_NODE_DEPTH uses: both bound the same FHIR nesting
/// (src/FF_Compactor.cpp:98, src/FF_Parser.cpp:643). The root walk must not drift.
inline constexpr std::size_t FF_RECOVERY_MAX_DEPTH = 64;

// ---------------------------------------------------------------------------
// StreamMap — one map type, two producers (Iris FileMap correspondence,
// ../Iris-File-Extension/include/IrisFileExtension.hpp:393)
// ---------------------------------------------------------------------------

enum class StreamMapEntryType : uint8_t {
    Undefined = 0,
    Header,  // FF_HEADER — found by MAGIC, not by the VALIDATION word (byte 0
             // is the magic word, so a header can never pass the self-offset test)
    Block,   // DATA_BLOCK-shaped: VALIDATION == own offset
    Array,   // FF_ARRAY-shaped: header + ENTRY_COUNT x stride (size derivable)
    String,  // FF_STRING-shaped: header + stamped LENGTH (size derivable)
};

/// One located block in the arena. `size` is 0 for plain blocks, whose extent
/// is only knowable by walking them — the caller asks the block, as the Iris
/// FileMap contract does.
struct StreamMapEntry {
    StreamMapEntryType type   = StreamMapEntryType::Undefined;
    Offset             offset = FF_NULL_OFFSET;
    Size               size   = 0;
};

/// Every self-consistent block in the stream, keyed by offset. `upper_bound`
/// is the "what lives after this write offset" query an in-place repair needs
/// before overwriting anything (REC-15 apply(), Block C) — std::map provides it.
struct StreamMap : public std::map<Offset, StreamMapEntry> {
    Size file_size = 0;
};

// ---------------------------------------------------------------------------
// Block references — a slot in a parent datablock that references a child
// datablock: both wire witnesses, the compiled expectation, and the
// reconciliation verdict (P0-3 / IN-G2)
// ---------------------------------------------------------------------------

/// The wire facts of one parent→child block reference, read from the arena:
/// a slot inside the parent datablock `parent` pointing at the child datablock
/// `child`, whose type the slot declares. The unit recovery counts is the
/// block reference (IN-G2).
struct BlockRef {
    Offset       parent   = FF_NULL_OFFSET;        ///< referencing (parent) block
    Offset       field    = 0;                     ///< slot offset inside the parent
                                                   ///< (Offset, not uint16_t: array
                                                   ///< element slots can exceed 64K)
    FF_FieldKind kind     = FF_FIELD_UNKNOWN;      ///< V-Table kind of the slot
    Offset       child    = FF_NULL_OFFSET;        ///< referenced (child) block
    RECOVERY_TAG declared = FF_RECOVER_UNDEFINED;  ///< 1c compiled expectation,
                                                   ///< or the stored tag half for
                                                   ///< choice/resource slots (F1)
    RECOVERY_TAG actual   = FF_RECOVER_UNDEFINED;  ///< 2b from the child's header
};

/// The repair classes (TASKS.md REC-13). Corroborated is a search (mode 1);
/// TagRepaired / PositionRepaired are deterministic rewrites (mode 2); the
/// classes are reported separately because they carry different evidence.
enum class RepairClass : uint8_t {
    Intact = 0,        ///< both witnesses agreed on the wire — nothing to repair
    Corroborated,      ///< mode 1: parent offset corrupt, unique matching orphan
    TagRepaired,       ///< mode 2b: child tag rewritten from the parent's copy
    PositionRepaired,  ///< mode 2a: VALIDATION recomputed from the parent-named
                       ///< address — position-verified, content NOT verified
    ExtentDerived,     ///< array ENTRY_COUNT corrupt; extent recomputed by the
                       ///< element-tag-discriminated walk (F4)
    Ambiguous,         ///< ≥2 live readings at equal cost — reported, never guessed
    Unrecovered,       ///< no candidate within the flip budget
};

/// One block reference plus its verdict. `blocks` in the report carries every
/// reference so the benchmark's anchored check can verify recovered ⊆ baseline
/// over (parent, field, child, tag) — the unit that can audit attachment (F3).
struct BlockVerdict {
    BlockRef                block;
    RepairClass             class_    = RepairClass::Unrecovered;
    uint32_t                bit_cost  = 0;  ///< Hamming cost of the repair, 0 = intact
    std::vector<Offset>     candidates;    ///< populated for Ambiguous
};

/// The P0-3 reconciliation result. Counts are precomputed so a driver can
/// report "blocks recovered / total blocks" without re-walking the vectors.
struct FF_RecoveryReport {
    std::size_t blocks_total      = 0;
    std::size_t intact            = 0;
    std::size_t corroborated      = 0;
    std::size_t tag_repaired      = 0;
    std::size_t position_repaired = 0;
    std::size_t extent_derived    = 0;
    std::size_t ambiguous         = 0;
    std::size_t unrecovered       = 0;

    /// Every enumerated block reference, each with its verdict — the atom the
    /// benchmark fingerprint is built from (F3: parent identity makes
    /// misattachment fail the subset check).
    std::vector<BlockVerdict> blocks;
};

// ---------------------------------------------------------------------------
// Recovery — the scanner's view of untrusted bytes (Instrument G test 5)
// ---------------------------------------------------------------------------

/**
 * @brief Two-sided reconciliation of block references over arbitrary bytes.
 *
 * Constructing this never dereferences a header field (CAPI-15 was paid once;
 * it does not get paid again). Every read below is bounds-checked against the
 * buffer size. All methods are noexcept leaf helpers where they cannot fail.
 *
 * TWO ENTRY POINTS, ONE PURPOSE EACH — one offset-chain walk under both:
 *   reachable_blocks() — offset-chain walk only (reachable_blocks_map),
 *                        enumerated as BlockRefs. No scan, no classification.
 *                        This is the CLEAN-STREAM baseline path: what a
 *                        fingerprint enumerates when the caller vouches for
 *                        the bytes. Cost O(blocks), no census.
 *   recover()          — DAMAGED streams only: byte census (scan) and the
 *                        reachability walk in parallel, joined, then the
 *                        orphan test and two-sided reconciliation. A baseline
 *                        must never call this — the census is pure waste on
 *                        bytes that are known good.
 *
 * This is the replacement for the half-implementation that lived in
 * FF_Parser.cpp (REC-16): next_valid_resource_of() and the whole-stream
 * scan_all_resources() fallback are retired — one mechanism, not two.
 */
class Recovery {
public:
    /// Recovery requires the Memory arena. Unlike Parser it provides no
    /// read-only view of raw bytes — it is a scanner over the arena you
    /// suspect is damaged — so there is no (ptr, size) entry point. Never
    /// dereferences a header field at construction.
    explicit Recovery(const Memory& memory) noexcept;

    /// Byte-wise signature walk (mirrors Iris recover_file_structure). Finds
    /// every self-consistent block; the header is found by MAGIC, not VALIDATION.
    StreamMap scan() const;

    /// Offset-chain walk from the root through intact references (mirrors Iris
    /// generate_file_map): every reachable block, as a map. The reachability
    /// half of the orphan test, and the baseline producer.
    StreamMap reachable_blocks_map() const;

    /// The block references of every block reachable from the root via the
    /// offset chain. NO scan, NO classification: the clean-stream baseline path
    /// (benchmark calc_stream_hash). Shares the per-block enumerator with
    /// recover(), so recovered ⊆ baseline stays like-for-like.
    std::vector<BlockRef> reachable_blocks() const;

    /// The P0-3 reconciliation: enumerate every block reference, classify each
    /// against the two witnesses, and report blocks restored per class. The
    /// byte census (scan) and the offset-chain reachability walk run on two
    /// threads in parallel and are joined before the orphan test and
    /// classification — the two maps never touch each other's state. Worker
    /// exceptions are captured and rethrown on the calling thread, so a
    /// failing worker surfaces instead of std::terminate. Read-only; apply()
    /// (REC-15) is the only mutating entry point.
    FF_RecoveryReport recover() const;

    /// Hamming distance between two 64-bit values — the ranker's cost function.
    static uint32_t hamming_cost(uint64_t a, uint64_t b) noexcept;

    /// Loose band-membership test for a RECOVERY_TAG read from untrusted bytes:
    /// anything in the assigned bands (stripped of the array bit). A false
    /// positive costs a rejected candidate; a false negative costs a whole
    /// edge, so the filter errs permissive (REC-1's rationale).
    static bool plausible_tag(RECOVERY_TAG tag) noexcept;

private:
    bool         valid_validation(Offset off) const noexcept;
    RECOVERY_TAG tag_at(Offset off) const noexcept;
    bool         header_is_readable() const noexcept;

    /// Enumerate the block references of one block (V-Table slots + array
    /// elements), bounds-checked. Appends to `out`.
    void enumerate_block_refs(Offset block_offset, RECOVERY_TAG block_tag,
                              std::vector<BlockRef>& out) const;

    /// The ONE offset-chain walk: DFS from the root through intact references,
    /// depth- and cycle-bounded. Returns every reachable block offset (the
    /// orphan test's other half); when `out` is non-null, also appends each
    /// visited block's references (the clean-stream baseline enumeration).
    std::vector<Offset> walk_chain(std::vector<BlockRef>* out) const;

    const BYTE* m_base = nullptr;
    size_t      m_size = 0;
};

}  // namespace FastFHIR
