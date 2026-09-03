/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// P0-1 / CAPI-13: the POCO must agree with the reflective lens.
//
// Why this file exists (2026-08-26). `generate_eager_deserializer()` had no
// FF_FIELD_BLOCK branch, so every SINGULAR block-typed field -- CodeableConcept,
// Reference, a nested BackboneElement -- matched no arm of its if/elif chain and
// emitted NO CODE AT ALL. 736 fields across all 37 generated resource files
// hydrated as null while the same field on the same node read correctly through
// the lens. Extension.url (FF_FIELD_URL) was dropped the same way.
//
// The bytes were never wrong: ingest, validate, export and every round-trip gate
// were green throughout, because none of them goes through `as<T>()`. The whole
// POCO materialization path -- the public API a consumer reaches for first -- was
// covered by nothing. This test is that coverage.
//
// It is deliberately a PARITY test, not a fixture-expectation test: the lens is
// the oracle, so it stays honest as the corpus changes. And it asserts a
// NON-ZERO FLOOR before it asserts agreement (TASKS.md P0-2) -- 0 == 0 is how
// this class of defect passes for months.

#include <FastFHIR.hpp>
#include <FF_Ingestor.hpp>
#include "FF_AllTypes.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace FastFHIR;

static int failures = 0;

static void CHECK(bool ok, const std::string& what) {
    printf("  %-64s %s\n", what.c_str(), ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

#ifndef FASTFHIR_SYNTHEA_DIR
#  define FASTFHIR_SYNTHEA_DIR ""
#endif

// Sorted for the same reason ff_test_roundtrip_validate sorts: directory order
// differs between machines, so an unsorted truncation makes a red irreproducible.
static std::vector<fs::path> find_bundles(std::size_t limit) {
    std::vector<fs::path> out;
    const fs::path root(FASTFHIR_SYNTHEA_DIR);
    if (root.empty()) return out;
    for (const auto& dir : {root / "fhir", root}) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".json") out.push_back(entry.path());
        }
        if (!out.empty()) break;
    }
    std::sort(out.begin(), out.end());
    if (out.size() > limit) out.resize(limit);
    return out;
}

static std::vector<BYTE> sha256(const unsigned char* data, Size len) {
    std::vector<BYTE> hash(EVP_MAX_MD_SIZE);
    unsigned int out_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash.data(), &out_len);
    EVP_MD_CTX_free(ctx);
    hash.resize(out_len);
    return hash;
}

// Every counter here is reported, not just asserted. "It passed" and "it looked
// at anything" are different claims, and this suite exists because the second
// was being read as the first.
struct Census {
    std::size_t observations       = 0;  // Observation resources reached
    std::size_t lens_code          = 0;  // code.coding[0].code readable via Node
    std::size_t poco_code          = 0;  // ... and via as<ObservationData>()
    std::size_t code_mismatch      = 0;  // both present, different text
    std::size_t lens_subject       = 0;  // subject.reference readable via Node
    std::size_t poco_subject       = 0;
    std::size_t subject_mismatch   = 0;
    std::size_t patients           = 0;  // Patient resources reached
    std::size_t lens_extensions    = 0;  // Patient.extension entries on the wire
    std::size_t poco_extensions    = 0;  // ... surviving into PatientData
    std::size_t poco_extension_url = 0;  // ... with a resolved URL-directory ref
    // Observation.value[x] as a Quantity -- a BLOCK-typed choice.
    std::size_t lens_value_qty      = 0;  // readable via Node
    std::size_t poco_value_qty      = 0;  // ... and carried as a DECODED value
    std::size_t value_qty_mismatch  = 0;  // both present, different number
};

// The lens reading of Observation.code -- the value the exporter emits and the
// benchmark's query stage searches. Empty when absent.
static std::string_view lens_first_coding_code(const Reflective::Entry& concept_entry) {
    if (!concept_entry) return {};
    auto codings = concept_entry[Fields::CODEABLECONCEPT::CODING].entries();
    if (codings.empty()) return {};
    return codings[0][Fields::CODING::CODE].as<std::string_view>();
}

