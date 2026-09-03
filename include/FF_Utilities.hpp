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

// =====================================================================
// FASTFHIR STRUCTURE UTILITIES
// =====================================================================

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
    // RANGE check, not a high-byte test. The resource band is 0x1000-0x1FFF --
    // it spans 16 high-byte values, because 256 slots could not hold FHIR's 178
    // concrete resource types with any headroom. The old `(tag & 0xFF00) ==
    // 0x0300` silently returned false for every resource above 0x03FF.
    //
    // The range named here was stale: it still said 0x0300-0x0FFF, the band's
    // home before the 2026-08-14 re-cut, while the constants below (and
    // dictionaries/master_tags.json, which is authoritative) had moved to
    // 0x1000-0x1FFF. The CODE was always right because it reads the constants;
    // only this comment lied, which is the worst place for it -- beside the
    // classifier that future band work will be reasoned about from.
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
    // All FastFHIR null sentinels are all-ones bit patterns, so "empty" is a
    // byte test — no endian-aware load needed (FF_Ops.hpp stays internal).
    const auto slot_all_ones = [base, field_absolute_offset](size_t n) noexcept {
        for (size_t i = 0; i < n; ++i)
            if (base[field_absolute_offset + i] != 0xFF) return false;
        return true;
    };
    switch (kind) {

        case FF_FIELD_RESOURCE:
        case FF_FIELD_CHOICE:
            if (FF_GET_RECOVERY_TAG(base, field_absolute_offset)
                    == FF_RECOVER_UNDEFINED) return true;
            [[fallthrough]];
        case FF_FIELD_STRING:
        case FF_FIELD_ARRAY:
        case FF_FIELD_BLOCK:
            return slot_all_ones(8);
            
        // Signedness does not enter into it: the sentinel is a BIT PATTERN
        // (all ones), so int32_t and uint32_t share the test. FF_FIELD_INT32 was
        // omitted here and became reachable the moment `integer` choice variants
        // started carrying RECOVER_FF_INT32 instead of RECOVER_FF_UINT32 -- at
        // which point every `valueInteger` in the corpus stopped being mislabelled
        // `valueUnsignedInt` and started vanishing outright, because `default`
        // below says "absent" and print_json drops it. Same trap as the date/time
        // one described below, sprung the same way: a kind reachable for the
        // first time meets a switch that never listed it.
        case FF_FIELD_CODE:
        case FF_FIELD_URL:
        case FF_FIELD_UINT32:
        case FF_FIELD_INT32:
            return slot_all_ones(4);

        // Both are 8 inline bytes whose null is all-ones, so they share a case.
        // Omitting FF_FIELD_DATETIME would not fail loudly: the `default` below
        // returns true, so every date/time field in the stream would report as
        // absent and be dropped on export. FF_FIELD_INT64/UINT64 join them for
        // the same reason -- 8 inline bytes, all-ones null -- and were likewise
        // absent; nothing emits an integer64 choice variant in the current
        // profile, so they were latently broken rather than visibly so.
        case FF_FIELD_FLOAT64:
        case FF_FIELD_DATETIME:
        case FF_FIELD_INT64:
        case FF_FIELD_UINT64:
            return slot_all_ones(8);

        case FF_FIELD_BOOL:
            return slot_all_ones(1);

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

/// Sign-extend a 31-bit relative offset packed beside FF_CODEABLE_CONCEPT_FLAG.
///
/// Callers pass the FULL slot word; the flag is not stripped first -- the
/// idiom is one idea in two steps: `<< 1` discards the flag from bit 31 and
/// moves bit 30 (the payload's sign) into the sign position, and the
/// arithmetic `>> 1` copies that sign back down, reconstructing the signed
/// value: bits 30-0 become a +/-1 GiB offset. The packers (FF_Primitives.cpp)
/// enforce that range and refuse the rel == -1 pattern that would collide with
/// FF_CODE_NULL, so backward offsets are first-class on the wire. A plain
/// `& FF_CODE_PAYLOAD_MASK` would ZERO-extend instead -- identical for forward
/// offsets, and a wrong huge positive for a legal backward one.
inline Offset FF_ResolveCodeableConceptOffset(uint32_t raw,
                                            Offset parent_off) noexcept {
    int32_t rel_off = static_cast<int32_t>(raw << 1) >> 1;
    return parent_off + static_cast<Offset>(static_cast<int64_t>(rel_off));
}

/// The 8-byte counterpart: sign-extend a 63-bit relative offset out of a packed
/// date/time slot. Identical arithmetic to the 31-bit case one width up -- the
/// two are written the same way on purpose, so a reader who has understood one
/// has understood both. The caller passes the FULL slot word: `<< 1` discards
/// FF_DATETIME_FALLBACK_FLAG and moves bit 62 into the sign position, and the
/// arithmetic `>> 1` sign-extends it. PRECONDITION: the caller has excluded
/// FF_DATETIME_NULL and confirmed the flag is set; a packed value is not an
/// offset.
inline Offset FF_ResolveDateTimeOffset(uint64_t raw,
                                       Offset parent_off) noexcept {
    int64_t rel_off = static_cast<int64_t>(raw << 1) >> 1;
    return parent_off + static_cast<Offset>(rel_off);
}