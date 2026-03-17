// MARK: - FastFHIR Core Primitives
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include <stdexcept>

// Ensure FF_EXPORT is defined (adjust to your actual export macro)
#ifndef FF_EXPORT
#define FF_EXPORT
#endif

// =====================================================================
// CORE TYPES & CONSTANTS
// =====================================================================
typedef uint8_t  BYTE;
typedef uint64_t Offset;
typedef uint64_t Size;

constexpr Offset FF_NULL_OFFSET = 0xFFFFFFFFFFFFFFFF;
constexpr uint32_t FF_CUSTOM_STRING_FLAG = 0x80000000;

#define FHIR_VERSION_R4 0x0400
#define FHIR_VERSION_R5 0x0500

enum FF_Result_Code {
    FF_SUCCESS = 0,
    FF_FAILURE = 1,
    FF_VALIDATION_FAILURE = 2,
    FF_WARNING = 4
};

struct Result {
    uint32_t code;
    std::string message;
    inline operator bool() const { return code == FF_SUCCESS; }
    inline bool operator&(uint32_t flag) const { return (code & flag) != 0; }
    inline bool operator!=(uint32_t flag) const { return code != flag; }
};

// =====================================================================
// MEMORY ALIGNMENT & UNALIGNED ACCESS
// =====================================================================
inline Offset align_up(Offset offset, uint32_t alignment) {
    return (offset + (alignment - 1)) & ~(alignment - 1);
}

// Safe unaligned memory access (compiles to efficient instructions on x86, prevents traps on ARM/Wasm)
template<typename T> inline T _LOAD(const BYTE* ptr) {
    T val; std::memcpy(&val, ptr, sizeof(T)); return val;
}
template<typename T> inline void _STORE(BYTE* ptr, const T& val) {
    std::memcpy(ptr, &val, sizeof(T));
}

#define LOAD_U8(ptr)   _LOAD<uint8_t>(ptr)
#define LOAD_U16(ptr)  _LOAD<uint16_t>(ptr)
#define LOAD_U32(ptr)  _LOAD<uint32_t>(ptr)
#define LOAD_U64(ptr)  _LOAD<uint64_t>(ptr)
#define LOAD_F32(ptr)  _LOAD<float>(ptr)
#define LOAD_F64(ptr)  _LOAD<double>(ptr)

#define STORE_U8(ptr, v)   _STORE<uint8_t>(ptr, v)
#define STORE_U16(ptr, v)  _STORE<uint16_t>(ptr, v)
#define STORE_U32(ptr, v)  _STORE<uint32_t>(ptr, v)
#define STORE_U64(ptr, v)  _STORE<uint64_t>(ptr, v)
#define STORE_F32(ptr, v)  _STORE<float>(ptr, v)
#define STORE_F64(ptr, v)  _STORE<double>(ptr, v)

// =====================================================================
// BASE DATA BLOCK
// =====================================================================
struct FF_EXPORT DATA_BLOCK {
    enum vtable_sizes   { VALIDATION_S = 8, RECOVERY_S = 2 };
    enum vtable_offsets { VALIDATION = 0, RECOVERY = 8, HEADER_SIZE = 10 };

#ifdef __EMSCRIPTEN__
    Offset      __remote   = NULL_OFFSET;
    void* __response = nullptr; // Abstracting your Fetch API response type
#endif

    Offset      __offset   = NULL_OFFSET;
    Size        __size     = 0;
    uint32_t    __version  = 0;

    explicit DATA_BLOCK() = default;
    explicit DATA_BLOCK(Offset off, Size total_size, uint32_t ver) 
        : __offset(off), __size(total_size), __version(ver) {}

    operator bool() const { return __offset != NULL_OFFSET; }

    Result validate_offset(const BYTE* const __base, const char* type_name, uint16_t recovery_tag) const noexcept;
    
#ifdef __EMSCRIPTEN__
    void check_and_fetch_remote(const BYTE* const& __base);
#endif
};

// =====================================================================
// STRING HANDLING
// =====================================================================
// Recovery tag for custom strings stored outside the dictionary
constexpr uint16_t RECOVER_FF_STRING = 0xFFFF; 

struct FF_EXPORT FF_STRING : DATA_BLOCK {
    static constexpr char type [] = "FF_STRING";
    static constexpr uint16_t recovery = RECOVER_FF_STRING;

    enum vtable_offsets {
        VALIDATION = 0, 
        RECOVERY = 8, 
        LENGTH = 10,  // uint32_t
        STRING_DATA = 14,
        HEADER_SIZE = 14
    };

    explicit FF_STRING(Offset off, Size size, uint32_t ver) : DATA_BLOCK(off, size, ver) {}
    
    Result validate_full(const BYTE* const __base) const noexcept;
    std::string read(const BYTE* const __base) const;
};

// Emitter Signatures
Offset STORE_FF_STRING(BYTE* const __base, Offset& write_head, const std::string& str);
uint32_t ENCODE_FF_CODE(BYTE* const __base, Offset block_offset, Offset& write_head, const std::string& code_str);