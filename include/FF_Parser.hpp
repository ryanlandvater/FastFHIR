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

#include <concepts>
#include <iosfwd>
#include <type_traits>

#include "FF_Dictionary.hpp"
#include "FF_Memory.hpp"
#include "FF_Ops.hpp"
#include "FF_Primitives.hpp"

namespace FastFHIR {
class Builder;
template<typename T> struct TypeTraits;

/// Concept: true for any T that has a FastFHIR::TypeTraits<T> specialization with a read() method.
template <typename T>
concept HasTypeTraits = requires(const BYTE* base, Offset off, Size sz, uint32_t ver) {
    { FastFHIR::TypeTraits<T>::read(base, off, sz, ver) } -> std::convertible_to<T>;
};

namespace Reflective {
struct ParserOps;
class Node;
struct Entry;
}

/**
 * @class Parser
 * @brief Entry point for reading a FastFHIR byte stream.
 *
 * `Parser` validates file structure at creation time and exposes read-only
 * accessors for header metadata, checksum metadata, and the root node.
 */
class Parser {
    friend class Builder;
    const Memory m_memory;
    const BYTE*     m_base = nullptr;
    Size            m_size = 0;
    uint32_t        m_version = 0;
    FF_StreamLayout m_stream_layout = FF_STREAM_LAYOUT_STANDARD;
    const Reflective::ParserOps* m_ops = nullptr;
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

    /** @brief Pointer to the underlying stream bytes. */
    const BYTE* data() const { return m_base; }

    /** @brief Total stream size in bytes. */
    Size size_bytes() const { return m_size; }

    /**
    * @brief Get the stream layout mode detected from header metadata.
    * @return Standard or compact stream layout mode.
     */
    FF_StreamLayout stream_layout() const { return m_stream_layout; }
    
    /** 
    * @brief Get the root resource recovery/type tag from the header.
    * @return Recovery/type tag for the root resource.
     */
    uint16_t root_type() const;

    /**
    * @brief Get the root resource node.
    * @return Root node of the parsed FastFHIR stream.
     */
    Reflective::Node root() const;

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

namespace Reflective {
// Vtable slot coordinate — represents one field slot within a parent block.
struct Entry {
    const BYTE*  base            = nullptr;
    Offset       parent_offset   = FF_NULL_OFFSET;
    uint32_t     vtable_offset   = 0;
    RECOVERY_TAG target_recovery = FF_RECOVER_UNDEFINED;
    FF_FieldKind kind            = FF_FIELD_UNKNOWN;
    Size         m_size          = 0;
    uint32_t     m_version       = 0;
    const ParserOps* m_ops       = nullptr;

    Entry() = default;
    // 5-arg constructor — backward-compatible; size/version remain zero.
    Entry(const BYTE* b, Offset parent, uint32_t vtable, RECOVERY_TAG recovery, FF_FieldKind field_kind)
        : base(b), parent_offset(parent), vtable_offset(vtable), target_recovery(recovery), kind(field_kind) {}
    // 7-arg constructor — bakes in size/version for self-contained chaining.
    Entry(const BYTE* b, Offset parent, uint32_t vtable, RECOVERY_TAG recovery, FF_FieldKind field_kind,
            Size size, uint32_t version, const ParserOps* ops = nullptr)
        : base(b), parent_offset(parent), vtable_offset(vtable), target_recovery(recovery), kind(field_kind),
            m_size(size), m_version(version), m_ops(ops) {}

    Offset absolute_offset() const {
        if (parent_offset == FF_NULL_OFFSET) return FF_NULL_OFFSET;
        return parent_offset + static_cast<Offset>(vtable_offset);
    }

    // Validity check — true when this vtable slot is non-null.
    explicit operator bool() const {
        return base != nullptr && parent_offset != FF_NULL_OFFSET;
    }

    // Read a fixed-width scalar value at this vtable slot.
    template <typename T>
    requires std::is_arithmetic_v<T>
    T as_scalar(RECOVERY_TAG expected_tag) const {
        return Decode::scalar<T>(base, absolute_offset(), expected_tag);
    }

    // Serialize an inline scalar (bool, int, uint, float, code) directly to JSON.
    void print_scalar_json(std::ostream& out, uint32_t version) const;

    // Produce a Node over a DATA_BLOCK field (block, array, string, choice, resource).
    // 4-arg form used by generated code and Python bindings.
    Node as_node(Size size, uint32_t version, RECOVERY_TAG tag, FF_FieldKind field_kind,
                 const ParserOps* ops = nullptr) const;
    // 0-arg form — uses stored m_size/m_version; requires the 7-arg constructor.
    Node as_node() const;

