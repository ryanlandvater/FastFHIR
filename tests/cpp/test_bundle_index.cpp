/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file test_bundle_index.cpp
 * @brief FF_BundleIndex resolves what a linear scan resolves (TASKS.md T11).
 *
 * The oracle is the O(N) scan the index replaces -- the one the CAP example
 * carries -- so the property under test is agreement, not a restatement of the
 * implementation. Streams are built through the real writer and read back
 * through the real Parser, never hand-assembled (COV-1).
 *
 *   parity       every reference resolves to the same resource both ways
 *   urn          a urn:uuid: fullUrl is matched whole
 *   relative     Type/id resolves against an absolute fullUrl that ends in it
 *   ambiguous    a relative key two entries answer resolves to NEITHER
 *   unresolved   absent, external and identifier-only references are falsy
 *   degenerate   a non-Bundle and an empty Bundle index to nothing, no throw
 */

#include <FastFHIR.hpp>
#include <FF_BundleIndex.hpp>
#include <FF_FieldKeys.hpp>
#include "FF_AllTypes.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "FFHR_tests.hpp"
using namespace FastFHIR;

namespace
{

constexpr Size kArena = 8ull << 20;

/// One entry's worth of intent: what its fullUrl says and who it points at.
struct EntrySpec
{
    std::string full_url;
    std::string subject_ref;  // empty for no subject at all
};

/// The scan FF_BundleIndex replaces, kept verbatim as the oracle: walk every
/// entry and compare fullUrl. If the index and this ever disagree, one of them
/// is wrong and the test does not care which.
Reflective::Node scan_for(const Reflective::Node &bundle, std::string_view target)
{
    if (target.empty()) return {};
    const auto entries = bundle[Fields::BUNDLE::ENTRY];
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const Reflective::Node entry = entries[i];
        if (static_cast<std::string_view>(entry[Fields::BUNDLE_ENTRY::FULL_URL]) != target)
            continue;
        return entry[Fields::BUNDLE_ENTRY::RESOURCE].as_node();
    }
    return {};
}

/// Build and seal a Bundle of Observations, one per spec, collect-then-
/// serialize (README 6a). Returns the arena so the caller can parse it.
Memory build_bundle(const std::vector<EntrySpec> &specs)
{
    Memory mem = Memory::create(kArena);

    FF_BuilderCreateInfo info;
    info.arena   = mem;
    info.version = FHIR_VERSION_R5;
    FF_Builder builder;
    if (!FF_CreateBuilder(info, builder)) throw std::runtime_error("FF_CreateBuilder failed");

    BundleData bundle;
    bundle.type = FF_BundleType::Collection;
    bundle.entry.resize(specs.size());

    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        ObservationData observation;
        // FF_String copies a std::string, so these locals may die here -- the
        // deque of backing strings the older bundle tests carry is no longer
        // needed for this shape.
        observation.id     = "obs-" + std::to_string(i);
        observation.status = FF_ObservationStatus::Final;
        if (!specs[i].subject_ref.empty())
            observation.subject = ReferenceData{.reference = specs[i].subject_ref};

        bundle.entry[i].fullurl  = specs[i].full_url;
        bundle.entry[i].resource = static_cast<ResourceReference>(builder->append_obj(observation));
    }

    const Reflective::ObjectHandle root = builder->append_obj(bundle);
    FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root});
    Memory::View view;
    if (!FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, view))
        throw std::runtime_error("seal failed");
    return mem;
}

/// The `id` of a resolved resource, or "" -- the cheapest way to say "these two
/// resolutions landed on the same resource" without a Node comparison operator.
std::string id_of(const Reflective::Node &resource)
{
    if (!resource) return {};
    const auto id = resource[Fields::OBSERVATION::ID];
    return id ? std::string(static_cast<std::string_view>(id)) : std::string{};
}

// ── Cases ────────────────────────────────────────────────────────────────────

