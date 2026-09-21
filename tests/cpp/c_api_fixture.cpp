/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file c_api_fixture.cpp
 * @brief Seals one small, typed stream to a file for the C ABI test to read.
 *
 * The C surface cannot append a TYPED resource — the typed append is a C++
 * template — so a stream whose root is a real resource has to be produced from
 * C++. That is exactly the boundary this fixture marks: the C++ side writes the
 * .ffhr, the C side (test_c_api.c) opens, parses, validates, exports and
 * compacts it. Run as: c_api_fixture <output.ffhr>
 */

#include <FastFHIR.hpp>
#include <FF_Observation.hpp>

#include <cstdio>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: c_api_fixture <output.ffhr>\n");
        return 2;
    }

    using namespace FastFHIR;

    FF_Builder builder;
    // Designated initializers in declaration order: C++20 requires it, and GCC
    // and MSVC reject any other order outright (Clang only warns).
    if (!FF_CreateBuilder(FF_BuilderCreateInfo{.capacity = 1u << 20, .filepath = argv[1]}, builder))
    {
        std::fprintf(stderr, "c_api_fixture: FF_CreateBuilder failed\n");
        return 3;
    }

    QuantityData quantity;
    quantity.value = 94.0;
    quantity.unit  = "mg/dL";

    ObservationData observation;
    observation.id                 = "c_api-1";
    observation.status             = FF_ObservationStatus::Final;
    observation.value.tag          = RECOVER_FF_QUANTITY;
    observation.value.block        = FF_MakeChoiceBlock(RECOVER_FF_QUANTITY);
    observation.value.block->value = std::move(quantity);

    const Reflective::ObjectHandle root = builder->append_obj(observation);
    if (!FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root}))
    {
        std::fprintf(stderr, "c_api_fixture: FF_BuilderSetRoot failed\n");
        return 4;
    }

    Memory::View sealed;
    if (!FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, sealed))
    {
        std::fprintf(stderr, "c_api_fixture: FF_BuilderFinalize failed\n");
        return 5;
    }

    return 0;
}
