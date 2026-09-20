/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FF_BundleIndex.hpp"
#include "FF_FieldKeys.hpp"

namespace FastFHIR
{

namespace
{

/// The `Type/id` tail of an absolute URL, or empty when there is not one.
///
/// FHIR resolves a RELATIVE reference inside a Bundle against the entry whose
/// absolute fullUrl ends in that same `Type/id`, so the relative key is derived
/// from the fullUrl rather than from the resource. That keeps one source of
/// truth: an entry is reachable by what its fullUrl says, not by what its
/// payload separately claims its id to be.
///
/// A `urn:` fullUrl has no such tail. `urn:uuid:a-b-c` contains no '/' at all,
/// and a urn is matched whole by FHIR, so returning empty here is the rule and
/// not a limitation.
[[nodiscard]] std::string_view relative_key(std::string_view full_url) noexcept
{
    if (full_url.rfind("urn:", 0) == 0) return {};

    const std::size_t id_slash = full_url.rfind('/');
    if (id_slash == std::string_view::npos || id_slash + 1 >= full_url.size()) return {};
    // id_slash - 1 wraps to SIZE_MAX when id_slash is 0. rfind clamps a huge
    // pos back to the whole string, so it is defined rather than broken -- but
    // relying on the wrap reads as a bug. Returning early is belt-and-braces:
    // the Type/id shape check below rejects the result either way, since a
    // string whose only slash is at the front leaves no separator in the key.
    if (id_slash == 0) return {};

    const std::size_t type_slash = full_url.rfind('/', id_slash - 1);
    if (type_slash == std::string_view::npos) return {};

    // The key must be exactly `Type/id`: one separator, neither side empty.
    // Checked on the result rather than on the input, so every odd shape --
    // "http://host", "http://host/", "//x" -- is rejected by one rule.
    const std::string_view key = full_url.substr(type_slash + 1);
    const std::size_t sep = key.find('/');
    if (sep == std::string_view::npos || sep == 0 || sep + 1 >= key.size()) return {};
    return key;
}

}  // namespace

BundleIndex::BundleIndex(const Reflective::Node &bundle)
{
    const auto entries = bundle[Fields::BUNDLE::ENTRY];
    if (!entries) return;

    const std::size_t count = entries.size();
    m_targets.reserve(count * 2);  // fullUrl plus at most one relative key each

    for (std::size_t i = 0; i < count; ++i)
    {
        const Reflective::Node entry = entries[i];
        if (!entry) continue;

        const auto url_entry = entry[Fields::BUNDLE_ENTRY::FULL_URL];
        if (!url_entry) continue;
        const std::string_view full_url = url_entry;
        if (full_url.empty()) continue;

        // The resource is a 10-byte {offset, tag} tuple, so a falsy node here
        // means the slot is absent or the target did not vouch for itself.
        // Either way there is nothing to resolve TO, so it is not a target.
        const Reflective::Node resource = entry[Fields::BUNDLE_ENTRY::RESOURCE].as_node();
        if (!resource) continue;

        // First writer wins on an exact fullUrl. Two entries sharing one is a
        // malformed Bundle, and taking the first is what the linear scan this
        // replaces did. m_indexed counts DISTINCT targets, so it does not grow
        // on the duplicate.
        if (m_targets.emplace(full_url, resource).second) ++m_indexed;

        const std::string_view relative = relative_key(full_url);
        if (relative.empty()) continue;

        // A relative key two entries both answer names neither of them. Map it
        // to a default Node -- falsy -- so resolve() reports it exactly as it
        // reports an absent key, rather than silently returning whichever entry
        // happened to be indexed first. No comparison is needed to detect this:
        // each entry is visited once, and a fullUrl that IS already relative
        // (`Patient/123`, no host) yields no relative key at all, so a second
        // arrival at the same key is always a second entry.
        const auto [it, inserted] = m_targets.emplace(relative, resource);
        if (!inserted) it->second = Reflective::Node{};
    }
}

Reflective::Node BundleIndex::resolve(std::string_view reference) const
{
    if (reference.empty()) return {};
    const auto it = m_targets.find(reference);
    return it == m_targets.end() ? Reflective::Node{} : it->second;
}

Reflective::Node BundleIndex::resolve(const Reflective::Node &reference) const
{
    const auto text = reference[Fields::REFERENCE::REFERENCE];
    if (!text) return {};
    return resolve(static_cast<std::string_view>(text));
}

}  // namespace FastFHIR
