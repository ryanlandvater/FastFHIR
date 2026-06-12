/**
 * @file FF_CodeableConcept.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file
 *         in the project root for terms.
 *
 * @brief Enriched encode/decode for FF_CODEABLE_CONCEPT blocks.
 *
 * Each external code system uses a pre-built hash map
 * (ConceptRegistry) to map canonical code strings ↔ compact 56-bit IDs.
 * Unknown codes fall back to system-specific inline packing.
 */

#include "../include/FF_CodeableConcept.hpp"
#include "../dictionaries/FF_UCUM_Codes.hpp"
#include "../include/FF_Utilities.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace FastFHIR {

// =====================================================================
// ConceptRegistry
// =====================================================================

ConceptRegistry::ConceptRegistry(const std::pair<const char*, CodeId>* entries,
                                 size_t count) {
    if (!entries || count == 0) return;
    m_forward.reserve(count);
    CodeId max_id = 0;
    for (size_t i = 0; i < count; ++i)
        if (entries[i].second > max_id) max_id = entries[i].second;
    m_reverse.resize(static_cast<size_t>(max_id) + 1);

    for (size_t i = 0; i < count; ++i) {
        std::string key(entries[i].first);
        CodeId id = entries[i].second;
        m_forward.emplace(std::move(key), id);
        m_reverse[static_cast<size_t>(id)] = entries[i].first;
    }
}

ConceptRegistry::CodeId ConceptRegistry::find(const std::string& code) const noexcept {
    auto it = m_forward.find(code);
    return (it != m_forward.end()) ? it->second : 0;
}

std::string_view ConceptRegistry::resolve(CodeId id) const noexcept {
    auto idx = static_cast<size_t>(id);
    if (idx >= m_reverse.size()) return {};
    return m_reverse[idx];
}

// =====================================================================
// Pre-compiled concept tables (populated from generated data)
// =====================================================================

// ── SNOMED CT ───────────────────────────────────────────────────────
// SNOMED concept IDs are numeric and self-encoding — no registry needed.
// The numeric ID is stored directly as a 56-bit big-endian integer.

}  // namespace FastFHIR

// ── Extern concept arrays (defined in dictionaries/) ──────────────
extern const std::pair<const char*, FF_UCUM_CODES> FF_UCUM_CONCEPTS[];
extern const size_t                                 FF_UCUM_CONCEPTS_SIZE;
extern const std::pair<const char*, uint64_t> FF_LOINC_CONCEPTS[];
extern const size_t                            FF_LOINC_CONCEPTS_SIZE;

