/**
 * @file FF_utilities.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @brief FastFHIR / IFE Core Utilities
 * @license FastFHIR Shared Source License (FF-SSL) — see LICENSE file in the project root for terms.
 *
 * This header provides essential utilities for FastFHIR and IFE, including:
 * - Endian-aware unaligned memory accessors (LOAD_U16, STORE_U32, etc.)
 * - Architecture detection for endianess and floating-point support
 * - Standardized structure layouts and validation helpers for FastFHIR blocks
 * - Choice type resolution utilities
 *
 * These utilities are designed for high performance and low overhead, enabling zero-copy
 * parsing and efficient serialization of FHIR resources in the FastFHIR format.
 *
 */

// MARK: - FastFHIR / IFE Core Utilities
#pragma once

#include "FF_Primitives.hpp"

#ifdef _MSC_VER
#include <stdlib.h>
static_assert(sizeof(short) == 2, "short must be 2 bytes");
static_assert(sizeof(long) == 4, "long must be 4 bytes");
static_assert(sizeof(long long) == 8, "long long must be 8 bytes");
#define bswap16(X) _byteswap_ushort(X)
#define bswap32(X) _byteswap_ulong(X)
#define bswap64(X) _byteswap_uint64(X)
#else
#include <cstdint>
#define bswap16(X) __builtin_bswap16(X)
#define bswap32(X) __builtin_bswap32(X)
#define bswap64(X) __builtin_bswap64(X)
#endif

// =====================================================================
// ARCHITECTURE & COMPILER FLAGS
// =====================================================================
// FastFHIR is strictly Little-Endian on the wire.
constexpr bool requires_byteswap = std::endian::native != std::endian::little;
constexpr bool is_ieee754 = std::numeric_limits<float>::is_iec559;

// =====================================================================
// ENDIAN-AWARE UNALIGNED LOADERS
// =====================================================================
inline uint8_t LOAD_U8(const void* ptr) { 
    uint8_t v; 
    std::memcpy(&v, ptr, 1); 
    return v; 
}

inline uint16_t LOAD_U16(const void* ptr) { 
    uint16_t v; 
    std::memcpy(&v, ptr, 2); 
    if constexpr (requires_byteswap) return bswap16(v);
    return v; 
}

inline uint32_t LOAD_U32(const void* ptr) { 
    uint32_t v; 
    std::memcpy(&v, ptr, 4); 
    if constexpr (requires_byteswap) return bswap32(v);
    return v; 
}

inline uint64_t LOAD_U64(const void* ptr) { 
    uint64_t v; 
    std::memcpy(&v, ptr, 8); 
    if constexpr (requires_byteswap) return bswap64(v);
    return v; 
}

// =====================================================================
// ENDIAN-AWARE UNALIGNED EMITTERS
// =====================================================================
inline void STORE_U8(void* ptr, uint8_t v) { 
    std::memcpy(ptr, &v, 1); 
}

inline void STORE_U16(void* ptr, uint16_t v) { 
    if constexpr (requires_byteswap) v = bswap16(v);
    std::memcpy(ptr, &v, 2); 
}

inline void STORE_U32(void* ptr, uint32_t v) { 
    if constexpr (requires_byteswap) v = bswap32(v);
    std::memcpy(ptr, &v, 4); 
}

inline void STORE_U64(void* ptr, uint64_t v) { 
    if constexpr (requires_byteswap) v = bswap64(v);
    std::memcpy(ptr, &v, 8); 
}

// =====================================================================
// NON-IEEE 754 FALLBACK CONVERSIONS
// =====================================================================

// --- 32-Bit (Float) Conversions ---
inline float F32_CONVERT_NON_IEEE(uint32_t val) {
    constexpr uint32_t SIGN_BIT = 0x80000000;
    constexpr uint32_t EXP_MASK = 0x7F800000;
    constexpr uint32_t MANTISSA_MASK = 0x007FFFFF;
    constexpr double IEEE_BASE = 8388608.0; // 2^23

    if ((val & ~SIGN_BIT) == 0) return 0.0f; // Handle 0.0 and -0.0

    float result = std::ldexp(1.0 + static_cast<double>(val & MANTISSA_MASK) / IEEE_BASE,
                              static_cast<int>((val & EXP_MASK) >> 23) - 127);
                              
    return (val & SIGN_BIT) ? -result : result;
}

