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

namespace FastFHIR {

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

Node Node::scalar(const BYTE* base, Size size, uint32_t version,
                  Offset parent_offset, Offset scalar_offset, FF_FieldKind kind) {
    Node node;
    node.m_base = base;
    node.m_size = size;
    node.m_version = version;
    node.m_node_offset = parent_offset;        
    node.m_global_scalar_offset = scalar_offset;
    node.m_kind = kind;
    return node;
}

bool Node::is_empty() const {
    if (!*this) return true;
    
    if (is_array()) return size() == 0;
    
    if (is_string() || kind() == FF_FIELD_CODE) 
        return as<std::string_view>().empty();
    
    if (is_scalar())
        return FF_IsFieldEmpty(m_base, m_global_scalar_offset, m_kind);
    
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
        && (m_node_offset != FF_NULL_OFFSET || m_global_scalar_offset != FF_NULL_OFFSET);
}

bool Node::is_array()  const { return m_kind == FF_FIELD_ARRAY; }
bool Node::is_object() const { return m_kind == FF_FIELD_BLOCK; }
bool Node::is_string() const { return m_kind == FF_FIELD_STRING; }
bool Node::is_scalar() const { return m_global_scalar_offset != FF_NULL_OFFSET; }

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
        // --- INLINE POLYMORPHIC TUPLE ARRAY (e.g., Patient.contained) ---
        if (m_child_recovery == RECOVER_FF_RESOURCE && !m_array_entries_are_offsets) {
            Offset item_ptr = entries_start + (i * step);
            Offset actual_off = LOAD_U64(m_base + item_ptr);
            RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>
                (LOAD_U16(m_base + item_ptr + DATA_BLOCK::RECOVERY));
            
            if (actual_off == FF_NULL_OFFSET) {
                out.push_back(Node());
            } else {
                out.push_back(Node(m_base, m_size, m_version, actual_off, actual_tag, FF_FIELD_BLOCK));
            }
            continue;
        }
        
        // --- 2. STANDARD POINTER ARRAY ---
        if (m_array_entries_are_offsets) {
            Offset child_off = LOAD_U64(m_base + entries_start + (i * step));
            if (child_off == FF_NULL_OFFSET) {
                out.push_back(Node());
                continue;
            }
            RECOVERY_TAG actual_recovery = static_cast<RECOVERY_TAG>(LOAD_U16(m_base + child_off + DATA_BLOCK::RECOVERY));
            FF_FieldKind child_kind = (m_child_recovery == FF_STRING::recovery) ? FF_FIELD_STRING : FF_FIELD_BLOCK;
            out.push_back(Node(m_base, m_size, m_version, child_off, actual_recovery, child_kind));
        } 
        // --- 3. FAST PATH: INLINE STRUCT ARRAY ---
        else {
            Offset item_off = entries_start + (i * step);
            out.push_back(Node(m_base, m_size, m_version, item_off, m_child_recovery, FF_FIELD_BLOCK));
        }
    }
    
    return out;
}
// =====================================================================
// Node child lookup
// =====================================================================
Node Node::operator[](FF_FieldKey key) const {
    if (!is_object()) return {};

    // --- 1. INHERITANCE-AWARE SCHEMA VALIDATION ---
    if (key.owner_recovery != FF_RECOVER_UNDEFINED &&
        key.owner_recovery != m_recovery) {
        
        // Allow inherited base-class keys (Resource.id) to access valid concrete resources
        if (!(key.owner_recovery == RECOVER_FF_RESOURCE && FF_IsResourceTag(m_recovery))) {
            return {};
        }
    }

    // --- 2. DYNAMIC LOOKUP ---
    if (key.kind == FF_FIELD_UNKNOWN) {
        const std::string_view key_name = key.view();
        if (!key_name.empty()) 
            return reflected_child_node(m_base, m_size, m_version, m_node_offset, m_recovery, key_name);
        return {};
    }

    const Offset value_offset = m_node_offset + key.field_offset;

    // --- 3. FAST PATH: EMPTY SLOT ---
    if (FF_IsFieldEmpty(m_base, value_offset, key.kind)) {
        return {};
    }

    // --- 4. AUTOMATIC INDIRECTION & NODE CONSTRUCTION ---
    switch (key.kind) {
        
        // -- INLINE SCALARS (Zero Indirection) --
        case FF_FIELD_BOOL:
        case FF_FIELD_UINT32:
        case FF_FIELD_FLOAT64:
        case FF_FIELD_CODE: 
            return Node::scalar(
                m_base, m_size, m_version, 
                m_node_offset, value_offset, key.kind
            );

        // -- INLINE POLYMORPHIC TUPLE --
        case FF_FIELD_RESOURCE: {
            Offset actual_off = LOAD_U64(m_base + value_offset);
            RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>
                (LOAD_U16(m_base + value_offset + DATA_BLOCK::RECOVERY));
            
            if (actual_off == FF_NULL_OFFSET) return {};
            
            return Node(m_base, m_size, m_version, actual_off, actual_tag, FF_FIELD_BLOCK);
        }

        // -- COMPLEX TYPES (Standard 8-Byte Pointers) --
        case FF_FIELD_BLOCK:
        case FF_FIELD_ARRAY:
        case FF_FIELD_STRING: {
            // Standard pointer hop
            Offset child_offset = LOAD_U64(m_base + value_offset);
            if (child_offset == FF_NULL_OFFSET) return {};

            RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>(
                LOAD_U16(m_base + child_offset + DATA_BLOCK::RECOVERY)
            );

            return Node(
                m_base, m_size, m_version, 
                child_offset, actual_tag, key.kind, 
                key.child_recovery, key.array_entries_are_offsets
            );
        }

        default:
            return {};
    }
}

