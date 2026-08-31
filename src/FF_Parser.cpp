/**
 * @file FF_Parser.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief 
 * @version 0.1
 * @date 2026-03-18
 * 
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * 
 */

#include "FF_Utilities.hpp"
#include "FF_Ops.hpp"
#include "FF_Parser.hpp"
#include "FF_SIMD.hpp"
#include "FF_Dictionary.hpp"
#include "FF_Reflection.hpp"
#include "FF_FieldKeys.hpp"  // FastFHIR::Fields (Bundle.entry, BundleEntry.resource) for Recovery
#include <assert.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <unordered_set>

namespace FastFHIR {
namespace Reflective {
class Node;
struct Entry;

using NodeSizeFn = size_t (*)(const Node&);
using NodeEntriesFn = std::vector<Node> (*)(const Node&);
using NodeLookupFieldFn = Entry (*)(const Node&, FF_FieldKey);
using NodeLookupIndexFn = Node (*)(const Node&, size_t);
using EntryAsNodeFn = Node (*)(const Entry&, Size, uint32_t, RECOVERY_TAG, FF_FieldKind, const ParserOps*);

// ParserOps is the vtable that makes one parser drive both stream layouts.
// The FF wire format has two physically different encodings:
//
//   STANDARD  — every block carries a compiled V-Table of FF_FieldInfo
//               (reflected_fields_view), so the reader knows a block's shape
//               from its RECOVERY_TAG alone and walks slots by offset.
//   COMPACT   — the V-Table is replaced by a per-type presence bitmap
//               (compact_presence_bytes) and a denser slot layout, so the
//               reader must consult the bitmap to know which slots exist and
//               how wide they are.
//
// Same FHIR data, two different ways to read a node: how big it is
// (node_size), what its children are (node_entries), how a named field or an
// index resolves (node_lookup_field / node_lookup_index), and how a flat
// Entry expands into a navigable Node (entry_as_node). Each member has a
// standard_* and a compact_* implementation below; the ParserOps chosen at
// construction (standard_ops_ptr / compact_ops_ptr) is the one dispatch point
// that keeps the rest of the parser layout-agnostic. layout records which
// branch is active so callers can branch on it (e.g. gap analysis refuses
// compact streams).
struct ParserOps {
    FF_StreamCompaction layout;
    NodeSizeFn node_size;
    NodeEntriesFn node_entries;
    NodeLookupFieldFn node_lookup_field;
    NodeLookupIndexFn node_lookup_index;
    EntryAsNodeFn entry_as_node;

    static size_t standard_node_size(const Node& n);
    static std::vector<Node> standard_node_entries(const Node& n);
    static Entry standard_node_lookup_field(const Node& n, FF_FieldKey key);
    static Node standard_node_lookup_index(const Node& n, size_t index);
    static Node standard_entry_as_node(const Entry& e, Size size, uint32_t version,
                                       RECOVERY_TAG expected_tag, FF_FieldKind schema_kind,
                                       const ParserOps* ops);

    // Shared by every path that turns a CODE slot into a Node -- the two
    // entry_as_node implementations and resolve_choice. See the definition.
    static Node code_node(const BYTE* base, Size size, uint32_t version,
                          Offset block_offset, Offset slot_offset,
                          const ParserOps* ops, uint32_t engine_ver);

    // The single decision point for "what is array element i?" -- both
    // standard_node_entries and standard_node_lookup_index route through it.
    // See the definition.
    static Node array_element(const Node& n, const FF_ARRAY& arr,
                              RECOVERY_TAG elem, uint32_t index);
    static RECOVERY_TAG array_element_tag(const Node& n);

