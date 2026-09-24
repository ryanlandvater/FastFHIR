/**
 * @file FF_Recovery.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * @version 0.2
 *
 * @brief FastFHIR Archive Recovery — census, branch solver, transactional apply (REC-25)
 *
 * recovery_algorithm_handoff.md is the design of record. In brief:
 *
 *   CENSUS    Every self-validating block and every slot of it are read, and
 *             the FF_HEADER's root and metadata offsets are read as slots too.
 *             A link whose every witness holds is decided on the spot; every
 *             other slot the header can reach becomes an open question.
 *   CYCLES    Each question's branch is ranked whole: every block its slot
 *             could name, read as every type it could be, down to the leaves.
 *             A branch is decided, in RAM, only when its best explanation
 *             leads the next valid one by ACCEPT_MARGIN bits. Decisions shrink
 *             the orphan pools, so later questions get easier, and cycles run
 *             until one decides nothing. A decision is never revisited.
 *   APPLY     Every decided link is planned as the writes it needs, and each
 *             plan is enacted into a copy as one group that lands whole or
 *             not at all.
 *
 * THE MODEL. Every witness is compared with the value a hypothesis says it
 * should hold, never with another witness. A witness that matches is worth its
 * width in bits -- a pointer or self-offset 64, a four-byte code pointer 32, a
 * tag 16 -- less C_FLIP for each bit it is off by. A link is worth its
 * witnesses plus the best explanation of its child's own slots, and a slot
 * nothing explains costs U_UNEXPLAINED. The cheapest configuration of a branch
 * is the explanation of the bytes that needs the least coincidence, and more
 * corroborating children always outweigh fewer.
 *
 * HARD CONSTRAINTS eliminate a hypothesis; everything else only ranks it.
 * H1 a tuple holds only a type its field allows. H2 a proposed position lies
 * inside the trusted extent. H3 a data block's V-Table never reaches the next
 * block. H5 every block has one parent.
 *
 * LAYOUT CONTRACT (REC-19.1): the file reads top to bottom as the call stack.
 */

#include "FF_Recovery.hpp"
#include "FF_Ops.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace FastFHIR {

// =====================================================================
// TOP — policy, leaf reads, and the board
// =====================================================================
namespace {

// The evidence weights are properties of the wire; the thresholds are
// calibrated against the gates (recovery_algorithm_handoff.md §5.1).
constexpr Offset   HEADER_PARENT = 0;           // the FF_HEADER parents the root and the metadata blocks
constexpr uint32_t TAG_RADIUS    = 2;           // how far a type hypothesis may sit from a wire copy of it
constexpr uint32_t SELF_RADIUS   = 2;           // how far a word may sit from a value it must hold (its own
                                                // address, or null) and still be read as that value
constexpr double   MAX_CHANCE    = 1.0 / 64;  // coincidental candidates a pointer search may expect to admit
constexpr int64_t  C_FLIP        = 8;           // the evidence one disagreeing bit costs
constexpr int64_t  U_UNEXPLAINED = 64;          // the cost of a slot nothing explains
constexpr int64_t  ACCEPT_ABS    = 32;          // every decided link must be worth at least this
constexpr int64_t  ACCEPT_MARGIN = 64;          // the best configuration's lead over the next valid one
constexpr size_t   MAX_COMPONENT = 8;           // branches solved together
constexpr size_t   K_BEST        = 256;         // configurations a node keeps
constexpr uint32_t UNEXPLAINED   = UINT32_MAX;  // a slot's configuration that takes no candidate

// ---- leaf reads ----

bool in_extent(uint64_t off, uint64_t width, size_t size) noexcept {
    return off <= size && width <= size - off;
}

uint64_t load(const BYTE* base, uint64_t seat, uint8_t width) noexcept {
    switch (width) {
        case 2:  return LOAD_U16(base + seat);
        case 4:  return LOAD_U32(base + seat);
        default: return LOAD_U64(base + seat);
    }
}

void store(BYTE* base, uint64_t seat, uint8_t width, uint64_t value) noexcept {
    switch (width) {
        case 2:  STORE_U16(base + seat, static_cast<uint16_t>(value)); break;
        case 4:  STORE_U32(base + seat, static_cast<uint32_t>(value)); break;
        default: STORE_U64(base + seat, value); break;
    }
}

uint32_t hamming(uint64_t a, uint64_t b) noexcept {
    return static_cast<uint32_t>(std::popcount(a ^ b));
}

bool self_validates(const BYTE* base, size_t size, uint64_t off) noexcept {
    return in_extent(off, DATA_BLOCK::HEADER_SIZE, size) && LOAD_U64(base + off) == off;
}

RECOVERY_TAG tag_at(const BYTE* base, size_t size, uint64_t off) noexcept {
    return in_extent(off, DATA_BLOCK::HEADER_SIZE, size)
               ? static_cast<RECOVERY_TAG>(LOAD_U16(base + off + DATA_BLOCK::RECOVERY))
               : FF_RECOVER_UNDEFINED;
}

bool has_magic(const BYTE* base, size_t size) noexcept {
    return size >= FF_HEADER::HEADER_SIZE && FF_HEADER(size).get_magic(base) == FF_MAGIC_BYTES;
}

// A newer writer may append V-Table slots, assign dictionary IDs and emit
// encodings this build has never seen, all legitimately, so the unfamiliar is
// evidence of damage only when this answers false. The version is composed
// exactly as the writer composes it (src/FF_Primitives.cpp).
bool stream_is_newer(const BYTE* base, size_t size) noexcept {
    if (!has_magic(base, size))
        return false;
    constexpr uint32_t kThisEngine =
        ((static_cast<uint32_t>(FASTFHIR_VERSION_MAJOR) & 0x3FFFu) << 16) |
         (static_cast<uint32_t>(FASTFHIR_VERSION_MINOR) & 0xFFFFu);
    return FF_HEADER(size).get_engine_version(base) > FF_HEADER_ENGINE_VERSION(kThisEngine);
}

// ---- what a tag says about a block's layout ----

// How an array's entries are laid out, which the element tag alone decides:
// measured over 70,000 arrays, the kind bits and the stride in KIND_AND_STEP
// never say anything the element tag does not. They are a second copy, and
// the census reads the layout from the tag.
enum class ElementShape : uint8_t {
    Scalar,     // raw values, which name nothing
    Block,      // each entry is itself a block: its first eight bytes are its own offset
    Tuple,      // 10-byte {offset, tag}: the offset names the entry's block elsewhere
    OffsetPtr,  // 8-byte offsets, each naming an FF_STRING, the one variable-length element
};

ElementShape element_shape(RECOVERY_TAG element) noexcept {
    if (element == RECOVER_FF_RESOURCE || FF_IsResourceTag(element))
        return ElementShape::Tuple;
    if (FF_IsStringLayoutTag(element))
        return ElementShape::OffsetPtr;
    if (FF_IsScalarBlockTag(element))
        return ElementShape::Scalar;
    return ElementShape::Block;
}

// The stride the writer gives this element type. A newer writer's blocks may
// be longer than this build's table says, and so its strides too.
uint64_t element_stride(RECOVERY_TAG element) noexcept {
    switch (element_shape(element)) {
        case ElementShape::Scalar:    return ff_slot_width(Recovery_to_Kind(element));
        case ElementShape::Block:     return Recovery::derived_block_size(element);
        case ElementShape::Tuple:     return TYPE_SIZE_RESOURCE;
        case ElementShape::OffsetPtr: return TYPE_SIZE_OFFSET;
    }
    return 0;
}

// Does this build know the layout of a block carrying `tag`? plausible_tag()
// cannot answer that: it maps every value from 0x0200 up to FF_FIELD_BLOCK, so
// 0xFFFF passes it. A newer writer's tag fails this too, which only ever makes
// recovery trust a position less.
bool sizable_tag(RECOVERY_TAG tag) {
    if (IsArrayTag(tag)) {
        const RECOVERY_TAG element = GetTypeFromTag(tag);
        if (element_shape(element) == ElementShape::Block)
            return sizable_tag(element);
        return element == RECOVER_FF_RESOURCE || FF_IsStringLayoutTag(element) ||
               Recovery_to_Kind(element) != FF_FIELD_UNKNOWN;
    }
    if (FF_IsStringLayoutTag(tag) || tag == RECOVER_FF_CODED_VALUE || tag == RECOVER_FF_CHECKSUM ||
        tag == RECOVER_FF_URL_DIRECTORY || tag == RECOVER_FF_MODULE_REGISTRY)
        return true;
    return !reflected_fields_view(static_cast<uint16_t>(tag)).empty();
}

// The provisional extent of the block at `off` under the tag it carries: what
// the census charges it when tiling the arena. An array of blocks is charged
// only its header, because its entries are blocks the census finds on their
// own. A stamped length that overruns the arena falls back to the header.
StreamMapEntry classify_block(const BYTE* base, size_t size, Offset off) {
    const RECOVERY_TAG tag  = FF_GET_RECOVERY_TAG(base, off);
    const uint64_t     room = size - off;
    const auto sized = [&](StreamMapEntryType type, uint64_t header, uint64_t extent) {
        return StreamMapEntry{type, off, static_cast<Size>(extent <= room ? extent : header), tag};
    };
    if (IsArrayTag(tag)) {
        if (room < FF_ARRAY::HEADER_SIZE || element_shape(GetTypeFromTag(tag)) == ElementShape::Block)
            return {StreamMapEntryType::Array, off, FF_ARRAY::HEADER_SIZE, tag};
        const FF_ARRAY array(off, size, 0);
        return sized(StreamMapEntryType::Array, FF_ARRAY::HEADER_SIZE,
                     FF_ARRAY::HEADER_SIZE + uint64_t{array.entry_count(base)} * array.entry_step(base));
    }
    if (FF_IsStringLayoutTag(tag)) {
        if (room < FF_STRING::STRING_DATA)
            return {StreamMapEntryType::String, off, 0, tag};
        return sized(StreamMapEntryType::String, 0,
                     FF_STRING::STRING_DATA + uint64_t{FF_GET_STRING_LENGTH(base, off)});
    }
    if (tag == RECOVER_FF_CODED_VALUE && room >= FF_CODED_VALUE::HEADER_SIZE)
        return sized(StreamMapEntryType::Block, FF_CODED_VALUE::HEADER_SIZE,
                     FF_CODED_VALUE::HEADER_SIZE + uint64_t{FF_CODED_VALUE(off, size, 0).length(base)});
    if (tag == RECOVER_FF_URL_DIRECTORY && room >= FF_URL_DIRECTORY::HEADER_SIZE)
        return sized(StreamMapEntryType::Block, FF_URL_DIRECTORY::HEADER_SIZE,
                     FF_URL_DIRECTORY::HEADER_SIZE +
                         uint64_t{FF_URL_DIRECTORY(off, size, 0).entry_count(base)} *
                             FF_URL_DIRECTORY::URL_ENTRY_SIZE);
    if (tag == RECOVER_FF_MODULE_REGISTRY && room >= FF_MODULE_REGISTRY::HEADER_SIZE)
        return sized(StreamMapEntryType::Block, FF_MODULE_REGISTRY::HEADER_SIZE,
                     FF_MODULE_REGISTRY::HEADER_SIZE +
                         uint64_t{LOAD_U32(base + off + FF_MODULE_REGISTRY::ENTRY_COUNT)} *
                             FF_MODULE_REGISTRY::REG_ENTRY_SIZE);
    if (tag == RECOVER_FF_CHECKSUM)
        return {StreamMapEntryType::Block, off, FF_CHECKSUM::HEADER_SIZE, tag};
    return {StreamMapEntryType::Block, off, Recovery::derived_block_size(tag), tag};
}

// ---- the board: everything recovery knows, in RAM ----

// What a slot's bytes say, read on their own.
enum class Reading : uint8_t {
    Absent,    // the slot is empty
    Inline,    // the word is a value: a dictionary ID, a packed date/time, a scalar choice
    Intact,    // every witness of the link holds
    Dangling,  // the word names a position that is not a block of the expected type
    Suspect,   // the word reads as a value, but it also names a block a damaged pointer would name
};

struct Judged {
    Reading reading = Reading::Absent;
    Offset  target  = FF_NULL_OFFSET;
};

// A block the board knows: one that vouches for itself, or one placed in a
// hole by chaining from the block before it, whose self-offset is the damage.
struct Anchor {
    RECOVERY_TAG tag    = FF_RECOVER_UNDEFINED;  // on the wire, or the type a decision gave it
    bool         lost   = false;
    /// Whether it ends exactly where the next block or a gap begins. A real
    /// block does, because the arena is dense; a pointer word that a flip has
    /// turned into its own address self-validates too, and almost never does.
    bool         tiles  = true;
};

// One hypothesis: `slot` names the block at `target`, whose type is `variant`.
// For a tuple the variant is what its tag half should read; for every other
// slot it is the compiled type. Each distance compares one witness with the
// value the hypothesis says it should hold. A target of FF_NULL_OFFSET is the
// hypothesis that the slot is empty and its word a damaged null, which has the
// pointer as its only witness.
struct Link {
    Recovery::Slot slot;
    Offset         target      = FF_NULL_OFFSET;
    RECOVERY_TAG   variant     = FF_RECOVER_UNDEFINED;
    uint8_t        d_ptr       = 0;  // the parent's word against the target's address
    uint8_t        d_self      = 0;  // the target's self-offset against its address
    uint8_t        d_tag_child = 0;  // the target's tag against the type
    uint8_t        d_tag_slot  = 0;  // a tuple's tag half against the type
};

// An array's stamped layout: KIND_AND_STEP and ENTRY_COUNT.
struct Geometry {
    uint16_t packing = 0;
    uint32_t count   = 0;
    bool operator==(const Geometry&) const = default;
};

struct Question {
    Recovery::Point     point;
    bool                open = true;
    std::vector<Offset> candidates;  // the blocks it could name, as of its last cycle
};

struct Board {
    const BYTE* base         = nullptr;
    size_t      size         = 0;
    bool        newer_stream = false;
    std::map<Offset, Anchor>                   anchors;
    std::vector<Gap>                           holes;     // ascending
    std::unordered_map<Offset, Recovery::Slot> owner;     // child -> its one parent's slot (H5)
    std::unordered_map<Offset, Link>           decided;   // child -> the link a cycle decided
    std::map<Offset, Geometry>                 extents;   // array -> the geometry a cycle decided
    std::vector<Link>                          emptied;   // slots a cycle decided hold a damaged null
    std::vector<Question>                      questions;
};

// ---- the callees below recover(), in call order ----

Board take_census(const BYTE* base, size_t size, const StreamMap& scan_map);
bool  run_cycle(Board& B);
void  report(const Board& B, const StreamMap& scan_map, FF_RecoveryReport& rep);

}  // namespace

