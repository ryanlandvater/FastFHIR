/**
 * @file conformance_layer.cpp
 * @brief The FHIR conformance layer, detached and attached — a worked example.
 *
 * Validation in FastFHIR is split in two, and the split is the thing to
 * understand before writing an encoder:
 *
 *   1. STRUCTURAL validation is inline and mandatory. A block stores its own
 *      offset, carries its recovery tag, and fits inside the arena. Every write
 *      checks that, always, whether or not a layer is attached. Wrong here means
 *      bytes that cannot be read.
 *   2. CONFORMANCE validation is what this example is about: optional, attached
 *      at runtime, generated from the HL7 StructureDefinitions. Observation.status
 *      is required; Bundle.entry.request.method is required. These say nothing
 *      about whether the bytes parse. Wrong here means a perfectly readable file
 *      that a FHIR server rejects.
 *
 * That is the Vulkan validation-layer arrangement, and it is deliberate:
 * conformance policy is a development tool, not a production dependency. A
 * shipped product links `fastfhir_conformance` only when it wants it, and a
 * detached append costs one null check.
 *
 * Attaching is three lines: copy conformance_layer(), point it at your sink,
 * hand it to the Builder.
 *
 * Build: -DFASTFHIR_BUILD_CONFORMANCE=ON, then link fastfhir_conformance.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0 — see LICENSE or http://mozilla.org/MPL/2.0/.
 */
#include <FastFHIR.hpp>

#include "FF_AllTypes.hpp"
#include "FF_Conformance_Layer.hpp"
#include "FF_Logger.hpp"

#include <atomic>
#include <cstdio>
#include <exception>
#include <source_location>
#include <stdexcept>
#include <string>

using namespace FastFHIR;
using namespace FastFHIR::Conformance;

namespace
{

/// Every expectation fails loudly. A demonstration that silently accepts the
/// wrong behaviour teaches the wrong lesson.
void expect(bool ok, const char* what,
            const std::source_location loc = std::source_location::current())
{
    if (!ok)
        throw std::runtime_error(std::string(what) + " (" + loc.file_name() + ":" +
                                 std::to_string(loc.line()) + ")");
}

/// Structurally perfect, and a spec violation: Observation.status and
/// Observation.code are both min = 1.
ObservationData incomplete_observation()
{
    ObservationData observation;
    observation.id = "example-1";
    return observation;
}

FF_Builder new_builder()
{
    FF_BuilderCreateInfo info;
    FF_Builder           builder;
    expect(static_cast<bool>(FF_CreateBuilder(info, builder)), "stream created");
    return builder;
}

} // namespace

int main()
{
    try
    {
        // ── 1. Detached: conformance is not the library's business ─────────
        {
            FF_Builder builder = new_builder();
            builder->append_obj(incomplete_observation());
            std::printf("1. detached append of an Observation with no status: "
                        "accepted (one null check)\n");
        }

        // ── 2. Attached, Throw: the same input is rejected, with a citation ─
        {
            FF_Builder      builder = new_builder();
            ValidationHooks hooks   = conformance_layer();  // copy, then customise
            builder->attach_layer(&hooks);

            std::string why;
            try
            {
                builder->append_obj(incomplete_observation());
            }
            catch (const std::runtime_error& e)
            {
                why = e.what();
            }
            expect(!why.empty(), "an attached append rejects spec-violating input");
            expect(why.find("Observation.status") != std::string::npos,
                   "the diagnostic names the element");
            std::printf("2. attached append, Throw policy:\n      %s\n", why.c_str());
        }

        // ── 3. Attached, Report: store it anyway, but never silently ────────
        // This is the policy a terminology layer uses (TASKS.md J5): clinical
        // data is never dropped for failing a check, but the failure is counted
        // and logged where a caller cannot miss it.
        {
            FF_Builder            builder = new_builder();
            ConcurrentLogger      logger;
            std::atomic<uint64_t> failures{0};

            ValidationHooks hooks = conformance_layer();
            hooks.policy     = LayerPolicy::Report;
            hooks.diagnostic = &logger;
            hooks.failures   = &failures;
            builder->attach_layer(&hooks);

            builder->append_obj(incomplete_observation());
            expect(failures.load() == 1, "the failure was counted");
            std::printf("3. attached append, Report policy: stored, %llu failure(s) counted\n"
                        "      %s",
                        static_cast<unsigned long long>(failures.load()),
                        logger.to_string().c_str());
        }

        // ── 4. Descent: a rule three levels down still fires ────────────────
        // Nothing about the Bundle itself is wrong. The layer walks into the
        // entry, then into its request, and finds the missing method there.
        {
            FF_Builder      builder = new_builder();
            ValidationHooks hooks   = conformance_layer();
            builder->attach_layer(&hooks);

            BundleData bundle;
            bundle.type = FF_BundleType::Collection;
            BundleentryData entry;
            entry.request      = std::make_unique<BundleentryrequestData>();
            entry.request->url = "Patient/1";
            bundle.entry.push_back(std::move(entry));

            std::string why;
            try
            {
                builder->append_obj(bundle);
            }
            catch (const std::runtime_error& e)
            {
                why = e.what();
            }
            expect(why.find("Bundle.entry.request.method") != std::string::npos,
                   "the layer descended into the nested backbone");
            std::printf("4. attached append of a Bundle:\n      %s\n", why.c_str());
        }

        // ── 5. What was NOT checked is as visible as what was ───────────────
        // A report that cannot say what it skipped is worth much less than one
        // that can. Every FHIRPath invariant and every required ValueSet binding
        // is listed, with the reason it is not evaluated.
        {
            std::size_t enforced = 0;
            std::size_t recorded = 0;
            for (const Rule& rule : conformance_rules(TypeTraits<PatientData>::recovery))
            {
                if (rule.kind == RuleKind::UNIMPLEMENTED)
                    ++recorded;
                else
                    ++enforced;
            }
            expect(recorded > 0, "Patient has recorded-but-unchecked rules");
            std::printf("5. Patient: %zu rule(s) enforced, %zu recorded as unchecked\n",
                        enforced, recorded);
        }

        // ── 6. The layer never changes the bytes ────────────────────────────
        // The check runs before any arena space is claimed, so this holds even
        // when the layer fires.
        {
            ObservationData good;
            good.id     = "example-2";
            good.status = FF_ObservationStatus::Final;
            good.code   = std::make_unique<CodeableConceptData>();

            auto seal = [&](bool attach) {
                FF_Builder      builder = new_builder();
                ValidationHooks hooks   = conformance_layer();
                if (attach)
                    builder->attach_layer(&hooks);
                auto root = builder->append_obj(good);
                expect(static_cast<bool>(
                           FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root})),
                       "root set");
                Memory::View view;
                expect(static_cast<bool>(
                           FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, view)),
                       "stream sealed");
                return std::string(reinterpret_cast<const char*>(view.data()), view.size());
            };
            expect(seal(false) == seal(true), "attached and detached bytes are identical");
            std::printf("6. byte identity with and without the layer: confirmed\n");
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "conformance_layer: FAILED - %s\n", e.what());
        return 1;
    }

    std::printf("conformance_layer: all checks passed\n");
    return 0;
}
