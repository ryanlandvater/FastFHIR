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
#include "FF_Ops.hpp"

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
            [[fallthrough]];
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
// Sign-extended relative offset resolution
// =====================================================================
// FF_FIELD_CODE slots pack signed relative offsets into 31 or 30 bits
// alongside a flag bit.  These helpers extract, sign-extend, and resolve
// the offset to an absolute arena address from the parent block.

/// Resolve a 31-bit signed offset (FF_CUSTOM_STRING_FLAG set) to absolute offset.
/// The lower 31 bits of @p raw are sign-extended via XOR-bias: bit 30 is the
/// sign bit, shifted to bit 31 in the int32_t result.
inline Offset FF_ResolveCustomStringOffset(uint32_t raw,
                                            Offset parent_off) noexcept {
    uint32_t val = raw & 0x7FFFFFFFu;  // strip flag bit
    int32_t rel = static_cast<int32_t>(val ^ 0x40000000u) - 0x40000000;
    return parent_off + static_cast<Offset>(static_cast<int64_t>(rel));
}

/// Resolve a 30-bit signed offset (FF_CODEABLE_CONCEPT_FLAG set) to absolute offset.
/// The lower 30 bits of @p raw are sign-extended: bit 29 is the sign bit.
inline Offset FF_ResolveCodeableConceptOffset(uint32_t raw,
                                               Offset parent_off) noexcept {
    uint32_t val = raw & 0x3FFFFFFFu;  // strip flag bits 31,30
    int32_t rel = static_cast<int32_t>(val ^ 0x20000000u) - 0x20000000;
    return parent_off + static_cast<Offset>(static_cast<int64_t>(rel));
}