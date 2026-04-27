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
    static Memory::View archive(const Parser& source, const Memory& destination,
                                FF_Checksum_Algorithm algo = FF_CHECKSUM_NONE,
                                const HashCallback& hasher = nullptr);
};

} // namespace FastFHIR
