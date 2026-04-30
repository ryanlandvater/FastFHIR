/**
 * @file FF_Parser.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief 
 * @version 0.1
 * @date 2026-03-18
 * 
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file in the project root for terms.
 * 
 */

#include "../include/FF_Utilities.hpp"
#include "../include/FF_Parser.hpp"
#include "../include/FF_SIMD.hpp"
#include "../generated_src/FF_Dictionary.hpp"
#include "../generated_src/FF_Reflection.hpp"
#include <assert.h>

namespace FastFHIR {
namespace Reflective {
class Node;
struct Entry;

using NodeSizeFn = size_t (*)(const Node&);
using NodeEntriesFn = std::vector<Node> (*)(const Node&);
using NodeLookupFieldFn = Entry (*)(const Node&, FF_FieldKey);
using NodeLookupIndexFn = Node (*)(const Node&, size_t);
using EntryAsNodeFn = Node (*)(const Entry&, Size, uint32_t, RECOVERY_TAG, FF_FieldKind, const ParserOps*);

struct ParserOps {
    FF_StreamLayout layout;
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

static inline uint32_t compact_presence_bytes(size_t field_count) {
    // Contract from design: field_count / 8 + 1 bytes.
    return static_cast<uint32_t>(field_count / 8 + 1);
}

static inline uint16_t compact_slot_size(FF_FieldKind kind) {
    switch (kind) {
        case FF_FIELD_BOOL:    return TYPE_SIZE_UINT8;
        case FF_FIELD_INT32:   return TYPE_SIZE_INT32;
        case FF_FIELD_UINT32:  return TYPE_SIZE_UINT32;
        case FF_FIELD_INT64:   return TYPE_SIZE_UINT64;
        case FF_FIELD_UINT64:  return TYPE_SIZE_UINT64;
        case FF_FIELD_FLOAT64: return TYPE_SIZE_FLOAT64;
        case FF_FIELD_CODE:    return TYPE_SIZE_UINT32;
        case FF_FIELD_RESOURCE:return TYPE_SIZE_RESOURCE;
        case FF_FIELD_CHOICE:  return TYPE_SIZE_CHOICE;
        default:               return TYPE_SIZE_OFFSET; // string/array/block pointers
    }
}

static inline bool compact_presence_contains(const BYTE* presence, size_t field_index) {
    const size_t byte_index = field_index / 8;
    const uint8_t bit_mask = static_cast<uint8_t>(1u << (field_index % 8));
    return (presence[byte_index] & bit_mask) != 0;
}

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

