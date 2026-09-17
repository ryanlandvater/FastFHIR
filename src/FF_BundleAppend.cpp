/**
 * @file FF_BundleAppend.cpp
 * @brief serialize_bundle_array and the tail-rewrite append (TASKS.md APPEND-1).
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 */
#include "FF_BundleAppend.hpp"
#include "FF_Bundle_internal.hpp"

#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>

namespace FastFHIR {

namespace {

constexpr Size kEntryStep = FF_BUNDLE_ENTRY::HEADER_SIZE;

// Every FF_BUNDLE_ENTRY slot that points at child data STOREd after the
// entries, inside the array's own region. RESOURCE is not one: it is a
// (offset, type) tuple naming a block written elsewhere.
constexpr Size kEntryChildSlots[] = {
    FF_BUNDLE_ENTRY::ID,     FF_BUNDLE_ENTRY::EXTENSION, FF_BUNDLE_ENTRY::MODIFIEREXTENSION,
    FF_BUNDLE_ENTRY::LINK,   FF_BUNDLE_ENTRY::FULLURL,   FF_BUNDLE_ENTRY::SEARCH,
    FF_BUNDLE_ENTRY::REQUEST, FF_BUNDLE_ENTRY::RESPONSE,
};

ResourceReference load_resource(const BYTE* base, Offset entry)
{
    return ResourceReference{LOAD_U64(base + entry + FF_BUNDLE_ENTRY::RESOURCE),
                             FF_GET_RECOVERY_TAG(base, entry + FF_BUNDLE_ENTRY::RESOURCE)};
}

} // namespace

Offset serialize_bundle_array(Builder& builder, const std::vector<BundleentryData>& entries)
{
    if (entries.empty())
        return FF_NULL_OFFSET;
    if (entries.size() > UINT32_MAX)
        throw std::length_error("FastFHIR: serialize_bundle_array: more than 2^32-1 entries");

    const uint32_t version = static_cast<uint32_t>(builder.FhirVersion());
    const auto n = static_cast<uint32_t>(entries.size());

    // The same layout the generated STORE_FF_BUNDLE writes for Bundle.entry:
    // [FF_ARRAY header | entry[0..n) | each entry's children, in order].
    Size total = FF_ARRAY::HEADER_SIZE;
    for (const auto& entry : entries)
        total += SIZE_FF_BUNDLE_ENTRY(entry, version);

    const Offset start = builder.claim_child_space(total);
    BYTE* const base = builder.memory().base();

    Offset child = start;
    STORE_FF_ARRAY_HEADER(base, child, FF_ARRAY::INLINE_BLOCK, kEntryStep, n,
                          ToArrayTag(RECOVER_FF_BUNDLE_ENTRY));
    const Offset first = child;
    child += static_cast<Offset>(n) * kEntryStep;
    for (uint32_t i = 0; i < n; ++i)
        child = STORE_FF_BUNDLE_ENTRY(base, first + i * kEntryStep, child, entries[i], version);

    // The SIZE/STORE contract Builder::append enforces, for the same reason:
    // a disagreement would let the next claim overlap this array's tail.
    if (child != start + total) {
        throw std::runtime_error(
            "FastFHIR: serialize_bundle_array: SIZE/STORE contract violated: claimed " +
            std::to_string(total) + " bytes but store consumed " + std::to_string(child - start));
    }
    return start;
}

FF_Result FF_BundleAppendEntries(const FF_BundleAppendInfo& info,
                                 FF_BundleAppendResult& out_result) noexcept
{
    static constexpr const char* op = "FF_BundleAppendEntries";
    out_result = FF_BundleAppendResult{};
    if (!info.builder)
        return FF_Result{FF_INVALID_ARGUMENT, std::string(op) + ": null builder handle"};
    if (!info.append)
        return FF_Result{FF_INVALID_ARGUMENT, std::string(op) + ": null append callback"};

    try {
        Builder& b = *info.builder;

        if (!b.try_begin_mutation())
            return FF_Result{FF_FAILURE, std::string(op) + ": builder is finalizing"};
        struct Guard {
            Builder* self;
            ~Guard() { self->end_mutation(); }
        } guard{&b};

        // Rolling the head back under another thread's claim would hand it
        // bytes the rewrite is about to overwrite. Best-effort: this sees
        // mutators already in flight, not ones that start after it.
        if (b.m_active_mutators.load(std::memory_order_acquire) != 1)
            return FF_Result{FF_FAILURE, std::string(op) +
                                             ": another mutation is in progress; the call must be exclusive"};

        if (b.m_root_offset == FF_NULL_OFFSET || b.m_root_recovery != RECOVER_FF_BUNDLE)
            return FF_Result{FF_INVALID_ARGUMENT, std::string(op) + ": the builder's root is not a Bundle"};

        const BYTE* const base = b.m_base;
        const Offset root = b.m_root_offset;
        const Size head = b.m_memory.size();
        const auto version = static_cast<uint32_t>(b.m_fhir_rev);

        if (root > head || head - root < FF_BUNDLE::ENTRY + sizeof(Offset))
            return FF_Result{FF_FAILURE, std::string(op) + ": root Bundle lies outside the payload"};

        const Offset A = LOAD_U64(base + root + FF_BUNDLE::ENTRY);
        out_result.previous_array = A;

        // The existing entries, and (only when their child data must survive
        // an overwrite) a private copy of the region those views point into.
        std::vector<BundleentryData> entries;
        std::vector<BYTE> saved;
        bool at_tail = false;

        if (A != FF_NULL_OFFSET) {
            if (A < FF_HEADER::HEADER_SIZE || A > head || head - A < FF_ARRAY::HEADER_SIZE)
                return FF_Result{FF_FAILURE, std::string(op) + ": Bundle.entry points outside the payload"};

            const FF_ARRAY array(A, head, version);
            if (array.entry_kind(base) != FF_ARRAY::INLINE_BLOCK || array.entry_step(base) != kEntryStep)
                return FF_Result{FF_FAILURE, std::string(op) + ": Bundle.entry is not an inline entry array"};

            const uint32_t n = array.entry_count(base);
            const Offset first = A + FF_ARRAY::HEADER_SIZE;
            const Offset entries_end = first + static_cast<Offset>(n) * kEntryStep;
            if (entries_end > head)
                return FF_Result{FF_FAILURE, std::string(op) + ": Bundle.entry runs past the payload"};

            // Pass 1, over the live bytes: n x (9 loads). Decides everything
            // below without deserializing anything.
            bool children_free = true;    // no entry carries child data
            bool children_inside = true;  // all first-level child data is in (entries_end, head)
            bool resources_below = true;  // every resource was written before the array
            for (uint32_t i = 0; i < n; ++i) {
                const Offset e = first + i * kEntryStep;
                for (Size slot : kEntryChildSlots) {
                    const Offset c = LOAD_U64(base + e + slot);
                    if (c == FF_NULL_OFFSET)
                        continue;
                    children_free = false;
                    if (c < entries_end || c >= head)
                        children_inside = false;
                }
                const Offset r = LOAD_U64(base + e + FF_BUNDLE_ENTRY::RESOURCE);
                if (r != FF_NULL_OFFSET && r >= A)
                    resources_below = false;
            }
            const bool dirs_below =
                (b.m_url_dir_offset == FF_NULL_OFFSET || b.m_url_dir_offset < A) &&
                (b.m_module_reg_offset == FF_NULL_OFFSET || b.m_module_reg_offset < A);
            const bool candidate = root < A && resources_below && dirs_below;

            Size extent = 0;
            if (children_free) {
                // The common shape: entries are bare resource tuples, so the
                // light vector is n values and nothing can dangle.
                entries.resize(n);
                for (uint32_t i = 0; i < n; ++i)
                    entries[i].resource = load_resource(base, first + i * kEntryStep);
                extent = FF_ARRAY::HEADER_SIZE + static_cast<Size>(n) * kEntryStep;
            } else if (candidate && children_inside) {
                // The rollback will overwrite the children the views point
                // into, so deserialize from a copy of the region instead.
                // `shifted` is a base under which region offsets resolve into
                // the copy; every read the deserializer makes is at an offset
                // in [A, head), which pass 1 and the extent check bound.
                saved.assign(base + A, base + head);
                const BYTE* const shifted = reinterpret_cast<const BYTE*>(
                    reinterpret_cast<std::uintptr_t>(saved.data()) - static_cast<std::uintptr_t>(A));
                entries.reserve(n);
                extent = FF_ARRAY::HEADER_SIZE;
                for (uint32_t i = 0; i < n; ++i) {
                    entries.push_back(FF_BUNDLE_ENTRY::deserialize(shifted, first + i * kEntryStep, head, version));
                    extent += SIZE_FF_BUNDLE_ENTRY(entries.back(), version);
                }
                out_result.copied_children = true;
            } else {
                // Relocation overwrites nothing, so views into the live
                // arena stay valid for the whole call.
                entries.reserve(n);
                for (uint32_t i = 0; i < n; ++i)
                    entries.push_back(FF_BUNDLE_ENTRY::deserialize(base, first + i * kEntryStep, head, version));
            }

            // The array (with its children) must be the LAST thing in the
            // payload. Anything appended after it -- a Bundle.signature, an
            // amendment's children, another resource -- moves the head past
            // A + extent, and rolling back would destroy it.
            at_tail = candidate && extent != 0 && A + extent == head;
        }

        if (at_tail) {
            b.m_memory.reset(A);
            out_result.rewrite_from = A;
        } else if (A != FF_NULL_OFFSET) {
            out_result.relocated = true;
        }

        BYTE* const mutable_base = b.m_base;
        const auto point_entry_at = [&](Offset array_offset) {
            STORE_U64(mutable_base + root + FF_BUNDLE::ENTRY, array_offset);
        };

        std::vector<BundleentryData> new_entries;
        try {
            info.append(b, new_entries);
        } catch (...) {
            // Put the existing entries back where the root can see them. On
            // the tail path their old bytes may already be overwritten; the
            // vector (values, or views into `saved`) is still intact.
            if (A != FF_NULL_OFFSET)
                point_entry_at(serialize_bundle_array(b, entries));
            throw;
        }

        entries.insert(entries.end(), std::make_move_iterator(new_entries.begin()),
                       std::make_move_iterator(new_entries.end()));
        const Offset new_array = serialize_bundle_array(b, entries);
        // A sanctioned re-point of an assigned slot: on the tail path the old
        // target no longer exists, and on the relocation path orphaning it is
        // the documented cost. _amend_prepare's orphan guard does not apply.
        point_entry_at(new_array);

        out_result.entry_array = new_array;
        out_result.entry_count = static_cast<uint32_t>(entries.size());
        return FF_Result{FF_SUCCESS};
    } catch (const std::exception& e) {
        return FF_Result{FF_FAILURE, std::string(op) + ": " + e.what()};
    } catch (...) {
        return FF_Result{FF_FAILURE, std::string(op) + ": unknown non-std exception"};
    }
}

} // namespace FastFHIR
