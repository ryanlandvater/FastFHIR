/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The attachable conformance layer (TASKS.md Block K).
//
// The property the whole design rests on, and the one that is easiest to break
// without noticing: THE LAYER OBSERVES, IT DOES NOT ENCODE. A stream written
// with the layer attached must be byte-for-byte the stream written without it.
// Case 4 is that test, and it is the reason the check runs before claim_space()
// rather than after -- a rejected write must leave the arena as it found it,
// with no claimed-but-unwritten hole.
//
// Fixture note: PatientData is the WRONG fixture for a required-element test
// and it was the one the plan originally named. Patient declares no min >= 1 at
// its top level -- every required element it has lives in a backbone. Measured
// before writing this file; Observation.status and Observation.code are the
// real ones.

#include <FastFHIR.hpp>
#include <FF_Ingestor.hpp>
#include "FF_AllTypes.hpp"
#include "FF_Bundle.hpp"
#include "FF_Conformance_Layer.hpp"
#include "FF_ConformanceEngine.hpp"
#include "FF_Logger.hpp"
#include "FF_Validate.hpp"

#include <filesystem>

#include <atomic>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "FFHR_tests.hpp"
#include "FFHR_test_corpus.hpp"

using namespace FastFHIR;
using namespace FastFHIR::Conformance;

namespace
{

/// A stream carrying one Observation, optionally under a layer.
/// Returns the sealed bytes so a caller can compare them.
std::vector<BYTE> write_observation(const ObservationData& observation,
                                    const ValidationHooks* hooks, bool& threw,
                                    std::string& what)
{
    threw = false;
    what.clear();
    FF_BuilderCreateInfo builder_info;
    FF_Builder           builder;
    if (!FF_CreateBuilder(builder_info, builder))
        return {};
    if (hooks != nullptr)
        builder->attach_layer(hooks);

    // append_obj, not append: it is the public entry point, and it reaches the
    // layer through the same append<T_Data> the ingest workers use.
    try
    {
        Reflective::ObjectHandle root = builder->append_obj(observation);
        if (!FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root}))
            return {};
    }
    catch (const std::runtime_error& e)
    {
        threw = true;
        what  = e.what();
        return {};
    }

    Memory::View view;
    if (!FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, view))
        return {};
    return std::vector<BYTE>(view.data(), view.data() + view.size());
}

/// Structurally perfect, spec-violating: status and code are both min = 1.
ObservationData violating()
{
    ObservationData observation;
    observation.id = "obs-violating";
    return observation;
}

/// The same resource with both required elements supplied.
ObservationData conforming()
{
    ObservationData observation;
    observation.id     = "obs-conforming";
    observation.status = FF_ObservationStatus::Final;
    observation.code   = std::make_unique<CodeableConceptData>();
    return observation;
}

// ── 1. Detached: conformance is not the library's business ────────────────
void detached_accepts_a_spec_violation()
{
    TEST_GROUP("detached");
    bool        threw = false;
    std::string what;
    const std::vector<BYTE> bytes = write_observation(violating(), nullptr, threw, what);
    CHECK(!threw, "a detached append does not throw on a spec violation: " << what);
    CHECK(!bytes.empty(), "a detached stream still seals");
}

// ── 2. Attached: the same input is a conformance failure ──────────────────
void attached_rejects_and_cites_the_specification()
{
    TEST_GROUP("attached");
    ValidationHooks hooks = conformance_layer();
    bool            threw = false;
    std::string     what;
    const std::vector<BYTE> bytes = write_observation(violating(), &hooks, threw, what);

    CHECK(threw, "an attached append rejects spec-violating input");
    CHECK(bytes.empty(), "nothing is sealed when the write is rejected");
    CHECK(what.rfind("FastFHIR: conformance: ", 0) == 0,
          "the message carries the write-path prefix: " << what);
    CHECK(what.find("Observation.status") != std::string::npos,
          "the message names the failing element: " << what);
    CHECK(what.find("minimum cardinality 1") != std::string::npos,
          "the message states the rule that produced it: " << what);
    CHECK(what.find("http://hl7.org/fhir/StructureDefinition/Observation") != std::string::npos,
          "the message cites the canonical StructureDefinition: " << what);
}

// ── 3. Report policy: fill the sink, count it, and continue ───────────────
void report_policy_counts_instead_of_throwing()
{
    TEST_GROUP("report");
    ConcurrentLogger      logger;
    std::atomic<uint64_t> failures{0};
    ValidationHooks       hooks = conformance_layer();
    hooks.policy     = LayerPolicy::Report;
    hooks.diagnostic = &logger;
    hooks.failures   = &failures;

    bool        threw = false;
    std::string what;
    const std::vector<BYTE> bytes = write_observation(violating(), &hooks, threw, what);

    CHECK(!threw, "Report policy does not throw: " << what);
    CHECK(!bytes.empty(), "Report policy still writes the resource");
    CHECK_EQ(failures.load(), 1u, "one failure counted");
    CHECK(logger.to_string().find("Observation.status") != std::string::npos,
          "the diagnostic reached the sink");
}

