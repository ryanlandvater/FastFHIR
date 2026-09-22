/**
 * @file FF_Primitives.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @brief Implementation of FastFHIR Core Primitives and Data Structures
    * @license This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * 
 * This source file provides the implementation for the core data structures defined in FF_Primitives.hpp, including:
 * - FF_HEADER: The main file header containing metadata, checksum, and root resource information.
 * - FF_ARRAY: A zero-copy array block for efficient storage of homogeneous entries.
 * - FF_STRING: A zero-copy string block for efficient storage of string data.
 * Each structure includes validation methods to ensure data integrity and recovery tags for error handling.
 * The primitives are designed for high performance and low overhead, enabling zero-copy parsing
 * 
 */

// MARK: - FastFHIR Core Primitives Implementation
#include "FF_Utilities.hpp"
#include "FF_Primitives.hpp"
#include "FF_Dictionary.hpp"
#include "FF_Ops.hpp"
#include <algorithm>

// =====================================================================
// DATA_BLOCK BASE VALIDATION
// =====================================================================
FF_Result DATA_BLOCK::validate_offset(const BYTE *const __base, const char* type_name, RECOVERY_TAG expected_tag) const noexcept {
    if (!*this) {
        return {FF_VALIDATION_FAILURE, std::string("Invalid ") + type_name + ". Offset is NULL."};
    }

#ifndef __EMSCRIPTEN__
    if (FF_GET_VALIDATION(__base, __offset) != static_cast<uint64_t>(__offset)) {
        return {FF_VALIDATION_FAILURE, std::string(type_name) + " failed absolute offset validation."};
    }
#else
    if (FF_GET_VALIDATION(__base, __offset) != static_cast<uint64_t>(__remote)) {
        return {FF_VALIDATION_FAILURE, std::string(type_name) + " failed remote offset validation."};
    }
#endif

    RECOVERY_TAG actual_recovery = FF_GET_RECOVERY_TAG(__base, __offset);
    if (actual_recovery != expected_tag) {
        return {FF_VALIDATION_FAILURE, std::string(type_name) + " recovery tag mismatch. Expected: " + std::to_string(expected_tag) + ", Found: " + std::to_string(actual_recovery)};
    }

    return FF_SUCCESS;
}

#ifdef __EMSCRIPTEN__
void DATA_BLOCK::check_and_fetch_remote(const BYTE *const &base) {
    // Emscripten implementation
}
#endif

// =====================================================================
// HEADER IMPLEMENTATION
// =====================================================================
FF_HEADER::FF_HEADER(Size file_size) noexcept :
    DATA_BLOCK(0, file_size, UINT32_MAX)
{
}

FF_Result FF_HEADER::validate_full(const BYTE* const __base) const noexcept {
    // 1. Magic Bytes Check
    if (get_magic(__base) != FF_MAGIC_BYTES) {
        return {FF_VALIDATION_FAILURE, "FF_HEADER magic bytes mismatch."};
    }
    
    // 2. FHIR Schema Revision Check (Uses 16-bit FHIR_REV)
    uint16_t fhir_rev = LOAD_U16(__base + FHIR_REV);
    if (fhir_rev < FHIR_VERSION_R4) {
        return {FF_VALIDATION_FAILURE, "FF_HEADER unsupported FHIR schema revision."};
    }

    // 3. FastFHIR Engine Version / Stream Layout metadata checks
    uint32_t encoded_version = LOAD_U32(__base + VERSION);
    uint32_t engine_ver = FF_HEADER_ENGINE_VERSION(encoded_version);
    FF_StreamCompaction layout = FF_HEADER_STREAM_LAYOUT(encoded_version);
    if (layout != FF_STREAM_COMPACTION_NONE && layout != FF_STREAM_COMPACTED) {
        return {FF_VALIDATION_FAILURE, "FF_HEADER stream layout flag is invalid."};
    }
    // if ((engine_ver >> 16) > FF_VERSION_MAJOR) return {FF_VALIDATION_FAILURE, "Unsupported engine."};

    // 4. Footer Checksum Validation
    // CAPI-15: validate_offset() dereferences base + checksum_off BEFORE its
    // own truncation check, so a corrupted in-bounds CHECKSUM_OFFSET used to
    // SEGV the Parser ctor instead of failing validation. Bound-check the
    // offset against the stream size here, before any dereference.
    Offset checksum_off = LOAD_U64(__base + CHECKSUM_OFFSET);
    if (checksum_off != FF_NULL_OFFSET) {
        if (checksum_off < FF_HEADER::HEADER_SIZE ||
            checksum_off > __size - FF_CHECKSUM::HEADER_SIZE) {
            return {FF_VALIDATION_FAILURE,
                    "FF_HEADER CHECKSUM_OFFSET out of bounds (corrupted header)."};
        }
        FF_CHECKSUM checksum(checksum_off, __size, fhir_rev, engine_ver);
        auto checksum_result = checksum.validate_full(__base);
        if (checksum_result != FF_SUCCESS) return checksum_result;
    }

    // 5. Cache the decoded engine version for downstream get_header_size() calls.
    const_cast<FF_HEADER*>(this)->__engine_version = engine_ver;

    return {FF_SUCCESS, ""};
}

// Accessors correctly split between Engine Version and FHIR Schema
uint32_t FF_HEADER::get_magic(const BYTE* const __base) const noexcept {
    return LOAD_U32(__base + MAGIC);
}
uint32_t FF_HEADER::get_engine_version(const BYTE* const __base) const {
    return FF_HEADER_ENGINE_VERSION(LOAD_U32(__base + VERSION));
}
FF_StreamCompaction FF_HEADER::get_stream_layout(const BYTE* const __base) const {
    return FF_HEADER_STREAM_LAYOUT(LOAD_U32(__base + VERSION));
}
uint16_t FF_HEADER::get_fhir_rev(const BYTE* const __base) const { return LOAD_U16(__base + FHIR_REV); }

Offset FF_HEADER::get_root(const BYTE* const __base) const { return LOAD_U64(__base + ROOT_OFFSET); }
RECOVERY_TAG FF_HEADER::get_root_type(const BYTE* const __base) const { return static_cast<RECOVERY_TAG>(LOAD_U16(__base + ROOT_RECOVERY)); }
Offset FF_HEADER::get_url_dir_offset(const BYTE* const __base) const { return LOAD_U64(__base + URL_DIR_OFFSET); }
Offset FF_HEADER::get_module_reg_offset(const BYTE* const __base) const { return LOAD_U64(__base + MODULE_REG_OFFSET); }

FF_CHECKSUM FF_HEADER::get_checksum(const BYTE* const __base) const {
    auto checksum = FF_CHECKSUM(LOAD_U64(__base + CHECKSUM_OFFSET), __size,
                                get_fhir_rev(__base), get_engine_version(__base));
    if (!checksum) return checksum;
    
    auto result = checksum.validate_offset(__base, FF_CHECKSUM::type, FF_CHECKSUM::recovery);
    if (result != FF_SUCCESS) throw std::runtime_error("Failed to retrieve checksum: " + result.message);
    
    return checksum;
}

