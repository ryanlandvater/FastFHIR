/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file main.cpp
 * @brief The installed package, used the way a consumer uses it.
 *
 * Both halves of the direct API, compiled against the install prefix only:
 * build an Observation from the generated POCO types, seal it, then read it
 * back through the zero-copy lens. Each check names what it expected, so a
 * failure in CI says which half of the package is broken.
 */

#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>
#include <FF_Observation.hpp>

#include <cstdio>
#include <string_view>

namespace
{

int g_failures = 0;

void check(bool ok, const char* what)
{
    if (ok) return;
    std::fprintf(stderr, "install consumer: FAILED: %s\n", what);
    ++g_failures;
}

} // namespace

int main()
{
    using namespace FastFHIR;

    FF_Builder builder;
    check(FF_CreateBuilder(FF_BuilderCreateInfo{.capacity = 1 << 20}, builder).succeeded(),
          "FF_CreateBuilder on an anonymous arena");
    if (!builder) return 1;

    QuantityData quantity;
    quantity.value = 94.0;
    quantity.unit  = "mg/dL";

    ObservationData observation;
    observation.id                 = "install-smoke";
    observation.status             = FF_ObservationStatus::Final;
    observation.value.tag          = RECOVER_FF_QUANTITY;
    observation.value.block        = FF_MakeChoiceBlock(RECOVER_FF_QUANTITY);
    observation.value.block->value = std::move(quantity);

    const Reflective::ObjectHandle root = builder->append_obj(observation);
    check(static_cast<bool>(root), "append_obj(ObservationData)");
    check(FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root}).succeeded(),
          "FF_BuilderSetRoot");

    Memory::View sealed;
    check(FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, sealed).succeeded(),
          "FF_BuilderFinalize");

    bool parser_ok = true;
    Parser parser;
    try { parser = Parser(sealed.data(), sealed.size()); }
    catch (const std::exception &) { parser_ok = false; }
    check(parser_ok, "Parser over the sealed view");

    const Reflective::Node observation_node = parser.root();
    check(observation_node.is<RESOURCETYPE::OBSERVATION>(), "root is an Observation");
    check(observation_node[Fields::OBSERVATION::ID] == std::string_view("install-smoke"),
          "Observation.id reads back");
    check(observation_node[Fields::OBSERVATION::STATUS] == std::string_view("final"),
          "Observation.status reads back");

    const Reflective::Node value = observation_node[Fields::OBSERVATION::VALUE].as_node();
    check(value[Fields::QUANTITY::VALUE].as<double>() == 94.0, "Observation.valueQuantity.value");
    check(value[Fields::QUANTITY::UNIT] == std::string_view("mg/dL"), "Observation.valueQuantity.unit");

    if (g_failures == 0) std::puts("install consumer: OK");
    return g_failures == 0 ? 0 : 1;
}
