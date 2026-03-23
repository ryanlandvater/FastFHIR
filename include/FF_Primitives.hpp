/**
 * @file FF_Primitives.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @version 0.1
 * @brief FastFHIR Core Primitives and Data Structures
    * @license FastFHIR Shared Source License (FF-SSL) — see LICENSE file in the project root for terms.
 * 
 * This header defines the core data structures and primitives for the FastFHIR format, including:
 * - FF_HEADER: The main file header containing metadata, checksum, and root resource information.
 * - FF_ARRAY: A zero-copy array block for efficient storage of homogeneous entries.
 * - FF_STRING: A zero-copy string block for efficient storage of string data.
 * - FF_RESOURCE: A generic wrapper for FHIR resources, allowing for flexible payload storage.
 * 
 * Each structure includes validation methods to ensure data integrity and recovery tags for error handling.
 * The primitives are designed for high performance and low overhead, enabling zero-copy parsing 
 * and efficient serialization of FHIR resources in the FastFHIR format.
 * 
 */

// MARK: - FastFHIR Core Primitives
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <limits>

#ifndef FF_EXPORT
#define FF_EXPORT
#endif

// =====================================================================
// CORE TYPES & CONSTANTS
// =====================================================================
typedef uint8_t  BYTE;
typedef uint64_t Offset;
typedef uint64_t Size;

// Standard Integer MAX Nulls
constexpr uint8_t   FF_NULL_UINT8  = 0xFF;
constexpr uint16_t  FF_NULL_UINT16 = 0xFFFF;
constexpr uint32_t  FF_NULL_UINT32 = 0xFFFFFFFF;
constexpr uint64_t  FF_NULL_UINT64 = 0xFFFFFFFFFFFFFFFF;

// Code Null (Safely traps 0xFFFFFFFF before custom string masking)
constexpr uint32_t  FF_CODE_NULL   = FF_NULL_UINT32;

// Float Nulls (Using max to adhere to the rule, though NaN is also an option)
constexpr float     FF_NULL_F32    = FF_NULL_UINT32;
constexpr double    FF_NULL_F64    = FF_NULL_UINT64;
constexpr Offset    FF_NULL_OFFSET = FF_NULL_UINT64;
constexpr uint32_t  FF_CUSTOM_STRING_FLAG = 0x80000000;

// FastFHIR magic bytes: "FFHR" in little-endian
constexpr uint32_t FF_MAGIC_BYTES = 0x52484646;

#define FHIR_VERSION_R4 0x0400
#define FHIR_VERSION_R5 0x0500

// =====================================================================
// RESULT TYPE
// =====================================================================
enum FF_Result_Code : uint32_t {
    FF_SUCCESS = 0,
    FF_FAILURE = 1,
    FF_VALIDATION_FAILURE = 2,
    FF_WARNING = 4
};

struct FF_Result {
    FF_Result_Code code;
    std::string message;
    FF_Result(FF_Result_Code c, std::string msg) : code(c), message(std::move(msg)) {}
    FF_Result(FF_Result_Code c) : code(c), message("") {}
    inline operator bool() const { return code == FF_SUCCESS; }
    inline bool operator==(FF_Result_Code c) const { return code == c; }
    inline bool operator!=(FF_Result_Code c) const { return code != c; }
};

// =====================================================================
// RECOVERY TAG REGISTRY (auto-generated from FHIR StructureDefinitions)
// =====================================================================
#include "../generated_src/FF_Recovery.hpp"

// =====================================================================
// TYPE SIZE CONSTANTS
// =====================================================================
enum TYPE_SIZE : uint8_t {
    TYPE_SIZE_UINT8     = 1,
    TYPE_SIZE_UINT16    = 2,
    TYPE_SIZE_UINT24    = 3,
    TYPE_SIZE_UINT32    = 4,
    TYPE_SIZE_INT32     = 4,
    TYPE_SIZE_UINT64    = 8,
    TYPE_SIZE_FLOAT32   = 4,
    TYPE_SIZE_FLOAT64   = 8,
    TYPE_SIZE_OFFSET    = 8,
};

// =====================================================================
// SUPPORTED CHECKSUM ALGORITHMS
// =====================================================================
enum FF_Checksum_Algorithm : uint16_t {
    FF_CHECKSUM_NONE   = 0,
    FF_CHECKSUM_CRC32  = 1, // 4 bytes  (32 bits)
    FF_CHECKSUM_MD5    = 2, // 16 bytes (128 bits)
    FF_CHECKSUM_SHA256 = 3, // 32 bytes (256 bits)
};

