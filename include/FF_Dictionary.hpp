/**
 * @file FF_Dictionary.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file
 *         in the project root for terms.
 * @version 0.1
 *
 * @brief FastFHIR Code Dictionary — Permanent code↔string mappings.
 *
 * This header declares the external arrays that hold the permanent,
 * auditable mapping between 31-bit FF_CODE values and their FHIR code
 * strings.  The actual data lives in generated_src/FF_R4_Dictionary.cpp
 * and FF_R5_Dictionary.cpp — those files are auto-generated but their
 * values are permanent once committed.
 *
 * Design rationale (see architecture.md §6):
 *   - Every known FHIR code string gets a unique, permanent uint32_t
 *     value.  Values are assigned sequentially in alphabetical order.
 *     Once assigned, a value never changes.
 *   - Bit 31 (MSB) is reserved for FF_CUSTOM_STRING_FLAG — dictionary
 *     codes always have MSB = 0.  The null sentinel is FF_CODE_NULL
 *     (0xFFFFFFFF).
 *   - Two version-specific dictionaries (R4, R5) coexist so that codes
 *     absent from one version don't pollute the namespace of the other.
 *     The same code string has the same value in both dictionaries.
 *   - This is the same permanent-assignment pattern used by
 *     FF_Recovery.hpp — explicit, auditable, and never silently changed.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// =====================================================================
// FF_CodeEntry — a single (value, string) pair in the dictionary table.
// Tables are sorted by `code` to enable O(log n) binary-search resolve.
// =====================================================================
struct FF_CodeEntry {
    uint32_t      code;   // 31-bit permanent value (MSB = 0)
    const char*   label;  // Canonical FHIR code string
};

// ── R4 dictionary ────────────────────────────────────────────────────
extern const FF_CodeEntry* const  FF_R4_DICTIONARY;
extern const size_t               FF_R4_DICTIONARY_SIZE;

// ── R5 dictionary ────────────────────────────────────────────────────
extern const FF_CodeEntry* const  FF_R5_DICTIONARY;
extern const size_t               FF_R5_DICTIONARY_SIZE;

// =====================================================================
// Public API
// =====================================================================

/**
 * @brief Resolve a 31-bit wire code to its FHIR code string.
 *
 * @param code    The uint32_t value read from an FF_FIELD_CODE vtable slot
 *                (MSB is ignored — callers should strip FF_CUSTOM_STRING_FLAG
 *                before calling this function).
 * @param version FHIR version selector (e.g. FHIR_VERSION_R5).
 * @return const char*  The code string, or nullptr if unknown.
 *
 * @note Freetext custom strings (MSB set) should NOT be passed here —
 *       they are handled by the caller (FF_Parser) using the relative
 *       offset path.
 */
const char* FF_ResolveCode(uint32_t code, uint32_t version) noexcept;

/**
 * @brief Look up a FHIR code string and return its permanent wire value.
 *
 * @param str     The code string to look up.
 * @param version FHIR version selector.
 * @return uint32_t  The permanent code value, or FF_CODE_NULL if not found.
 */
uint32_t FF_GetDictionaryCode(const std::string& str, uint32_t version) noexcept;
