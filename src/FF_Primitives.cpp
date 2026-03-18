/**
 * @file FF_Primitives.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @brief Implementation of FastFHIR Core Primitives and Data Structures
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
#include "../include/FF_Utilities.hpp"
#include "../include/FF_Primitives.hpp"
#include "../generated_src/FF_Dictionary.hpp"

// =====================================================================
// DATA_BLOCK BASE VALIDATION
// =====================================================================
FF_Result DATA_BLOCK::validate_offset(const BYTE *const __base, const char* type_name, uint16_t recovery_tag) const noexcept {
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

    if (LOAD_U16(__base + __offset + RECOVERY) != recovery_tag) {
        return {FF_VALIDATION_FAILURE, std::string(type_name) + " failed recovery tag validation."};
    }

    return {FF_SUCCESS, ""};
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
DATA_BLOCK (0, file_size, UINT32_MAX) {
    
}
FF_Result FF_HEADER::validate_full(const BYTE* const __base) const noexcept {
    if (LOAD_U32(__base + MAGIC) != FF_MAGIC_BYTES) {
        return {FF_VALIDATION_FAILURE, "FF_HEADER magic bytes mismatch."};
    }
    uint32_t ver = LOAD_U32(__base + VERSION);
    if (ver < FHIR_VERSION_R4) {
        return {FF_VALIDATION_FAILURE, "FF_HEADER unsupported FHIR version."};
    }

    Offset checksum_off = LOAD_U64(__base + CHECKSUM_OFFSET);
    if (checksum_off != FF_NULL_OFFSET) {
        FF_CHECKSUM checksum(checksum_off, __size, ver);
        auto checksum_result = checksum.validate_full(__base);
        if (checksum_result != FF_SUCCESS) return checksum_result;
    }
    return {FF_SUCCESS, ""};
}

uint32_t FF_HEADER::get_version(const BYTE* const __base) const { return LOAD_U32(__base + VERSION); }
FF_CHECKSUM FF_HEADER::get_checksum(const BYTE* const __base) const {
    auto checksum = FF_CHECKSUM(LOAD_U64(__base + CHECKSUM_OFFSET), __size, get_version(__base));
    if (!checksum) return checksum;
    auto result = checksum.validate_offset(__base, FF_CHECKSUM::type, FF_CHECKSUM::recovery);
    if (result != FF_SUCCESS) throw std::runtime_error("Failed to retrieve checksum: " + result.message);
    return checksum;
}
Offset FF_HEADER::get_root(const BYTE* const __base) const { return LOAD_U64(__base + ROOT_OFFSET); }
uint16_t FF_HEADER::get_root_type(const BYTE* const __base) const { return LOAD_U16(__base + ROOT_RECOVERY); }

void STORE_FF_HEADER(BYTE* const __base, uint32_t version, Offset checksum_offset, Offset root_offset, uint16_t root_recovery, Size payload_size) {
    STORE_U32(__base + FF_HEADER::MAGIC, FF_MAGIC_BYTES);
    STORE_U16(__base + FF_HEADER::RECOVERY, RECOVER_FF_HEADER);
    STORE_U32(__base + FF_HEADER::VERSION, version);
    STORE_U64(__base + FF_HEADER::CHECKSUM_OFFSET, checksum_offset);
    STORE_U64(__base + FF_HEADER::ROOT_OFFSET, root_offset);
    STORE_U16(__base + FF_HEADER::ROOT_RECOVERY, root_recovery);
    STORE_U64(__base + FF_HEADER::PAYLOAD_SIZE, payload_size);
}

// =====================================================================
// FIXED-SIZE CHECKSUM IMPLEMENTATION
// =====================================================================
FF_Result FF_CHECKSUM::validate_full(const BYTE* const __base) const noexcept {
    auto result = validate_offset(__base, type, recovery);
    if (!result) return result;
    
    // Since it's a fixed-size block, we just ensure it doesn't overflow the file buffer
    if (__offset + HEADER_SIZE > __size) {
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
    STORE_U32(__ptr + FF_CHECKSUM::PADDING,   0); // Zero out alignment padding
    
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
    auto result = validate_offset(__base, type, recovery);
    if (result != FF_SUCCESS) return result;
    uint16_t step = LOAD_U16(__base + __offset + ENTRY_STEP);
    uint32_t count = LOAD_U32(__base + __offset + ENTRY_COUNT);
    if (__offset + HEADER_SIZE + static_cast<uint64_t>(step) * count > __size) {
        return {FF_VALIDATION_FAILURE, "FF_ARRAY entries exceed file boundaries."};
    }
    return {FF_SUCCESS, ""};
}

uint16_t FF_ARRAY::entry_step(const BYTE* const __base) const { return LOAD_U16(__base + __offset + ENTRY_STEP); }
uint32_t FF_ARRAY::entry_count(const BYTE* const __base) const { return LOAD_U32(__base + __offset + ENTRY_COUNT); }
const BYTE* FF_ARRAY::entries(const BYTE* const __base) const { return __base + __offset + HEADER_SIZE; }

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
    if (__offset + HEADER_SIZE + len > __size) {
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
// GENERIC RESOURCE WRAPPER
// =====================================================================
FF_Result FF_RESOURCE::validate_full(const BYTE* const __base) const noexcept {
    auto result = validate_offset(__base, type, recovery);
    if (result != FF_SUCCESS) return result;

    uint16_t child_recovery = LOAD_U16(__base + __offset + FF_RESOURCE::PAYLOAD_RECOVERY);
    Offset child_off = LOAD_U64(__base + __offset + FF_RESOURCE::PAYLOAD_OFFSET);

    if (child_off == FF_NULL_OFFSET || child_off >= __size) {
        return {FF_VALIDATION_FAILURE, "FF_RESOURCE payload offset is invalid."};
    }
    if (child_recovery == RECOVER_FF_STRING) {
        FF_STRING s(child_off, __size, __version);
        return s.validate_full(__base);
    }
    return {FF_WARNING, "FF_RESOURCE payload recovery is not yet supported for typed dispatch."};
}

ResourceData FF_RESOURCE::read(const BYTE* const __base) const {
    ResourceData data;
    data.resourceType = "Resource";
    data.payloadRecovery = LOAD_U16(__base + __offset + FF_RESOURCE::PAYLOAD_RECOVERY);
    Offset child_off = LOAD_U64(__base + __offset + FF_RESOURCE::PAYLOAD_OFFSET);

    if (data.payloadRecovery == RECOVER_FF_STRING && child_off != FF_NULL_OFFSET) {
        FF_STRING s(child_off, __size, __version);
        data.json = s.read(__base);
    }
    return data;
}

void STORE_FF_RESOURCE(BYTE* const __base, Offset entry_off, Offset& write_head, const ResourceData& data) {
    auto __ptr = __base + entry_off;
    STORE_U64(__ptr + DATA_BLOCK::VALIDATION, entry_off);
    STORE_U16(__ptr + DATA_BLOCK::RECOVERY, RECOVER_FF_RESOURCE);

    uint16_t child_recovery = data.payloadRecovery;
    if (child_recovery == RECOVER_FF_STRING) {
        Offset payload_off = write_head;
        STORE_U16(__ptr + FF_RESOURCE::PAYLOAD_RECOVERY, RECOVER_FF_STRING);
        STORE_U64(__ptr + FF_RESOURCE::PAYLOAD_OFFSET, payload_off);
        write_head += STORE_FF_STRING(__base, write_head, data.json);
        return;
    }
    STORE_U16(__ptr + FF_RESOURCE::PAYLOAD_RECOVERY, RECOVER_FF_STRING);
    STORE_U64(__ptr + FF_RESOURCE::PAYLOAD_OFFSET, write_head);
    write_head += STORE_FF_STRING(__base, write_head, data.json);
}

// =====================================================================
// LOCK-FREE STRING & MSB DICTIONARY EMITTERS
// =====================================================================

Size SIZE_FF_CODE(std::string_view code_str) {
    if (code_str.empty()) return 0;
    // Fallback std::string allocation for dictionary check
    uint32_t dict_code = FF_GetDictionaryCode(std::string(code_str), FHIR_VERSION_R5);
    if (dict_code != 0) return 0; // Fits inside the 32-bit slot
    return align_up(FF_STRING::HEADER_SIZE + code_str.size(), 8);
}

Size STORE_FF_STRING(BYTE* const __base, Offset start_offset, std::string_view str) {
    auto __ptr = __base + start_offset;
    uint32_t length = static_cast<uint32_t>(str.size());
    
    STORE_U64(__ptr + DATA_BLOCK::VALIDATION, start_offset);
    STORE_U16(__ptr + DATA_BLOCK::RECOVERY,   RECOVER_FF_STRING);
    STORE_U32(__ptr + FF_STRING::LENGTH,      length);
    
    std::memcpy(__ptr + FF_STRING::STRING_DATA, str.data(), length);
    
    return align_up(FF_STRING::HEADER_SIZE + length, 8); 
}

uint32_t ENCODE_FF_CODE(BYTE* const __base, Offset block_offset, Offset& child_off, const std::string& code_str, uint32_t version) {
    if (code_str.empty()) return 0; // FF_CODE_NULL

    uint32_t dict_code = FF_GetDictionaryCode(code_str, version);
    if (dict_code != 0) {
        return dict_code; // MSB is 0
    }

    // Custom String Fallback
    Offset string_offset = child_off;
    child_off += STORE_FF_STRING(__base, string_offset, code_str);
    
    Offset relative_offset = string_offset - block_offset;
    if (relative_offset > 0x7FFFFFFF) {
        throw std::runtime_error("FastFHIR: Custom string relative offset exceeds 2GB.");
    }
    return static_cast<uint32_t>(relative_offset) | FF_CUSTOM_STRING_FLAG;
}