void the_index_agrees_with_a_linear_scan()
{
    TEST_GROUP("parity");

    // Ring of references: every Observation points at the next entry's fullUrl,
    // so all N references resolve and none is self-referential.
    constexpr std::size_t kCount = 12;
    std::vector<EntrySpec> specs(kCount);
    for (std::size_t i = 0; i < kCount; ++i)
    {
        specs[i].full_url    = "urn:uuid:res-" + std::to_string(i);
        specs[i].subject_ref = "urn:uuid:res-" + std::to_string((i + 1) % kCount);
    }

    Memory mem = build_bundle(specs);
    Parser parser(mem);
    const Reflective::Node bundle = parser.root();

    const BundleIndex index(bundle);
    REQUIRE(index.size() == kCount, "every entry indexed, got " << index.size());

    const auto entries = bundle[Fields::BUNDLE::ENTRY];
    REQUIRE(entries.size() == kCount, "the Bundle round-tripped its entries");

    // P0-2: count the comparisons before asserting anything about them, or an
    // empty walk satisfies every assertion below it.
    std::size_t compared = 0;
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const Reflective::Node observation = entries[i][Fields::BUNDLE_ENTRY::RESOURCE].as_node();
        const auto subject = observation[Fields::OBSERVATION::SUBJECT];
        if (!subject) continue;
        const auto ref_text = subject.as_node()[Fields::REFERENCE::REFERENCE];
        if (!ref_text) continue;

        const auto target = static_cast<std::string_view>(ref_text);
        const std::string scanned = id_of(scan_for(bundle, target));
        const std::string indexed = id_of(index.resolve(target));
        const std::string via_block = id_of(index.resolve(subject.as_node()));

        CHECK(!scanned.empty(), "the scan resolved " << target);
        CHECK(indexed == scanned, "index and scan agree for " << target
                                  << ": " << indexed << " vs " << scanned);
        CHECK(via_block == scanned, "the Reference-block overload agrees too");
        ++compared;
    }
    REQUIRE(compared == kCount, "every reference was compared, got " << compared);
}

void a_urn_full_url_is_matched_whole()
{
    TEST_GROUP("urn");

    Memory mem = build_bundle({{"urn:uuid:aaaa-bbbb", ""}, {"urn:oid:1.2.3.4", ""}});
    Parser parser(mem);
    const BundleIndex index(parser.root());

    CHECK(id_of(index.resolve("urn:uuid:aaaa-bbbb")) == "obs-0", "the uuid urn resolves");
    CHECK(id_of(index.resolve("urn:oid:1.2.3.4")) == "obs-1", "the oid urn resolves");
    // A urn has no Type/id tail, so no prefix or suffix of it may resolve.
    CHECK(!index.resolve("uuid:aaaa-bbbb"), "a urn is not matched by its tail");
    CHECK(!index.resolve("urn:uuid:aaaa"), "nor by a prefix");
}

void a_relative_reference_resolves_against_an_absolute_full_url()
{
    TEST_GROUP("relative");

    Memory mem = build_bundle({{"http://example.org/fhir/Observation/rel-a", ""},
                               {"http://other.example.org/base/Observation/rel-b", ""}});
    Parser parser(mem);
    const BundleIndex index(parser.root());

    CHECK(id_of(index.resolve("http://example.org/fhir/Observation/rel-a")) == "obs-0",
          "the absolute fullUrl resolves");
    CHECK(id_of(index.resolve("Observation/rel-a")) == "obs-0",
          "and so does the relative form FHIR defines for it");
    CHECK(id_of(index.resolve("Observation/rel-b")) == "obs-1",
          "including across different bases");
    CHECK(!index.resolve("Observation/absent"), "an unknown relative key resolves to nothing");
    CHECK(!index.resolve("rel-a"), "the bare id is not a reference and does not resolve");
}

/// fullUrls that are not `.../Type/id` must contribute no relative key at all.
/// A rooted path is the one that bites: deriving the key by scanning back from
/// the last slash wraps past the front of "/abc" and indexes "abc".
void an_odd_full_url_contributes_no_relative_key()
{
    TEST_GROUP("relative");

    Memory mem = build_bundle({{"/abc", ""},
                               {"http://host", ""},
                               {"http://host/", ""},
                               {"Observation/bare", ""}});
    Parser parser(mem);
    const BundleIndex index(parser.root());

    // Each still resolves by its exact fullUrl -- nothing is lost.
    CHECK(id_of(index.resolve("/abc")) == "obs-0", "a rooted path resolves exactly");
    CHECK(id_of(index.resolve("http://host")) == "obs-1", "a host-only URL resolves exactly");
    CHECK(id_of(index.resolve("http://host/")) == "obs-2", "a trailing slash resolves exactly");
    CHECK(id_of(index.resolve("Observation/bare")) == "obs-3",
          "a fullUrl that is ALREADY relative resolves exactly");

    // ...and none of them invents a relative key.
    CHECK(!index.resolve("abc"), "\"/abc\" does not index \"abc\"");
    CHECK(!index.resolve("host"), "\"http://host\" does not index \"host\"");
    CHECK(!index.resolve("//host"), "nor \"//host\"");
}