// Define the maximum inline hash size (256 bits = 32 bytes)
constexpr uint32_t FF_MAX_HASH_BYTES = 32;

// =====================================================================
// FORWARD DECLARATIONS FOR DATA TYPES
// =====================================================================
enum FF_FieldKind : uint16_t {
    FF_FIELD_UNKNOWN = 0,
    FF_FIELD_STRING = 1,
    FF_FIELD_ARRAY = 2,
    FF_FIELD_BLOCK = 3,
    FF_FIELD_CODE = 4,
    FF_FIELD_BOOL = 5,
    FF_FIELD_UINT32 = 6,
    FF_FIELD_FLOAT64 = 7,
};

struct FF_FieldInfo {
    const char* name = nullptr;
    FF_FieldKind kind = FF_FIELD_UNKNOWN;
    uint16_t field_offset = 0;
    RECOVERY_TAG child_recovery = FF_RECOVER_UNDEFINED;
    uint8_t array_entries_are_offsets = 0;
};

struct FF_FieldKey {
    RECOVERY_TAG owner_recovery = FF_RECOVER_UNDEFINED;
    FF_FieldKind kind = FF_FIELD_UNKNOWN;
    uint16_t field_offset = 0;
    RECOVERY_TAG child_recovery = FF_RECOVER_UNDEFINED;
    uint8_t array_entries_are_offsets = 0;
    const char* name = nullptr;
    std::size_t name_len = 0;

    constexpr FF_FieldKey() = default;

    template <std::size_t N>
    constexpr FF_FieldKey(const char (&literal)[N]) noexcept : name(literal), name_len(N - 1) {}
    
    template <std::size_t N>
    constexpr FF_FieldKey(RECOVERY_TAG owner,
                          FF_FieldKind field_kind,
                          uint16_t offset,
                          RECOVERY_TAG child,
                          uint8_t array_offsets,
                          const char (&field_name)[N]) noexcept
    : owner_recovery(owner),
    kind(field_kind),
    field_offset(offset),
    child_recovery(child),
    array_entries_are_offsets(array_offsets),
    name(field_name),
    name_len(N - 1) {}
    
    constexpr FF_FieldKey(RECOVERY_TAG owner,
                          FF_FieldKind field_kind,
                          uint16_t offset,
                          RECOVERY_TAG child,
                          uint8_t array_offsets,
                          const char* field_name,
                          std::size_t field_name_len) noexcept
    : owner_recovery(owner),
    kind(field_kind),
    field_offset(offset),
    child_recovery(child),
    array_entries_are_offsets(array_offsets),
    name(field_name),
    name_len(field_name_len) {}
    
    constexpr FF_FieldKey(const char* key_name, std::size_t key_name_len) noexcept
    : name(key_name),
    name_len(key_name_len) {}
    
    static FF_FieldKey from_cstr(const char* key_name) noexcept {
        return FF_FieldKey(key_name, key_name ? std::char_traits<char>::length(key_name) : 0);
    }
    
    static FF_FieldKey from_cstr(RECOVERY_TAG owner,
                                 FF_FieldKind field_kind,
                                 uint16_t offset,
                                 RECOVERY_TAG child,
                                 uint8_t array_offsets,
                                 const char* field_name) noexcept {
        return FF_FieldKey(owner,
                           field_kind,
                           offset,
                           child,
                           array_offsets,
                           field_name,
                           field_name ? std::char_traits<char>::length(field_name) : 0);
    }

    constexpr std::string_view view() const noexcept {
        return (name && name_len > 0) ? std::string_view{name, name_len} : std::string_view{};
    }
    
    constexpr operator std::string_view() const noexcept { return view(); }
};

struct DATA_BLOCK;  // Base structure for all data blocks
struct FF_HEADER;   // Stream header block
struct FF_CHECKSUM; // Checksum block
struct FF_ARRAY;    // Array template block
struct FF_STRING;   // String block
struct FF_RESOURCE; // Resource template block

// =====================================================================
// BASE DATA BLOCK
// =====================================================================
struct FF_EXPORT DATA_BLOCK {
    enum vtable_sizes {
        VALIDATION_S    = TYPE_SIZE_UINT64,
        RECOVERY_S      = TYPE_SIZE_UINT16,
    };
    enum vtable_offsets {
        VALIDATION      = 0,
        RECOVERY        = VALIDATION + VALIDATION_S,
        HEADER_SIZE     = RECOVERY + RECOVERY_S,
    };

#ifdef __EMSCRIPTEN__
    Offset      __remote   = FF_NULL_OFFSET;
    void*       __response = nullptr;
#endif