// =====================================================================
// ENTRANCE — recover()
// =====================================================================

FF_RecoveryReport Recovery::recover() const {
    FF_RecoveryReport rep;
    const StreamMap scan_map = scan();
    Board B = take_census(m_base, m_size, scan_map);
    while (run_cycle(B)) {
    }
    // The gaps the report states are the ones left AFTER the decisions: a
    // block a cycle placed in a hole covers its bytes again.
    StreamMap after;
    after.file_size = static_cast<Size>(m_size);
    if (scan_map.contains(0))
        after[0] = scan_map.at(0);
    for (const auto& [off, a] : B.anchors)
        if (!a.lost)
            after[off] = classify_block(m_base, m_size, off);
    find_gaps(after);
    after.failures = scan_map.failures;
    report(B, after, rep);
    return rep;
}

namespace {

// =====================================================================
// PHASE 1 — the census (handoff §6)
// =====================================================================

std::vector<Recovery::Slot> header_slots(const BYTE* base, size_t size);
void   chain_hole(Board& B, const Gap& hole);
bool   slots_of(const Board& B, Offset off, RECOVERY_TAG tag, std::vector<Recovery::Slot>& out,
                bool* refuted = nullptr);
Judged judge(const Board& B, const Recovery::Slot& s);
std::unordered_map<Offset, std::vector<Offset>> children(const Board& B);
std::vector<Offset> attached(const Board& B, const std::unordered_map<Offset, std::vector<Offset>>& kids);
bool   ask(Board& B, const Recovery::Point& p);

Board take_census(const BYTE* base, size_t size, const StreamMap& scan_map) {
    Board B{base, size, stream_is_newer(base, size)};

    // The anchors and the holes come straight from the byte census. Each hole
    // is chained from its start, where the block before it ended, to place the
    // blocks whose self-offsets were destroyed.
    for (const auto& [off, entry] : scan_map)
        if (entry.type != StreamMapEntryType::Header)
            B.anchors.emplace(off, Anchor{entry.recovery, false, true});
    // A trailing run is chained too: in a sealed stream the checksum footer is
    // the last block, and one whose self-offset was destroyed leaves exactly
    // that run behind. Arena slack is zeros, which name no type, so the chain
    // places nothing there.
    std::unordered_set<uint64_t> skew_starts;
    for (const Gap& g : scan_map.gaps) {
        if (g.class_ == GapClass::VersionSkew)
            skew_starts.insert(g.start);
        else
            chain_hole(B, g);
        if (g.class_ == GapClass::Hole)
            B.holes.push_back(g);
    }
    // A block tiles when its end meets the next block -- one that vouches for
    // itself, or a lost one the chain placed there -- or the end of the
    // extent, or a newer writer's benign trailing run. A gap is no evidence:
    // gaps are measured from these same extents, so an extent that is too
    // short always leaves one starting exactly where it ends.
    for (auto it = B.anchors.begin(); it != B.anchors.end(); ++it) {
        const uint64_t end  = it->first + classify_block(base, size, it->first).size;
        const auto     next = std::next(it);
        it->second.tiles    = end >= size || skew_starts.contains(end) ||
                              (next != B.anchors.end() && next->first == end);
    }

    // Judge every slot: the header's first, then every block's under its own
    // wire tag -- a lost block's too, so its subtree hangs from it rather than
    // scattering into the orphan pools, where its own children would crowd the
    // search for it. Decided links keep this order, so each parent's children
    // come out in the order of its slots.
    std::vector<Recovery::Slot> slots = header_slots(base, size);
    std::unordered_set<Offset> refuted;
    for (const auto& [off, a] : B.anchors) {
        bool bad = false;
        if (slots_of(B, off, a.tag, slots, &bad) && bad)
            refuted.insert(off);
    }
    std::vector<Recovery::Edge>                    intact;
    std::unordered_map<Offset, size_t>             claim;      // child -> its first claimant
    std::map<Offset, std::vector<size_t>>          contested;  // child -> every claimant
    std::vector<std::pair<Recovery::Slot, Offset>> suspects;
    std::unordered_map<Offset, std::vector<Recovery::Slot>> open_of;
    for (const Recovery::Slot& s : slots) {
        const Judged j = judge(B, s);
        if (j.reading == Reading::Dangling)
            open_of[s.parent].push_back(s);
        if (j.reading == Reading::Suspect)
            suspects.emplace_back(s, j.target);
        if (j.reading != Reading::Intact)
            continue;
        const auto [it, first] = claim.emplace(j.target, intact.size());
        if (!first) {
            std::vector<size_t>& claimants = contested[j.target];
            if (claimants.empty())
                claimants.push_back(it->second);
            claimants.push_back(intact.size());
        }
        intact.push_back({s, j.target});
    }

    // A block two slots claim keeps neither claim: at most one of the two
    // words is right and the census cannot say which, so both open and a
    // cycle weighs them. A header slot keeps its claim, because it is the one
    // slot allowed to name the root, and the other word is the damage.
    std::vector<bool> dropped(intact.size(), false);
    for (const auto& [child, claimants] : contested)
        for (const size_t i : claimants)
            if (i != claimants.front() || intact[i].slot.parent != HEADER_PARENT) {
                dropped[i] = true;
                open_of[intact[i].slot.parent].push_back(intact[i].slot);
            }
    for (size_t i = 0; i < intact.size(); ++i)
        if (!dropped[i])
            B.owner.emplace(intact[i].child, intact[i].slot);

    // A suspect word is evidence only while the block it names has no parent;
    // a block some other slot names intactly accounts for itself.
    for (const auto& [s, target] : suspects)
        if (!B.owner.contains(target))
            open_of[s.parent].push_back(s);

    // Only what the header reaches becomes a question. An orphaned subtree's
    // questions are answered when a cycle attaches it, as part of its branch.
    for (const Offset m : attached(B, children(B))) {
        if (const auto it = open_of.find(m); it != open_of.end())
            for (const Recovery::Slot& s : it->second)
                ask(B, {Recovery::PointKind::Open, s, FF_NULL_OFFSET});
        if (refuted.contains(m))
            ask(B, {Recovery::PointKind::ArrayExtent, {}, m});
    }
    return B;
}

// The FF_HEADER's slots. ROOT_OFFSET followed by ROOT_RECOVERY is exactly a
// tuple's layout, so the root is a tuple slot; the checksum footer, the URL
// directory and the module registry are absolute offsets of a fixed type.
std::vector<Recovery::Slot> header_slots(const BYTE* base, size_t size) {
    if (size < FF_HEADER::HEADER_SIZE)
        return {};
    std::vector<Recovery::Slot> out{
        {HEADER_PARENT, FF_HEADER::ROOT_OFFSET, FF_FIELD_RESOURCE, Recovery::SlotRepr::Tuple,
         LOAD_U64(base + FF_HEADER::ROOT_OFFSET),
         static_cast<RECOVERY_TAG>(LOAD_U16(base + FF_HEADER::ROOT_RECOVERY)), FF_RECOVER_UNDEFINED, nullptr}};
    constexpr std::pair<Offset, RECOVERY_TAG> metadata[] = {
        {FF_HEADER::CHECKSUM_OFFSET, RECOVER_FF_CHECKSUM},
        {FF_HEADER::URL_DIR_OFFSET, RECOVER_FF_URL_DIRECTORY},
        {FF_HEADER::MODULE_REG_OFFSET, RECOVER_FF_MODULE_REGISTRY},
    };
    for (const auto& [seat, tag] : metadata)
        out.push_back({HEADER_PARENT, seat, FF_FIELD_BLOCK, Recovery::SlotRepr::Absolute,
                       LOAD_U64(base + seat), FF_RECOVER_UNDEFINED, tag, nullptr});
    return out;
}

// Place the blocks a hole holds. The arena is dense, so the first lost block
// starts where the hole starts, and each one's residual tag gives the extent
// that places the next. A block whose self-offset took any number of flips is
// found this way; the chain stops at a tag this build cannot size.
void chain_hole(Board& B, const Gap& hole) {
    const uint64_t end = static_cast<uint64_t>(hole.start) + hole.length;
    for (uint64_t q = hole.start; q + DATA_BLOCK::HEADER_SIZE <= end;) {
        if (!sizable_tag(tag_at(B.base, B.size, q)))
            return;
        const uint64_t extent = classify_block(B.base, B.size, static_cast<Offset>(q)).size;
        if (extent == 0 || q + extent > end)
            return;
        B.anchors.emplace(static_cast<Offset>(q), Anchor{tag_at(B.base, B.size, q), true, true});
        q += extent;
    }
}

// Where the first block after `off` starts, or the end of the extent. The
// arena is dense, so this is where the block at `off` must end. Only a block
// that vouches for itself, carries a tag this build can size, and tiles counts:
// a pointer flipped into its own address self-validates too (748 with bit 7
// cleared is 620, its own seat), and the bytes after it are neither an
// assigned tag nor the start of an extent that ends on the next block, as a
// real block's does.
uint64_t next_block_after(const Board& B, uint64_t off) {
    for (auto it = B.anchors.upper_bound(static_cast<Offset>(off)); it != B.anchors.end(); ++it)
        if (!it->second.lost && it->second.tiles && sizable_tag(it->second.tag))
            return std::min<uint64_t>(it->first, B.size);
    return B.size;
}

bool in_hole(const Board& B, uint64_t p) {
    const auto h = std::upper_bound(B.holes.begin(), B.holes.end(), p,
                                    [](uint64_t q, const Gap& g) { return q < g.start; });
    return h != B.holes.begin() && p < static_cast<uint64_t>(std::prev(h)->start) + std::prev(h)->length;
}

// The tag at `p` as the board knows it: a known block's type, which may be a
// decided one, or the residual bytes of a hole position.
RECOVERY_TAG block_tag(const Board& B, uint64_t p) {
    if (const auto a = B.anchors.find(static_cast<Offset>(p)); a != B.anchors.end())
        return a->second.tag;
    return in_hole(B, p) ? tag_at(B.base, B.size, p) : FF_RECOVER_UNDEFINED;
}

// ---- arrays: the geometry the bytes support ----

Geometry stamped(const Board& B, Offset off) {
    return {LOAD_U16(B.base + off + FF_ARRAY::KIND_AND_STEP), LOAD_U32(B.base + off + FF_ARRAY::ENTRY_COUNT)};
}

uint16_t packing_of(RECOVERY_TAG element, uint64_t stride) {
    const uint16_t kind =
        element_shape(element) == ElementShape::OffsetPtr ? FF_ARRAY::OFFSET : FF_ARRAY::INLINE_BLOCK;
    return static_cast<uint16_t>(kind | (stride & FF_ARRAY::STEP_MASK));
}

// Can position `p` hold an entry of an array of blocks? It must fit before the
// next block (H3), and whatever is there -- a block, or a hole's remains --
// must carry the element type (EXACT) or a tag within TAG_RADIUS of it (NEAR).
enum class Fit : uint8_t { Exact, Near, Foreign };

Fit entry_fit(const Board& B, uint64_t p, uint64_t stride, RECOVERY_TAG element) {
    if (!in_extent(p, stride, B.size) || p + stride > next_block_after(B, p))
        return Fit::Foreign;
    const RECOVERY_TAG t = block_tag(B, p);
    if (t == element)
        return Fit::Exact;
    return t != FF_RECOVER_UNDEFINED && hamming(t, element) <= TAG_RADIUS ? Fit::Near : Fit::Foreign;
}

// Does the word at `p` name an entry every witness of which holds? A word
// naming its own position is the header of the block that follows the array,
// read as if it were an entry, and never one; nor is a block another slot
// already names (H5).
bool names_an_entry(const Board& B, uint64_t p, ElementShape shape, RECOVERY_TAG element) {
    const auto a = B.anchors.find(static_cast<Offset>(LOAD_U64(B.base + p)));
    if (a == B.anchors.end() || a->second.lost || a->first == p || B.owner.contains(a->first))
        return false;
    const RECOVERY_TAG want =
        shape == ElementShape::Tuple ? static_cast<RECOVERY_TAG>(LOAD_U16(B.base + p + 8)) : element;
    return a->second.tag == want;
}

// The geometry the bytes around an array support, or nothing when they do not
// settle it. An array's entries carry no witnesses of their own, so its count
// is checked against the arena instead: a tuple or offset array's entries end
// at the next block, and an array of blocks runs for as long as blocks of the
// element type follow at its stride. Where the bytes leave the count open -- a
// block one tag flip from the element type at the tail, which is either a
// damaged entry or the block after the array -- the stamped count decides.
std::optional<Geometry> derive(const Board& B, Offset off, RECOVERY_TAG tag) {
    const RECOVERY_TAG element = GetTypeFromTag(tag);
    const ElementShape shape   = element_shape(element);
    const Geometry     st      = stamped(B, off);
    const uint64_t     entries = static_cast<uint64_t>(off) + FF_ARRAY::HEADER_SIZE;
    const uint64_t     width   = element_stride(element);
    if (width == 0 || !in_extent(entries, 0, B.size))
        return std::nullopt;

    if (shape != ElementShape::Block) {
        const uint64_t limit = next_block_after(B, off);
        if (limit < entries)
            return std::nullopt;
        const uint64_t room = (limit - entries) / width;
        Geometry g{packing_of(element, width), st.count};
        if (st.count <= room) {
            // A count flipped low leaves entries past the stamped ones whose
            // every witness still holds.
            while (shape != ElementShape::Scalar && g.count < room &&
                   names_an_entry(B, entries + g.count * width, shape, element))
                ++g.count;
            return g;
        }
        // An entry flipped into its own address self-validates too, and would
        // end the entries early. The stamp stands when that one word sits on an
        // entry boundary, the stamped entries fit before the block after it,
        // and the entry following it still names an entry.
        const bool on_boundary = (limit - entries) % width == 0;
        const bool fits_past   = entries + uint64_t{st.count} * width <= next_block_after(B, limit);
        if (shape != ElementShape::Scalar && on_boundary && fits_past && room + 1 < st.count &&
            names_an_entry(B, entries + (room + 1) * width, shape, element))
            return g;
        // A count flipped high overruns the next block. The entries must tile
        // exactly up to it, and the last of them must still name its entry.
        const bool tiles = (limit - entries) % width == 0;
        const bool last_holds = shape == ElementShape::Scalar || room == 0 ||
                                names_an_entry(B, entries + (room - 1) * width, shape, element);
        if (!tiles || !last_holds)
            return std::nullopt;
        g.count = static_cast<uint32_t>(room);
        return g;
    }

    // An array of blocks. The stamped count is its witness, and a damaged entry
    // does not end the array: an entry whose self-offset or tag was damaged
    // still sits at its stride, in a hole or under a near tag (REC-21.3, where
    // ending at the first damaged entry overwrote an intact count of 1,473 with
    // 28). It ends early only where something else demonstrably starts inside
    // the stamped entries: a block that vouches for itself and is not an entry
    // (its tag is foreign, or no entry fits before the next block), or bytes
    // that are neither a block nor a hole. It runs late while blocks of exactly
    // the element type continue at the stride. The stride is the stamped one
    // unless that is shorter than any block of the type, and failing that the
    // compiled one.
    const auto ends_at = [&](uint64_t p, uint64_t stride) {
        const auto a = B.anchors.find(static_cast<Offset>(p));
        if (a != B.anchors.end() && !a->second.lost)
            return entry_fit(B, p, stride, element) == Fit::Foreign;
        return !in_extent(p, stride, B.size) || (a == B.anchors.end() && !in_hole(B, p));
    };
    // An entry past the stamp is a block of exactly the element type that
    // tiles -- the block after an array can have its own tag flipped into the
    // element type, and then it fits but does not end where an entry would --
    // and that no other slot names (H5).
    const auto continues_at = [&](uint64_t p, uint64_t stride) {
        const auto a = B.anchors.find(static_cast<Offset>(p));
        const auto o = B.owner.find(static_cast<Offset>(p));
        return a != B.anchors.end() && !a->second.lost && a->second.tiles &&
               entry_fit(B, p, stride, element) == Fit::Exact && (o == B.owner.end() || o->second.parent == off);
    };
    const uint64_t stamped_stride = st.packing & FF_ARRAY::STEP_MASK;
    std::optional<Geometry> first;
    for (const uint64_t stride : {std::max(stamped_stride, width), width}) {
        uint32_t count = 0;
        while (count < st.count && !ends_at(entries + uint64_t{count} * stride, stride))
            ++count;
        while (count >= st.count && continues_at(entries + uint64_t{count} * stride, stride))
            ++count;
        const Geometry g{packing_of(element, stride), count};
        if (count == st.count)
            return g;
        if (!first)
            first = g;
    }
    return first;
}

// The slots of an array's first `g.count` entries, as far as the bytes allow:
// no further than the next block, unless `g` is a geometry derive() settled.
void read_entries(const Board& B, Offset off, RECOVERY_TAG element, const Geometry& g, bool settled,
                  std::vector<Recovery::Slot>& out) {
    const ElementShape shape   = element_shape(element);
    const uint64_t     entries = static_cast<uint64_t>(off) + FF_ARRAY::HEADER_SIZE;
    const uint64_t     width   = element_stride(element);
    if (shape == ElementShape::Scalar || width == 0)
        return;
    const bool     blocks = shape == ElementShape::Block;
    const uint64_t stride = blocks ? std::max<uint64_t>(g.packing & FF_ARRAY::STEP_MASK, width) : width;
    const uint64_t limit  = blocks || settled ? B.size : next_block_after(B, off);
    for (uint64_t i = 0; i < g.count && entries + (i + 1) * stride <= limit; ++i) {
        const uint64_t p = entries + i * stride;
        if (blocks && entry_fit(B, p, stride, element) == Fit::Foreign)
            return;
        Recovery::Slot s{off, static_cast<Offset>(p), FF_FIELD_BLOCK, Recovery::SlotRepr::InlineEntry,
                         0, FF_RECOVER_UNDEFINED, element, nullptr};
        if (shape == ElementShape::Tuple)
            s = {off, static_cast<Offset>(p), FF_FIELD_RESOURCE, Recovery::SlotRepr::Tuple,
                 LOAD_U64(B.base + p), static_cast<RECOVERY_TAG>(LOAD_U16(B.base + p + 8)),
                 FF_RECOVER_UNDEFINED, nullptr};
        if (shape == ElementShape::OffsetPtr)
            s = {off, static_cast<Offset>(p), FF_FIELD_STRING, Recovery::SlotRepr::Absolute,
                 LOAD_U64(B.base + p), FF_RECOVER_UNDEFINED, element, nullptr};
        out.push_back(s);
    }
}

// ---- reading and judging slots ----

// Every slot of the block at `off`, read as `tag`, appended to `out`. False
// when the reading is impossible under H3: a data block never contains another
// block's first byte, so a type whose V-Table would reach the next block is not
// this block's type, and reading it anyway lifts the next block's slots as
// this one's. A newer writer only ever makes a block larger than this build's
// table says, so the true type is never refused by this. `refuted` reports an
// array whose stamped geometry the bytes around it contradict.
bool slots_of(const Board& B, Offset off, RECOVERY_TAG tag, std::vector<Recovery::Slot>& out,
              bool* refuted) {
    if (IsArrayTag(tag)) {
        if (!in_extent(off, FF_ARRAY::HEADER_SIZE, B.size))
            return false;
        const std::optional<Geometry> g  = derive(B, off, tag);
        const Geometry                st = stamped(B, off);
        if (refuted)
            *refuted = !g || *g != st;
        // No further than both the stamp and the bytes allow: the entries the
        // stamp omits are the question a cycle answers.
        read_entries(B, off, GetTypeFromTag(tag), g ? Geometry{g->packing, std::min(g->count, st.count)} : st,
                     g.has_value(), out);
        return true;
    }
    if (tag == RECOVER_FF_URL_DIRECTORY) {
        // Each 16-byte entry ends with the offset of its segment's FF_STRING.
        const uint64_t table = static_cast<uint64_t>(off) + FF_URL_DIRECTORY::HEADER_SIZE;
        const uint64_t limit = next_block_after(B, off);
        if (limit < table)
            return true;
        const uint64_t n = std::min<uint64_t>(LOAD_U32(B.base + off + FF_URL_DIRECTORY::ENTRY_COUNT),
                                              (limit - table) / FF_URL_DIRECTORY::URL_ENTRY_SIZE);
        for (uint64_t i = 0; i < n; ++i) {
            const uint64_t seat =
                table + i * FF_URL_DIRECTORY::URL_ENTRY_SIZE + FF_URL_DIRECTORY::URL_ENTRY_SEG_OFFSET;
            out.push_back({off, static_cast<Offset>(seat), FF_FIELD_STRING, Recovery::SlotRepr::Absolute,
                           LOAD_U64(B.base + seat), FF_RECOVER_UNDEFINED, RECOVER_FF_STRING, nullptr});
        }
        return true;
    }
    const auto fields = reflected_fields_view(static_cast<uint16_t>(tag));
    if (fields.empty())
        return true;  // a leaf, or a tag this build does not know
    if (static_cast<uint64_t>(off) + Recovery::derived_block_size(tag) > next_block_after(B, off))
        return false;
    for (const FF_FieldInfo& f : fields) {
        const uint64_t seat = static_cast<uint64_t>(off) + f.field_offset;
        Recovery::Slot s{off, static_cast<Offset>(seat), f.kind, Recovery::SlotRepr::Absolute,
                         0, FF_RECOVER_UNDEFINED, f.child_recovery, &f};
        switch (f.kind) {
            case FF_FIELD_BLOCK:
            case FF_FIELD_STRING:
            case FF_FIELD_ARRAY:
                s.stored = LOAD_U64(B.base + seat);
                break;
            case FF_FIELD_RESOURCE:
            case FF_FIELD_CHOICE:
                s.repr       = Recovery::SlotRepr::Tuple;
                s.stored     = LOAD_U64(B.base + seat);
                s.stored_tag = static_cast<RECOVERY_TAG>(LOAD_U16(B.base + seat + 8));
                s.expect     = FF_RECOVER_UNDEFINED;
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
            // An inline value names nothing. A URL slot indexes the URL
            // directory. An identity slot is a cross-reference outside the
            // offset chain (H5), which takes no part in structural recovery.
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
    return true;
}

// How a slot's word names a block when its child's type is `x`. A tuple's
// encoding follows its variant: a code or date/time variant names its
// fallback leaf by an offset relative to the block that holds the slot.
enum class Encoding : uint8_t { Absolute, Relative32, Relative63, Geometry };

Encoding encoding(const Recovery::Slot& s, RECOVERY_TAG x) noexcept {
    switch (s.repr) {
        case Recovery::SlotRepr::Absolute:    return Encoding::Absolute;
        case Recovery::SlotRepr::Relative32:  return Encoding::Relative32;
        case Recovery::SlotRepr::Relative63:  return Encoding::Relative63;
        case Recovery::SlotRepr::InlineEntry: return Encoding::Geometry;
        case Recovery::SlotRepr::Tuple:       break;
    }
    const FF_FieldKind kind = Recovery_to_Kind(x);
    if (kind == FF_FIELD_CODE)
        return Encoding::Relative32;
    return kind == FF_FIELD_DATETIME ? Encoding::Relative63 : Encoding::Absolute;
}

uint8_t pointer_width(Encoding e) noexcept {
    return e == Encoding::Relative32 ? 4 : e == Encoding::Geometry ? 0 : 8;
}

uint64_t pointer_word(const Recovery::Slot& s, Encoding e) noexcept {
    return e == Encoding::Relative32 ? static_cast<uint32_t>(s.stored) : s.stored;
}

Offset decode(const Recovery::Slot& s, Encoding e) noexcept {
    switch (e) {
        case Encoding::Absolute:   return static_cast<Offset>(s.stored);
        case Encoding::Relative32: return FF_ResolveCodeableConceptOffset(static_cast<uint32_t>(s.stored), s.parent);
        case Encoding::Relative63: return FF_ResolveDateTimeOffset(s.stored, s.parent);
        case Encoding::Geometry:   return s.seat;
    }
    return FF_NULL_OFFSET;
}

// What an empty slot holds in each encoding.
uint64_t null_word(Encoding e) noexcept {
    switch (e) {
        case Encoding::Relative32: return FF_CODE_NULL;
        case Encoding::Relative63: return FF_DATETIME_NULL;
        case Encoding::Absolute:
        case Encoding::Geometry:   return FF_NULL_OFFSET;
    }
    return FF_NULL_OFFSET;
}

uint64_t encode(const Recovery::Slot& s, Encoding e, Offset target) noexcept {
    if (target == FF_NULL_OFFSET)
        return null_word(e);
    const uint64_t relative = static_cast<uint64_t>(target) - static_cast<uint64_t>(s.parent);
    switch (e) {
        case Encoding::Absolute:   return target;
        case Encoding::Relative32: return (relative & 0x7FFFFFFFu) | FF_CODED_VALUE_FLAG;
        case Encoding::Relative63: return (relative & FF_DATETIME_PAYLOAD_MASK) | FF_DATETIME_FALLBACK_FLAG;
        case Encoding::Geometry:   return 0;
    }
    return 0;
}

// The tag the child must carry on the wire when the slot's type is `x`. An
// ARRAY slot's compiled type is the element, which the array carries with the
// array bit set; a code or date/time variant's child is its fallback leaf.
RECOVERY_TAG child_wire(const Recovery::Slot& s, RECOVERY_TAG x) noexcept {
    if (s.kind == FF_FIELD_ARRAY && s.repr == Recovery::SlotRepr::Absolute)
        return ToArrayTag(x);
    const FF_FieldKind kind = Recovery_to_Kind(x);
    if (kind == FF_FIELD_CODE)
        return RECOVER_FF_CODED_VALUE;
    return kind == FF_FIELD_DATETIME ? RECOVER_FF_STRING : x;
}

// Whether a variant names a block at all. A scalar or identity variant is a
// value held in the slot itself.
bool names_a_block(RECOVERY_TAG x) noexcept {
    switch (Recovery_to_Kind(x)) {
        case FF_FIELD_STRING:
        case FF_FIELD_BLOCK:
        case FF_FIELD_RESOURCE:
        case FF_FIELD_ARRAY:
        case FF_FIELD_CHOICE:
        case FF_FIELD_CODE:
        case FF_FIELD_DATETIME:
            return true;
        case FF_FIELD_UNKNOWN:
        case FF_FIELD_BOOL:
        case FF_FIELD_INT32:
        case FF_FIELD_UINT32:
        case FF_FIELD_INT64:
        case FF_FIELD_UINT64:
        case FF_FIELD_FLOAT64:
        case FF_FIELD_URL:
        case FF_FIELD_ID:
            return false;
    }
    return false;
}

// H1: which types a tuple may hold. The Builder may root a stream at any
// block; a resource slot holds a resource, or one kept verbatim as opaque JSON
// from outside the profile; a choice holds one of its StructureDefinitions'
// variants over every revision this build reads.
std::span<const RECOVERY_TAG> band(const Recovery::Slot& s) {
    static const auto known = [](bool resources) {
        std::vector<RECOVERY_TAG> v;
        for (uint32_t t = 1; t < RECOVER_ARRAY_BIT; ++t) {
            const auto tag = static_cast<RECOVERY_TAG>(t);
            if (sizable_tag(tag) && (!resources || FF_IsResourceTag(tag) || tag == RECOVER_FF_OPAQUE_JSON))
                v.push_back(tag);
        }
        return v;
    };
    static const std::vector<RECOVERY_TAG> any = known(false), resources = known(true);
    if (s.parent == HEADER_PARENT || (s.kind == FF_FIELD_CHOICE && s.field == nullptr))
        return any;
    return s.kind == FF_FIELD_CHOICE ? s.field->variants : std::span<const RECOVERY_TAG>(resources);
}

bool band_admits(const Recovery::Slot& s, RECOVERY_TAG tag) {
    const std::span<const RECOVERY_TAG> b = band(s);
    return Recovery::plausible_tag(tag) && std::find(b.begin(), b.end(), tag) != b.end();
}

// Which types a slot's child may have. A typed slot's is compiled into the
// V-Table, which damage cannot reach. A tuple's is a latent choice from its
// band, narrowed to the types within TAG_RADIUS of either wire copy, and
// widened back to the whole band only when both copies are gone.
std::vector<RECOVERY_TAG> admissible(const Board& B, const Recovery::Slot& s) {
    if (s.repr != Recovery::SlotRepr::Tuple)
        return {s.expect};
    const std::span<const RECOVERY_TAG> all = band(s);
    std::vector<RECOVERY_TAG> near;
    for (const RECOVERY_TAG x : all) {
        const RECOVERY_TAG seen = block_tag(B, decode(s, encoding(s, x)));
        if (hamming(x, s.stored_tag) <= TAG_RADIUS ||
            (seen != FF_RECOVER_UNDEFINED && hamming(child_wire(s, x), seen) <= TAG_RADIUS))
            near.push_back(x);
    }
    return near.empty() ? std::vector<RECOVERY_TAG>(all.begin(), all.end()) : near;
}

// Does the word name a block that vouches for itself and carries exactly `want`?
Judged settle(const Board& B, uint64_t t, RECOVERY_TAG want) {
    const auto a = B.anchors.find(static_cast<Offset>(t));
    const bool holds = a != B.anchors.end() && !a->second.lost && a->second.tag == want;
    return {holds ? Reading::Intact : Reading::Dangling, static_cast<Offset>(t)};
}

// What a slot's bytes say on their own. A word that reads as a value is still
// Suspect when setting it back to a reference names exactly the block a
// damaged reference would name, or when it is a value no writer emits and the
// stream is not newer than this build: a dictionary ID outside the ledger, a
// packed date/time outside its ranges, a tuple tag outside its band (H1).
Judged judge(const Board& B, const Recovery::Slot& s) {
    switch (s.repr) {
        case Recovery::SlotRepr::Absolute:
            if (s.stored == static_cast<uint64_t>(FF_NULL_OFFSET))
                return {};
            return settle(B, s.stored, child_wire(s, s.expect));
        case Recovery::SlotRepr::InlineEntry:
            return settle(B, s.seat, s.expect);
        case Recovery::SlotRepr::Relative32:
        case Recovery::SlotRepr::Relative63:
            break;
        case Recovery::SlotRepr::Tuple: {
            if (s.stored == static_cast<uint64_t>(FF_NULL_OFFSET))
                return {};
            if (!band_admits(s, s.stored_tag))
                return {Reading::Suspect, static_cast<Offset>(s.stored)};
            if (encoding(s, s.stored_tag) == Encoding::Absolute && names_a_block(s.stored_tag))
                return settle(B, s.stored, s.stored_tag);
            // A flipped tag half can turn a pointer into what reads as a value.
            // The word then still holds its child's exact offset, which a real
            // value almost never does.
            const auto a = B.anchors.find(static_cast<Offset>(s.stored));
            if (a != B.anchors.end() && !a->second.lost && names_a_block(a->second.tag) &&
                encoding(s, a->second.tag) == Encoding::Absolute &&
                hamming(a->second.tag, s.stored_tag) <= TAG_RADIUS && band_admits(s, a->second.tag))
                return {Reading::Suspect, static_cast<Offset>(s.stored)};
            if (encoding(s, s.stored_tag) == Encoding::Absolute)
                return {Reading::Inline, FF_NULL_OFFSET};  // a scalar or identity variant
            break;
        }
    }
    const Encoding e    = s.repr == Recovery::SlotRepr::Tuple ? encoding(s, s.stored_tag)
                          : s.repr == Recovery::SlotRepr::Relative32 ? Encoding::Relative32
                                                                     : Encoding::Relative63;
    const bool     code = e == Encoding::Relative32;
    const uint64_t raw  = pointer_word(s, e);
    if (raw == (code ? uint64_t{FF_CODE_NULL} : FF_DATETIME_NULL))
        return {};
    const Offset       target = decode(s, e);
    const RECOVERY_TAG leaf   = code ? RECOVER_FF_CODED_VALUE : RECOVER_FF_STRING;
    if (raw & (code ? uint64_t{FF_CODED_VALUE_FLAG} : FF_DATETIME_FALLBACK_FLAG))
        return settle(B, target, leaf);
    if (settle(B, target, leaf).reading == Reading::Intact)
        return {Reading::Suspect, target};
    if (B.newer_stream)
        return {Reading::Inline, FF_NULL_OFFSET};
    bool well_formed = FF_ResolveCode(static_cast<uint32_t>(raw), 0) != nullptr;
    if (!code) {
        const FF_DateTimeParts parts = FF_UNPACK_DATETIME(raw);
        well_formed = ff_datetime_fits(parts) &&
                      static_cast<uint8_t>(parts.precision) <= static_cast<uint8_t>(FF_DateTimePrecision::FRAC3);
    }
    return {well_formed ? Reading::Inline : Reading::Suspect, target};
}

// Each parent's decided children, in the order of its slots.
std::unordered_map<Offset, std::vector<Offset>> children(const Board& B) {
    std::vector<std::pair<const Recovery::Slot*, Offset>> links;
    links.reserve(B.owner.size());
    for (const auto& [child, slot] : B.owner)
        links.emplace_back(&slot, child);
    std::sort(links.begin(), links.end(), [](const auto& a, const auto& b) { return a.first->seat < b.first->seat; });
    std::unordered_map<Offset, std::vector<Offset>> kids;
    for (const auto& [slot, child] : links)
        kids[slot->parent].push_back(child);
    return kids;
}

// Every block the FF_HEADER reaches through decided links, the header first,
// then depth first in the order of each parent's slots.
std::vector<Offset> attached(const Board& B, const std::unordered_map<Offset, std::vector<Offset>>& kids) {
    std::vector<Offset> members;
    if (B.size < FF_HEADER::HEADER_SIZE)
        return members;
    std::unordered_set<Offset> seen;
    std::vector<Offset> stack{HEADER_PARENT};
    while (!stack.empty()) {
        const Offset b = stack.back();
        stack.pop_back();
        if (!seen.insert(b).second)
            continue;
        members.push_back(b);
        if (const auto k = kids.find(b); k != kids.end())
            stack.insert(stack.end(), k->second.rbegin(), k->second.rend());
    }
    return members;
}

// Open a question, unless the same one is already open. Returns whether it did.
bool ask(Board& B, const Recovery::Point& p) {
    const bool open = std::any_of(B.questions.begin(), B.questions.end(), [&p](const Question& q) {
        return q.open && q.point.kind == p.kind &&
               (p.kind == Recovery::PointKind::Open ? q.point.slot.seat == p.slot.seat
                                                    : q.point.array == p.array);
    });
    if (!open)
        B.questions.push_back({p, true, {}});
    return !open;
}

// =====================================================================
// PHASE 2 — one cycle (handoff §7)
// =====================================================================

// Every block no decided link names, by its tag: the pools candidates come from.
using Pools = std::map<RECOVERY_TAG, std::vector<Offset>>;

// Every way to explain one slot or one block, ranked. Built bottom-up over an
// explicit stack, memoised per cycle, and pruned at every node to the
// configurations within ACCEPT_MARGIN of that node's best. The pruning loses
// nothing a decision needs: a configuration within the margin of the best is
// no further than that from the best in any one of its parts.
class Tree {
public:
    struct Entry {
        int64_t               cost = 0;
        std::vector<uint32_t> pick;  // slot: {candidate, child entry} or {UNEXPLAINED}; block: one per part
    };
    struct Node {
        bool                  block    = false;
        bool                  possible = true;
        Offset                off      = FF_NULL_OFFSET;        // block: where it is
        RECOVERY_TAG          tag      = FF_RECOVER_UNDEFINED;  // block: the type it is read as
        Recovery::Slot        slot;                             // slot
        std::vector<Link>     links;                            // slot: its candidates, one per kid
        std::vector<uint32_t> kids;                             // block: its slots; slot: its candidates' blocks
        std::vector<Entry>    list;
        bool                  truncated = false;                // K_BEST cut something inside the margin
        uint8_t               state     = 0;                    // 0 unexpanded, 1 on the stack, 2 done
    };
    struct Config {
        std::vector<Link>           links;  // parents before children
        std::vector<Recovery::Slot> unexplained;
    };

    Tree(const Board& board, const Pools& pools) : B(board), pools(pools) {}

    uint32_t slot_node(const Recovery::Slot& s) {
        const auto key = std::make_tuple(s.seat, s.kind, s.repr, s.expect, reinterpret_cast<uintptr_t>(s.field));
        if (const auto it = slots.find(key); it != slots.end())
            return it->second;
        nodes.push_back({});
        nodes.back().slot = s;
        return slots[key] = static_cast<uint32_t>(nodes.size() - 1);
    }

    // Rank a node's configurations. Post-order over an explicit stack: a node
    // is combined once every node beneath it is. A node met again while still
    // on the stack is a loop, which only damage makes, and it is impossible.
    const Node& solve(uint32_t root) {
        std::vector<std::pair<uint32_t, uint32_t>> stack;  // node, next kid
        if (nodes[root].state == 0) {
            expand(root);
            stack.emplace_back(root, 0);
        }
        while (!stack.empty()) {
            const auto [n, i] = stack.back();
            if (i == nodes[n].kids.size()) {
                combine(n);
                stack.pop_back();
                continue;
            }
            ++stack.back().second;
            const uint32_t k = nodes[n].kids[i];
            if (nodes[k].state == 0) {
                expand(k);
                stack.emplace_back(k, 0);
            }
        }
        return nodes[root];
    }

    void materialize(uint32_t root, uint32_t entry, Config& out) const {
        std::vector<std::pair<uint32_t, uint32_t>> stack{{root, entry}};
        while (!stack.empty()) {
            const auto [n, e] = stack.back();
            stack.pop_back();
            const Node&                  node = nodes[n];
            const std::vector<uint32_t>& pick = node.list[e].pick;
            if (node.block) {
                for (size_t i = node.kids.size(); i-- > 0;)
                    stack.emplace_back(node.kids[i], pick[i]);
            } else if (pick[0] == UNEXPLAINED) {
                out.unexplained.push_back(node.slot);
            } else {
                out.links.push_back(node.links[pick[0]]);
                stack.emplace_back(node.kids[pick[0]], pick[1]);
            }
        }
    }

    const Node& operator[](uint32_t n) const { return nodes[n]; }

    // Every way to take one entry from each list whose total lies within
    // ACCEPT_MARGIN of the cheapest total, cheapest first, at most K_BEST.
    // Precondition: no list is empty.
    static std::vector<Entry> sums(const std::vector<const std::vector<Entry>*>& lists, bool& truncated) {
        std::vector<Entry>    out;
        std::vector<uint32_t> pick(lists.size(), 0);
        int64_t best = 0;
        for (const auto* l : lists)
            best += l->front().cost;
        const auto walk = [&](auto& self, size_t i, int64_t slack) -> void {
            if (out.size() > K_BEST)
                return;
            if (i == lists.size()) {
                out.push_back({best + ACCEPT_MARGIN - slack, pick});
                return;
            }
            const std::vector<Entry>& l = *lists[i];
            for (uint32_t j = 0; j < l.size() && l[j].cost - l[0].cost <= slack; ++j) {
                pick[i] = j;
                self(self, i + 1, slack - (l[j].cost - l[0].cost));
            }
        };
        walk(walk, 0, ACCEPT_MARGIN);
        prune(out, truncated);
        return out;
    }

private:
    void expand(uint32_t n);
    void combine(uint32_t n);

    static void prune(std::vector<Entry>& list, bool& truncated) {
        std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) {
            return a.cost != b.cost ? a.cost < b.cost : a.pick < b.pick;
        });
        if (list.empty())
            return;
        const int64_t bound = list.front().cost + ACCEPT_MARGIN;
        list.erase(std::find_if(list.begin(), list.end(), [bound](const Entry& e) { return e.cost > bound; }),
                   list.end());
        if (list.size() > K_BEST) {
            list.resize(K_BEST);
            truncated = true;
        }
    }

    uint32_t block_node(Offset off, RECOVERY_TAG tag) {
        const auto key = std::make_pair(off, tag);
        if (const auto it = blocks.find(key); it != blocks.end())
            return it->second;
        nodes.push_back({});
        nodes.back().block = true;
        nodes.back().off   = off;
        nodes.back().tag   = tag;
        return blocks[key] = static_cast<uint32_t>(nodes.size() - 1);
    }

    const Board& B;
    const Pools& pools;
    std::vector<Node> nodes;
    std::map<std::pair<Offset, RECOVERY_TAG>, uint32_t> blocks;
    std::map<std::tuple<Offset, FF_FieldKind, Recovery::SlotRepr, RECOVERY_TAG, uintptr_t>, uint32_t> slots;
};

std::vector<Link> candidates(const Board& B, const Pools& pools, const Recovery::Slot& s);
int64_t value_header(const Link& L) noexcept;

void Tree::expand(uint32_t n) {
    nodes[n].state = 1;
    std::vector<uint32_t> kids;
    if (nodes[n].block) {
        std::vector<Recovery::Slot> read;
        nodes[n].possible = slots_of(B, nodes[n].off, nodes[n].tag, read);
        for (const Recovery::Slot& s : read) {
            const Reading r = judge(B, s).reading;
            if (r != Reading::Absent && r != Reading::Inline)
                kids.push_back(slot_node(s));
        }
    } else {
        std::vector<Link> links = candidates(B, pools, nodes[n].slot);
        for (const Link& L : links)  // an empty slot's child is a leaf at FF_NULL_OFFSET
            kids.push_back(block_node(L.target, L.target == FF_NULL_OFFSET ? FF_RECOVER_UNDEFINED
                                                                          : child_wire(L.slot, L.variant)));
        nodes[n].links = std::move(links);
    }
    nodes[n].kids = std::move(kids);
}

void Tree::combine(uint32_t n) {
    Node& node = nodes[n];
    node.state = 2;
    if (!node.block) {
        for (uint32_t i = 0; i < node.kids.size(); ++i) {
            const Node& child = nodes[node.kids[i]];
            if (child.state != 2)
                continue;  // still on the stack: a loop
            node.truncated |= child.truncated;
            for (uint32_t j = 0; j < child.list.size(); ++j)
                node.list.push_back({child.list[j].cost - value_header(node.links[i]), {i, j}});
        }
        node.list.push_back({U_UNEXPLAINED, {UNEXPLAINED}});
        prune(node.list, node.truncated);
        return;
    }
    std::vector<const std::vector<Entry>*> parts;
    for (const uint32_t k : node.kids) {
        node.possible &= nodes[k].state == 2;  // a slot still on the stack: a loop
        node.truncated |= nodes[k].truncated;
        parts.push_back(&nodes[k].list);
    }
    if (node.possible)
        node.list = sums(parts, node.truncated);
}

// Does `t` lie strictly inside the extent of the block before it? No block
// starts there (H3), whether that block vouches for itself or was placed in
// its hole by chaining.
bool inside_a_block(const Board& B, uint64_t t) {
    auto it = B.anchors.upper_bound(static_cast<Offset>(t));
    if (it == B.anchors.begin())
        return false;
    --it;
    return it->first < t && t < it->first + classify_block(B.base, B.size, it->first).size;
}

// How many bits a pointer search may repair before a match stops being
// evidence: the largest r at which the pool is expected to put fewer than
// MAX_CHANCE blocks within r bits of a word by coincidence, or -1 when even an
// exact match would be coincidence that often.
int significant_radius(size_t extent, size_t pool) noexcept {
    const unsigned width   = static_cast<unsigned>(std::bit_width(extent));
    const double   density = static_cast<double>(pool) / static_cast<double>(extent);
    double ball = 0, choose = 1;  // C(width, d)
    int radius = -1;
    for (unsigned d = 0; d <= FF_RECOVERY_MAX_FLIPS && d <= width; ++d) {
        ball += choose;
        if (density * ball >= MAX_CHANCE)
            break;
        radius = static_cast<int>(d);
        choose = choose * (width - d) / (d + 1);
    }
    return radius;
}

// Every block the slot could name, as a scored link (handoff §7.2). The stored
// word as it stands costs no search. A block from the pools costs one, so it
// is admitted only inside the significance radius, counted in the low bits the
// extent spans: set bits above them must be cleared to land in bounds at all,
// and clearing them is forced, not searched.
std::vector<Link> candidates(const Board& B, const Pools& pools, const Recovery::Slot& s) {
    std::vector<Link> out;
    const uint64_t low = (uint64_t{1} << std::bit_width(B.size)) - 1;
    // A position the stored word names exactly needs no search, so it is a
    // candidate wherever a block could be: a known block, a hole the chain
    // did not reach, or bytes one or two flips from their own address.
    const auto consider = [&](RECOVERY_TAG x, Offset t, bool stored) {
        if (!in_extent(t, DATA_BLOCK::HEADER_SIZE, B.size))
            return;  // H2
        if (const auto o = B.owner.find(t); o != B.owner.end() && o->second.seat != s.seat)
            return;  // H5: another slot holds it
        const auto     a    = B.anchors.find(t);
        const uint32_t self = hamming(LOAD_U64(B.base + t), t);
        if (a == B.anchors.end() && self > SELF_RADIUS && !(stored && in_hole(B, t) && !inside_a_block(B, t)))
            return;  // no block is there
        const Encoding     e    = encoding(s, x);
        const RECOVERY_TAG seen = a != B.anchors.end() ? a->second.tag : tag_at(B.base, B.size, t);
        const Link L{s, t, x,
                     static_cast<uint8_t>(pointer_width(e) ? hamming(pointer_word(s, e), encode(s, e, t)) : 0),
                     static_cast<uint8_t>(std::min<uint32_t>(self, 64)),
                     static_cast<uint8_t>(hamming(seen, child_wire(s, x))),
                     static_cast<uint8_t>(s.repr == Recovery::SlotRepr::Tuple ? hamming(s.stored_tag, x) : 0)};
        // A block whose own tag lies further than TAG_RADIUS from the type is
        // some other block, whatever the pointer says.
        if (L.d_tag_child <= TAG_RADIUS && std::max({L.d_ptr, L.d_self, L.d_tag_slot}) <= FF_RECOVERY_MAX_FLIPS)
            out.push_back(L);
    };
    std::vector<RECOVERY_TAG> types = admissible(B, s);
    if (s.repr == Recovery::SlotRepr::Tuple)
        std::erase_if(types, [](RECOVERY_TAG x) { return !names_a_block(x); });  // values, which
                                                                                    // "unexplained" stands for
    for (const RECOVERY_TAG x : types)
        consider(x, decode(s, encoding(s, x)), true);
    if (s.repr == Recovery::SlotRepr::InlineEntry)
        return out;  // an inline entry's place is fixed by its array; there is nothing to search

    // The slot may be empty, its word a damaged null. No block lies within a
    // few bits of all-ones, so a word that close to null has no other reading.
    // It is held to SELF_RADIUS, like a damaged self-offset, for the same
    // reason: a V-Table read under the wrong type lands on runs of null words
    // shifted out of alignment, which sit a handful of bits from all-ones and
    // would otherwise make the wrong reading look cheap.
    const Encoding stored = s.repr == Recovery::SlotRepr::Relative32   ? Encoding::Relative32
                            : s.repr == Recovery::SlotRepr::Relative63 ? Encoding::Relative63
                                                                       : Encoding::Absolute;
    const uint32_t from_null = hamming(pointer_word(s, stored), null_word(stored));
    if (from_null <= SELF_RADIUS)
        out.push_back({s, FF_NULL_OFFSET, FF_RECOVER_UNDEFINED, static_cast<uint8_t>(from_null)});

    // The search: one radius for the slot, taken over every unowned block any
    // of its types could be. A radius per type would let a type with few
    // orphans nearby search further than the true type, whose neighbourhood
    // the same damage has crowded.
    std::vector<std::pair<Offset, RECOVERY_TAG>> reach;  // block, a type it could be
    for (const auto& [tag, offs] : pools)
        for (const RECOVERY_TAG x : types)
            if (hamming(tag, child_wire(s, x)) <= TAG_RADIUS)
                for (const Offset o : offs)
                    reach.emplace_back(o, x);
    std::vector<Offset> distinct;
    for (const auto& [o, x] : reach)
        distinct.push_back(o);
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    // Bits set above the extent must be cleared to land in bounds at all, so
    // clearing them is forced rather than searched -- but a real pointer has
    // at most a flip or two there. A word with more is not a damaged pointer:
    // it is what a V-Table read under the wrong type finds, and letting it
    // search lets that wrong reading adopt some other block's orphaned subtree.
    const int radius = significant_radius(B.size, distinct.size());
    for (const auto& [o, x] : reach) {
        const Encoding e    = encoding(s, x);
        const uint64_t word = pointer_word(s, e), want = encode(s, e, o);
        if (o != decode(s, e) && hamming(word & ~low, want & ~low) <= SELF_RADIUS &&
            static_cast<int>(hamming(word & low, want & low)) <= radius)
            consider(x, o, false);
    }
    // One link per (target, type): the stored word and a pool can find the same block.
    std::sort(out.begin(), out.end(), [](const Link& a, const Link& b) {
        if (a.target != b.target || a.variant != b.variant)
            return std::tie(a.target, a.variant) < std::tie(b.target, b.variant);
        return value_header(a) > value_header(b);
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const Link& a, const Link& b) { return a.target == b.target && a.variant == b.variant; }),
              out.end());
    return out;
}