static void census_observation(const Reflective::Node& resource, Census& c) {
    ++c.observations;

    const std::string_view lens_code = lens_first_coding_code(resource[Fields::OBSERVATION::CODE]);
    const std::string_view lens_subj =
        resource[Fields::OBSERVATION::SUBJECT][Fields::REFERENCE::REFERENCE].as<std::string_view>();

    const ObservationData obs = resource.as<ObservationData>();

    if (!lens_code.empty()) ++c.lens_code;
    if (obs.code && !obs.code->coding.empty() && !obs.code->coding[0].code.empty()) {
        ++c.poco_code;
        if (!lens_code.empty() && obs.code->coding[0].code != lens_code) ++c.code_mismatch;
    }

    // A BLOCK-TYPED CHOICE MUST ARRIVE AS A VALUE.
    //
    // Observation.value[x] holding a Quantity is the commonest instance -- the
    // measurement itself. The POCO used to receive the Quantity's raw ARENA
    // OFFSET in ChoiceEntry's uint64_t arm: a structural address handed out
    // through the value API, useless to the caller and actively destructive if
    // the struct was ever re-serialized into a different arena, because STORE
    // wrote that foreign address back out verbatim.
    //
    // The lens always read it correctly, which is why nothing caught it -- the
    // same shape as the FF_FIELD_BLOCK gap this file was written for.
    // Gate on the TAG before reading. value[x] is polymorphic -- the same slot
    // holds a Quantity here and a CodeableConcept or a string one resource
    // later -- so navigating QUANTITY::VALUE without checking first reads a
    // field map that does not belong to the block (it segfaulted on the first
    // Observation that was not a Quantity).
    if (obs.value.tag == RECOVER_FF_QUANTITY) {
        const auto lens_v = resource[Fields::OBSERVATION::VALUE].as_node();
        const bool lens_has = static_cast<bool>(lens_v);
        const double lens_num = lens_has ? lens_v[Fields::QUANTITY::VALUE].as<double>() : 0.0;
        if (lens_has) ++c.lens_value_qty;

        if (obs.value.block) {
            if (const auto* q = std::get_if<QuantityData>(&obs.value.block->value)) {
                ++c.poco_value_qty;
                if (lens_has && q->value != lens_num) ++c.value_qty_mismatch;
            }
        }
    }

    if (!lens_subj.empty()) ++c.lens_subject;
    if (obs.subject && !obs.subject->reference.empty()) {
        ++c.poco_subject;
        if (!lens_subj.empty() && obs.subject->reference != lens_subj) ++c.subject_mismatch;
    }
}

// Extension.url is a 4-byte FF_URL_DIRECTORY index, not a string. The POCO
// carries the index; FF_NULL_UINT32 in every slot is what "the field emitted no
// deserialize code" looks like from the outside.
static void census_patient(const Reflective::Node& resource, Census& c) {
    ++c.patients;
    c.lens_extensions += resource[Fields::PATIENT::EXTENSION].entries().size();

    const PatientData pat = resource.as<PatientData>();
    c.poco_extensions += pat.extension.size();
    for (const auto& ext : pat.extension) {
        if (ext.url != FF_NULL_UINT32) ++c.poco_extension_url;
    }
}

