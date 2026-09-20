/**
 * @file FF_ConformanceEngine.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief The hand-written half of the conformance layer: one engine, driven by generated tables.
 * @version 0.1
 * @date 2026-09-09
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 *
 * INTERNAL to the conformance library. It is not reachable from FastFHIR.hpp and
 * must not be: it needs the generated visit_fields() and TypeTraits<>, which the
 * core library's public surface does not expose.
 *
 * WHY AN ENGINE AND NOT EMITTED CODE. The Iris File Extension emits the check
 * body for every field of every block. At 18 blocks that reads well. At 209
 * blocks and ~6,000 fields it would be a megabyte of near-identical C++, and
 * every fix to a check would be a regeneration rather than an edit. So the
 * generator emits DATA — a Rule[] per block — and this file is the only code
 * that interprets it. One implementation of "is this field absent", one walk,
 * one place a bug can live.
 *
 * The engine reaches a POCO's fields through the generated visit_fields(), so a
 * Rule addresses its field by ORDINAL: the field's position in that walk, which
 * is emitted from the same layout list that emits the walk itself. Matching is
 * an integer compare and can never drift into a string compare.
 */
#pragma once

#include "FF_Conformance.hpp"
#include "FF_Logger.hpp"
// TypeTraits<T>::recovery is the tag a block dispatches on, and it is declared
// here. Included by name rather than relied upon transitively: the generated
// layer includes this engine BEFORE any resource header, so an implicit
// dependency would break on include order alone.
#include "FF_Parser.hpp"
#include "FF_Primitives.hpp"

