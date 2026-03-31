/**
 * @file FF_Parser.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file in the project root for terms.
 * @version 0.1
 * 
 * @brief FastFHIR File Parser — Internal Declarations
 * 
 * Prefer including FastFHIR.hpp instead of this header directly.
 * 
 * Parser and Node are lightweight value types with no shared ownership overhead.
 * 
 */

 #pragma once

#include "FF_Primitives.hpp"
#include "FF_Memory.hpp"

namespace FastFHIR {
template<typename T> struct TypeTraits;
class Node;
/**
 * @class Parser
 * @brief Entry point for reading a FastFHIR byte stream.
 *
 * `Parser` validates file structure at creation time and exposes read-only
 * accessors for header metadata, checksum metadata, and the root node.
 */
class Parser {
    const Memory m_memory;
    const BYTE*     m_base = nullptr;
    Size            m_size = 0;
    uint32_t        m_version = 0;
    Offset          m_root_offset = FF_NULL_OFFSET;
    RECOVERY_TAG    m_root_recovery = FF_RECOVER_UNDEFINED;

public:
    /**
    * @brief Create a parser from a raw in-memory byte buffer.
    * @param buffer Pointer to the beginning of a FastFHIR file in memory.
    * @param size Total number of bytes available at @p buffer.
    * @return A valid parser bound to the supplied buffer.
    * @throws std::runtime_error If the header is truncated or fails validation.
     */
    explicit Parser (const void* buffer, size_t size);

    /**
    * @brief Create a parser directly from an FF_Memory's memory mapping.
     * 
    * @param memory Shared pointer to an initialized FF_Memory providing the memory mapping for the parser.
     * @return A valid parser bound to the supplied allocator.
     * @throws std::runtime_error If the header is truncated or fails validation.
     */
    explicit Parser (const Memory& memory);

    /** 
    * @brief Check whether this parser instance references a parsed buffer.
    * @return `true` when the parser holds a valid underlying buffer; otherwise `false`.
     */
    explicit operator bool() const { return m_base != nullptr; }

    /**
    * @brief Get the encoded FastFHIR/FHIR version from the file header.
    * @return Version value stored in the header.
     */
    uint32_t version() const;
    
    /** 
    * @brief Get the root resource recovery/type tag from the header.
    * @return Recovery/type tag for the root resource.
     */
    uint16_t root_type() const;

    /**
    * @brief Get the root resource node.
    * @return Root node of the parsed FastFHIR stream.
     */
    Node root() const;

    /**
     * @brief Stream the entire FastFHIR file back out as minified FHIR JSON.
     * @param out The output stream (e.g., std::cout, std::ofstream).
     */
    void print_json(std::ostream& out) const;

    /**
    * @struct ChecksumValidation
    * @brief Checksum metadata extracted from the file.
     * 
     * Provides the necessary information to perform checksum validation, including:
     * - A pointer to the first byte of the payload to be checksummed.
     * - The total number of bytes to include in the checksum calculation.
     * - The checksum algorithm used (e.g., CRC32, MD5, SHA256).
     * - The expected checksum value as stored in the file's footer.
     * 
     * Checksum validation is performed outside of the parser to allow for flexibility
     *  in how and when it is executed, and to avoid coupling the parser to specific 
     *  checksum implementations.
     */
    struct ChecksumValidation {
        /// Pointer to first byte included in the checksum span.
        const void*           first_byte;
        /// Number of bytes to include starting at @ref first_byte.
        size_t                total_bytes;
        /// Hash/checksum algorithm declared by the file.
        FF_Checksum_Algorithm algorithm;
        /// Expected digest/hash value stored in the file footer.
        std::string_view      expected_checksum;
        /// Convenience validity check; true when an expected checksum is present.
        explicit operator bool() const { return !expected_checksum.empty(); }
    };

