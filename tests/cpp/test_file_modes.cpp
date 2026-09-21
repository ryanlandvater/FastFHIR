/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file test_file_modes.cpp
 * @brief What opening an arena may do to what is already there.
 *
 * Every case goes through the real pipeline -- a Builder writes the file,
 * FF_Parse or a second Builder opens it -- and then compares the bytes on disk.
 * A hand-built buffer would only prove the reader agrees with this file's idea
 * of the format (CLAUDE.md, "A green suite here has meant less than it looks").
 *
 *   read          openReadOnly maps the file as-is and changes nothing, ever
 *   missing       a reader given a wrong path creates nothing
 *   fresh         starting over is the caller's own remove(); the library has
 *                 no mode that discards a sealed stream
 *   amend         createFromFile appends to a finalized stream
 *   damaged       AMEND refuses a file that is not a finalized stream, and
 *                 leaves it byte-identical for Recovery
 *   url_directory re-opening restores the URL directory, so Extension.url
 *                 survives an amend-and-reseal
 *   attach        a second party mapping a LIVE shared segment disturbs
 *                 nothing: the producer's write head is not touched
 */

#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>
#include "FF_AllTypes.hpp"

#include <filesystem>
#include <fstream>
#if defined(_WIN32) || defined(_WIN64)
#include <process.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "FFHR_tests.hpp"
#include "FFHR_test_checksum.hpp"

namespace fs = std::filesystem;
using namespace FastFHIR;