    Offset      __offset   = FF_NULL_OFFSET;
    Size        __size     = 0;
    uint32_t    __version  = 0;

    explicit DATA_BLOCK() = default;
    explicit DATA_BLOCK(Offset off, Size total_size, uint32_t ver) 
        : __offset(off), __size(total_size), __version(ver) {}

    operator bool() const { return __offset != FF_NULL_OFFSET; }

    FF_Result validate_offset(const BYTE* const __base, const char* type_name, uint16_t recovery_tag) const noexcept;
    
#ifdef __EMSCRIPTEN__
    void check_and_fetch_remote(const BYTE* const& __base);
#endif
};

// =====================================================================
// HEADER
// =====================================================================
struct FF_EXPORT FF_HEADER : DATA_BLOCK {
    static constexpr char type [] = "FF_HEADER";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_HEADER;
    enum vtable_sizes {
        MAGIC_S             = TYPE_SIZE_UINT32,
        RECOVERY_S          = TYPE_SIZE_UINT16,
        VERSION_S           = TYPE_SIZE_UINT32,
        CHECKSUM_OFFSET_S   = TYPE_SIZE_UINT64,
        ROOT_OFFSET_S       = TYPE_SIZE_UINT64,
        ROOT_RECOVERY_S     = TYPE_SIZE_UINT16,
        PAYLOAD_SIZE_S      = TYPE_SIZE_UINT64,
    };
    enum vtable_offsets {
        MAGIC           = 0,
        RECOVERY        = MAGIC         + MAGIC_S,
        VERSION         = RECOVERY      + RECOVERY_S,
        CHECKSUM_OFFSET = VERSION       + VERSION_S,
        ROOT_OFFSET     = CHECKSUM_OFFSET + CHECKSUM_OFFSET_S,
        ROOT_RECOVERY   = ROOT_OFFSET   + ROOT_OFFSET_S,
        PAYLOAD_SIZE    = ROOT_RECOVERY + ROOT_RECOVERY_S,
        HEADER_SIZE     = PAYLOAD_SIZE + PAYLOAD_SIZE_S,
    };

    explicit FF_HEADER(Size file_size) noexcept;

    FF_Result       validate_full(const BYTE* const __base) const noexcept;
    uint32_t        get_version  (const BYTE* const __base) const;
    FF_CHECKSUM     get_checksum (const BYTE* const __base) const;
    Offset          get_root     (const BYTE* const __base) const;
    RECOVERY_TAG    get_root_type(const BYTE* const __base) const;
};

void FF_EXPORT STORE_FF_HEADER(BYTE* const __base, uint32_t version,
                               Offset checksum_offset, Offset root_offset,
                               RECOVERY_TAG root_recovery, Size payload_size);



// =====================================================================
// FIXED-SIZE CHECKSUM FOOTER
// =====================================================================
struct FF_EXPORT FF_CHECKSUM : DATA_BLOCK {
    static constexpr char type [] = "FF_CHECKSUM";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_CHECKSUM;
    
    enum vtable_sizes {
        VALIDATION_S    = TYPE_SIZE_UINT64,
        RECOVERY_S      = TYPE_SIZE_UINT16,
        ALGORITHM_S     = TYPE_SIZE_UINT16,
        PADDING_S       = 4,
        HASH_DATA_S     = FF_MAX_HASH_BYTES, // 256 bits
    };
    enum vtable_offsets {
        VALIDATION      = 0,
        RECOVERY        = VALIDATION + VALIDATION_S,
        ALGORITHM       = RECOVERY   + RECOVERY_S,
        PADDING         = ALGORITHM  + ALGORITHM_S,
        HASH_DATA       = PADDING    + PADDING_S,
        
        // Exact predictable size: 48 bytes
        HEADER_SIZE     = HASH_DATA  + HASH_DATA_S,
    };

    explicit FF_CHECKSUM(Offset off, Size size, uint32_t ver) : DATA_BLOCK(off, size, ver) {}
    
    FF_Result validate_full(const BYTE* const __base) const noexcept;
    FF_Checksum_Algorithm get_algorithm(const BYTE* const __base) const;
    std::string_view get_hash_view(const BYTE* const __base) const;
};

// Allocates the block, writes the metadata, and returns a pointer to the 32-byte hash buffer
BYTE* FF_EXPORT STORE_FF_CHECKSUM_METADATA(BYTE* const __base, Offset start_offset, FF_Checksum_Algorithm algo);

