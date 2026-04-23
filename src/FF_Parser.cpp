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
#include "../generated_src/FF_Dictionary.hpp"
#include "../generated_src/FF_Reflection.hpp"
#include <assert.h>

namespace FastFHIR {
// =====================================================================
// Parser implementation
// =====================================================================
Parser::Parser(const void* buffer, size_t size) : m_memory(), m_base(static_cast<const BYTE*>(buffer)), m_size(size) {
    if (size < FF_HEADER::HEADER_SIZE) {
        throw std::runtime_error("FastFHIR Parsing Error: Buffer too small to contain a valid header.");
    }
    FF_HEADER header(size);
    auto validation_result = header.validate_full(m_base);
    if (validation_result != FF_SUCCESS) {
        throw std::runtime_error("FastFHIR Parsing Error: Header validation failed with error " + validation_result.message);
    }
    m_version = header.get_fhir_rev(m_base);
    m_root_offset = header.get_root(m_base);
    m_root_recovery = header.get_root_type(m_base);
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
    m_version = header.get_fhir_rev(m_base);
    m_root_offset = header.get_root(m_base);
    m_root_recovery = header.get_root_type(m_base);
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
                m_root_offset, m_root_recovery, FF_FIELD_BLOCK);
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
    if (tag == RECOVER_FF_BOOL) return "Boolean";
    if (tag == RECOVER_FF_FLOAT64) return "Decimal";
    if (tag == RECOVER_FF_INT32) return "Integer";
    if (tag == RECOVER_FF_UINT32) return "UnsignedInt";
    if (tag == RECOVER_FF_INT64 || tag == RECOVER_FF_UINT64) return "Integer64";
    if (tag == RECOVER_FF_STRING) return "String";
    
    // For complex types, pull the capitalized resource/data type name
    std::string_view name = FastFHIR::reflected_resource_type(tag);
    return name;
}