namespace
{

/// A fresh path under the test artifact dir. A std::string, not an fs::path,
/// because the FF_* Info structs take `const char*` and fs::path::c_str() is
/// wchar_t on Windows.
std::string scratch(const char *name)
{
    const fs::path path = fs::path(FF_TEST_ARTIFACT_DIR) / "file_modes" / name;
    fs::create_directories(path.parent_path());
    std::error_code ignored;
    fs::remove(path, ignored);
    return path.string();
}

std::vector<char> file_bytes(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_bytes(const fs::path &path, const std::vector<char> &bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// Seal a one-Patient stream at @p path through the public API. A Builder
/// always amends: an existing finalized stream is appended to, a missing file
/// is created.
void seal_patient(const std::string &path, std::string_view id)
{
    FF_BuilderCreateInfo info;
    info.filepath = path.c_str();
    info.capacity = 1 << 20;
    FF_Builder builder;
    REQUIRE(FF_CreateBuilder(info, builder).succeeded(), "create builder at " << path);

    PatientData patient;
    patient.id = id;
    const auto root = builder->append_obj(patient);
    REQUIRE(FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root}).succeeded(),
            "set root");
    Memory::View sealed;
    REQUIRE(FF_BuilderFinalize(FF_BuilderFinalizeInfo{
                .builder = builder, .algorithm = FF_CHECKSUM_SHA256, .hasher = &ff_test::sha256},
            sealed).succeeded(),
            "finalize");
}

/// Parse a sealed file the way a receiver does: a read-only arena, handed to
/// the Parser. Nothing here may create, grow or write the file.
FF_Result parse_file(const std::string &path, Parser &out)
{
    FF_Memory memory;
    try
    {
        memory = Memory::openReadOnly(path);
    }
    catch (const std::exception &e)
    {
        return FF_Result{FF_FAILURE, e.what()};
    }
    return FF_Parse(FF_ParseInfo{.memory = memory}, out);
}

std::string root_id(const Parser &parser)
{
    return std::string(static_cast<std::string_view>(parser.root()[Fields::PATIENT::ID]));
}

template <typename Fn>
bool throws_mentioning(Fn fn, std::string_view needle)
{
    try
    {
        fn();
    }
    catch (const std::exception &e)
    {
        return std::string_view(e.what()).find(needle) != std::string_view::npos;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────

void read_changes_nothing()
{
    TEST_GROUP("read");
    const std::string path = scratch("read.ffhr");
    seal_patient(path, "p-read");
    const std::vector<char> before = file_bytes(path);
    REQUIRE(before.size() > FF_HEADER::HEADER_SIZE, "sealed stream on disk");

    {
        Parser parser;
        CHECK(parse_file(path, parser).succeeded(), "a read-only arena parses");
        CHECK(root_id(parser) == "p-read", "root reads back through the read-only mapping");
        CHECK(parser.validate_FFHR_stream().succeeded(), "the mapped stream validates");
    }
    {
        Memory memory = Memory::openReadOnly(path);
        CHECK(memory->read_only(), "openReadOnly arena reports read_only()");
        CHECK(memory->capacity() == before.size(), "it maps exactly the file: " << memory->capacity());
        CHECK(throws_mentioning([&] { memory->claim_space(8); }, "read-only"), "claim_space throws, named");
        CHECK(throws_mentioning([&] { memory->reset(0); }, "read-only"), "reset throws, named");
        CHECK(throws_mentioning([&] { memory->truncate_file(0); }, "read-only"), "truncate_file throws, named");
        CHECK(throws_mentioning([&] { Builder_t builder(memory, FHIR_VERSION_R5); }, "read-only"),
              "a Builder refuses a read-only arena");
    }

    CHECK(file_bytes(path) == before, "the file is byte-identical after every read-side operation");
}

void missing_path_creates_nothing()
{
    TEST_GROUP("missing");
    const std::string missing = scratch("never-written.ffhr");

    Parser parser;
    CHECK(!parse_file(missing, parser).succeeded(),
          "FF_Parse on a missing path fails");
    CHECK(!fs::exists(missing), "and creates no file");

    const std::string stub = scratch("stub.ffhr");
    write_bytes(stub, std::vector<char>(10, 'x'));
    // Memory maps bytes and does not judge them: a 10-byte file mounts, and the
    // Parser is what rejects it. Either way nothing grows the file.
    Parser stub_parser;
    CHECK(!parse_file(stub, stub_parser).succeeded(), "a 10-byte file is not a stream");
    CHECK(fs::file_size(stub) == 10, "and reading it does not grow it");
}

void starting_over_is_the_callers_own_remove()
{
    TEST_GROUP("fresh");
    const std::string path = scratch("fresh.ffhr");
    seal_patient(path, "p-aaaa");
    const auto first_size = fs::file_size(path);

    // There is no library mode that discards a sealed stream. A caller who
    // wants a new one removes the file, in their own code.
    std::error_code ec;
    fs::remove(path, ec);
    seal_patient(path, "p-bbbb");

    Parser parser;
    REQUIRE(parse_file(path, parser).succeeded(), "parse the new stream");
    CHECK(root_id(parser) == "p-bbbb", "the new stream holds only the new root");
    CHECK(fs::file_size(path) == first_size,
          "same-sized content (the ids are the same length), same-sized file: nothing of the first stream remains ("
              << fs::file_size(path) << " vs " << first_size << ")");
}

void amend_appends_by_default()
{
    TEST_GROUP("amend");
    const std::string path = scratch("amend.ffhr");
    seal_patient(path, "p-original");
    const auto sealed_size = fs::file_size(path);

    // The default mode, spelled out nowhere: a Builder over an existing path amends.
    seal_patient(path, "p-appended");
    const auto amended_size = fs::file_size(path);

    Parser parser;
    REQUIRE(parse_file(path, parser).succeeded(), "parse amended stream");
    CHECK(root_id(parser) == "p-appended", "the new root is the one set on the amend");
    CHECK(parser.validate_FFHR_stream().succeeded(), "the amended stream validates");
    CHECK(amended_size > sealed_size, "the file grew by the appended block: " << sealed_size << " -> " << amended_size);
    CHECK(amended_size < 2 * sealed_size, "the old checksum block was reclaimed, not left behind");

    // A header region that was never written holds no data: AMEND initializes it.
    const std::string zeroed = scratch("zeroed.ffhr");
    write_bytes(zeroed, std::vector<char>(4096, '\0'));
    seal_patient(zeroed, "p-zeroed");
    REQUIRE(parse_file(zeroed, parser).succeeded(), "parse zero-filled start");
    CHECK(root_id(parser) == "p-zeroed", "an all-zero file is initialized as a new stream");
}

void damaged_stream_is_refused_untouched()
{
    TEST_GROUP("damaged");
    const std::string path = scratch("damaged.ffhr");
    seal_patient(path, "p-damaged");
    std::vector<char> bytes = file_bytes(path);
    bytes[0] = 'X';  // the FFHR magic
    write_bytes(path, bytes);

    FF_Builder builder;
    const FF_Result amend = FF_CreateBuilder(FF_BuilderCreateInfo{.capacity = 1 << 20, .filepath = path.c_str()},
                                             builder);
    CHECK(!amend.succeeded(), "a Builder refuses a stream whose header does not validate");
    CHECK(amend.message.find("Recovery") != std::string::npos, "and names Recovery: " << amend.message);
    CHECK(file_bytes(path) == bytes, "the damaged file is byte-identical: its header was not zeroed");

    Parser parser;
    CHECK(!parse_file(path, parser).succeeded(), "FF_Parse reports the damage");
    CHECK(file_bytes(path) == bytes, "and still leaves the file byte-identical");

    const std::string foreign = scratch("foreign.bin");
    const std::vector<char> text(256, 'q');
    write_bytes(foreign, text);
    CHECK(!FF_CreateBuilder(FF_BuilderCreateInfo{.capacity = 1 << 20, .filepath = foreign.c_str()}, builder)
               .succeeded(),
          "a Builder refuses a file that was never a FastFHIR stream");
    CHECK(file_bytes(foreign) == text, "and leaves it untouched");
}

void url_directory_survives_reopen()
{
    TEST_GROUP("url_directory");
    const std::string path = scratch("url_directory.ffhr");
    const std::string_view url = "http://example.org/fhir/StructureDefinition/cap-participant";

    {
        FF_Ingestor ingestor;
        REQUIRE(FF_CreateIngestor(FF_IngestorCreateInfo{}, ingestor).succeeded(), "create ingestor");
        FF_BuilderCreateInfo info;
        info.filepath = path.c_str();  // scratch() removed any previous run
        info.capacity = 1 << 20;
        FF_Builder builder;
        REQUIRE(FF_CreateBuilder(info, builder).succeeded(), "create builder");
        Reflective::ObjectHandle root;
        Size parsed = 0;
        REQUIRE(FF_Ingest(FF_IngestInfo{
                    .ingestor = ingestor,
                    .builder  = builder,
                    .payload  = R"({"resourceType":"Patient","id":"p-url","extension":[{"url":"http://example.org/fhir/StructureDefinition/cap-participant","valueString":"CAP-12345"}]})",
                }, root, parsed).succeeded(),
                "ingest a Patient carrying one extension");
        REQUIRE(FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = builder, .root = root}).succeeded(), "set root");
        Memory::View sealed;
        REQUIRE(FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, sealed).succeeded(), "finalize");
    }