// Strongly-typed C++ implementation (Replaces the Macro)
void STORE_FF_HEADER (BYTE* const __base,
                            uint16_t fhir_rev,
                            Size stream_size,
                            Offset root_offset,
                            RECOVERY_TAG root_recovery,
                            Offset checksum_offset,
                            Offset url_dir_offset,
                            Offset module_reg_offset,
                            FF_StreamCompaction stream_layout) {
                            
    STORE_U32(__base + FF_HEADER::MAGIC, FF_MAGIC_BYTES);
    STORE_U16(__base + FF_HEADER::RECOVERY, RECOVER_FF_HEADER); // Assumes you defined this tag
    STORE_U16(__base + FF_HEADER::FHIR_REV, fhir_rev);

    // Hardware-aligned 64-bit boundaries (Safe for std::atomic_ref)
    STORE_U64(__base + FF_HEADER::STREAM_SIZE, stream_size);
    STORE_U64(__base + FF_HEADER::ROOT_OFFSET, root_offset);
    
    STORE_U16(__base + FF_HEADER::ROOT_RECOVERY, root_recovery);
    STORE_U64(__base + FF_HEADER::CHECKSUM_OFFSET, checksum_offset);
    STORE_U64(__base + FF_HEADER::URL_DIR_OFFSET, url_dir_offset);
    STORE_U64(__base + FF_HEADER::MODULE_REG_OFFSET, module_reg_offset);
    
    // Bake engine version + stream layout into the 32-bit slot.
    // MAJOR is explicitly masked to 14 bits (0x3FFF) before shifting into bits 29..16
    // so that it can never set bits 31..30, which are reserved for FF_STREAM_LAYOUT flags.
    // MINOR is masked to 16 bits (0xFFFF) for symmetry.
    // FF_ENCODE_HEADER_VERSION also applies FF_ENGINE_VERSION_MASK as a second line of defence.
    static_assert(FASTFHIR_VERSION_MAJOR <= 0x3FFF,
        "FASTFHIR_VERSION_MAJOR exceeds 14 bits and would corrupt FF_HEADER::VERSION stream-layout flags");
    uint32_t engine_version =
        ((static_cast<uint32_t>(FASTFHIR_VERSION_MAJOR) & 0x3FFFu) << 16) |
         (static_cast<uint32_t>(FASTFHIR_VERSION_MINOR) & 0xFFFFu);
    STORE_U32(__base + FF_HEADER::VERSION, FF_ENCODE_HEADER_VERSION(engine_version, stream_layout));
}

// =====================================================================
// FIXED-SIZE CHECKSUM IMPLEMENTATION
// =====================================================================
FF_Result FF_CHECKSUM::validate_full(const BYTE* const __base) const noexcept {
    auto result = validate_offset(__base, type, recovery);
    if (!result) return result;
    
    // Since it's a fixed-size block, we just ensure it doesn't overflow the file buffer
    if (__offset + get_header_size() > __size) {
        return {FF_VALIDATION_FAILURE, "FF_CHECKSUM block truncated."};
    }
    return {FF_SUCCESS, ""};
}

FF_Checksum_Algorithm FF_CHECKSUM::get_algorithm(const BYTE* const __base) const {
    return static_cast<FF_Checksum_Algorithm>(LOAD_U16(__base + __offset + ALGORITHM));
}

std::string_view FF_CHECKSUM::get_hash_view(const BYTE* const __base) const {
    FF_Checksum_Algorithm algo = get_algorithm(__base);
    size_t len = 0;
    
    // Determine exact slice size based on the algorithm
    switch(algo) {
        case FF_CHECKSUM_CRC32:  len = 4;  break;
        case FF_CHECKSUM_MD5:    len = 16; break;
        case FF_CHECKSUM_SHA256: len = 32; break;
        default: len = 0; break;
    }
    
    const char* hash_ptr = reinterpret_cast<const char*>(__base + __offset + HASH_DATA);
    return std::string_view(hash_ptr, len);
}

BYTE* STORE_FF_CHECKSUM_METADATA(BYTE* const __base, Offset start_offset, FF_Checksum_Algorithm algo) {
    auto __ptr = __base + start_offset;
    
    // Write metadata
    STORE_U64(__ptr + DATA_BLOCK::VALIDATION, start_offset);
    STORE_U16(__ptr + DATA_BLOCK::RECOVERY,   RECOVER_FF_CHECKSUM);
    STORE_U16(__ptr + FF_CHECKSUM::ALGORITHM, algo);
    
    // Identify the payload pointer
    BYTE* hash_buffer = __ptr + FF_CHECKSUM::HASH_DATA;
    
    // Zero out the entire 256-bit buffer first
    std::memset(hash_buffer, 0, FF_MAX_HASH_BYTES);
    
    return hash_buffer;
}

// =====================================================================
// ARRAY BLOCK IMPLEMENTATION
// =====================================================================
FF_Result FF_ARRAY::validate_full(const BYTE* const __base) const noexcept {
    if (!*this) return {FF_VALIDATION_FAILURE, "Invalid FF_ARRAY. Offset is NULL."};

#ifndef __EMSCRIPTEN__
    if (LOAD_U64(__base + __offset + VALIDATION) != __offset)
        return {FF_VALIDATION_FAILURE, "FF_ARRAY failed absolute offset validation."};
#endif

    RECOVERY_TAG actual_recovery = static_cast<RECOVERY_TAG>(LOAD_U16(__base + __offset + RECOVERY));
    if (!IsArrayTag(actual_recovery)) {
        return {FF_VALIDATION_FAILURE, "FF_ARRAY missing semantic array bit (0x8000)."};
    }

    uint16_t step = entry_step(__base);
    switch (entry_kind(__base)) {
        case SCALAR:
        case OFFSET:
        case INLINE_BLOCK: break;
        default: return {FF_VALIDATION_FAILURE, "FF_ARRAY contains undefined entry kind"};
    }
    
    uint32_t count = entry_count(__base);
    if (__offset + get_header_size() + static_cast<uint64_t>(step) * count > __size) {
        return {FF_VALIDATION_FAILURE, "FF_ARRAY entries exceed file boundaries."};
    }
    return FF_SUCCESS;
}

uint16_t FF_ARRAY::entry_step(const BYTE* const __base) const
{ return LOAD_U16(__base + __offset + KIND_AND_STEP) & STEP_MASK; }

FF_ARRAY::EntryKind FF_ARRAY::entry_kind(const BYTE *const __base) const
{ return static_cast<EntryKind>(LOAD_U16(__base + __offset + KIND_AND_STEP) & KIND_MASK); }

bool FF_ARRAY::entries_are_pointers(const BYTE *const base) const
{return entry_kind(base) == OFFSET;}

uint32_t FF_ARRAY::entry_count(const BYTE* const __base) const
{
    // The stamped count is a WIRE VALUE with no second witness (F4), so it is
    // clamped to what the stream can actually hold. Iris does the same thing
    // structurally -- ArrayHeader::fits() requires stride*count to fit inside
    // the file -- and the reason is not tidiness: a corrupted count is not a
    // wrong number, it is an unbounded loop. Measured, a 256-flip artifact
    // turned the reader from a segfault into a HANG once the offsets were
    // bounds-checked but the count still was not.
    //
    // Clamped, not rejected: a truncated array is recoverable data, and the
    // element-by-element walk (walk_array_extent) is what says how many are
    // really there.
    const uint32_t stamped = LOAD_U32(__base + __offset + ENTRY_COUNT);
    const Size header = get_header_size();
    if (!FF_BLOCK_IN_BOUNDS(__offset, __size, header))
        return 0;
    const uint16_t step = entry_step(__base);
    if (step == 0)
        return 0;  // no stride: nothing in this array is addressable, so it
                   // holds nothing readable. Returning the stamped count here
                   // was an unbounded loop wearing a clamp -- up to 4 billion
                   // iterations over entries that are all at the same address.
                   // DeepValidator already treats step==0 with count!=0 as
                   // damage; the reader must not disagree with it.
    const std::size_t room =
        (static_cast<std::size_t>(__size) - static_cast<std::size_t>(__offset) - header) / step;
    return static_cast<uint32_t>(std::min<std::size_t>(stamped, room));
}

const BYTE* FF_ARRAY::entries(const BYTE* const __base) const
{ return __base + __offset + get_header_size(); }

