/**
 * @file FF_BundleIndex.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief Resolve Bundle references once instead of scanning per reference.
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 *
 * A FHIR Bundle is a set, not an index: `Observation.subject` holds the TEXT
 * `"urn:uuid:..."`, and finding what it names means comparing that text against
 * every `Bundle.entry.fullUrl`. One scan is O(N); a consumer reading four
 * references per Observation across N entries does O(N*M) of them.
 *
 * Reading each `fullUrl` is already cheap -- it comes straight out of the arena
 * with no DOM in between -- so what is expensive is the COUNT of comparisons,
 * not their cost. This builds the map once and turns every later resolution
 * into a hash lookup.
 *
 * THIS ALLOCATES, and it is the only part of the read path that does. README §1
 * promises allocation-free navigation and that still holds: `Node` traversal
 * allocates nothing, and this is an opt-in index a caller constructs
 * deliberately. One unordered_map; the keys are views into the arena and the
 * values are 48-byte Nodes, so no string or resource is copied.
 *
 * LIFETIME: the index borrows the arena, exactly as a `Node` does. It must not
 * outlive the `Parser` (or `Memory`) the Bundle was read from.
 *
 * WHAT RESOLVES, and what a FHIR Bundle says should:
 *
 *   urn:uuid:... / urn:oid:...   exact match against entry.fullUrl
 *   http://host/base/Type/id     exact match against entry.fullUrl
 *   Type/id (relative)           matches an entry whose absolute fullUrl ends
 *                                in `/Type/id` -- the resolution FHIR defines
 *                                for a relative reference inside a Bundle
 *
 * A relative key that two entries would both answer is AMBIGUOUS and resolves
 * to nothing rather than to whichever was indexed first. Anything unresolved --
 * absent, external, or ambiguous -- comes back as a falsy `Node`, because the
 * read path degrades and does not throw (CLAUDE.md invariant 10).
 *
 * Interning `fullUrl` and `Reference.reference` into the URL directory (URL-1)
 * would make a resolution a uint32 compare and this map unnecessary. That is
 * not in scope here; this is the version that works against the wire as it is.
 */
#pragma once

#include "FastFHIR.hpp"

#include <cstddef>
#include <string_view>
#include <unordered_map>

namespace FastFHIR
{

class FF_EXPORT BundleIndex
{
public:
    /**
     * @brief Index every entry of @p bundle by `fullUrl`.
     *
     * A node that is not a Bundle, or a Bundle with no entries, yields an empty
     * index rather than an error: an empty Bundle is a legal document, and a
     * caller that hands over the wrong node learns it from `empty()` or from a
     * falsy `resolve`. Entries without a `fullUrl`, and entries whose
     * `resource` is absent or does not vouch for itself, are skipped -- an
     * entry that cannot be reached is not a resolution target.
     */
    explicit BundleIndex(const Reflective::Node &bundle);

    /**
     * @brief The resource @p reference names, or a falsy Node.
     * @param reference The text of `Reference.reference`, e.g. `"urn:uuid:..."`.
     */
    [[nodiscard]] Reflective::Node resolve(std::string_view reference) const;

    /**
     * @brief The resource a `Reference` block points at, or a falsy Node.
     *
     * The overload that takes the block rather than the text, so a caller
     * writes `index.resolve(observation[Fields::OBSERVATION::SUBJECT])` instead
     * of reaching through `Fields::REFERENCE::REFERENCE` itself. A block with
     * no `reference` (a logical reference carrying only `identifier`) resolves
     * to nothing, which is correct: it never named a target in this stream.
     */
    [[nodiscard]] Reflective::Node resolve(const Reflective::Node &reference) const;

    /// Entries indexed. Not `Bundle.entry.size()`: entries without a usable
    /// `fullUrl` or `resource` are not targets and are not counted.
    [[nodiscard]] std::size_t size() const noexcept { return m_indexed; }
    [[nodiscard]] bool empty() const noexcept { return m_indexed == 0; }

private:
    /// fullUrl (and the relative `Type/id` it implies) -> resource.
    /// An AMBIGUOUS relative key maps to a default-constructed Node, which is
    /// falsy -- so `resolve` reports it exactly as it reports an absent one,
    /// and no second container is needed to remember the ambiguity.
    std::unordered_map<std::string_view, Reflective::Node> m_targets;
    std::size_t m_indexed = 0;
};

}  // namespace FastFHIR

/// Global alias for consumers outside the namespace, matching FF_String and
/// FF_Optional. The FF_ prefix is the namespace when there is not one.
using FF_BundleIndex = FastFHIR::BundleIndex;
