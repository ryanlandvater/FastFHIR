/**
 * @file FF_utilities.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @brief FastFHIR / IFE Core Utilities
 * @license This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
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
    // RANGE check, not a high-byte test. The resource band is 0x0300-0x0FFF --
    // it spans 13 high-byte values, because 256 slots could not hold FHIR's 178
    // concrete resource types with any headroom. The old `(tag & 0xFF00) ==
    // 0x0300` silently returned false for every resource above 0x03FF.
    // Strip the array bit first so an array-of-resource still classifies.
    const uint16_t base = static_cast<uint16_t>(GetTypeFromTag(tag));
    return base >= RECOVER_BAND_RESOURCE_FIRST && base <= RECOVER_BAND_RESOURCE_LAST;
}

/**
 * @brief Utility to determine if a given RECOVERY_TAG corresponds to a BackboneElement.
 */
inline constexpr bool FF_IsBackboneTag(RECOVERY_TAG tag) {
    const uint16_t base = static_cast<uint16_t>(GetTypeFromTag(tag));
    return base >= RECOVER_BAND_BACKBONE_FIRST && base <= RECOVER_BAND_BACKBONE_LAST;
}

/**
 * @brief Utility to determine if a given RECOVERY_TAG corresponds to an inline scalar block.
 * This is used for validation and to apply scalar-specific logic during parsing and building.
 * 
 * @param tag The RECOVERY_TAG to check.
 * @return true if the tag corresponds to an inline scalar block.
 */
inline constexpr bool FF_IsScalarBlockTag(RECOVERY_TAG tag) {
    // The scalar band is exactly one high byte wide, so `& 0xFF00` is still
    // correct here -- but it is written as a range check so every classifier
    // reads the same way and a future band re-cut only touches the boundary
    // constants. The open-coded `(x & 0xFF00) == RECOVER_FF_SCALAR_BLOCK` in
    // FF_Primitives.hpp, FF_Ops.hpp and FF_Parser.cpp stays valid for the same
    // reason; if the scalar band is ever widened, those three must move here.
    const uint16_t base = static_cast<uint16_t>(GetTypeFromTag(tag));
    return base >= RECOVER_BAND_SCALAR_FIRST && base <= RECOVER_BAND_SCALAR_LAST;
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

/// Sign-extend a 31-bit relative offset (FF_CODEABLE_CONCEPT_FLAG is already
/// stripped by the caller).  Arithmetic right-shift on int32_t reconstructs
/// the signed value: bits 30–0 become a ±1 GB offset.
inline Offset FF_ResolveCodeableConceptOffset(uint32_t raw,
                                            Offset parent_off) noexcept {
    int32_t rel_off = static_cast<int32_t>(raw << 1) >> 1;
    return parent_off + static_cast<Offset>(static_cast<int64_t>(rel_off));
}