void STORE_FF_ARRAY_HEADER(BYTE* const __base, Offset& write_head, FF_ARRAY::EntryKind kind, uint32_t entry_step, uint32_t entry_count, RECOVERY_TAG entry_recovery_tag) {    // Validate that the step size doesn't overflow into the kind bits (max 16.38kb)
    if (entry_step > FF_ARRAY::STEP_MASK) {
        throw std::runtime_error("FastFHIR: FF_ARRAY entry step size exceeds maximum 14-bit permitted value.");
    }
    auto __ptr = __base + write_head;
    // Dynamic pointer validation maintained
    STORE_U64(__ptr + FF_ARRAY::VALIDATION, write_head);
    STORE_U16(__ptr + FF_ARRAY::RECOVERY, RECOVER_ARRAY_BIT | entry_recovery_tag);
    // Bitwise pack the Kind (top 2 bits) and the Step (bottom 14 bits)
    STORE_U16(__ptr + FF_ARRAY::KIND_AND_STEP, static_cast<uint16_t>(kind) | static_cast<uint16_t>(entry_step));
    STORE_U32(__ptr + FF_ARRAY::ENTRY_COUNT, entry_count);
    write_head += FF_ARRAY::HEADER_SIZE;
}

// =====================================================================
// STRING BLOCK IMPLEMENTATION
// =====================================================================
FF_Result FF_STRING::validate_full(const BYTE* const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<FF_STRING&>(*this).check_and_fetch_remote(__base);
#endif
    auto result = validate_offset(__base, type, recovery);
    if (result != FF_SUCCESS) return result;
    uint32_t len = length(__base);
    if (__offset + get_header_size() + len > __size) {
        return {FF_VALIDATION_FAILURE, "FF_STRING length exceeds file boundaries."};
    }
    return {FF_SUCCESS, ""};
}
// Zero-Copy Mapped View
std::string_view FF_STRING::read_view(const BYTE* const __base) const {
    // REFUSE, DO NOT CLAMP -- the Iris contract (ByteArrayHeader::fits requires
    // count <= size - (off + header_size); a block that does not fit is simply
    // not a block).
    //
    // Both halves of a string are wire values: the OFFSET that got us here and
    // the LENGTH stamped at +10. An earlier version of this clamped an
    // over-long length to the remaining arena, on the reasoning that a
    // truncated string beats a crash. That was wrong twice over. Clamping to
    // the rest of the buffer is not a truncation, it INVENTS a string -- up to
    // a megabyte of arbitrary bytes that were never this field's -- and
    // print_json then dutifully JSON-escapes every one of them. Measured on a
    // 256-flip artifact, that turned a segfault into a process that never
    // finished, burning all its time in escape_json_string.
    //
    // A length that does not fit is damage. Empty is the honest answer, and
    // validate_FFHR_stream() is what reports it.
    if (__size != 0) {
        if (!FF_BLOCK_IN_BOUNDS(__offset, __size, STRING_DATA))
            return {};
        const std::size_t avail =
            static_cast<std::size_t>(__size) - static_cast<std::size_t>(__offset) - STRING_DATA;
        if (static_cast<std::size_t>(length(__base)) > avail)
            return {};
    }
    return std::string_view(reinterpret_cast<const char*>(__base + __offset + STRING_DATA),
                            length(__base));
}


// Fallback std::string allocation for dictionary parsers
std::string FF_STRING::read(const BYTE* const __base) const {
    return std::string(read_view(__base));
}

// =====================================================================
// LOCK-FREE STRING & MSB DICTIONARY EMITTERS
// =====================================================================
// SIZE_FF_STRING reports the size of what STORE_FF_STRING WRITES -- nothing else.
// It must never special-case the empty string: STORE_FF_STRING writes a full
// 14-byte FF_STRING header for a zero-length payload, so a size of 0 here means
// the caller claims 14 bytes too few and the store runs into whatever the NEXT
// claim_space() hands out. That is the exact mechanism of TASKS.md A23 Bug B,
// reachable through three separate paths (string[] elements, unique_ptr-stored
// dateTime/markdown/... fields, and TypeTraits<std::string_view>).
//
// "Absent" is NOT this function's job. Callers that treat an empty string as an
// absent field guard with `!empty()` and write FF_NULL_OFFSET into the slot --
// see generate_size_fields/generate_store_fields, which guard in lockstep. Array
// elements cannot be skipped that way (it would change the element count), so
// they store a real empty FF_STRING and this size must account for it.
Size SIZE_FF_STRING(std::string_view str) {
    return FF_STRING::HEADER_SIZE + str.size();
}
Size SIZE_FF_CODE(std::string_view code_str, uint32_t version = FHIR_VERSION_R5) {
    if (code_str.empty()) return 0;
    if (FF_GetDictionaryCode(std::string(code_str), version) != FF_CODE_NULL) return 0;
    return SIZE_FF_STRING(code_str);
}
Size STORE_FF_CODE(BYTE* const __base, Offset start_offset, std::string_view code_str, uint32_t version) {
    // If the code is in the dictionary, it's stored inline as a uint32_t
    // in the vtable slot (4 bytes, already counted in the block header stride).
    // If not found in the dictionary, store as a custom FF_STRING.
    if (FF_GetDictionaryCode(std::string(code_str), version) != FF_CODE_NULL)
        return 0; // Dictionary code — inline, nothing extra to store
    return STORE_FF_STRING(__base, start_offset, code_str);
}

Size STORE_FF_STRING(BYTE* const __base, Offset start_offset, std::string_view str,
                     RECOVERY_TAG tag) {
    auto __ptr = __base + start_offset;
    uint32_t length = static_cast<uint32_t>(str.size());

    STORE_U64(__ptr + DATA_BLOCK::VALIDATION, start_offset);
    STORE_U16(__ptr + DATA_BLOCK::RECOVERY,   tag);
    STORE_U32(__ptr + FF_STRING::LENGTH,      length);
    
    std::memcpy(__ptr + FF_STRING::STRING_DATA, str.data(), length);
    
    return FF_STRING::HEADER_SIZE + length;
}

#include <cstdlib>  // strtoull

static uint32_t _pack_codeable_concept_offset(Offset cc_offset, Offset block_offset) {
    int64_t rel = static_cast<int64_t>(cc_offset) - static_cast<int64_t>(block_offset);
    if (rel < -0x40000000LL || rel > 0x3FFFFFFFLL) {
        throw std::runtime_error("FastFHIR: CodeableConcept relative offset exceeds ±1 GB.");
    }
    // rel == -1 encodes to all-ones, which IS FF_CODE_NULL. It cannot occur
    // -- the smallest block is DATA_BLOCK::HEADER_SIZE bytes, so a preceding
    // FF_CODED_VALUE is at least that far back -- but the sentinel's whole
    // claim to that bit pattern is that no real offset produces it, and a claim
    // worth making is worth checking. FF_DATETIME has the identical case at 63 bits.
    if (rel == -1) {
        throw std::runtime_error(
            "FastFHIR: CodeableConcept fallback offset of -1 collides with FF_CODE_NULL.");
    }
    return (static_cast<uint32_t>(static_cast<int32_t>(rel)) & FF_CODE_PAYLOAD_MASK) | FF_CODED_VALUE_FLAG;
}

// =====================================================================
// CodeableConcept codec table — ONE description per system
// =====================================================================
// Encode and decode both drive from this. Neither has its own per-system
// switch, so a system's payload width, numeric base and output format are
// stated exactly once.
//
// They used to be two independent switches. Widening CPT from 2 to 4 bytes
// meant editing both, and the decode side still carried a `char buf[4]`
// sized for the old uint8 CVX -- enough to truncate 65535 to "655". A second
// switch is a second chance to get it wrong.
//
// Adding a system: add a row. Changing a width: change one number.
struct FF_CC_Codec {
    FF_CodeableConceptSystem system;
    uint8_t     payload_bytes;  ///< 0 => variable-length ASCII payload
    uint8_t     parse_base;     ///< 10, or 16 for DICOM; ignored when variable
    bool        hex_out;        ///< render as %08X instead of decimal
    const char *name;           ///< for diagnostics
};

