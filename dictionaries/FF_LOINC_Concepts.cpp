/**
 * @file FF_LOINC_Concepts.cpp
 * @brief LOINC concept registry — permanent code↔ID mappings.
 *
 * Each entry maps a LOINC code (e.g., "12345-6") to a compact sequential ID.
 * IDs are assigned permanently — once committed, they never change.
 *
 * Currently empty placeholder.  Populate from LOINC release data.
 */

#include <cstdint>
#include <cstddef>
#include <utility>

const std::pair<const char*, uint64_t> FF_LOINC_CONCEPTS[] = {};

const size_t FF_LOINC_CONCEPTS_SIZE =
    sizeof(FF_LOINC_CONCEPTS) / sizeof(FF_LOINC_CONCEPTS[0]);