// ── 4. A conforming resource passes with the layer attached ───────────────
void conforming_input_passes_attached()
{
    TEST_GROUP("conforming");
    ConcurrentLogger      logger;
    std::atomic<uint64_t> failures{0};
    ValidationHooks       hooks = conformance_layer();
    hooks.policy     = LayerPolicy::Report;
    hooks.diagnostic = &logger;
    hooks.failures   = &failures;

    bool        threw = false;
    std::string what;
    const std::vector<BYTE> bytes = write_observation(conforming(), &hooks, threw, what);

    CHECK(!threw, "conforming input is not rejected: " << what);
    CHECK(!bytes.empty(), "conforming input seals");
    CHECK_EQ(failures.load(), 0u, "nothing counted for conforming input");
    CHECK(logger.to_string().empty(), "nothing reported for conforming input");
}

// ── 5. BYTE IDENTITY — the property that makes the layer safe to ship ─────
void the_layer_never_changes_the_bytes()
{
    TEST_GROUP("byte identity");
    ConcurrentLogger      logger;
    std::atomic<uint64_t> failures{0};
    ValidationHooks       hooks = conformance_layer();
    hooks.policy     = LayerPolicy::Report;
    hooks.diagnostic = &logger;
    hooks.failures   = &failures;

    bool        threw = false;
    std::string what;
    const std::vector<BYTE> detached = write_observation(conforming(), nullptr, threw, what);
    const std::vector<BYTE> attached = write_observation(conforming(), &hooks, threw, what);

    REQUIRE(!detached.empty(), "detached stream sealed");
    CHECK_EQ(attached.size(), detached.size(), "attached and detached streams are the same size");
    CHECK(attached == detached, "attached and detached streams are byte-identical");

    // The same must hold when the layer FIRES. A Report-policy failure that
    // still writes must write the same bytes a detached build would.
    const std::vector<BYTE> bad_detached = write_observation(violating(), nullptr, threw, what);
    const std::vector<BYTE> bad_attached = write_observation(violating(), &hooks, threw, what);
    REQUIRE(!bad_detached.empty(), "detached stream sealed for violating input");
    CHECK(bad_attached == bad_detached,
          "a REPORTED failure still writes byte-identical output");
    CHECK(failures.load() > 0, "the layer actually fired -- a zero here would make "
                               "the identity above vacuous (P0-2)");
}

// ── 6. Descent: a rule on a nested backbone fires from the parent append ──
void descent_reaches_a_nested_backbone()
{
    TEST_GROUP("descent");
    // Bundle.entry.request.method is min = 1, three levels below BundleData.
    // Nothing about BundleData itself is wrong -- if descent is broken, this
    // passes and the layer is worth much less than it looks.
    BundleData bundle;
    bundle.type = FF_BundleType::Collection;
    BundleentryData entry;
    entry.request = std::make_unique<BundleentryrequestData>();
    entry.request->url = "Patient/1";
    bundle.entry.push_back(std::move(entry));

    FF_BuilderCreateInfo builder_info;
    FF_Builder           builder;
    REQUIRE(FF_CreateBuilder(builder_info, builder), "create stream");
    ValidationHooks hooks = conformance_layer();
    builder->attach_layer(&hooks);

    std::string what;
    try
    {
        builder->append_obj(bundle);
    }
    catch (const std::runtime_error& e)
    {
        what = e.what();
    }
    CHECK(what.find("Bundle.entry.request.method") != std::string::npos,
          "descent reached Bundle.entry.request.method: " << what);
}

// ── 7. Chaining: `next` is reached only when the first layer passes ───────
std::atomic<int> g_trace_calls{0};

Status tracing_check(const void*, uint32_t, const ValidationHooks*) noexcept
{
    g_trace_calls.fetch_add(1, std::memory_order_relaxed);
    return {};
}

void layers_chain_in_both_orders()
{
    TEST_GROUP("chaining");
    static const Entry TRACE_ENTRIES[] = {
        {TypeTraits<ObservationData>::recovery, &tracing_check},
    };
    ValidationHooks conformance = conformance_layer();
    conformance.policy = LayerPolicy::Report;

    ValidationHooks tracing{};
    tracing.entries = TRACE_ENTRIES;
    tracing.count   = 1;
    tracing.policy  = LayerPolicy::Report;

    bool        threw = false;
    std::string what;

    // Tracing first: it passes, so the conformance layer behind it still runs.
    g_trace_calls = 0;
    tracing.next  = &conformance;
    write_observation(violating(), &tracing, threw, what);
    CHECK_EQ(g_trace_calls.load(), 1, "the first layer ran");

    // Conformance first: it FAILS, so the chain stops and tracing is not reached.
    g_trace_calls     = 0;
    tracing.next      = nullptr;
    conformance.next  = &tracing;
    write_observation(violating(), &conformance, threw, what);
    CHECK_EQ(g_trace_calls.load(), 0, "a failing layer stops the chain");

    // ...and with conforming input the chain runs all the way through.
    g_trace_calls = 0;
    write_observation(conforming(), &conformance, threw, what);
    CHECK_EQ(g_trace_calls.load(), 1, "a passing layer hands on to the next");
}