void Reflective::Node::print_json(std::ostream& out) const {
    if (is_empty()) return;

    if (is_object()) {
        out << "{";
        auto f_list = fields();
        bool first = true;

        if (FF_IsResourceTag(m_recovery)) {
            out << "\"resourceType\":\"" << reflected_resource_type(m_recovery) << "\"";
            first = false;
        }

        for (size_t i = 0; i < f_list.size(); ++i) {
            const auto& f = f_list[i];

            // 1. FAST PATH: Raw memory peek. Skip completely field if empty!
            if (FF_IsFieldEmpty(m_base, m_node_offset + f.field_offset, f.kind)) {
                continue;
            }

            // 2. Construct the O(1) field key blueprint
            FF_FieldKey key = FF_FieldKey::from_cstr(
                m_recovery, f.kind, f.field_offset, 
                f.child_recovery, f.array_entries_are_offsets, f.name
            );

            // 3. Pure pointer-math lookup (Zero loops)
            auto child_entry = (*this)[key];
            
            if (!first) out << ",";
            out << "\"" << f.name;
            
            // Utilize Entry's native target_recovery metadata
            if (f.kind == FF_FIELD_CHOICE) out << get_choice_suffix(child_entry.target_recovery);
            out << "\":";
            
            // Scalars are inline values, not DATA_BLOCKs — serialize directly from Entry.
            // Everything else is a DATA_BLOCK and goes through Node.
            switch (f.kind)
            {
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
                child_entry.as_node(m_size, m_version, child_entry.target_recovery, child_entry.kind).print_json(out);
                break;
            }
            first = false;
        }
        out << "}";
    }
    else if (is_array()) {
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
    }
    // Scalar leaf: only reachable for choice[x] nodes resolved to an inline scalar value.
    else if (is_scalar()) {
        switch (m_kind) {
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
        default: out << "null"; break;
        }
    }
    else if (is_string()) {
        out << "\"";
        escape_json_string(out, as<std::string_view>());
        out << "\"";
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
           RECOVERY_TAG child_recovery, bool array_entries_are_offsets)
    : m_base(base),
      m_size(size),
      m_version(version),
      m_node_offset(offset),
      m_recovery(recovery),
      m_child_recovery(child_recovery),
      m_kind(kind),
      m_array_entries_are_offsets(array_entries_are_offsets) {}

Node Node::resolve_choice(const BYTE* base, Size size, uint32_t version, 
                             Offset parent_offset, Offset value_offset, FF_FieldKind schema_kind) {
    assert(schema_kind == FF_FIELD_CHOICE && "resolve_choice called on non-choice V-Table slot");
    RECOVERY_TAG tag = static_cast<RECOVERY_TAG>(LOAD_U16(base + value_offset + DATA_BLOCK::RECOVERY));
    
    if ((tag & 0xFF00) == RECOVER_FF_SCALAR_BLOCK) { 
        Node n(base, size, version, parent_offset, tag, Recovery_to_Kind(tag));
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
    return Node(base, size, version, child_off, tag, dynamic_kind);
}

bool Node::is_empty() const {
    if (!*this) return true;
    
    if (is_array()) return size() == 0;

    // Strings are empty when their decoded view is empty.
    if (is_string())
        return as<std::string_view>().empty();

    // Codes are empty only when the raw slot is the explicit FF_CODE_NULL sentinel.
    // Do not treat unresolved dictionary codes as empty, otherwise print_json can emit
    // invalid key/value pairs like "type":,
    if (kind() == FF_FIELD_CODE)
        return FF_IsFieldEmpty(m_base, m_node_offset, FF_FIELD_CODE);
    
    if (is_scalar())
        return FF_IsFieldEmpty(m_base, m_node_offset, m_kind);
    
    if (is_object()) { 
        auto f_list = fields(); 
        for (size_t i = 0; i < f_list.size(); ++i) {
            const auto& f = f_list[i];
            if (!FF_IsFieldEmpty(m_base, m_node_offset + f.field_offset, f.kind)) 
                return false;
        }
        return true;
    }
    
    return true;
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
    if (is_array()) {
        FF_ARRAY array(m_node_offset, m_size, m_version);
        return array.entry_count(m_base);
    }
    if (is_object()) {
        return fields().size();
    }
    return 0;
}

std::vector<Node> Node::entries() const {
    std::vector<Node> out;
    if (!is_array()) return out;

    FF_ARRAY array(m_node_offset, m_size, m_version);
    uint32_t count = array.entry_count(m_base);
    uint16_t step  = array.entry_step(m_base);

    out.reserve(count);
    Offset entries_start = m_node_offset + FF_ARRAY::HEADER_SIZE;

    for (uint32_t i = 0; i < count; ++i) {
        Offset item_ptr = entries_start + (i * step);

        // --- INLINE POLYMORPHIC TUPLE ARRAY ---
        if (m_child_recovery == RECOVER_FF_RESOURCE && !m_array_entries_are_offsets) {
            Offset actual_off = LOAD_U64(m_base + item_ptr);
            
            if (actual_off == FF_NULL_OFFSET) {
                out.push_back(Node());
                continue;
            }
            
            RECOVERY_TAG tuple_tag = static_cast<RECOVERY_TAG>(LOAD_U16(m_base + item_ptr + DATA_BLOCK::RECOVERY));
            RECOVERY_TAG block_tag = static_cast<RECOVERY_TAG>(LOAD_U16(m_base + actual_off + DATA_BLOCK::RECOVERY));
            
            if (tuple_tag != block_tag) throw std::runtime_error
                ("Node::entries() Inline polymorphic tuple array (RECOVERY_FF_RESOURCE) mismatch with actual type");
            
            out.push_back(Node(m_base, m_size, m_version, actual_off, tuple_tag, FF_FIELD_BLOCK));
            continue;
        }

        // --- STANDARD POINTER ARRAY (e.g., Bundle.entry, Patient.name) ---
        if (m_array_entries_are_offsets) {
            Offset child_off = LOAD_U64(m_base + item_ptr);
            if (child_off == FF_NULL_OFFSET) {
                out.push_back(Node());
                continue;
            }

            // Schema provides the concrete element type. Validate it against the
            // DATA_BLOCK header in debug builds; use the schema type directly in release.
            RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>(
                LOAD_U16(m_base + child_off + DATA_BLOCK::RECOVERY)
            );
            assert((m_child_recovery == FF_RECOVER_UNDEFINED || actual_tag == m_child_recovery)
                   && "Node::entries() pointer-array element tag mismatch with schema child_recovery");

            FF_FieldKind child_kind = FF_FIELD_BLOCK;
            switch (m_child_recovery) {
            case RECOVER_FF_STRING: child_kind = FF_FIELD_STRING; break;
            case RECOVER_FF_CODE:   child_kind = FF_FIELD_CODE;   break;
            default: break;
            }

            RECOVERY_TAG use_tag = (m_child_recovery != FF_RECOVER_UNDEFINED) ? m_child_recovery : actual_tag;
            out.push_back(Node(m_base, m_size, m_version, child_off, use_tag, child_kind));
            continue;
        }
        
        // --- FAST PATH: INLINE ARRAY (Structs) ---
        out.push_back(Node(m_base, m_size, m_version, item_ptr, m_child_recovery, FF_FIELD_BLOCK));
    }
    
    return out;
}
// =====================================================================
// Node child lookup
// =====================================================================
const Entry NULL_ENTRY = {nullptr, FF_NULL_OFFSET, 0, FF_RECOVER_UNDEFINED, FF_FIELD_UNKNOWN};
Entry Node::operator[](FF_FieldKey key) const {
    if (!is_object()) return {nullptr, FF_NULL_OFFSET, 0, FF_RECOVER_UNDEFINED, FF_FIELD_UNKNOWN};

    // 1. Validation
    if (key.owner_recovery != FF_RECOVER_UNDEFINED && key.owner_recovery != m_recovery) {
        if (!(key.owner_recovery == RECOVER_FF_RESOURCE && FF_IsResourceTag(m_recovery))) {
            return NULL_ENTRY;
        }
    }

    const Offset value_offset = m_node_offset + key.field_offset;

    // 2. Fast Path: Empty slot
    if (FF_IsFieldEmpty(m_base, value_offset, key.kind)) {
        return NULL_ENTRY;
    }

    // 3. Return lightweight coordinate entry.
    // Entry::kind already encodes whether this is an array; target_recovery holds the
    // clean element type directly — no ToArrayTag encoding needed or wanted here.
    RECOVERY_TAG target_tag = key.child_recovery;
    if (target_tag == FF_RECOVER_UNDEFINED && key.kind != FF_FIELD_UNKNOWN) {
        target_tag = Kind_to_Recovery(key.kind);
    }

    return {m_base, m_node_offset, key.field_offset, target_tag, key.kind};
}
Node Entry::as_node(Size size, uint32_t version, RECOVERY_TAG expected_tag, FF_FieldKind schema_kind) const {
    if (base == nullptr || absolute_offset() == FF_NULL_OFFSET) return {};

    const Offset slot_offset = absolute_offset();

    switch (schema_kind) {
        case FF_FIELD_BOOL:
        case FF_FIELD_INT32:
        case FF_FIELD_UINT32:
        case FF_FIELD_INT64:
        case FF_FIELD_UINT64:
        case FF_FIELD_FLOAT64:
        case FF_FIELD_CODE:
            // Scalars are not DATA_BLOCKs. Use Entry::print_scalar_json() instead.
            assert(false && "Entry::as_node() called on a scalar kind");
            return {};

        case FF_FIELD_CHOICE: 
            return Node::resolve_choice(base, size, version, slot_offset, slot_offset, schema_kind);

        case FF_FIELD_RESOURCE: {
            Offset actual_off = LOAD_U64(base + slot_offset);
            if (actual_off == FF_NULL_OFFSET) return {};
            RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>(LOAD_U16(base + slot_offset + DATA_BLOCK::RECOVERY));
            return Node(base, size, version, actual_off, actual_tag, FF_FIELD_BLOCK);
        }

        case FF_FIELD_ARRAY: {
            Offset child_offset = LOAD_U64(base + slot_offset);
            if (child_offset == FF_NULL_OFFSET) return {};
            // m_recovery is unused on array Nodes (fields() returns {} for non-objects).
            // expected_tag already encodes the element type exactly — no memory read needed.
            FF_ARRAY arr_hdr(child_offset, size, version);
            bool entries_are_offsets = arr_hdr.entries_are_pointers(base);
            return Node(base, size, version, child_offset, expected_tag, schema_kind,
                        expected_tag, entries_are_offsets);
        }

        default: {
            // Standard pointer hop for Blocks and Strings
            Offset child_offset = LOAD_U64(base + slot_offset);
            if (child_offset == FF_NULL_OFFSET) return {};
            RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>(LOAD_U16(base + child_offset + DATA_BLOCK::RECOVERY));
            return Node(base, size, version, child_offset, actual_tag, schema_kind);
        }
    }
}
} // namespace Reflective
} // namespace FastFHIR