static bool census_bundle(const fs::path& fixture, Census& c) {
    std::ifstream f(fixture, std::ios::binary | std::ios::ate);
    if (!f) { printf("    cannot open %s\n", fixture.string().c_str()); return false; }
    const auto size = f.tellg();
    f.seekg(0);
    std::string json(static_cast<std::size_t>(size), '\0');
    f.read(json.data(), size);

    auto mem = Memory::create(2ull * 1024 * 1024 * 1024);
    FF_StreamCreateInfo stream_info;
    stream_info.arena = std::make_shared<Memory>(mem);
    stream_info.version = FHIR_VERSION_R5;
    FF_Stream stream;
    if (!FF_CreateStream(stream_info, stream)) return false;

    FF_IngestorCreateInfo ingestor_info;
    FF_Ingestor ingestor;
    if (!FF_CreateIngestor(ingestor_info, ingestor)) return false;

    Reflective::ObjectHandle root_handle;
    Size resource_count = 0;
    // FILTER_NONE so every Extension.url reaches the URL directory -- the
    // default suppresses profile-native ones, which would hide the FF_FIELD_URL
    // half of this test behind a filter rather than a defect.
    const auto ingest = FF_Ingest(FF_IngestInfo{
        .ingestor = ingestor,
        .stream = stream,
        .source_type = FF_SOURCE_FHIR_JSON,
        .extension_filter = FF_ExtensionFilterMode::FILTER_NONE,
        .payload = json,
    }, root_handle, resource_count);
    if (ingest.failed()) { printf("    ingest failed: %s\n", ingest.message.c_str()); return false; }

    if (!FF_StreamSetRoot(FF_StreamSetRootInfo{.stream = stream, .root = root_handle})) return false;

    Memory::View view;
    if (!FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream, .algorithm = FF_CHECKSUM_SHA256, .hasher = sha256}, view))
        return false;
    if (view.empty()) return false;

    Parser parser(mem);
    auto root = parser.root();
    if (!root) { printf("    re-parsed root is null\n"); return false; }

    const auto entries = root[Fields::BUNDLE::ENTRY].entries();
    if (entries.empty()) { printf("    bundle has no entries\n"); return false; }

    for (const auto& entry : entries) {
        const auto resource = entry[Fields::BUNDLE_ENTRY::RESOURCE].as_node();
        if (!resource) continue;
        switch (resource.recovery()) {
            case RECOVER_FF_OBSERVATION: census_observation(resource, c); break;
            case RECOVER_FF_PATIENT:     census_patient(resource, c);     break;
            default: break;
        }
    }
    return true;
}

// ── Shared scaffolding for the synthetic probes ─────────────────────────────
//
// Both probes below need the same six steps -- arena, stream, ingestor, ingest,
// set-root, finalize -- and differ only in the JSON they feed and what they
// then assert. One pipeline, so each probe is just the thing it is testing.
//
// The arena and the Parser travel WITH the node: Reflective::Node is a
// non-owning lens, so both must outlive it. Members destroy in reverse
// declaration order, which retires the Parser before the arena it reads.
struct SyntheticObservation {
    Memory                  arena;
    std::unique_ptr<Parser> parser;
    Reflective::Node        node;

    explicit operator bool() const { return parser != nullptr && static_cast<bool>(node); }
};

// 16 MiB: these documents hold ONE Observation. Every failure reports through
// CHECK rather than returning silently -- a probe that cannot build its fixture
// has to say so, not read as a pass on nothing.
static SyntheticObservation ingest_first_observation(const std::string& json) {
    SyntheticObservation out{Memory::create(16ull * 1024 * 1024), nullptr, {}};

    FF_StreamCreateInfo stream_info;
    stream_info.arena = std::make_shared<Memory>(out.arena);
    stream_info.version = FHIR_VERSION_R5;
    FF_Stream stream;
    if (!FF_CreateStream(stream_info, stream)) { CHECK(false, "synthetic: create stream"); return out; }

    FF_IngestorCreateInfo ingestor_info;
    FF_Ingestor ingestor;
    if (!FF_CreateIngestor(ingestor_info, ingestor)) { CHECK(false, "synthetic: create ingestor"); return out; }

    Reflective::ObjectHandle root_handle;
    Size resource_count = 0;
    const auto ingest = FF_Ingest(FF_IngestInfo{
        .ingestor = ingestor,
        .stream = stream,
        .source_type = FF_SOURCE_FHIR_JSON,
        .extension_filter = FF_ExtensionFilterMode::FILTER_NONE,
        .payload = json,
    }, root_handle, resource_count);
    if (ingest.failed()) { CHECK(false, "synthetic: ingest failed"); return out; }
    if (!FF_StreamSetRoot(FF_StreamSetRootInfo{.stream = stream, .root = root_handle})) {
        CHECK(false, "synthetic: set root"); return out;
    }

    Memory::View view;
    if (!FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream, .algorithm = FF_CHECKSUM_SHA256, .hasher = sha256}, view)) {
        CHECK(false, "synthetic: finalize failed"); return out;
    }

    out.parser = std::make_unique<Parser>(out.arena);
    const auto root = out.parser->root();
    if (root) {
        for (const auto& entry : root[Fields::BUNDLE::ENTRY].entries()) {
            const auto resource = entry[Fields::BUNDLE_ENTRY::RESOURCE].as_node();
            if (resource && resource.recovery() == RECOVER_FF_OBSERVATION) {
                out.node = resource;
                return out;
            }
        }
    }
    CHECK(false, "synthetic: no Observation in bundle");
    return out;
}

