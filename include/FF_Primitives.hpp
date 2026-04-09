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
typedef uint8_t BYTE;
typedef uint64_t Offset;
typedef uint64_t Size;

// Standard Integer MAX Nulls
constexpr uint8_t FF_NULL_UINT8 = 0xFF;
constexpr uint16_t FF_NULL_UINT16 = 0xFFFF;
constexpr uint32_t FF_NULL_UINT32 = 0xFFFFFFFF;
constexpr uint64_t FF_NULL_UINT64 = 0xFFFFFFFFFFFFFFFF;

// Code Null (Safely traps 0xFFFFFFFF before custom string masking)
constexpr uint32_t FF_CODE_NULL = FF_NULL_UINT32;

// Float Nulls (Using max to adhere to the rule, though NaN is also an option)
constexpr float FF_NULL_F32 = FF_NULL_UINT32;
constexpr double FF_NULL_F64 = FF_NULL_UINT64;
constexpr Offset FF_NULL_OFFSET = FF_NULL_UINT64;
constexpr uint32_t FF_CUSTOM_STRING_FLAG = 0x80000000;

// FastFHIR magic bytes: "FFHR" in little-endian
constexpr uint32_t FF_MAGIC_BYTES = 0x52484646;

enum FHIR_VERSION : uint16_t {
    FHIR_VERSION_R4 = 0x0400,
    FHIR_VERSION_R5 = 0x0500,
};

#ifndef FF_VERSION_MAJOR
#define FF_VERSION_MAJOR 0
#endif

#ifndef FF_VERSION_MINOR
#define FF_VERSION_MINOR 1
#endif


// =====================================================================
// RESULT TYPE
// =====================================================================
enum FF_Result_Code : uint32_t
{
    FF_SUCCESS = 0,
    FF_FAILURE = 1,
    FF_VALIDATION_FAILURE = 2,
    FF_WARNING = 4
};

struct FF_Result
{
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
enum TYPE_SIZE : uint8_t
{
    TYPE_SIZE_UINT8 = 1,
    TYPE_SIZE_UINT16 = 2,
    TYPE_SIZE_UINT24 = 3,
    TYPE_SIZE_UINT32 = 4,
    TYPE_SIZE_INT32 = 4,
    TYPE_SIZE_UINT64 = 8,
    TYPE_SIZE_FLOAT32 = 4,
    TYPE_SIZE_FLOAT64 = 8,
    TYPE_SIZE_OFFSET = 8,
    TYPE_SIZE_RESOURCE = 10,
    TYPE_SIZE_CHOICE = 10,
};

// =====================================================================
// SUPPORTED CHECKSUM ALGORITHMS
// =====================================================================
enum FF_Checksum_Algorithm : uint16_t
{
    FF_CHECKSUM_NONE = 0,
    FF_CHECKSUM_CRC32 = 1,  // 4 bytes  (32 bits)
    FF_CHECKSUM_MD5 = 2,    // 16 bytes (128 bits)
    FF_CHECKSUM_SHA256 = 3, // 32 bytes (256 bits)
};

// Define the maximum inline hash size (256 bits = 32 bytes)
constexpr uint32_t FF_MAX_HASH_BYTES = 32;

// =====================================================================
// FORWARD DECLARATIONS FOR DATA TYPES
// =====================================================================
enum FF_FieldKind : uint16_t
{
    FF_FIELD_UNKNOWN = 0,
    FF_FIELD_STRING,
    FF_FIELD_ARRAY,
    FF_FIELD_BLOCK,
    FF_FIELD_CODE,
    FF_FIELD_BOOL,
    FF_FIELD_INT32,
    FF_FIELD_UINT32,
    FF_FIELD_INT64,
    FF_FIELD_UINT64,
    FF_FIELD_FLOAT64,
    FF_FIELD_RESOURCE,
    FF_FIELD_CHOICE,
};

struct FF_FieldInfo
{
    const char *name = nullptr;
    FF_FieldKind kind = FF_FIELD_UNKNOWN;
    uint16_t field_offset = 0;
    RECOVERY_TAG child_recovery = FF_RECOVER_UNDEFINED;
    uint8_t array_entries_are_offsets = 0;
};
 struct FF_FieldKey
 {
     RECOVERY_TAG owner_recovery = FF_RECOVER_UNDEFINED;
     FF_FieldKind kind = FF_FIELD_UNKNOWN;
     uint16_t field_offset = 0;
     RECOVERY_TAG child_recovery = FF_RECOVER_UNDEFINED;
     uint8_t array_entries_are_offsets = 0;
     const char *name = nullptr;
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
                           const char *field_name,
                           std::size_t field_name_len) noexcept
         : owner_recovery(owner),
           kind(field_kind),
           field_offset(offset),
           child_recovery(child),
           array_entries_are_offsets(array_offsets),
           name(field_name),
           name_len(field_name_len) {}

