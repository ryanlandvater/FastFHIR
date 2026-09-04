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
uint32_t FF_GetDictionaryCode(const std::string& str, uint32_t /*version*/) noexcept {
    if (str.empty()) return FF_CODE_NULL;

    // THE EXACT INVERSE OF FF_ResolveCode, over ONE table.
    //
    // The dictionary is a STRING INTERN TABLE, not a concept table: the ID is
    // an index into FF_DICTIONARY_STRINGS, and that mapping is a bijection --
    // 4,634 strings, 4,634 IDs, no string appearing twice. A code's CodeSystem
    // is recovered from the element's binding, never from the ID, which is why
    // Quantity.code "F" and a US Core birthsex "F" legitimately share one ID.
    // Deciding "unit or not" here is therefore not possible and not needed;
    // whether a code is VALID for its element is conformance, and conformance
    // is an attachable layer (TASKS.md Block K), not this function.
    //
    // This used to consult three filtered views of that one bijection -- UCUM,
    // then R5 or R4 by requested version. Measured across the built tables:
    // their union is all 4,634 strings, and NO string maps to different IDs in
    // different tables. A partition of a bijection cannot answer better than
    // the bijection; it can only fail to answer. It did, for 999 codes.
    //
    // The cause is upstream and not a FHIR fact: R5 moved shared terminology
    // out of hl7.fhir.r5.core into hl7.terminology, which generator/specs.py
    // does not fetch, so R4 core contributes 1,062 CodeSystems and R5 core 448.
    // Codes like FDI-surface "M" are perfectly valid R5 codes that simply are
    // not in the package the generator reads. Under R5 they missed here,
    // returned FF_CODE_NULL, and sent the writer to an FF_CODEABLE_CONCEPT
    // fallback for a code whose permanent ID already existed -- while
    // FF_ResolveCode, which ignores version entirely, resolved them happily.
    // Write was version-gated and read was not; that asymmetry is the defect.
    //
    // `version` is ignored for the same reason FF_ResolveCode ignores it: an
    // ID means one string in every revision. Kept in the signature so callers
    // and the ABI are unchanged.
    //
    // EXACT MATCHES ONLY -- there is deliberately no case-insensitive fallback,
    // and building from FF_DICTIONARY_STRINGS preserves that for free.
    //
    // There used to be one: every UCUM label also inserted a lowercased alias,
    // "for fuzzy matching". UCUM is case-SENSITIVE by specification -- 'a' is a
    // year and 'A' an ampere, 'm' a metre and 'M' the mega prefix, 't' a tonne
    // and 'T' a tesla -- so a lowercase spelling is not a typo to be forgiven,
    // it is a different unit or no unit at all. Accepting it silently converts
    // a clinical quantity into one nobody wrote. 640 such aliases were live,
    // and because unordered_map::emplace does not overwrite, they also shadowed
    // real entries: 'a' returned ampere, and 11 FHIR codes ('f', 'n', 'w', 'c',
    // 'v', ...) were unreachable behind lowercased UCUM labels.
    //
    // Nothing is lost by refusing: an unrecognised code returns FF_CODE_NULL,
    // which sends the writer to an FF_CODEABLE_CONCEPT block holding the
    // ORIGINAL text. A non-conformant unit round-trips verbatim instead of
    // being silently "corrected" into a different one.
    using Map = std::unordered_map<std::string_view, uint32_t>;
    static const Map s_map = [] {
        Map m;
        m.reserve(FF_DICTIONARY_STRINGS_SIZE * 2);
        for (size_t i = 0; i < FF_DICTIONARY_STRINGS_SIZE; ++i) {
            const char* const label = FF_DICTIONARY_STRINGS[i];
            // Index 0 is the reserved null slot; empty entries are holes.
            if (label != nullptr && *label != '\0')
                m.emplace(std::string_view(label), static_cast<uint32_t>(i));
        }
        return m;
    }();

    const auto it = s_map.find(std::string_view(str));
    return it == s_map.end() ? FF_CODE_NULL : it->second;
}