inline uint32_t F32_CONVERT_NON_IEEE(float val) {
    constexpr uint32_t SIGN_BIT = 0x80000000;
    constexpr uint32_t EXP_MASK = 0x7F800000;
    constexpr uint32_t MANTISSA_MASK = 0x007FFFFF;

    if (val == 0.0f) return std::signbit(val) ? SIGN_BIT : 0;
    if (std::isinf(val)) return std::signbit(val) ? (SIGN_BIT | 0x7F800000) : 0x7F800000;
    if (std::isnan(val)) return 0x7FC00000; // qNaN

    uint32_t neg = std::signbit(val) ? SIGN_BIT : 0;
    int exp = 0;
    
    // frexp returns fraction in [0.5, 1.0). Scale to [0.0, 1.0) and shift 23 bits
    uint32_t mantissa = static_cast<uint32_t>(std::round(std::ldexp(std::frexp(std::fabs(val), &exp) * 2.0 - 1.0, 23)));
    
    return neg | (((static_cast<int32_t>(exp) + 126) << 23) & EXP_MASK) | (mantissa & MANTISSA_MASK);
}

// --- 64-Bit (Double) Conversions ---
inline double F64_CONVERT_NON_IEEE(uint64_t val) {
    constexpr uint64_t SIGN_BIT = 0x8000000000000000ULL;
    constexpr uint64_t EXP_MASK = 0x7FF0000000000000ULL;
    constexpr uint64_t MANTISSA_MASK = 0x000FFFFFFFFFFFFFULL;
    constexpr double IEEE_BASE = 4503599627370496.0; // 2^52

    if ((val & ~SIGN_BIT) == 0) return 0.0; 

    double result = std::ldexp(1.0 + static_cast<double>(val & MANTISSA_MASK) / IEEE_BASE,
                               static_cast<int>((val & EXP_MASK) >> 52) - 1023);
                               
    return (val & SIGN_BIT) ? -result : result;
}

inline uint64_t F64_CONVERT_NON_IEEE(double val) {
    constexpr uint64_t SIGN_BIT = 0x8000000000000000ULL;
    constexpr uint64_t EXP_MASK = 0x7FF0000000000000ULL;
    constexpr uint64_t MANTISSA_MASK = 0x000FFFFFFFFFFFFFULL;

    if (val == 0.0) return std::signbit(val) ? SIGN_BIT : 0;
    // FIXED: Infinity is 0x7FF0, not 0x7FF8
    if (std::isinf(val)) return std::signbit(val) ? (SIGN_BIT | 0x7FF0000000000000ULL) : 0x7FF0000000000000ULL;
    if (std::isnan(val)) return 0x7FF8000000000000ULL; // qNaN

    uint64_t neg = std::signbit(val) ? SIGN_BIT : 0;
    int exp = 0;
    
    // FIXED: Removed 1.0f truncation bug and replaced abs() with std::fabs()
    uint64_t mantissa = static_cast<uint64_t>(std::round(std::ldexp(std::frexp(std::fabs(val), &exp) * 2.0 - 1.0, 52)));
    
    return neg | (((static_cast<int64_t>(exp) + 1022) << 52) & EXP_MASK) | (mantissa & MANTISSA_MASK);
}

inline float LOAD_F32(const void *ptr)
{
    uint32_t raw = LOAD_U32(ptr);
    if constexpr (is_ieee754)
        return std::bit_cast<float>(raw);
    else
        return F32_CONVERT_NON_IEEE(raw);
}
inline void STORE_F32(void *ptr, float v)
{
    if constexpr (is_ieee754)
        STORE_U32(ptr, std::bit_cast<uint32_t>(v));
    else
        STORE_U32(ptr, F32_CONVERT_NON_IEEE(v));
}

