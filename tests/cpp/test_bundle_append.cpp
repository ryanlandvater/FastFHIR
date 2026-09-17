/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// APPEND-1: serialize_bundle_array and the tail-rewrite append
// (include/FF_BundleAppend.hpp).
//
// The claim under test is about BYTES, so the assertions are too. Every append
// is checked three ways:
//
//   1. growth    -- the stream grows by exactly the new resources plus one
//                   84-byte entry each (the tail path), and no more;
//   2. untouched -- every byte below the rollback point is identical after
//                   the append (the header, which the reseal restamps, is the
//                   only exception, and it sits below every array);
//   3. the map   -- FastFHIR's own StreamMap producers (FF_Recovery.hpp):
//                   a block the byte scan finds but the root walk cannot reach
//                   is dead space. A tail rewrite must add none; a relocation
//                   adds exactly the orphaned array.
//
// Streams are built through the real writer, never by hand (COV-1).

#include <FastFHIR.hpp>
#include <FF_BundleAppend.hpp>
#include <FF_FieldKeys.hpp>
#include "FF_AllTypes.hpp"
#include "FF_Bundle_internal.hpp"
#include "FF_Recovery.hpp"

#include <cstring>
#include <deque>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "FFHR_tests.hpp"
using namespace FastFHIR;

namespace {

constexpr Size kArena = 64ull << 20;
constexpr Size kEntry = FF_BUNDLE_ENTRY::HEADER_SIZE;

enum class Layout { Tail, Backfill };

struct BuildSpec {
    Layout layout = Layout::Tail;
    size_t count = 5;
    bool entry_children = false;  // fullUrl + request on every entry
    bool signature = false;       // Bundle.signature, STOREd after the entries
};

// string_views in the POCOs point here; a deque never moves its elements.
using Storage = std::deque<std::string>;

ObservationData make_observation(Storage& storage, const std::string& id)
{
    ObservationData obs;
    obs.id = storage.emplace_back(id);
    obs.status = FF_ObservationStatus::Final;
    return obs;
}

FF_Builder open_builder(const Memory& mem)
{
    FF_BuilderCreateInfo info;
    info.arena = std::make_shared<Memory>(mem);
    info.version = FHIR_VERSION_R5;
    FF_Builder builder;
    if (!FF_CreateBuilder(info, builder))
        throw std::runtime_error("FF_CreateBuilder failed");
    return builder;
}

bool seal(const FF_Builder& builder, Memory::View& view)
{
    return static_cast<bool>(FF_BuilderFinalize(FF_BuilderFinalizeInfo{
        .builder = builder,
        .algorithm = FF_CHECKSUM_NONE,
    }, view));
}

// Build a sealed Bundle of `spec.count` Observations named obs-0.. .
Memory build_stream(const BuildSpec& spec, Storage& storage)
{
    Memory mem = Memory::create(kArena);
    FF_Builder builder = open_builder(mem);

    BundleData bundle;
    bundle.type = FF_BundleType::Collection;
    bundle.entry.resize(spec.count);
    for (size_t i = 0; i < spec.count; ++i) {
        if (!spec.entry_children)
            continue;
        bundle.entry[i].fullurl = storage.emplace_back("urn:uuid:entry-" + std::to_string(i));
        bundle.entry[i].request = std::make_unique<BundleentryrequestData>();
        bundle.entry[i].request->method = FF_HTTPVerb::POST;
        bundle.entry[i].request->url = storage.emplace_back("Observation/" + std::to_string(i));
    }
    if (spec.signature) {
        bundle.signature = std::make_unique<SignatureData>();
        bundle.signature->targetformat = storage.emplace_back("application/fhir+json");
    }

    Reflective::ObjectHandle root;
    if (spec.layout == Layout::Tail) {
        // README 6a: resources first, the Bundle (and its array) last.
        for (size_t i = 0; i < spec.count; ++i) {
            bundle.entry[i].resource = static_cast<ResourceReference>(
                builder->append_obj(make_observation(storage, "obs-" + std::to_string(i))));
        }
        root = builder->append_obj(bundle);
    } else {
        // README 6b: the Bundle and its N empty entries first, then backfill.
        root = builder->append_obj(bundle);
        Reflective::ObjectHandle entries = root[Fields::BUNDLE::ENTRY];
        for (size_t i = 0; i < spec.count; ++i) {
            entries[i][Fields::BUNDLE_ENTRY::RESOURCE] =
                builder->append_obj(make_observation(storage, "obs-" + std::to_string(i)));
        }
    }
    FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root});
    Memory::View view;
    if (!seal(builder, view))
        throw std::runtime_error("seal failed");
    return mem;
}