// ── Synthetic probe: a date/time CHOICE in fallback form ────────────────────
//
// The corpus probe above cannot reach the defect this targets: Synthea
// date/times all pack, and a PACKED slot is its own value. The FALLBACK form --
// legal FHIR text that does not fit the 63-bit civil slot (here: seven
// fractional digits) -- is stored as an FF_STRING whose flagged RELATIVE OFFSET
// sits in the choice slot. The lens always resolved that offset, because it
// walks with the parent block in hand; the POCO path could not until the
// containing block was plumbed into the generated choice decode, and before
// that, deserialize read the 8 slot bytes raw: the choice arrived as a NUMBER.
//
// Deliberately self-contained (no Census, no fixture file): it must compile
// and run against BOTH the pre-fix and the fixed generator, so it touches only
// the ChoiceEntry surface both sides share -- tag + value -- and uses the lens
// as the oracle.
static void probe_fallback_datetime_choice() {
    const std::string json = R"({"resourceType":"Bundle","type":"collection","entry":[
      {"fullUrl":"urn:uuid:o1","resource":{
        "resourceType":"Observation","id":"o1","status":"final",
        "code":{"coding":[{"system":"http://loinc.org","code":"718-7","display":"Hemoglobin [Mass/volume] in Blood"}]},
        "subject":{"reference":"Patient/p1"},
        "effectiveDateTime":"2019-04-01T13:45:30.1234567+05:30"}}]})";

    const auto fixture = ingest_first_observation(json);
    if (!fixture) return;

    const ObservationData obs = fixture.node.as<ObservationData>();
    if (obs.effective.tag != RECOVER_FF_DATETIME) {
        CHECK(false, "synthetic: effective[x] must hold a dateTime");
        return;
    }

    const auto* text = std::get_if<std::string_view>(&obs.effective.value);
    if (text == nullptr) {
        CHECK(false, "POCO date/time CHOICE (fallback) arrives as text, not a raw slot");
        return;
    }

    const auto lens = fixture.node[Fields::OBSERVATION::EFFECTIVE].as_node();
    const bool lens_has = static_cast<bool>(lens);
    CHECK(lens_has, "lens reads the fallback date/time (non-zero floor)");
    if (!lens_has) return;
    CHECK(*text == lens.as<std::string_view>(),
          "POCO and lens agree on the fallback date/time text");
}

