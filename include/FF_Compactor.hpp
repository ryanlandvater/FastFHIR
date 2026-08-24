/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#pragma once

#include "FF_Parser.hpp"
#include "FF_Memory.hpp"
#include <functional>

namespace FastFHIR {

class Compactor {
public:
    using HashCallback = std::function<std::vector<BYTE>(const unsigned char* byte_start, Size bytes_to_hash)>;

    // Post-finalize archival transform.
    // Current implementation compacts the root object into dense field form and
    // copies the remaining stream payload unchanged.
    // Optionally seals the compacted stream with a checksum via the same callback
    // contract as Builder::finalize().
    //
    // The stored-graph traversal is depth-bounded and cycle-checked (TASKS.md
    // XP-1.1): ArchiveContext tracks ancestry (`path`) and archived-once (`done`)
    // sets with MAX_NODE_DEPTH, and every recursive entry into the graph funnels
    // through the guarded archive_node. Sealing (header + checksum + hash) is
    // shared with Builder::finalize via seal_stream() in FF_Memory.hpp.
    //
    // Every deferred slot is pre-filled with a reserved in-flight sentinel --
    // FF_PENDING_OFFSET for 8-byte pointer slots, FF_PENDING_CODE for 4-byte
    // code slots (both in FF_Primitives.hpp). Neither is confusable with the
    // reader's "absent" values (FF_NULL_OFFSET / FF_CODE_NULL), so an
    // unresolved slot cannot masquerade as a cleanly dropped field. Before the
    // header is stamped, a pending-balance counter and a residual scan of every
    // tracked slot must both pass, so a dropped deferred write fails loudly
    // instead of sealing a stream that silently lost fields.
    static Memory::View archive(const Parser& source, const Memory& destination,
                                FF_Checksum_Algorithm algo = FF_CHECKSUM_NONE,
                                const HashCallback& hasher = nullptr);
};

} // namespace FastFHIR
