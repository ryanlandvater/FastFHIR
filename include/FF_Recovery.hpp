/**
 * @file FF_Recovery.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * @version 0.2
 *
 * @brief FastFHIR Archive Recovery — census, branch solver, repair report
 *
 * Prefer including FastFHIR.hpp instead of this header directly.
 *
 * Every parent→child link in a stream is witnessed more than once: the parent's
 * pointer, the child's self-offset (its VALIDATION word), the child's tag, and
 * for a {offset, tag} tuple the parent's own copy of that tag. A bit flip
 * damages one witness and leaves the others standing. Recovery compares every
 * witness with the value a hypothesis says it should hold, ranks whole
 * explanations of a damaged branch against each other, and repairs a branch
 * only when one explanation clearly beats every other. The design of record is
 * recovery_algorithm_handoff.md.
 *
 * THREAT MODEL — bit flips only (TASKS.md REC-17). Nothing is inserted,
 * deleted or moved. Integrity, not authenticity: every repair is reported, and
 * apply() is the only path that writes, into a copy.
 *
 * The "no second witness" boundary (TASKS.md P0-3): inline scalars, string
 * payload bytes and a packed date/time are covered by no redundancy. The
 * checksum footer proves something changed; it localizes nothing.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "FF_Memory.hpp"
#include "FF_Primitives.hpp"
#include "FF_Reflection.hpp"
#include "FF_Utilities.hpp"

namespace FastFHIR {

/// No single witness is repaired by more bits than this. A candidate further
/// from its witnesses is a guess, and recovery does not guess.
inline constexpr uint32_t FF_RECOVERY_MAX_FLIPS = 8;

/// The FHIR nesting bound, the same 64 the compactor and the parser use.
inline constexpr std::size_t FF_RECOVERY_MAX_DEPTH = 64;

// ---------------------------------------------------------------------------
// StreamMap — the located blocks of an arena
// ---------------------------------------------------------------------------

enum class StreamMapEntryType : uint8_t {
    Undefined = 0,
    Header,  ///< FF_HEADER, found by MAGIC. It has no VALIDATION word.
    Block,   ///< a data block: VALIDATION == its own offset
    Array,   ///< an FF_ARRAY: header plus entries
    String,  ///< an FF_STRING-layout block: header plus stamped LENGTH
};

/// One located block and the extent it is charged when the arena is tiled.
struct StreamMapEntry {
    StreamMapEntryType type     = StreamMapEntryType::Undefined;
    Offset             offset   = FF_NULL_OFFSET;
    Size               size     = 0;
    /// The tag on the wire. A single witness, which may itself be the damage.
    RECOVERY_TAG       recovery = FF_RECOVER_UNDEFINED;
};

/// Why a run of bytes belongs to no entry (REC-18.4).
enum class GapClass : uint8_t {
    Hole = 0,     ///< unattributed and large enough to have held a block: a
                  ///< block whose self-offset was damaged leaves exactly this
    VersionSkew,  ///< benign: a newer writer's V-Table is longer than this
                  ///< reader's, so every block of the tag trails the same run
    Trailing,     ///< after the last entry: arena slack
};

/// One run of bytes no map entry claims.
struct Gap {
    Offset       start  = FF_NULL_OFFSET;
    Size         length = 0;
    RECOVERY_TAG after  = FF_RECOVER_UNDEFINED;  ///< tag of the entry it trails
    GapClass     class_ = GapClass::Hole;
    const char*  why    = "";
};

/// A doubt the byte census recorded about a block it found.
enum class ProducerFailureKind : uint8_t {
    ScanTagInvalid,  ///< the self-offset holds, but the tag is no type this build knows
};

struct ProducerFailure {
    ProducerFailureKind kind     = ProducerFailureKind::ScanTagInvalid;
    Offset              at       = FF_NULL_OFFSET;
    RECOVERY_TAG        expected = FF_RECOVER_UNDEFINED;
    RECOVERY_TAG        actual   = FF_RECOVER_UNDEFINED;
    const char*         why      = "";
};

/// Every self-validating block in the arena, keyed by offset, and the runs of
/// bytes none of them covers.
struct StreamMap : public std::map<Offset, StreamMapEntry> {
    Size                         file_size = 0;
    std::vector<Gap>             gaps;
    std::vector<ProducerFailure> failures;
};

// ---------------------------------------------------------------------------
// References and verdicts
// ---------------------------------------------------------------------------

/// One parent→child reference: the slot at `parent + field` names `child`.
struct BlockRef {
    Offset       parent   = FF_NULL_OFFSET;
    Offset       field    = 0;                     ///< the slot's offset inside the parent
    FF_FieldKind kind     = FF_FIELD_UNKNOWN;      ///< the slot's kind
    Offset       child    = FF_NULL_OFFSET;
    RECOVERY_TAG declared = FF_RECOVER_UNDEFINED;  ///< the tag the child must carry on the wire
    RECOVERY_TAG actual   = FF_RECOVER_UNDEFINED;  ///< the tag the child carries now
};

/// What a verdict says about a reference. The first five are named by the
/// writes the repair plans; the last two are the engine declining to choose.
enum class RepairClass : uint8_t {
    Intact = 0,        ///< every witness holds; nothing is written
    Corroborated,      ///< the parent's pointer is rewritten
    TagRepaired,       ///< only copies of the child's type are rewritten
    PositionRepaired,  ///< the child's self-offset is rewritten
    ExtentDerived,     ///< an array's stamped geometry is rewritten
    Ambiguous,         ///< explanations exist, and none clearly beats the next
    Unrecovered,       ///< nothing admissible explains the slot
};

/// One byte-level write a repair needs.
struct PlannedWrite {
    Offset   seat  = FF_NULL_OFFSET;  ///< absolute byte offset in the stream
    uint8_t  width = 0;               ///< 2, 4 or 8
    uint64_t value = 0;
};

/// One reference and what recovery decided about it. apply() enacts `writes`
/// as one unit and nothing else: the class is a label for the report.
struct BlockVerdict {
    BlockRef                  block;
    RepairClass               class_         = RepairClass::Unrecovered;
    uint32_t                  bit_cost       = 0;  ///< bits the writes change
    uint32_t                  derived_extent = 0;  ///< ExtentDerived: the entry count written
    std::vector<Offset>       candidates;          ///< Ambiguous: the blocks still in contention
    std::vector<PlannedWrite> writes;              ///< empty unless the verdict is a repair
};

// ---------------------------------------------------------------------------
// The FF_HEADER
// ---------------------------------------------------------------------------

/// Which FF_HEADER field a verdict concerns. The header has no self-offset,
/// so each field is checked against a witness elsewhere: a compiled constant
/// (MAGIC, RECOVERY), a closed set (the FHIR revision, the stream layout), an
/// identity with another field (STREAM_SIZE with the checksum footer), or the
/// block the field names (the root and the three metadata offsets, which the
/// census reads as ordinary slots).
enum class HeaderField : uint8_t {
    Magic = 0,
    Recovery,
    FhirRevision,
    StreamSize,
    RootOffset,
    RootRecovery,
    ChecksumOffset,
    UrlDirectoryOffset,
    ModuleRegistryOffset,
    StreamLayout,
    /// The 30-bit engine version beside the layout bits. It has no second
    /// witness: a newer writer is legitimate, and find_gaps() relies on being
    /// able to see one. So it is reported and never written.
    EngineVersion,
};

/// One header field and what the evidence says about it. Pointer fields are
/// written by their references' verdicts; this records the outcome.
struct HeaderVerdict {
    HeaderField           field    = HeaderField::Magic;
    RepairClass           class_   = RepairClass::Unrecovered;
    uint64_t              stored   = 0;
    uint64_t              restored = 0;
    uint32_t              bit_cost = 0;
    const char*           why      = "";
    std::vector<uint64_t> candidates;  ///< Ambiguous: the values still supported
};

/// Everything recover() found and decided.
struct FF_RecoveryReport {
    std::size_t blocks_total      = 0;
    std::size_t intact            = 0;
    std::size_t corroborated      = 0;
    std::size_t tag_repaired      = 0;
    std::size_t position_repaired = 0;
    std::size_t extent_derived    = 0;
    std::size_t ambiguous         = 0;
    std::size_t unrecovered       = 0;

    /// One verdict per reference the FF_HEADER can reach, by seat.
    std::vector<BlockVerdict> blocks;

    /// One verdict per FF_HEADER field, in HeaderField order.
    std::vector<HeaderVerdict> header;
    std::size_t                header_repaired = 0;

    std::vector<ProducerFailure> failures;  ///< the byte census's doubts

    /// Runs of bytes no block covers. `holes` counts the ones that mean damage.
    std::size_t      holes        = 0;
    std::size_t      version_skew = 0;
    std::vector<Gap> gaps;
};

/// What apply() did.
struct FF_ApplyReport {
    std::size_t applied  = 0;  ///< written and verified
    std::size_t declined = 0;  ///< not selected, or not a repair
    std::size_t failed   = 0;  ///< written, did not verify, rolled back
    std::vector<Offset> failed_edges;
};

/// Which verdicts apply() enacts. Null selects every repair; Ambiguous and
/// Unrecovered plan no writes and are never enacted.
using ApplyFilter = std::function<bool(const BlockVerdict&)>;

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

/**
 * @brief Recovery over the bytes of an arena that may be damaged.
 *
 * Construction reads nothing but the trusted extent. Every read is bounded by
 * it, and nothing but apply() writes, and apply() writes into a copy.
 */