    // ── Implicit conversions — bodies defined after Node ──────────────────────
    // String and code reads require a Node expansion (as_node() pointer-hop).
    operator std::string_view() const;
    // TypeTraits-backed struct reads (PatientData, BundleData, …).
    template <HasTypeTraits T>
    operator T() const;
    // Pure in-place scalar reads — no Node expansion needed.
    operator int32_t()  const { return as_scalar<int32_t> (RECOVER_FF_INT32);   }
    operator uint32_t() const { return as_scalar<uint32_t>(RECOVER_FF_UINT32);  }
    operator int64_t()  const { return as_scalar<int64_t> (RECOVER_FF_INT64);   }
    operator uint64_t() const { return as_scalar<uint64_t>(RECOVER_FF_UINT64);  }
    operator double()   const { return as_scalar<double>  (RECOVER_FF_FLOAT64); }
    // Explicit escape hatch — handles bool and auto/template contexts.
    // Body defined after Node.
    template <typename T>
    T as() const;

    // ── Chained traversal — bodies defined after Node ─────────────────────────
    Entry             operator[](FF_FieldKey key) const;
    Node              operator[](size_t index)    const;
    std::vector<Node> entries()                   const;
    bool   is_array()  const;
    bool   is_object() const;
    bool   is_string() const;
    bool   is_scalar() const;
    size_t size()      const;
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
protected:
    friend struct ParserOps;
    const BYTE* m_base = nullptr;
    Offset m_node_offset = FF_NULL_OFFSET;
    Size m_size = 0;
    uint32_t m_version = 0;
    RECOVERY_TAG m_recovery = FF_RECOVER_UNDEFINED;
    RECOVERY_TAG m_child_recovery = FF_RECOVER_UNDEFINED;
    FF_FieldKind m_kind = FF_FIELD_UNKNOWN;
    bool m_array_entries_are_offsets = false;
    const ParserOps* m_ops = nullptr;
public:
    /** @brief Construct an empty/invalid node handle. */
    Node() = default;
    /**
    * @brief Construct a node backed by a structured (offset-based) value.
    */
    Node(const BYTE* base, Size size, uint32_t version, Offset offset,
         RECOVERY_TAG recovery, FF_FieldKind kind,
            RECOVERY_TAG child_recovery = FF_RECOVER_UNDEFINED, bool array_entries_are_offsets = false,
            const ParserOps* ops = nullptr);

    /** @brief Check whether this node contains a value */
    bool is_empty() const;

    /**
     * @brief Resolve a choice node to its concrete type based on the schema kind.
     * 
     * @param base Pointer to the base of the FastFHIR data.
     * @param size Size of the FastFHIR data.
     * @param version Version of the FastFHIR data.
     * @param parent_offset Offset of the parent node.
     * @param value_offset Offset of the value within the parent node.
     * @param schema_kind The schema kind to resolve the choice against.
     * @return Node representing the resolved choice.
     */
    static Node resolve_choice(const BYTE* base, Size size, uint32_t version, 
                       Offset parent_offset, Offset value_offset, FF_FieldKind schema_kind,
                       const ParserOps* ops = nullptr);

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
     * @brief Lookup a child field by precomputed key descriptor.
     * @return An Entry containing vtable slot coordinates; implicitly converts to scalars or structs.
     */
    Entry operator[](FF_FieldKey key) const;

    /**
     * @brief Lookup an array element by index.
     * @return The resolved child Node at the given index.
     */
    Node operator[](size_t index) const;

    /**
     * @brief Materialize this block node into a typed C++ struct.
     * @tparam T A generated FastFHIR data struct with a TypeTraits specialization.
     */
    template <HasTypeTraits T>
    operator T() const { return as<T>(); }