     constexpr FF_FieldKey(const char *key_name, std::size_t key_name_len) noexcept
         : name(key_name),
           name_len(key_name_len) {}

     static FF_FieldKey from_cstr(const char *key_name) noexcept
     {
         return FF_FieldKey(key_name, key_name ? std::char_traits<char>::length(key_name) : 0);
     }

     static FF_FieldKey from_cstr(RECOVERY_TAG owner,
                                  FF_FieldKind field_kind,
                                  uint16_t offset,
                                  RECOVERY_TAG child,
                                  uint8_t array_offsets,
                                  const char *field_name) noexcept
     {
         return FF_FieldKey(owner,
                            field_kind,
                            offset,
                            child,
                            array_offsets,
                            field_name,
                            field_name ? std::char_traits<char>::length(field_name) : 0);
     }

     constexpr std::string_view view() const noexcept
     {
         return (name && name_len > 0) ? std::string_view{name, name_len} : std::string_view{};
     }

     constexpr operator std::string_view() const noexcept { return view(); }
 };

struct DATA_BLOCK;  // Base structure for all data blocks
struct FF_HEADER;   // Stream header block
struct FF_CHECKSUM; // Checksum block
struct FF_ARRAY;    // Array template block
struct FF_STRING;   // String block

// =====================================================================
// BASE DATA BLOCK
// =====================================================================
struct FF_EXPORT DATA_BLOCK
{
    enum vtable_sizes
    {
        VALIDATION_S = TYPE_SIZE_UINT64,
        RECOVERY_S = TYPE_SIZE_UINT16,
    };
    enum vtable_offsets
    {
        VALIDATION = 0,
        RECOVERY = VALIDATION + VALIDATION_S,
        HEADER_SIZE = RECOVERY + RECOVERY_S,
    };

#ifdef __EMSCRIPTEN__
    Offset __remote = FF_NULL_OFFSET;
    void *__response = nullptr;
#endif

    Offset __offset = FF_NULL_OFFSET;
    Size __size = 0;
    uint32_t __version = 0;

    explicit DATA_BLOCK() = default;
    explicit DATA_BLOCK(Offset off, Size total_size, uint32_t ver)
        : __offset(off), __size(total_size), __version(ver) {}

    operator bool() const { return __offset != FF_NULL_OFFSET; }

    FF_Result validate_offset(const BYTE *const __base, const char *type_name, uint16_t recovery_tag) const noexcept;

#ifdef __EMSCRIPTEN__
    void check_and_fetch_remote(const BYTE *const &__base);
#endif
};

// =====================================================================
// HEADER
// =====================================================================
struct FF_EXPORT FF_HEADER : DATA_BLOCK
{
    static constexpr char type[] = "FF_HEADER";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_HEADER;

    enum vtable_sizes
    {
        MAGIC_S = TYPE_SIZE_UINT32,           // 4
        RECOVERY_S = TYPE_SIZE_UINT16,        // 2
        FHIR_REV_S = TYPE_SIZE_UINT16,        // 2
        STREAM_SIZE_S = TYPE_SIZE_UINT64,     // 8
        ROOT_OFFSET_S = TYPE_SIZE_UINT64,     // 8
        ROOT_RECOVERY_S = TYPE_SIZE_UINT16,   // 2
        CHECKSUM_OFFSET_S = TYPE_SIZE_UINT64, // 8
        VERSION_S = TYPE_SIZE_UINT32,         // 4
    };

