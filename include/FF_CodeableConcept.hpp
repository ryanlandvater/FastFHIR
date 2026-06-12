/**
 * @file FF_CodeableConcept.hpp
 * @brief Enriched encode/decode for FF_CODEABLE_CONCEPT blocks.
 *
 * Each external code system uses a compiled-in lookup strategy:
 *   UCUM:   FF_GetUCUMCode() — O(1) lazy hash map + O(1) reverse string table
 *   LOINC:  (future) same pattern
 *   SNOMED: self-encoding numeric ID
 */

#pragma once

#include "FF_Primitives.hpp"
#include <cstdint>
#include <string_view>

namespace FastFHIR {

uint64_t FF_PackCode(FF_ExternalCodeSystem sys,
                     std::string_view code_str) noexcept;

std::string_view FF_UnpackCode(FF_ExternalCodeSystem sys,
                               uint64_t packed_id) noexcept;

}  // namespace FastFHIR