    static size_t compact_node_size(const Node& n);
    static std::vector<Node> compact_node_entries(const Node& n);
    static Entry compact_node_lookup_field(const Node& n, FF_FieldKey key);
    static Node compact_node_lookup_index(const Node& n, size_t index);
    static Node compact_entry_as_node(const Entry& e, Size size, uint32_t version,
                                      RECOVERY_TAG expected_tag, FF_FieldKind schema_kind,
                                      const ParserOps* ops);
};

static const Entry NULL_ENTRY = {nullptr, FF_NULL_OFFSET, 0, FF_RECOVER_UNDEFINED, FF_FIELD_UNKNOWN};
static const ParserOps* standard_ops_ptr();
static const ParserOps* compact_ops_ptr();

// compact_presence_bytes / compact_presence_contains live in FF_SIMD.hpp --
// one definition shared with the compactor (writer) so the bitmap layout
// cannot drift between the two sides.

size_t ParserOps::compact_node_size(const Node& n) {
    // Arrays keep the existing FF_ARRAY layout in compact mode for now.
    return standard_node_size(n);
}

std::vector<Node> ParserOps::compact_node_entries(const Node& n) {
    // Array traversal reuses existing FF_ARRAY geometry.
    return standard_node_entries(n);
}

Node ParserOps::compact_node_lookup_index(const Node& n, size_t index) {
    // Array traversal reuses existing FF_ARRAY geometry.
    return standard_node_lookup_index(n, index);
}

Entry ParserOps::compact_node_lookup_field(const Node& n, FF_FieldKey key) {
    if (!n.is_object()) return NULL_ENTRY;

    const RECOVERY_TAG owner_recovery = GetTypeFromTag(key.owner_recovery);
    if (owner_recovery != FF_RECOVER_UNDEFINED && owner_recovery != n.m_recovery) {
        if (!(owner_recovery == RECOVER_FF_RESOURCE && FF_IsResourceTag(n.m_recovery))) {
            return NULL_ENTRY;
        }
    }

    const auto f_list = reflected_fields_view(n.m_recovery);
    if (f_list.empty()) return NULL_ENTRY;

    const uint8_t* sizes_table = compact_field_sizes(n.m_recovery);
    if (!sizes_table) return NULL_ENTRY;

    size_t target_index = SIZE_MAX;
    FF_FieldInfo target_field{};
    for (size_t i = 0; i < f_list.size(); ++i) {
        if (f_list[i].field_offset == key.field_offset) {
            target_index = i;
            target_field = f_list[i];
            break;
        }
    }
    if (target_index == SIZE_MAX) return NULL_ENTRY;

    const Offset presence_start = n.m_node_offset + DATA_BLOCK::HEADER_SIZE;
    const BYTE* presence = n.m_base + presence_start;
    if (!compact_presence_contains(presence, target_index)) return NULL_ENTRY;

    const Offset dense_start = presence_start + compact_presence_bytes(f_list.size());
    const Offset rel = ff_compact_dense_offset(presence, sizes_table, target_index);

    const Offset slot_offset = dense_start + rel;
    RECOVERY_TAG target_tag = key.child_recovery;
    FF_FieldKind out_kind = key.kind;
    if (out_kind == FF_FIELD_UNKNOWN) out_kind = target_field.kind;
    if (key.kind == FF_FIELD_CHOICE) {
        // Same rule as the standard path: the runtime variant tag in the
        // slot is authoritative; the static child_recovery only names the
        // first variant. The presence bitmap above guarantees the slot is
        // present, so the runtime tag is valid here — never FF_RECOVER_UNDEFINED.
        target_tag = FF_GET_RECOVERY_TAG(n.m_base, slot_offset);
    } else if (target_tag == FF_RECOVER_UNDEFINED && out_kind != FF_FIELD_UNKNOWN) {
        target_tag = Kind_to_Recovery(out_kind);
    }

    const ParserOps* child_ops = compact_ops_ptr();
    return Entry(
        n.m_base,
        n.m_node_offset,
        static_cast<uint32_t>(slot_offset - n.m_node_offset),
        target_tag,
        out_kind,
        n.m_size,
        n.m_version,
        child_ops,
        n.m_engine_version
    );
}

// A code slot holds either an inline dictionary id or, when
// FF_CODEABLE_CONCEPT_FLAG is set, a signed relative offset to an
// FF_CODEABLE_CONCEPT block -- relative to the CONTAINING BLOCK, the convention
// ENCODE_FF_CODE writes and the compactor and both Entry readers already use.
//
// The flagged case is resolved HERE, at the last point where the containing
// block is known: a Node carries only its own offset, so any caller that defers
// this arithmetic has already lost an operand. Deferring it is precisely what
// went wrong -- Node::as<string_view>() resolved against the node's own offset,
// which for a choice variant is the SLOT, landing a V-Table width away from the
// real block and decoding whatever happened to sit there.
Node ParserOps::code_node(const BYTE* base, Size size, uint32_t version,
                          Offset block_offset, Offset slot_offset,
                          const ParserOps* ops, uint32_t engine_ver) {
    const uint32_t raw = LOAD_U32(base + slot_offset);
    if (raw != FF_CODE_NULL && (raw & FF_CODEABLE_CONCEPT_FLAG)) {
        // Kind stays FF_FIELD_CODE so print_json still treats this as a coded
        // leaf; the RECOVERY tag is what tells as<>() the arithmetic is done.
        return Node(base, size, version,
                    FF_ResolveCodeableConceptOffset(raw, block_offset),
                    RECOVER_FF_CODEABLE_CONCEPT, FF_FIELD_CODE,
                    FF_RECOVER_UNDEFINED, false, ops, engine_ver);
    }
    return Node(base, size, version, slot_offset, RECOVER_FF_CODE, FF_FIELD_CODE,
                FF_RECOVER_UNDEFINED, false, ops, engine_ver);
}

Node ParserOps::compact_entry_as_node(const Entry& e, Size size, uint32_t version,
                                      RECOVERY_TAG expected_tag, FF_FieldKind schema_kind,
                                      const ParserOps* ops) {
    if (e.base == nullptr || e.absolute_offset() == FF_NULL_OFFSET) return {};

    const Offset slot_offset = e.absolute_offset();
    const ParserOps* compact_ops = ops ? ops : compact_ops_ptr();

    switch (schema_kind) {
        case FF_FIELD_BOOL:
        case FF_FIELD_INT32:
        case FF_FIELD_UINT32:
        case FF_FIELD_INT64:
        case FF_FIELD_UINT64:
        case FF_FIELD_FLOAT64:
        case FF_FIELD_URL:
            return Node(e.base, size, version, slot_offset, expected_tag, schema_kind,
                        FF_RECOVER_UNDEFINED, false, compact_ops, e.m_engine_version);

        case FF_FIELD_CODE:
            return code_node(e.base, size, version, e.parent_offset, slot_offset,
                             compact_ops, e.m_engine_version);

        case FF_FIELD_DATETIME: {
            // DT-2: same discriminator as code_node, one width up — packed/null
            // values are inline, bit 63 set is a relative offset to an FF_STRING.
            const uint64_t raw = LOAD_U64(e.base + slot_offset);
            if (raw == FF_DATETIME_NULL || !FF_DATETIME_IS_FALLBACK(raw)) {
                return Node(e.base, size, version, slot_offset, expected_tag,
                            schema_kind, FF_RECOVER_UNDEFINED, false, compact_ops,
                            e.m_engine_version);
            }
            return Node(e.base, size, version,
                        FF_ResolveDateTimeOffset(raw, e.parent_offset),
                        RECOVER_FF_STRING, FF_FIELD_STRING,
                        FF_RECOVER_UNDEFINED, false, compact_ops, e.m_engine_version);
        }

        case FF_FIELD_CHOICE:
            // parent_offset, not slot_offset: the fallback offset inside a
            // choice variant is relative to the containing block (see
            // resolve_choice). Passing the slot made it self-relative.
            return Node::resolve_choice(e.base, size, version, e.parent_offset, slot_offset, schema_kind, compact_ops);

        case FF_FIELD_RESOURCE: {
            Offset child_offset = LOAD_U64(e.base + slot_offset);
            if (child_offset == FF_NULL_OFFSET) return {};
            RECOVERY_TAG actual_tag = FF_GET_RECOVERY_TAG(e.base, slot_offset);
            // Same rule as the standard path: the kind follows the tag, so an
            // out-of-profile resource retained as opaque JSON is walked as the
            // string-layout block it is.
            return Node(e.base, size, version, child_offset, actual_tag,
                        Recovery_to_Kind(actual_tag) == FF_FIELD_STRING ? FF_FIELD_STRING
                                                                        : FF_FIELD_BLOCK,
                        FF_RECOVER_UNDEFINED, false, compact_ops, e.m_engine_version);
        }

        case FF_FIELD_ARRAY: {
            Offset child_offset = LOAD_U64(e.base + slot_offset);
            if (child_offset == FF_NULL_OFFSET) return {};
            FF_ARRAY arr_hdr(child_offset, size, version, e.m_engine_version);
            bool entries_are_offsets = arr_hdr.entries_are_pointers(e.base);
            return Node(e.base, size, version, child_offset, expected_tag, schema_kind,
                        expected_tag, entries_are_offsets, compact_ops, e.m_engine_version);
        }

        case FF_FIELD_STRING: {
            Offset child_offset = LOAD_U64(e.base + slot_offset);
            if (child_offset == FF_NULL_OFFSET) return {};
            // The stored tag, not RECOVER_FF_STRING assumed: an opaque-JSON
            // payload shares this layout and must keep its own identity, or the
            // render sites quote and escape a whole resource.
            RECOVERY_TAG actual_tag = FF_GET_RECOVERY_TAG(e.base, child_offset);
            return Node(e.base, size, version, child_offset, actual_tag, schema_kind,
                        FF_RECOVER_UNDEFINED, false, standard_ops_ptr(), e.m_engine_version);
        }

        default: {
            Offset child_offset = LOAD_U64(e.base + slot_offset);
            if (child_offset == FF_NULL_OFFSET) return {};
            RECOVERY_TAG actual_tag = FF_GET_RECOVERY_TAG(e.base, child_offset);
            // The recovery tag is ground truth, exactly as in the standard path
            // above -- and this branch is missing that rule was a live defect,
            // not a hypothetical. `Attachment.data` declares schema kind
            // FF_FIELD_BLOCK with child_recovery RECOVER_FF_STRING (the
            // complex-block mapping for base64Binary), so the compact reader
            // built a BLOCK node over an FF_STRING, `fields()` asked
            // reflected_fields_view for a string's V-Table, got {}, and
            // print_json read the empty field list as "no members present" and
            // dropped every DiagnosticReport attachment from the compact export.
            // The standard path fixed this in A23.3 ("Bug C"); the compact path
            // was never given the same correction, and nothing compacted a real
            // document until COV-1.5.
            FF_FieldKind child_kind = schema_kind;
            if (FF_IsStringLayoutTag(actual_tag)) child_kind = FF_FIELD_STRING;
            return Node(e.base, size, version, child_offset, actual_tag, child_kind,
                        FF_RECOVER_UNDEFINED, false, compact_ops, e.m_engine_version);
        }
    }
}

static const ParserOps STANDARD_OPS{
    FF_STREAM_COMPACTION_NONE,
    &ParserOps::standard_node_size,
    &ParserOps::standard_node_entries,
    &ParserOps::standard_node_lookup_field,
    &ParserOps::standard_node_lookup_index,
    &ParserOps::standard_entry_as_node,
};

static const ParserOps COMPACT_OPS{
    FF_STREAM_COMPACTED,
    &ParserOps::compact_node_size,
    &ParserOps::compact_node_entries,
    &ParserOps::compact_node_lookup_field,
    &ParserOps::compact_node_lookup_index,
    &ParserOps::compact_entry_as_node,
};

static const ParserOps* standard_ops_ptr() {
    return &STANDARD_OPS;
}

static const ParserOps* compact_ops_ptr() {
    return &COMPACT_OPS;
}

static const ParserOps* select_ops(FF_StreamCompaction layout) {
    switch (layout) {
        case FF_STREAM_COMPACTION_NONE: return &STANDARD_OPS;
        case FF_STREAM_COMPACTED:  return &COMPACT_OPS;
        default:                        return &STANDARD_OPS;
    }
}
} // namespace Reflective

// =====================================================================
// Parser implementation
// =====================================================================
// Both constructors follow identical logic: FF_HEADER is unconditionally at
// offset 0 (no preamble detection required). After validate_full(), the two
// optional block offsets (URL_DIR_OFFSET, MODULE_REG_OFFSET) are read from
// the header. Both default to FF_NULL_OFFSET, meaning the corresponding
// feature is absent in this stream.
// The root offset is the entry point to every traversal, so it is the one
// offset worth checking before it is stored. FF_NULL_OFFSET is legitimate --
// it is the "no root" sentinel m_root_offset is default-initialised to -- but
// any other value must address a block that actually fits in the buffer.
// Checking `off < size` alone would admit an offset in the last nine bytes,
// where the universal 10-byte block header cannot fit, so the bound is against
// DATA_BLOCK::HEADER_SIZE rather than against size itself.
static Offset checked_root_offset(Offset root, size_t size) {
    if (root == FF_NULL_OFFSET) return root;
    if (root < FF_HEADER::HEADER_SIZE ||
        root > size - DATA_BLOCK::HEADER_SIZE) {
        throw std::runtime_error(
            "FastFHIR Parsing Error: ROOT_OFFSET " + std::to_string(root) +
            " is out of bounds; a root block must lie within [" +
            std::to_string(FF_HEADER::HEADER_SIZE) + ", " +
            std::to_string(size - DATA_BLOCK::HEADER_SIZE) + "] for a " +
            std::to_string(size) + "-byte stream.");
    }
    return root;
}

Parser::Parser(const void* buffer, size_t size) : m_memory(), m_base(static_cast<const BYTE*>(buffer)), m_size(size) {
    if (size < FF_HEADER::HEADER_SIZE) {
        throw std::runtime_error("FastFHIR Parsing Error: Buffer too small to contain a valid header.");
    }
    FF_HEADER header(size);
    auto validation_result = header.validate_full(m_base);
    if (validation_result != FF_SUCCESS) {
        throw std::runtime_error("FastFHIR Parsing Error: Header validation failed with error " + validation_result.message);
    }
    m_version            = header.get_fhir_rev(m_base);
    m_engine_version     = header.get_engine_version(m_base);
    m_stream_layout      = header.get_stream_layout(m_base);
    m_ops                = Reflective::select_ops(m_stream_layout);
    m_root_offset        = checked_root_offset(header.get_root(m_base), m_size);
    m_root_recovery      = header.get_root_type(m_base);
    m_url_dir_offset     = header.get_url_dir_offset(m_base);    // FF_NULL_OFFSET if no extension URLs
    m_module_reg_offset  = header.get_module_reg_offset(m_base); // FF_NULL_OFFSET until Phase 7
}

Parser::Parser(const Memory& memory) : m_memory(memory), m_base(memory.base()), m_size(memory.size()) {
    if (m_size < FF_HEADER::HEADER_SIZE) {
        throw std::runtime_error("FastFHIR Parsing Error: Buffer too small to contain a valid header.");
    }
    FF_HEADER header(m_size);
    auto validation_result = header.validate_full(m_base);
    if (validation_result != FF_SUCCESS) {
        throw std::runtime_error("FastFHIR Parsing Error: Header validation failed with code " + validation_result.message);
    }
    m_version            = header.get_fhir_rev(m_base);
    m_engine_version     = header.get_engine_version(m_base);
    m_stream_layout      = header.get_stream_layout(m_base);
    m_ops                = Reflective::select_ops(m_stream_layout);
    m_root_offset        = checked_root_offset(header.get_root(m_base), m_size);
    m_root_recovery      = header.get_root_type(m_base);
    m_url_dir_offset     = header.get_url_dir_offset(m_base);
    m_module_reg_offset  = header.get_module_reg_offset(m_base);
}

uint32_t Parser::version()   const { return m_version; }
uint16_t Parser::root_type() const { return m_root_recovery; }

Parser::ChecksumValidation Parser::checksum() const {
    FF_HEADER header(m_size);
    FF_CHECKSUM cs = header.get_checksum(m_base);

    const Size cs_header_size = cs.get_header_size();

    if (!cs || cs.__offset + cs_header_size > m_size) {
        throw std::runtime_error("FastFHIR Parsing Error: Invalid checksum metadata in header.");
    }

    return {
        m_base,
        static_cast<size_t>(cs.__offset),
        cs.get_algorithm(m_base),
        cs.get_hash_view(m_base)
    };
}

// =====================================================================
// XP-2.3 — Parser::validate_FFHR_stream()
//
// Explicit, never automatic. Construction stays O(1) so Builder::query() and
// the ordinary open path pay nothing; a caller reading bytes it did not produce
// asks for this walk deliberately. (IFE's split, which this mirrors:
// validate_FFHR_stream is a call, not a constructor side effect.)
//
// Everything here reads RAW OFFSETS and bounds-checks them BEFORE addressing
// the bytes. It deliberately does not go through Node/Entry: that lens assumes
// the offsets it is handed are already trustworthy, which is exactly the
// assumption under test. The per-block checks mirror DATA_BLOCK::validate_offset
// (self-offset word, recovery tag) but are open-coded so a failure can name the
// offset it was reached from.
//
// Cycle and depth policy is XP-1's, and MAX_VALIDATION_DEPTH is deliberately
// the same 64 as the compactor's MAX_NODE_DEPTH: both bound the same FHIR
// object graph, so they must not drift. `done` is recorded on COMPLETION, never
// on entry -- marking on entry would let a block that reaches itself find its
// own entry and report success.
namespace {

constexpr std::size_t MAX_VALIDATION_DEPTH = 64;

// Which V-Table slot kinds can hold an offset into the arena. Everything not
// listed here is inline data and structurally inert.
static inline bool slot_carries_offset(FF_FieldKind k) {
    switch (k) {
        case FF_FIELD_STRING:
        case FF_FIELD_BLOCK:
        case FF_FIELD_ARRAY:
        case FF_FIELD_RESOURCE:
        case FF_FIELD_CHOICE:
        case FF_FIELD_CODE:      // only when FF_CODEABLE_CONCEPT_FLAG is set
        case FF_FIELD_DATETIME:  // only when FF_DATETIME_FALLBACK_FLAG is set
            return true;
        default:
            return false;
    }
}

struct DeepValidator {
    const BYTE* base = nullptr;
    Size        size = 0;
    std::vector<Offset>        path;
    // "Already fully validated" set. A node-based unordered_set costs a hash
    // and an allocation per block, and this walk visits every block in the
    // stream -- 913,809 of them in a 50 MB fixture, where that overhead was
    // more than half the total runtime. A bit per byte-offset removes both,
    // but costs size/8 bytes, so it is only used when that is affordable;
    // above the threshold we fall back to a pre-reserved hash set.
    // Structural pass only, or also inspect inline scalar values. Scalars
    // cannot point anywhere: they are data inside a V-Table this walk has
    // already bounds-checked as a whole, so they are irrelevant to "is this
    // stream trying to make me read memory I do not own". Skipping them is
    // what separates validate_FFHR_stream() from its _deep() counterpart.
    bool check_scalars = false;
    uint32_t version = 0;   // FHIR revision, for dictionary resolution in _deep()