// ── 8. UNIMPLEMENTED rows are queryable, and never fire ───────────────────
void what_was_not_checked_is_visible()
{
    TEST_GROUP("unimplemented");
    const std::span<const Rule> rules = conformance_rules(TypeTraits<PatientData>::recovery);
    CHECK(!rules.empty(), "Patient has recorded rules");

    bool found_dom2 = false;
    bool found_binding = false;
    for (const Rule& rule : rules)
    {
        if (std::string_view(rule.key) == "dom-2")
        {
            found_dom2 = true;
            CHECK(rule.kind == RuleKind::UNIMPLEMENTED, "dom-2 is recorded, not enforced");
            CHECK(std::string_view(rule.human).find("FHIRPath") != std::string_view::npos,
                  "dom-2 says WHY it is not evaluated");
        }
        if (rule.kind == RuleKind::UNIMPLEMENTED && std::string_view(rule.binding).size() > 0)
            found_binding = true;
    }
    CHECK(found_dom2, "the dom-2 invariant is listed as unimplemented");
    CHECK(found_binding, "a required ValueSet binding is listed with its ValueSet URL");

    // A resource with only UNIMPLEMENTED rows must still write cleanly: those
    // rows are records, not checks.
    ValidationHooks hooks = conformance_layer();
    FF_BuilderCreateInfo builder_info;
    FF_Builder           builder;
    REQUIRE(FF_CreateBuilder(builder_info, builder), "create stream");
    builder->attach_layer(&hooks);
    PatientData patient;
    patient.id = "p1";
    std::string what;
    try { builder->append_obj(patient); }
    catch (const std::runtime_error& e) { what = e.what(); }
    CHECK(what.empty(), "an UNIMPLEMENTED row never fires: " << what);
}

// ── 9. MAX_CARDINALITY: the engine branch the base spec never exercises ───
// The compiled StructureDefinitions spell `max` only as "1" or "*", so the
// generated layer emits ZERO max-cardinality rules. An engine branch no rule
// ever reaches is not evidence that it works, so this builds the rule by hand.
// If a profile ever introduces a numeric max, this is the code that will run.

/// The field's position in visit_fields() -- the same ordinal a Rule carries.
/// Derived here rather than hardcoded, so the test cannot drift from the layout
/// the generator emitted.
uint16_t ordinal_of(const ObservationData& observation, std::string_view name)
{
    uint16_t index = 0;
    uint16_t found = 0xFFFF;
    visit_fields(observation, [&](const char* field_name, const auto&) {
        if (std::string_view(field_name) == name)
            found = index;
        ++index;
    });
    return found;
}

void a_numeric_max_is_enforced()
{
    TEST_GROUP("max cardinality");
    const uint16_t identifier_ordinal = ordinal_of(conforming(), "identifier");
    CHECK_NE(identifier_ordinal, 0xFFFF, "Observation.identifier has a visit ordinal");

    static Rule max_rules[] = {
        {"Observation.identifier", "", "Observation.identifier admits at most 2 element(s).",
         "http://hl7.org/fhir/StructureDefinition/Observation", "", 0, 2, 0,
         RuleKind::MAX_CARDINALITY, CONF_VERSION_ALL},
    };
    max_rules[0].ordinal = identifier_ordinal;

    static const Entry MAX_ENTRIES[] = {
        {TypeTraits<ObservationData>::recovery,
         [](const void* data, uint32_t version, const ValidationHooks* self) noexcept -> Status {
             return run_rules(*static_cast<const ObservationData*>(data), max_rules, 1, version,
                              self);
         }},
    };
    ValidationHooks hooks{};
    hooks.entries = MAX_ENTRIES;
    hooks.count   = 1;
    hooks.policy  = LayerPolicy::Throw;

    bool        threw = false;
    std::string what;

    ObservationData within = conforming();
    within.identifier.resize(2);
    write_observation(within, &hooks, threw, what);
    CHECK(!threw, "two identifiers is within a max of 2: " << what);

    ObservationData over = conforming();
    over.identifier.resize(3);
    write_observation(over, &hooks, threw, what);
    CHECK(threw, "three identifiers exceeds a max of 2");
    CHECK(what.find("Observation.identifier") != std::string::npos,
          "the failure names the repeating element: " << what);
}

// ── 10. An ABI mismatch is refused at attach, loudly ──────────────────────
void an_abi_mismatch_is_refused()
{
    TEST_GROUP("abi");
    FF_BuilderCreateInfo builder_info;
    FF_Builder           builder;
    REQUIRE(FF_CreateBuilder(builder_info, builder), "create stream");

    ValidationHooks stale = conformance_layer();
    stale.abi_version = CONFORMANCE_ABI + 1;
    std::string what;
    try { builder->attach_layer(&stale); }
    catch (const std::runtime_error& e) { what = e.what(); }
    CHECK(what.find("ABI mismatch") != std::string::npos,
          "a layer from another release is refused at attach: " << what);
    CHECK(builder->layer() == nullptr, "the refused layer was not attached");

    // The FF_ boundary reports the SAME refusal as an FF_Result, and attaches a
    // matching layer without throwing. Both routes reach one engine call.
    FF_Builder      good;
    REQUIRE(FF_CreateBuilder(FF_BuilderCreateInfo{}, good), "create stream");
    ValidationHooks fresh = conformance_layer();
    CHECK(static_cast<bool>(FF_BuilderAttachLayer(
              FF_BuilderAttachLayerInfo{.builder = good, .hooks = &fresh})),
          "FF_BuilderAttachLayer attaches a matching layer");
    CHECK(good->layer() == &fresh, "and the builder holds it");

    FF_Builder refused;
    REQUIRE(FF_CreateBuilder(FF_BuilderCreateInfo{}, refused), "create stream");
    const FF_Result attach =
        FF_BuilderAttachLayer(FF_BuilderAttachLayerInfo{.builder = refused, .hooks = &stale});
    CHECK(!attach, "FF_BuilderAttachLayer returns a failure for an ABI mismatch");
    CHECK(attach.message.find("ABI mismatch") != std::string::npos,
          "and the message says so: " << attach.message);
    CHECK(refused->layer() == nullptr, "the refused layer was not attached");

    // A null builder is refused rather than dereferenced; a null layer detaches.
    CHECK(!FF_BuilderAttachLayer(FF_BuilderAttachLayerInfo{}),
          "a null builder handle is an FF_Result, not a crash");
}