// =====================================================================
// ZERO-COPY ARRAY BLOCK
// =====================================================================
struct FF_EXPORT FF_ARRAY : DATA_BLOCK {
    static constexpr char type [] = "FF_ARRAY";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_ARRAY;
    enum vtable_sizes {
        VALIDATION_S    = TYPE_SIZE_UINT64,
        RECOVERY_S      = TYPE_SIZE_UINT16,
        ENTRY_STEP_S    = TYPE_SIZE_UINT16,
        ENTRY_COUNT_S   = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets {
        VALIDATION      = 0,
        RECOVERY        = VALIDATION  + VALIDATION_S,
        ENTRY_STEP      = RECOVERY    + RECOVERY_S,
        ENTRY_COUNT     = ENTRY_STEP  + ENTRY_STEP_S,
        HEADER_SIZE     = ENTRY_COUNT + ENTRY_COUNT_S,
    };

    explicit FF_ARRAY(Offset off, Size size, uint32_t ver) : DATA_BLOCK(off, size, ver) {}

    FF_Result validate_full(const BYTE* const __base) const noexcept;
    uint16_t  entry_step (const BYTE* const __base) const;
    uint32_t  entry_count(const BYTE* const __base) const;
    const BYTE* entries  (const BYTE* const __base) const;
};

void FF_EXPORT STORE_FF_ARRAY_HEADER(BYTE* const __base, Offset& write_head,
                                      uint16_t entry_step, uint32_t entry_count);

// =====================================================================
// ZERO-COPY STRING BLOCK
// =====================================================================
struct FF_EXPORT FF_STRING : DATA_BLOCK {
    static constexpr char type [] = "FF_STRING";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_STRING;
    enum vtable_sizes {
        VALIDATION_S    = TYPE_SIZE_UINT64,
        RECOVERY_S      = TYPE_SIZE_UINT16,
        LENGTH_S        = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets {
        VALIDATION      = 0,
        RECOVERY        = VALIDATION + VALIDATION_S,
        LENGTH          = RECOVERY   + RECOVERY_S,
        STRING_DATA     = LENGTH     + LENGTH_S,
        HEADER_SIZE     = STRING_DATA,
    };

    explicit FF_STRING(Offset off, Size size, uint32_t ver) : DATA_BLOCK(off, size, ver) {}
    
    FF_Result validate_full(const BYTE* const __base) const noexcept;
    
    // Zero-Copy Mapped View
    std::string_view read_view(const BYTE* const __base) const;
    
    // Fallback std::string allocation for dictionary parsers
    std::string read (const BYTE* const __base) const;
};

// =====================================================================
// GENERIC RESOURCE WRAPPER
// =====================================================================
struct ResourceData {
    uint16_t payloadRecovery = RECOVER_FF_STRING;
    std::string resourceType;
    std::string json;
};

struct FF_EXPORT FF_RESOURCE : DATA_BLOCK {
    static constexpr char type [] = "FF_RESOURCE";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_RESOURCE;
    enum vtable_sizes {
        VALIDATION_S      = TYPE_SIZE_UINT64,
        RECOVERY_S        = TYPE_SIZE_UINT16,
        PAYLOAD_RECOVERY_S= TYPE_SIZE_UINT16,
        PAYLOAD_OFFSET_S  = TYPE_SIZE_UINT64,
    };
    enum vtable_offsets {
        VALIDATION        = 0,
        RECOVERY          = VALIDATION + VALIDATION_S,
        PAYLOAD_RECOVERY  = RECOVERY + RECOVERY_S,
        PAYLOAD_OFFSET    = PAYLOAD_RECOVERY + PAYLOAD_RECOVERY_S,
        HEADER_SIZE       = PAYLOAD_OFFSET + PAYLOAD_OFFSET_S,
    };

    explicit FF_RESOURCE(Offset off, Size size, uint32_t ver) : DATA_BLOCK(off, size, ver) {}

    FF_Result    validate_full(const BYTE* const __base) const noexcept;
    ResourceData read         (const BYTE* const __base) const;
};

void STORE_FF_RESOURCE(BYTE* const __base, Offset entry_off, Offset& write_head, const ResourceData& data);

// =====================================================================
// LOCK-FREE EMITTER SIGNATURES
// =====================================================================
Size     SIZE_FF_STRING(std::string_view str);
Size     SIZE_FF_CODE(std::string_view code_str, uint32_t version);
Size     STORE_FF_STRING (BYTE* const __base, Offset start_offset, std::string_view str);
uint32_t ENCODE_FF_CODE  (BYTE* const __base, Offset block_offset, Offset& child_off, const std::string& code_str, uint32_t version = FHIR_VERSION_R5);