static constexpr FF_CC_Codec FF_CC_CODECS[] = {
    // system                              bytes  base  hex    name
    {FF_CodeableConceptSystem::UNKNOWN,     0,    0,   false, "UNKNOWN"},
    {FF_CodeableConceptSystem::UCUM,        0,    0,   false, "UCUM"},
    {FF_CodeableConceptSystem::SNOMED_CT,   8,   10,   false, "SNOMED CT"},
    {FF_CodeableConceptSystem::RXNORM,      4,   10,   false, "RxNorm"},
    {FF_CodeableConceptSystem::LOINC,       0,    0,   false, "LOINC"},
    {FF_CodeableConceptSystem::DICOM,       4,   16,   true,  "DICOM"},
    {FF_CodeableConceptSystem::CPT,         4,   10,   false, "CPT"},
    {FF_CodeableConceptSystem::CVX,         2,   10,   false, "CVX"},
    {FF_CodeableConceptSystem::NDC,         0,    0,   false, "NDC"},
    {FF_CodeableConceptSystem::ICD_9_CM,    0,    0,   false, "ICD-9-CM"},
    {FF_CodeableConceptSystem::ICD_10,      0,    0,   false, "ICD-10"},
    {FF_CodeableConceptSystem::ISO_3166,    0,    0,   false, "ISO 3166"},
    {FF_CodeableConceptSystem::MDC,         4,   10,   false, "MDC"},
    {FF_CodeableConceptSystem::UNII,        0,    0,   false, "UNII"},
    {FF_CodeableConceptSystem::MED_RT,      8,   10,   false, "MED-RT"},
    {FF_CodeableConceptSystem::PCLOCD,      0,    0,   false, "pCLOCD"},
    {FF_CodeableConceptSystem::IDMP,        8,   10,   false, "IDMP"},
};

static constexpr const FF_CC_Codec *ff_cc_codec(FF_CodeableConceptSystem system)
{
    for (const auto &c : FF_CC_CODECS)
        if (c.system == system) return &c;
    return nullptr;
}

/// Store `value` little-endian in `bytes` bytes. Width comes from the codec
/// table, so there is no per-system store logic anywhere else.
static void ff_cc_store(BYTE *at, uint64_t value, uint8_t bytes)
{
    switch (bytes) {
    case 1: at[0] = static_cast<uint8_t>(value); break;
    case 2: STORE_U16(at, static_cast<uint16_t>(value)); break;
    case 4: STORE_U32(at, static_cast<uint32_t>(value)); break;
    case 8: STORE_U64(at, value); break;
    default: throw std::runtime_error("FastFHIR: unsupported CodeableConcept payload width.");
    }
}

static uint64_t ff_cc_load(const BYTE *at, uint8_t bytes)
{
    switch (bytes) {
    case 1: return at[0];
    case 2: return LOAD_U16(at);
    case 4: return LOAD_U32(at);
    case 8: return LOAD_U64(at);
    default: return 0;
    }
}

// Parse a numeric code and refuse anything the fixed-width payload cannot hold.
// Narrowing silently is data corruption: CPT is a 2-byte payload but real CPT
// codes run to 99499, so static_cast<uint16_t>(99213) stores 33677 and the
// decoder faithfully reports the wrong procedure.
static uint64_t parse_fixed_width_code_(const std::string& code_str, unsigned payload_bytes,
                                        const char* system_name, int base = 10)
{
    char* end = nullptr;
    const uint64_t value = strtoull(code_str.c_str(), &end, base);
    if (end == code_str.c_str() || (end && *end != '\0')) {
        throw std::runtime_error(std::string("FastFHIR: ") + system_name +
                                 " code '" + code_str + "' is not a valid number.");
    }
    const uint64_t limit = (payload_bytes >= 8) ? UINT64_MAX
                                                : ((uint64_t{1} << (payload_bytes * 8)) - 1);
    if (value > limit) {
        throw std::runtime_error(std::string("FastFHIR: ") + system_name + " code '" + code_str +
                                 "' does not fit the " + std::to_string(payload_bytes) +
                                 "-byte payload (max " + std::to_string(limit) +
                                 "). Storing it would silently truncate to " +
                                 std::to_string(value & limit) + ".");
    }
    return value;
}

// Write the FF_CODED_VALUE header and advance the child-offset cursor.
// Every system branch in ENCODE_FF_CODE shares this — previously copy-pasted
// 7 times. If the CC layout changes, this is the ONE place to update.
static void write_cc_header_(BYTE* ptr, Offset cc_offset, Offset& child_off,
                             FF_CodeableConceptSystem system, uint8_t payload_len)
{
    child_off += FF_CODED_VALUE::HEADER_SIZE + payload_len;
    STORE_U64(ptr + FF_CODED_VALUE::VALIDATION, cc_offset);
    STORE_U16(ptr + FF_CODED_VALUE::RECOVERY,   RECOVER_FF_CODED_VALUE);
    ptr[FF_CODED_VALUE::SYSTEM] = static_cast<uint8_t>(system);
    ptr[FF_CODED_VALUE::LENGTH] = payload_len;
}

uint32_t ENCODE_FF_CODE(BYTE* const __base, Offset block_offset, Offset& child_off, const std::string& code_str, uint32_t version, FF_CodeableConceptSystem system) {
    if (code_str.empty()) return FF_CODE_NULL;

    // Dictionary lookup first — always the fast path (MSB = 0).
    uint32_t dict_code = FF_GetDictionaryCode(code_str, version);
    if (dict_code != FF_CODE_NULL) {
        return dict_code;
    }

    // Everything below is driven by FF_CC_CODECS. There is deliberately no
    // per-system switch here -- see the table's comment.
    const FF_CC_Codec *codec = ff_cc_codec(system);
    if (codec == nullptr) codec = ff_cc_codec(FF_CodeableConceptSystem::UNKNOWN);

    const Offset cc_offset = child_off;
    BYTE *ptr = __base + cc_offset;

    if (codec->payload_bytes > 0) {
        // Fixed-width numeric payload. parse_fixed_width_code_ refuses a value
        // the width cannot hold rather than narrowing it silently.
        const uint64_t value =
            parse_fixed_width_code_(code_str, codec->payload_bytes, codec->name, codec->parse_base);
        write_cc_header_(ptr, cc_offset, child_off, codec->system, codec->payload_bytes);
        ff_cc_store(ptr + FF_CODED_VALUE::PAYLOAD, value, codec->payload_bytes);
        return _pack_codeable_concept_offset(cc_offset, block_offset);
    }

    if (codec->system == FF_CodeableConceptSystem::UNKNOWN) {
        // 2-byte URL-directory index, then the raw code string.
        constexpr size_t URL_IDX_BYTES = 2;
        if (code_str.size() > 255 - URL_IDX_BYTES) {
            throw std::runtime_error(
                "FastFHIR: Code string too long for UNKNOWN CodeableConcept (max " +
                std::to_string(255 - URL_IDX_BYTES) + " bytes, got " +
                std::to_string(code_str.size()) + ").");
        }
        const uint8_t payload_len = static_cast<uint8_t>(URL_IDX_BYTES + code_str.size());
        write_cc_header_(ptr, cc_offset, child_off, codec->system, payload_len);
        STORE_U16(ptr + FF_CODED_VALUE::PAYLOAD, uint16_t{0});  // 0 = not yet registered
        std::memcpy(ptr + FF_CODED_VALUE::PAYLOAD + URL_IDX_BYTES,
                    code_str.data(), code_str.size());
        return _pack_codeable_concept_offset(cc_offset, block_offset);
    }

    // Variable-length ASCII payload (UCUM, LOINC, NDC, ICD, ISO, UNII, pCLOCD).
    if (code_str.size() > 255) {
        throw std::runtime_error(
            std::string("FastFHIR: Code string too long for ") + codec->name +
            " CodeableConcept (max 255 bytes, got " + std::to_string(code_str.size()) + ").");
    }
    const uint8_t payload_len = static_cast<uint8_t>(code_str.size());
    write_cc_header_(ptr, cc_offset, child_off, codec->system, payload_len);
    std::memcpy(ptr + FF_CODED_VALUE::PAYLOAD, code_str.data(), code_str.size());
    return _pack_codeable_concept_offset(cc_offset, block_offset);
}