    static constexpr Size BITSET_MAX_STREAM = 1ull << 30;   // 1 GiB -> 128 MiB bits
    std::vector<uint64_t>      done_bits;
    std::unordered_set<Offset> done_set;
    bool use_bits = false;

    void init_visited(Size size) {
        use_bits = size <= BITSET_MAX_STREAM;
        if (use_bits) done_bits.assign((size >> 6) + 2, 0);
        else          done_set.reserve(1u << 20);
    }
    bool seen(Offset o) const {
        return use_bits ? ((done_bits[o >> 6] >> (o & 63)) & 1ull) != 0
                        : done_set.count(o) != 0;
    }
    void mark(Offset o) {
        if (use_bits) done_bits[o >> 6] |= 1ull << (o & 63);
        else          done_set.insert(o);
    }
    // The recursion returns bool and threads ONE FF_Result by reference; it
    // never returns FF_Result by value. FF_Result carries a std::string, and
    // materialising one at every level of a walk that visits ~900k blocks with
    // ~20 fields each meant millions of string constructions to report success.
    // Measured: that alone was ~180 ms of the 186 ms this walk used to cost on
    // a 50 MB stream -- 50x more than touching every block header in random
    // order (3.7 ms). The result is written once, by the frame that fails, and
    // floats up untouched as each caller returns false.
    //
    // Threading it by reference rather than parking it on the validator was
    // measured against that alternative: 105-107 ms either way, i.e. identical.
    // The reference wins on being visible in the signature instead of hidden
    // state, not on speed.
    static bool fail(FF_Result& out, std::string msg) {
        out = {FF_VALIDATION_FAILURE, "FastFHIR Validation Error: " + std::move(msg)};
        return false;
    }
    // `need <= size` first: size - need would wrap on a short buffer.
    bool fits(Offset off, Size need) const {
        return need <= size && off >= FF_HEADER::HEADER_SIZE && off <= size - need;
    }

    bool walk(Offset off, RECOVERY_TAG expected, std::size_t depth, const char* via,
              FF_Result& out);
    bool walk_array(Offset off, RECOVERY_TAG array_tag, std::size_t depth, FF_Result& out);
    bool walk_fields(Offset off, RECOVERY_TAG tag, std::size_t depth, FF_Result& out);