// The value of a link's own witnesses, in bits (handoff §5.3): an exact match
// is worth the witness's width, and each disagreeing bit costs C_FLIP.
int64_t value_header(const Link& L) noexcept {
    const auto w = [](int64_t width, uint8_t d) { return width - C_FLIP * d; };
    if (L.target == FF_NULL_OFFSET)
        return w(8 * pointer_width(encoding(L.slot, L.variant)), L.d_ptr);
    int64_t v = w(64, L.d_self) + w(16, L.d_tag_child);
    if (const uint8_t width = pointer_width(encoding(L.slot, L.variant)))
        v += w(8 * width, L.d_ptr);
    if (L.slot.repr == Recovery::SlotRepr::Tuple)
        v += w(16, L.d_tag_slot);
    return v;
}

bool resolve_extent(Board& B, size_t question);
bool decide(Board& B, const Tree::Config& best, const std::vector<size_t>& branch);

// One cycle: the arrays whose geometry the bytes now settle, then every open
// slot's branch, ranked whole. Branches that want the same block are solved as
// one, because only one of them can have it (H5). Returns whether anything
// changed, which is what keeps the cycles going.
bool run_cycle(Board& B) {
    bool changed = false;
    for (size_t q = 0; q < B.questions.size(); ++q)
        if (B.questions[q].open && B.questions[q].point.kind == Recovery::PointKind::ArrayExtent)
            changed |= resolve_extent(B, q);

    Pools pools;
    for (const auto& [off, a] : B.anchors)
        if (!B.owner.contains(off))
            pools[a.tag].push_back(off);
    Tree tree(B, pools);

    std::vector<size_t> open;
    for (size_t q = 0; q < B.questions.size(); ++q)
        if (B.questions[q].open && B.questions[q].point.kind == Recovery::PointKind::Open)
            open.push_back(q);
    std::sort(open.begin(), open.end(), [&B](size_t a, size_t b) {
        return B.questions[a].point.slot.seat < B.questions[b].point.slot.seat;
    });

    // Rank each branch, and join the branches that want the same block.
    std::vector<uint32_t> roots(open.size());
    std::vector<size_t>   group(open.size());
    const auto find = [&group](size_t i) {
        while (group[i] != i)
            i = group[i];
        return i;
    };
    std::unordered_map<Offset, size_t> wanted;
    for (size_t i = 0; i < open.size(); ++i) {
        Question& q = B.questions[open[i]];
        roots[i]    = tree.slot_node(q.point.slot);
        group[i]    = i;
        q.candidates.clear();
        for (const Link& L : tree.solve(roots[i]).links) {
            q.candidates.push_back(L.target);
            const auto [it, first] = wanted.emplace(L.target, i);
            if (!first)
                group[std::max(find(i), find(it->second))] = std::min(find(i), find(it->second));
        }
    }
    std::map<size_t, std::vector<size_t>> components;
    for (size_t i = 0; i < open.size(); ++i)
        components[find(i)].push_back(i);

    for (const auto& [first, members] : components) {
        if (members.size() > MAX_COMPONENT)
            continue;
        std::vector<const std::vector<Tree::Entry>*> lists;
        bool truncated = false;
        for (const size_t i : members) {
            lists.push_back(&tree[roots[i]].list);
            truncated |= tree[roots[i]].truncated;
        }
        // The cheapest configuration that uses no block twice, and the cost of the next.
        std::optional<Tree::Config> best;
        int64_t best_cost = 0;
        std::optional<int64_t> alt_cost;
        for (const Tree::Entry& e : Tree::sums(lists, truncated)) {
            Tree::Config cfg;
            for (size_t k = 0; k < members.size(); ++k)
                tree.materialize(roots[members[k]], e.pick[k], cfg);
            std::unordered_set<Offset> used;
            const bool valid = std::all_of(cfg.links.begin(), cfg.links.end(), [&](const Link& L) {
                const auto o = B.owner.find(L.target);
                return L.target == FF_NULL_OFFSET ||
                       (used.insert(L.target).second && (o == B.owner.end() || o->second.seat == L.slot.seat));
            });
            if (!valid)
                continue;
            if (best) {
                alt_cost = e.cost;
                break;
            }
            best      = std::move(cfg);
            best_cost = e.cost;
        }
        // Rule 7: decide only what the evidence separates, and only links
        // each worth something in their own right.
        const bool separated = alt_cost ? *alt_cost - best_cost >= ACCEPT_MARGIN : !truncated;
        if (!best || best->links.empty() || !separated || best->unexplained.size() > best->links.size() ||
            std::any_of(best->links.begin(), best->links.end(),
                        [](const Link& L) { return value_header(L) < ACCEPT_ABS; }))
            continue;
        std::vector<size_t> branch;
        for (const size_t i : members)
            branch.push_back(open[i]);
        changed |= decide(B, *best, branch);
    }
    return changed;
}

