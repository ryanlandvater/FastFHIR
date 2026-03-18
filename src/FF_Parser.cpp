/**
 * @file FF_Parser.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief 
 * @version 0.1
 * @date 2026-03-18
 * 
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
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
           uint16_t recovery, FF_FieldKind kind,
           uint16_t child_recovery, bool array_entries_are_offsets)
    : m_base(base),
      m_size(size),
      m_version(version),
      m_offset(offset),
      m_recovery(recovery),
      m_child_recovery(child_recovery),
      m_kind(kind),
      m_array_entries_are_offsets(array_entries_are_offsets) {}

Node Node::scalar(const BYTE* base, Size size, uint32_t version,
                  Offset scalar_offset, FF_FieldKind kind) {
    Node node;
    node.m_base = base;
    node.m_size = size;
    node.m_version = version;
    node.m_scalar_offset = scalar_offset;
    node.m_kind = kind;
    return node;
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
uint16_t     Node::recovery() const { return m_recovery; }

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
                              value_offset, key.kind);
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

    if (!m_base)
        return out;

    switch (m_kind) {
        case FF_FIELD_STRING:
            if (m_offset != FF_NULL_OFFSET) {
                out.payload.string_value = FF_STRING(m_offset, m_size, m_version).read_view(m_base);
                out.has_value = true;
            }
            break;
        case FF_FIELD_CODE:
            if (m_scalar_offset != FF_NULL_OFFSET) {
                out.payload.string_value = FF_ResolveCode(LOAD_U32(m_base + m_scalar_offset), m_version);
                out.has_value = true;
            }
            break;
        case FF_FIELD_BOOL:
            if (m_scalar_offset != FF_NULL_OFFSET) {
                out.payload.bool_value = LOAD_U8(m_base + m_scalar_offset) != 0;
                out.has_value = true;
            }
            break;
        case FF_FIELD_UINT32:
            if (m_scalar_offset != FF_NULL_OFFSET) {
                out.payload.uint32_value = LOAD_U32(m_base + m_scalar_offset);
                out.has_value = true;
            }
            break;
        case FF_FIELD_FLOAT64:
            if (m_scalar_offset != FF_NULL_OFFSET) {
                out.payload.float64_value = LOAD_F64(m_base + m_scalar_offset);
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

    if (size < FF_FILE_HEADER::HEADER_SIZE)
        throw std::runtime_error("FastFHIR: Buffer too small to contain file header.");

    FF_FILE_HEADER header(size);
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
    FF_FILE_HEADER header(m_size);
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