    // The two MSB-discriminated value types, factored out because each is
    // reachable through TWO slot shapes: its own dedicated V-Table slot, and a
    // choice ([x]) slot whose active variant happens to be that type. Writing
    // the check twice would mean a fix to one shape silently missing the other,
    // which is exactly how the choice path came to be unvalidated in the first
    // place. `block` is the containing block: the fallback offset is signed and
    // relative to it in both shapes, which is the convention ENCODE_FF_CODE,
    // the compactor and Entry all use.
    bool check_code_value(Offset block, uint32_t raw, std::size_t depth, const char* via,
                          FF_Result& out);
    bool check_datetime_value(Offset block, uint64_t raw, std::size_t depth, const char* via,
                              FF_Result& out);
};

bool DeepValidator::check_code_value(Offset block, uint32_t raw, std::size_t depth,
                                     const char* via, FF_Result& out) {
    if (raw == FF_CODE_NULL) return true;
    if ((raw & FF_CODEABLE_CONCEPT_FLAG) == 0) {
        // Plain dictionary id: structurally inert, so only _deep() checks that
        // it actually resolves to a code.
        const char* resolved = FF_ResolveCode(raw, version);
        if (check_scalars && (resolved == nullptr || *resolved == 0)) {
            return fail(out, "code slot '" + std::string(via) + "' holds dictionary id " +
                        std::to_string(raw) + ", which resolves to nothing");
        }
        return true;
    }
    const int32_t rel = static_cast<int32_t>(raw << 1) >> 1;
    const Offset cc = block + static_cast<Offset>(static_cast<int64_t>(rel));
    return walk(cc, RECOVER_FF_CODEABLE_CONCEPT, depth + 1, via, out);
}

bool DeepValidator::check_datetime_value(Offset block, uint64_t raw, std::size_t depth,
                                         const char* via, FF_Result& out) {
    if (raw == FF_DATETIME_NULL) return true;
    if (!FF_DATETIME_IS_FALLBACK(raw)) {
        // Packed value: inline bytes inside a V-Table this walk has already
        // bounds-checked, so it cannot aim the reader anywhere. Only _deep()
        // looks, and only to reject a value no writer of ours would emit.
        if (check_scalars && !ff_datetime_fits(FF_UNPACK_DATETIME(raw))) {
            return fail(out, "date/time slot '" + std::string(via) +
                        "' holds a packed value outside the legal ranges");
        }
        return true;
    }
    return walk(FF_ResolveDateTimeOffset(raw, block), RECOVER_FF_STRING, depth + 1, via, out);
}

bool DeepValidator::walk(Offset off, RECOVERY_TAG expected, std::size_t depth,
                              const char* via, FF_Result& out) {
    if (off == FF_NULL_OFFSET) return true;

    if (depth > MAX_VALIDATION_DEPTH) {
        return fail(out, "nesting exceeds the maximum depth of " +
                    std::to_string(MAX_VALIDATION_DEPTH) + " at offset " +
                    std::to_string(off) + " (via " + via + ")");
    }
    // BOUNDS FIRST -- before anything indexes by `off`. seen() addresses the
    // visited bitset at off>>6, so checking it ahead of this would itself read
    // out of bounds on exactly the hostile offset this function exists to
    // reject. That ordering bug was live for one build and showed up as a
    // NON-DETERMINISTIC test failure (the OOB read usually hit mapped memory
    // and happened to return 0), which is how heap overflows normally present.
    if (!fits(off, DATA_BLOCK::HEADER_SIZE)) {
        return fail(out, "offset " + std::to_string(off) + " (via " + via +
                    ") is out of bounds for a " + std::to_string(size) + "-byte stream");
    }
    if (std::find(path.begin(), path.end(), off) != path.end()) {
        return fail(out, "cycle in the stored graph at offset " + std::to_string(off) +
                    " (via " + via + "); a block references one of its own ancestors");
    }
    // THE TAG CHECK RUNS PER EDGE, NOT PER BLOCK, so it must sit ABOVE the
    // visited short-circuit. `done` memoises "this block and its subtree are
    // structurally sound", which is a property of the block; "the referring
    // slot said this would be a Foo" is a property of the EDGE, and a block can
    // be reached by many edges. With `seen()` first, only the first edge to a
    // block was type-checked and every later one was waved through -- so a
    // crafted stream could aim a code slot (or any typed slot) at any block the
    // walk had already passed, and the reader would then decode an Identifier
    // as a CodeableConcept. Bounds were never at risk, but type confusion was.
    // Found by the DT-1.5 flagged-offset test; the contract in the header
    // always claimed this check was per edge.
    const Offset stored = static_cast<Offset>(FF_GET_VALIDATION(base, off));
    if (stored != off) {
        return fail(out, "block at offset " + std::to_string(off) + " (via " + via +
                    ") has self-offset " + std::to_string(stored) +
                    "; the offset chain is broken");
    }
    const RECOVERY_TAG actual = FF_GET_RECOVERY_TAG(base, off);
    if (expected != FF_RECOVER_UNDEFINED && actual != expected) {
        return fail(out, "block at offset " + std::to_string(off) + " (via " + via +
                    ") has recovery tag " + std::to_string(actual) + ", expected " +
                    std::to_string(expected));
    }

    // Only now is the memo safe: the subtree below is unchanged since it was
    // proven, so re-walking it would re-derive the same answer.
    if (seen(off)) return true;

    path.push_back(off);
    bool r = true;
    if (IsArrayTag(actual)) {
        r = walk_array(off, actual, depth, out);
    } else if (FF_IsStringLayoutTag(actual)) {
        // Both string-layout tags carry a length-prefixed payload and no
        // V-Table, so they need the bounds check here and would get NOTHING
        // from walk_fields -- reflected_fields_view returns {} for them and the
        // empty span is an early `return true`. An opaque-JSON block reaching
        // that branch would be waved through with its length unchecked.
        const uint32_t len = FF_GET_STRING_LENGTH(base, off);
        if (!fits(off, FF_STRING::HEADER_SIZE + len)) {
            r = fail(out, "string at offset " + std::to_string(off) + " (via " + via +
                     ") claims " + std::to_string(len) + " bytes and overruns the stream");
        }
    } else {
        r = walk_fields(off, actual, depth, out);
    }
    path.pop_back();
    if (r) mark(off);   // on completion only
    return r;
}

bool DeepValidator::walk_array(Offset off, RECOVERY_TAG array_tag,
                               std::size_t depth, FF_Result& out) {
    if (!fits(off, FF_ARRAY::HEADER_SIZE)) {
        return fail(out, "array header at offset " + std::to_string(off) + " overruns the stream");
    }
    const FF_ARRAY array(off, size, 0);
    const uint16_t step   = array.entry_step(base);
    const auto     kind   = array.entry_kind(base);
    const uint32_t count  = array.entry_count(base);
    const RECOVERY_TAG element = GetTypeFromTag(array_tag);

    if (step == 0 && count != 0) {
        return fail(out, "array at offset " + std::to_string(off) + " has a zero stride");
    }
    const Size span = static_cast<Size>(count) * static_cast<Size>(step);
    if (!fits(off, FF_ARRAY::HEADER_SIZE + span)) {
        return fail(out, "array at offset " + std::to_string(off) + " declares " +
                    std::to_string(count) + " entries of " + std::to_string(step) +
                    " bytes and overruns the stream");
    }

    const Offset entries = off + FF_ARRAY::HEADER_SIZE;
    if (kind == FF_ARRAY::OFFSET) {
        for (uint32_t i = 0; i < count; ++i) {
            const Offset child = LOAD_U64(base + entries + static_cast<Size>(i) * step);
            if (!walk(child, element, depth + 1, "array entry", out)) return false;
        }
        return true;
    }
    // Inline entries that are VALUES carry no offsets, so the span check above
    // is the whole check -- and walking them as blocks is not merely wasteful,
    // it rejects the stream: a uint32 element has no self-offset at +0 and no
    // recovery tag at +8, so `walk` reads adjacent entry bytes and reports a
    // broken offset chain. Every stream holding a populated scalar array failed
    // validation that way (TASKS.md AR-1).
    //
    // The element TAG decides this, never EntryKind: the writer stamps
    // INLINE_BLOCK on scalars and block headers alike, so the kind bits cannot
    // tell a value from a block. Recovery_to_Kind resolves the scalar band to
    // an inline-scalar kind and every block band to FF_FIELD_BLOCK.
    if (kind != FF_ARRAY::INLINE_BLOCK) return true;

    if (ff_kind_is_inline_scalar(Recovery_to_Kind(element))) return true;

    // An inline polymorphic tuple is 10 bytes of {offset, concrete tag} -- it is
    // not a block either. Its first 8 bytes are the TARGET's offset, so walking
    // the tuple itself reports a broken offset chain (the self-offset check sees
    // the target's offset, not its own). Same shape as walk_fields'
    // FF_FIELD_RESOURCE case: follow the offset, and take the concrete type from
    // the tag beside it rather than from the array header.
    if (element == RECOVER_FF_RESOURCE) {
        for (uint32_t i = 0; i < count; ++i) {
            const Offset slot = entries + static_cast<Size>(i) * step;
            const auto tgt = FF_GET_RECOVERY_TAG(base, slot);
            if (!walk(LOAD_U64(base + slot), tgt, depth + 1, "resource array entry", out))
                return false;
        }
        return true;
    }

    // Each entry is a complete block at its own offset, so the same checks apply.
    for (uint32_t i = 0; i < count; ++i) {
        const Offset child = entries + static_cast<Size>(i) * step;
        if (!walk(child, element, depth + 1, "inline array entry", out)) return false;
    }
    return true;
}

bool DeepValidator::walk_fields(Offset off, RECOVERY_TAG tag,
                                std::size_t depth, FF_Result& out) {
    const std::span<const FF_FieldInfo> fields = reflected_fields_view(tag);
    if (fields.empty()) return true;

    // Bounds-check the whole V-Table ONCE, then the per-field loop below is
    // pure offset arithmetic and loads. Doing it per field cost ~18M redundant
    // range checks on a 50 MB stream. Slots are emitted in ascending offset
    // order, so the last one bounds the table; TYPE_SIZE_RESOURCE is the widest
    // slot (10 bytes) and covers every kind.
    const Size vtable_end =
        static_cast<Size>(fields.back().field_offset) + TYPE_SIZE_RESOURCE;
    if (!fits(off, vtable_end)) {
        return fail(out, "block at offset " + std::to_string(off) +
                    " has a V-Table that overruns the stream");
    }

    for (const FF_FieldInfo& f : fields) {
        // Structural pass: only slots that can carry an offset are worth
        // visiting. Everything else is inline bytes inside the V-Table already
        // bounds-checked above, and cannot aim the reader anywhere.
        if (!check_scalars && !slot_carries_offset(f.kind)) continue;

        const Offset slot = off + f.field_offset;

        switch (f.kind) {
            case FF_FIELD_STRING:
                if (!walk(LOAD_U64(base + slot), RECOVER_FF_STRING, depth + 1, f.name, out))
                    return false;
                break;
            case FF_FIELD_BLOCK:
                if (!walk(LOAD_U64(base + slot), f.child_recovery, depth + 1, f.name, out))
                    return false;
                break;
            case FF_FIELD_ARRAY:
                if (!walk(LOAD_U64(base + slot), ToArrayTag(f.child_recovery),
                                  depth + 1, f.name, out))
                    return false;
                break;
            case FF_FIELD_RESOURCE: {
                // 10-byte slot: the concrete type is inline beside the offset.
                const auto tgt = FF_GET_RECOVERY_TAG(base, slot);
                if (!walk(LOAD_U64(base + slot), tgt, depth + 1, f.name, out)) return false;
                break;
            }
            case FF_FIELD_CHOICE: {
                // 8 raw bytes + 2-byte tag. For a scalar variant the raw bytes
                // ARE the value -- EXCEPT for the two MSB-discriminated types,
                // where the same bytes may instead be a fallback offset. Band
                // membership alone is therefore not enough to call a variant
                // inert: `FF_IsScalarBlockTag(tgt) -> break` used to skip a
                // flagged code here, leaving an attacker-controlled offset that
                // the export path then dereferenced.
                const auto tgt = FF_GET_RECOVERY_TAG(base, slot);
                if (tgt == FF_RECOVER_UNDEFINED) break;
                if (FF_IsScalarBlockTag(tgt)) {
                    if (tgt == RECOVER_FF_CODE) {
                        // The code occupies the low 4 bytes of the 8 raw ones.
                        if (!check_code_value(off, LOAD_U32(base + slot), depth, f.name, out))
                            return false;
                    } else if (Recovery_to_Kind(tgt) == FF_FIELD_DATETIME) {
                        if (!check_datetime_value(off, LOAD_U64(base + slot), depth, f.name, out))
                            return false;
                    }
                    break;
                }
                if (!walk(LOAD_U64(base + slot), tgt, depth + 1, f.name, out)) return false;
                break;
            }
            case FF_FIELD_CODE:
                // MSB set => signed relative offset to an FF_CODEABLE_CONCEPT.
                if (!check_code_value(off, LOAD_U32(base + slot), depth, f.name, out))
                    return false;
                break;
            case FF_FIELD_DATETIME:
                // The same shape one width up: MSB set => signed relative
                // offset, this time to an FF_STRING holding the original text.
                if (!check_datetime_value(off, LOAD_U64(base + slot), depth, f.name, out))
                    return false;
                break;
            case FF_FIELD_BOOL:
                // Structurally inert, so only the _deep() pass looks. A bool
                // outside {0,1} is not an attack -- the byte is inside a
                // bounds-checked V-Table -- but it is a stream no writer of
                // ours produced.
                if (check_scalars) {
                    const uint8_t v = base[slot];
                    if (v > 1 && v != FF_NULL_UINT8) {
                        return fail(out, "bool slot '" + std::string(f.name) +
                                    "' at offset " + std::to_string(slot) +
                                    " holds " + std::to_string(v) +
                                    "; expected 0, 1 or the null sentinel");
                    }
                }
                break;
            default:
                break;   // remaining inline scalars carry no offset
        }
    }
    return true;
}

} // namespace

FF_Result Parser::validate_FFHR_stream() const {
    if (m_root_offset == FF_NULL_OFFSET) return {FF_SUCCESS};   // rootless stream
    DeepValidator v;
    v.base = m_base;
    v.size = m_size;
    v.init_visited(m_size);
    FF_Result out{FF_SUCCESS};
    v.walk(m_root_offset, m_root_recovery, 0, "root", out);
    return out;
}

FF_Result Parser::validate_FFHR_stream_deep() const {
    if (m_root_offset == FF_NULL_OFFSET) return {FF_SUCCESS};
    DeepValidator v;
    v.base = m_base;
    v.size = m_size;
    v.check_scalars = true;
    v.version = m_version;
    v.init_visited(m_size);
    FF_Result out{FF_SUCCESS};
    v.walk(m_root_offset, m_root_recovery, 0, "root", out);
    return out;
}

Reflective::Node Parser::root() const {
    return Reflective::Node(m_base, m_size, m_version,
                m_root_offset, m_root_recovery, FF_FIELD_BLOCK,
                FF_RECOVER_UNDEFINED, false, m_ops, m_engine_version);
}

FF_URL_DIRECTORY Parser::url_directory() const {
    if (m_url_dir_offset == FF_NULL_OFFSET)
        throw std::runtime_error("FastFHIR: Stream has no URL directory (legacy format or not yet written).");
    return FF_URL_DIRECTORY(m_url_dir_offset, m_size, m_version, m_engine_version);
}

} // namespace FastFHIR


// =====================================================================
// JSON Serialization
// =====================================================================

#include <ostream>
namespace FastFHIR {


// High-speed string escaper for clinical narratives and markdown
static void escape_json_string(std::ostream& out, std::string_view str) {
    for (char c : str) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b";  break;
            case '\f': out << "\\f";  break;
            case '\n': out << "\\n";  break;
            case '\r': out << "\\r";  break;
            case '\t': out << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Optional: Hex encode other control characters if needed
                } else {
                    out << c;
                }
        }
    }
}

// Render a decimal slot: the value at +0, the source scale at +8.
//
// `out << double` is not an option here and never was -- ostream's default is
// six significant digits, which truncated 42.142567166419695 to 42.1426 on the
// way out. With a recorded scale the source form is reproducible exactly
// (%.*f); without one, the shortest representation that round-trips to these
// bits is the faithful answer, because that IS what the stream stores. Both
// paths are exact for every decimal in the R4 spec corpus.
// The digit count is a PARAMETER, not something this reads out of the slot,
// because a decimal reaches the exporter through two slot shapes that agree at
// +0 and disagree at +8. A plain FF_FIELD_FLOAT64 field is the 9-byte
// [ double | sigfigs ] slot. A decimal choice ([x]) variant lives in the
// 10-byte [ double | RECOVERY_TAG ] slot, where +8 is the tag -- reading it
// unconditionally turns the tag's low byte into a digit count, which rendered
// 42.142567166419695 as 42.142567 (six decimals, because that byte was 6).
// Choice variants pass FF_DECIMAL_SIGFIGS_UNSPECIFIED and fall to
// shortest-round-trip; only the field path has a real count to hand over.
static void print_decimal_json(std::ostream& out, double value, uint8_t sigfigs) {
    char buf[512];
    if (sigfigs != FF_DECIMAL_SIGFIGS_UNSPECIFIED && sigfigs <= FF_DECIMAL_SIGFIGS_MAX) {
        const int n = std::snprintf(buf, sizeof buf, "%.*f", static_cast<int>(sigfigs), value);
        if (n > 0 && static_cast<size_t>(n) < sizeof buf) { out.write(buf, n); return; }
    }
    const auto res = std::to_chars(buf, buf + sizeof buf, value);
    if (res.ec == std::errc{}) out.write(buf, res.ptr - buf);
    else                       out << value;   // unreachable for finite doubles
}