// The geometry the bytes now settle for a refuted array: decide it, and ask
// about every entry it exposes. Each exposed entry is a question rather than a
// decision here, so its whole branch is ranked like any other.
bool resolve_extent(Board& B, size_t question) {
    const Offset array = B.questions[question].point.array;
    const auto   a     = B.anchors.find(array);
    const std::optional<Geometry> g = a == B.anchors.end() ? std::nullopt : derive(B, array, a->second.tag);
    if (!g)
        return false;
    // The stamped count is a witness too. A single flip moves it by one bit,
    // so a geometry further than that from the stamp needs more damage than
    // it explains -- an orphan of the element type that happens to follow the
    // array is likelier -- and the question stays open, reported.
    if (hamming(g->count, stamped(B, array).count) > 1)
        return false;
    B.questions[question].open = false;
    if (*g == stamped(B, array))
        return true;
    B.extents[array] = *g;
    std::vector<Recovery::Slot> entries;
    read_entries(B, array, GetTypeFromTag(a->second.tag), *g, true, entries);
    for (const Recovery::Slot& s : entries) {
        const Judged j = judge(B, s);
        const auto   o = B.owner.find(j.target);
        if (j.reading == Reading::Intact && o != B.owner.end() && o->second.seat == s.seat)
            continue;  // the census already decided it
        if (j.reading != Reading::Absent && j.reading != Reading::Inline)
            ask(B, {Recovery::PointKind::Open, s, FF_NULL_OFFSET});
    }
    return true;
}

