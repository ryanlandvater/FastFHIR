#pragma once

#include "FF_Parser.hpp"
#include "FF_Memory.hpp"

namespace FastFHIR {

class Compactor {
public:
    // Post-finalize archival transform.
    // Current implementation compacts the root object into dense field form and
    // copies the remaining stream payload unchanged.
    static Memory::View archive(const Parser& source, const Memory& destination);
};

} // namespace FastFHIR
