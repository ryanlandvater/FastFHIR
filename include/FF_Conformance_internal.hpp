/**
 * @file FF_Conformance_internal.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief How the library CALLS a whole-stream conformance check.
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 *
 * INTERNAL. Reachable from the core library, from the conformance library, and
 * from a layer author who is implementing a `stream_check`. It is deliberately
 * NOT installed — `tests/install/install_smoke.cmake` asserts that, the same way
 * it does for `FF_Ops.hpp` and `FF_UrlDirectory.hpp`.
 *
 * WHY THE SPLIT. `FF_Conformance.hpp` is the public boundary, and the question a
 * consumer asks there is "is this stream conformant". The answer takes a
 * finished stream and nothing else, which is
 * `FastFHIR::Conformance::validate_stream(const Memory&)` in `FF_Validate.hpp`.
 * The arena base, its mapped length, the FHIR revision, the root offset and the
 * root tag are a different question — they are how the LIBRARY invokes a hook it
 * already holds, assembled from state only the Builder or the Parser has. A
 * consumer that had to gather those five values would be reimplementing
 * `finalize`, so publishing them as the entry point would document the wrong
 * one.
 *
 * `FF_Conformance.hpp` therefore declares `StreamCheckInfo` incomplete and
 * this header completes it. A `StreamCheckFn` — a pointer to a function taking a
 * reference to it — needs no definition to be declared or stored, so
 * `ValidationHooks` stays whole over there. Only code that BUILDS or READS one
 * includes this, and that is the library plus whoever writes a hook.
 *
 * Nothing here pulls in a FastFHIR header, so the ABI seam keeps the
 * standalone-compilable property `FF_Conformance.hpp` documents.
 */
#pragma once

#include "FF_Conformance.hpp"

#include <cstdint>
#include <type_traits>

namespace FastFHIR
{
namespace Conformance
{

/// The whole-stream check's arguments, bundled.
///
/// Six positional numbers, five of them interchangeable at a call site, is the
/// shape CONTRIBUTING §5 says to bundle rather than to pass — and bundling is
/// what lets this seam grow without changing every signature on it. The fields
/// are the raw values the hook works in rather than FastFHIR's types, because
/// this header pulls in no FastFHIR headers.
///
/// `self` is the layer OFFERING the check. dispatch_stream() stamps it per layer
/// as the chain is walked; a caller leaves it null.
struct StreamCheckInfo
{
    const void* arena         = nullptr;   ///< base of the arena the stream lives in
    uint64_t    arena_size    = 0;         ///< mapped length of `arena`
    uint32_t    fhir_version  = 0;         ///< the revision the stream was written as
    uint64_t    root_offset   = 0;         ///< offset of the document root block
    uint64_t    root_recovery = 0;         ///< RECOVERY_TAG of the document root block
    const ValidationHooks* self = nullptr; ///< the layer offering the check; set by dispatch_stream()
};

/// Runs the whole-stream check through every layer in the chain, stopping at the
/// first failure. The stream-level counterpart of dispatch(), with the same
/// chain walk, the same "a layer reports its own failures" rule, and the same
/// policy carried back on the verdict.
///
/// A layer that offers no stream check is skipped rather than treated as a
/// pass, so a chain of block-only layers runs nothing here -- which is right:
/// there is no stream-level question they have an opinion about.
///
/// `info.self` is stamped per layer as the walk descends, so each hook reports
/// through the layer that owns it rather than the one the caller attached.
///
/// NOT noexcept, and the reason is the whole difference between this and
/// dispatch(). A StreamCheckFn is allowed to allocate — the shipped one builds a
/// BundleIndex, because "does any entry name this reference" cannot be asked
/// until every resource is written — so the call below can throw std::bad_alloc.
/// Declaring the walk noexcept would convert that into std::terminate, which
/// takes a recoverable out-of-memory on a large document and makes it a process
/// death with no diagnostic. The throw propagates to Builder::finalize, whose
/// latch restores the builder on unwind, and the FF_ boundary turns it into an
/// FF_Result.
[[nodiscard]] inline Status dispatch_stream(const ValidationHooks* head,
                                            const StreamCheckInfo& info)
{
    for (const ValidationHooks* layer = head; layer != nullptr; layer = layer->next)
    {
        if (layer->stream_check == nullptr) continue;
        StreamCheckInfo call = info;
        call.self = layer;
        Status status = layer->stream_check(call);
        if (!status)
        {
            status.policy = layer->policy;
            return status;
        }
    }
    return {};
}

// Same rule as the types in FF_Conformance.hpp: this crosses a library seam, so
// a non-trivial copy is a layout question waiting to become a crash.
static_assert(std::is_trivially_copyable_v<StreamCheckInfo>);

} // namespace Conformance
} // namespace FastFHIR
