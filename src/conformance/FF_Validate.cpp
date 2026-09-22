/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * FF_Validate.cpp — the consumer half of the conformance layer.
 *
 * The whole content of this file is the translation between two shapes of the
 * same question. `StreamCheckInfo` is what a hook is CALLED with: five raw
 * values the Builder happens to be holding at finalize. A consumer holds a
 * finished stream instead, so the five values are read back out of the header
 * that stored them, and the walk is the same walk.
 */
#include "FF_Validate.hpp"

#include "FF_Conformance_Layer.hpp"
#include "FF_Conformance_internal.hpp"
#include "FF_Parser.hpp"
#include "FF_Primitives.hpp"

namespace FastFHIR
{
namespace Conformance
{

Status validate_stream(const Memory& stream)
{
    return validate_stream(stream, conformance_layer());
}

Status validate_stream(const Memory& stream, const ValidationHooks& layer)
{
    // Parser's constructor is the header gate: it refuses a null arena, a
    // buffer too small to hold a header, a bad magic, and a root offset outside
    // the stream -- throwing, because those are structural faults and a
    // structural fault is never spelled as a conformance one (invariant 5a).
    // Taking it here means the arithmetic below runs on a header that has
    // already been checked, rather than on whatever bytes were handed in.
    const Parser parser(stream);

    // ff_mapped_extent, by way of Parser::size(). The arena's CAPACITY is a
    // reservation (Memory::create takes 4 GiB by default) and would let every
    // bounds check in the walk pass on addresses the stream does not own; the
    // Builder passes capacity because it is writing INTO that reservation, and
    // a reader must not.
    const BYTE* const base = static_cast<const BYTE*>(stream->base());
    const Size        size = parser.size();
    const FF_HEADER   header(size);

    StreamCheckInfo info;
    info.arena         = base;
    info.arena_size    = static_cast<uint64_t>(size);
    info.fhir_version  = header.get_fhir_rev(base);
    info.root_offset   = static_cast<uint64_t>(header.get_root(base));
    info.root_recovery = static_cast<uint64_t>(header.get_root_type(base));
    // info.self is stamped per layer inside dispatch_stream().
    return dispatch_stream(&layer, info);
}

} // namespace Conformance
} // namespace FastFHIR