namespace FastFHIR {
namespace {
    ConceptRegistry g_ucum_registry(reinterpret_cast<const std::pair<const char*, uint64_t>*>(FF_UCUM_CONCEPTS), FF_UCUM_CONCEPTS_SIZE);
}

// ── LOINC ───────────────────────────────────────────────────────────
namespace {
    ConceptRegistry g_loinc_registry(FF_LOINC_CONCEPTS, FF_LOINC_CONCEPTS_SIZE);
}

// ── BCP-47 / MIME — no registries yet (TODO: add dictionaries) ────
namespace {
    ConceptRegistry g_bcp47_registry;  // empty
    ConceptRegistry g_mime_registry;   // empty
}

const ConceptRegistry* FF_GetConceptRegistry(FF_ExternalCodeSystem sys) noexcept {
    switch (sys) {
    case FF_ExternalCodeSystem::UCUM:      return &g_ucum_registry;
    case FF_ExternalCodeSystem::LOINC:     return &g_loinc_registry;
    case FF_ExternalCodeSystem::BCP_47:    return &g_bcp47_registry;
    case FF_ExternalCodeSystem::MIME:      return &g_mime_registry;
    case FF_ExternalCodeSystem::SNOMED_CT: return nullptr;  // self-encoding
    default:                               return nullptr;
    }
}


// =====================================================================
// System-specific inline packing — see global-scope helpers below.

#include <cstdlib>  // strtoull

/// Parse a numeric string_view via strtoull.  SNOMED IDs ≤ 18 digits fit in u64.
static uint64_t _parse_numeric(std::string_view sv) {
    if (sv.empty()) return 0;
    char buf[20];
    size_t n = sv.size() < 19 ? sv.size() : 19;
    std::memcpy(buf, sv.data(), n);
    buf[n] = '\0';
    return strtoull(buf, nullptr, 10);
}

// =====================================================================
// FF_PackCode — string → 56-bit packed ID
// =====================================================================

uint64_t FF_PackCode(FF_ExternalCodeSystem sys,
                     std::string_view code_str) noexcept {
    if (code_str.empty()) return 0;

    switch (sys) {

    case FF_ExternalCodeSystem::SNOMED_CT:
        // SNOMED concept IDs are self-encoding: the numeric string IS the ID.
        // Parse directly into a 56-bit integer.
        return _parse_numeric(code_str);

    case FF_ExternalCodeSystem::UCUM: {
        // Tier 1: check the concept registry for known synonyms.
        if (auto* reg = FF_GetConceptRegistry(sys)) {
            if (auto id = reg->find(std::string(code_str)))
                return id;
        }
        // Tier 2 fallback: hash the first 7 ASCII bytes into a 56-bit token.
        // The caller should prefer the custom-string path for complex
        // compositions; this is a last-resort inline encoding.
        uint64_t token = 0;
        size_t n = code_str.size() < 7 ? code_str.size() : 7;
        for (size_t i = 0; i < n; ++i)
            token = (token << 8) | static_cast<uint8_t>(code_str[i]);
        return token;
    }

    case FF_ExternalCodeSystem::LOINC: {
        // Check concept registry first.
        if (auto* reg = FF_GetConceptRegistry(sys)) {
            if (auto id = reg->find(std::string(code_str)))
                return id;
        }
        // Fallback: pack the LOINC code (e.g., "12345-6") as ASCII.
        uint64_t token = 0;
        for (char c : code_str) {
            if (c == '-') continue;  // skip check-digit hyphen
            token = (token << 8) | static_cast<uint8_t>(c);
        }
        return token;
    }

    case FF_ExternalCodeSystem::BCP_47:
    case FF_ExternalCodeSystem::MIME:
        // Check concept registry.
        if (auto* reg = FF_GetConceptRegistry(sys)) {
            if (auto id = reg->find(std::string(code_str)))
                return id;
        }
        // Fallback: pack first 7 ASCII bytes.
        {
            uint64_t token = 0;
            size_t n = code_str.size() < 7 ? code_str.size() : 7;
            for (size_t i = 0; i < n; ++i)
                token = (token << 8) | static_cast<uint8_t>(code_str[i]);
            return token;
        }

    default:
        return 0;
    }
}

// =====================================================================
// FF_UnpackCode — 56-bit packed ID → string
// =====================================================================

std::string_view FF_UnpackCode(FF_ExternalCodeSystem sys,
                               uint64_t packed_id) noexcept {
    thread_local char buf[128];

    if (packed_id == 0) return {};

    switch (sys) {

    case FF_ExternalCodeSystem::SNOMED_CT: {
        // SNOMED: the packed ID is the numeric concept ID.
        int pos = snprintf(buf, sizeof(buf), "%llu",
                          static_cast<unsigned long long>(packed_id));
        return std::string_view(buf, static_cast<size_t>(pos));
    }

    case FF_ExternalCodeSystem::UCUM: {
        // Try concept registry reverse lookup first.
        if (auto* reg = FF_GetConceptRegistry(sys)) {
            if (auto sv = reg->resolve(packed_id); !sv.empty())
                return sv;
        }
        // Fallback: unpack ASCII from the 56-bit token.
        size_t len = 0;
        uint64_t tmp = packed_id;
        // Find the first non-zero byte from the MSB side.
        for (int i = 6; i >= 0; --i) {
            uint8_t b = static_cast<uint8_t>(tmp >> (i * 8));
            if (b != 0) buf[len++] = static_cast<char>(b);
        }
        return std::string_view(buf, len);
    }

    case FF_ExternalCodeSystem::LOINC: {
        // Try concept registry.
        if (auto* reg = FF_GetConceptRegistry(sys)) {
            if (auto sv = reg->resolve(packed_id); !sv.empty())
                return sv;
        }
        // Fallback: unpack ASCII from token, re-insert hyphen at position 5.
        uint64_t tmp = packed_id;
        size_t len = 0;
        for (int i = 5; i >= 0; --i) {
            uint8_t b = static_cast<uint8_t>(tmp >> (i * 8));
            if (b != 0) {
                if (len == 5) buf[len++] = '-';
                buf[len++] = static_cast<char>(b);
            }
        }
        return std::string_view(buf, len);
    }

    case FF_ExternalCodeSystem::BCP_47:
    case FF_ExternalCodeSystem::MIME:
        // Try concept registry.
        if (auto* reg = FF_GetConceptRegistry(sys)) {
            if (auto sv = reg->resolve(packed_id); !sv.empty())
                return sv;
        }
        // Fallback: unpack ASCII.
        {
            size_t len = 0;
            uint64_t tmp = packed_id;
            for (int i = 6; i >= 0; --i) {
                uint8_t b = static_cast<uint8_t>(tmp >> (i * 8));
                if (b != 0) buf[len++] = static_cast<char>(b);
            }
            return std::string_view(buf, len);
        }

    default:
        return {};
    }
}

// =====================================================================
// Wire-format encode / decode (moved from FF_Primitives.cpp)
// =====================================================================

}  // namespace FastFHIR

