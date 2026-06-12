/**
 * @file FF_UCUM_Concepts.cpp
 * @brief UCUM Tier-1 concept registry — permanent, auditable code↔ID mappings.
 *
 * Populated from the Regenstrief UCUM Common Synonyms list.
 * Each entry maps a UCUM expression string to a compact sequential ID.
 * IDs are assigned permanently — once committed, they never change.
 *
 * This file follows the same pattern as FF_R4_Dictionary.cpp:
 *   - Extern const array of {string, uint64_t} pairs
 *   - Extern const size_t
 *   - Consumed by ConceptRegistry constructor in FF_CodeableConcept.cpp
 *
 * Currently empty placeholder.  Run tools/extract_ucum_concepts.py to populate.
 */

#include <cstdint>
#include <cstddef>
#include <utility>

// Placeholder — no entries yet.
// Format: {"ucum_expression", sequential_id}
const std::pair<const char*, uint64_t> FF_UCUM_CONCEPTS[] = {
    // {"m",   1},
    // {"g",   2},
    // {"s",   3},
};

const size_t FF_UCUM_CONCEPTS_SIZE =
    sizeof(FF_UCUM_CONCEPTS) / sizeof(FF_UCUM_CONCEPTS[0]);