// ── 11. Version masking: R4 and R5 disagree, and the layer knows ──────────
void a_rule_only_applies_to_the_revision_that_states_it()
{
    TEST_GROUP("versions");
    // Condition.clinicalStatus is min = 1 in R5 and min = 0 in R4 -- verified
    // against both StructureDefinitions. The same POCO must therefore be
    // rejected by an R5 stream and accepted by an R4 one.
    ConditionData condition;
    condition.id = "c1";

    ValidationHooks hooks = conformance_layer();
    for (const FHIR_VERSION version : {FHIR_VERSION_R5, FHIR_VERSION_R4})
    {
        FF_BuilderCreateInfo builder_info;
        builder_info.version = version;
        FF_Builder builder;
        REQUIRE(FF_CreateBuilder(builder_info, builder), "create stream");
        builder->attach_layer(&hooks);
        std::string what;
        try { builder->append_obj(condition); }
        catch (const std::runtime_error& e) { what = e.what(); }

        const bool named = what.find("Condition.clinicalStatus") != std::string::npos;
        if (version == FHIR_VERSION_R5)
            CHECK(named, "R5 requires Condition.clinicalStatus: " << what);
        else
            CHECK(!named, "R4 does not require Condition.clinicalStatus: " << what);
    }
}

// ── 12. The layer through the real pipeline (COV-1) ───────────────────────
// Case 5 proves identity for a hand-built POCO on the calling thread. The
// layer's real deployment is the ingest, where every nested and contained
// resource reaches it through a worker's append<T_Data>. So ingest real Synthea
// bundles with and without the layer, twice over:
//
//   ONE worker   -- the sealed arenas must be byte-identical. Only here is that
//                   a meaningful claim: with a pool, block placement follows
//                   worker scheduling, and two DETACHED ingests of one bundle
//                   already differ in ~70% of their bytes (measured
//                   2026-09-15). A detached-vs-detached control runs first, so
//                   if single-worker ingest ever stops being deterministic the
//                   failure names that instead of blaming the layer.
//   FULL pool    -- the layer runs concurrently on the workers, and the
//                   exported documents must be identical.
//
// A tracing layer sits in FRONT of the conformance layer (it always passes, so
// the chain always reaches conformance). Its call count and the failure count
// prove the layer actually ran -- identical output from a layer that never ran
// would be vacuous (P0-2).

/// Ingest one payload into a fresh stream, optionally under @p hooks, and
/// return the sealed bytes. Empty on any failure.
std::vector<BYTE> ingest_bundle(const std::string& json, const ValidationHooks* hooks,
                                uint32_t concurrency)
{
    FF_BuilderCreateInfo builder_info;
    builder_info.arena   = Memory::create(2ull * 1024 * 1024 * 1024);
    builder_info.version = FHIR_VERSION_R5;
    FF_Builder builder;
    if (!FF_CreateBuilder(builder_info, builder))
        return {};
    if (hooks != nullptr)
        builder->attach_layer(hooks);

    FF_IngestorCreateInfo ingestor_info;
    ingestor_info.concurrency = concurrency;
    FF_Ingestor ingestor;
    if (!FF_CreateIngestor(ingestor_info, ingestor))
        return {};

    Reflective::ObjectHandle root;
    Size                     resource_count = 0;
    const FF_Result ingest = FF_Ingest(FF_IngestInfo{
        .ingestor         = ingestor,
        .builder          = builder,
        .source_type      = FF_SOURCE_FHIR_JSON,
        .extension_filter = FF_ExtensionFilterMode::FILTER_NONE,
        .payload          = json,
    }, root, resource_count);
    if (ingest.failed())
    {
        printf("    ingest failed: %s\n", ingest.message.c_str());
        return {};
    }
    if (!FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root}))
        return {};

    Memory::View view;
    if (!FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, view))
        return {};
    return std::vector<BYTE>(view.data(), view.data() + view.size());
}

/// The document a sealed stream exports.
std::string exported_json(const std::vector<BYTE>& bytes)
{
    std::ostringstream out;
    Parser(bytes.data(), bytes.size()).print_json(out);
    return out.str();
}

