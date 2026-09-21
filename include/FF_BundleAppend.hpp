/**
 * @file FF_BundleAppend.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief Bundle.entry as a tail structure: serialize it last, append to it by
 *        rewriting the tail (TASKS.md APPEND-1).
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 *
 * FastFHIR builds an array one of two ways, and both are first-class (README
 * Example 6):
 *
 *   - allocate-then-backfill: append the Bundle with N empty entries, then let
 *     workers assign each slot. The array lands at the FRONT of the stream.
 *   - collect-then-serialize: keep a std::vector<BundleentryData> in memory,
 *     then serialize it after the fact. The array lands at the TAIL.
 *
 * This header is the tail half. `serialize_bundle_array` writes a finished
 * entry vector as one inline array. `FF_BundleAppendEntries` appends
 * resources to an EXISTING Bundle and keeps its entry array at the tail:
 *
 *     before   [resources ... | Bundle | entry[N] | checksum]
 *     after    [resources ... | Bundle | new resources | entry[N+1] | checksum']
 *
 * The write head is rolled back to the old array, the new resources are
 * written where it was, and the N+1 array is written after them. The stream
 * grows by the new resources plus one 84-byte entry each -- the minimum an
 * added entry can cost -- and nothing is orphaned. N x 84 bytes are rewritten
 * in place.
 *
 * When the array is NOT the last thing in the payload (a backfilled stream,
 * or a Bundle with `signature`/`issues` written after its entries), rolling
 * back would destroy live data, so the append RELOCATES instead: the resources
 * and the N+1 array are appended at the head and Bundle.entry is re-pointed,
 * leaving the old array unreferenced. That costs one N x 84-byte copy, once;
 * the relocated array is at the tail, so the next append rewrites it.
 */
#pragma once

#include "FastFHIR.hpp"
#include "FF_Bundle.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace FastFHIR {

/**
 * @brief Serialize a finished entry vector as ONE inline Bundle.entry array at
 *        the write head, and return the array's offset.
 *
 * The collect-then-serialize primitive (README 6a): every `resource` in
 * `entries` must already be written to this builder's arena. Point a Bundle's
 * `entry` slot at the result (`amend_pointer` on an unassigned slot) or let
 * `FF_BundleAppendEntries` do it.
 *
 * Thread-safe in the same sense as `Builder_t::append`: the claim is lock-free
 * and nothing else is touched. Throws on a SIZE/STORE disagreement.
 *
 * @return FF_NULL_OFFSET for an empty vector (an absent array has no block).
 */
FF_EXPORT Offset serialize_bundle_array(Builder_t& builder, const std::vector<BundleentryData>& entries);

/**
 * @brief Where an append put things. Offsets are absolute in the arena.
 */
struct FF_BundleAppendResult
{
    /// The Bundle.entry array now referenced by the root.
    Offset entry_array = FF_NULL_OFFSET;
    /// The array the root referenced before the call (FF_NULL_OFFSET if none).
    Offset previous_array = FF_NULL_OFFSET;
    /// First byte rewritten in place: the old array's offset on the tail path,
    /// FF_NULL_OFFSET when nothing was rolled back.
    Offset rewrite_from = FF_NULL_OFFSET;
    /// True when the old array was not at the tail and was left unreferenced.
    bool relocated = false;
    /// True when the entries' child data (fullUrl, request, ...) had to be
    /// copied aside before the rollback could overwrite it.
    bool copied_children = false;
    /// Entries in the new array.
    uint32_t entry_count = 0;
};

/**
 * @brief Append resources to the root Bundle's entries, keeping the array at
 *        the tail (TASKS.md APPEND-1).
 *
 * `append` is called exactly once, AFTER any rollback. It writes the new
 * resources through the builder it is handed and pushes one BundleentryData
 * per resource onto `new_entries`, which arrives empty. The new array is the
 * existing entries, in order, followed by `new_entries`. (The existing ones
 * are kept apart so a throwing `append` cannot damage them -- they are what
 * the failure path restores.)
 *
 * Requirements -- the call refuses or relocates rather than guess:
 *   - the builder's root is a Bundle (open the Builder on the sealed stream,
 *     which hydrates the root and rewinds over the old checksum block);
 *   - the call is EXCLUSIVE: no other thread may append, amend or finalize on
 *     this builder until it returns. Threads started inside `append` are fine.
 *
 * Not crash-atomic. Between the rollback and the reseal the old array is
 * gone; the resources below it are intact, so a torn tail is recoverable by a
 * resource scan. If `append` throws, the existing entries are re-serialized
 * at the head and Bundle.entry is pointed at them before the error is
 * returned, so the stream stays consistent (the partial resources are
 * orphaned).
 *
 * Reseal afterwards with FF_BuilderFinalize, as for any mutation.
 */
struct FF_BundleAppendInfo
{
    FF_Builder builder;
    std::function<void(Builder_t& builder, std::vector<BundleentryData>& new_entries)> append;
};

FF_EXPORT FF_Result FF_BundleAppendEntries(const FF_BundleAppendInfo& info,
                                           FF_BundleAppendResult& out_result) noexcept;

} // namespace FastFHIR