// ── Synthetic probe: a PACKED date/time CHOICE renders via to_string() ─────
//
// The fallback probe above exercises the decode half; this one exercises the
// RENDER half. A packed slot has no text on the wire -- the 8 bytes are civil
// parts -- so the generated decode carries the packed value (by design) and
// text is SYNTHESIZED by ChoiceEntry::to_string() when a caller asks. The lens
// cannot be the oracle here: it has no text to offer for a packed slot either
// (it formats only at print time), so the oracle is the value itself -- the
// rendered text must REPACK to the exact civil value the slot holds.
//
// This probe is compile-red at the parent commit (2205cae): to_string() was
// added by the very change this suite gates, so a test of the capability
// cannot compile against the old API -- its red phase is a compile failure,
// the same class as the .block census checks above.
static void probe_packed_datetime_to_string() {
    // Three fractional digits fit the packed slot -- no fallback. The offset is
    // +05:30 so the assertion never depends on how "Z" is normalized.
    const std::string json = R"({"resourceType":"Bundle","type":"collection","entry":[
      {"fullUrl":"urn:uuid:o2","resource":{
        "resourceType":"Observation","id":"o2","status":"final",
        "code":{"coding":[{"system":"http://loinc.org","code":"718-7","display":"Hemoglobin [Mass/volume] in Blood"}]},
        "effectiveDateTime":"2019-04-01T13:45:30.123+05:30"}}]})";

    const auto fixture = ingest_first_observation(json);
    if (!fixture) return;

    const ObservationData obs = fixture.node.as<ObservationData>();
    if (obs.effective.tag != RECOVER_FF_DATETIME) {
        CHECK(false, "synthetic: effective[x] must hold a dateTime");
        return;
    }

    const auto* raw = std::get_if<uint64_t>(&obs.effective.value);
    CHECK(raw != nullptr, "packed date/time CHOICE carries the civil value");
    if (raw == nullptr) return;
    CHECK(*raw != FF_DATETIME_NULL, "packed slot present (non-zero floor)");
    if (*raw == FF_DATETIME_NULL) return;

    // The oracle is the VALUE, not the lens: a packed slot has no text on the
    // wire for either side to read, so the rendered text has to repack to the
    // exact civil value the slot holds.
    const std::string text = obs.effective.to_string();
    CHECK(!text.empty(), "packed date/time CHOICE renders civil text");
    if (text.empty()) return;
    const auto reparsed = FF_PARSE_DATETIME(text, RECOVER_FF_DATETIME);
    CHECK(reparsed.has_value() && FF_PACK_DATETIME(*reparsed) == *raw,
          "rendered text re-packs to the same civil value");
}


// ── The two reflection surfaces must agree ──────────────────────────────────
//
// FastFHIR now reflects a document two ways: reflected_fields_view() walks the
// WIRE (it needs an arena and an offset), and visit_fields() walks the POCO
// (it needs neither). Both are generated from the same layout, so they must
// enumerate the same elements in the same order -- and if they ever drift, a
// caller reading through one surface silently sees a different document than a
// caller reading through the other.
//
// That is not hypothetical. This whole file exists because the POCO
// deserializer once dropped 736 block-typed fields the lens read correctly, and
// nothing noticed for months. A generated cross-check is cheaper than
// rediscovering it.
static void test_poco_reflection_matches_the_wire() {
    std::vector<std::string> poco;
    const PatientData d{};
    visit_fields(d, [&](const char* name, const auto&) { poco.emplace_back(name); });

    std::vector<std::string> wire;
    for (const auto& f : reflected_fields_view(RECOVER_FF_PATIENT))
        wire.emplace_back(f.name);

    // Non-zero floor first: two empty lists compare equal, which is how a
    // vacuous check passes forever.
    CHECK(!poco.empty(), "visit_fields enumerates PatientData (non-zero)");
    CHECK(!wire.empty(), "reflected_fields_view enumerates Patient (non-zero)");
    CHECK(poco.size() == wire.size(),
          "POCO and wire reflection list the same NUMBER of fields (" +
              std::to_string(poco.size()) + " vs " + std::to_string(wire.size()) + ")");
    CHECK(poco == wire, "POCO and wire reflection agree field for field, in order");
}