void the_layer_never_changes_ingested_output()
{
    TEST_GROUP("ingest identity");
    const std::vector<ff_test::fs::path> bundles = ff_test::find_bundles(3);
    if (bundles.empty())
    {
        printf("  SKIP: Synthea corpus not configured (FASTFHIR_DOWNLOAD_SYNTHEA)\n");
        return;
    }

    static const Entry TRACE_ENTRIES[] = {
        {TypeTraits<ObservationData>::recovery, &tracing_check},
    };
    ConcurrentLogger      logger;
    std::atomic<uint64_t> failures{0};
    ValidationHooks       conformance = conformance_layer();
    conformance.policy     = LayerPolicy::Report;
    conformance.diagnostic = &logger;
    conformance.failures   = &failures;

    ValidationHooks tracing{};
    tracing.entries = TRACE_ENTRIES;
    tracing.count   = 1;
    tracing.policy  = LayerPolicy::Report;
    tracing.next    = &conformance;

    for (const ff_test::fs::path& path : bundles)
    {
        const std::string name = path.filename().string();
        const std::string json = ff_test::read_file(path);
        REQUIRE(!json.empty(), "read " << name);

        // One worker: exact bytes.
        const std::vector<BYTE> control  = ingest_bundle(json, nullptr, 1);
        const std::vector<BYTE> detached = ingest_bundle(json, nullptr, 1);
        REQUIRE(!detached.empty(), "single-worker detached ingest sealed: " << name);
        REQUIRE(control == detached,
                "single-worker ingest is deterministic (precondition for the byte "
                "comparison, not a layer defect): " << name);

        g_trace_calls = 0;
        failures      = 0;
        const std::vector<BYTE> attached = ingest_bundle(json, &tracing, 1);
        CHECK(attached == detached,
              "single-worker ingest is byte-identical with the layer attached: " << name);
        CHECK(g_trace_calls.load() > 0, "the layer chain was consulted: " << name);
        CHECK(failures.load() > 0, "the conformance layer fired on real data: " << name);

        // Full pool: the layer runs on the workers; the document must not change.
        g_trace_calls = 0;
        const std::vector<BYTE> pool_detached = ingest_bundle(json, nullptr, 0);
        const std::vector<BYTE> pool_attached = ingest_bundle(json, &tracing, 0);
        REQUIRE(!pool_detached.empty() && !pool_attached.empty(), "pooled ingest sealed: " << name);
        CHECK(g_trace_calls.load() > 0, "the pooled workers consulted the layer chain: " << name);
        CHECK(exported_json(pool_attached) == exported_json(pool_detached),
              "pooled ingest exports an identical document with the layer attached: " << name);
    }
}

// ══════════════════════════════════════════════════════════════════════════
// The STREAM-level check (T10). Every check above runs on ONE block as it is
// written; whether a reference names an entry is a question about the whole
// document, so it is asked once, at finalize.
// ══════════════════════════════════════════════════════════════════════════

/// A `collection` Bundle of Observations, one per (fullUrl, subject) pair. An
/// empty subject writes no `Reference` at all. Every element the PER-BLOCK layer
/// requires is supplied, so a failure in these cases is the stream check and
/// never a block check firing first.
std::vector<BYTE> write_bundle(const std::vector<std::pair<std::string, std::string>>& entries,
                               const ValidationHooks* hooks, bool& refused, std::string& what)
{
    refused = false;
    what.clear();
    FF_Builder builder;
    if (!FF_CreateBuilder(FF_BuilderCreateInfo{}, builder))
        return {};
    if (hooks != nullptr)
        builder->attach_layer(hooks);

    try
    {
        BundleData bundle;
        bundle.type = FF_BundleType::Collection;
        for (const auto& [url, subject] : entries)
        {
            ObservationData observation;
            observation.status = FF_ObservationStatus::Final;
            observation.code   = CodeableConceptData{};
            if (!subject.empty())
                observation.subject = ReferenceData{.reference = subject};

            BundleentryData entry;
            entry.fullurl  = url;
            entry.resource = static_cast<ResourceReference>(builder->append_obj(observation));
            bundle.entry.push_back(std::move(entry));
        }
        const Reflective::ObjectHandle root = builder->append_obj(bundle);
        if (!FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root}))
            return {};
    }
    catch (const std::runtime_error& e)
    {
        refused = true;
        what    = e.what();
        return {};
    }

    // Finalize is where the stream check runs; FF_BuilderFinalize converts the
    // throw into a Result, so this is where a refusal surfaces.
    Memory::View view;
    const FF_Result sealed = FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, view);
    if (!sealed)
    {
        refused = true;
        what    = sealed.message;
        return {};
    }
    return std::vector<BYTE>(view.data(), view.data() + view.size());
}

/// A reference to an entry that IS in the Bundle resolves, and silence is the
/// whole point: the check must not report healthy documents.
void a_resolvable_reference_is_silent()
{
    TEST_GROUP("stream resolves");
    ConcurrentLogger      logger;
    std::atomic<uint64_t> failures{0};
    ValidationHooks       hooks = conformance_layer();
    hooks.policy     = LayerPolicy::Report;
    hooks.diagnostic = &logger;
    hooks.failures   = &failures;

    bool        refused = false;
    std::string what;
    const std::vector<BYTE> bytes =
        write_bundle({{"urn:uuid:a", ""}, {"urn:uuid:b", "urn:uuid:a"}}, &hooks, refused, what);

    CHECK(!refused, "a reference that names an entry is not reported: " << what);
    CHECK(!bytes.empty(), "and the stream seals");
    CHECK_EQ(failures.load(), 0u, "nothing counted");
    CHECK(logger.to_string().empty(), "nothing logged: " << logger.to_string());
}