struct AppendOutcome {
    bool ok = false;
    FF_BundleAppendResult result;
    Size resources_bytes = 0;  // what the appended resources claimed
};

// Reopen the sealed stream, append `ids`, reseal.
AppendOutcome append_ids(const Memory& mem, Storage& storage, const std::vector<std::string>& ids,
                         bool throw_midway = false)
{
    AppendOutcome out;
    FF_Builder builder = open_builder(mem);
    const FF_Result r = FF_BundleAppendEntries(
        FF_BundleAppendInfo{
            .builder = builder,
            .append =
                [&](Builder& b, std::vector<BundleentryData>& new_entries) {
                    for (const auto& id : ids) {
                        ObservationData obs = make_observation(storage, id);
                        out.resources_bytes += TypeTraits<ObservationData>::size(obs, FHIR_VERSION_R5);
                        BundleentryData entry;
                        entry.resource = static_cast<ResourceReference>(b.append_obj(obs));
                        new_entries.push_back(std::move(entry));
                        if (throw_midway)
                            throw std::runtime_error("injected failure");
                    }
                },
        },
        out.result);
    out.ok = static_cast<bool>(r);
    Memory::View view;
    if (!seal(builder, view))
        throw std::runtime_error("reseal failed");
    return out;
}

Size sealed_size(const Memory& mem) { return Parser(mem).size_bytes(); }

// Raw slot reads. The lens hides block offsets by design; these tests are
// about where bytes live, so they read the wire directly.
Offset root_offset(const Memory& mem) { return LOAD_U64(mem.base() + FF_HEADER::ROOT_OFFSET); }
Offset bundle_slot(const Memory& mem, Size slot) { return LOAD_U64(mem.base() + root_offset(mem) + slot); }

std::vector<std::string> entry_ids(const Memory& mem)
{
    std::vector<std::string> ids;
    Parser parser(mem);
    auto root = parser.root();
    if (auto entries = root[Fields::BUNDLE::ENTRY]) {
        for (auto& entry : entries.entries()) {
            auto node = entry[Fields::BUNDLE_ENTRY::RESOURCE].as_node();
            std::string_view id = node ? std::string_view(node[Fields::OBSERVATION::ID]) : "<null>";
            ids.emplace_back(id);
        }
    }
    return ids;
}

std::vector<std::string> expected_ids(size_t count, const std::vector<std::string>& extra = {})
{
    std::vector<std::string> ids;
    for (size_t i = 0; i < count; ++i)
        ids.push_back("obs-" + std::to_string(i));
    ids.insert(ids.end(), extra.begin(), extra.end());
    return ids;
}

std::string describe_dead(const Memory& mem, const std::set<Offset>& offsets)
{
    Recovery recovery(mem);
    const StreamMap scanned = recovery.scan();
    std::string out;
    for (Offset o : offsets) {
        const auto it = scanned.find(o);
        out += " @" + std::to_string(o) + "(type " +
               std::to_string(it == scanned.end() ? -1 : static_cast<int>(it->second.type)) + ", tag 0x";
        char hex[8];
        std::snprintf(hex, sizeof hex, "%04x", it == scanned.end() ? 0u : static_cast<unsigned>(it->second.recovery));
        out += std::string(hex) + ", size " + std::to_string(it == scanned.end() ? 0 : it->second.size) + ")";
    }
    return out + " of " + std::to_string(mem.size());
}