    /**
     * @brief Explicit typed read — escape hatch for auto/template/bool contexts.
     * @tparam T Primitives, std::string_view, or a HasTypeTraits struct.
     * @throws std::runtime_error on recovery tag mismatch.
     */
    template <typename T>
    T as() const {
        if constexpr (std::is_same_v<T, std::string_view>) {
            if (m_recovery == RECOVER_FF_CODE) {
                uint32_t raw_code = LOAD_U32(m_base + m_node_offset);
                if (raw_code == FF_CODE_NULL) return "";

                if (const char* resolved = FF_ResolveCode(raw_code, m_version)) {
                    return resolved;
                }

                if (raw_code & FF_CUSTOM_STRING_FLAG) {
                    Offset relative_off = (raw_code & ~FF_CUSTOM_STRING_FLAG);
                    return FF_STRING(m_node_offset + relative_off, m_size, m_version).read_view(m_base);
                }
                return "";
            }

            if (m_recovery != RECOVER_FF_STRING) {
                throw std::runtime_error("FastFHIR: Node is not a string or code.");
            }
            return FF_STRING(m_node_offset, m_size, m_version).read_view(m_base);
        } else if constexpr (
            std::is_same_v<T, bool> ||
            std::is_same_v<T, int32_t> ||
            std::is_same_v<T, uint32_t> ||
            std::is_same_v<T, int64_t> ||
            std::is_same_v<T, uint64_t> ||
            std::is_same_v<T, double>
        ) {
            RECOVERY_TAG expected = FF_RECOVER_UNDEFINED;
            if constexpr (std::is_same_v<T, bool>) expected = RECOVER_FF_BOOL;
            else if constexpr (std::is_same_v<T, int32_t>) expected = RECOVER_FF_INT32;
            else if constexpr (std::is_same_v<T, uint32_t>) expected = RECOVER_FF_UINT32;
            else if constexpr (std::is_same_v<T, int64_t>) expected = RECOVER_FF_INT64;
            else if constexpr (std::is_same_v<T, uint64_t>) expected = RECOVER_FF_UINT64;
            else if constexpr (std::is_same_v<T, double>) expected = RECOVER_FF_FLOAT64;

            if (m_recovery != expected) {
                throw std::runtime_error("Node::as() recovery tag mismatch");
            }
            return Decode::scalar<T>(m_base, m_node_offset, m_recovery);
        } else {
            if (m_recovery != TypeTraits<T>::recovery) {
                throw std::runtime_error("Node::as() recovery tag mismatch");
            }
            return TypeTraits<T>::read(m_base, m_node_offset, m_size, m_version);
        }
    }

public:
    /**
     * @brief Recursively print this node and all children as minified FHIR JSON.
     * @param out The output stream.
     */
    void print_json(std::ostream& out) const;
};

// ── Entry methods requiring the complete Node definition ─────────────────────
inline Node Entry::as_node() const {
    return as_node(m_size, m_version, target_recovery, kind, m_ops);
}

inline Entry::operator std::string_view() const {
    if (kind == FF_FIELD_CODE) {
        uint32_t raw_code = LOAD_U32(base + absolute_offset());
        if (raw_code == FF_CODE_NULL) return "";

        if (const char* resolved = FF_ResolveCode(raw_code, m_version)) {
            return resolved;
        }

        if (raw_code & FF_CUSTOM_STRING_FLAG) {
            Offset relative_off = static_cast<Offset>(raw_code & ~FF_CUSTOM_STRING_FLAG);
            return FF_STRING(parent_offset + relative_off, m_size, m_version).read_view(base);
        }

        return "";
    }

    return as_node().as<std::string_view>();
}

template <HasTypeTraits T>
inline Entry::operator T() const { return as_node().as<T>(); }

template <typename T>
inline T Entry::as() const {
    if constexpr (std::is_same_v<T, bool>)               return as_scalar<bool>(RECOVER_FF_BOOL);
    else if constexpr (std::is_same_v<T, std::string_view>) return operator std::string_view();
    else if constexpr (std::is_same_v<T, int32_t>)       return operator int32_t();
    else if constexpr (std::is_same_v<T, uint32_t>)      return operator uint32_t();
    else if constexpr (std::is_same_v<T, int64_t>)       return operator int64_t();
    else if constexpr (std::is_same_v<T, uint64_t>)      return operator uint64_t();
    else if constexpr (std::is_same_v<T, double>)        return operator double();
    else                                                  return operator T();
}

inline Entry             Entry::operator[](FF_FieldKey key) const { return as_node()[key]; }
inline Node              Entry::operator[](size_t index)    const { return as_node()[index]; }
inline std::vector<Node> Entry::entries()                   const { return as_node().entries(); }
inline bool   Entry::is_array()  const { return as_node().is_array(); }
inline bool   Entry::is_object() const { return as_node().is_object(); }
inline bool   Entry::is_string() const { return as_node().is_string(); }
inline bool   Entry::is_scalar() const { return as_node().is_scalar(); }
inline size_t Entry::size()      const { return as_node().size(); }

} // namespace Reflective
} // namespace FastFHIR