/// A `urn:` identifier has no meaning outside the Bundle that defines it, so an
/// unresolved one is an error, and under the default Throw policy it stops the
/// finalize.
void an_unresolved_urn_reference_is_refused()
{
    TEST_GROUP("stream urn error");
    ValidationHooks hooks = conformance_layer();  // Throw, as shipped

    bool        refused = false;
    std::string what;
    const std::vector<BYTE> bytes =
        write_bundle({{"urn:uuid:a", "urn:uuid:missing"}}, &hooks, refused, what);

    CHECK(refused, "an unresolved urn: reference stops the finalize under Throw");
    CHECK(bytes.empty(), "and nothing is sealed");
    CHECK(what.find("urn:uuid:missing") != std::string::npos,
          "the message names the reference: " << what);
    CHECK(what.find("Observation.subject") != std::string::npos,
          "and the path that carries it: " << what);
    CHECK(what.find("not an entry in this Bundle") != std::string::npos,
          "and says what is wrong: " << what);
}

/// The same document under Report: counted, logged, and written. The reference
/// is still a defect, but the policy says a caller wants the stream anyway.
void a_reported_reference_failure_still_seals()
{
    TEST_GROUP("stream urn report");
    ConcurrentLogger      logger;
    std::atomic<uint64_t> failures{0};
    ValidationHooks       hooks = conformance_layer();
    hooks.policy     = LayerPolicy::Report;
    hooks.diagnostic = &logger;
    hooks.failures   = &failures;

    bool        refused = false;
    std::string what;
    const std::vector<BYTE> bytes =
        write_bundle({{"urn:uuid:a", "urn:uuid:missing"}}, &hooks, refused, what);

    CHECK(!refused, "Report policy does not stop the finalize: " << what);
    CHECK(!bytes.empty(), "it writes the stream");
    CHECK_EQ(failures.load(), 1u, "one unresolved reference counted");
    CHECK(logger.to_string().find("urn:uuid:missing") != std::string::npos,
          "the diagnostic reached the sink: " << logger.to_string());

    // BYTE IDENTITY on the stream check's own path: a reported failure must
    // write exactly what a detached build writes.
    bool        detach_refused = false;
    std::string detach_what;
    const std::vector<BYTE> detached =
        write_bundle({{"urn:uuid:a", "urn:uuid:missing"}}, nullptr, detach_refused, detach_what);
    REQUIRE(!detached.empty(), "detached stream sealed");
    CHECK(bytes == detached, "a REPORTED reference failure writes byte-identical output");
}

/// A relative reference may resolve on the receiving server, so an unresolved
/// one is a warning: reported and counted, but it must not stop a finalize --
/// not even under Throw.
void a_relative_reference_only_warns()
{
    TEST_GROUP("stream relative");
    ConcurrentLogger      logger;
    std::atomic<uint64_t> failures{0};
    ValidationHooks       hooks = conformance_layer();  // Throw, as shipped
    hooks.diagnostic = &logger;
    hooks.failures   = &failures;

    bool        refused = false;
    std::string what;
    const std::vector<BYTE> bytes =
        write_bundle({{"urn:uuid:a", "Patient/999"}}, &hooks, refused, what);

    CHECK(!refused, "a relative reference does not stop the finalize: " << what);
    CHECK(!bytes.empty(), "the stream seals");
    CHECK_EQ(failures.load(), 1u, "the warning was counted");
    CHECK(logger.to_string().find("Patient/999") != std::string::npos,
          "and logged: " << logger.to_string());
}

/// An absolute URL names something outside this stream by construction, so it is
/// not this check's business and produces nothing.
void an_absolute_reference_is_not_checked()
{
    TEST_GROUP("stream absolute");
    ConcurrentLogger      logger;
    std::atomic<uint64_t> failures{0};
    ValidationHooks       hooks = conformance_layer();
    hooks.diagnostic = &logger;
    hooks.failures   = &failures;

    bool        refused = false;
    std::string what;
    const std::vector<BYTE> bytes =
        write_bundle({{"urn:uuid:a", "http://example.org/fhir/Patient/1"}}, &hooks, refused, what);

    CHECK(!refused, "an absolute reference is not reported: " << what);
    CHECK(!bytes.empty(), "the stream seals");
    CHECK_EQ(failures.load(), 0u, "nothing counted");
    CHECK(logger.to_string().empty(), "nothing logged: " << logger.to_string());
}

/// A refusal from the stream check must leave the Builder USABLE, so a caller
/// can correct the stream and finalize again rather than rebuild it. The check
/// writes nothing, so releasing the finalize latch is safe -- and this is the
/// property that would be lost if the latch stayed one-way.
void a_stream_refusal_leaves_the_builder_usable()
{
    TEST_GROUP("stream latch");
    FF_Builder builder;
    REQUIRE(FF_CreateBuilder(FF_BuilderCreateInfo{}, builder), "create builder");
    ValidationHooks hooks = conformance_layer();
    builder->attach_layer(&hooks);

    ObservationData observation;
    observation.status  = FF_ObservationStatus::Final;
    observation.code    = CodeableConceptData{};
    observation.subject = ReferenceData{.reference = "urn:uuid:missing"};

    BundleData bundle;
    bundle.type = FF_BundleType::Collection;
    BundleentryData entry;
    entry.fullurl  = "urn:uuid:a";
    entry.resource = static_cast<ResourceReference>(builder->append_obj(observation));
    bundle.entry.push_back(std::move(entry));
    const Reflective::ObjectHandle root = builder->append_obj(bundle);
    REQUIRE(FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root}), "set root");

    Memory::View view;
    const FF_Result first =
        FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, view);
    CHECK(!first, "the first finalize refuses the dangling reference");

    bool append_ok = true;
    try { builder->append_obj(observation); }
    catch (const std::runtime_error&) { append_ok = false; }
    CHECK(append_ok, "the Builder is still mutable after a stream-check refusal");
}