// Record a branch's decisions on the board. Nothing is written to the stream:
// each link's writes are planned only when the report is built.
bool decide(Board& B, const Tree::Config& best, const std::vector<size_t>& branch) {
    bool changed = false;
    for (const Link& L : best.links) {
        if (L.target == FF_NULL_OFFSET) {
            B.emptied.push_back(L);
            changed = true;
            continue;
        }
        const auto o     = B.owner.find(L.target);
        const bool exact = L.d_ptr == 0 && L.d_self == 0 && L.d_tag_child == 0 && L.d_tag_slot == 0;
        if (exact && o != B.owner.end() && o->second.seat == L.slot.seat)
            continue;  // a link the census already decided
        B.owner[L.target]   = L.slot;
        B.decided[L.target] = L;
        B.anchors[L.target] = Anchor{child_wire(L.slot, L.variant), false, true};
        changed = true;
    }
    for (const size_t q : branch) {
        const Offset seat = B.questions[q].point.slot.seat;
        B.questions[q].open = std::any_of(best.unexplained.begin(), best.unexplained.end(),
                                          [seat](const Recovery::Slot& s) { return s.seat == seat; });
        changed |= !B.questions[q].open;
    }
    for (const Recovery::Slot& s : best.unexplained)
        changed |= ask(B, {Recovery::PointKind::Open, s, FF_NULL_OFFSET});
    return changed;
}