    const auto f_list = reflected_fields(n.m_recovery);
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
    if (target_tag == FF_RECOVER_UNDEFINED && out_kind != FF_FIELD_UNKNOWN) {
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
        case FF_FIELD_CODE:
            return Node(e.base, size, version, slot_offset, expected_tag, schema_kind,
                        FF_RECOVER_UNDEFINED, false, compact_ops, e.m_engine_version);

        case FF_FIELD_CHOICE:
            return Node::resolve_choice(e.base, size, version, slot_offset, slot_offset, schema_kind, compact_ops);

        case FF_FIELD_RESOURCE: {
            Offset child_offset = LOAD_U64(e.base + slot_offset);
            if (child_offset == FF_NULL_OFFSET) return {};
            RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>(LOAD_U16(e.base + slot_offset + DATA_BLOCK::RECOVERY));
            return Node(e.base, size, version, child_offset, actual_tag, FF_FIELD_BLOCK,
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
            return Node(e.base, size, version, child_offset, RECOVER_FF_STRING, schema_kind,
                        FF_RECOVER_UNDEFINED, false, standard_ops_ptr(), e.m_engine_version);
        }

        default: {
            Offset child_offset = LOAD_U64(e.base + slot_offset);
            if (child_offset == FF_NULL_OFFSET) return {};
            RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>(LOAD_U16(e.base + child_offset + DATA_BLOCK::RECOVERY));
            return Node(e.base, size, version, child_offset, actual_tag, schema_kind,
                        FF_RECOVER_UNDEFINED, false, compact_ops, e.m_engine_version);
        }
    }
}

static const ParserOps STANDARD_OPS{
    FF_STREAM_LAYOUT_STANDARD,
    &ParserOps::standard_node_size,
    &ParserOps::standard_node_entries,
    &ParserOps::standard_node_lookup_field,
    &ParserOps::standard_node_lookup_index,
    &ParserOps::standard_entry_as_node,
};

static const ParserOps COMPACT_OPS{
    FF_STREAM_LAYOUT_COMPACT,
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

static const ParserOps* select_ops(FF_StreamLayout layout) {
    switch (layout) {
        case FF_STREAM_LAYOUT_STANDARD: return &STANDARD_OPS;
        case FF_STREAM_LAYOUT_COMPACT:  return &COMPACT_OPS;
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
    m_root_offset        = header.get_root(m_base);
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
    m_root_offset        = header.get_root(m_base);
    m_root_recovery      = header.get_root_type(m_base);
    m_url_dir_offset     = header.get_url_dir_offset(m_base);
    m_module_reg_offset  = header.get_module_reg_offset(m_base);
}

uint32_t Parser::version()   const { return m_version; }
uint16_t Parser::root_type() const { return m_root_recovery; }

Parser::ChecksumValidation Parser::checksum() const {
    FF_HEADER header(m_size);
    FF_CHECKSUM cs = header.get_checksum(m_base);

    if (!cs || cs.__offset + FF_CHECKSUM::HEADER_SIZE > m_size)
        return {};

    return {
        m_base,
        static_cast<size_t>(cs.__offset),
        cs.get_algorithm(m_base),
        cs.get_hash_view(m_base)
    };
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

static std::string_view get_choice_suffix(RECOVERY_TAG tag) {
    switch (tag) {
        case RECOVER_FF_BOOL:    return "Boolean";
        case RECOVER_FF_FLOAT64: return "Decimal";
        case RECOVER_FF_INT32:   return "Integer";
        case RECOVER_FF_UINT32:  return "UnsignedInt";
        case RECOVER_FF_INT64:
        case RECOVER_FF_UINT64:  return "Integer64";
        case RECOVER_FF_STRING:  return "String";
        default:
            // For complex types, pull the capitalized resource/data type name
            return FastFHIR::reflected_resource_type(tag);
    }
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

                if (!first) out << ",";
                out << "\"" << f.name;

                // Utilize Entry's native target_recovery metadata
                if (f.kind == FF_FIELD_CHOICE) out << get_choice_suffix(child_entry.target_recovery);
                out << "\":";

                // Scalars are inline values, not DATA_BLOCKs — serialize directly from Entry.
                // Everything else is a DATA_BLOCK and goes through Node.
                switch (f.kind) {
                    case FF_FIELD_BOOL:
                    case FF_FIELD_INT32:
                    case FF_FIELD_UINT32:
                    case FF_FIELD_INT64:
                    case FF_FIELD_UINT64:
                    case FF_FIELD_FLOAT64:
                    case FF_FIELD_CODE:
                        child_entry.print_scalar_json(out, m_version);
                        break;
                    default:
                        child_entry.as_node().print_json(out);
                        break;
                }
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
        case FF_FIELD_FLOAT64: out << as<double>(); break;
        case FF_FIELD_CODE:
            out << "\"";
            escape_json_string(out, as<std::string_view>());
            out << "\"";
            break;
        default: break;
    }
}

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
            out << Decode::scalar<double>(base, slot, RECOVER_FF_FLOAT64);
            break;
        case FF_FIELD_CODE: {
            uint32_t raw = LOAD_U32(base + slot);
            if (raw == FF_CODE_NULL) { out << "null"; break; }
            if (raw & FF_CUSTOM_STRING_FLAG) {
                Offset str_off = parent_offset + static_cast<Offset>(raw & ~FF_CUSTOM_STRING_FLAG);
                out << '"';
                escape_json_string(out, FF_STRING(str_off, 0, version).read_view(base));
                out << '"';
            } else {
                if (const char* resolved = FF_ResolveCode(raw, version)) {
                    out << '"';
                    escape_json_string(out, resolved);
                    out << '"';
                } else {
                    out << "null";
                }
            }
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
      m_size(size),
      m_version(version),
      m_engine_version(engine_ver),
      m_node_offset(offset),
      m_recovery(recovery),
      m_child_recovery(child_recovery),
      m_kind(kind),
            m_array_entries_are_offsets(array_entries_are_offsets),
            m_ops(ops) {}

Node Node::resolve_choice(const BYTE* base, Size size, uint32_t version, 
                                                    Offset parent_offset, Offset value_offset, FF_FieldKind schema_kind,
                                                    const ParserOps* ops) {
    assert(schema_kind == FF_FIELD_CHOICE && "resolve_choice called on non-choice V-Table slot");
    RECOVERY_TAG tag = static_cast<RECOVERY_TAG>(LOAD_U16(base + value_offset + DATA_BLOCK::RECOVERY));
    
    if ((tag & 0xFF00) == RECOVER_FF_SCALAR_BLOCK) { 
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

        case FF_FIELD_BOOL:
        case FF_FIELD_INT32:
        case FF_FIELD_UINT32:
        case FF_FIELD_INT64:
        case FF_FIELD_UINT64:
        case FF_FIELD_FLOAT64:
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
bool Node::is_scalar() const { return m_kind == FF_FIELD_BOOL || m_kind == FF_FIELD_INT32 || m_kind == FF_FIELD_UINT32 || m_kind == FF_FIELD_INT64 || m_kind == FF_FIELD_UINT64 || m_kind == FF_FIELD_FLOAT64 || m_kind == FF_FIELD_CODE; }

FF_FieldKind Node::kind()     const { return m_kind; }
RECOVERY_TAG Node::recovery() const { return m_recovery; }

// =====================================================================
// Node reflection helpers
// =====================================================================
std::vector<FF_FieldInfo> Node::fields() const {
    if (!is_object()) return {};
    return reflected_fields(m_recovery);
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
        return reflected_fields(n.m_recovery).size();
    }
    return 0;
}

std::vector<Node> ParserOps::standard_node_entries(const Node& n) {
    std::vector<Node> out;
    if (!n.is_array()) return out;

    FF_ARRAY array(n.m_node_offset, n.m_size, n.m_version, n.m_engine_version);
    uint32_t count = array.entry_count(n.m_base);
    uint16_t step  = array.entry_step(n.m_base);

    out.reserve(count);
    Offset entries_start = n.m_node_offset + array.get_header_size();

    for (uint32_t i = 0; i < count; ++i) {
        Offset item_ptr = entries_start + (i * step);

        // --- INLINE POLYMORPHIC TUPLE ARRAY ---
        if (n.m_child_recovery == RECOVER_FF_RESOURCE && !n.m_array_entries_are_offsets) {
            Offset actual_off = LOAD_U64(n.m_base + item_ptr);
            
            if (actual_off == FF_NULL_OFFSET) {
                out.push_back(Node());
                continue;
            }
            
            RECOVERY_TAG tuple_tag = static_cast<RECOVERY_TAG>(LOAD_U16(n.m_base + item_ptr + DATA_BLOCK::RECOVERY));
            RECOVERY_TAG block_tag = static_cast<RECOVERY_TAG>(LOAD_U16(n.m_base + actual_off + DATA_BLOCK::RECOVERY));
            
            if (tuple_tag != block_tag) throw std::runtime_error
                ("Node::entries() Inline polymorphic tuple array (RECOVERY_FF_RESOURCE) mismatch with actual type");
            
            out.push_back(Node(n.m_base, n.m_size, n.m_version, actual_off, tuple_tag, FF_FIELD_BLOCK,
                               FF_RECOVER_UNDEFINED, false, n.m_ops, n.m_engine_version));
            continue;
        }

        // --- STANDARD POINTER ARRAY (e.g., Bundle.entry, Patient.name) ---
        if (n.m_array_entries_are_offsets) {
            Offset child_off = LOAD_U64(n.m_base + item_ptr);
            if (child_off == FF_NULL_OFFSET) {
                out.push_back(Node());
                continue;
            }

            // Use the actual recovery tag stored in the block — the schema's child_recovery
            // may differ (e.g., code arrays store FF_STRING blocks with RECOVER_FF_STRING,
            // even though the schema marks child_recovery as RECOVER_FF_CODE).
            RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>(
                LOAD_U16(n.m_base + child_off + DATA_BLOCK::RECOVERY)
            );

            FF_FieldKind child_kind = FF_FIELD_BLOCK;
            switch (actual_tag) {
            case RECOVER_FF_STRING: child_kind = FF_FIELD_STRING; break;
            default: break;
            }

            out.push_back(Node(n.m_base, n.m_size, n.m_version, child_off, actual_tag, child_kind,
                               FF_RECOVER_UNDEFINED, false, n.m_ops, n.m_engine_version));
            continue;
        }
        
        // --- FAST PATH: INLINE ARRAY (Structs) ---
        out.push_back(Node(n.m_base, n.m_size, n.m_version, item_ptr, n.m_child_recovery, FF_FIELD_BLOCK,
                           FF_RECOVER_UNDEFINED, false, n.m_ops, n.m_engine_version));
    }
    
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
    if (target_tag == FF_RECOVER_UNDEFINED && key.kind != FF_FIELD_UNKNOWN) {
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
    uint32_t count = arr.entry_count(n.m_base);
    if (index >= count) return {};

    const BYTE* entries_start = arr.entries(n.m_base);
    uint16_t step = arr.entry_step(n.m_base);
    FF_ARRAY::EntryKind ekind = arr.entry_kind(n.m_base);

    switch (ekind) {
        case FF_ARRAY::SCALAR: {
            Offset item_off = n.m_node_offset + arr.get_header_size() + index * step;
            return Node(n.m_base, n.m_size, n.m_version, item_off, n.m_child_recovery, n.m_kind,
                        FF_RECOVER_UNDEFINED, false, n.m_ops, n.m_engine_version);
        }
        case FF_ARRAY::OFFSET: {
            Offset ptr_off = static_cast<Offset>(entries_start - n.m_base) + index * step;
            Offset item_off = LOAD_U64(n.m_base + ptr_off);
            if (item_off == FF_NULL_OFFSET) return {};
            RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>(LOAD_U16(n.m_base + item_off + DATA_BLOCK::RECOVERY));
            return Node(n.m_base, n.m_size, n.m_version, item_off, actual_tag, FF_FIELD_BLOCK,
                        FF_RECOVER_UNDEFINED, false, n.m_ops, n.m_engine_version);
        }
        case FF_ARRAY::INLINE_BLOCK: {
            Offset item_off = static_cast<Offset>(entries_start - n.m_base) + index * step;
            RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>(LOAD_U16(n.m_base + item_off + DATA_BLOCK::RECOVERY));
            return Node(n.m_base, n.m_size, n.m_version, item_off, actual_tag, FF_FIELD_BLOCK,
                        FF_RECOVER_UNDEFINED, false, n.m_ops, n.m_engine_version);
        }
    }
    return {};
}

Node Entry::as_node(Size size, uint32_t version, RECOVERY_TAG expected_tag, FF_FieldKind schema_kind,
                    const ParserOps* ops) const {
    const ParserOps* use_ops = ops ? ops : &STANDARD_OPS;
    return use_ops->entry_as_node(*this, size, version, expected_tag, schema_kind, use_ops);
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
        case FF_FIELD_CODE:
            // Scalar slots are inline values at slot_offset.
            return Node(e.base, size, version, slot_offset, expected_tag, schema_kind,
                        FF_RECOVER_UNDEFINED, false, ops, e.m_engine_version);

        case FF_FIELD_CHOICE: 
            return Node::resolve_choice(e.base, size, version, slot_offset, slot_offset, schema_kind, ops);

        case FF_FIELD_RESOURCE: {
            Offset actual_off = LOAD_U64(e.base + slot_offset);
            if (actual_off == FF_NULL_OFFSET) return {};
            RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>(LOAD_U16(e.base + slot_offset + DATA_BLOCK::RECOVERY));
            return Node(e.base, size, version, actual_off, actual_tag, FF_FIELD_BLOCK,
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
            RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>(LOAD_U16(e.base + child_offset + DATA_BLOCK::RECOVERY));
            return Node(e.base, size, version, child_offset, actual_tag, schema_kind,
                        FF_RECOVER_UNDEFINED, false, ops, e.m_engine_version);
        }
    }
}
} // namespace Reflective
} // namespace FastFHIR