// The JSON name of a choice ([x]) field is the base name plus its ACTIVE
// variant's FHIR type -- `value` + `Quantity`. The runtime tag in the slot is
// the only thing that names that variant, so this mapping has to be total over
// the tags a choice can hold. Anything it misses exports as a bare `value`:
// well-formed JSON, wrong FHIR, and silent.
static std::string_view get_choice_suffix(RECOVERY_TAG tag) {
    switch (tag) {
        // Inline scalars: the tag names a wire representation, and the FHIR
        // spelling has to be chosen here because no block exists to ask.
        case RECOVER_FF_BOOL:     return "Boolean";
        case RECOVER_FF_FLOAT64:  return "Decimal";
        case RECOVER_FF_INT32:    return "Integer";
        case RECOVER_FF_UINT32:   return "UnsignedInt";
        case RECOVER_FF_INT64:
        case RECOVER_FF_UINT64:   return "Integer64";
        case RECOVER_FF_STRING:   return "String";
        case RECOVER_FF_CODE:     return "Code";
        // DT-2's four date/time tags share one kind and one layout, so the tag
        // is likewise the only thing that distinguishes valueDate from
        // valueDateTime on the way out.
        case RECOVER_FF_DATE:     return "Date";
        case RECOVER_FF_DATETIME: return "DateTime";
        case RECOVER_FF_TIME:     return "Time";
        case RECOVER_FF_INSTANT:  return "Instant";
        default:
            // Complex variants: the generated table knows every top-level FHIR
            // type name. reflected_resource_type is NOT the right lookup --
            // it enumerates resources only and returns "" for Quantity,
            // CodeableConcept, Period and every other data type, which is how
            // 1,416 fields came out as bare `value`.
            return FastFHIR::reflected_choice_suffix(tag);
    }
}

// Resolve an FF_FIELD_URL ref (an FF_URL_DIRECTORY entry index) back to its
// URL text. The directory offset lives in the stream header; a missing
// directory or an all-ones ref (absent/unknown URL) yields an empty string.
static std::string resolve_url_ref(const BYTE* base, Size size, uint32_t version,
                                   uint32_t ref) {
    if (ref == FF_NULL_UINT32) return {};
    const Offset dir_off = FF_HEADER(size).get_url_dir_offset(base);
    if (dir_off == FF_NULL_OFFSET || dir_off >= size) return {};
    return FF_URL_DIRECTORY(dir_off, size, version).get_url(base, ref);
}

void Reflective::Node::print_json(std::ostream& out) const {
    if (is_empty()) return;

    switch (m_kind) {
        case FF_FIELD_BLOCK: {
            out << "{";
            auto f_list = fields();
            bool first = true;

            if (FF_IsResourceTag(m_recovery)) {
                out << "\"resourceType\":\"" << reflected_resource_type(m_recovery) << "\"";
                first = false;
            }

            for (size_t i = 0; i < f_list.size(); ++i) {
                const auto& f = f_list[i];

                // Construct the O(1) field key blueprint
                FF_FieldKey key = FF_FieldKey::from_cstr(
                    m_recovery, f.kind, f.field_offset,
                    f.child_recovery, f.array_entries_are_offsets, f.name
                );

                // Pure pointer-math lookup (Zero loops)
                auto child_entry = (*this)[key];
                if (!child_entry) continue;

                // A present slot is not the same as a renderable value. An
                // inline scalar always emits exactly one token, but a slot
                // pointing at a DATA_BLOCK can reach a block whose every field
                // is absent, and Node::print_json emits NOTHING for that. The
                // emptiness has to be discovered before the key is written, or
                // the key lands in the document with nothing behind it:
                // "dose":} is not JSON, and a whole bundle stops parsing on it.
                // (Node::is_empty's FF_FIELD_CODE case guards the same hazard
                // one level down; this is its block-shaped half.)
                const bool inline_scalar = ff_kind_is_inline_scalar(f.kind);
                const Node child_node = inline_scalar ? Node() : child_entry.as_node();
                if (!inline_scalar && child_node.is_empty()) continue;

                if (!first) out << ",";
                out << "\"" << f.name;

                // Utilize Entry's native target_recovery metadata
                if (f.kind == FF_FIELD_CHOICE) out << get_choice_suffix(child_entry.target_recovery);
                out << "\":";

                // Scalars are inline values, not DATA_BLOCKs — serialize directly from Entry.
                if (inline_scalar) child_entry.print_scalar_json(out, m_version);
                else               child_node.print_json(out);
                first = false;
            }
            out << "}";
            break;
        }
        case FF_FIELD_ARRAY: {
            out << "[";
            auto arr = entries();
            bool first = true;
            for (size_t i = 0; i < arr.size(); ++i) {
                if (!arr[i].is_empty()) {
                    if (!first) out << ",";
                    arr[i].print_json(out);
                    first = false;
                }
            }
            out << "]";
            break;
        }
        case FF_FIELD_STRING:
            // The one place the two string-layout tags diverge. An opaque-JSON
            // payload is already a serialized JSON value -- an object, for a
            // retained out-of-profile resource -- so it is spliced in verbatim.
            // Quoting and escaping it would export the resource as a string
            // literal: valid JSON, wrong document, and a round-trip diff on
            // every field it contains at once.
            if (m_recovery == RECOVER_FF_OPAQUE_JSON) {
                out << as<std::string_view>();
                break;
            }
            out << "\"";
            escape_json_string(out, as<std::string_view>());
            out << "\"";
            break;
        // Scalar leaf: only reachable for choice[x] nodes resolved to an inline scalar value.
        case FF_FIELD_BOOL:    out << (as<bool>() ? "true" : "false"); break;
        case FF_FIELD_INT32:   out << as<int32_t>(); break;
        case FF_FIELD_UINT32:  out << as<uint32_t>(); break;
        case FF_FIELD_INT64:   out << as<int64_t>(); break;
        case FF_FIELD_UINT64:  out << as<uint64_t>(); break;
        // Reached for a decimal choice ([x]) variant, whose slot has no sigfigs
        // byte, and for decimal array elements, which store the sentinel.
        case FF_FIELD_FLOAT64:
            print_decimal_json(out, LOAD_F64(m_base + m_node_offset),
                               FF_DECIMAL_SIGFIGS_UNSPECIFIED);
            break;
        case FF_FIELD_CODE:
            out << "\"";
            escape_json_string(out, as<std::string_view>());
            out << "\"";
            break;
        case FF_FIELD_DATETIME: {
            // Inline packed value — a flagged fallback resolves to an
            // FF_STRING node via entry_as_node, which prints via the string
            // case above.
            const uint64_t raw = LOAD_U64(m_base + m_node_offset);
            std::string dt_text = (raw == FF_DATETIME_NULL)
                                      ? std::string()
                                      : FF_FORMAT_DATETIME(FF_UNPACK_DATETIME(raw), m_recovery);
            out << "\"";
            escape_json_string(out, dt_text);
            out << "\"";
            break;
        }
        case FF_FIELD_URL: {
            const uint32_t ref = LOAD_U32(m_base + m_node_offset);
            out << "\"";
            escape_json_string(out, resolve_url_ref(m_base, m_size, m_version, ref));
            out << "\"";
            break;
        }
        default: break;
    }
}

#ifndef NDEBUG
// ===========================================================================
// to_debug_json -- print_json plus what the reader believed about each value
// ===========================================================================
// Every defect in this file's history has been a value that decoded to
// plausible JSON under a wrong belief: a date/time tagged RECOVER_FF_STRING, a
// choice variant labelled from the wrong tag, a scalar array element labelled a
// block. Comparing output JSON to input JSON cannot see any of them -- the text
// matches, or the field is simply absent with nothing to compare. This dump
// prints the belief alongside the value so the mismatch is visible.
//
// It deliberately does NOT skip values print_json would drop. A field whose
// Entry exists but whose is_empty() says "absent" is emitted with
// "_empty":true, because "present on the wire, dropped on export" is precisely
// the failure this is meant to catch (it was 136,006 diffs, and an
// empty-skipping dump would have hidden it exactly as print_json did).

static const char* ff_kind_name(FF_FieldKind k) {
    switch (k) {
        case FF_FIELD_UNKNOWN:  return "FF_FIELD_UNKNOWN";
        case FF_FIELD_STRING:   return "FF_FIELD_STRING";
        case FF_FIELD_ARRAY:    return "FF_FIELD_ARRAY";
        case FF_FIELD_BLOCK:    return "FF_FIELD_BLOCK";
        case FF_FIELD_CODE:     return "FF_FIELD_CODE";
        case FF_FIELD_BOOL:     return "FF_FIELD_BOOL";
        case FF_FIELD_INT32:    return "FF_FIELD_INT32";
        case FF_FIELD_UINT32:   return "FF_FIELD_UINT32";
        case FF_FIELD_INT64:    return "FF_FIELD_INT64";
        case FF_FIELD_UINT64:   return "FF_FIELD_UINT64";
        case FF_FIELD_FLOAT64:  return "FF_FIELD_FLOAT64";
        case FF_FIELD_RESOURCE: return "FF_FIELD_RESOURCE";
        case FF_FIELD_CHOICE:   return "FF_FIELD_CHOICE";
        case FF_FIELD_DATETIME: return "FF_FIELD_DATETIME";
        case FF_FIELD_URL:      return "FF_FIELD_URL";
    }
    return "FF_FIELD_?";
}

namespace {
// Indentation state threaded through the recursion. depth < 0 means minified,
// which keeps the common grep-the-whole-corpus case on one line per value.
struct DebugFmt {
    int step;
    int depth;
    bool pretty() const { return step > 0; }
    void nl(std::ostream& o) const {
        if (!pretty()) return;
        o << "\n";
        for (int i = 0; i < depth * step; ++i) o << ' ';
    }
    DebugFmt in() const { return {step, depth + 1}; }
};

void debug_meta(std::ostream& o, Offset off, RECOVERY_TAG tag, FF_FieldKind kind) {
    o << "\"_off\":" << off
      << ",\"_tag\":\"" << FF_RecoveryName(tag) << "\""
      << ",\"_hex\":\"0x" << std::hex << std::uppercase << tag << std::dec << std::nouppercase << "\""
      << ",\"_kind\":\"" << ff_kind_name(kind) << "\"";
}
} // namespace