    enum vtable_offsets
    {
        MAGIC = 0,                                         // 4 bytes (0-3)
        RECOVERY = MAGIC + MAGIC_S,                        // 2 bytes (4-5)
        FHIR_REV = RECOVERY + RECOVERY_S,                  // 2 bytes (6-7)
        STREAM_SIZE = FHIR_REV + FHIR_REV_S,               // 8 bytes (8-15)  -> Hardware Aligned
        ROOT_OFFSET = STREAM_SIZE + STREAM_SIZE_S,         // 8 bytes (16-23) -> Hardware Aligned
        ROOT_RECOVERY = ROOT_OFFSET + ROOT_OFFSET_S,       // 2 bytes (24-25)
        CHECKSUM_OFFSET = ROOT_RECOVERY + ROOT_RECOVERY_S, // 8 bytes (26-33)
        VERSION = CHECKSUM_OFFSET + CHECKSUM_OFFSET_S,     // 4 bytes (34-37)
        HEADER_SIZE = VERSION + VERSION_S                  // 38 bytes total
    };

    explicit FF_HEADER(Size file_size) noexcept;

    FF_Result validate_full(const BYTE *const __base) const noexcept;
    uint32_t get_engine_version(const BYTE *const __base) const;
    uint16_t get_fhir_rev(const BYTE *const __base) const;
    FF_CHECKSUM get_checksum(const BYTE *const __base) const;
    Offset get_root(const BYTE *const __base) const;
    RECOVERY_TAG get_root_type(const BYTE *const __base) const;
};
void FF_EXPORT STORE_FF_HEADER(BYTE *const __base, uint16_t fhir_revision,
                               Offset checksum_offset, Offset root_offset,
                               RECOVERY_TAG root_recovery, Size payload_size);

// =====================================================================
// FIXED-SIZE CHECKSUM FOOTER
// =====================================================================
struct FF_EXPORT FF_CHECKSUM : DATA_BLOCK
{
    static constexpr char type[] = "FF_CHECKSUM";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_CHECKSUM;

    enum vtable_sizes
    {
        VALIDATION_S = TYPE_SIZE_UINT64, // 8
        RECOVERY_S = TYPE_SIZE_UINT16,   // 2
        ALGORITHM_S = TYPE_SIZE_UINT16,  // 2
        HASH_DATA_S = FF_MAX_HASH_BYTES, // 32
    };
    enum vtable_offsets
    {
        VALIDATION = 0,
        RECOVERY = VALIDATION + VALIDATION_S,  // 8
        ALGORITHM = RECOVERY + RECOVERY_S,     // 10
        HASH_DATA = ALGORITHM + ALGORITHM_S,   // 12
        HEADER_SIZE = HASH_DATA + HASH_DATA_S, // 44 bytes exactly
    };

    explicit FF_CHECKSUM(Offset off, Size size, uint32_t ver) : DATA_BLOCK(off, size, ver) {}

    FF_Result validate_full(const BYTE *const __base) const noexcept;
    FF_Checksum_Algorithm get_algorithm(const BYTE *const __base) const;
    std::string_view get_hash_view(const BYTE *const __base) const;
};

// Allocates the block, writes the metadata, and returns a pointer to the 32-byte hash buffer
BYTE *FF_EXPORT STORE_FF_CHECKSUM_METADATA(BYTE *const __base, Offset start_offset, FF_Checksum_Algorithm algo);

// =====================================================================
// ZERO-COPY ARRAY BLOCK
// =====================================================================
struct FF_EXPORT FF_ARRAY : DATA_BLOCK
{
    static constexpr char type[] = "FF_ARRAY";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_ARRAY;

    // Bitmasks for the packed 16-bit Kind & Step field at offset 10
    static constexpr uint16_t KIND_MASK = 0xC000; // Bits 15-14
    static constexpr uint16_t STEP_MASK = 0x3FFF; // Bits 13-0

    // High-bit flags identifying the physical layout of the elements
    enum EntryKind : uint16_t {
        SCALAR       = 0x0000, // 00... (e.g., bool, double, uint32)
        OFFSET       = 0x4000, // 01... (64-bit pointers to blocks)
        INLINE_BLOCK = 0x8000  // 10... (Contiguous structured blocks)
    };

