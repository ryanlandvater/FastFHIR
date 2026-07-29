/**
 * @file FF_Dictionary.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public
 *         License, v. 2.0. If a copy of the MPL was not distributed with this
 *         file, You can obtain one at http://mozilla.org/MPL/2.0/.
 * @version 0.1
 *
 * @brief FastFHIR Code Dictionary — Permanent code↔string mappings.
 *
 * This header declares the external arrays that hold the permanent,
 * auditable mapping between 31-bit FF_CODE values and their FHIR code
 * strings.  The actual data lives in dictionaries/FF_R4_Dictionary.cpp
 * and dictionaries/FF_R5_Dictionary.cpp — those files are auto-generated but their
 * values are permanent once committed.
 *
 * Design rationale (see architecture.md §6):
 *   - Every known FHIR code string gets a unique, permanent uint32_t
 *     value.  Values are assigned sequentially in alphabetical order.
 *     Once assigned, a value never changes.
 *   - Bit 31 (MSB) is reserved for FF_CODEABLE_CONCEPT_FLAG — dictionary
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
#include <string_view>
#include "FF_Primitives.hpp"
#include "FF_Codes.hpp"

/// One row of a per-version lookup table: a permanent code ID and the string
/// it decodes to. The label always points into FF_DICTIONARY_STRINGS, so the
/// ID->string meaning has exactly one source of truth.
struct FF_CodeEntry {
    uint32_t     code;
    const char*  label;
};

extern const char* const FF_DICTIONARY_STRINGS[];
extern const size_t      FF_DICTIONARY_STRINGS_SIZE;

extern const FF_CodeEntry* const  FF_R4_DICTIONARY;
extern const size_t               FF_R4_DICTIONARY_SIZE;

extern const FF_CodeEntry* const  FF_R5_DICTIONARY;
extern const size_t               FF_R5_DICTIONARY_SIZE;

extern const FF_CodeEntry* const  FF_UCUM_DICTIONARY;
extern const size_t               FF_UCUM_DICTIONARY_SIZE;

inline const char* FF_ResolveCode(uint32_t code, uint32_t /*version*/) noexcept {
    if (code == FF_CODE_NULL || code >= FF_DICTIONARY_STRINGS_SIZE)
        return nullptr;
    return FF_DICTIONARY_STRINGS[code];
}

uint32_t FF_GetDictionaryCode(const std::string& str, uint32_t version) noexcept;

/// UCUM unit lookup. Codes are plain uint32_t IDs -- FastFHIR::FF_CODE::UCUM is
/// a namespace of constants (see dictionaries/FF_Codes.hpp), not a type, because
/// the same ID is also reachable under its FHIR CodeSystem scope.
inline uint32_t FF_GetUCUMCode(std::string_view label) noexcept {
    return FF_GetDictionaryCode(std::string(label), FHIR_VERSION_R5);
}

inline const char* FF_ResolveUCUMCode(uint32_t code) noexcept {
    return FF_ResolveCode(code, FHIR_VERSION_R5);
}