// The recursion lives inside the member function on purpose: access to Node's
// protected state is granted per-CLASS, so a lambda declared here reaches every
// Node it visits, and no debug-only helper has to be declared in the public
// header to get at m_base/m_recovery/m_kind.
void Reflective::Node::to_debug_json(std::ostream& out, int indent) const {

    // A block field: the value, plus the slot metadata only the Entry holds.
    const auto emit_entry = [&out](const Node& parent, const Entry& e,
                                   const FF_FieldInfo& f, DebugFmt fmt,
                                   const auto& emit_node) -> void {
        const Offset slot = e.absolute_offset();
        out << "{";
        debug_meta(out, slot, e.target_recovery, f.kind);

        // The schema's claim about this field kept beside the runtime one:
        // where they differ, the difference IS the bug (six code arrays declare
        // RECOVER_FF_CODE and store strings -- TASKS.md AR-2).
        if (f.child_recovery != FF_RECOVER_UNDEFINED &&
            GetTypeFromTag(f.child_recovery) != GetTypeFromTag(e.target_recovery))
            out << ",\"_schema_tag\":\"" << FF_RecoveryName(f.child_recovery) << "\"";

        if (f.kind == FF_FIELD_CHOICE)
            out << ",\"_suffix\":\"" << get_choice_suffix(e.target_recovery) << "\"";

        // A code slot is inline unless its flag routes it to a CodeableConcept.
        if (f.kind == FF_FIELD_CODE) {
            const uint32_t raw = LOAD_U32(parent.m_base + slot);
            out << ",\"_code\":" << (raw & ~FF_CODEABLE_CONCEPT_FLAG)
                << ",\"_cc_fallback\":" << ((raw & FF_CODEABLE_CONCEPT_FLAG) ? "true" : "false");
        }
        // A date/time slot is 8 inline bytes unless bit 63 routes it to a string.
        if (f.kind == FF_FIELD_DATETIME) {
            const uint64_t raw = LOAD_U64(parent.m_base + slot);
            out << ",\"_dt_fallback\":"
                << ((raw != FF_DATETIME_NULL && FF_DATETIME_IS_FALLBACK(raw)) ? "true" : "false");
        }

        const bool inline_scalar = ff_kind_is_inline_scalar(f.kind);
        const Node child = inline_scalar ? Node() : e.as_node();
        if (!inline_scalar && child.is_empty()) out << ",\"_empty\":true";

        out << ",\"_v\":";
        if (inline_scalar) e.print_scalar_json(out, parent.m_version);
        else               emit_node(child, fmt, emit_node);
        out << "}";
    };

    const auto emit_node = [&out, &emit_entry](const Node& n, DebugFmt fmt,
                                               const auto& self) -> void {
        if (!n) { out << "null"; return; }

        switch (n.m_kind) {
            case FF_FIELD_BLOCK: {
                out << "{";
                debug_meta(out, n.m_node_offset, n.m_recovery, n.m_kind);
                if (FF_IsResourceTag(n.m_recovery))
                    out << ",\"resourceType\":\"" << reflected_resource_type(n.m_recovery) << "\"";

                const DebugFmt inner = fmt.in();
                for (const FF_FieldInfo& f : n.fields()) {
                    const FF_FieldKey key = FF_FieldKey::from_cstr(
                        n.m_recovery, f.kind, f.field_offset,
                        f.child_recovery, f.array_entries_are_offsets, f.name);
                    const Entry child_entry = n[key];
                    if (!child_entry) continue;   // slot genuinely absent

                    out << ",";
                    inner.nl(out);
                    // The SAME key print_json emits, choice suffix included.
                    // Path parity is the point: a diff reported at
                    // .../valueInteger has to resolve to this node, and it
                    // cannot if the debug dump calls the field `value`.
                    out << "\"" << f.name;
                    if (f.kind == FF_FIELD_CHOICE)
                        out << get_choice_suffix(child_entry.target_recovery);
                    out << "\":";
                    emit_entry(n, child_entry, f, inner, self);
                }
                fmt.nl(out);
                out << "}";
                break;
            }
            case FF_FIELD_ARRAY: {
                const FF_ARRAY arr(n.m_node_offset, n.m_size, n.m_version, n.m_engine_version);
                const auto ekind = arr.entry_kind(n.m_base);
                const RECOVERY_TAG elem = GetTypeFromTag(FF_GET_RECOVERY_TAG(n.m_base, n.m_node_offset));

                out << "{";
                debug_meta(out, n.m_node_offset, n.m_recovery, n.m_kind);
                out << ",\"_entry_kind\":\""
                    << (ekind == FF_ARRAY::OFFSET         ? "OFFSET"
                        : ekind == FF_ARRAY::INLINE_BLOCK ? "INLINE_BLOCK"
                        : ekind == FF_ARRAY::SCALAR       ? "SCALAR(unwritten)"
                                                          : "?")
                    << "\",\"_stride\":" << arr.entry_step(n.m_base)
                    << ",\"_count\":" << arr.entry_count(n.m_base)
                    << ",\"_elem\":\"" << FF_RecoveryName(elem) << "\"";

                out << ",\"_v\":[";
                const DebugFmt inner = fmt.in();
                const auto items = n.entries();
                for (size_t i = 0; i < items.size(); ++i) {
                    if (i) out << ",";
                    inner.nl(out);
                    self(items[i], inner, self);
                }
                fmt.nl(out);
                out << "]}";
                break;
            }
            default:
                // Leaves: metadata plus exactly the token print_json emits, so
                // the two dumps stay comparable value-for-value.
                out << "{";
                debug_meta(out, n.m_node_offset, n.m_recovery, n.m_kind);
                if (n.is_empty()) out << ",\"_empty\":true";
                out << ",\"_v\":";
                n.print_json(out);
                out << "}";
                break;
        }
    };

    emit_node(*this, DebugFmt{indent, 0}, emit_node);
}
#endif // NDEBUG

void Reflective::Entry::print_scalar_json(std::ostream& out, uint32_t version) const {
    const Offset slot = absolute_offset();
    if (slot == FF_NULL_OFFSET) { out << "null"; return; }

    switch (kind) {
        case FF_FIELD_BOOL:
            out << (Decode::scalar<bool>(base, slot, RECOVER_FF_BOOL) ? "true" : "false");
            break;
        case FF_FIELD_INT32:
            out << Decode::scalar<int32_t>(base, slot, RECOVER_FF_INT32);
            break;
        case FF_FIELD_UINT32:
            out << Decode::scalar<uint32_t>(base, slot, RECOVER_FF_UINT32);
            break;
        case FF_FIELD_INT64:
            out << Decode::scalar<int64_t>(base, slot, RECOVER_FF_INT64);
            break;
        case FF_FIELD_UINT64:
            out << Decode::scalar<uint64_t>(base, slot, RECOVER_FF_UINT64);
            break;
        case FF_FIELD_FLOAT64:
            // The real 9-byte decimal field slot: the source digit count is at +8.
            print_decimal_json(out, LOAD_F64(base + slot),
                               LOAD_U8(base + slot + TYPE_SIZE_UINT64));
            break;
        case FF_FIELD_CODE: {
            uint32_t raw = LOAD_U32(base + slot);
            if (raw == FF_CODE_NULL) { out << "null"; break; }            if (raw & FF_CODEABLE_CONCEPT_FLAG) {
                // One decoder, shared with every other read path.
                //
                // This used to be a third copy of the per-system switch, and it
                // had drifted: only UNKNOWN, UCUM, SNOMED_CT and DICOM were
                // handled, so CPT, CVX, RxNorm, MDC, MED-RT and IDMP fell to a
                // `default:` that printed their fixed-width binary payload as if
                // it were ASCII -- raw bytes straight into the JSON.
                const Offset block_off =
                    FF_ResolveCodeableConceptOffset(raw, parent_offset);
                const auto decoded = FF_DECODE_CODEABLE_CONCEPT(base, block_off, version);
                if (decoded.label.empty()) {
                    out << "null";
                } else {
                    out << '"';
                    escape_json_string(out, decoded.label);
                    out << '"';
                }
            } else {
                // Dictionary lookup (31-bit index)
                if (const char* label = FF_ResolveCode(raw, version)) {
                    out << '"';
                    escape_json_string(out, label);
                    out << '"';
                } else {
                    out << "null";
                }
            }
            break;
        }
        case FF_FIELD_DATETIME: {
            // DT-2: packed values format canonically; a flagged fallback
            // returns the ORIGINAL text byte-exact.
            const uint64_t raw = LOAD_U64(base + slot);
            if (raw == FF_DATETIME_NULL) { out << "null"; break; }
            std::string dt_text;
            if (FF_DATETIME_IS_FALLBACK(raw)) {
                FF_STRING s(FF_ResolveDateTimeOffset(raw, parent_offset), 0, version);
                dt_text = std::string(s.read_view(base));
            } else {
                dt_text = FF_FORMAT_DATETIME(FF_UNPACK_DATETIME(raw), target_recovery);
            }
            out << '"';
            escape_json_string(out, dt_text);
            out << '"';
            break;
        }
        case FF_FIELD_URL: {
            const uint32_t ref = LOAD_U32(base + slot);
            const std::string url = resolve_url_ref(base, m_size, version, ref);
            if (url.empty()) { out << "null"; break; }
            out << '"';
            escape_json_string(out, url);
            out << '"';
            break;
        }
        default:
            out << "null";
            break;
    }
}

void Parser::print_json(std::ostream& out) const {
    auto r = root();
    if (r) {
        r.print_json(out);
    } else {
        out << "{\"error\":\"Invalid FastFHIR Root Node\"}";
    }
}

