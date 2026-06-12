/**
 * @file FF_CodeableConcept.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file
 *         in the project root for terms.
 * @version 0.1
 *
 * @brief Enriched encode/decode logic for FF_CODEABLE_CONCEPT blocks.
 *
 * Each registered FF_ExternalCodeSystem maintains a pre-built
 * hash map that maps canonical code strings to compact 56-bit integer IDs.
 * This allows the 7-byte CODE payload to store a dense index rather than
 * raw ASCII, making lookups O(1) and keeping the block small.
 *
 * Systems without pre-built maps (or codes not found in them) fall back to
 * system-specific inline encoding (e.g., SNOMED stores the numeric concept
 * ID directly; unknown UCUM compositions go through the custom-string path).
 */

#pragma once

#include "FF_Primitives.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FastFHIR {

// =====================================================================
// ConceptRegistry — bidirectional string ↔ 56-bit integer mapping
// =====================================================================
// One instance per external code system.  Populated at startup from
// pre-compiled data tables (generated_src/FF_*_Concepts.cpp).
//
// Thread-safety: all methods are read-only after construction.  The maps
// are never mutated after initialization, so concurrent reads are safe
// without locks.
class ConceptRegistry {
public:
    using CodeId = uint64_t;  // 56-bit compact ID
    constexpr static CodeId MAX_CODE_ID = 0x00FFFFFFFFFFFFFF;

    ConceptRegistry() = default;  // empty registry

    // Construct from a permanent array of {string, id} pairs.
    ConceptRegistry(const std::pair<const char*, CodeId>* entries,
                    size_t count);

    // Look up a code string → compact ID.  Returns 0 if not found.
    CodeId find(const std::string& code) const noexcept;

    // Reverse lookup: compact ID → code string.  Returns empty if not found.
    std::string_view resolve(CodeId id) const noexcept;

    // Number of registered entries.
    size_t size() const noexcept { return m_forward.size(); }

    // Largest valid CodeId (inclusive).  All IDs in [1, max_id()] are valid.
    // Returns 0 when the registry is empty.
    CodeId max_id() const noexcept {
        return m_reverse.empty() ? 0 : static_cast<CodeId>(m_reverse.size() - 1);
    }

private:
    std::unordered_map<std::string, CodeId> m_forward;
    std::vector<std::string>                m_reverse;  // indexed by CodeId
};

// =====================================================================
// Per-system concept registries (populated at startup)
// =====================================================================

// Returns the global ConceptRegistry for a given code system.
// Returns nullptr if the system has no pre-built registry.
const ConceptRegistry* FF_GetConceptRegistry(FF_ExternalCodeSystem sys) noexcept;

// =====================================================================
// Enriched encode / decode
// =====================================================================

// Pack a code string into a 56-bit packed ID for the given system.
// Uses the ConceptRegistry for hash-based lookup when available;
// falls back to system-specific inline encoding.
//
// Returns the packed 56-bit integer (host byte order).  The caller
// is responsible for storing it in big-endian format in the wire block.
uint64_t FF_PackCode(FF_ExternalCodeSystem sys,
                     std::string_view code_str) noexcept;

// Unpack a 56-bit integer (host byte order) back to a code string.
// Uses the ConceptRegistry reverse lookup; falls back to system-specific
// inline decoding.
//
// The returned string_view points into a thread-local buffer and is
// valid until the next call on the same thread.
std::string_view FF_UnpackCode(FF_ExternalCodeSystem sys,
                               uint64_t packed_id) noexcept;

}  // namespace FastFHIR