// ── 19. The CONSUMER entry point: validate_stream(const Memory&) ──────────
//
// Every case above drives the layer on the WRITE path, where the Builder holds
// the arena, the revision and the root and hands them to the hook. These drive
// the READ path, which is the other half of the same layer: a finished stream
// arrived from somewhere, and the five values the hook wants have to come back
// out of its header. That translation is the only thing FF_Validate.cpp does,
// so what these pin is that it agrees with the write path on the same document.

/// A stream on disk, written with NO layer attached -- the external tool's
/// premise. Returns the path.
std::string sealed_bundle_file(
    const char* name, const std::vector<std::pair<std::string, std::string>>& entries)
{
    namespace fs = std::filesystem;
    const fs::path path = fs::path(FF_TEST_ARTIFACT_DIR) / "conformance" / name;
    fs::create_directories(path.parent_path());
    std::error_code ignored;
    fs::remove(path, ignored);

    const std::string path_str = path.string();
    FF_BuilderCreateInfo create;
    create.filepath = path_str.c_str();
    FF_Builder builder;
    const bool created = FF_CreateBuilder(create, builder).succeeded();
    CHECK(created, "create a file-backed builder");
    if (!created) return {};

    BundleData bundle;
    bundle.type = FF_BundleType::Collection;
    for (const auto& [url, subject] : entries)
    {
        ObservationData observation;
        observation.status = FF_ObservationStatus::Final;
        observation.code   = CodeableConceptData{};
        if (!subject.empty())
            observation.subject = ReferenceData{.reference = subject};

        BundleentryData entry;
        entry.fullurl  = url;
        entry.resource = static_cast<ResourceReference>(builder->append_obj(observation));
        bundle.entry.push_back(std::move(entry));
    }
    const Reflective::ObjectHandle root = builder->append_obj(bundle);
    const bool rooted =
        FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root}).succeeded();
    CHECK(rooted, "set root");
    if (!rooted) return {};

    Memory::View view;
    // No layer attached, so nothing is checked here. That is the point: the
    // defect has to survive to disk for the reader to be the one that finds it.
    const bool sealed =
        FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, view).succeeded();
    CHECK(sealed, "seal");
    return sealed ? path_str : std::string{};
}

/// The headline case. A Bundle whose reference names no entry was written
/// WITHOUT the layer, so nothing objected at finalize; opening it read-only and
/// asking validate_stream() finds the same defect the write path finds, with the
/// same wording. One Memory in, one Status out -- no revision, no root offset.
void validate_stream_finds_what_the_write_path_would_have()
{
    TEST_GROUP("validate_stream");
    const std::string path =
        sealed_bundle_file("unresolved.ffhr", {{"urn:uuid:a", "urn:uuid:missing"}});

    const Memory stream = Memory::openReadOnly(path);
    REQUIRE(static_cast<bool>(stream), "the sealed stream opens read-only");

    const Status status = validate_stream(stream);
    CHECK(!status, "an unresolved urn: reference is reported on the read path");
    CHECK(status.code == Check::UNRESOLVED_REFERENCE, "and it is the reference check that said so");
    // `path` stays empty by design here and the FHIR path is carried inside
    // `human`, because the reference check's whole message IS the locator plus
    // the reference; see record() in FF_StreamCheck.cpp.
    const std::string reported = status.human;
    CHECK(reported.find("Observation.subject") != std::string::npos,
          "the FHIR path locates it: " << reported);
    CHECK(reported.find("urn:uuid:missing") != std::string::npos,
          "and names the reference: " << reported);
    CHECK(reported.find("not an entry in this Bundle") != std::string::npos,
          "with the same wording the write path uses: " << reported);
}

/// A document whose references all resolve returns OK. Silence on healthy input
/// is worth a case of its own: a checker that reports everything reports nothing.
void validate_stream_is_silent_on_a_sound_document()
{
    TEST_GROUP("validate_stream ok");
    const std::string path =
        sealed_bundle_file("resolved.ffhr", {{"urn:uuid:a", ""}, {"urn:uuid:b", "urn:uuid:a"}});

    const Memory stream = Memory::openReadOnly(path);
    REQUIRE(static_cast<bool>(stream), "open");
    CHECK(static_cast<bool>(validate_stream(stream)), "a resolvable Bundle validates clean");
}