// =====================================================================
// THE REPORT (handoff §9)
// =====================================================================

// The writes a decided link needs: every copy of every witness brought to the
// value the decision says it holds. A relative pointer stays relative.
std::vector<PlannedWrite> plan(const Board& B, const Link& L) {
    std::vector<PlannedWrite> w;
    const Encoding     e    = encoding(L.slot, L.variant);
    const RECOVERY_TAG wire = child_wire(L.slot, L.variant);
    if (pointer_width(e) && encode(L.slot, e, L.target) != pointer_word(L.slot, e))
        w.push_back({L.slot.seat, pointer_width(e), encode(L.slot, e, L.target)});
    if (L.target == FF_NULL_OFFSET)
        return w;
    if (L.slot.repr == Recovery::SlotRepr::Tuple && L.slot.stored_tag != L.variant)
        w.push_back({L.slot.seat + 8, 2, L.variant});
    if (LOAD_U64(B.base + L.target) != L.target)
        w.push_back({L.target, 8, L.target});
    if (tag_at(B.base, B.size, L.target) != wire)
        w.push_back({L.target + DATA_BLOCK::RECOVERY, 2, wire});
    return w;
}

// A verdict is named by the strongest write it plans.
RepairClass class_of(const BlockVerdict& v, Offset seat) {
    int strongest = 0;
    for (const PlannedWrite& w : v.writes)  // an inline entry's seat is its own self-offset
        strongest = std::max(strongest, w.seat == v.block.child                          ? 3
                                        : w.seat == seat                                 ? 4
                                        : w.seat == v.block.child + DATA_BLOCK::RECOVERY ||
                                              w.seat == seat + 8                         ? 2
                                                                                         : 1);
    constexpr RepairClass classes[] = {RepairClass::Intact, RepairClass::ExtentDerived, RepairClass::TagRepaired,
                                       RepairClass::PositionRepaired, RepairClass::Corroborated};
    return classes[strongest];
}

void header_verdicts(const Board& B, const std::vector<BlockVerdict>& blocks, FF_RecoveryReport& rep);

