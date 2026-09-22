/**
 * @file FF_StreamCheck.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief The whole-stream reference check — the one conformance question a
 *        single block cannot answer.
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 *
 * Every other check in the conformance layer looks at ONE block as it is about
 * to be written. "Does every reference that should resolve actually resolve" is
 * not that kind of question: it needs the whole entry set, and during a
 * concurrent build the target may simply not have been appended yet. So it runs
 * at finalize, after the mutators have drained, and it is the ONE hook that
 * looks at the finished document rather than at a block on its way in.
 *
 * The classification is by reference FORM, because that is what decides whether
 * a reference was ever meant to resolve here:
 *
 *   urn:uuid:... / urn:oid:...   a temporary identifier, meaningful ONLY
 *                                 inside the Bundle that defines it, so an
 *                                 unresolved one is a broken document
 *   http(s)://...                 external by construction; nothing to check
 *   Type/id (relative)            may resolve on the receiving server, so an
 *                                 unresolved one is a warning, not an error
 *   identifier only               a logical reference that never named a target
 *
 * This header declares the hook; FF_StreamCheck.cpp implements it. It is INTERNAL
 * to the conformance library — reachable from the generated layer's TU, not from
 * FastFHIR.hpp — because it needs the generated reflection tables and the
 * BundleIndex, neither of which the core library's public surface exposes.
 */
#pragma once

#include "FF_Conformance_internal.hpp"

#include <cstdint>

namespace FastFHIR
{
namespace Conformance
{

/**
 * @brief Report every reference in a Bundle that should resolve and does not.
 *
 * Attached as `ValidationHooks::stream_check` and invoked once by
 * `Builder::finalize` through `dispatch_stream()`. Reads the arena and writes
 * nothing, so a stream finalized with this attached is byte-identical to one
 * finalized without it — the same guarantee the per-block checks make.
 *
 * A stream not rooted at a Bundle is not this check's subject: there is no entry
 * set to resolve against, so it returns OK rather than reporting every reference
 * as unresolved.
 *
 * @param info  The whole-stream arguments: the arena and its length, the FHIR
 *              revision, the root block's offset and tag, and `self` — the layer
 *              that owns this check, for its sink and failure counter. Stamped
 *              by dispatch_stream(); see FF_Conformance.hpp.
 * @return The FIRST unresolved `urn:` reference, or OK when there is none. A
 *         `urn:` that does not resolve is an error; a relative reference that
 *         does not resolve is reported through `self` and counted, but is not
 *         returned — a warning must not stop a finalize that is otherwise fine.
 */
[[nodiscard]] Status check_stream_references(const StreamCheckInfo& info);

} // namespace Conformance
} // namespace FastFHIR