/// The overload carries a caller's policy and sink, which is how every finding
/// is collected rather than only the first. Also the read-only proof: the arena
/// is mapped PROT_READ, so a check that wrote anything would fault here.
void validate_stream_reports_through_a_caller_supplied_layer()
{
    TEST_GROUP("validate_stream layer");
    const std::string path = sealed_bundle_file(
        "reported.ffhr", {{"urn:uuid:a", "urn:uuid:x"}, {"urn:uuid:b", "Patient/123"}});

    const Memory stream = Memory::openReadOnly(path);
    REQUIRE(static_cast<bool>(stream), "open");

    ConcurrentLogger      logger;
    std::atomic<uint64_t> failures{0};
    ValidationHooks       hooks = conformance_layer();
    hooks.policy     = LayerPolicy::Report;
    hooks.diagnostic = &logger;
    hooks.failures   = &failures;

    const Status status = validate_stream(stream, hooks);
    CHECK(!status, "the hard failure is still returned under Report");
    CHECK(failures.load() >= 2u,
          "both the unresolved urn: and the relative reference were counted: " << failures.load());
    const std::string logged = logger.to_string();
    CHECK(logged.find("urn:uuid:x") != std::string::npos, "the urn: reached the sink: " << logged);
    CHECK(logged.find("Patient/123") != std::string::npos,
          "and so did the relative reference the return value cannot carry: " << logged);
}

/// A stream rooted at something other than a Bundle has no entry set, so there
/// is nothing to resolve against and OK is the honest answer. This is the case
/// that would otherwise report every reference in the document as broken.
void validate_stream_passes_a_non_bundle_root()
{
    TEST_GROUP("validate_stream non-bundle");
    namespace fs = std::filesystem;
    const fs::path path = fs::path(FF_TEST_ARTIFACT_DIR) / "conformance" / "single.ffhr";
    fs::create_directories(path.parent_path());
    std::error_code ignored;
    fs::remove(path, ignored);
    const std::string path_str = path.string();

    FF_BuilderCreateInfo create;
    create.filepath = path_str.c_str();
    FF_Builder builder;
    REQUIRE(FF_CreateBuilder(create, builder), "create");
    ObservationData observation;
    observation.status = FF_ObservationStatus::Final;
    observation.code   = CodeableConceptData{};
    observation.subject = ReferenceData{.reference = "urn:uuid:nowhere"};
    const Reflective::ObjectHandle root = builder->append_obj(observation);
    REQUIRE(FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root}), "set root");
    Memory::View view;
    REQUIRE(FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, view), "seal");

    const Memory stream = Memory::openReadOnly(path_str);
    REQUIRE(static_cast<bool>(stream), "open");
    CHECK(static_cast<bool>(validate_stream(stream)),
          "a non-Bundle root has no entry set, so its references are not this check's subject");
}

/// Garbage in is a STRUCTURAL fault and is spelled as one -- a throw, never a
/// conformance Status. Invariant 5a: the two verdicts mean different things, and
/// a caller that cannot tell them apart cannot act on either.
void validate_stream_refuses_bytes_that_are_not_a_stream()
{
    TEST_GROUP("validate_stream garbage");
    Memory arena = Memory::create(1 << 16);
    REQUIRE(static_cast<bool>(arena), "an arena with no header written");

    bool        threw = false;
    std::string what;
    try { (void)validate_stream(arena); }
    catch (const std::runtime_error& e) { threw = true; what = e.what(); }
    CHECK(threw, "an unwritten arena is refused rather than reported as non-conformant");
    CHECK(what.find("FastFHIR") != std::string::npos, "and the message says who refused: " << what);
}

} // namespace

int main(int argc, char** argv)
{
    ff_test::set_filter(argc, argv);
    ff_test::run("detached", detached_accepts_a_spec_violation);
    ff_test::run("attached", attached_rejects_and_cites_the_specification);
    ff_test::run("report", report_policy_counts_instead_of_throwing);
    ff_test::run("conforming", conforming_input_passes_attached);
    ff_test::run("byte_identity", the_layer_never_changes_the_bytes);
    ff_test::run("descent", descent_reaches_a_nested_backbone);
    ff_test::run("chaining", layers_chain_in_both_orders);
    ff_test::run("unimplemented", what_was_not_checked_is_visible);
    ff_test::run("max_cardinality", a_numeric_max_is_enforced);
    ff_test::run("abi", an_abi_mismatch_is_refused);
    ff_test::run("versions", a_rule_only_applies_to_the_revision_that_states_it);
    ff_test::run("ingest_identity", the_layer_never_changes_ingested_output);
    ff_test::run("stream_resolves", a_resolvable_reference_is_silent);
    ff_test::run("stream_urn_error", an_unresolved_urn_reference_is_refused);
    ff_test::run("stream_urn_report", a_reported_reference_failure_still_seals);
    ff_test::run("stream_relative", a_relative_reference_only_warns);
    ff_test::run("stream_absolute", an_absolute_reference_is_not_checked);
    ff_test::run("stream_latch", a_stream_refusal_leaves_the_builder_usable);
    ff_test::run("validate_stream", validate_stream_finds_what_the_write_path_would_have);
    ff_test::run("validate_stream_ok", validate_stream_is_silent_on_a_sound_document);
    ff_test::run("validate_stream_layer", validate_stream_reports_through_a_caller_supplied_layer);
    ff_test::run("validate_stream_non_bundle", validate_stream_passes_a_non_bundle_root);
    ff_test::run("validate_stream_garbage", validate_stream_refuses_bytes_that_are_not_a_stream);
    return ff_test::report("conformance layer");
}