    enum vtable_sizes
    {
        VALIDATION_S = TYPE_SIZE_UINT64,    // 8
        RECOVERY_S = TYPE_SIZE_UINT16,      // 2
        KIND_AND_STEP_S = TYPE_SIZE_UINT16, // 2 (Packed Kind & Step)
        ENTRY_COUNT_S = TYPE_SIZE_UINT32,   // 4
    };
    
    enum vtable_offsets
    {
        VALIDATION = 0,
        RECOVERY = VALIDATION + VALIDATION_S,          // 8
        KIND_AND_STEP = RECOVERY + RECOVERY_S,         // 10
        ENTRY_COUNT = KIND_AND_STEP + KIND_AND_STEP_S, // 12
        HEADER_SIZE = ENTRY_COUNT + ENTRY_COUNT_S,     // 16 bytes exactly
    };

    explicit FF_ARRAY(Offset off, Size size, uint32_t ver) : DATA_BLOCK(off, size, ver) {}

    FF_Result validate_full(const BYTE *const __base) const noexcept;
    uint16_t entry_step(const BYTE *const __base) const;
    EntryKind entry_kind(const BYTE *const __base) const;
    bool entries_are_pointers(const BYTE *const __base) const;
    uint32_t entry_count(const BYTE *const __base) const;
    const BYTE *entries(const BYTE *const __base) const;
};

void FF_EXPORT STORE_FF_ARRAY_HEADER(BYTE *const __base, Offset &write_head,
                                     FF_ARRAY::EntryKind kind,
                                     uint32_t entry_step, uint32_t entry_count);
void FF_EXPORT STORE_FF_POINTER_ARRAY(BYTE *const __base, Offset &write_head,
                                      const std::vector<Offset> &offsets);

// =====================================================================
// ZERO-COPY STRING BLOCK
// =====================================================================
struct FF_EXPORT FF_STRING : DATA_BLOCK
{
    static constexpr char type[] = "FF_STRING";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_STRING;
    enum vtable_sizes
    {
        VALIDATION_S = TYPE_SIZE_UINT64, // 8
        RECOVERY_S = TYPE_SIZE_UINT16,   // 2
        LENGTH_S = TYPE_SIZE_UINT32,     // 4
    };
    enum vtable_offsets
    {
        VALIDATION = 0,
        RECOVERY = VALIDATION + VALIDATION_S, // 8
        LENGTH = RECOVERY + RECOVERY_S,       // 10
        STRING_DATA = LENGTH + LENGTH_S,      // 14
        HEADER_SIZE = STRING_DATA,            // 14 bytes exactly
    };

    explicit FF_STRING(Offset off, Size size, uint32_t ver) : DATA_BLOCK(off, size, ver) {}

    FF_Result validate_full(const BYTE *const __base) const noexcept;

    // Zero-Copy Mapped View
    std::string_view read_view(const BYTE *const __base) const;

    // Fallback std::string allocation for dictionary parsers
    std::string read(const BYTE *const __base) const;
};

// =====================================================================
// GENERIC RESOURCE WRAPPER
// =====================================================================
// A passive coordinate for polymorphic resources (ie Bundle.Entry.Resource)
struct ResourceReference {
    Offset offset = FF_NULL_OFFSET;
    RECOVERY_TAG recovery = FF_RECOVER_UNDEFINED;

    ResourceReference() = default;
    ResourceReference(Offset off, RECOVERY_TAG rec) : offset(off), recovery(rec) {}
};

// Slim staging structure for polymorphic FHIR choice [x] fields
struct ChoiceEntry {
    RECOVERY_TAG tag = FF_RECOVER_UNDEFINED;
    std::variant<
        std::monostate,
        bool,
        int32_t,
        uint32_t,
        int64_t,
        uint64_t,
        double,
        std::string_view
    > value;

    bool is_empty() const { return tag == FF_RECOVER_UNDEFINED; }
};

// =====================================================================
// LOCK-FREE EMITTER SIGNATURES
// =====================================================================
Size SIZE_FF_STRING(std::string_view str);
Size SIZE_FF_CODE(std::string_view code_str, uint32_t version);
Size STORE_FF_STRING(BYTE *const __base, Offset start_offset, std::string_view str);
uint32_t ENCODE_FF_CODE(BYTE *const __base, Offset block_offset, Offset &child_off, const std::string &code_str, uint32_t version = FHIR_VERSION_R5);
