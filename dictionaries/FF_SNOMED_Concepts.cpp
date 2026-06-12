/**
 * @file FF_SNOMED_Concepts.cpp
 * @brief SNOMED CT concept registry — permanent code↔ID mappings.
 *
 * SNOMED CT concept IDs are numeric and self-encoding — the numeric string
 * IS the compact ID stored in the 7-byte payload.  This file exists as a
 * placeholder for the concept display-name → ID lookup table (future).
 *
 * Currently empty.  Populate from SNOMED CT RF2 release data.
 */

#include <cstdint>
#include <cstddef>
#include <utility>

extern const std::pair<const char*, uint64_t> FF_SNOMED_CONCEPTS[] = {};

extern const size_t FF_SNOMED_CONCEPTS_SIZE =
    sizeof(FF_SNOMED_CONCEPTS) / sizeof(FF_SNOMED_CONCEPTS[0]);
