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

    // EXACT MATCHES ONLY. UCUM first (it wins dedup over R4/R5), then R4 or R5
    // by requested version. There is deliberately no case-insensitive fallback.
    //
    // There used to be one: every UCUM label also inserted a lowercased alias,
    // "for fuzzy matching". UCUM is case-SENSITIVE by specification -- 'a' is a
    // year and 'A' an ampere, 'm' a metre and 'M' the mega prefix, 't' a tonne
    // and 'T' a tesla -- so a lowercase spelling is not a typo to be forgiven,
    // it is a different unit or no unit at all. Accepting it silently converts
    // a clinical quantity into one nobody wrote.
    //
    // The aliases corrupted two things at once, and 640 of them were live:
    //
    //   * Real UCUM codes. Labels and aliases went into one map in one pass,
    //     and unordered_map::emplace does not overwrite, so 'A' (ampere) seeded
    //     the key 'a' before the real 'a' (year) was reached and the real entry
    //     no-opped. Every lookup of "a" returned ampere. 55 of the 1,384 labels
    //     differ from another only by case; which of each pair broke came down
    //     to table order -- 'd' (day) precedes 'D' (deci-) and so looked
    //     correct, which is most of why this survived.
    //
    //   * Real FHIR codes. The aliases sat in the UCUM map, consulted before
    //     R4/R5, so a lowercased UCUM label shadowed any FHIR code spelled the
    //     same way: 'f' resolved to farad, 'n' to newton, 'w' to watt, 'c' to
    //     coulomb, 'v' to volt -- 11 codes unreachable, each with its own
    //     permanent ledger ID.
    //
    // Nothing is lost by refusing: an unrecognised code returns FF_CODE_NULL,
    // which sends the writer to an FF_CODEABLE_CONCEPT block holding the
    // ORIGINAL text. A non-conformant unit round-trips verbatim instead of
    // being silently "corrected" into a different one.
    {
        static const Map s_ucum_map = [] {
            Map m; m.reserve(FF_UCUM_DICTIONARY_SIZE * 2);
            for (size_t i = 0; i < FF_UCUM_DICTIONARY_SIZE; ++i)
                m.emplace(FF_UCUM_DICTIONARY[i].label,
                          static_cast<uint32_t>(FF_UCUM_DICTIONARY[i].code));
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