class Recovery {
public:
    explicit Recovery(const Memory& memory) noexcept;

    /// Diagnose: the census, then the task loop, then the report. Read-only.
    FF_RecoveryReport recover() const;

    /**
     * @brief Enact a report's repairs into a COPY of the stream.
     *
     * Each verdict's writes land as one group: none may touch a byte an
     * earlier group wrote, and the group must leave its link holding (every
     * write reads back, the child vouches for its own offset and carries the
     * type the link names). A group that fails is rolled back byte for byte.
     */
    FF_ApplyReport apply(const FF_RecoveryReport& report, std::vector<BYTE>& repaired,
                         const ApplyFilter& filter = nullptr) const;

    /// Every reference the FF_HEADER reaches through links whose witnesses all
    /// hold. On a clean stream that is every reference in it.
    std::vector<BlockRef> reachable_blocks() const;

    /// The blocks those references reach, as a map, with their gaps.
    StreamMap reachable_blocks_map() const;

    /// The byte census: every position holding its own offset.
    StreamMap scan() const;

    /// Tile `map` and record every run of bytes no entry covers.
    void find_gaps(StreamMap& map) const;

    /// Hamming distance, the unit every witness is scored in.
    static uint32_t hamming_cost(uint64_t a, uint64_t b) noexcept;