inline double LOAD_F64(const void *ptr)
{
    uint64_t raw = LOAD_U64(ptr);
    if constexpr (is_ieee754)
        return std::bit_cast<double>(raw);
    else
        return F64_CONVERT_NON_IEEE(raw);
}
inline void STORE_F64(void *ptr, double v)
{
    if constexpr (is_ieee754)
        STORE_U64(ptr, std::bit_cast<uint64_t>(v));
    else
        STORE_U64(ptr, F64_CONVERT_NON_IEEE(v));
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
struct FF_ArrayHeader
{
    static constexpr uint64_t SIZE = 16;

    static void Store(uint8_t *const __base, uint64_t &write_head, uint16_t recovery, uint16_t step, uint32_t count)
    {
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
inline bool FF_IsChoicePresent(uint64_t choice_offset)
{
    return choice_offset != FF_NULL_OFFSET;
}

/**
 * @brief Utility to determine if a given RECOVERY_TAG corresponds to a top-level FHIR resource.
 * This is used for validation and to apply resource-specific logic during parsing and building.
 */
inline constexpr bool FF_IsResourceTag(RECOVERY_TAG tag) {
    return (tag & 0xFF00) == RECOVER_FF_RESOURCE_BLOCK; // 0x0300
}

/**
 * @brief Utility to determine if a given RECOVERY_TAG corresponds to an inline scalar block.
 * This is used for validation and to apply scalar-specific logic during parsing and building.
 * 
 * @param tag The RECOVERY_TAG to check.
 * @return true if the tag corresponds to an inline scalar block.
 */
inline constexpr bool FF_IsScalarBlockTag(RECOVERY_TAG tag) {
    return (tag & 0xFF00) == RECOVER_FF_SCALAR_BLOCK; // 0x0100
}

/**
 * @brief Zero-allocation raw memory peek to determine if a FastFHIR field is null/empty.
 */
inline constexpr bool FF_IsFieldEmpty(const BYTE* base, Offset field_absolute_offset, FF_FieldKind kind) {
    switch (kind) {

        case FF_FIELD_RESOURCE:
        case FF_FIELD_CHOICE:
        if (LOAD_U16(base + field_absolute_offset + DATA_BLOCK::RECOVERY) 
                == FF_RECOVER_UNDEFINED) return true;
            
        case FF_FIELD_STRING:
        case FF_FIELD_ARRAY:
        case FF_FIELD_BLOCK:
            return LOAD_U64(base + field_absolute_offset) == FF_NULL_OFFSET;
            
        case FF_FIELD_CODE:
        case FF_FIELD_UINT32:
            return LOAD_U32(base + field_absolute_offset) == FF_NULL_UINT32;
            
        case FF_FIELD_FLOAT64:
            return LOAD_U64(base + field_absolute_offset) == FF_NULL_UINT64;
            
        case FF_FIELD_BOOL:
            return LOAD_U8(base + field_absolute_offset) == FF_NULL_UINT8;
            
        default:
            return true;
    }
}
// =====================================================================
// SCALAR RESOLVER (Decode & Encode)
// =====================================================================
namespace FastFHIR::Decode {
    template <typename T>
    inline T scalar(const BYTE* base, Offset absolute_offset, RECOVERY_TAG tag) {
        if constexpr (std::is_same_v<T, bool>) {
            return LOAD_U8(base + absolute_offset) != 0;
        } else if constexpr (std::is_same_v<T, double>) {
            return LOAD_F64(base + absolute_offset);
        } else if constexpr (sizeof(T) == 4) {
            return static_cast<T>(LOAD_U32(base + absolute_offset));
        } else if constexpr (sizeof(T) == 8) {
            return static_cast<T>(LOAD_U64(base + absolute_offset));
        }
        throw std::runtime_error("FastFHIR: Unsupported scalar decode target.");
    }
}
namespace FastFHIR::Encode {
    template <typename T>
    requires std::is_arithmetic_v<T>
    inline void scalar(BYTE* base, Offset absolute_offset, T val) {
        if constexpr (std::is_same_v<T, bool>) {
            STORE_U8(base + absolute_offset, val ? 1 : 0);
        } else if constexpr (std::is_same_v<T, double>) {
            STORE_F64(base + absolute_offset, val);
        } else if constexpr (sizeof(T) == 4) {
            STORE_U32(base + absolute_offset, static_cast<uint32_t>(val));
        } else if constexpr (sizeof(T) == 8) {
            STORE_U64(base + absolute_offset, static_cast<uint64_t>(val));
        }
    }
}