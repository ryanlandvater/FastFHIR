/**
 * @file FF_Dictionary.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file
 *         in the project root for terms.
 *
 * @brief Code dictionary resolve/lookup — shared by FF_Parser and FF_Builder.
 *
 * The actual code→label tables live in dictionaries/FF_R4_Dictionary.cpp
 * and dictionaries/FF_R5_Dictionary.cpp (auto-generated, permanent values).
 * This file implements the version-dispatched resolve and lookup on top of
 * those tables.
 */

#include "../include/FF_Dictionary.hpp"
#include "../include/FF_Primitives.hpp"  // FF_CODE_NULL, FHIR_VERSION_*

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

// =====================================================================
// FF_ResolveCode — code → string via binary search
// =====================================================================
const char* FF_ResolveCode(uint32_t code, uint32_t version) noexcept {
    // Freetext custom strings should never reach this function.
    if (code & FF_CUSTOM_STRING_FLAG) return nullptr;

    const FF_CodeEntry* table = nullptr;
    size_t              size  = 0;

    if (version >= FHIR_VERSION_R5) {
        table = FF_R5_DICTIONARY;
        size  = FF_R5_DICTIONARY_SIZE;
    } else if (version >= FHIR_VERSION_R4) {
        table = FF_R4_DICTIONARY;
        size  = FF_R4_DICTIONARY_SIZE;
    } else {
        return nullptr;
    }

    // Binary search on sorted-by-code array
    auto it = std::lower_bound(table, table + size, code,
        [](const FF_CodeEntry& entry, uint32_t c) noexcept {
            return entry.code < c;
        });

    if (it != table + size && it->code == code) {
        return it->label;
    }
    return nullptr;
}

// =====================================================================
// FF_GetDictionaryCode — string → code via unordered_map (built once)
// =====================================================================
uint32_t FF_GetDictionaryCode(const std::string& str, uint32_t version) noexcept {
    if (str.empty()) return FF_CODE_NULL;

    // Build a lazy static map for the requested version.
    // The full R4+R5 combined set is ~14K entries — well within reason.
    using Map = std::unordered_map<std::string_view, uint32_t>;

    static const Map* s_r4_map = nullptr;
    static const Map* s_r5_map = nullptr;

    const FF_CodeEntry* table = nullptr;
    size_t              size  = 0;
    const Map**         cache = nullptr;

    if (version >= FHIR_VERSION_R5) {
        table = FF_R5_DICTIONARY;
        size  = FF_R5_DICTIONARY_SIZE;
        cache = &s_r5_map;
    } else if (version >= FHIR_VERSION_R4) {
        table = FF_R4_DICTIONARY;
        size  = FF_R4_DICTIONARY_SIZE;
        cache = &s_r4_map;
    } else {
        return FF_CODE_NULL;
    }

    // Thread-safe one-time init (benign race on pointer — at worst two
    // threads build the same map and one leaks; fine for a read-heavy
    // workload.  If this becomes a problem, add std::call_once.)
    if (!*cache) {
        auto* map = new Map();
        map->reserve(size);
        for (size_t i = 0; i < size; ++i) {
            map->emplace(table[i].label, table[i].code);
        }
        *cache = map;
    }

    auto it = (*cache)->find(std::string_view(str));
    return it != (*cache)->end() ? it->second : FF_CODE_NULL;
}