    /// Loose test for a tag read from untrusted bytes: does it map to a kind.
    static bool plausible_tag(RECOVERY_TAG tag) noexcept;

    /// A generated block's extent under this build's reflection table.
    static Size derived_block_size(RECOVERY_TAG tag) noexcept;

    // -----------------------------------------------------------------------
    // The census (recovery_algorithm_handoff.md §6), public so it can be
    // observed on its own: a clean stream is one attached island with no open
    // points, and one flipped bit opens exactly the point it damaged.
    // -----------------------------------------------------------------------

    /// How a slot's word names its child.
    enum class SlotRepr : uint8_t {
        Absolute,     ///< an 8-byte absolute offset
        Tuple,        ///< a 10-byte {value, tag}; the FF_HEADER's root is one too
        Relative32,   ///< FF_FIELD_CODE: bit 31 set, an offset relative to the containing block
        Relative63,   ///< FF_FIELD_DATETIME: bit 63 set, an offset relative to the containing block
        InlineEntry,  ///< an array entry that is itself a block, placed by the array's geometry
    };

    /// One place where a parent names, or may name, a child.
    struct Slot {
        Offset              parent     = FF_NULL_OFFSET;  ///< the owning block; 0 for the FF_HEADER
        Offset              seat       = FF_NULL_OFFSET;  ///< absolute position of the slot's first byte
        FF_FieldKind        kind       = FF_FIELD_UNKNOWN;
        SlotRepr            repr       = SlotRepr::Absolute;
        uint64_t            stored     = 0;               ///< the word as it stands on the wire
        RECOVERY_TAG        stored_tag = FF_RECOVER_UNDEFINED;  ///< Tuple: the tag half
        RECOVERY_TAG        expect     = FF_RECOVER_UNDEFINED;  ///< the compiled child type (ARRAY: the element)
        const FF_FieldInfo* field      = nullptr;         ///< the reflected field, when the slot is one
    };

    /// A link whose every witness holds.
    struct Edge {
        Slot   slot;
        Offset child = FF_NULL_OFFSET;
    };

    /// Blocks joined by such links. The FF_HEADER's island is attached from
    /// the start; every other island is an orphaned subtree.
    struct Island {
        Offset              root     = FF_NULL_OFFSET;  ///< 0 for the FF_HEADER's island
        bool                attached = false;
        std::vector<Offset> members;                    ///< the root first, then depth first
    };

    enum class PointKind : uint8_t {
        Open,         ///< a reachable slot whose child is not established
        ArrayExtent,  ///< a reachable array whose stamped geometry the bytes around it contradict
    };

    struct Point {
        PointKind kind  = PointKind::Open;
        Slot      slot;                    ///< Open
        Offset    array = FF_NULL_OFFSET;  ///< ArrayExtent
    };

    struct Census {
        Size                extent  = 0;
        std::size_t         anchors = 0;  ///< self-validating blocks, the FF_HEADER excluded
        std::vector<Edge>   edges;        ///< every link whose witnesses hold, by seat
        std::vector<Island> islands;      ///< the FF_HEADER's first, then by root offset
        std::vector<Gap>    holes;
        std::vector<Point>  points;       ///< the open questions, by seat
    };

    /// Run the census alone. Read-only.
    Census census() const;

private:
    const BYTE* m_base = nullptr;
    size_t      m_size = 0;
};

}  // namespace FastFHIR