    auto exported = [&]() -> std::string {
        Parser parser;
        const bool parsed = parse_file(path, parser).succeeded();
        CHECK(parsed, "parse " << path);
        if (!parsed) return {};
        std::ostringstream json;
        parser.root().print_json(json);
        return json.str();
    };
    const std::string before = exported();
    REQUIRE(before.find(url) != std::string::npos, "the extension URL exports before re-open: " << before);

    {
        FF_Builder builder;
        REQUIRE(FF_CreateBuilder(FF_BuilderCreateInfo{.capacity = 1 << 20, .filepath = path.c_str()}, builder)
                    .succeeded(),
                "re-open with the default mode");
        CHECK(builder->url_dir_offset() != FF_NULL_OFFSET, "the Builder restored the URL directory offset");
        builder->root_handle()[Fields::PATIENT::ACTIVE] = true;
        Memory::View sealed;
        REQUIRE(FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, sealed).succeeded(), "re-finalize");
    }

    const std::string after = exported();
    CHECK(after.find(url) != std::string::npos, "the extension URL survives amend-and-reseal: " << after);
    CHECK(after.find("\"url\":null") == std::string::npos, "no extension URL exports as null");
    CHECK(after.find("\"active\":true") != std::string::npos, "and the amendment itself landed");
}

