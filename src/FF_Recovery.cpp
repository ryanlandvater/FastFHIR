
/**
 * @file FF_Recovery.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * @version 0.1
 *
 * @brief FastFHIR Archive Recovery — implementation (TASKS.md REC-10…17)
 *
 * The two-sided reconciliation of P0-3: every parent→child block reference is
 * encoded twice (parent slot {expected tag, stored offset} + child header
 * {VALIDATION, RECOVERY}), and recover() turns the surviving half into a
 * restored reference, classified by which witness supplied the fix.
 *
 * Threat model: bit-flip only (REC-17). Every read is bounds-checked; nothing
 * here dereferences a header field at construction (CAPI-15).
 */

#include "FF_Recovery.hpp"

#include <bit>
#include <exception>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace FastFHIR {

namespace {

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
    Tuple,       // inline 10-byte {offset|tag} — +0 is the TARGET's offset (follow it)
    OffsetPtr,   // OFFSET table — 8-byte pointers to FF_STRING blocks (strings only)
};

ElementShape element_shape_of(RECOVERY_TAG element_tag, uint16_t kind_step) noexcept {
    // The top two bits are the wire truth for the physical layout (FF_ARRAY
    // EntryKind, already pre-shifted): OFFSET and SCALAR need no tag help.
    const uint16_t kind = kind_step & FF_ARRAY::KIND_MASK;
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

}  // namespace

// =====================================================================
// Construction + leaf helpers
// =====================================================================

Recovery::Recovery(const Memory& memory) noexcept
    : m_base(memory.base()), m_size(memory.size()) {}

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

bool Recovery::valid_validation(Offset off) const noexcept {
    if (off < 0 || static_cast<size_t>(off) > m_size ||
        m_size - static_cast<size_t>(off) < DATA_BLOCK::HEADER_SIZE)
        return false;
    return LOAD_U64(m_base + off) == static_cast<uint64_t>(off);
}

RECOVERY_TAG Recovery::tag_at(Offset off) const noexcept {
    if (off < 0 || static_cast<size_t>(off) > m_size ||
        m_size - static_cast<size_t>(off) < DATA_BLOCK::HEADER_SIZE)
        return FF_RECOVER_UNDEFINED;
    return static_cast<RECOVERY_TAG>(LOAD_U16(m_base + off + DATA_BLOCK::RECOVERY));
}

bool Recovery::header_is_readable() const noexcept {
    if (m_size < FF_HEADER::HEADER_SIZE)
        return false;
    return LOAD_U32(m_base + FF_HEADER::MAGIC) == FF_MAGIC_BYTES;
}

// =====================================================================
// StreamMap — the two producers
// =====================================================================

// Classify one self-consistent block start: which StreamMapEntryType it is and
// how big it is (0 = not knowable without a walk).
static StreamMapEntry classify_block(const BYTE* base, size_t size, Offset off) {
    const RECOVERY_TAG tag = static_cast<RECOVERY_TAG>(LOAD_U16(base + off + DATA_BLOCK::RECOVERY));
    if (IsArrayTagged(tag)) {
        // size = 16 + count x stride, when the header fits and the math lands
        // inside the arena; otherwise the header itself is suspect.
        if (static_cast<size_t>(off) + FF_ARRAY::HEADER_SIZE <= size) {
            const uint16_t kind_step = LOAD_U16(base + off + FF_ARRAY::KIND_AND_STEP);
            const uint32_t count     = LOAD_U32(base + off + FF_ARRAY::ENTRY_COUNT);
            const uint64_t stride    = kind_step & FF_ARRAY::STEP_MASK;
            const uint64_t total     = FF_ARRAY::HEADER_SIZE + static_cast<uint64_t>(count) * stride;
            if (total <= size - static_cast<size_t>(off))
                return {StreamMapEntryType::Array, off, static_cast<Size>(total)};
        }
        return {StreamMapEntryType::Array, off, FF_ARRAY::HEADER_SIZE};
    }
    if (FF_IsStringLayoutTag(tag)) {
        // FF_STRING stamps its LENGTH at +10; size = STRING_DATA + LENGTH.
        if (static_cast<size_t>(off) + FF_STRING::STRING_DATA <= size) {
            const uint32_t len = LOAD_U32(base + off + FF_STRING::LENGTH);
            if (static_cast<uint64_t>(len) <= size - static_cast<size_t>(off) - FF_STRING::STRING_DATA)
                return {StreamMapEntryType::String, off, static_cast<Size>(FF_STRING::STRING_DATA + len)};
        }
        return {StreamMapEntryType::String, off, 0};
    }
    return {StreamMapEntryType::Block, off, 0};
}

