/**
 * @file FF_Validate.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief Conformance-check a finished stream — the consumer entry point.
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 *
 * The conformance layer is two tools wearing one name, and this header is the
 * half a consumer uses.
 *
 * INSIDE the library it is a WRITE-path layer: `Builder::attach_layer` hangs it
 * off the builder, every append runs the per-block checks before `claim_space`,
 * and `finalize` runs the whole-stream check once the mutators have drained.
 * That path knows the arena base, the mapped length, the FHIR revision and the
 * root, because it is the thing that produced them.
 *
 * OUTSIDE the library it is a READ-path tool: someone holds a `.ffhr` that
 * arrived from somewhere else and wants to know whether a FHIR server will
 * accept it. That caller has a `Memory` and nothing else, so that is all this
 * asks for — the five values the internal hook takes are read out of the
 * stream's own header here rather than demanded from the caller.
 *
 * ```cpp
 * FastFHIR::Memory stream = FastFHIR::Memory::createFromFile("bundle.ffhr");
 * if (const auto status = FastFHIR::Conformance::validate_stream(stream); !status)
 *     std::cerr << status.path << ": " << status.human << '\n';
 * ```
 *
 * Build with `-DFASTFHIR_BUILD_CONFORMANCE=ON` and link `fastfhir_conformance`;
 * this header has no implementation in the core library.
 *
 * WHAT THIS IS NOT. Conformance and structure are separate questions, and only
 * one of them is answered here (CLAUDE.md invariant 5a). `Parser::validate_FFHR_stream()`
 * asks whether the BYTES are sound — every offset in bounds, every block
 * vouching for itself — and a failure there means an unreadable file. This asks
 * whether the DOCUMENT is sound — required elements, cardinality, references
 * that resolve — and a failure here means a readable file a FHIR server
 * rejects. On a stream you did not produce, run the structural check first: it
 * is what establishes that the offsets this walk follows are real.
 */
#pragma once

#include "FF_Conformance.hpp"
#include "FF_Export.h"
#include "FF_Memory.hpp"

namespace FastFHIR
{
namespace Conformance
{

/**
 * @brief Conformance-check a finalized stream with the layer this build ships.
 *
 * Reads the stream's `FF_HEADER` for the revision and the root, then runs every
 * whole-stream check the generated layer offers. The arena is read and never
 * written, so this is safe to run on a read-only mount and leaves the bytes
 * exactly as it found them.
 *
 * @param stream A finalized stream. `Memory::createFromFile` or the view a
 *               `Builder::finalize` returned both work.
 * @return `Check::OK`, or the FIRST hard failure, carrying the FHIR path, the
 *         rule key and the specification's own wording. Test it with
 *         `operator bool`.
 *
 * @throws std::runtime_error if @p stream holds no readable FastFHIR header.
 *         That is a structural fault rather than a conformance one, and the two
 *         are never spelled the same way (CLAUDE.md invariant 5a).
 *
 * ONE failure comes back, because `Status` names one. A caller that wants every
 * finding uses the overload below with `LayerPolicy::Report` and a sink, which
 * is how the diagnostics reach a log instead of a return value.
 */
[[nodiscard]] FF_EXPORT Status validate_stream(const Memory& stream);

/**
 * @brief The same check, through a layer the caller configured.
 *
 * Use this to set `LayerPolicy::Report` (so a failure counts and logs rather
 * than stopping at the first), to attach a `ConcurrentLogger`, or to chain a
 * Block J terminology layer behind the generated one via `ValidationHooks::next`.
 *
 * @param stream A finalized stream.
 * @param layer  Borrowed for the duration of the call. Copy
 *               `conformance_layer()`'s struct before setting
 *               `policy`/`next`/`diagnostic`/`failures` on it — the one it
 *               returns is shared and immutable.
 * @return The first hard failure, or `Check::OK`. Under `Report` the verdict is
 *         still returned; what changes is that the Builder-side policy would not
 *         throw on it, and every finding reaches `layer.diagnostic`.
 *
 * @throws std::runtime_error if @p stream holds no readable FastFHIR header.
 * @throws std::bad_alloc if the resolution index does not fit. A stream check
 *         may allocate by design — whether one resource names another cannot be
 *         asked until every resource is written.
 */
[[nodiscard]] FF_EXPORT Status validate_stream(const Memory& stream, const ValidationHooks& layer);

} // namespace Conformance
} // namespace FastFHIR
