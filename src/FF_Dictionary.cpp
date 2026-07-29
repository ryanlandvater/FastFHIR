/**
 * @file FF_Dictionary.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public
 *         License, v. 2.0. If a copy of the MPL was not distributed with this
 *         file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * @brief Code dictionary resolve/lookup — shared by FF_Parser and FF_Builder.
 *
 * The actual code→label tables live in dictionaries/FF_R4_Dictionary.cpp
 * and dictionaries/FF_R5_Dictionary.cpp (auto-generated, permanent values).
 * This file implements the version-dispatched resolve and lookup on top of
 * those tables.
 */

#include "FF_Dictionary.hpp"
#include "FF_Primitives.hpp"  // FF_CODE_NULL, FHIR_VERSION_*

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

// =====================================================================
// FF_ResolveCode — now O(1) inline in FF_Dictionary.hpp.
// =====================================================================

// =====================================================================
// FF_GetDictionaryCode — string → code via unordered_map (built once)
// =====================================================================
uint32_t FF_GetDictionaryCode(const std::string& str, uint32_t version) noexcept {
    if (str.empty()) return FF_CODE_NULL;

    using Map = std::unordered_map<std::string_view, uint32_t>;

    // UCUM first — highest priority in dedup (UCUM > R4 > R5).
    // Also inserts lowered keys for case-insensitive fuzzy matching.
    {
        static const Map s_ucum_map = [] {
            static std::vector<std::string> lowered;
            Map m; m.reserve(FF_UCUM_DICTIONARY_SIZE * 2);
            for (size_t i = 0; i < FF_UCUM_DICTIONARY_SIZE; ++i) {
                const char* label = FF_UCUM_DICTIONARY[i].label;
                auto code = static_cast<uint32_t>(FF_UCUM_DICTIONARY[i].code);
                m.emplace(label, code);
                // Lowered key for fuzzy UCUM matching
                std::string lo(label);
                for (auto& c : lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                lowered.push_back(std::move(lo));
                m.emplace(lowered.back(), code);
            }
            return m;
        }();
        auto it = s_ucum_map.find(std::string_view(str));
        if (it != s_ucum_map.end()) return it->second;
    }

    if (version >= FHIR_VERSION_R5) {
        static const Map s_r5_map = [] {
            Map m; m.reserve(FF_R5_DICTIONARY_SIZE);
            for (size_t i = 0; i < FF_R5_DICTIONARY_SIZE; ++i)
                m.emplace(FF_R5_DICTIONARY[i].label, static_cast<uint32_t>(FF_R5_DICTIONARY[i].code));
            return m;
        }();
        auto it = s_r5_map.find(std::string_view(str));
        if (it != s_r5_map.end()) return it->second;
    } else if (version >= FHIR_VERSION_R4) {
        static const Map s_r4_map = [] {
            Map m; m.reserve(FF_R4_DICTIONARY_SIZE);
            for (size_t i = 0; i < FF_R4_DICTIONARY_SIZE; ++i)
                m.emplace(FF_R4_DICTIONARY[i].label, static_cast<uint32_t>(FF_R4_DICTIONARY[i].code));
            return m;
        }();
        auto it = s_r4_map.find(std::string_view(str));
        if (it != s_r4_map.end()) return it->second;
    }

    return FF_CODE_NULL;
}
