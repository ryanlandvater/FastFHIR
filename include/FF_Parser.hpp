/**
 * @file FF_Parser.hpp
 * @author Ryan Landvater (ryan.landvater@example.com)
 * @copyright Copyright (c) 2026
 * @version 0.1
 * 
 * @brief FastFHIR File Parser Declarations
 * 
 * 
 * 
 */

 #pragma once

#include "FF_Primitives.hpp"
#include "../generated_src/FF_AllTypes.hpp"
#include <stdexcept>
#include <memory>

namespace FastFHIR {

class Node {
    const BYTE* m_base = nullptr;
    Size m_size = 0;
    uint32_t m_version = 0;
    Offset m_offset = FF_NULL_OFFSET;
    Offset m_scalar_offset = FF_NULL_OFFSET;
    uint16_t m_recovery = 0;
    uint16_t m_child_recovery = 0;
    FF_FieldKind m_kind = FF_FIELD_UNKNOWN;
    bool m_array_entries_are_offsets = false;

public:
    Node() = default;
    Node(const BYTE* base, Size size, uint32_t version, Offset offset, uint16_t recovery, FF_FieldKind kind,
         uint16_t child_recovery = 0, bool array_entries_are_offsets = false)
        : m_base(base),
          m_size(size),
          m_version(version),
          m_offset(offset),
          m_scalar_offset(FF_NULL_OFFSET),
          m_recovery(recovery),
          m_child_recovery(child_recovery),
          m_kind(kind),
          m_array_entries_are_offsets(array_entries_are_offsets) {}

    static Node scalar(const BYTE* base, Size size, uint32_t version, Offset scalar_offset, FF_FieldKind kind) {
        Node node;
        node.m_base = base;
        node.m_size = size;
        node.m_version = version;
        node.m_scalar_offset = scalar_offset;
        node.m_kind = kind;
        return node;
    }

    explicit operator bool() const { return m_base != nullptr && (m_offset != FF_NULL_OFFSET || m_scalar_offset != FF_NULL_OFFSET); }
    bool is_array() const { return m_kind == FF_FIELD_ARRAY; }
    bool is_object() const { return m_kind == FF_FIELD_BLOCK; }
    bool is_string() const { return m_kind == FF_FIELD_STRING; }
    bool is_scalar() const { return m_scalar_offset != FF_NULL_OFFSET; }

    FF_FieldKind kind() const { return m_kind; }
    uint16_t recovery() const { return m_recovery; }

    std::vector<FF_FieldInfo> fields() const;
    std::vector<std::string_view> keys() const;
    size_t size() const;
    std::vector<Node> entries() const;

    Node operator[](std::string_view key) const;
    Node operator[](FF_FieldKey key) const;
    Node operator[](size_t index) const;

    std::string_view as_string() const;
    bool as_bool() const;
    uint32_t as_uint32() const;
    double as_float64() const;
};

class Parser {
    const BYTE* m_base;
    Size        m_size;
    uint32_t    m_version;
    Offset      m_root_offset;
    uint16_t    m_root_recovery;

public:
    /**
     * @brief Maps a raw byte buffer to the FastFHIR architecture.
     * Throws a std::runtime_error if the header is corrupt or truncated.
     */
    Parser(const void* buffer, size_t size);

    // --- Stream Metadata ---
    uint32_t version() const { return m_version; }
    uint16_t root_type() const { return m_root_recovery; }
    Node root() const;

    // --- Validation ---
    /**
     * @brief Extracts the expected 32-byte hash from the footer.
     * The application should compute its own hash from byte 0 to (size - 48)
     * and memcmp() it against this view to guarantee data integrity.
     */
    std::string_view expected_checksum() const;

    // --- Zero-Copy Navigation ---
    /**
     * @brief Returns a lightweight, zero-copy handle to the root resource.
     * Performs simple offset/recovery validation only.
     * Example: auto handle = parser.view_root<FF_OBSERVATION>();
     */
    template<typename T_Block>
    T_Block view_root() const {
        if (T_Block::recovery != m_root_recovery) {
            throw std::runtime_error("FastFHIR: Root type mismatch requested.");
        }
        T_Block root(m_root_offset, m_size, m_version);
        auto res = root.validate_offset(m_base, T_Block::type, T_Block::recovery);
        if (!res) throw std::runtime_error("FastFHIR Root Validation: " + res.message);

        return root;
    }

    /**
     * @brief Returns the root resource after performing full recursive validation.
     * Example: auto handle = parser.view_root_full<FF_OBSERVATION>();
     */
    template<typename T_Block>
    T_Block view_root_full() const {
        T_Block root = view_root<T_Block>();
        auto res = root.validate_full(m_base);
        if (!res) throw std::runtime_error("FastFHIR Root Full Validation: " + res.message);

        return root;
    }

    // --- Full Deserialization ---
    /**
     * @brief Allocates and reads the entire resource tree into memory.
     * Example: ObservationData data = parser.read_root<FF_OBSERVATION>();
     */
    template<typename T_Block>
    auto read_root() const {
        return view_root<T_Block>().read(m_base);
    }

    /**
     * @brief Fully validates the root resource, then reads the entire resource tree into memory.
     * Example: ObservationData data = parser.read_root_full<FF_OBSERVATION>();
     */
    template<typename T_Block>
    auto read_root_full() const {
        return view_root_full<T_Block>().read(m_base);
    }
};

} // namespace FastFHIR