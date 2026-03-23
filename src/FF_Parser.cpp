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
      m_offset(offset),
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
    node.m_offset = parent_offset;        
    node.m_scalar_offset = scalar_offset;
    node.m_kind = kind;
    return node;
}

bool Node::is_empty() const {
    if (!*this) return true;
    
    if (is_array()) return size() == 0;
    
    if (is_string() || kind() == FF_FIELD_CODE) {
        auto val = value();
        return !val || val.as_string().empty();
    }
    
    if (is_scalar()) {
        auto val = value();
        return !val.has_value; 
    }
    
    if (is_object()) {
        auto k = keys();
        for (size_t i = 0; i < k.size(); ++i) {
            if (!(*this)[k[i]].is_empty()) return false;
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
        && (m_offset != FF_NULL_OFFSET || m_scalar_offset != FF_NULL_OFFSET);
}

bool Node::is_array()  const { return m_kind == FF_FIELD_ARRAY; }
bool Node::is_object() const { return m_kind == FF_FIELD_BLOCK; }
bool Node::is_string() const { return m_kind == FF_FIELD_STRING; }
bool Node::is_scalar() const { return m_scalar_offset != FF_NULL_OFFSET; }

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
        FF_ARRAY array(m_offset, m_size, m_version);
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
    FF_ARRAY array(m_offset, m_size, m_version);
    auto count = array.entry_count(m_base);
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        out.push_back((*this)[static_cast<size_t>(i)]);
    }
    return out;
}

// =====================================================================
// Node child lookup
// =====================================================================
Node Node::operator[](std::string_view key) const {
    if (!is_object()) return {};

    Node result = reflected_child_node(
        m_base, m_size, m_version,
        m_offset, m_recovery, key);

    return result;
}

Node Node::operator[](FF_FieldKey key) const {
    if (!is_object()) return {};

    if (key.owner_recovery != 0 && key.owner_recovery != m_recovery)
        return {};

    if (key.kind == FF_FIELD_UNKNOWN) {
        const std::string_view key_name = key.view();
        if (!key_name.empty()) return this->operator[](key_name);
        return {};
    }

    const Offset value_offset = m_offset + key.field_offset;
    Node result;

    if (key.kind == FF_FIELD_STRING) {
        const Offset child_offset = LOAD_U64(m_base + value_offset);
        if (child_offset != FF_NULL_OFFSET)
            result = Node(m_base, m_size, m_version,
                          child_offset, FF_STRING::recovery, FF_FIELD_STRING);
    } else if (key.kind == FF_FIELD_ARRAY) {
        const Offset child_offset = LOAD_U64(m_base + value_offset);
        if (child_offset != FF_NULL_OFFSET)
            result = Node(m_base, m_size, m_version,
                          child_offset, FF_ARRAY::recovery, FF_FIELD_ARRAY,
                          key.child_recovery, key.array_entries_are_offsets != 0);
    } else if (key.kind == FF_FIELD_BLOCK) {
        const Offset child_offset = LOAD_U64(m_base + value_offset);
        if (child_offset != FF_NULL_OFFSET)
            result = Node(m_base, m_size, m_version,
                          child_offset, key.child_recovery, FF_FIELD_BLOCK);
    } else {
        result = Node::scalar(m_base, m_size, m_version,
                              m_offset, value_offset, key.kind);
    }

    return result;
}

Node Node::operator[](size_t index) const {
    if (!is_array()) return {};

    FF_ARRAY array(m_offset, m_size, m_version);
    auto count = array.entry_count(m_base);
    if (index >= count) return {};

    const BYTE* entry = array.entries(m_base) + index * array.entry_step(m_base);
    Node result;

    if (m_array_entries_are_offsets) {
        Offset child_offset = LOAD_U64(entry);
        if (child_offset != FF_NULL_OFFSET)
            result = Node(m_base, m_size, m_version,
                          child_offset, m_child_recovery, FF_FIELD_STRING);
    } else {
        result = Node(m_base, m_size, m_version,
                      static_cast<Offset>(entry - m_base),
                      m_child_recovery, FF_FIELD_BLOCK);
    }

    return result;
}