void report(const Board& B, const StreamMap& scan_map, FF_RecoveryReport& rep) {
    const std::vector<Offset>        members = attached(B, children(B));
    const std::unordered_set<Offset> reach(members.begin(), members.end());
    std::unordered_set<Offset> unsettled;  // arrays whose geometry stayed open
    for (const Question& q : B.questions)
        if (q.open && q.point.kind == Recovery::PointKind::ArrayExtent)
            unsettled.insert(q.point.array);

    // Every link the header reaches: Intact, or the repair its writes name.
    for (const auto& [child, slot] : B.owner) {
        if (!reach.contains(slot.parent))
            continue;
        const auto d = B.decided.find(child);
        const RECOVERY_TAG type = d != B.decided.end()                   ? d->second.variant
                                  : slot.repr == Recovery::SlotRepr::Tuple ? slot.stored_tag
                                                                           : slot.expect;
        BlockVerdict v{{slot.parent, slot.seat - slot.parent, slot.kind, child, child_wire(slot, type),
                        tag_at(B.base, B.size, child)}};
        if (d != B.decided.end())
            v.writes = plan(B, d->second);
        if (const auto x = B.extents.find(child); x != B.extents.end()) {
            const Geometry st = stamped(B, child);
            if (x->second.packing != st.packing)
                v.writes.push_back({child + FF_ARRAY::KIND_AND_STEP, 2, x->second.packing});
            if (x->second.count != st.count)
                v.writes.push_back({child + FF_ARRAY::ENTRY_COUNT, 4, x->second.count});
            v.derived_extent = x->second.count;
        }
        for (const PlannedWrite& w : v.writes)
            v.bit_cost += hamming(load(B.base, w.seat, w.width), w.value);
        v.class_ = class_of(v, slot.seat);
        if (v.class_ == RepairClass::Intact && unsettled.contains(child))
            v.class_ = RepairClass::Ambiguous;
        rep.blocks.push_back(std::move(v));
    }
    // Every slot a cycle emptied: its word was a damaged null.
    for (const Link& L : B.emptied) {
        BlockVerdict v{{L.slot.parent, L.slot.seat - L.slot.parent, L.slot.kind, FF_NULL_OFFSET,
                        FF_RECOVER_UNDEFINED, FF_RECOVER_UNDEFINED}};
        v.writes = plan(B, L);
        for (const PlannedWrite& w : v.writes)
            v.bit_cost += hamming(load(B.base, w.seat, w.width), w.value);
        v.class_ = RepairClass::Corroborated;
        rep.blocks.push_back(std::move(v));
    }
    // Every question still open: named, never guessed.
    for (const Question& q : B.questions) {
        if (!q.open || q.point.kind != Recovery::PointKind::Open)
            continue;
        const Recovery::Slot& s    = q.point.slot;
        const RECOVERY_TAG    type = s.repr == Recovery::SlotRepr::Tuple ? s.stored_tag : s.expect;
        const Offset          at   = judge(B, s).target;
        BlockVerdict v{{s.parent, s.seat - s.parent, s.kind, at, child_wire(s, type), tag_at(B.base, B.size, at)}};
        v.class_     = q.candidates.empty() ? RepairClass::Unrecovered : RepairClass::Ambiguous;
        v.candidates = q.candidates;
        rep.blocks.push_back(std::move(v));
    }
    std::sort(rep.blocks.begin(), rep.blocks.end(), [](const BlockVerdict& a, const BlockVerdict& b) {
        return a.block.parent + a.block.field < b.block.parent + b.block.field;
    });

    rep.blocks_total = rep.blocks.size();
    std::size_t* const counters[] = {&rep.intact,          &rep.corroborated,   &rep.tag_repaired,
                                     &rep.position_repaired, &rep.extent_derived, &rep.ambiguous,
                                     &rep.unrecovered};
    for (const BlockVerdict& v : rep.blocks)
        ++*counters[static_cast<size_t>(v.class_)];
    header_verdicts(B, rep.blocks, rep);
    rep.failures = scan_map.failures;
    rep.gaps     = scan_map.gaps;
    for (const Gap& g : rep.gaps) {
        rep.holes        += g.class_ == GapClass::Hole;
        rep.version_skew += g.class_ == GapClass::VersionSkew;
    }
}

// One verdict per FF_HEADER field, in HeaderField order. The pointer fields
// report what the census and the cycles decided for the header's slots, whose
// own verdicts carry the writes; the rest are checked here against their
// witnesses.
void header_verdicts(const Board& B, const std::vector<BlockVerdict>& blocks, FF_RecoveryReport& rep) {
    if (B.size < FF_HEADER::HEADER_SIZE)
        return;
    const auto record = [&rep](HeaderField field, RepairClass c, uint64_t stored, uint64_t restored,
                               const char* why, std::vector<uint64_t> candidates = {}) {
        rep.header.push_back({field, c, stored, restored, hamming(stored, restored), why, std::move(candidates)});
        rep.header_repaired += c == RepairClass::Corroborated;
    };
    const auto slot_verdict = [&blocks](Offset seat) -> const BlockVerdict* {
        const auto v = std::find_if(blocks.begin(), blocks.end(), [seat](const BlockVerdict& b) {
            return b.block.parent == HEADER_PARENT && b.block.field == seat;
        });
        return v == blocks.end() ? nullptr : &*v;
    };
    const auto settled = [](const BlockVerdict* v) {
        return v != nullptr && v->class_ != RepairClass::Ambiguous && v->class_ != RepairClass::Unrecovered;
    };

    // A constant is restored only where the bytes are demonstrably a FastFHIR
    // arena: a position holding its own offset occurs by chance at 2^-64, so
    // eight of them settle it.
    const auto anchored = std::count_if(B.anchors.begin(), B.anchors.end(), [](const auto& a) { return !a.second.lost; });
    const auto constant = [&](HeaderField field, uint64_t stored, uint64_t want) {
        const RepairClass c = stored == want                                                  ? RepairClass::Intact
                              : anchored >= 8 && hamming(stored, want) <= FF_RECOVERY_MAX_FLIPS ? RepairClass::Corroborated
                                                                                              : RepairClass::Unrecovered;
        record(field, c, stored, want, "a compiled constant, corroborated by the census");
    };
    constant(HeaderField::Magic, LOAD_U32(B.base + FF_HEADER::MAGIC), FF_MAGIC_BYTES);
    constant(HeaderField::Recovery, LOAD_U16(B.base + FF_HEADER::RECOVERY), RECOVER_FF_HEADER);

    // A closed set: the one legal member nearest the stored value. R4 and R5
    // are one bit apart, so a flip between them cannot be seen (F31).
    const uint16_t rev = LOAD_U16(B.base + FF_HEADER::FHIR_REV);
    const uint32_t d4 = hamming(rev, FHIR_VERSION_R4), d5 = hamming(rev, FHIR_VERSION_R5);
    const uint16_t nearest = d4 < d5 ? FHIR_VERSION_R4 : FHIR_VERSION_R5;
    if (d4 == d5)
        record(HeaderField::FhirRevision, RepairClass::Ambiguous, rev, rev, "equidistant from two legal revisions",
               {FHIR_VERSION_R4, FHIR_VERSION_R5});
    else
        record(HeaderField::FhirRevision,
               std::min(d4, d5) == 0 ? RepairClass::Intact
               : std::min(d4, d5) <= FF_RECOVERY_MAX_FLIPS ? RepairClass::Corroborated
                                                           : RepairClass::Unrecovered,
               rev, nearest, "the nearest legal revision");

    // The checksum footer is the last block a sealed stream holds.
    const uint64_t      size     = LOAD_U64(B.base + FF_HEADER::STREAM_SIZE);
    const BlockVerdict* checksum = slot_verdict(FF_HEADER::CHECKSUM_OFFSET);
    if (settled(checksum)) {
        const uint64_t sealed = checksum->block.child + FF_CHECKSUM::HEADER_SIZE;
        record(HeaderField::StreamSize, size == sealed ? RepairClass::Intact : RepairClass::Corroborated, size,
               sealed, "the checksum footer is the last block");
    } else {
        record(HeaderField::StreamSize, RepairClass::Unrecovered, size, size,
               "no checksum footer to measure the payload's end by");
    }

    const auto pointer = [&](HeaderField field, Offset seat) {
        const uint64_t      stored = LOAD_U64(B.base + seat);
        const BlockVerdict* v      = slot_verdict(seat);
        if (v == nullptr)
            record(field, RepairClass::Intact, stored, stored, "absent");
        else if (!settled(v))
            record(field, v->class_, stored, stored, "the block this names is not established",
                   std::vector<uint64_t>(v->candidates.begin(), v->candidates.end()));
        else
            record(field, v->block.child == stored ? RepairClass::Intact : RepairClass::Corroborated, stored,
                   v->block.child, "the block the header's slot names");
    };
    pointer(HeaderField::RootOffset, FF_HEADER::ROOT_OFFSET);
    const uint64_t      root_tag = LOAD_U16(B.base + FF_HEADER::ROOT_RECOVERY);
    const BlockVerdict* root     = slot_verdict(FF_HEADER::ROOT_OFFSET);
    if (settled(root))
        record(HeaderField::RootRecovery, root->block.declared == root_tag ? RepairClass::Intact : RepairClass::Corroborated,
               root_tag, root->block.declared, "the type of the block the root names");
    else
        record(HeaderField::RootRecovery, root ? root->class_ : RepairClass::Unrecovered, root_tag, root_tag,
               "the root is not established");
    pointer(HeaderField::ChecksumOffset, FF_HEADER::CHECKSUM_OFFSET);
    pointer(HeaderField::UrlDirectoryOffset, FF_HEADER::URL_DIR_OFFSET);
    pointer(HeaderField::ModuleRegistryOffset, FF_HEADER::MODULE_REG_OFFSET);

    const uint32_t word   = LOAD_U32(B.base + FF_HEADER::VERSION);
    const auto     layout = FF_HEADER_STREAM_LAYOUT(word);
    const bool     legal  = layout == FF_STREAM_COMPACTION_NONE || layout == FF_STREAM_COMPACTED;
    record(HeaderField::StreamLayout, legal ? RepairClass::Intact : RepairClass::Corroborated, layout,
           legal ? layout : FF_STREAM_COMPACTION_NONE, "one of the two layouts the writer emits");
    record(HeaderField::EngineVersion, RepairClass::Unrecovered, FF_HEADER_ENGINE_VERSION(word),
           FF_HEADER_ENGINE_VERSION(word), "no second witness: a newer writer is legitimate, so this reader must not choose");
}

}  // namespace

// =====================================================================
// PHASE 3 — apply(): the only writer
// =====================================================================

FF_ApplyReport Recovery::apply(const FF_RecoveryReport& report, std::vector<BYTE>& repaired,
                               const ApplyFilter& filter) const {
    FF_ApplyReport out;
    repaired.assign(m_base, m_base + m_size);  // the copy; the arena is untouched
    BYTE* const  dst = repaired.data();
    const size_t n   = repaired.size();
    std::vector<bool> touched(n, false);

    // Enact one group of writes as a unit: every seat in bounds and written by
    // no earlier group, then written, then `holds` asked of the result. A
    // group that fails is restored byte for byte.
    const auto enact = [&](const std::vector<PlannedWrite>& writes, const auto& holds, Offset edge) {
        const bool free = std::all_of(writes.begin(), writes.end(), [&](const PlannedWrite& w) {
            return in_extent(w.seat, w.width, n) &&
                   std::none_of(touched.begin() + static_cast<std::ptrdiff_t>(w.seat),
                                touched.begin() + static_cast<std::ptrdiff_t>(w.seat + w.width),
                                [](bool t) { return t; });
        });
        std::vector<uint64_t> before;
        if (free)
            for (const PlannedWrite& w : writes) {
                before.push_back(load(dst, w.seat, w.width));
                store(dst, w.seat, w.width, w.value);
            }
        if (free && holds()) {
            for (const PlannedWrite& w : writes)
                std::fill_n(touched.begin() + static_cast<std::ptrdiff_t>(w.seat), w.width, true);
            ++out.applied;
            return;
        }
        for (size_t k = before.size(); k-- > 0;)
            store(dst, writes[k].seat, writes[k].width, before[k]);
        ++out.failed;
        out.failed_edges.push_back(edge);
    };
    const auto reads_back = [dst](const std::vector<PlannedWrite>& writes) {
        return std::all_of(writes.begin(), writes.end(),
                           [dst](const PlannedWrite& w) { return load(dst, w.seat, w.width) == w.value; });
    };

    // The header's own fields first. Its pointer fields are written by their
    // references' verdicts below, together with the rest of each link.
    struct Seat { HeaderField field; Offset seat; uint8_t width; };
    constexpr Seat seats[] = {{HeaderField::Magic, FF_HEADER::MAGIC, 4},
                              {HeaderField::Recovery, FF_HEADER::RECOVERY, 2},
                              {HeaderField::FhirRevision, FF_HEADER::FHIR_REV, 2},
                              {HeaderField::StreamSize, FF_HEADER::STREAM_SIZE, 8},
                              {HeaderField::StreamLayout, FF_HEADER::VERSION, 4}};
    for (const HeaderVerdict& v : report.header) {
        // The root and the metadata offsets are absent from `seats`: their
        // writes belong to their references' verdicts, and land with those.
        const Seat* s = std::find_if(std::begin(seats), std::end(seats), [&v](const Seat& x) { return x.field == v.field; });
        if (v.class_ != RepairClass::Corroborated || s == std::end(seats) || n < FF_HEADER::HEADER_SIZE) {
            ++out.declined;
            continue;
        }
        const uint64_t value =
            v.field == HeaderField::StreamLayout
                ? FF_ENCODE_HEADER_VERSION(FF_HEADER_ENGINE_VERSION(LOAD_U32(dst + FF_HEADER::VERSION)),
                                           static_cast<FF_StreamCompaction>(v.restored))
                : v.restored;
        const std::vector<PlannedWrite> writes{{s->seat, s->width, value}};
        enact(writes, [&] { return reads_back(writes); }, s->seat);
    }

    // Then every selected link: the writes land, and the link holds -- its
    // child vouches for its own offset and carries the type the link names.
    for (const BlockVerdict& v : report.blocks) {
        if (v.writes.empty() || (filter && !filter(v))) {
            ++out.declined;
            continue;
        }
        const auto holds = [&] {
            return reads_back(v.writes) &&
                   (v.block.child == FF_NULL_OFFSET ||
                    (self_validates(dst, n, v.block.child) && tag_at(dst, n, v.block.child) == v.block.declared));
        };
        enact(v.writes, holds, v.block.child);
    }
    return out;
}

