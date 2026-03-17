// MARK: - FastFHIR / IFE Core Utilities
#pragma once

#include <bit>
#include <limits>
#include <cstdint>
#include <cstring>
#include <bit>
#include <limits>
#include <type_traits>
#include <string>

// =====================================================================
// ARCHITECTURE & COMPILER FLAGS
// =====================================================================
constexpr bool little_endian = std::endian::native == std::endian::little;
constexpr bool is_ieee754    = std::numeric_limits<float>::is_iec559;

// FastFHIR specific constants
constexpr uint64_t FF_NULL_OFFSET = 0xFFFFFFFFFFFFFFFF;
constexpr uint32_t FF_CUSTOM_STRING_FLAG = 0x80000000;

// Standard 8-byte alignment for IFE/FastFHIR blocks
inline uint64_t ff_align(uint64_t off) { return (off + 7) & ~7; }

// =====================================================================
// SAFE UNALIGNED MEMORY ACCESS
// =====================================================================
template<typename T>
inline T load_unaligned(const void* ptr) {
    static_assert(std::is_trivially_copyable_v<T>, "load_unaligned requires trivially copyable type");
    T val;
    std::memcpy(&val, ptr, sizeof(T));
    return val;
}

template<typename T>
inline void store_unaligned(void* const ptr, T val) {
    static_assert(std::is_trivially_copyable_v<T>, "store_unaligned requires trivially copyable type");
    std::memcpy(ptr, &val, sizeof(T));
}

// =====================================================================
// ZERO-COST LOADERS & STORERS (Compile-Time Endian Resolution)
// =====================================================================
inline uint8_t LOAD_U8(const void* ptr) { return *static_cast<const uint8_t*>(ptr); }
inline void   STORE_U8(void* ptr, uint8_t v) { *static_cast<uint8_t*>(ptr) = v; }

inline uint16_t LOAD_U16(const void* ptr) {
    if constexpr (little_endian) return load_unaligned<uint16_t>(ptr);
    else return __builtin_bswap16(load_unaligned<uint16_t>(ptr));
}
inline void STORE_U16(void* ptr, uint16_t v) {
    if constexpr (little_endian) store_unaligned<uint16_t>(ptr, v);
    else store_unaligned<uint16_t>(ptr, __builtin_bswap16(v));
}

inline uint32_t LOAD_U32(const void* ptr) {
    if constexpr (little_endian) return load_unaligned<uint32_t>(ptr);
    else return __builtin_bswap32(load_unaligned<uint32_t>(ptr));
}
inline void STORE_U32(void* ptr, uint32_t v) {
    if constexpr (little_endian) store_unaligned<uint32_t>(ptr, v);
    else store_unaligned<uint32_t>(ptr, __builtin_bswap32(v));
}

inline uint64_t LOAD_U64(const void* ptr) {
    if constexpr (little_endian) return load_unaligned<uint64_t>(ptr);
    else return __builtin_bswap64(load_unaligned<uint64_t>(ptr));
}
inline void STORE_U64(void* ptr, uint64_t v) {
    if constexpr (little_endian) store_unaligned<uint64_t>(ptr, v);
    else store_unaligned<uint64_t>(ptr, __builtin_bswap64(v));
}

// =====================================================================
// FLOATING POINT SUPPORT
// =====================================================================
// Forward declarations for non-IEEE hardware support
inline float    F32_CONVERT_NON_IEEE(uint32_t val);
inline uint32_t F32_CONVERT_NON_IEEE(float val);
inline double   F64_CONVERT_NON_IEEE(uint64_t val);
inline uint64_t F64_CONVERT_NON_IEEE(double val);

inline float LOAD_F32(const void* ptr) {
    uint32_t raw = LOAD_U32(ptr);
    if constexpr (is_ieee754) return std::bit_cast<float>(raw);
    else return F32_CONVERT_NON_IEEE(raw);
}
inline void STORE_F32(void* ptr, float v) {
    if constexpr (is_ieee754) STORE_U32(ptr, std::bit_cast<uint32_t>(v));
    else STORE_U32(ptr, F32_CONVERT_NON_IEEE(v));
}

inline double LOAD_F64(const void* ptr) {
    uint64_t raw = LOAD_U64(ptr);
    if constexpr (is_ieee754) return std::bit_cast<double>(raw);
    else return F64_CONVERT_NON_IEEE(raw);
}
inline void STORE_F64(void* ptr, double v) {
    if constexpr (is_ieee754) STORE_U64(ptr, std::bit_cast<uint64_t>(v));
    else STORE_U64(ptr, F64_CONVERT_NON_IEEE(v));
}

// =====================================================================
// FASTFHIR STRUCTURE UTILITIES
// =====================================================================

/**
 * Standard Header for FastFHIR Arrays
 * Offset 0:  Validation Offset (Self-pointer)
 * Offset 8:  Recovery Tag (RECOVER_ARRAY)
 * Offset 10: Item Step Size (uint16_t)
 * Offset 12: Item Count (uint32_t)
 */
struct FF_ArrayHeader {
    static constexpr uint64_t SIZE = 16;
    
    static void Store(uint8_t* const __base, uint64_t& write_head, uint16_t recovery, uint16_t step, uint32_t count) {
        auto __ptr = __base + write_head;
        STORE_U64(__ptr + 0, write_head);
        STORE_U16(__ptr + 8, recovery);
        STORE_U16(__ptr + 10, step);
        STORE_U32(__ptr + 12, count);
        write_head += SIZE;
    }
};

/**
 * Choice Resolution Helper
 * FastFHIR handles choice types [x] by storing the specific block offset.
 */
inline bool FF_IsChoicePresent(uint64_t choice_offset) {
    return choice_offset != FF_NULL_OFFSET;
}