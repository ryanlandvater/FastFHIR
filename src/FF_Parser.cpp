/**
 * @file FF_Parser.cpp
 * @author Ryan Landvater (ryan.landvater@example.com)
 * @brief 
 * @version 0.1
 * @date 2026-03-18
 * 
 * @copyright Copyright (c) 2026
 * 
 */

#include "../include/FF_Utilities.hpp"
#include "../include/FF_Parser.hpp"
#include "../generated_src/FF_Dictionary.hpp"
#include "../generated_src/FF_Reflection.hpp"

#include <unordered_map>

namespace FastFHIR {

// =====================================================================
// Node::Data — internal state + child caches
// =====================================================================
struct Node::Data {
    const BYTE* base = nullptr;
    Size size = 0;
    uint32_t version = 0;
    Offset offset = FF_NULL_OFFSET;
    Offset scalar_offset = FF_NULL_OFFSET;
    uint16_t recovery = 0;
    uint16_t child_recovery = 0;
    FF_FieldKind kind = FF_FIELD_UNKNOWN;
    bool array_entries_are_offsets = false;

    mutable std::unordered_map<std::string, std::shared_ptr<Data>> named_cache;
    mutable std::unordered_map<size_t, std::shared_ptr<Data>> index_cache;
};

// =====================================================================
// Node constructors
// =====================================================================
Node::Node(std::shared_ptr<Data> data) : m_data(std::move(data)) {}

Node::Node(const BYTE* base, Size size, uint32_t version, Offset offset,
           uint16_t recovery, FF_FieldKind kind,
           uint16_t child_recovery, bool array_entries_are_offsets)
    : m_data(std::make_shared<Data>(Data{
          base, size, version, offset, FF_NULL_OFFSET,
          recovery, child_recovery, kind, array_entries_are_offsets, {}, {}})) {}

Node Node::scalar(const BYTE* base, Size size, uint32_t version,
                  Offset scalar_offset, FF_FieldKind kind) {
    return Node(std::make_shared<Data>(Data{
        base, size, version, FF_NULL_OFFSET, scalar_offset,
        0, 0, kind, false, {}, {}}));
}

// =====================================================================
// Node observers
// =====================================================================
Node::operator bool() const {
    return m_data
        && m_data->base != nullptr
        && (m_data->offset != FF_NULL_OFFSET || m_data->scalar_offset != FF_NULL_OFFSET);
}

bool Node::is_array()  const { return m_data && m_data->kind == FF_FIELD_ARRAY; }
bool Node::is_object() const { return m_data && m_data->kind == FF_FIELD_BLOCK; }
bool Node::is_string() const { return m_data && m_data->kind == FF_FIELD_STRING; }
bool Node::is_scalar() const { return m_data && m_data->scalar_offset != FF_NULL_OFFSET; }

FF_FieldKind Node::kind()     const { return m_data ? m_data->kind : FF_FIELD_UNKNOWN; }
uint16_t     Node::recovery() const { return m_data ? m_data->recovery : uint16_t(0); }

// =====================================================================
// Node reflection helpers
// =====================================================================
std::vector<FF_FieldInfo> Node::fields() const {
    if (!is_object()) return {};
    return reflected_fields(m_data->recovery);
}

std::vector<std::string_view> Node::keys() const {
    if (!is_object()) return {};
    return reflected_keys(m_data->recovery);
}

size_t Node::size() const {
    if (is_array()) {
        FF_ARRAY array(m_data->offset, m_data->size, m_data->version);
        return array.entry_count(m_data->base);
    }
    if (is_object()) {
        return fields().size();
    }
    return 0;
}

std::vector<Node> Node::entries() const {
    std::vector<Node> out;
    if (!is_array()) return out;
    FF_ARRAY array(m_data->offset, m_data->size, m_data->version);
    auto count = array.entry_count(m_data->base);
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        out.push_back((*this)[static_cast<size_t>(i)]);
    }
    return out;
}

// =====================================================================
// Node child lookup (cached)
// =====================================================================
Node Node::operator[](std::string_view key) const {
    if (!is_object()) return {};

    const std::string key_str(key);
    auto it = m_data->named_cache.find(key_str);
    if (it != m_data->named_cache.end()) return Node(it->second);

    Node result = reflected_child_node(
        m_data->base, m_data->size, m_data->version,
        m_data->offset, m_data->recovery, key);

    if (result.m_data)
        m_data->named_cache.emplace(std::move(key_str), result.m_data);

    return result;
}