int main() {
    // BEFORE the corpus gate: both probes build their own document inline and
    // need no fixture. Below the gate they were skipped entirely whenever
    // FASTFHIR_SYNTHEA_DIR is empty (it defaults to "" with
    // FASTFHIR_DOWNLOAD_SYNTHEA=OFF -- an offline or air-gapped build), which
    // is a pass on zero coverage for the newest code in the tree: exactly the
    // reporting failure the SKIP branch below exists to avoid.
    probe_fallback_datetime_choice();
    probe_packed_datetime_to_string();
    test_poco_reflection_matches_the_wire();

    const auto bundles = find_bundles(8);
    if (bundles.empty()) {
        // A supported configuration, but say so loudly: a pass on zero coverage
        // is the reporting failure this whole task is about.
        printf("SKIP: no Synthea fixtures (FASTFHIR_SYNTHEA_DIR unset or empty)\n");
        // NOT an unconditional 0: the synthetic probes above have already run
        // and may have failed. Returning 0 here regardless would report their
        // failure as a pass -- the same reporting failure this branch exists to
        // avoid, reintroduced one line below the comment saying so.
        printf("%s\n", failures ? "FAILURES" : "synthetic probes only (no corpus)");
        return failures ? 1 : 0;
    }

    printf("POCO/lens parity over %zu Synthea bundles\n", bundles.size());
    Census c;
    for (const auto& b : bundles) {
        const std::string name = b.filename().string().substr(0, 40);
        CHECK(census_bundle(b, c), std::string("ingest + census ") + name);
    }

    printf("  observations=%zu  lens_code=%zu  poco_code=%zu  lens_subject=%zu poco_subject=%zu\n",
           c.observations, c.lens_code, c.poco_code, c.lens_subject, c.poco_subject);
    printf("  patients=%zu  lens_extensions=%zu  poco_extensions=%zu  poco_extension_url=%zu\n",
           c.patients, c.lens_extensions, c.poco_extensions, c.poco_extension_url);

    // ── Floors first (TASKS.md P0-2) ─────────────────────────────────────────
    // Every equality below is vacuous if the corpus stopped containing the thing
    // being compared. Assert presence before agreement, always.
    CHECK(c.observations > 0, "corpus yields Observations at all");
    CHECK(c.lens_code > 0,    "lens reads Observation.code.coding[0].code (non-zero)");
    CHECK(c.lens_subject > 0, "lens reads Observation.subject.reference (non-zero)");
    CHECK(c.patients > 0,     "corpus yields Patients at all");
    CHECK(c.lens_extensions > 0, "lens reads Patient.extension entries (non-zero)");

    // ── Parity: the POCO must see what the lens sees ─────────────────────────
    // Singular block field, 0/93 before the fix.
    CHECK(c.poco_code == c.lens_code,
          "as<ObservationData>().code matches the lens (" + std::to_string(c.poco_code) +
              "/" + std::to_string(c.lens_code) + ")");
    // Non-zero floor first: 0 == 0 is how this class of defect passes for
    // months, and it is exactly how the offset-in-a-value-slot survived.
    CHECK(c.lens_value_qty > 0,
          "lens reads Observation.value[x] as a Quantity (non-zero)");
    CHECK(c.poco_value_qty == c.lens_value_qty,
          "POCO carries every block-typed choice the lens reads");
    CHECK(c.value_qty_mismatch == 0,
          "POCO and lens agree on the Quantity's value");

    CHECK(c.poco_subject == c.lens_subject,
          "as<ObservationData>().subject matches the lens (" + std::to_string(c.poco_subject) +
              "/" + std::to_string(c.lens_subject) + ")");
    // Equal counts with different values would be a decode bug, not a drop.
    CHECK(c.code_mismatch == 0,
          "no Observation.code text disagrees with the lens (" +
              std::to_string(c.code_mismatch) + " mismatches)");
    CHECK(c.subject_mismatch == 0,
          "no Observation.subject text disagrees with the lens (" +
              std::to_string(c.subject_mismatch) + " mismatches)");

    // Repeated block field -- this arm always worked; it pins that the fix did
    // not regress the ARRAY branch it sits next to.
    CHECK(c.poco_extensions == c.lens_extensions,
          "as<PatientData>().extension matches the lens (" + std::to_string(c.poco_extensions) +
              "/" + std::to_string(c.lens_extensions) + ")");
    // FF_FIELD_URL, 0/N before the fix.
    CHECK(c.poco_extension_url == c.poco_extensions,
          "every hydrated Extension carries its URL ref (" +
              std::to_string(c.poco_extension_url) + "/" +
              std::to_string(c.poco_extensions) + ")");

    printf("%s\n", failures ? "FAILURES" : "POCO and lens agree on every counted field");
    return failures ? 1 : 0;
}