// =====================================================================
// PACKED DATE/TIME — parse, format, and the SIZE/STORE/ENCODE triple
// =====================================================================
// The date/time counterpart of the code emitters above; the slot contract is
// tabulated against FF_CODE's in FF_Primitives.hpp. Text that parses and fits
// becomes a packed 63-bit value; anything else keeps its ORIGINAL bytes in an
// FF_STRING reached by a signed relative offset. That is what makes the round
// trip byte-exact without asking the wire format to be a text format.
//
// A parse failure is NOT an exception, deliberately. It takes the same fallback
// path as text that is legal but does not fit, for the same reason a code that
// is not in the dictionary becomes a block instead of an error: preserving the
// bytes that arrived is always defensible, and deciding whether they are legal
// FHIR belongs to ingest (DT-3), which has the resource context to say so.

static uint64_t _pack_datetime_offset(Offset str_offset, Offset block_offset) {
    int64_t rel = static_cast<int64_t>(str_offset) - static_cast<int64_t>(block_offset);
    if (rel < -0x4000000000000000LL || rel > 0x3FFFFFFFFFFFFFFFLL) {
        throw std::runtime_error(
            "FastFHIR: date/time fallback offset " + std::to_string(rel) +
            " exceeds the 63-bit signed relative range.");
    }
    // rel == -1 encodes to all-ones, which IS FF_DATETIME_NULL. It cannot occur
    // -- the smallest block is DATA_BLOCK::HEADER_SIZE bytes, so a preceding
    // FF_STRING is at least that far back -- but the sentinel's whole claim to
    // that bit pattern is that no real offset produces it, and a claim worth
    // making is worth checking. FF_CODE has the identical latent case at 31 bits.
    if (rel == -1) {
        throw std::runtime_error(
            "FastFHIR: date/time fallback offset of -1 collides with FF_DATETIME_NULL.");
    }
    return (static_cast<uint64_t>(rel) & FF_DATETIME_PAYLOAD_MASK) | FF_DATETIME_FALLBACK_FLAG;
}

namespace {

constexpr bool dt_is_leap(uint32_t y) noexcept {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

constexpr uint32_t dt_days_in_month(uint32_t y, uint32_t m) noexcept {
    constexpr uint8_t DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && dt_is_leap(y)) ? 29u : DAYS[m - 1];
}

constexpr uint8_t dt_rank(FF_DateTimePrecision p) noexcept {
    return static_cast<uint8_t>(p);
}

// Exactly `count` ASCII digits at `pos`. Strict on purpose: FHIR zero-pads every
// field, so "2024-1-5" is not a value FHIR can express, and accepting it would
// re-render as different text than arrived -- a silent edit, not a round trip.
bool dt_digits(std::string_view s, size_t pos, size_t count, uint32_t& out) noexcept {
    if (pos + count > s.size()) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < count; ++i) {
        const char c = s[pos + i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint32_t>(c - '0');
    }
    out = v;
    return true;
}

// `Z` or +/-hh:mm, and it must be the whole remainder of the text.
bool dt_timezone(std::string_view s, size_t pos, int16_t& out) noexcept {
    if (pos >= s.size()) return false;
    if (s[pos] == 'Z') {
        if (pos + 1 != s.size()) return false;
        out = FF_DATETIME_OFFSET_Z;
        return true;
    }
    const char sign = s[pos];
    if (sign != '+' && sign != '-') return false;

    uint32_t hh = 0, mm = 0;
    if (!dt_digits(s, pos + 1, 2, hh)) return false;
    if (pos + 3 >= s.size() || s[pos + 3] != ':') return false;
    if (!dt_digits(s, pos + 4, 2, mm) || pos + 6 != s.size()) return false;
    if (mm > 59) return false;

    const int32_t minutes = static_cast<int32_t>(hh) * 60 + static_cast<int32_t>(mm);
    if (minutes > FF_DATETIME_OFFSET_MAX) return false;   // beyond +/-14:00
    out = static_cast<int16_t>(sign == '-' ? -minutes : minutes);
    return true;
}

// hh:mm:ss[.f{1,3}] starting at `pos`; `end` lands on the first byte after it.
// A fraction of 4+ digits is legal FHIR that this layout cannot hold, so it is
// rejected here and collected by the FF_STRING fallback.
bool dt_time(std::string_view s, size_t pos, FF_DateTimeParts& p, size_t& end) noexcept {
    uint32_t hh = 0, mm = 0, ss = 0;
    if (!dt_digits(s, pos, 2, hh)) return false;
    if (pos + 2 >= s.size() || s[pos + 2] != ':') return false;
    if (!dt_digits(s, pos + 3, 2, mm)) return false;
    if (pos + 5 >= s.size() || s[pos + 5] != ':') return false;
    if (!dt_digits(s, pos + 6, 2, ss)) return false;
    if (hh > 23 || mm > 59 || ss > 60) return false;   // 60: leap seconds are legal FHIR

    p.hour      = static_cast<uint8_t>(hh);
    p.minute    = static_cast<uint8_t>(mm);
    p.second    = static_cast<uint8_t>(ss);
    p.precision = FF_DateTimePrecision::SECOND;
    end         = pos + 8;

    if (end >= s.size() || s[end] != '.') return true;

    uint32_t frac = 0, digits = 0;
    for (size_t i = end + 1; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, ++digits) {
        if (digits == 3) return false;                 // 4+ fractional digits
        frac = frac * 10 + static_cast<uint32_t>(s[i] - '0');
    }
    if (digits == 0) return false;                     // a bare '.' is not a value

    constexpr uint16_t SCALE[3] = {100, 10, 1};
    p.millisecond = static_cast<uint16_t>(frac * SCALE[digits - 1]);
    p.precision   = static_cast<FF_DateTimePrecision>(
        dt_rank(FF_DateTimePrecision::SECOND) + digits);
    end += 1 + digits;
    return true;
}

void dt_append(std::string& s, uint32_t value, size_t width) {
    char buf[4];
    for (size_t i = width; i-- > 0; value /= 10) {
        buf[i] = static_cast<char>('0' + value % 10);
    }
    s.append(buf, width);
}

} // namespace

