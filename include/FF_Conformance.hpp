/**
 * @file FF_Conformance.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief Attachable FHIR conformance layer — the boundary types.
 * @version 0.1
 * @date 2026-09-09
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 *
 * VALIDATION IN FASTFHIR IS SPLIT IN TWO, AND THE SPLIT IS THE POINT.
 *
 *   1. STRUCTURAL validation is inline and mandatory. A block sits at its own
 *      offset, carries the right RECOVERY_TAG, and fits inside the arena. The
 *      Builder and Parser do this unconditionally. Wrong here means bytes that
 *      cannot be read.
 *   2. CONFORMANCE validation is what this header declares: optional, attached
 *      at runtime, and generated from the HL7 StructureDefinitions. Cardinality,
 *      required fields, bound ValueSets. Wrong here means a perfectly readable
 *      file that a FHIR server rejects.
 *
 * The layer OBSERVES; it never encodes. A stream written with a layer attached
 * is byte-identical to one written without it — the check runs BEFORE any arena
 * space is claimed, so even a rejected write leaves the stream untouched.
 *
 * Borrowed from Vulkan, and from the Iris File Extension's layer before it:
 * attachment decided once, a `next` pointer so layers compose rather than
 * compete, and diagnostics through a caller-owned sink so the layer never owns
 * I/O policy. NOT borrowed: the loader, trampolines and manifest discovery.
 * Block J's runtime-discovered terminology layers populate THIS struct; they do
 * not introduce a second mechanism.
 *
 * This header deliberately pulls in no FastFHIR headers. RECOVERY_TAG is
 * declared opaquely (a fixed underlying type makes it a complete type) and
 * ConcurrentLogger is forward declared, so the boundary can be compiled and
 * reasoned about on its own.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <type_traits>

/// The permanent 2-byte block-type identifier. Declared opaquely on purpose:
/// see the file comment. Defined in generated_src/FF_RecoveryTags.hpp.
enum RECOVERY_TAG : uint16_t;

namespace FastFHIR
{
class ConcurrentLogger;

namespace Conformance
{

/// Bumped when the layout or meaning of anything below changes. A layer
/// compiled against a different value is refused at attach time rather than
/// misread at dispatch time — a runtime-loaded Block J layer is the case this
/// exists for, and refusing loudly is the whole reason the field is first.
inline constexpr uint32_t FF_CONFORMANCE_ABI = 1;

/// Which FHIR revisions a Rule applies to. FHIR_VERSION's own values (0x0400,
/// 0x0500) are ordinals, not flags, so a rule that holds for both revisions
/// could not be spelled with them. R4 and R5 genuinely disagree about some
/// cardinalities, and a rule emitted from one revision must not be enforced
/// against a document written as the other.
inline constexpr uint8_t FF_CONF_VERSION_R4  = 1u << 0;
inline constexpr uint8_t FF_CONF_VERSION_R5  = 1u << 1;
inline constexpr uint8_t FF_CONF_VERSION_ALL = FF_CONF_VERSION_R4 | FF_CONF_VERSION_R5;

/// Maps a FHIR_VERSION value onto its Rule bit.
///
/// An unrecognised revision answers ALL rather than none: a future R6 should
/// run the checks we have and report what it finds, not silently pass
/// everything. Silent degradation is the failure mode this project has paid
/// for repeatedly.
[[nodiscard]] inline constexpr uint8_t version_bit(uint32_t fhir_version) noexcept
{
    if (fhir_version == 0x0400) return FF_CONF_VERSION_R4;
    if (fhir_version == 0x0500) return FF_CONF_VERSION_R5;
    return FF_CONF_VERSION_ALL;
}

/// What went wrong. Structural codes are deliberately absent: those failures
/// are raised by the Builder and Parser whether or not a layer is attached, and
/// duplicating them here would suggest this layer is load-bearing for them.
///
/// This vocabulary lists only what something actually produces. A layer that
/// needs a new code bumps FF_CONFORMANCE_ABI along with it, which is the whole
/// reason that field exists — an enumerator nobody emits is indistinguishable
/// from one that is broken.
enum class Check : uint8_t
{
    OK = 0,
    REQUIRED_MISSING,  ///< an element with min >= 1 is absent
    CARDINALITY_MAX,   ///< an array holds more elements than max allows
};

/// What a Rule row asks of a field.
///
/// UNIMPLEMENTED is a first-class member, not an omission: a conformance report
/// that cannot distinguish "checked and passed" from "deliberately not checked"
/// from "forgotten" is worth much less than one that can. Everything the
/// generator declines to enforce becomes one of these, carrying its key, its
/// human text, and its bound ValueSet where it has one — queryable through
/// conformance_rules(), so what was NOT checked is as visible as what was.
///
/// Three things are deliberately UNIMPLEMENTED rather than checked:
///   * every `constraint[]` invariant, because its expression is FHIRPath and a
///     partial evaluator inside an emitter is how a schema becomes a
///     programming language;
///   * `fixed[x]` / `pattern[x]`, measured at ZERO elements across the compiled
///     profile — they live in profiles, and the build profile selects
///     resources, not profile constraints;
///   * required ValueSet bindings, which are terminology membership and
///     therefore Block J's work, reached through this same struct.
enum class RuleKind : uint8_t
{
    REQUIRED,         ///< min >= 1: the element must be present
    MAX_CARDINALITY,  ///< an array's declared numeric upper bound
    UNIMPLEMENTED,    ///< recorded and reported, never enforced; see below
};

/// What the Builder does with a failure. The layer itself never decides — it
/// reports a Status and stays noexcept; this says how the API boundary reacts.
enum class LayerPolicy : uint8_t
{
    Throw,   ///< std::runtime_error, prefixed "FastFHIR: " (the write-path convention)
    Report,  ///< write to the sink, count it, and continue
};

/// One generated check, as data rather than as code.
///
/// `ordinal` is the field's position in the block's generated visit_fields()
/// order, so matching a rule to a field is an integer compare and never a
/// string compare. `path`, `key`, `human` and `url` all point at static storage
/// in the generated layer, which is what lets the failure path allocate nothing.
struct Rule
{
    const char* path    = "";  ///< "Observation.status"
    const char* key     = "";  ///< FHIR invariant key ("pat-1"), or "" if none
    const char* human   = "";  ///< the specification's own wording, verbatim
    const char* url     = "";  ///< the resource's canonical StructureDefinition URL
    const char* binding = "";  ///< bound ValueSet URL where the rule has one, else ""
    uint32_t min        = 0;   ///< REQUIRED: the declared minimum
    uint32_t max        = 0;   ///< MAX_CARDINALITY: the declared maximum
    uint16_t ordinal    = 0;   ///< visit_fields() position of the field
    RuleKind kind       = RuleKind::UNIMPLEMENTED;
    uint8_t  versions   = FF_CONF_VERSION_ALL;
};

/// Everything needed to describe a failure, unformatted.
///
/// No arena offset, unlike the Iris File Extension's equivalent: the check runs
/// before claim_space(), so at the moment a Status is produced the block has no
/// address to name. The FHIR path is the locator instead, and it is the one a
/// clinical user can act on.
struct Status
{
    Check        code  = Check::OK;
    RECOVERY_TAG block = RECOVERY_TAG{};
    const char*  path  = "";
    const char*  key   = "";
    const char*  human = "";
    const char*  url   = "";
    uint64_t     found    = 0;
    uint64_t     expected = 0;

    /// How the layer that produced this failure wants the API boundary to
    /// react. Stamped by dispatch() from the failing layer, NOT from the one
    /// the caller attached: a Report-policy terminology layer chained behind a
    /// Throw-policy conformance layer must not inherit the throw. Meaningless
    /// when `code` is OK.
    LayerPolicy policy = LayerPolicy::Throw;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return code == Check::OK; }
};

struct ValidationHooks;

/// One block type's check. Type-erased at exactly this one point: the caller
/// resolves the tag from TypeTraits<T>::recovery and the callee casts back to
/// the same T, and both halves are emitted by the same generator run.
using CheckFn = Status (*)(const void* data, uint32_t fhir_version,
                           const ValidationHooks* self) noexcept;

/// A tag and its check. Tables are sorted by tag so find() is a binary search.
struct Entry
{
    RECOVERY_TAG tag;
    CheckFn      check;
};

/// The attachable layer.
///
/// Copy this struct before setting `policy`, `next`, `diagnostic` or
/// `failures` — conformance_layer() returns a shared immutable instance, and
/// the pointers you attach are yours, not its.
///
/// Every pointer here is BORROWED and must outlive the Builder it is attached
/// to. Nothing in this struct owns anything.
struct ValidationHooks
{
    /// First, and first for a reason: a runtime-loaded layer's ABI is checked
    /// before any other field is trusted.
    uint32_t abi_version = FF_CONFORMANCE_ABI;

    const Entry* entries = nullptr;  ///< sorted by tag, ascending
    uint32_t     count   = 0;

    LayerPolicy policy = LayerPolicy::Throw;

    /// The next layer, or null. Walked once, at the top-level dispatch; a
    /// layer's own descent into nested blocks stays within that layer.
    const ValidationHooks* next = nullptr;

    /// Where diagnostics go, if anywhere. Lock-free by requirement, not by
    /// preference: appends run concurrently on the ingest worker pool, so a
    /// plain string sink would be a data race. Null stays silent — the Status
    /// still carries the failure.
    ConcurrentLogger* diagnostic = nullptr;

    /// Incremented once per reported failure, so a caller can see "1,412 codes
    /// failed" without scraping log text. Null to not count.
    std::atomic<uint64_t>* failures = nullptr;

    /// The check for `tag` in THIS layer, or null if this layer has no opinion
    /// about that block type. Binary search; entries must be sorted.
    [[nodiscard]] const Entry* find(RECOVERY_TAG tag) const noexcept
    {
        const Entry* const last = entries + count;
        const Entry* const it   = std::lower_bound(
            entries, last, tag,
            [](const Entry& e, RECOVERY_TAG t) noexcept { return e.tag < t; });
        return (it != last && it->tag == tag) ? it : nullptr;
    }
};

/// Runs `tag`'s check through every layer in the chain, stopping at the first
/// failure.
///
/// The chain walk lives HERE and nowhere else. The Iris File Extension repeats
/// its chain tail at the bottom of every generated check, which means a fix to
/// the traversal reaches one function; a generated check in FastFHIR returns
/// its own verdict and knows nothing about layering.
///
/// A LAYER REPORTS ITS OWN FAILURES. Writing to `diagnostic` and incrementing
/// `failures` is the check's job, because only the check knows which of the
/// chained layers' sinks is the right one. What comes back here is the verdict
/// alone, carrying the policy that decides whether the Builder throws.
[[nodiscard]] inline Status dispatch(const ValidationHooks* head, RECOVERY_TAG tag,
                                     const void* data, uint32_t fhir_version) noexcept
{
    for (const ValidationHooks* layer = head; layer != nullptr; layer = layer->next)
    {
        const Entry* const entry = layer->find(tag);
        if (entry == nullptr) continue;
        Status status = entry->check(data, fhir_version, layer);
        if (!status)
        {
            status.policy = layer->policy;
            return status;
        }
    }
    return {};
}

// The boundary is data: it crosses a library seam, and a runtime-loaded layer
// may be compiled by another toolchain entirely. Anything with a non-trivial
// copy is a layout question waiting to become a crash.
static_assert(std::is_trivially_copyable_v<Rule>);
static_assert(std::is_trivially_copyable_v<Status>);
static_assert(std::is_trivially_copyable_v<Entry>);
static_assert(std::is_trivially_copyable_v<ValidationHooks>);

} // namespace Conformance
} // namespace FastFHIR