    /**
     * @brief Get checksum metadata for external checksum verification.
     * @return A populated @ref ChecksumValidation when present, otherwise an empty one
     *         (`first_byte == nullptr`, `total_bytes == 0`, and empty checksum text).
     */
    ChecksumValidation checksum() const;
};

/**
 * @class Node
 * @brief Lightweight view over a FastFHIR value.
 * 
 * The Node class represents a node in the parsed FastFHIR data structure. It can represent
 * various types of data, including objects (blocks), arrays, strings, and scalar values.
 * 
 * Nodes are lightweight, non-owning value objects. The class provides methods for
 * accessing child nodes by key or index,
 * as well as methods for retrieving the node's value in the appropriate type (e.g., string, bool, uint32_t, double).
 * 
 */
class Node {
    const BYTE*   m_base = nullptr;
    Size          m_size = 0;
    uint32_t      m_version = 0;
    Offset        m_node_offset = FF_NULL_OFFSET;
    Offset        m_global_scalar_offset = FF_NULL_OFFSET;
    RECOVERY_TAG  m_recovery = FF_RECOVER_UNDEFINED;
    RECOVERY_TAG  m_child_recovery = FF_RECOVER_UNDEFINED;
    FF_FieldKind  m_kind = FF_FIELD_UNKNOWN;
    bool          m_array_entries_are_offsets = false;

public:
    /** @brief Construct an empty/invalid node handle. */
    Node() = default;
    /**
    * @brief Construct a node backed by a structured (offset-based) value.
    */
    Node(const BYTE* base, Size size, uint32_t version, Offset offset,
         RECOVERY_TAG recovery, FF_FieldKind kind,
         RECOVERY_TAG child_recovery = FF_RECOVER_UNDEFINED, bool array_entries_are_offsets = false);

    /** @brief Check whether this node contains a value */
    bool is_empty() const;

    /**
    * @brief Construct a node backed by an inline scalar value.
    */
    static Node scalar(const BYTE* base, Size size, uint32_t version,
                       Offset parent_offset, Offset scalar_offset, FF_FieldKind kind);

    /**
     * @brief Check whether this node references a valid underlying value.
     * @return `true` if this node is bound to valid data; otherwise `false`.
     */
    explicit operator bool() const;

    /** @brief Check whether this node is an array value. */
    bool is_array() const;
    /** @brief Check whether this node is an object/block value. */
    bool is_object() const;
    /** @brief Check whether this node is a string value. */
    bool is_string() const;
    /** @brief Check whether this node is an inline scalar value. */
    bool is_scalar() const;

    /**
     * @brief Get the node field kind.
     * @return Encoded FastFHIR field kind for this node.
     */
    FF_FieldKind kind() const;

    /**
     * @brief Get the node recovery/type tag.
     * @return Recovery/type tag associated with this node.
     */
    RECOVERY_TAG recovery() const;

    /**
     * @brief Enumerate reflected field metadata for object nodes.
     * @return Field metadata list for object nodes; empty for non-object nodes.
     */
    std::vector<FF_FieldInfo> fields() const;

    /**
     * @brief Enumerate reflected key names for object nodes.
     * @return Key list for object nodes; empty for non-object nodes.
     */
    std::vector<std::string_view> keys() const;

    /**
     * @brief Get the number of direct children.
     * @return Entry count for arrays, key count for objects, otherwise `0`.
     */
    size_t size() const;

    /**
     * @brief Materialize all direct array entries.
     * @return Child nodes for arrays; empty for non-array nodes.
     */
    std::vector<Node> entries() const;

    /**
     * @brief Lookup an object child by precomputed field key descriptor.
     * @param key Field key descriptor produced from generated field metadata.
     * @return Matching child node, or an empty node if incompatible/not found.
     */
    Node operator[](FF_FieldKey key) const;

    /**
     * @brief Lookup an array child by index.
     * @param index Zero-based entry index.
     * @return Matching child node, or an empty node if out of bounds/not an array.
     */
    Node operator[](size_t index) const;
    
    /**
     * @brief Eagerly parses this node into its concrete C++ POD structure.
     * @tparam T_Data The generated FastFHIR Data struct (e.g., PatientData).
     * @throws std::invalid_argument if the node's recovery tag doesn't match the requested type.
     */
    template <typename T_Data>
    T_Data as() const {
        if (!*this || is_empty()) return T_Data{};

        // Strict schema validation!
        if (m_recovery != TypeTraits<T_Data>::recovery) {
            throw std::invalid_argument("FastFHIR Schema Violation: Cannot cast Node to requested type.");
        }

        return TypeTraits<T_Data>::read(m_base, m_node_offset, m_size, m_version);
    }
    
    /**
     * @brief Recursively print this node and all children as minified FHIR JSON.
     * @param out The output stream.
     */
    void print_json(std::ostream& out) const;
};

// =====================================================================
// Explicit Node Specializations for Primitives
// =====================================================================
template <> std::string_view Node::as<std::string_view>() const;
template <> bool Node::as<bool>() const;
template <> uint32_t Node::as<uint32_t>() const;
template <> double Node::as<double>() const;


} // namespace FastFHIR