std::optional<FF_DateTimeParts> FF_PARSE_DATETIME(std::string_view text, RECOVERY_TAG tag) {
    if (text.empty()) return std::nullopt;

    const RECOVERY_TAG base = GetTypeFromTag(tag);
    FF_DateTimeParts p;

    // FHIR 'time' is a duration since midnight: no date, no timezone. Both the
    // days and offset fields stay zero, which is the rule that keeps two equal
    // values one integer compare (see FF_DateTimeParts).
    if (base == RECOVER_FF_TIME) {
        size_t end = 0;
        if (!dt_time(text, 0, p, end) || end != text.size()) return std::nullopt;
        return p;
    }

    uint32_t y = 0, m = 1, d = 1;
    if (!dt_digits(text, 0, 4, y) || y < 1) return std::nullopt;   // 4 digits caps y at 9999
    p.precision = FF_DateTimePrecision::YEAR;
    size_t pos = 4;

    if (pos < text.size()) {
        if (text[pos] != '-' || !dt_digits(text, pos + 1, 2, m)) return std::nullopt;
        if (m < 1 || m > 12) return std::nullopt;
        p.precision = FF_DateTimePrecision::YEAR_MONTH;
        pos += 3;
    }
    if (pos < text.size() && text[pos] == '-') {
        if (!dt_digits(text, pos + 1, 2, d)) return std::nullopt;
        if (d < 1 || d > dt_days_in_month(y, m)) return std::nullopt;
        p.precision = FF_DateTimePrecision::DATE;
        pos += 3;
    }
    p.days = ff_datetime_days_from_civil(static_cast<int32_t>(y), m, d);

    if (pos < text.size()) {
        // FHIR's grammar makes the timezone mandatory once 'T' is present, which
        // is also why there is no MINUTE precision to represent.
        if (text[pos] != 'T') return std::nullopt;
        size_t end = 0;
        if (!dt_time(text, pos + 1, p, end)) return std::nullopt;
        if (!dt_timezone(text, end, p.utc_offset)) return std::nullopt;
    }

    // Per-type rules. One encoder, four tags: the tag is what says which of the
    // union's members this text is allowed to be.
    const bool has_time = dt_rank(p.precision) >= dt_rank(FF_DateTimePrecision::SECOND);
    if (base == RECOVER_FF_DATE && has_time) return std::nullopt;
    if (base == RECOVER_FF_INSTANT && !has_time) return std::nullopt;
    return p;
}

std::string FF_FORMAT_DATETIME(const FF_DateTimeParts& p, RECOVERY_TAG tag) {
    const RECOVERY_TAG base = GetTypeFromTag(tag);
    const uint8_t prec = dt_rank(p.precision);
    std::string out;

    if (base != RECOVER_FF_TIME) {
        const FF_CivilDate civil = ff_datetime_civil_from_days(p.days);
        dt_append(out, static_cast<uint32_t>(civil.year), 4);
        if (prec >= dt_rank(FF_DateTimePrecision::YEAR_MONTH)) {
            out += '-';
            dt_append(out, civil.month, 2);
        }
        if (prec >= dt_rank(FF_DateTimePrecision::DATE)) {
            out += '-';
            dt_append(out, civil.day, 2);
        }
        if (prec < dt_rank(FF_DateTimePrecision::SECOND)) return out;
        out += 'T';
    }

    dt_append(out, p.hour, 2);
    out += ':';
    dt_append(out, p.minute, 2);
    out += ':';
    dt_append(out, p.second, 2);

    if (prec > dt_rank(FF_DateTimePrecision::SECOND)) {
        constexpr uint16_t SCALE[3] = {100, 10, 1};
        const uint8_t digits = prec - dt_rank(FF_DateTimePrecision::SECOND);
        out += '.';
        dt_append(out, p.millisecond / SCALE[digits - 1], digits);
    }

    if (base == RECOVER_FF_TIME) return out;
    if (p.utc_offset == FF_DATETIME_OFFSET_Z) return out += 'Z';

    const uint32_t magnitude =
        static_cast<uint32_t>(p.utc_offset < 0 ? -p.utc_offset : p.utc_offset);
    out += (p.utc_offset < 0) ? '-' : '+';
    dt_append(out, magnitude / 60, 2);
    out += ':';
    dt_append(out, magnitude % 60, 2);
    return out;
}

Size SIZE_FF_DATETIME(std::string_view text, RECOVERY_TAG tag) {
    if (text.empty()) return 0;
    const std::optional<FF_DateTimeParts> parts = FF_PARSE_DATETIME(text, tag);
    if (parts && ff_datetime_fits(*parts)) return 0;   // packs inline, no child block
    return SIZE_FF_STRING(text);
}

Size STORE_FF_DATETIME(BYTE* const __base, Offset start_offset,
                         std::string_view text, RECOVERY_TAG tag) {
    if (text.empty()) return 0;
    const std::optional<FF_DateTimeParts> parts = FF_PARSE_DATETIME(text, tag);
    if (parts && ff_datetime_fits(*parts)) return 0;
    return STORE_FF_STRING(__base, start_offset, text);
}

uint64_t ENCODE_FF_DATETIME(BYTE* const __base, Offset block_offset, Offset& child_off,
                            std::string_view text, RECOVERY_TAG tag) {
    if (text.empty()) return FF_DATETIME_NULL;

    // ff_datetime_fits is not redundant with the parser's own range checks: it
    // is the one place FF_PACK_DATETIME's precondition is enforced, so a future
    // parser change cannot quietly start packing a value the fields cannot hold.
    const std::optional<FF_DateTimeParts> parts = FF_PARSE_DATETIME(text, tag);
    if (parts && ff_datetime_fits(*parts)) return FF_PACK_DATETIME(*parts);

    const Offset str_offset = child_off;
    child_off += STORE_FF_STRING(__base, str_offset, text);
    return _pack_datetime_offset(str_offset, block_offset);
}

// =====================================================================
// FF_DECODE_CODED_VALUE — unified dynamic fallback block decoder
// =====================================================================
// Reads the SYSTEM discriminator byte and variable-length PAYLOAD from
// the unified FF_CODED_VALUE.  Returns the code as a human-readable
// string_view (thread-local buffer for fixed-width types).
FF_CodedValueResult FF_DECODE_CODED_VALUE(
    const BYTE* base, Offset offset, uint32_t version, Size stream_size)
{
    // Same gate as every other block hop; a CodeableConcept is a block.
    if (!FF_BLOCK_IN_BOUNDS(offset, stream_size, FF_CODED_VALUE::HEADER_SIZE))
        return {};

    using S = FF_CodeableConceptSystem;
    const S sys = static_cast<S>(base[offset + FF_CODED_VALUE::SYSTEM]);
    const uint8_t len = base[offset + FF_CODED_VALUE::LENGTH];
    const BYTE *payload = base + offset + FF_CODED_VALUE::PAYLOAD;

    // Same table the encoder uses -- no second per-system switch.
    const FF_CC_Codec *codec = ff_cc_codec(sys);
    if (codec == nullptr) return {S::UNKNOWN, 0, {}};

    if (codec->payload_bytes > 0) {
        if (len < codec->payload_bytes) return {sys, 0, {}};  // truncated block
        const uint64_t value = ff_cc_load(payload, codec->payload_bytes);
        // Sized for the widest payload (uint64 -> 20 digits) so a width change
        // in the table can never outgrow the buffer.
        thread_local char buf[24];
        const int pos = codec->hex_out
                            ? snprintf(buf, sizeof(buf), "%08llX",
                                       static_cast<unsigned long long>(value))
                            : snprintf(buf, sizeof(buf), "%llu",
                                       static_cast<unsigned long long>(value));
        return {sys, value, std::string_view(buf, static_cast<size_t>(pos))};
    }

    if (sys == S::UNKNOWN) {
        // LENGTH covers the 2-byte url index plus the string. Check BEFORE
        // reading the index, or a truncated block reads past the payload.
        constexpr uint8_t URL_IDX_BYTES = 2;
        if (len < URL_IDX_BYTES) return {S::UNKNOWN, 0, {}};
        const uint16_t url_idx = LOAD_U16(payload);
        return {sys, url_idx,
                std::string_view(reinterpret_cast<const char *>(payload + URL_IDX_BYTES),
                                 static_cast<size_t>(len - URL_IDX_BYTES))};
    }

    // Variable-length ASCII payload.
    return {sys, 0, std::string_view(reinterpret_cast<const char *>(payload), len)};
}

