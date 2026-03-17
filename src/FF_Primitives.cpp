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
    // Implement your asynchronous Fetch API logic here
    // e.g., if (!__response) { ... FETCH_DATABLOCK(...) }
}
#endif

// =====================================================================
// FILE HEADER IMPLEMENTATION
// =====================================================================
FF_Result FF_FILE_HEADER::validate_full(const BYTE* const __base) const noexcept {
    auto result = validate_offset(__base, type, recovery);
    if (result != FF_SUCCESS) return result;

    if (LOAD_U32(__base + MAGIC) != FF_MAGIC_BYTES) {
        return {FF_VALIDATION_FAILURE, "FF_FILE_HEADER magic bytes mismatch."};
    }

    uint32_t ver = LOAD_U32(__base + VERSION);
    if (ver < FHIR_VERSION_R4) {
        return {FF_VALIDATION_FAILURE, "FF_FILE_HEADER unsupported FHIR version."};
    }

    return {FF_SUCCESS, ""};
}

uint32_t FF_FILE_HEADER::get_version(const BYTE* const __base) const {
    return LOAD_U32(__base + VERSION);
}

Offset FF_FILE_HEADER::get_root(const BYTE* const __base) const {
    return LOAD_U64(__base + ROOT_OFFSET);
}

uint16_t FF_FILE_HEADER::get_root_type(const BYTE* const __base) const {
    return LOAD_U16(__base + ROOT_RECOVERY);
}

void STORE_FF_FILE_HEADER(BYTE* const __base, uint32_t version,
                           Offset root_offset, uint16_t root_recovery,
                           Size payload_size) {
    STORE_U64(__base + FF_FILE_HEADER::VALIDATION, 0);
    STORE_U16(__base + FF_FILE_HEADER::RECOVERY,   RECOVER_FF_FILE_HEADER);
    STORE_U32(__base + FF_FILE_HEADER::MAGIC,      FF_MAGIC_BYTES);
    STORE_U32(__base + FF_FILE_HEADER::VERSION,    version);
    STORE_U64(__base + FF_FILE_HEADER::ROOT_OFFSET, root_offset);
    STORE_U16(__base + FF_FILE_HEADER::ROOT_RECOVERY, root_recovery);
    STORE_U64(__base + FF_FILE_HEADER::PAYLOAD_SIZE, payload_size);
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

uint16_t FF_ARRAY::entry_step(const BYTE* const __base) const {
    return LOAD_U16(__base + __offset + ENTRY_STEP);
}

uint32_t FF_ARRAY::entry_count(const BYTE* const __base) const {
    return LOAD_U32(__base + __offset + ENTRY_COUNT);
}

const BYTE* FF_ARRAY::entries(const BYTE* const __base) const {
    return __base + __offset + HEADER_SIZE;
}

void STORE_FF_ARRAY_HEADER(BYTE* const __base, Offset& write_head,
                            uint16_t entry_step, uint32_t entry_count) {
    auto __ptr = __base + write_head;
    STORE_U64(__ptr + FF_ARRAY::VALIDATION, write_head);
    STORE_U16(__ptr + FF_ARRAY::RECOVERY,   RECOVER_FF_ARRAY);
    STORE_U16(__ptr + FF_ARRAY::ENTRY_STEP, entry_step);
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
    if (__offset + HEADER_SIZE + len > __size) {
        return {FF_VALIDATION_FAILURE, "FF_STRING length exceeds file boundaries."};
    }
    return {FF_SUCCESS, ""};
}

std::string FF_STRING::read(const BYTE* const __base) const {
    FF_Result res = validate_full(__base);
    if (res != FF_SUCCESS) throw std::runtime_error(res.message);

    uint32_t len = LOAD_U32(__base + __offset + LENGTH);
    const char* str_start = reinterpret_cast<const char*>(__base + __offset + STRING_DATA);
    return std::string(str_start, len);
}

// =====================================================================
// STRING & MSB DICTIONARY EMITTERS
// =====================================================================
Offset STORE_FF_STRING(BYTE* const __base, Offset& write_head, const std::string& str) {
    Offset start_offset = write_head;
    auto __ptr = __base + start_offset;
    
    uint32_t length = static_cast<uint32_t>(str.size());
    
    STORE_U64(__ptr + DATA_BLOCK::VALIDATION, start_offset);
    STORE_U16(__ptr + DATA_BLOCK::RECOVERY,   RECOVER_FF_STRING);
    STORE_U32(__ptr + FF_STRING::LENGTH,      length);
    
    std::memcpy(__ptr + FF_STRING::STRING_DATA, str.data(), length);
    
    write_head += align_up(FF_STRING::HEADER_SIZE + length, 8); 
    return start_offset;
}

uint32_t ENCODE_FF_CODE(BYTE* const __base, Offset block_offset, Offset& write_head, const std::string& code_str, uint32_t version) {
    if (code_str.empty()) return 0; // FF_CODE_NULL

    uint32_t dict_code = FF_GetDictionaryCode(code_str, version);
    if (dict_code != 0) {
        return dict_code; // MSB is 0
    }

    // Custom String Fallback
    Offset string_offset = STORE_FF_STRING(__base, write_head, code_str);
    Offset relative_offset = string_offset - block_offset;
    
    if (relative_offset > 0x7FFFFFFF) {
        throw std::runtime_error("FastFHIR: Custom string relative offset exceeds 2GB.");
    }
    
    // Apply the 0x80000000 mask
    return static_cast<uint32_t>(relative_offset) | FF_CUSTOM_STRING_FLAG;
}