void attaching_to_a_live_shared_arena_disturbs_nothing()
{
    TEST_GROUP("attach");
#if defined(_WIN32) || defined(_WIN64)
    const int pid = ::_getpid();
#else
    const int pid = ::getpid();
#endif
    const std::string name = "ff_test_attach_" + std::to_string(pid);

    // A producer mid-build: an arena with data and NO finalized header, which is
    // the normal state of a shared segment another process is writing into.
    Memory producer = Memory::create(4 * 1024 * 1024, name);
    FF_Builder first_writer;
    REQUIRE(FF_CreateBuilder(FF_BuilderCreateInfo{.arena = producer},
                             first_writer).succeeded(),
            "a Builder over a fresh shared arena");
    PatientData first;
    first.id = "shm-first";
    const auto first_handle = first_writer->append_obj(first);
    const uint64_t head = producer->size();
    REQUIRE(head > FF_HEADER::HEADER_SIZE, "the producer claimed space: " << head);

    // A sibling attaches by name -- the same call another process makes.
    Memory attached = Memory::create(4 * 1024 * 1024, name);
    CHECK(attached->size() == head, "the attached arena sees the producer's head: " << attached->size());
    CHECK(producer->size() == head, "and attaching did not move it: " << producer->size());

    // It can append to the live arena, after the producer's data, not over it.
    FF_Builder second_writer;
    REQUIRE(FF_CreateBuilder(FF_BuilderCreateInfo{.arena = attached},
                             second_writer).succeeded(),
            "a second Builder attaches to the in-progress arena");
    PatientData second;
    second.id = "shm-second";
    const auto second_handle = second_writer->append_obj(second);
    CHECK(second_handle.offset() >= head, "the attached writer appends past the producer's data");
    CHECK(producer->size() > head, "and the shared write head advanced for both");

    // Sealing from either handle leaves both resources in one readable stream.
    REQUIRE(FF_BuilderSetRoot(FF_BuilderSetRootInfo{.builder = first_writer, .root = first_handle})
                .succeeded(),
            "set root");
    Memory::View sealed;
    REQUIRE(FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = first_writer}, sealed).succeeded(),
            "finalize the shared arena");

    Parser parser;
    REQUIRE(FF_Parse(FF_ParseInfo{.memory = attached}, parser).succeeded(),
            "the attached arena parses once sealed");
    CHECK(root_id(parser) == "shm-first", "the producer's resource survived the attach");
    CHECK(parser.validate_FFHR_stream().succeeded(), "and the stream validates");
    (void)second_handle;

#if !defined(_WIN32) && !defined(_WIN64)
    ::shm_unlink(("/" + name).c_str());
#endif
}

} // namespace

int main(int argc, char **argv)
{
    ff_test::set_filter(argc, argv);
    ff_test::run("read", read_changes_nothing);
    ff_test::run("missing", missing_path_creates_nothing);
    ff_test::run("fresh", starting_over_is_the_callers_own_remove);
    ff_test::run("amend", amend_appends_by_default);
    ff_test::run("damaged", damaged_stream_is_refused_untouched);
    ff_test::run("url_directory", url_directory_survives_reopen);
    ff_test::run("attach", attaching_to_a_live_shared_arena_disturbs_nothing);
    return ff_test::report("arena access: every mount did exactly what it promises to what was already there");
}