std::string differing_ranges(const std::string& a, const std::string& b, Offset base_offset)
{
    std::string out;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (a[i] == b[i]) continue;
        size_t j = i;
        while (j < a.size() && j < b.size() && a[j] != b[j]) ++j;
        out += " [" + std::to_string(base_offset + i) + "," + std::to_string(base_offset + j) + ")";
        i = j;
    }
    return out;
}

// Blocks the byte scan finds that the root walk cannot reach: dead space.
std::set<Offset> dead_blocks(const Memory& mem)
{
    Recovery recovery(mem);
    const StreamMap scanned = recovery.scan();
    const StreamMap live = recovery.reachable_blocks_map();
    // The checksum block is referenced by the stream header, not by the root,
    // so the root walk never reaches it -- and every reseal moves it to the
    // new end. It is structure, not dead space.
    const Offset checksum = LOAD_U64(mem.base() + FF_HEADER::CHECKSUM_OFFSET);
    std::set<Offset> dead;
    for (const auto& [offset, entry] : scanned) {
        if (entry.type == StreamMapEntryType::Header || offset == checksum)
            continue;
        if (!live.count(offset))
            dead.insert(offset);
    }
    return dead;
}

// The bytes a tail rewrite must leave alone: everything between the stream
// header (restamped by every reseal) and the rollback point.
std::string bytes_between(const Memory& mem, Offset from, Offset to)
{
    return std::string(reinterpret_cast<const char*>(mem.base()) + from, to - from);
}

bool stream_valid(const Memory& mem) { return static_cast<bool>(Parser(mem).validate_FFHR_stream()); }

// ---------------------------------------------------------------------------

void tail_append_grows_by_the_minimum()
{
    TEST_GROUP("tail");
    Storage storage;
    Memory mem = build_stream(BuildSpec{}, storage);
    REQUIRE(stream_valid(mem), "fixture validates");

    // The fixture's own dead space (the checksum block, if the scan counts
    // it): the invariant is that appends add none, not that it is zero.
    const std::set<Offset> dead_before = dead_blocks(mem);

    std::vector<std::string> appended;
    const std::vector<std::vector<std::string>> rounds = {{"new-0"}, {"new-1", "new-2"}, {"new-3"}};
    for (const auto& round : rounds) {
        const Size before = sealed_size(mem);
        const Offset old_array = bundle_slot(mem, FF_BUNDLE::ENTRY);
        // Below the rollback point exactly one slot may change: Bundle.entry,
        // re-pointed at the new array. Blank it on both sides and compare the rest.
        const Offset entry_slot = root_offset(mem) + FF_BUNDLE::ENTRY;
        const auto without_entry_slot = [&](std::string bytes) {
            bytes.replace(entry_slot - FF_HEADER::HEADER_SIZE, sizeof(Offset), sizeof(Offset), '\0');
            return bytes;
        };
        const std::string untouched = without_entry_slot(bytes_between(mem, FF_HEADER::HEADER_SIZE, old_array));

        const AppendOutcome out = append_ids(mem, storage, round);
        REQUIRE(out.ok, "append succeeds");
        CHECK(!out.result.relocated, "tail array is rewritten, not relocated");
        CHECK(!out.result.copied_children, "bare entries need no copy");
        CHECK_EQ(out.result.rewrite_from, out.result.previous_array, "rollback starts at the old array");
        CHECK_EQ(out.result.previous_array, old_array, "previous_array names the old array");

        const Size growth = sealed_size(mem) - before;
        CHECK_EQ(growth, out.resources_bytes + round.size() * kEntry,
                 "stream grows by the resources plus one 84-byte entry each");
        const std::string after = without_entry_slot(bytes_between(mem, FF_HEADER::HEADER_SIZE, old_array));
        CHECK(after == untouched, "every byte below the rollback point, bar Bundle.entry, is unchanged; changed:"
                                      << differing_ranges(untouched, after, FF_HEADER::HEADER_SIZE)
                                      << " root " << root_offset(mem) << " old array " << old_array);

        appended.insert(appended.end(), round.begin(), round.end());
        CHECK(entry_ids(mem) == expected_ids(5, appended), "entries read back in order");
        CHECK(stream_valid(mem), "stream validates after the append");
        const std::set<Offset> dead_now = dead_blocks(mem);
        CHECK(dead_now == dead_before, "no new dead blocks (map: scan minus reachable): before"
                                           << describe_dead(mem, dead_before) << " / now"
                                           << describe_dead(mem, dead_now));
    }
}