// =====================================================================
// Global-scope helpers — 7-byte big-endian pack/unpack
// =====================================================================

static void _store_be56(uint8_t (&out)[7], uint64_t val) {
    for (int i = 6; i >= 0; --i) {
        out[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }
}

static uint64_t _load_be56(const uint8_t (&in)[7]) {
    uint64_t val = 0;
    for (int i = 0; i < 7; ++i)
        val = (val << 8) | in[i];
    return val;
}

// =====================================================================
// Global-scope functions (declared in FF_Primitives.hpp outside namespace)
// =====================================================================

uint32_t ENCODE_FF_CODEABLE_CONCEPT(BYTE* __base, Offset block_offset,
                                     Offset& child_off,
                                     const std::string& code_str,
                                     FF_ExternalCodeSystem system,
                                     uint32_t /*version*/) {
    if (code_str.empty()) return FF_CODE_NULL;

    Offset cc_offset = child_off;
    child_off += FF_CODEABLE_CONCEPT::HEADER_SIZE;

    auto* ptr = __base + cc_offset;
    STORE_U64(ptr + FF_CODEABLE_CONCEPT::VALIDATION, cc_offset);
    STORE_U16(ptr + FF_CODEABLE_CONCEPT::RECOVERY,
              FF_CODEABLE_CONCEPT::recovery);
    STORE_U8(ptr + FF_CODEABLE_CONCEPT::SYSTEM,
             static_cast<uint8_t>(system));

    // Pack the code string via the enriched lookup path.
    uint64_t packed_id = FastFHIR::FF_PackCode(system, code_str);
    uint8_t wire[7];
    _store_be56(wire, packed_id);
    std::memcpy(ptr + FF_CODEABLE_CONCEPT::CODE, wire, 7); // TODO: Memcopy doesn't belong here. It's for assignment and should live in the FF_UTILITIES

    // Compute 30-bit signed relative offset.
    int64_t rel = static_cast<int64_t>(cc_offset) - static_cast<int64_t>(block_offset);
    if (rel < -0x20000000LL || rel > 0x1FFFFFFFLL) {
        throw std::runtime_error("FastFHIR: CodeableConcept offset exceeds ±512 MB.");
    }
    uint32_t packed_off = (static_cast<uint32_t>(static_cast<int32_t>(rel)) & 0x3FFFFFFFu)
                        | FF_CODEABLE_CONCEPT_FLAG;
    return packed_off;
}

void FF_CODEABLE_CONCEPT::code_bytes(const BYTE* base, uint8_t (&out)[7]) const noexcept {
    std::memcpy(out, base + __offset + CODE, 7);
}

std::string_view FF_DECODE_CODEABLE_CONCEPT(const BYTE* base, Offset offset,
                                             uint32_t /*version*/) {
    FF_CODEABLE_CONCEPT cc(offset, 0, 0);
    auto sys = cc.system(base);
    uint8_t wire[7];
    cc.code_bytes(base, wire);
    uint64_t packed_id = _load_be56(wire);
    return FastFHIR::FF_UnpackCode(sys, packed_id);
}
