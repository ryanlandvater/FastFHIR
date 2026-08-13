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

// =====================================================================
// DATA_BLOCK BASE VALIDATION
// =====================================================================
FF_Result DATA_BLOCK::validate_offset(const BYTE *const __base, const char* type_name, RECOVERY_TAG recovery_tag) const noexcept {
    if (!*this) {
        return {FF_VALIDATION_FAILURE, std::string("Invalid ") + type_name + ". Offset is NULL."};
    }

#ifndef __EMSCRIPTEN__
    if (LOAD_U64(__base + __offset + VALIDATION) != __offset) {
        return {FF_VALIDATION_FAILURE, std::string(type_name) + " failed absolute offset validation."};
    }
#else
    if (LOAD_U64(__base + __offset + VALIDATION) != __remote) {
        return {FF_VALIDATION_FAILURE, std::string(type_name) + " failed remote offset validation."};
    }
#endif

    RECOVERY_TAG actual_recovery = static_cast<RECOVERY_TAG>(LOAD_U16(__base + __offset + RECOVERY));
    if (actual_recovery != recovery_tag) {
        return {FF_VALIDATION_FAILURE, std::string(type_name) + " recovery tag mismatch. Expected: " + std::to_string(recovery_tag) + ", Found: " + std::to_string(actual_recovery)};
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
    if (LOAD_U32(__base + MAGIC) != FF_MAGIC_BYTES) {
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
    Offset checksum_off = LOAD_U64(__base + CHECKSUM_OFFSET);
    if (checksum_off != FF_NULL_OFFSET) {
        FF_CHECKSUM checksum(checksum_off, __size, fhir_rev, engine_ver);
        auto checksum_result = checksum.validate_full(__base);
        if (checksum_result != FF_SUCCESS) return checksum_result;
    }

    // 5. Cache the decoded engine version for downstream get_header_size() calls.
    const_cast<FF_HEADER*>(this)->__engine_version = engine_ver;

    return {FF_SUCCESS, ""};
}

// Accessors correctly split between Engine Version and FHIR Schema
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
    
    uint32_t count = LOAD_U32(__base + __offset + ENTRY_COUNT);
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
{ return LOAD_U32(__base + __offset + ENTRY_COUNT); }

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
    uint32_t len = LOAD_U32(__base + __offset + LENGTH);
    if (__offset + get_header_size() + len > __size) {
        return {FF_VALIDATION_FAILURE, "FF_STRING length exceeds file boundaries."};
    }
    return {FF_SUCCESS, ""};
}
// Zero-Copy Mapped View
std::string_view FF_STRING::read_view(const BYTE* const __base) const {
    uint32_t len = LOAD_U32(__base + __offset + LENGTH);
    const char* str_start = reinterpret_cast<const char*>(__base + __offset + STRING_DATA);
    return std::string_view(str_start, len);
}

// Fallback std::string allocation for dictionary parsers
std::string FF_STRING::read(const BYTE* const __base) const {
    return std::string(read_view(__base));
}

// =====================================================================
// LOCK-FREE STRING & MSB DICTIONARY EMITTERS
// =====================================================================
Size SIZE_FF_STRING(std::string_view str) {
    if (str.empty()) return 0;
    return FF_STRING::HEADER_SIZE + str.size();
}
Size SIZE_FF_CODE(std::string_view code_str, uint32_t version = FHIR_VERSION_R5) {
    if (code_str.empty()) return 0;
    if (FF_GetDictionaryCode(std::string(code_str), version) != FF_CODE_NULL) return 0;
    return SIZE_FF_STRING(code_str);
}
Offset STORE_FF_CODE(BYTE* const __base, Offset start_offset, std::string_view code_str, uint32_t version) {
    // If the code is in the dictionary, it's stored inline as a uint32_t
    // in the vtable slot (4 bytes, already counted in the block header stride).
    // If not found in the dictionary, store as a custom FF_STRING.
    if (FF_GetDictionaryCode(std::string(code_str), version) != FF_CODE_NULL)
        return 0; // Dictionary code — inline, nothing extra to store
    return STORE_FF_STRING(__base, start_offset, code_str);
}

Size STORE_FF_STRING(BYTE* const __base, Offset start_offset, std::string_view str) {
    auto __ptr = __base + start_offset;
    uint32_t length = static_cast<uint32_t>(str.size());
    
    STORE_U64(__ptr + DATA_BLOCK::VALIDATION, start_offset);
    STORE_U16(__ptr + DATA_BLOCK::RECOVERY,   RECOVER_FF_STRING);
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
    return (static_cast<uint32_t>(static_cast<int32_t>(rel)) & 0x7FFFFFFFu) | FF_CODEABLE_CONCEPT_FLAG;
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

// Write the FF_CODEABLE_CONCEPT header and advance the child-offset cursor.
// Every system branch in ENCODE_FF_CODE shares this — previously copy-pasted
// 7 times. If the CC layout changes, this is the ONE place to update.
static void write_cc_header_(BYTE* ptr, Offset cc_offset, Offset& child_off,
                             FF_CodeableConceptSystem system, uint8_t payload_len)
{
    child_off += FF_CODEABLE_CONCEPT::HEADER_SIZE + payload_len;
    STORE_U64(ptr + FF_CODEABLE_CONCEPT::VALIDATION, cc_offset);
    STORE_U16(ptr + FF_CODEABLE_CONCEPT::RECOVERY,   RECOVER_FF_CODEABLE_CONCEPT);
    ptr[FF_CODEABLE_CONCEPT::SYSTEM] = static_cast<uint8_t>(system);
    ptr[FF_CODEABLE_CONCEPT::LENGTH] = payload_len;
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
        ff_cc_store(ptr + FF_CODEABLE_CONCEPT::PAYLOAD, value, codec->payload_bytes);
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
        STORE_U16(ptr + FF_CODEABLE_CONCEPT::PAYLOAD, uint16_t{0});  // 0 = not yet registered
        std::memcpy(ptr + FF_CODEABLE_CONCEPT::PAYLOAD + URL_IDX_BYTES,
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
    std::memcpy(ptr + FF_CODEABLE_CONCEPT::PAYLOAD, code_str.data(), code_str.size());
    return _pack_codeable_concept_offset(cc_offset, block_offset);
}

// =====================================================================
// FF_DECODE_CODEABLE_CONCEPT — unified dynamic fallback block decoder
// =====================================================================
// Reads the SYSTEM discriminator byte and variable-length PAYLOAD from
// the unified FF_CODEABLE_CONCEPT.  Returns the code as a human-readable
// string_view (thread-local buffer for fixed-width types).
FF_CodeableConceptResult FF_DECODE_CODEABLE_CONCEPT(
    const BYTE* base, Offset offset, uint32_t version)
{
    using S = FF_CodeableConceptSystem;
    const S sys = static_cast<S>(base[offset + FF_CODEABLE_CONCEPT::SYSTEM]);
    const uint8_t len = base[offset + FF_CODEABLE_CONCEPT::LENGTH];
    const BYTE *payload = base + offset + FF_CODEABLE_CONCEPT::PAYLOAD;

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
std::string_view FF_URL_DIRECTORY::seg_string(const BYTE* base, uint32_t entry_idx) const {
    Offset ep      = __offset + HEADER_SIZE + static_cast<Offset>(entry_idx) * URL_ENTRY_SIZE;
    Offset seg_off = LOAD_U64(base + ep + URL_ENTRY_SEG_OFFSET);
    if (seg_off == FF_NULL_OFFSET) return {};
    return FF_STRING(seg_off, 0, __version).read_view(base);
}
std::string FF_URL_DIRECTORY::get_url(const BYTE* base, uint32_t entry_idx) const {
    // Walk the prior chain, collecting segments from leaf → root.
    // Reverse them and join with "/" between entries.
    std::vector<std::string_view> segs;
    uint32_t cur = entry_idx;
    while (cur != NO_PRIOR) {
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