// =====================================================================
// Node value accessor
// =====================================================================
Node::Value Node::value() const {
    Value out;
    out.kind = m_kind;
    if (!m_base) return out;

    switch (m_kind) {
        case FF_FIELD_BOOL:
            if (m_scalar_offset != FF_NULL_OFFSET) {
                uint8_t raw_bool = LOAD_U8(m_base + m_scalar_offset);
                if (raw_bool == FF_NULL_UINT8) {
                    out.has_value = false;
                } else {
                    out.payload.bool_value = (raw_bool != 0); // 0 is False, 1 is True
                    out.has_value = true;
                }
            }
            break;
        case FF_FIELD_CODE:
            if (m_scalar_offset != FF_NULL_OFFSET) {
                uint32_t raw_code = LOAD_U32(m_base + m_scalar_offset);
                
                // 1. Trap the MAX null before bitwise operations
                if (raw_code == FF_CODE_NULL) {
                    out.has_value = false;
                }
                // 2. Evaluate the MSB for Custom Strings safely
                else if ((raw_code & FF_CUSTOM_STRING_FLAG) != 0) {
                    Offset rel_off = static_cast<Offset>(raw_code & ~FF_CUSTOM_STRING_FLAG);
                    Offset custom_off = m_offset + rel_off;
                    if (rel_off > 0 && custom_off < m_size) {
                        out.payload.string_value = FF_STRING(custom_off, m_size, m_version).read_view(m_base);
                        out.has_value = true;
                    }
                }
                // 3. Standard Dictionary Lookup
                else if (const char* resolved = FF_ResolveCode(raw_code, m_version)) {
                    // Ensure valid pointer AND that it isn't an out-of-bounds empty string fallback
                    if (resolved && resolved[0] != '\0') {
                        out.payload.string_value = resolved;
                        out.has_value = true;
                    } else {
                        out.has_value = false;
                    }
                }
            }
            break;
        case FF_FIELD_UINT32:
            if (m_scalar_offset != FF_NULL_OFFSET) {
                uint32_t raw = LOAD_U32(m_base + m_scalar_offset);
                if (raw == FF_NULL_UINT32) {
                    out.has_value = false;
                } else {
                    out.payload.uint32_value = raw;
                    out.has_value = true;
                }
            }
            break;
        case FF_FIELD_FLOAT64:
            if (m_scalar_offset != FF_NULL_OFFSET) {
                double raw = LOAD_F64(m_base + m_scalar_offset);
                if (raw == FF_NULL_F64) {
                    out.has_value = false;
                } else {
                    out.payload.float64_value = raw;
                    out.has_value = true;
                }
            }
            break;
        case FF_FIELD_STRING:
            if (m_offset != FF_NULL_OFFSET) {
                out.payload.string_value = FF_STRING(m_offset, m_size, m_version).read_view(m_base);
                out.has_value = true;
            }
            break;
        default:
            break;
    }
    return out;
}

uint32_t Parser::version()   const { return m_version; }
uint16_t Parser::root_type() const { return m_root_recovery; }

// =====================================================================
// Parser factory
// =====================================================================
Parser Parser::create(const void* buffer, size_t size) {
    auto base = static_cast<const BYTE*>(buffer);

    if (size < FF_HEADER::HEADER_SIZE)
        throw std::runtime_error("FastFHIR: Buffer too small to contain file header.");

    FF_HEADER header(size);
    auto res = header.validate_full(base);
    if (!res)
        throw std::runtime_error("FastFHIR Header Validation: " + res.message);

    Parser parser;
    parser.m_base = base;
    parser.m_size = size;
    parser.m_version = header.get_version(base);
    parser.m_root_offset = header.get_root(base);
    parser.m_root_recovery = header.get_root_type(base);
    return parser;
}

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
        auto k = keys();
        bool first = true;

        // FastFHIR assigns standard Resources to the 0x0200 recovery tag range.
        // BackboneElements (0x0300) and DataTypes (0x0100) do not get a resourceType.
        if (m_recovery >= 0x0200 && m_recovery < 0x0300) {
            out << "\"resourceType\":\"" << reflected_resource_type(m_recovery) << "\"";
            first = false;
        }

        for (size_t i = 0; i < k.size(); ++i) {
            auto child = (*this)[k[i]];
            if (!child.is_empty()) {
                if (!first) out << ",";
                out << "\"" << k[i] << "\":";
                child.print_json(out);
                first = false;
            }
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
        auto val = value();
        if (m_kind == FF_FIELD_STRING || m_kind == FF_FIELD_CODE) {
            out << "\"";
            escape_json_string(out, val.as_string());
            out << "\"";
        } else if (m_kind == FF_FIELD_BOOL) {
            out << (val.as_bool() ? "true" : "false");
        } else if (m_kind == FF_FIELD_UINT32) {
            out << val.as_uint32();
        } else if (m_kind == FF_FIELD_FLOAT64) {
            out << val.as_float64();
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