#include <bit>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace FastFHIR
{
namespace Conformance
{

// =====================================================================
// Absence — one overload set, and a missing overload is a COMPILE ERROR
// =====================================================================
// architecture.md §3.4 is the contract: every FastFHIR sentinel is all-ones at
// the slot's width, generated enums spell absence as FF_UNSET = 255, and the
// double sentinel is a quiet NaN that must be compared on BITS because NaN is
// unordered against everything including itself.
//
// The set is deliberately closed. A POCO member type with no overload here does
// not compile, which is the coverage gate: a new field type cannot slip through
// as "present" by default. `bool` is excluded from the integral overload for
// exactly that reason -- no POCO has one today, and if one appears, all-ones
// would silently read `true` as absent.

[[nodiscard]] inline bool is_absent(std::string_view v) noexcept { return v.empty(); }
[[nodiscard]] inline bool is_absent(const std::string& v) noexcept { return v.empty(); }

/// Every string-like POCO member. Exact, rather than leaning on String's
/// implicit conversion to string_view: an exact overload cannot be made
/// ambiguous later by a second conversion, and this header is the coverage
/// gate -- a member type with no matching overload must fail to compile.
[[nodiscard]] inline bool is_absent(const FastFHIR::String& v) noexcept { return v.empty(); }

/// A 0..1 child block. FF_Optional, not std::unique_ptr: the POCO member type
/// changed when the structs became values, and this overload follows it.
template <typename T>
[[nodiscard]] inline bool is_absent(const FF_Optional<T>& v) noexcept
{
    return v == nullptr;
}

template <typename T>
[[nodiscard]] inline bool is_absent(const std::vector<T>& v) noexcept
{
    return v.empty();
}

/// All-ones at the member's own width. Signed and unsigned alike: the sentinel
/// is a bit pattern, not a number, so it is spelled as one.
template <typename T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
[[nodiscard]] inline constexpr bool is_absent(T v) noexcept
{
    return v == static_cast<T>(~static_cast<std::make_unsigned_t<T>>(0));
}

/// Every generated ValueSet enum is `: uint8_t` with FF_UNSET = 255. Constrained
/// on the underlying type so a wider enum -- RECOVERY_TAG, say -- cannot match
/// this by accident and be read as a code.
template <typename E>
    requires(std::is_enum_v<E> && std::is_same_v<std::underlying_type_t<E>, uint8_t>)
[[nodiscard]] inline constexpr bool is_absent(E v) noexcept
{
    return static_cast<uint8_t>(v) == 0xFFu;
}

/// The NaN sentinel, compared on bits. `v != FF_NULL_F64` is true for EVERY
/// value including the sentinel itself, which is the defect architecture.md
/// §3.4 records; std::bit_cast is the only correct spelling.
[[nodiscard]] inline bool is_absent(double v) noexcept
{
    return std::bit_cast<uint64_t>(v) == FF_NULL_UINT64;
}

/// A choice slot carries its type beside its value; no tag means no value.
[[nodiscard]] inline bool is_absent(const ChoiceEntry& v) noexcept
{
    return v.tag == FF_RECOVER_UNDEFINED;
}

[[nodiscard]] inline bool is_absent(const ResourceReference& v) noexcept
{
    return v.offset == FF_NULL_OFFSET;
}

// =====================================================================
// Cardinality
// =====================================================================
// Only a repeating element has a count. Everything else answers 1 when present
// and 0 when absent, so one rule shape covers both without a second table.

template <typename T>
[[nodiscard]] inline uint64_t element_count(const std::vector<T>& v) noexcept
{
    return v.size();
}

template <typename T>
[[nodiscard]] inline uint64_t element_count(const T& v) noexcept
{
    return is_absent(v) ? 0u : 1u;
}

// =====================================================================
// Descent
// =====================================================================
// A layer descends into its OWN table, never through `next`. Chaining is a
// property of the top-level dispatch (FF_Conformance.hpp): if every layer also
// walked the chain at every nesting level, a two-layer chain would visit a
// three-deep resource eight times.

/// True for a POCO the wire knows by tag. ResourceReference and the scalars fail
/// this, so descent skips them at COMPILE time rather than testing at runtime.
template <typename T>
concept HasRecoveryTag = requires { TypeTraits<T>::recovery; };

template <typename T>
[[nodiscard]] inline Status descend(const T& child, uint32_t fhir_version,
                                    const ValidationHooks* self) noexcept
{
    if constexpr (HasRecoveryTag<T>)
    {
        const Entry* const entry = self->find(TypeTraits<T>::recovery);
        if (entry != nullptr)
            return entry->check(&child, fhir_version, self);
    }
    return {};
}

template <typename T>
[[nodiscard]] inline Status descend(const FF_Optional<T>& child, uint32_t fhir_version,
                                    const ValidationHooks* self) noexcept
{
    return child == nullptr ? Status{} : descend(*child, fhir_version, self);
}

template <typename T>
[[nodiscard]] inline Status descend(const std::vector<T>& children, uint32_t fhir_version,
                                    const ValidationHooks* self) noexcept
{
    for (const T& child : children)
    {
        const Status status = descend(child, fhir_version, self);
        if (!status)
            return status;
    }
    return {};
}

// A leaf is not a block: nothing to descend into, and saying so as an overload
// keeps the caller free of `if constexpr` ladders.
[[nodiscard]] inline Status descend(std::string_view, uint32_t, const ValidationHooks*) noexcept
{
    return {};
}
[[nodiscard]] inline Status descend(const std::string&, uint32_t, const ValidationHooks*) noexcept
{
    return {};
}
[[nodiscard]] inline Status descend(const ChoiceEntry&, uint32_t, const ValidationHooks*) noexcept
{
    // A choice variant's block is a ChoiceBlock variant, reachable only through
    // the generated TU that completes it. Recorded as a gap rather than papered
    // over: a rule on a field INSIDE a choice variant will not fire today.
    return {};
}
[[nodiscard]] inline Status descend(const ResourceReference&, uint32_t,
                                    const ValidationHooks*) noexcept
{
    // The referenced resource was appended -- and therefore checked -- on its own
    // append() call, before the parent that names it could exist. Following it
    // here would check it a second time and report the same fault twice.
    return {};
}

// =====================================================================
// Reporting
// =====================================================================

/// Writes a failure to the layer's own sink and counts it.
///
/// The LAYER reports, not the Builder: in a chain only the layer that found the
/// fault knows which sink is its own. Every operand is static storage from the
/// generated tables, so this allocates nothing and is safe on an ingest worker
/// -- ConcurrentLogger::log claims its range with a lock-free CAS (LOG-1).
inline void report(const ValidationHooks* self, const Status& status) noexcept
{
    if (self == nullptr)
        return;
    if (self->diagnostic != nullptr)
        self->diagnostic->log(status.human);
    if (self->failures != nullptr)
        self->failures->fetch_add(1, std::memory_order_relaxed);
}

// =====================================================================
// The engine
// =====================================================================

/// Runs one block's rules over one POCO, then descends into its children.
///
/// Rules arrive sorted by ordinal and the walk visits ordinals in order, so the
/// two advance together: the cost is linear in fields plus rules, not their
/// product. The first failure wins and stops the walk -- reporting every fault
/// in a document is the job of a report over a finished archive, not of a check
/// standing between a caller and a write.
template <typename T>
[[nodiscard]] Status run_rules(const T& data, const Rule* rules, uint32_t rule_count,
                               uint32_t fhir_version, const ValidationHooks* self) noexcept
{
    const uint8_t active_version = version_bit(fhir_version);
    Status   failure{};
    uint16_t ordinal  = 0;
    uint32_t cursor   = 0;
    // Whether `failure` came back from a child, which has already reported it
    // through its own layer entry. Tracked rather than inferred from the tag:
    // Extension nests inside Extension, so "the failing block is my type" is
    // true of a child there too, and one fault would be logged once per level.
    bool     from_child = false;

    visit_fields(data, [&](const char*, const auto& value) noexcept {
        const uint16_t here = ordinal++;
        if (!failure)
            return;

        while (cursor < rule_count && rules[cursor].ordinal < here)
            ++cursor;

        for (uint32_t i = cursor; i < rule_count && rules[i].ordinal == here; ++i)
        {
            const Rule& rule = rules[i];
            if ((rule.versions & active_version) == 0)
                continue;
            // UNIMPLEMENTED rows are the record of what was NOT checked. They
            // are queried through conformance_rules(); they never fire.
            if (rule.kind == RuleKind::UNIMPLEMENTED)
                continue;

            const uint64_t count = element_count(value);
            if (rule.kind == RuleKind::REQUIRED && count < rule.min)
            {
                failure = {Check::REQUIRED_MISSING, TypeTraits<T>::recovery,
                           rule.path,  rule.key,    rule.human,
                           rule.url,   count,       rule.min};
                return;
            }
            if (rule.kind == RuleKind::MAX_CARDINALITY && count > rule.max)
            {
                failure = {Check::CARDINALITY_MAX, TypeTraits<T>::recovery,
                           rule.path, rule.key,    rule.human,
                           rule.url,  count,       rule.max};
                return;
            }
        }

        const Status nested = descend(value, fhir_version, self);
        if (!nested)
        {
            failure    = nested;
            from_child = true;
        }
    });

    // A nested failure was already reported where it was found; reporting it
    // again at every level up the stack would turn one fault into one line per
    // ancestor.
    if (!failure && !from_child)
        report(self, failure);
    return failure;
}

} // namespace Conformance
} // namespace FastFHIR
