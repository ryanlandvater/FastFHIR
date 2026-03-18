/**
 * @file FF_Parser.hpp
 * @author Ryan Landvater (ryan.landvater@example.com)
 * @copyright Copyright (c) 2026
 * @version 0.1
 * 
 * @brief FastFHIR File Parser — Internal Declarations
 * 
 * Prefer including FastFHIR.hpp instead of this header directly.
 * 
 * Parser and Node are lightweight, shared handles (internally reference-counted).
 * Copying is cheap: all copies share the same underlying state. Child nodes are
 * cached on first lookup to avoid redundant validation.
 * 
 */

 #pragma once

#include "FF_Primitives.hpp"
#include <memory>

namespace FastFHIR {

class Node {
    struct Data;
    std::shared_ptr<Data> m_data;

    explicit Node(std::shared_ptr<Data> data);

public:
    Node() = default;
    Node(const BYTE* base, Size size, uint32_t version, Offset offset,
         uint16_t recovery, FF_FieldKind kind,
         uint16_t child_recovery = 0, bool array_entries_are_offsets = false);

    static Node scalar(const BYTE* base, Size size, uint32_t version,
                       Offset scalar_offset, FF_FieldKind kind);

    explicit operator bool() const;
    bool is_array() const;
    bool is_object() const;
    bool is_string() const;
    bool is_scalar() const;

    FF_FieldKind kind() const;
    uint16_t recovery() const;

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
    struct Data;
    std::shared_ptr<Data> m_data;

    explicit Parser(std::shared_ptr<Data> data);

public:
    /**
     * @brief Creates a Parser from a raw byte buffer.
     * Throws a std::runtime_error if the header is corrupt or truncated.
     */
    static Parser create(const void* buffer, size_t size);

    explicit operator bool() const;
    uint32_t version() const;
    uint16_t root_type() const;

    /**
     * @brief Returns the root node of the parsed FHIR stream.
     * 
     * @return Node 
     */
    Node root() const;

    /**
     * @brief Represents the context for checksum validation.
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
        const void*           first_byte;
        size_t                total_bytes;
        FF_Checksum_Algorithm algorithm;
        std::string_view      expected_checksum;
        explicit operator bool() const { return !expected_checksum.empty(); }
    };

    /**
     * @brief Returns the checksum validation context.
     * 
     * If the file contains a checksum block, this method will return a ChecksumValidation struct
     * populated with the relevant information for performing checksum validation. If no checksum
     * block is present, it will return an empty ChecksumValidation 
     * including nullptr for first_byte and 0 total_bytes, and with expected_checksum empty.
     */
    ChecksumValidation checksum() const;
};

} // namespace FastFHIR