// =====================================================================
// FF_URL_DIRECTORY — stream-level URL intern table (chained-segment model)
// =====================================================================
uint32_t FF_URL_DIRECTORY::entry_count(const BYTE* base) const {
    return LOAD_U32(base + __offset + ENTRY_COUNT);
}
uint32_t FF_URL_DIRECTORY::prior_idx(const BYTE* base, uint32_t entry_idx) const {
    Offset ep = __offset + HEADER_SIZE + static_cast<Offset>(entry_idx) * URL_ENTRY_SIZE;
    return LOAD_U32(base + ep + URL_ENTRY_PRIOR_IDX);
}
Offset FF_URL_DIRECTORY::seg_offset(const BYTE* base, uint32_t entry_idx) const {
    Offset ep = __offset + HEADER_SIZE + static_cast<Offset>(entry_idx) * URL_ENTRY_SIZE;
    return LOAD_U64(base + ep + URL_ENTRY_SEG_OFFSET);
}
std::string_view FF_URL_DIRECTORY::seg_string(const BYTE* base, uint32_t entry_idx) const {
    // BOUND FIRST (P2). `entry_idx` is a 4-byte ref lifted from a V-Table slot a
    // hostile stream can forge, and entry_count() is itself a 4-byte read from a
    // block that stream can forge -- so neither the index nor the declared count
    // is a bound. The count is clamped to what the STREAM can hold and an index
    // past that is refused, not indexed. The read path degrades rather than
    // throwing (CLAUDE.md invariant 10): an out-of-range ref reads as an absent
    // URL, the same as an all-ones ref.
    const Offset table = __offset + HEADER_SIZE;
    if (table > __size) return {};   // the 16-byte header itself does not fit
    const uint64_t room = (__size - table) / URL_ENTRY_SIZE;
    if (entry_idx >= entry_count(base) || entry_idx >= room) return {};

    Offset ep      = table + static_cast<Offset>(entry_idx) * URL_ENTRY_SIZE;
    Offset seg_off = LOAD_U64(base + ep + URL_ENTRY_SEG_OFFSET);
    if (seg_off == FF_NULL_OFFSET) return {};
    // __size, not 0. This block knows the stream extent -- it is a member --
    // and passing 0 told the string "you have no idea how big the buffer is",
    // which switched off the only bounds check on the payload. Live code, and
    // the URL directory is read for every Extension.url and fullUrl.
    return FF_STRING(seg_off, __size, __version).read_view(base);
}
std::string FF_URL_DIRECTORY::get_url(const BYTE* base, uint32_t entry_idx) const {
    const Offset table = __offset + HEADER_SIZE;
    if (table > __size) return {};
    const uint64_t room = (__size - table) / URL_ENTRY_SIZE;
    const uint32_t bound =
        (entry_count(base) < room) ? entry_count(base) : static_cast<uint32_t>(room);
    if (entry_idx >= bound) return {};

    // Walk the prior chain, collecting segments from leaf → root.
    // Reverse them and join with "/" between entries.
    //
    // CYCLE-GUARDED (P2). A well-formed chain has at most `bound` links, one per
    // entry; a longer walk means a forged `prior_idx` closed a loop, which the
    // unguarded `while (cur != NO_PRIOR)` spun on forever -- a hang, not a wrong
    // answer. Finding 3. The step count and the per-link index are both bounded.
    std::vector<std::string_view> segs;
    uint32_t cur = entry_idx;
    for (uint32_t steps = 0; cur != NO_PRIOR; ++steps) {
        if (steps >= bound || cur >= bound) return {};   // cycle or over-long chain
        segs.push_back(seg_string(base, cur));
        cur = prior_idx(base, cur);
    }
    // Reverse: segs[0] is leaf, segs.back() is root
    std::string result;
    for (auto it = segs.rbegin(); it != segs.rend(); ++it) {
        if (it != segs.rbegin()) result += '/';
        result.append(*it);
    }
    return result;
}

// =====================================================================
// FF_MODULE_REGISTRY — stream-level WASM codec module registry
// =====================================================================
uint32_t FF_MODULE_REGISTRY::entry_count(const BYTE* base) const {
    return LOAD_U32(base + __offset + ENTRY_COUNT);
}
uint32_t FF_MODULE_REGISTRY::url_idx(const BYTE* base, uint32_t entry_idx) const {
    Offset ep = __offset + HEADER_SIZE + static_cast<Offset>(entry_idx) * get_entry_size();
    return LOAD_U32(base + ep + REG_ENTRY_URL_IDX);
}
FF_ModuleKind FF_MODULE_REGISTRY::kind(const BYTE* base, uint32_t entry_idx) const {
    const Size es = get_entry_size();
    if (es < REG_ENTRY_SIZE) return FF_MODULE_KIND_DYNAMIC; // legacy entry: implicitly DYNAMIC
    Offset ep = __offset + HEADER_SIZE + static_cast<Offset>(entry_idx) * es;
    return static_cast<FF_ModuleKind>(LOAD_U16(base + ep + REG_ENTRY_KIND));
}
Offset FF_MODULE_REGISTRY::wasm_blob_offset(const BYTE* base, uint32_t entry_idx) const {
    Offset ep = __offset + HEADER_SIZE + static_cast<Offset>(entry_idx) * get_entry_size();
    return LOAD_U64(base + ep + REG_ENTRY_WASM_BLOB_OFFSET);
}
uint32_t FF_MODULE_REGISTRY::wasm_blob_size(const BYTE* base, uint32_t entry_idx) const {
    Offset ep = __offset + HEADER_SIZE + static_cast<Offset>(entry_idx) * get_entry_size();
    return LOAD_U32(base + ep + REG_ENTRY_WASM_BLOB_SIZE);
}
std::string_view FF_MODULE_REGISTRY::module_hash(const BYTE* base, uint32_t entry_idx) const {
    Offset ep = __offset + HEADER_SIZE + static_cast<Offset>(entry_idx) * get_entry_size();
    return std::string_view(
        reinterpret_cast<const char*>(base + ep + REG_ENTRY_MODULE_HASH),
        REG_ENTRY_HASH_SIZE);
}
std::string_view FF_MODULE_REGISTRY::schema_hash(const BYTE* base, uint32_t entry_idx) const {
    const Size es = get_entry_size();
    if (es < REG_ENTRY_SIZE) return {}; // legacy entry: no schema hash
    Offset ep = __offset + HEADER_SIZE + static_cast<Offset>(entry_idx) * es;
    return std::string_view(
        reinterpret_cast<const char*>(base + ep + REG_ENTRY_SCHEMA_HASH),
        REG_ENTRY_HASH_SIZE);
}
uint32_t FF_MODULE_REGISTRY::find_entry(const BYTE* base, uint32_t search_url_idx) const {
    uint32_t n = entry_count(base);
    if (n == 0) return FF_NULL_UINT32;
    // Binary search on url_idx; entries are written sorted ascending.
    const Size es = get_entry_size();
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        Offset ep = __offset + HEADER_SIZE + static_cast<Offset>(mid) * es;
        uint32_t mid_idx = LOAD_U32(base + ep + REG_ENTRY_URL_IDX);
        if (mid_idx == search_url_idx) return mid;
        if (mid_idx < search_url_idx) lo = mid + 1;
        else                          hi = mid;
    }
    return FF_NULL_UINT32;
}

// =====================================================================
// FF_UUID / FF_Id — the identity value types (§17)
// =====================================================================
namespace {
// A hex digit's value, or -1. LOWERCASE ONLY: the wire stores raw bytes and
// the renderer always emits lowercase, so accepting an uppercase spelling
// here would let a value be packed that does not render back byte-identically
// -- the one thing §17.4 forbids. Uppercase text routes to RAW_STRING instead.
int hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// The 7-byte little-endian offset at bytes 9..15. FastFHIR is strictly little-
// endian on the wire (FF_Ops.hpp), so these assemble bytes the same way
// FF_GET_VALIDATION does in the header.
void store_offset7(BYTE* dst, uint64_t v) noexcept {
    for (int i = 0; i < 7; ++i) dst[i] = static_cast<BYTE>((v >> (8 * i)) & 0xFF);
}
uint64_t load_offset7(const BYTE* src) noexcept {
    uint64_t v = 0;
    for (int i = 6; i >= 0; --i) v = (v << 8) | src[i];
    return v;
}
}  // namespace