void an_ambiguous_relative_key_resolves_to_neither()
{
    TEST_GROUP("ambiguous");

    // Two different servers, same Type/id. `Observation/dup` names neither.
    Memory mem = build_bundle({{"http://a.example.org/fhir/Observation/dup", ""},
                               {"http://b.example.org/fhir/Observation/dup", ""}});
    Parser parser(mem);
    const BundleIndex index(parser.root());

    CHECK(!index.resolve("Observation/dup"),
          "an ambiguous relative key resolves to nothing rather than to the first entry");
    // The exact fullUrls are still unambiguous and must still work.
    CHECK(id_of(index.resolve("http://a.example.org/fhir/Observation/dup")) == "obs-0",
          "the first absolute fullUrl still resolves");
    CHECK(id_of(index.resolve("http://b.example.org/fhir/Observation/dup")) == "obs-1",
          "and so does the second");
}

void unresolvable_references_are_falsy_not_fatal()
{
    TEST_GROUP("unresolved");

    Memory mem = build_bundle({{"urn:uuid:present", "http://elsewhere.example.org/fhir/Patient/x"},
                               {"urn:uuid:other", ""}});
    Parser parser(mem);
    const Reflective::Node bundle = parser.root();
    const BundleIndex index(bundle);

    // Invariant 10: the read path degrades. None of these may throw.
    CHECK(!index.resolve(""), "an empty reference is falsy");
    CHECK(!index.resolve("urn:uuid:never-written"), "an absent urn is falsy");
    CHECK(!index.resolve("http://elsewhere.example.org/fhir/Patient/x"),
          "a reference to a resource outside this Bundle is falsy");

    // An identifier-only Reference names no target in this stream. The block
    // overload must report that rather than reaching through a missing field.
    const Reflective::Node first =
        bundle[Fields::BUNDLE::ENTRY][size_t{0}][Fields::BUNDLE_ENTRY::RESOURCE].as_node();
    const auto subject = first[Fields::OBSERVATION::SUBJECT];
    REQUIRE(static_cast<bool>(subject), "the fixture's external subject is present");
    CHECK(!index.resolve(subject.as_node()), "an external Reference block is falsy");

    const Reflective::Node second =
        bundle[Fields::BUNDLE::ENTRY][size_t{1}][Fields::BUNDLE_ENTRY::RESOURCE].as_node();
    CHECK(!index.resolve(second[Fields::OBSERVATION::SUBJECT].as_node()),
          "so is a Reference block that was never written");
}

void a_bundleless_node_indexes_to_nothing()
{
    TEST_GROUP("degenerate");

    // An empty Bundle is a legal document.
    Memory empty_mem = build_bundle({});
    Parser empty_parser(empty_mem);
    const BundleIndex empty_index(empty_parser.root());
    CHECK(empty_index.empty(), "an entry-less Bundle indexes nothing");
    CHECK(!empty_index.resolve("urn:uuid:anything"), "and resolves nothing");

    // A node that is not a Bundle at all: the wrong-node case must report
    // itself through empty()/falsy rather than by throwing.
    Memory mem = build_bundle({{"urn:uuid:only", ""}});
    Parser parser(mem);
    const Reflective::Node observation =
        parser.root()[Fields::BUNDLE::ENTRY][size_t{0}][Fields::BUNDLE_ENTRY::RESOURCE].as_node();
    REQUIRE(observation.is<RESOURCETYPE::OBSERVATION>(), "the fixture resource is an Observation");

    const BundleIndex wrong(observation);
    CHECK(wrong.empty(), "indexing a non-Bundle yields an empty index");
    CHECK(!wrong.resolve("urn:uuid:only"), "and resolves nothing");
}

}  // namespace

int main(int argc, char **argv)
{
    ff_test::set_filter(argc, argv);
    ff_test::run("parity", the_index_agrees_with_a_linear_scan);
    ff_test::run("urn", a_urn_full_url_is_matched_whole);
    ff_test::run("relative", a_relative_reference_resolves_against_an_absolute_full_url);
    ff_test::run("relative", an_odd_full_url_contributes_no_relative_key);
    ff_test::run("ambiguous", an_ambiguous_relative_key_resolves_to_neither);
    ff_test::run("unresolved", unresolvable_references_are_falsy_not_fatal);
    ff_test::run("degenerate", a_bundleless_node_indexes_to_nothing);
    return ff_test::report("FF_BundleIndex resolves what a linear scan resolves");
}