void entry_children_survive_the_rewrite()
{
    TEST_GROUP("children");
    Storage storage;
    Memory mem = build_stream(BuildSpec{.entry_children = true}, storage);
    const std::set<Offset> dead_before = dead_blocks(mem);
    const Size before = sealed_size(mem);

    const AppendOutcome out = append_ids(mem, storage, {"new-0"});
    REQUIRE(out.ok, "append succeeds");
    CHECK(!out.result.relocated, "still a tail rewrite");
    CHECK(out.result.copied_children, "children were copied aside before the rollback");
    CHECK_EQ(sealed_size(mem) - before, out.resources_bytes + kEntry,
             "growth is the new resource plus one entry (the old children are rewritten, not duplicated)");
    CHECK(entry_ids(mem) == expected_ids(5, {"new-0"}), "entries read back in order");

    BundleData bundle = Parser(mem).root();
    REQUIRE(bundle.entry.size() == 6, "six entries");
    for (size_t i = 0; i < 5; ++i) {
        CHECK_EQ(std::string(bundle.entry[i].fullurl), "urn:uuid:entry-" + std::to_string(i),
                 "fullUrl " << i << " survives byte for byte");
        REQUIRE(bundle.entry[i].request != nullptr, "request " << i << " survives");
        CHECK_EQ(std::string(bundle.entry[i].request->url), "Observation/" + std::to_string(i),
                 "request.url " << i << " survives");
        CHECK(bundle.entry[i].request->method == FF_HTTPVerb::POST, "request.method " << i << " survives");
    }
    CHECK(bundle.entry[5].fullurl.empty() && !bundle.entry[5].request, "the new entry carries only its resource");
    CHECK(stream_valid(mem), "stream validates");
    CHECK(dead_blocks(mem) == dead_before, "no new dead blocks");
}

void backfilled_stream_relocates_once()
{
    TEST_GROUP("backfill");
    Storage storage;
    Memory mem = build_stream(BuildSpec{.layout = Layout::Backfill}, storage);
    const std::set<Offset> dead_before = dead_blocks(mem);

    // First append: the array is at the FRONT, resources follow it, so a
    // rollback would destroy them. Relocate; the old array becomes dead.
    const Offset front_array = bundle_slot(mem, FF_BUNDLE::ENTRY);
    AppendOutcome first = append_ids(mem, storage, {"new-0"});
    REQUIRE(first.ok, "first append succeeds");
    CHECK(first.result.relocated, "front array is relocated");
    CHECK_EQ(first.result.rewrite_from, FF_NULL_OFFSET, "nothing is rolled back");
    CHECK(first.result.entry_array > front_array, "the new array is past the old one");
    const std::set<Offset> dead_after_first = dead_blocks(mem);
    CHECK(dead_after_first.count(front_array) == 1, "the map sees the old array as dead");
    CHECK(dead_after_first.size() > dead_before.size(), "relocation leaves dead space, once");
    CHECK(entry_ids(mem) == expected_ids(5, {"new-0"}), "entries read back after relocation");

    // Second append: the relocated array is at the tail now.
    AppendOutcome second = append_ids(mem, storage, {"new-1"});
    REQUIRE(second.ok, "second append succeeds");
    CHECK(!second.result.relocated, "second append is a tail rewrite");
    CHECK_EQ(second.result.rewrite_from, first.result.entry_array, "it rewrites the relocated array");
    CHECK(dead_blocks(mem) == dead_after_first, "and adds no dead space");
    CHECK(entry_ids(mem) == expected_ids(5, {"new-0", "new-1"}), "entries read back in order");
    CHECK(stream_valid(mem), "stream validates");
}