namespace Reflective {
// =====================================================================
// Node constructors
// =====================================================================
Node::Node(const BYTE* base, Size size, uint32_t version, Offset offset,
           RECOVERY_TAG recovery, FF_FieldKind kind,
                     RECOVERY_TAG child_recovery, bool array_entries_are_offsets,
                     const ParserOps* ops, uint32_t engine_ver)
    : m_base(base),
      m_node_offset(offset),
      m_size(size),
      m_version(version),
      m_engine_version(engine_ver),
      m_recovery(recovery),
      m_child_recovery(child_recovery),
      m_kind(kind),
            m_array_entries_are_offsets(array_entries_are_offsets),
            m_ops(ops) {}

Node Node::resolve_choice(const BYTE* base, Size size, uint32_t version, 
                                                    Offset parent_offset, Offset value_offset, FF_FieldKind schema_kind,
                                                    const ParserOps* ops) {
    assert(schema_kind == FF_FIELD_CHOICE && "resolve_choice called on non-choice V-Table slot");
    RECOVERY_TAG tag = FF_GET_RECOVERY_TAG(base, value_offset);
    
    if ((tag & 0xFF00) == RECOVER_FF_SCALAR_BLOCK) {
        // A FLAGGED CODE VARIANT IS NOT INLINE. Its low 4 bytes are a signed
        // relative offset to an FF_CODEABLE_CONCEPT, and -- like every other
        // spelling of this offset (ENCODE_FF_CODE, the compactor's
        // write_choice_slot, Entry's two readers) -- it is relative to the
        // CONTAINING BLOCK, not to the slot.
        //
        // It has to be resolved HERE because this is the last point at which
        // the containing block is known: the Node that comes out carries only
        // its own offset, so arithmetic deferred past this line has already
        // lost an operand. Deferring it is exactly what went wrong before --
        // Node::as<string_view>() resolved against the node's own offset, which
        // for a choice variant is the slot, and so read the containing block's
        // V-Table width away from the real block.
        if (tag == RECOVER_FF_CODE) {
            return ParserOps::code_node(base, size, version, parent_offset, value_offset,
                                        ops, /*engine_ver=*/0);
        }
        // DT-3: the date/time slot owes the identical treatment one width up.
        // A flagged date/time variant is not inline either -- bit 63 set means
        // the low 63 bits are a signed offset, relative to the CONTAINING
        // BLOCK, to an FF_STRING holding the original text. parent_offset is
        // live here and gone from the Node that comes out, so resolving it
        // later is not possible, and skipping it hands print_json a fallback
        // word to FF_UNPACK_DATETIME as if it were a packed civil value.
        if (FF_IsDateTimeTag(tag)) {
            const uint64_t raw = LOAD_U64(base + value_offset);
            if (raw != FF_DATETIME_NULL && FF_DATETIME_IS_FALLBACK(raw)) {
                return Node(base, size, version,
                            FF_ResolveDateTimeOffset(raw, parent_offset),
                            RECOVER_FF_STRING, FF_FIELD_STRING,
                            FF_RECOVER_UNDEFINED, false, ops);
            }
        }
        Node n(base, size, version, parent_offset, tag, Recovery_to_Kind(tag),
               FF_RECOVER_UNDEFINED, false, ops);
        n.m_node_offset = value_offset;
        return n;
    }
    
    Offset child_off = LOAD_U64(base + value_offset);
    if (child_off == FF_NULL_OFFSET) return {}; 
    
    FF_FieldKind dynamic_kind = FF_FIELD_BLOCK;
    switch (tag) {
        case RECOVER_FF_STRING: dynamic_kind = FF_FIELD_STRING; break;
        case RECOVER_FF_CODE: dynamic_kind = FF_FIELD_CODE; break;
        default: break;
    }
    return Node(base, size, version, child_off, tag, dynamic_kind,
                FF_RECOVER_UNDEFINED, false, ops);
}

bool Node::is_empty() const {
    if (!*this) return true;

    switch (m_kind) {
        case FF_FIELD_ARRAY:
            return size() == 0;

        case FF_FIELD_STRING:
            // Strings are empty when their decoded view is empty.
            return as<std::string_view>().empty();

        case FF_FIELD_CODE:
            // Codes are empty only when the raw slot is the explicit FF_CODE_NULL sentinel.
            // Do not treat unresolved dictionary codes as empty, otherwise print_json can emit
            // invalid key/value pairs like "type":,
            return FF_IsFieldEmpty(m_base, m_node_offset, FF_FIELD_CODE);

        // Every inline-scalar kind must be listed. The `default` below returns
        // true, so an omission does not fail loudly -- it silently reports the
        // field absent and print_json drops it. FF_IsFieldEmpty carries the
        // same warning about the same two kinds; this switch is its mirror and
        // has to stay in step with it.
        //
        // FF_FIELD_DATETIME and FF_FIELD_URL were missing here. It stayed
        // invisible only because nothing produced a Node of either kind: URL
        // slots print through Entry, and date/time choice variants were still
        // mis-tagged RECOVER_FF_STRING, so resolve_choice handed back a STRING
        // node. Tagging them correctly (DT-2) made resolve_choice return a real
        // FF_FIELD_DATETIME node, and all 536 of them vanished from the export.
        case FF_FIELD_BOOL:
        case FF_FIELD_INT32:
        case FF_FIELD_UINT32:
        case FF_FIELD_INT64:
        case FF_FIELD_UINT64:
        case FF_FIELD_FLOAT64:
        case FF_FIELD_DATETIME:
        case FF_FIELD_URL:
            return FF_IsFieldEmpty(m_base, m_node_offset, m_kind);

        case FF_FIELD_BLOCK: {
            auto f_list = fields();
            for (size_t i = 0; i < f_list.size(); ++i) {
                const auto& f = f_list[i];
                FF_FieldKey key = FF_FieldKey::from_cstr(
                    m_recovery, f.kind, f.field_offset,
                    f.child_recovery, f.array_entries_are_offsets, f.name
                );
                if ((*this)[key])
                    return false;
            }
            return true;
        }

        default:
            return true;
    }
}

// =====================================================================
// Node observers
// =====================================================================
Node::operator bool() const {
    return m_base != nullptr
    && m_node_offset != FF_NULL_OFFSET;
}

bool Node::is_array()  const { return m_kind == FF_FIELD_ARRAY; }
bool Node::is_object() const { return m_kind == FF_FIELD_BLOCK; }
bool Node::is_string() const { return m_kind == FF_FIELD_STRING; }
// FF_FIELD_DATETIME belongs here: a packed date/time is an inline value in the
// V-Table slot, exactly like FF_FIELD_CODE. That a flagged one can point at an
// FF_STRING no more makes it a string than a fallback CodeableConcept makes a
// code a block -- the slot is still the value's home.
bool Node::is_scalar() const { return ff_kind_is_inline_scalar(m_kind); }

FF_FieldKind Node::kind()     const { return m_kind; }
RECOVERY_TAG Node::recovery() const { return m_recovery; }

// =====================================================================
// Node reflection helpers
// =====================================================================
std::span<const FF_FieldInfo> Node::fields() const {
    if (!is_object()) return {};
    return reflected_fields_view(m_recovery);
}

std::vector<std::string_view> Node::keys() const {
    if (!is_object()) return {};
    return reflected_keys(m_recovery);
}

size_t Node::size() const {
    const ParserOps* ops = m_ops ? m_ops : &STANDARD_OPS;
    return ops->node_size(*this);
}

std::vector<Node> Node::entries() const {
    const ParserOps* ops = m_ops ? m_ops : &STANDARD_OPS;
    return ops->node_entries(*this);
}

size_t ParserOps::standard_node_size(const Node& n) {
    if (n.is_array()) {
        FF_ARRAY array(n.m_node_offset, n.m_size, n.m_version, n.m_engine_version);
        return array.entry_count(n.m_base);
    }
    if (n.is_object()) {
        return reflected_fields_view(n.m_recovery).size();
    }
    return 0;
}

// An array's own header is the only description of its entries that cannot
// drift from them: the call that laid out the entries wrote it. So element
// identity is decided HERE, from that header, and both readers -- entries()
// and node[i] -- route through this one function rather than each deciding for
// itself. When they decided independently they disagreed, and the disagreement
// was silent (TASKS.md AR-1).
//
// Two header fields answer two separate questions, and neither substitutes for
// the other:
//   RECOVERY tag       -- WHAT the element is. Authoritative. Schema-side
//                         copies are not: FF_FieldKeys.hpp names six `code`
//                         arrays that are stored as strings (AR-2).
//   entries_are_pointers -- WHETHER the entry is the value or a pointer to it.
//                         The tag cannot answer this while date/time arrays
//                         are still written as FF_STRING blocks under a
//                         RECOVER_FF_DATETIME header tag (TASKS.md DT-2.4).
//
// EntryKind's three-way split is NOT consulted for element shape: the writer
// stamps INLINE_BLOCK on scalars, block headers and resource tuples alike, so
// reading it as "these are blocks" is what made every scalar array export as
// an empty list.
Node ParserOps::array_element(const Node& n, const FF_ARRAY& arr,
                              RECOVERY_TAG elem, uint32_t index) {
    const Offset item_ptr = n.m_node_offset + arr.get_header_size() +
                            static_cast<Offset>(index) * arr.entry_step(n.m_base);

    // --- POINTER TABLE (variable-length elements: every FF_STRING-backed field) ---
    if (arr.entries_are_pointers(n.m_base)) {
        const Offset child_off = LOAD_U64(n.m_base + item_ptr);
        if (child_off == FF_NULL_OFFSET) return {};
        // The pointed-to block's own tag outranks even the array header here:
        // a `code` array declares RECOVER_FF_STRING and stores FF_STRINGs, and
        // a dateTime array declares RECOVER_FF_DATETIME and stores them too.
        const RECOVERY_TAG actual = FF_GET_RECOVERY_TAG(n.m_base, child_off);
        return Node(n.m_base, n.m_size, n.m_version, child_off, actual,
                    Recovery_to_Kind(actual), FF_RECOVER_UNDEFINED, false,
                    n.m_ops, n.m_engine_version);
    }

    // --- INLINE POLYMORPHIC TUPLE (10 bytes: offset + concrete tag) ---
    if (elem == RECOVER_FF_RESOURCE) {
        const Offset actual_off = LOAD_U64(n.m_base + item_ptr);
        if (actual_off == FF_NULL_OFFSET) return {};

        const RECOVERY_TAG tuple_tag = FF_GET_RECOVERY_TAG(n.m_base, item_ptr);
        const RECOVERY_TAG block_tag = FF_GET_RECOVERY_TAG(n.m_base, actual_off);
        if (tuple_tag != block_tag) {
            // ONE DAMAGED ELEMENT IS NOT A DAMAGED DOCUMENT.
            //
            // The two halves of a tuple disagreeing means this element is
            // corrupt, and that is worth knowing -- but this used to THROW, and
            // a throw from a per-element accessor does not stay local. entries()
            // walks elements in a loop, so the first bad tuple aborted the whole
            // array; and in a Bundle the entire payload hangs off exactly one
            // array. Measured: a stream whose references recovery had restored
            // to 99.9% still yielded ZERO readable values, because one element
            // of Bundle.entry raised and took the document with it.
            //
            // The read path's contract is falsy Nodes and null sentinels, not
            // exceptions (CLAUDE.md invariant 5); throwing here also broke that.
            // The disagreement is still reported, loudly and in one place, by
            // validate_FFHR_stream() -- which is where a structural fault
            // belongs, because it can name every one of them instead of
            // stopping at the first.
            return {};
        }

        // The kind follows the tag for the same reason the tag outranks the array
        // header above: a `contained` resource outside the compiled profile is
        // retained as an opaque-JSON block, which has string layout and no
        // V-Table. Calling it a block is the reflected_fields_view/{} ->
        // "no members" -> dropped-element shape.
        return Node(n.m_base, n.m_size, n.m_version, actual_off, tuple_tag,
                    Recovery_to_Kind(tuple_tag) == FF_FIELD_STRING ? FF_FIELD_STRING
                                                                   : FF_FIELD_BLOCK,
                    FF_RECOVER_UNDEFINED, false, n.m_ops, n.m_engine_version);
    }

    // --- INLINE ENTRY: the value itself, or a block header, per the tag ---
    // Recovery_to_Kind resolves the scalar band (0x0100-0x01FF) to the concrete
    // scalar kind and every block band to FF_FIELD_BLOCK, so one call covers
    // both remaining layouts.
    return Node(n.m_base, n.m_size, n.m_version, item_ptr, elem, Recovery_to_Kind(elem),
                FF_RECOVER_UNDEFINED, false, n.m_ops, n.m_engine_version);
}

// The element type is read from the array header once, not per entry.
RECOVERY_TAG ParserOps::array_element_tag(const Node& n) {
    return GetTypeFromTag(FF_GET_RECOVERY_TAG(n.m_base, n.m_node_offset));
}

std::vector<Node> ParserOps::standard_node_entries(const Node& n) {
    std::vector<Node> out;
    if (!n.is_array()) return out;

    FF_ARRAY array(n.m_node_offset, n.m_size, n.m_version, n.m_engine_version);
    const uint32_t count = array.entry_count(n.m_base);
    const RECOVERY_TAG elem = array_element_tag(n);

    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        out.push_back(array_element(n, array, elem, i));

    return out;
}
// =====================================================================
// Node child lookup
// =====================================================================
Entry Node::operator[](FF_FieldKey key) const {
    const ParserOps* ops = m_ops ? m_ops : &STANDARD_OPS;
    return ops->node_lookup_field(*this, key);
}

Entry ParserOps::standard_node_lookup_field(const Node& n, FF_FieldKey key) {
    if (!n.is_object()) return NULL_ENTRY;

    const RECOVERY_TAG owner_recovery = GetTypeFromTag(key.owner_recovery);
    if (owner_recovery != FF_RECOVER_UNDEFINED && owner_recovery != n.m_recovery) {
        if (!(owner_recovery == RECOVER_FF_RESOURCE && FF_IsResourceTag(n.m_recovery))) {
            return NULL_ENTRY;
        }
    }

    const Offset value_offset = n.m_node_offset + key.field_offset;

    if (FF_IsFieldEmpty(n.m_base, value_offset, key.kind)) {
        return NULL_ENTRY;
    }

    RECOVERY_TAG target_tag = key.child_recovery;
    if (key.kind == FF_FIELD_CHOICE) {
        // A choice slot's variant tag is written at runtime; the static
        // child_recovery only names the first variant and would mislabel the
        // JSON suffix (valueString for valueDecimal). FF_IsFieldEmpty above
        // already rejected absent choices, so the runtime tag is guaranteed
        // valid here — never FF_RECOVER_UNDEFINED.
        target_tag = FF_GET_RECOVERY_TAG(n.m_base, value_offset);
    } else if (target_tag == FF_RECOVER_UNDEFINED && key.kind != FF_FIELD_UNKNOWN) {
        target_tag = Kind_to_Recovery(key.kind);
    }
    return {n.m_base, n.m_node_offset, key.field_offset, target_tag, key.kind, n.m_size, n.m_version, n.m_ops, n.m_engine_version};
}

Node Node::operator[](size_t index) const {
    const ParserOps* ops = m_ops ? m_ops : &STANDARD_OPS;
    return ops->node_lookup_index(*this, index);
}

Node ParserOps::standard_node_lookup_index(const Node& n, size_t index) {
    if (!n.is_array()) return {};

    FF_ARRAY arr(n.m_node_offset, n.m_size, n.m_version, n.m_engine_version);
    if (index >= arr.entry_count(n.m_base)) return {};

    return array_element(n, arr, array_element_tag(n), static_cast<uint32_t>(index));
}

Node Entry::as_node(Size size, uint32_t version, RECOVERY_TAG expected_tag, FF_FieldKind schema_kind,
                    const ParserOps* ops) const {
    const ParserOps* use_ops = ops ? ops : &STANDARD_OPS;
    return use_ops->entry_as_node(*this, size, version, expected_tag, schema_kind, use_ops);
}

// =====================================================================
// Typed scalar slot reads — the wire loads live here, not in headers.
// =====================================================================
// Declared in FF_Parser.hpp; defined in this TU where FF_Ops.hpp is visible,
// and explicitly instantiated for the six FHIR scalar wire types. A caller
// requesting any other arithmetic T fails loudly at link time instead of
// silently misreading — the honest contract.
template <typename T>
    requires std::is_arithmetic_v<T>
T Entry::as_scalar(RECOVERY_TAG expected_tag) const {
    return Decode::scalar<T>(base, absolute_offset(), expected_tag);
}
template bool     Entry::as_scalar<bool>(RECOVERY_TAG) const;
template int32_t  Entry::as_scalar<int32_t>(RECOVERY_TAG) const;
template uint32_t Entry::as_scalar<uint32_t>(RECOVERY_TAG) const;
template int64_t  Entry::as_scalar<int64_t>(RECOVERY_TAG) const;
template uint64_t Entry::as_scalar<uint64_t>(RECOVERY_TAG) const;
template double   Entry::as_scalar<double>(RECOVERY_TAG) const;

template <typename T>
    requires std::is_arithmetic_v<T>
T Reflective::Node::read_scalar(RECOVERY_TAG expected_tag) const {
    return Decode::scalar<T>(m_base, m_node_offset, expected_tag);
}
template bool     Reflective::Node::read_scalar<bool>(RECOVERY_TAG) const;
template int32_t  Reflective::Node::read_scalar<int32_t>(RECOVERY_TAG) const;
template uint32_t Reflective::Node::read_scalar<uint32_t>(RECOVERY_TAG) const;
template int64_t  Reflective::Node::read_scalar<int64_t>(RECOVERY_TAG) const;
template uint64_t Reflective::Node::read_scalar<uint64_t>(RECOVERY_TAG) const;
template double   Reflective::Node::read_scalar<double>(RECOVERY_TAG) const;

uint32_t Reflective::Node::code_word() const {
    return LOAD_U32(m_base + m_node_offset);
}

Entry::operator std::string_view() const {
    if (kind == FF_FIELD_CODE) {
        uint32_t raw_code = LOAD_U32(base + absolute_offset());
        if (raw_code == FF_CODE_NULL) return "";

        if (const char* label = FF_ResolveCode(raw_code, m_version)) {
            return label;
        }

        if (raw_code & FF_CODEABLE_CONCEPT_FLAG) {
            Offset abs_off = FF_ResolveCodeableConceptOffset(raw_code, parent_offset);
            return FF_DECODE_CODEABLE_CONCEPT(base, abs_off, m_version).label;
        }

        return "";
    }

    return as_node().as<std::string_view>();
}

Node ParserOps::standard_entry_as_node(const Entry& e, Size size, uint32_t version,
                                       RECOVERY_TAG expected_tag, FF_FieldKind schema_kind,
                                       const ParserOps* ops) {
    if (e.base == nullptr || e.absolute_offset() == FF_NULL_OFFSET) return {};

    const Offset slot_offset = e.absolute_offset();

    switch (schema_kind) {
        case FF_FIELD_BOOL:
        case FF_FIELD_INT32:
        case FF_FIELD_UINT32:
        case FF_FIELD_INT64:
        case FF_FIELD_UINT64:
        case FF_FIELD_FLOAT64:
        case FF_FIELD_URL:
            // Scalar slots are inline values at slot_offset.
            return Node(e.base, size, version, slot_offset, expected_tag, schema_kind,
                        FF_RECOVER_UNDEFINED, false, ops, e.m_engine_version);

        case FF_FIELD_CODE:
            // A code is inline ONLY while its flag is clear; see code_node.
            return code_node(e.base, size, version, e.parent_offset, slot_offset,
                             ops, e.m_engine_version);

        case FF_FIELD_DATETIME: {
            // DT-2: the same discriminator as code_node, one width up. Packed
            // values and the null sentinel are inline 8 bytes; bit 63 set is a
            // signed relative offset to an FF_STRING holding the ORIGINAL text.
            const uint64_t raw = LOAD_U64(e.base + slot_offset);
            if (raw == FF_DATETIME_NULL || !FF_DATETIME_IS_FALLBACK(raw)) {
                return Node(e.base, size, version, slot_offset, expected_tag,
                            schema_kind, FF_RECOVER_UNDEFINED, false, ops,
                            e.m_engine_version);
            }
            return Node(e.base, size, version,
                        FF_ResolveDateTimeOffset(raw, e.parent_offset),
                        RECOVER_FF_STRING, FF_FIELD_STRING,
                        FF_RECOVER_UNDEFINED, false, ops, e.m_engine_version);
        }

        case FF_FIELD_CHOICE: 
            // parent_offset, not slot_offset -- see the compact path above.
            return Node::resolve_choice(e.base, size, version, e.parent_offset, slot_offset, schema_kind, ops);

        case FF_FIELD_RESOURCE: {
            Offset actual_off = LOAD_U64(e.base + slot_offset);
            if (actual_off == FF_NULL_OFFSET) return {};
            RECOVERY_TAG actual_tag = FF_GET_RECOVERY_TAG(e.base, slot_offset);
            // The tag beside the offset is ground truth for the KIND too, not
            // just the type name. This used to hardcode FF_FIELD_BLOCK, which
            // was right only while every resource slot held a generated
            // resource block: a resource outside the compiled profile is
            // retained as an opaque-JSON blob (string layout), and calling that
            // a block asks fields() for a V-Table it does not have -- the
            // reflected_fields_view/{} -> "no members" -> dropped-field shape
            // that has now cost four separate defects.
            return Node(e.base, size, version, actual_off, actual_tag,
                        Recovery_to_Kind(actual_tag) == FF_FIELD_STRING ? FF_FIELD_STRING
                                                                        : FF_FIELD_BLOCK,
                        FF_RECOVER_UNDEFINED, false, ops, e.m_engine_version);
        }

        case FF_FIELD_ARRAY: {
            Offset child_offset = LOAD_U64(e.base + slot_offset);
            if (child_offset == FF_NULL_OFFSET) return {};
            // m_recovery is unused on array Nodes (fields() returns {} for non-objects).
            // expected_tag already encodes the element type exactly — no memory read needed.
            FF_ARRAY arr_hdr(child_offset, size, version, e.m_engine_version);
            bool entries_are_offsets = arr_hdr.entries_are_pointers(e.base);
            return Node(e.base, size, version, child_offset, expected_tag, schema_kind,
                        expected_tag, entries_are_offsets, ops, e.m_engine_version);
        }

        default: {
            // Standard pointer hop for Blocks and Strings
            Offset child_offset = LOAD_U64(e.base + slot_offset);
            if (child_offset == FF_NULL_OFFSET) return {};
            RECOVERY_TAG actual_tag = FF_GET_RECOVERY_TAG(e.base, child_offset);
            // The schema kind can be BLOCK (e.g. dateTime fields resolved through the
            // complex-block mapping) while the stored block is actually an FF_STRING.
            // The recovery tag is ground truth — re-derive the kind so string nodes are
            // walked as strings (is_empty/print_json) instead of as empty blocks, which
            // emitted dangling keys like "start":, (Bug C, TASKS.md A23.3).
            FF_FieldKind child_kind = schema_kind;
            if (FF_IsStringLayoutTag(actual_tag)) child_kind = FF_FIELD_STRING;
            return Node(e.base, size, version, child_offset, actual_tag, child_kind,
                        FF_RECOVER_UNDEFINED, false, ops, e.m_engine_version);
        }
    }
}
} // namespace Reflective
} // namespace FastFHIR