Node Node::operator[](FF_FieldKey key) const {
    if (!is_object()) return {};

    if (key.owner_recovery != 0 && key.owner_recovery != m_data->recovery)
        return {};

    if (key.kind == FF_FIELD_UNKNOWN) {
        const std::string_view key_name = key.view();
        if (!key_name.empty()) return this->operator[](key_name);
        return {};
    }

    // Check named cache using the field key's name
    const std::string_view kv = key.view();
    if (!kv.empty()) {
        const std::string ks(kv);
        auto it = m_data->named_cache.find(ks);
        if (it != m_data->named_cache.end()) return Node(it->second);
    }

    // Resolve the child node
    const Offset value_offset = m_data->offset + key.field_offset;
    Node result;

    if (key.kind == FF_FIELD_STRING) {
        const Offset child_offset = LOAD_U64(m_data->base + value_offset);
        if (child_offset != FF_NULL_OFFSET)
            result = Node(m_data->base, m_data->size, m_data->version,
                          child_offset, FF_STRING::recovery, FF_FIELD_STRING);
    } else if (key.kind == FF_FIELD_ARRAY) {
        const Offset child_offset = LOAD_U64(m_data->base + value_offset);
        if (child_offset != FF_NULL_OFFSET)
            result = Node(m_data->base, m_data->size, m_data->version,
                          child_offset, FF_ARRAY::recovery, FF_FIELD_ARRAY,
                          key.child_recovery, key.array_entries_are_offsets != 0);
    } else if (key.kind == FF_FIELD_BLOCK) {
        const Offset child_offset = LOAD_U64(m_data->base + value_offset);
        if (child_offset != FF_NULL_OFFSET)
            result = Node(m_data->base, m_data->size, m_data->version,
                          child_offset, key.child_recovery, FF_FIELD_BLOCK);
    } else {
        result = Node::scalar(m_data->base, m_data->size, m_data->version,
                              value_offset, key.kind);
    }

    // Cache by name if available
    if (result.m_data && !kv.empty())
        m_data->named_cache.emplace(std::string(kv), result.m_data);

    return result;
}

Node Node::operator[](size_t index) const {
    if (!is_array()) return {};

    auto it = m_data->index_cache.find(index);
    if (it != m_data->index_cache.end()) return Node(it->second);

    FF_ARRAY array(m_data->offset, m_data->size, m_data->version);
    auto count = array.entry_count(m_data->base);
    if (index >= count) return {};

    const BYTE* entry = array.entries(m_data->base) + index * array.entry_step(m_data->base);
    Node result;

    if (m_data->array_entries_are_offsets) {
        Offset child_offset = LOAD_U64(entry);
        if (child_offset != FF_NULL_OFFSET)
            result = Node(m_data->base, m_data->size, m_data->version,
                          child_offset, m_data->child_recovery, FF_FIELD_STRING);
    } else {
        result = Node(m_data->base, m_data->size, m_data->version,
                      static_cast<Offset>(entry - m_data->base),
                      m_data->child_recovery, FF_FIELD_BLOCK);
    }

    if (result.m_data)
        m_data->index_cache.emplace(index, result.m_data);

    return result;
}

// =====================================================================
// Node value accessors
// =====================================================================
std::string_view Node::as_string() const {
    if (!m_data) return {};
    if (m_data->kind == FF_FIELD_STRING && m_data->offset != FF_NULL_OFFSET) {
        return FF_STRING(m_data->offset, m_data->size, m_data->version)
            .read_view(m_data->base);
    }
    if (m_data->kind == FF_FIELD_CODE && m_data->scalar_offset != FF_NULL_OFFSET) {
        return FF_ResolveCode(LOAD_U32(m_data->base + m_data->scalar_offset), m_data->version);
    }
    return {};
}

bool Node::as_bool() const {
    if (!m_data || m_data->kind != FF_FIELD_BOOL || m_data->scalar_offset == FF_NULL_OFFSET)
        return false;
    return LOAD_U8(m_data->base + m_data->scalar_offset) != 0;
}

uint32_t Node::as_uint32() const {
    if (!m_data || m_data->kind != FF_FIELD_UINT32 || m_data->scalar_offset == FF_NULL_OFFSET)
        return 0;
    return LOAD_U32(m_data->base + m_data->scalar_offset);
}

double Node::as_float64() const {
    if (!m_data || m_data->kind != FF_FIELD_FLOAT64 || m_data->scalar_offset == FF_NULL_OFFSET)
        return 0.0;
    return LOAD_F64(m_data->base + m_data->scalar_offset);
}

// =====================================================================
// Parser internals
// =====================================================================
struct Parser::Data {
    const BYTE* base;
    Size        size;
    uint32_t    version;
    Offset      root_offset;
    uint16_t    root_recovery;
};

Parser::Parser(std::shared_ptr<Data> data) : m_data(std::move(data)) {}

Parser::operator bool()    const { return m_data != nullptr; }
uint32_t Parser::version() const { return m_data->version; }
uint16_t Parser::root_type() const { return m_data->root_recovery; }

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

    auto data = std::make_shared<Data>();
    data->base           = base;
    data->size           = size;
    data->version        = header.get_version(base);
    data->root_offset    = header.get_root(base);
    data->root_recovery  = header.get_root_type(base);

    return Parser(std::move(data));
}

Parser::ChecksumValidation Parser::checksum() const {
    FF_FILE_HEADER header(m_data->size);
    FF_CHECKSUM cs = header.get_checksum(m_data->base);

    if (!cs || cs.__offset + FF_CHECKSUM::HEADER_SIZE > m_data->size)
        return {};

    return {
        m_data->base,
        static_cast<size_t>(cs.__offset),
        cs.get_algorithm(m_data->base),
        cs.get_hash_view(m_data->base)
    };
}

Node Parser::root() const {
    return Node(m_data->base, m_data->size, m_data->version,
                m_data->root_offset, m_data->root_recovery, FF_FIELD_BLOCK);
}

} // namespace FastFHIR