void live_data_after_the_array_forces_relocation()
{
    TEST_GROUP("signature");
    Storage storage;
    Memory mem = build_stream(BuildSpec{.signature = true}, storage);
    const Offset signature = bundle_slot(mem, FF_BUNDLE::SIGNATURE);
    REQUIRE(signature != FF_NULL_OFFSET, "fixture has a signature");
    const Offset array = bundle_slot(mem, FF_BUNDLE::ENTRY);
    REQUIRE(signature > array, "the signature was STOREd after the entries");
    const std::string signature_bytes = bytes_between(mem, signature, sealed_size(mem) - FF_CHECKSUM::HEADER_SIZE);

    const AppendOutcome out = append_ids(mem, storage, {"new-0"});
    REQUIRE(out.ok, "append succeeds");
    CHECK(out.result.relocated, "a live block after the array forces relocation");
    CHECK_EQ(bundle_slot(mem, FF_BUNDLE::SIGNATURE), signature,
             "Bundle.signature still points where it did");
    CHECK(bytes_between(mem, signature, signature + signature_bytes.size()) == signature_bytes,
          "and its bytes are untouched");
    BundleData bundle = Parser(mem).root();
    CHECK(bundle.signature && bundle.signature->targetformat == "application/fhir+json",
          "the signature still reads back");
    CHECK(entry_ids(mem) == expected_ids(5, {"new-0"}), "entries read back in order");
    CHECK(stream_valid(mem), "stream validates");
}

void a_throwing_callback_leaves_the_bundle_intact()
{
    TEST_GROUP("failure");
    Storage storage;
    Memory mem = build_stream(BuildSpec{}, storage);
    const AppendOutcome out = append_ids(mem, storage, {"new-0", "new-1"}, /*throw_midway=*/true);
    CHECK(!out.ok, "the injected failure is reported, not thrown");
    CHECK(entry_ids(mem) == expected_ids(5), "the original entries are restored, in order");
    CHECK(stream_valid(mem), "stream validates after the failed append");
}

void refusals()
{
    TEST_GROUP("refusals");
    // Root is not a Bundle.
    {
        Storage storage;
        Memory mem = Memory::create(kArena);
        FF_Builder builder = open_builder(mem);
        auto root = builder->append_obj(make_observation(storage, "lonely"));
        FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root});
        FF_BundleAppendResult result;
        const FF_Result r = FF_BundleAppendEntries(
            FF_BundleAppendInfo{.builder = builder, .append = [](Builder&, std::vector<BundleentryData>&) {}},
            result);
        CHECK(r.code == FF_INVALID_ARGUMENT, "a non-Bundle root is refused");
    }
    // Null handle and null callback.
    {
        FF_BundleAppendResult result;
        CHECK(FF_BundleAppendEntries(FF_BundleAppendInfo{}, result).code == FF_INVALID_ARGUMENT,
              "a null builder is refused");
        Memory mem = Memory::create(kArena);
        CHECK(FF_BundleAppendEntries(FF_BundleAppendInfo{.builder = open_builder(mem)}, result).code ==
                  FF_INVALID_ARGUMENT,
              "a null callback is refused");
    }
    // serialize_bundle_array of nothing writes nothing.
    {
        Memory mem = Memory::create(kArena);
        FF_Builder builder = open_builder(mem);
        const Size before = mem.size();
        CHECK_EQ(serialize_bundle_array(*builder, {}), FF_NULL_OFFSET, "an empty array has no block");
        CHECK_EQ(mem.size(), before, "and claims no space");
    }
}

} // namespace

int main(int argc, char** argv)
{
    ff_test::set_filter(argc, argv);
    ff_test::run("tail", tail_append_grows_by_the_minimum);
    ff_test::run("children", entry_children_survive_the_rewrite);
    ff_test::run("backfill", backfilled_stream_relocates_once);
    ff_test::run("signature", live_data_after_the_array_forces_relocation);
    ff_test::run("failure", a_throwing_callback_leaves_the_bundle_intact);
    ff_test::run("refusals", refusals);
    return ff_test::report("APPEND-1: every append grew by the minimum and left the rest alone");
}
