// MARK: - FastFHIR Core Primitives Implementation
#include "FF_Primitives.hpp"
#include "FF_Dictionary.hpp" // Needs FF_GetDictionaryCode

// =====================================================================
// DATA_BLOCK BASE VALIDATION
// =====================================================================
Result DATA_BLOCK::validate_offset(const BYTE *const __base, const char* type_name, uint16_t recovery_tag) const noexcept {
    if (!*this) {
        return {IRIS_VALIDATION_FAILURE, std::string("Invalid ") + type_name + ". Offset is NULL."};
    }

#ifndef __EMSCRIPTEN__
    if (LOAD_U64(__base + __offset + VALIDATION) != __offset) {
        return {IRIS_VALIDATION_FAILURE, std::string(type_name) + " failed absolute offset validation."};
    }
#else
    if (LOAD_U64(__base + __offset + VALIDATION) != __remote) {
        return {IRIS_VALIDATION_FAILURE, std::string(type_name) + " failed remote offset validation."};
    }
#endif

    if (LOAD_U16(__base + __offset + RECOVERY) != recovery_tag) {
        return {IRIS_VALIDATION_FAILURE, std::string(type_name) + " failed recovery tag validation."};
    }

    return {IRIS_SUCCESS, ""};
}

#ifdef __EMSCRIPTEN__
void DATA_BLOCK::check_and_fetch_remote(const BYTE *const &base) {
    // Implement your asynchronous Fetch API logic here
    // e.g., if (!__response) { ... FETCH_DATABLOCK(...) }
}
#endif

// =====================================================================
// STRING BLOCK IMPLEMENTATION
// =====================================================================
Result FF_STRING::validate_full(const BYTE* const __base) const noexcept {
#ifdef __EMSCRIPTEN__
    const_cast<FF_STRING&>(*this).check_and_fetch_remote(__base);
#endif
    auto result = validate_offset(__base, type, recovery);
    if (result != IRIS_SUCCESS) return result;

    uint32_t len = LOAD_U32(__base + __offset + LENGTH);
    if (__offset + HEADER_SIZE + len > __size) {
        return {IRIS_VALIDATION_FAILURE, "FF_STRING length exceeds file boundaries."};
    }
    return {IRIS_SUCCESS, ""};
}

std::string FF_STRING::read(const BYTE* const __base) const {
    Result res = validate_full(__base);
    if (res != IRIS_SUCCESS) throw std::runtime_error(res.message);

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

uint32_t ENCODE_FF_CODE(BYTE* const __base, Offset block_offset, Offset& write_head, const std::string& code_str) {
    if (code_str.empty()) return 0; // FF_CODE_NULL

    // Try the Dictionary (assuming FF_GetDictionaryCode returns 0 if not found)
    uint32_t dict_code = FF_GetDictionaryCode(code_str);
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