Node Node::operator[](size_t index) const {
    if (!is_array()) return {};

    FF_ARRAY array(m_node_offset, m_size, m_version);
    auto count = array.entry_count(m_base);
    if (index >= count) return {};

    // Jump to the specific array slot based on the pre-calculated step size
    const BYTE* entry = array.entries(m_base) + index * array.entry_step(m_base);

    // --- INLINE POLYMORPHIC TUPLE ARRAY (e.g., Patient.contained) ---
    if (m_child_recovery == RECOVER_FF_RESOURCE && !m_array_entries_are_offsets) {
        Offset actual_off = LOAD_U64(entry);
        RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>
            (LOAD_U16(entry + DATA_BLOCK::RECOVERY));
        
        if (actual_off == FF_NULL_OFFSET) return {};
        
        return Node(m_base, m_size, m_version, actual_off, actual_tag, FF_FIELD_BLOCK);
    }

    // --- STANDARD POINTER ARRAY (e.g., Patient.name) ---
    if (m_array_entries_are_offsets) {
        Offset child_off = LOAD_U64(entry);
        if (child_off == FF_NULL_OFFSET) return {}; 

        RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>(
            LOAD_U16(m_base + child_off + DATA_BLOCK::RECOVERY)
        );
        
        FF_FieldKind child_kind = (m_child_recovery == FF_STRING::recovery) ? FF_FIELD_STRING : FF_FIELD_BLOCK;
        
        return Node(m_base, m_size, m_version, child_off, actual_tag, child_kind);
    } 
    
    // --- FAST PATH: INLINE ARRAY ---
    return Node(m_base, m_size, m_version,
                static_cast<Offset>(entry - m_base),
                m_child_recovery, FF_FIELD_BLOCK);
}

// =====================================================================
// Node primitive accessors
// =====================================================================
template <>
std::string_view Node::as<std::string_view>() const {
    if (!*this || m_kind == FF_FIELD_UNKNOWN) return {};

    switch (m_kind)
    {
    case FF_FIELD_STRING:
        return FF_STRING(m_node_offset, m_size, m_version).read_view(m_base);
        case FF_FIELD_CODE: {
            uint32_t raw_code = LOAD_U32(m_base + m_global_scalar_offset);
            if (raw_code == FF_CODE_NULL) return {};
            
            // 1. Evaluate the MSB for Custom Strings safely
            if ((raw_code & FF_CUSTOM_STRING_FLAG) != 0) {
                Offset rel_off = static_cast<Offset>(raw_code & ~FF_CUSTOM_STRING_FLAG);
                return FF_STRING(m_node_offset + rel_off, m_size, m_version).read_view(m_base);
            }
            // 2. Standard Dictionary Lookup
            else if (const char* resolved = FF_ResolveCode(raw_code, m_version)) {
                return {resolved};
            }
        } break;

    default:
        break;
    }
    return {};
}

template <>
bool Node::as<bool>() const {
    if (!*this || m_kind != FF_FIELD_BOOL) return false;
    return LOAD_U8(m_base + m_global_scalar_offset) != 0;
}

template <>
uint32_t Node::as<uint32_t>() const {
    if (!*this || m_kind != FF_FIELD_UINT32) return 0;
    return LOAD_U32(m_base + m_global_scalar_offset);
}

template <>
double Node::as<double>() const {
    if (!*this || m_kind != FF_FIELD_FLOAT64) return 0.0;
    return LOAD_F64(m_base + m_global_scalar_offset);
}

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

Node Parser::root() const {
    return Node(m_base, m_size, m_version,
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


void Node::print_json(std::ostream& out) const {
    if (is_empty()) return;

    if (is_object()) {
        out << "{";
        auto f_list = fields();
        bool first = true;

        // First print the standard Resource Type! It's not required but encouraged.
        // FastFHIR assigns standard Resources to the 0x0200 recovery tag range.
        // BackboneElements (0x0300) and DataTypes (0x0100) do not get a resourceType.
        if (m_recovery >= 0x0200 && m_recovery < 0x0300) {
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
            auto child = (*this)[key];
            
            if (!first) out << ",";
            out << "\"" << f.name << "\":";
            child.print_json(out);
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
    // Scalar / Leaf Node
    else {
        switch (m_kind) {
        case FF_FIELD_STRING:
        case FF_FIELD_CODE:
            out << "\"";
            escape_json_string(out, as<std::string_view>());
            out << "\"";
            break;
        case FF_FIELD_BOOL:
            out << (as<bool>() ? "true" : "false");
            break;
        case FF_FIELD_UINT32:
            out << as<uint32_t>();
            break;
        case FF_FIELD_FLOAT64:
            out << as<double>();
            break;
        default:
            break;
        }
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

} // namespace FastFHIR