FF_UUID::FF_UUID() noexcept {
    std::memset(bytes, 0xFF, sizeof(bytes));
}

FF_UUID::FF_UUID(std::string_view hex) {
    // Canonical form only: an optional `urn:uuid:` prefix, then 8-4-4-4-12.
    constexpr std::string_view PREFIX = "urn:uuid:";
    if (hex.size() >= PREFIX.size() && hex.substr(0, PREFIX.size()) == PREFIX)
        hex.remove_prefix(PREFIX.size());
    // 36 characters: 32 lowercase hex digits with hyphens at 8, 13, 18, 23.
    if (hex.size() != 36 ||
        hex[8] != '-' || hex[13] != '-' || hex[18] != '-' || hex[23] != '-')
        throw std::invalid_argument(
            "FF_UUID: not a canonical 8-4-4-4-12 UUID: " + std::string(hex));
    std::size_t out = 0;
    for (std::size_t i = 0; i < 36; ) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { ++i; continue; }
        const int hi = hex_nibble(hex[i]);
        const int lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            throw std::invalid_argument(
                "FF_UUID: non-lowercase-hex digit in: " + std::string(hex));
        bytes[out++] = static_cast<BYTE>((hi << 4) | lo);
        i += 2;
    }
}

bool FF_UUID::operator==(const FF_UUID& other) const noexcept {
    return std::memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
}

FF_Id FF_Id::generated(Offset block_offset) noexcept {
    FF_Id id;
    id.m_offset = block_offset;
    id.m_form   = Form::GENERATED;
    return id;
}
FF_Id FF_Id::interned(uint32_t trie_index) noexcept {
    FF_Id id;
    id.m_index = trie_index;
    id.m_form  = Form::INTERNED;
    return id;
}
FF_Id FF_Id::raw_string(Offset string_offset) noexcept {
    FF_Id id;
    id.m_offset = string_offset;
    id.m_form   = Form::RAW_STRING;
    return id;
}
FF_Id FF_Id::pending() noexcept {
    FF_Id id;
    id.m_form = Form::PENDING;
    return id;
}

const FF_UUID& FF_Id::uuid() const {
    if (m_form != Form::UUID)
        throw std::runtime_error("FastFHIR: FF_Id::uuid() on a value that is not a UUID; "
                                 "test form() first");
    return m_uuid;
}
uint32_t FF_Id::index() const {
    if (m_form != Form::INTERNED)
        throw std::runtime_error("FastFHIR: FF_Id::index() on a value that is not INTERNED; "
                                 "test form() first");
    return m_index;
}
Offset FF_Id::raw_string_offset() const {
    if (m_form != Form::RAW_STRING)
        throw std::runtime_error("FastFHIR: FF_Id::raw_string_offset() on a value that is not "
                                 "RAW_STRING; test form() first");
    return m_offset;
}

FF_Id FF_Id::read_slot(const BYTE* slot) noexcept {
    // Test 1 FIRST. All-ones is the absence convention every sentinel follows,
    // and an all-ones slot would otherwise pass test 3 and read as a UUID whose
    // version nibble is 15 and whose variant bits are 11. The sentinel and the
    // test for it are spelled in the same domain -- here, raw bytes (§17.2).
    bool all_ones = true;
    for (Size i = 0; i < FF_IdSlot::WIDTH; ++i)
        if (slot[i] != 0xFF) { all_ones = false; break; }
    if (all_ones) return FF_Id{};   // ABSENT

    // Test 2: the check byte, AND a zero version byte. A kind value below 256
    // written little-endian leaves byte 6 zero, while an RFC 9562 UUID's
    // version nibble (byte 6's high nibble, values 1..8) never is; and the check
    // byte 0x01 sits outside a conformant UUID's variant range (0x80..0xBF).
    // Both would have to be wrong at once, which the write-side classifier
    // guarantees can never happen to text it packs.
    if (slot[FF_IdSlot::CHECK_BYTE] == FF_IdSlot::CHECK_VALUE && slot[6] == 0x00) {
        const uint32_t kind = static_cast<uint32_t>(slot[4])
                            | (static_cast<uint32_t>(slot[5]) << 8)
                            | (static_cast<uint32_t>(slot[6]) << 16)
                            | (static_cast<uint32_t>(slot[7]) << 24);
        switch (kind) {
        case FF_IdSlot::KIND_GENERATED:
            return FF_Id::generated(load_offset7(slot + FF_IdSlot::OFFSET_BYTES));
        case FF_IdSlot::KIND_INTERNED:
            return FF_Id::interned(static_cast<uint32_t>(slot[0])
                                 | (static_cast<uint32_t>(slot[1]) << 8)
                                 | (static_cast<uint32_t>(slot[2]) << 16)
                                 | (static_cast<uint32_t>(slot[3]) << 24));
        case FF_IdSlot::KIND_RAW_STRING:
            return FF_Id::raw_string(load_offset7(slot + FF_IdSlot::OFFSET_BYTES));
        case FF_IdSlot::KIND_PENDING:
        default:
            // A placeholder that survived a seal, or a kind this build does not
            // know. Both are PENDING: distinct from absent, so a caller can see
            // it and fail loudly rather than treating it as a dropped field.
            return FF_Id::pending();
        }
    }

    // Test 3: sixteen inline UUID bytes.
    FF_UUID u;
    std::memcpy(u.bytes, slot, FF_IdSlot::WIDTH);
    return FF_Id(u);
}

void FF_Id::write_slot(BYTE* slot) const {
    switch (m_form) {
    case Form::ABSENT:
        std::memset(slot, 0xFF, FF_IdSlot::WIDTH);
        return;
    case Form::UUID:
        std::memcpy(slot, m_uuid.bytes, FF_IdSlot::WIDTH);
        return;
    case Form::INTERNED:
        std::memset(slot, 0, FF_IdSlot::WIDTH);
        STORE_U32(slot, m_index);                                          // bytes 0..3
        STORE_U32(slot + FF_IdSlot::KIND_WORD, FF_IdSlot::KIND_INTERNED);  // bytes 4..7
        slot[FF_IdSlot::CHECK_BYTE] = FF_IdSlot::CHECK_VALUE;
        return;
    case Form::GENERATED:
    case Form::RAW_STRING:
        std::memset(slot, 0, FF_IdSlot::WIDTH);
        STORE_U32(slot + FF_IdSlot::KIND_WORD,
                  m_form == Form::GENERATED ? FF_IdSlot::KIND_GENERATED
                                            : FF_IdSlot::KIND_RAW_STRING);
        slot[FF_IdSlot::CHECK_BYTE] = FF_IdSlot::CHECK_VALUE;
        store_offset7(slot + FF_IdSlot::OFFSET_BYTES, m_offset);
        return;
    case Form::PENDING:
        // Kind 0, the reserved placeholder. The check byte still marks it a
        // pointer form, so a reader sees a placeholder rather than a UUID.
        std::memset(slot, 0, FF_IdSlot::WIDTH);
        slot[FF_IdSlot::CHECK_BYTE] = FF_IdSlot::CHECK_VALUE;
        return;
    case Form::POINTER:
        throw std::logic_error("FastFHIR: FF_Id::write_slot() on a POINTER form; resolve it "
                               "at append before encoding (§17.11)");
    }
}