// =====================================================================
// THE CENSUS AS A PRODUCER — census(), reachable_blocks(), reachable_blocks_map()
// =====================================================================

Recovery::Census Recovery::census() const {
    const Board B    = take_census(m_base, m_size, scan());
    const auto  kids = children(B);
    Census c;
    c.extent  = static_cast<Size>(m_size);
    c.anchors = static_cast<std::size_t>(
        std::count_if(B.anchors.begin(), B.anchors.end(), [](const auto& a) { return !a.second.lost; }));
    for (const auto& [child, slot] : B.owner)
        c.edges.push_back({slot, child});
    std::sort(c.edges.begin(), c.edges.end(), [](const Edge& a, const Edge& b) { return a.slot.seat < b.slot.seat; });

    // The header's island, then one per self-validating block no decided link names.
    std::unordered_set<Offset> placed;
    const auto grow = [&](Offset root, bool attached_to_header) {
        Island island{root, attached_to_header, {}};
        std::vector<Offset> stack{root};
        while (!stack.empty()) {
            const Offset b = stack.back();
            stack.pop_back();
            if (!placed.insert(b).second)
                continue;
            island.members.push_back(b);
            if (const auto k = kids.find(b); k != kids.end())
                stack.insert(stack.end(), k->second.rbegin(), k->second.rend());
        }
        c.islands.push_back(std::move(island));
    };
    if (m_size >= FF_HEADER::HEADER_SIZE)
        grow(HEADER_PARENT, true);
    for (const auto& [off, a] : B.anchors)
        if (!a.lost && !B.owner.contains(off) && !placed.contains(off))
            grow(off, false);
    c.holes = B.holes;
    for (const Question& q : B.questions)
        c.points.push_back(q.point);
    const auto key = [](const Point& p) {
        return p.kind == PointKind::Open ? p.slot.seat : p.array + FF_ARRAY::KIND_AND_STEP;
    };
    std::sort(c.points.begin(), c.points.end(), [&key](const Point& a, const Point& b) { return key(a) < key(b); });
    return c;
}

// The references the census decided and the header reaches. On a clean stream
// that is every reference in it, which makes this the clean-stream baseline.
std::vector<BlockRef> Recovery::reachable_blocks() const {
    const Board                      B       = take_census(m_base, m_size, scan());
    const std::vector<Offset>        members = attached(B, children(B));
    const std::unordered_set<Offset> reach(members.begin(), members.end());
    std::vector<BlockRef> out;
    for (const auto& [child, slot] : B.owner)
        if (reach.contains(slot.parent))
            out.push_back({slot.parent, slot.seat - slot.parent, slot.kind, child,
                           child_wire(slot, slot.repr == SlotRepr::Tuple ? slot.stored_tag : slot.expect),
                           B.anchors.at(child).tag});
    std::sort(out.begin(), out.end(),
              [](const BlockRef& a, const BlockRef& b) { return a.parent + a.field < b.parent + b.field; });
    return out;
}

StreamMap Recovery::reachable_blocks_map() const {
    const Board B = take_census(m_base, m_size, scan());
    StreamMap map;
    map.file_size = static_cast<Size>(m_size);
    for (const Offset m : attached(B, children(B)))
        map[m] = m == HEADER_PARENT
                     ? StreamMapEntry{StreamMapEntryType::Header, 0, FF_HEADER::HEADER_SIZE, RECOVER_FF_HEADER}
                     : classify_block(m_base, m_size, m);
    find_gaps(map);
    return map;
}

// =====================================================================
// THE BYTE CENSUS — scan() and find_gaps()
// =====================================================================

// Every position whose eight bytes hold its own offset. A random word does so
// with probability 2^-64, so this alone finds every block whose self-offset
// survived. Split across workers when the arena is large enough to pay for
// them; chunks overlap by a header's width so a block on a boundary is seen.
StreamMap Recovery::scan() const {
    StreamMap map;
    map.file_size = static_cast<Size>(m_size);
    if (has_magic(m_base, m_size))
        map[0] = {StreamMapEntryType::Header, 0, FF_HEADER::HEADER_SIZE, RECOVER_FF_HEADER};

    const unsigned hw      = std::thread::hardware_concurrency();
    const size_t   workers = (m_size >= (1u << 20) && hw > 1) ? std::min<size_t>(hw, 8u) : 1;
    const size_t   chunk   = (m_size + workers - 1) / workers;
    std::vector<std::vector<Offset>> found(workers);
    std::vector<std::exception_ptr>  errors(workers);
    const auto sweep = [&](size_t w) {
        try {
            const size_t end = std::min(m_size, (w + 1) * chunk + (w + 1 < workers ? DATA_BLOCK::HEADER_SIZE - 1 : 0));
            for (size_t off = w * chunk; off + DATA_BLOCK::HEADER_SIZE <= end; ++off)
                if (LOAD_U64(m_base + off) == off)
                    found[w].push_back(static_cast<Offset>(off));
        } catch (const std::exception&) {
            errors[w] = std::current_exception();  // rethrown on the calling thread below
        }
    };
    std::vector<std::thread> threads;
    for (size_t w = 1; w < workers; ++w)
        threads.emplace_back(sweep, w);
    sweep(0);
    for (std::thread& t : threads)
        t.join();
    for (const std::exception_ptr& e : errors)
        if (e)
            std::rethrow_exception(e);

    for (const std::vector<Offset>& offsets : found)
        for (const Offset off : offsets) {
            if (map.contains(off))
                continue;  // the header, or a chunk overlap
            map[off] = classify_block(m_base, m_size, off);
            if (!plausible_tag(map[off].recovery))
                map.failures.push_back({ProducerFailureKind::ScanTagInvalid, off, FF_RECOVER_UNDEFINED,
                                        map[off].recovery,
                                        "the self-offset holds, but the tag is no type this build knows"});
        }
    find_gaps(map);
    return map;
}

// Tile the arena and classify every run of bytes no entry covers (REC-18). A
// run is benign version skew only when the stream is newer than this reader
// and every block of the tag before it trails the same run: a longer V-Table,
// not damage. After the last entry it is slack; otherwise it is a hole.
void Recovery::find_gaps(StreamMap& map) const {
    map.gaps.clear();
    if (map.empty())
        return;
    if (has_magic(m_base, m_size) && FF_HEADER(m_size).get_stream_layout(m_base) == FF_STREAM_COMPACTED)
        return;  // the compact layout's geometry is another matter (handoff §11)

    // Each entry covers its extent, but never past the next entry: a block
    // never contains another block's first byte, and a stamped count or length
    // that damage inflated would otherwise swallow every hole behind it.
    struct Run { Offset start; Size length; RECOVERY_TAG after; };
    std::vector<Run> runs;
    std::unordered_map<uint16_t, size_t> instances;
    uint64_t     cursor = 0;
    RECOVERY_TAG after  = FF_RECOVER_UNDEFINED;
    for (auto it = map.begin(); it != map.end(); ++it) {
        const auto& [off, e] = *it;
        if (off > cursor)
            runs.push_back({static_cast<Offset>(cursor), static_cast<Size>(off - cursor), after});
        const auto next = std::next(it);
        const uint64_t end = static_cast<uint64_t>(off) + e.size;
        cursor = std::max<uint64_t>(cursor, next == map.end() ? end : std::min<uint64_t>(end, next->first));
        after  = e.recovery;
        ++instances[static_cast<uint16_t>(after)];
    }
    if (map.file_size > cursor)
        runs.push_back({static_cast<Offset>(cursor), static_cast<Size>(map.file_size - cursor), after});

    const bool newer = stream_is_newer(m_base, m_size);
    std::map<std::pair<uint16_t, Size>, size_t> trails;  // (tag, run length) -> how many
    for (const Run& r : runs)
        ++trails[{static_cast<uint16_t>(r.after), r.length}];
    for (const Run& r : runs) {
        const size_t same = trails[{static_cast<uint16_t>(r.after), r.length}];
        Gap g{r.start, r.length, r.after, GapClass::Hole, "unattributed bytes"};
        if (static_cast<uint64_t>(r.start) + r.length >= map.file_size) {
            g.class_ = GapClass::Trailing;
            g.why    = "arena slack past the last entry";
        } else if (newer && r.after != FF_RECOVER_UNDEFINED && same > 1 &&
                   same == instances[static_cast<uint16_t>(r.after)]) {
            g.class_ = GapClass::VersionSkew;
            g.why    = "every block of this tag trails the same run, and the stream is newer";
        } else if (r.length < DATA_BLOCK::HEADER_SIZE) {
            g.why = "unattributed, but too small to have held a block header";
        }
        map.gaps.push_back(g);
    }
}

// =====================================================================
// LEAF HELPERS
// =====================================================================

// A generated block's V-Table extent under this build's reflection table: the
// widest field offset plus slot width. An older reader under-sizes a newer
// writer's block this way, which find_gaps() expects.
Size Recovery::derived_block_size(RECOVERY_TAG tag) noexcept {
    Size widest = DATA_BLOCK::HEADER_SIZE;
    for (const FF_FieldInfo& f : reflected_fields_view(static_cast<uint16_t>(tag)))
        widest = std::max<Size>(widest, static_cast<Size>(f.field_offset) + ff_slot_width(f.kind));
    return widest;
}

uint32_t Recovery::hamming_cost(uint64_t a, uint64_t b) noexcept {
    return hamming(a, b);
}

bool Recovery::plausible_tag(RECOVERY_TAG tag) noexcept {
    return tag != FF_RECOVER_UNDEFINED && Recovery_to_Kind(tag) != FF_FIELD_UNKNOWN;
}

namespace {

// THE EXTENT, from the one fact the header states twice. STREAM_SIZE and
// CHECKSUM_OFFSET + FF_CHECKSUM::HEADER_SIZE agree on every sealed stream,
// because the checksum footer is the last block the writer lays down. A claim
// is believed only when the bytes it implies vouch for themselves as that
// footer, which random bytes do at 2^-64; otherwise the ceiling, which no wire
// value sets. Without this, one flipped bit in STREAM_SIZE set the extent to a
// 4 GiB sparse reservation, and the byte census swept 4 GiB for 3 MB of data.
size_t trusted_extent(const BYTE* base, uint64_t claimed, uint64_t ceiling) noexcept {
    if (base != nullptr && ceiling >= FF_HEADER::HEADER_SIZE) {
        const auto seals_at = [base, ceiling](uint64_t end) {
            if (end < static_cast<uint64_t>(FF_HEADER::HEADER_SIZE) + FF_CHECKSUM::HEADER_SIZE || end > ceiling)
                return false;
            const uint64_t seat = end - FF_CHECKSUM::HEADER_SIZE;
            return LOAD_U64(base + seat) == seat && FF_GET_RECOVERY_TAG(base, seat) == RECOVER_FF_CHECKSUM;
        };
        if (seals_at(claimed))
            return static_cast<size_t>(claimed);
        const uint64_t implied = LOAD_U64(base + FF_HEADER::CHECKSUM_OFFSET) + FF_CHECKSUM::HEADER_SIZE;
        if (seals_at(implied))
            return static_cast<size_t>(implied);
    }
    return static_cast<size_t>(std::min(claimed, ceiling));
}

}  // namespace

// Memory::size() reads the write head, which lives in the same eight bytes as
// FF_HEADER::STREAM_SIZE, so on a damaged stream it is damaged data. The
// ceiling is what the OS reports for the backing file, or the arena's
// reservation for an anonymous one; neither is read from the stream.
Recovery::Recovery(const Memory& memory) noexcept
    : m_base(memory->base()),
      m_size(trusted_extent(memory->base(), memory->size(),
                            memory->disk_size() != 0 ? memory->disk_size() : memory->capacity())) {}

}  // namespace FastFHIR