StreamMap Recovery::scan() const {
    StreamMap map;
    map.file_size = static_cast<Size>(m_size);
    if (header_is_readable())
        map[0] = {StreamMapEntryType::Header, 0, FF_HEADER::HEADER_SIZE};

    // Byte-wise, not block-wise: the arena is not aligned to block boundaries
    // and a block can start anywhere the bump allocator left it. A u64 equal to
    // its own position has a false-positive rate of ~2^-64 per position, so the
    // VALIDATION word alone is a reliable block detector.
    for (Offset off = 0; static_cast<size_t>(off) + DATA_BLOCK::HEADER_SIZE <= m_size; ++off) {
        if (LOAD_U64(m_base + off) != static_cast<uint64_t>(off))
            continue;
        if (map.contains(off))
            continue;  // the header (and any overlap) is already claimed
        map[off] = classify_block(m_base, m_size, off);
    }
    return map;
}

// =====================================================================
// Block reference enumeration
// =====================================================================

// The walked extent of one array: how many elements actually validate. Returns
// the stamped count when the shape makes verification impossible.
static uint32_t walk_array_extent(const BYTE* base, size_t size, Offset array_off,
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
            if (!Recovery::plausible_tag(static_cast<RECOVERY_TAG>(LOAD_U16(base + pos + DATA_BLOCK::RECOVERY))))
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
                const RECOVERY_TAG stored = static_cast<RECOVERY_TAG>(LOAD_U16(m_base + slot + DATA_BLOCK::RECOVERY));
                const FF_FieldKind variant_kind = Recovery_to_Kind(stored);
                if (variant_kind == FF_FIELD_UNKNOWN) {
                    // Tag half is garbage — record with no declared type so
                    // classification reports it as undecidable, never guessed.
                    out.push_back(BlockRef{block_offset, static_cast<Offset>(f.field_offset), f.kind,
                                           static_cast<Offset>(raw), FF_RECOVER_UNDEFINED,
                                           FF_RECOVER_UNDEFINED});
                    break;
                }
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
                // Element references — only when the array header is self-consistent.
                if (!valid_validation(array_off))
                    continue;
                if (static_cast<size_t>(array_off) + FF_ARRAY::HEADER_SIZE > m_size)
                    continue;
                const RECOVERY_TAG ah_tag = tag_at(array_off);
                const RECOVERY_TAG element_tag = GetTypeFromTag(ah_tag);
                const uint16_t kind_step = LOAD_U16(m_base + array_off + FF_ARRAY::KIND_AND_STEP);
                const uint16_t stride = static_cast<uint16_t>(kind_step & FF_ARRAY::STEP_MASK);
                const uint32_t stamped = LOAD_U32(m_base + array_off + FF_ARRAY::ENTRY_COUNT);
                const ElementShape shape = element_shape_of(element_tag, kind_step);
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
                        const RECOVERY_TAG stored = static_cast<RECOVERY_TAG>(LOAD_U16(m_base + pos + DATA_BLOCK::RECOVERY));
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
// Reachability — the orphan test's other half (REC-11)
// =====================================================================

std::vector<Offset> Recovery::walk_chain(std::vector<BlockRef>* out) const {
    std::vector<Offset> reachable;
    if (!header_is_readable())
        return reachable;
    const Offset root = static_cast<Offset>(LOAD_U64(m_base + FF_HEADER::ROOT_OFFSET));
    if (!valid_validation(root))
        return reachable;
    const RECOVERY_TAG root_tag = tag_at(root);
    if (!plausible_tag(root_tag))
        return reachable;

    // DFS through INTACT references only, depth- and cycle-bounded. Marking on
    // completion is unnecessary here (the verdict of a reference does not
    // depend on how it was reached), so a simple visited set bounds the work.
    std::unordered_set<Offset> visited;
    std::vector<std::pair<Offset, std::size_t>> stack{{root, 0}};
    std::vector<BlockRef> scratch;
    while (!stack.empty()) {
        const auto [off, depth] = stack.back();
        stack.pop_back();
        if (!visited.insert(off).second)
            continue;
        reachable.push_back(off);

        // One enumeration per block feeds both the walk (child discovery) and,
        // when requested, the baseline output — no second pass.
        scratch.clear();
        enumerate_block_refs(off, tag_at(off), scratch);
        if (out)
            out->insert(out->end(), scratch.begin(), scratch.end());
        if (depth >= FF_RECOVERY_MAX_DEPTH)
            continue;

        for (const BlockRef& r : scratch) {
            if (r.child == FF_NULL_OFFSET || !valid_validation(r.child))
                continue;  // damaged references do not extend the walk
            const RECOVERY_TAG actual = tag_at(r.child);
            if (r.declared != FF_RECOVER_UNDEFINED && actual != r.declared)
                continue;  // tag disagreement — not an intact reference (REC-5)
            stack.emplace_back(r.child, depth + 1);
        }
    }
    return reachable;
}

StreamMap Recovery::reachable_blocks_map() const {
    StreamMap map;
    map.file_size = static_cast<Size>(m_size);
    if (header_is_readable())
        map[0] = {StreamMapEntryType::Header, 0, FF_HEADER::HEADER_SIZE};
    for (const Offset off : walk_chain(nullptr)) {
        if (map.contains(off))
            continue;
        map[off] = classify_block(m_base, m_size, off);
    }
    return map;
}

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

// =====================================================================
// recover — the P0-3 reconciliation
// =====================================================================

FF_RecoveryReport Recovery::recover() const {
    FF_RecoveryReport rep;

    // The two maps in parallel, joined before reconciliation: the byte census
    // (scan) and the offset-chain reachability walk share no state — scan()
    // follows no pointers, walk_chain() scans no bytes — so a damaged stream
    // pays both costs concurrently. An uncaught exception in a std::thread
    // would std::terminate the process, so each worker captures its failure
    // and it is rethrown on the calling thread after join. Specific catch, not
    // `...`: anything that is not a std::exception is a bug worth crashing on.
    StreamMap map;
    std::vector<Offset> reachable;
    std::exception_ptr census_error;
    std::exception_ptr chain_error;
    std::thread census([this, &map, &census_error] {
        try {
            map = scan();
        } catch (const std::exception&) {
            census_error = std::current_exception();
        }
    });
    std::thread chain([this, &reachable, &chain_error] {
        try {
            reachable = walk_chain(nullptr);
        } catch (const std::exception&) {
            chain_error = std::current_exception();
        }
    });
    census.join();
    chain.join();
    if (census_error)
        std::rethrow_exception(census_error);
    if (chain_error)
        std::rethrow_exception(chain_error);

    // Orphan set: self-consistent ∧ ¬reachable (REC-11), bucketed by tag so a
    // corrupt slot's search is one bucket lookup, not a per-reference arena
    // sweep. The array bit is STRIPPED: an array block must land in the bucket
    // of its element type, which is what the slot declares (an entry-array slot
    // declaring BUNDLE_ENTRY must find its orphaned array, not just loose
    // entry blocks).
    std::unordered_set<Offset> reached(reachable.begin(), reachable.end());
    std::unordered_map<RECOVERY_TAG, std::vector<Offset>> orphans;
    for (const auto& [off, entry] : map) {
        if (entry.type == StreamMapEntryType::Header)
            continue;
        if (reached.contains(off))
            continue;
        const RECOVERY_TAG t = GetTypeFromTag(tag_at(off));
        if (plausible_tag(t))
            orphans[t].push_back(off);
    }

    // Enumerate every reference of every self-consistent block — orphaned
    // parents included, because their outgoing references are still real.
    std::vector<BlockRef> all;
    for (const auto& [off, entry] : map) {
        if (entry.type == StreamMapEntryType::Header)
            continue;
        enumerate_block_refs(off, tag_at(off), all);
    }

    rep.blocks.reserve(all.size());
    for (const BlockRef& r : all) {
        BlockVerdict v;
        v.block = r;

        if (r.declared == FF_RECOVER_UNDEFINED) {
            // Choice/resource slot with BOTH halves corrupt — undecidable
            // without a type to match. Never guessed (P0-3).
            v.class_ = (valid_validation(r.child) && plausible_tag(tag_at(r.child)))
                           ? RepairClass::Ambiguous
                           : RepairClass::Unrecovered;
        } else {
            // The block at the parent-named address is self-consistent AND
            // its tag corroborates the slot — nothing to repair. (The
            // VALIDATION guard is load-bearing: a broken VALIDATION with an
            // intact tag must NOT read as Intact.)
            const RECOVERY_TAG actual = tag_at(r.child);
            // Array children carry the array bit; the slot declares the element
            // type — compare the stripped forms.
            const RECOVERY_TAG actual_base =
                (r.kind == FF_FIELD_ARRAY) ? GetTypeFromTag(actual) : actual;
            if (valid_validation(r.child) && actual_base == r.declared) {
                v.class_ = RepairClass::Intact;
                // Array extent: the stamped ENTRY_COUNT has no second witness
                // (F4), but the element walk derives it — a disagreement means
                // the count is the damaged half.
                if (r.kind == FF_FIELD_ARRAY &&
                    static_cast<size_t>(r.child) + FF_ARRAY::HEADER_SIZE <= m_size) {
                    const uint16_t kind_step = LOAD_U16(m_base + r.child + FF_ARRAY::KIND_AND_STEP);
                    const uint32_t stamped = LOAD_U32(m_base + r.child + FF_ARRAY::ENTRY_COUNT);
                    const RECOVERY_TAG element_tag = GetTypeFromTag(actual);
                    const ElementShape shape = element_shape_of(element_tag, kind_step);
                    if (shape != ElementShape::None) {
                        const uint32_t walked = walk_array_extent(
                            m_base, m_size, r.child, shape,
                            static_cast<uint16_t>(kind_step & FF_ARRAY::STEP_MASK), stamped);
                        if (walked != stamped) {
                            v.class_ = RepairClass::ExtentDerived;
                            v.bit_cost = hamming_cost(walked, stamped);
                        }
                    }
                }
            } else {
                // Damaged reference: rank the repair hypotheses under the flip
                // budget (D1/D2). H_inplace repairs the child in place — a tag
                // mismatch (child validates) costs the tag distance; a broken
                // VALIDATION costs the self-offset distance, only when the
                // target is otherwise coherent as the declared type. H_off
                // repoints to a unique unclaimed orphan of the declared type.
                // Cheapest under budget wins; equal costs are ambiguous — never
                // guessed. The repoint hypothesis was previously never computed
                // on the child-validates path, so a 1-bit offset flip onto an
                // innocent valid block was misrepaired as a tag rewrite.
                uint32_t h_inplace = UINT32_MAX;
                if (valid_validation(r.child)) {
                    h_inplace = hamming_cost(actual_base, r.declared);
                } else {
                    const bool addressable = r.child >= 0 &&
                                             static_cast<size_t>(r.child) + DATA_BLOCK::HEADER_SIZE <= m_size;
                    const RECOVERY_TAG target_tag = addressable ? tag_at(r.child) : FF_RECOVER_UNDEFINED;
                    const RECOVERY_TAG target_base =
                        (r.kind == FF_FIELD_ARRAY) ? GetTypeFromTag(target_tag) : target_tag;
                    const bool coherent = IsTupleKind(r.kind) ? plausible_tag(target_tag)
                                                              : target_base == r.declared;
                    if (addressable && coherent)
                        h_inplace = hamming_cost(LOAD_U64(m_base + r.child), static_cast<uint64_t>(r.child));
                }

                // The repoint cost: cheapest unique orphan (mode 1), if any.
                uint32_t h_off = UINT32_MAX;
                bool off_unique = false;
                if (r.declared != FF_RECOVER_UNDEFINED) {
                    const auto it = orphans.find(r.declared);
                    if (it != orphans.end()) {
                        for (const Offset p : it->second) {
                            const uint32_t c = hamming_cost(static_cast<uint64_t>(r.child),
                                                            static_cast<uint64_t>(p));
                            if (c < h_off) {
                                h_off = c;
                                off_unique = true;
                                v.candidates.clear();
                                v.candidates.push_back(p);
                            } else if (c == h_off) {
                                off_unique = false;
                                v.candidates.push_back(p);
                            }
                        }
                    }
                }

                const bool in_place = h_inplace <= FF_RECOVERY_MAX_FLIPS;
                const bool in_off = off_unique && h_off <= FF_RECOVERY_MAX_FLIPS;
                if (in_place && in_off && h_inplace == h_off) {
                    v.class_ = RepairClass::Ambiguous;  // tie — never guess
                    v.bit_cost = h_inplace;
                } else if (in_place && (!in_off || h_inplace < h_off)) {
                    v.class_ = valid_validation(r.child) ? RepairClass::TagRepaired
                                                         : RepairClass::PositionRepaired;
                    v.bit_cost = h_inplace;
                } else if (in_off) {
                    v.class_ = RepairClass::Corroborated;
                    v.bit_cost = h_off;
                } else {
                    v.class_ = RepairClass::Unrecovered;
                    v.bit_cost = (h_off != UINT32_MAX && h_off < h_inplace) ? h_off : h_inplace;
                }
            }
        }

        switch (v.class_) {
            case RepairClass::Intact:            ++rep.intact; break;
            case RepairClass::Corroborated:      ++rep.corroborated; break;
            case RepairClass::TagRepaired:       ++rep.tag_repaired; break;
            case RepairClass::PositionRepaired:  ++rep.position_repaired; break;
            case RepairClass::ExtentDerived:     ++rep.extent_derived; break;
            case RepairClass::Ambiguous:         ++rep.ambiguous; break;
            case RepairClass::Unrecovered:       ++rep.unrecovered; break;
        }
        rep.blocks.push_back(std::move(v));
    }
    rep.blocks_total = rep.blocks.size();
    return rep;
}

}  // namespace FastFHIR
