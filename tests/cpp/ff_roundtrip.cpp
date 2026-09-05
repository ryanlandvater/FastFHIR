/**
 * @file ff_roundtrip.cpp
 * @brief Standalone round-trip harness for DOM parity testing.
 *
 * Reads a FHIR JSON file, ingests it into a Memory arena, seals,
 * re-parses, and prints the resulting JSON to stdout.
 *
 * Usage:
 *   ff_roundtrip <fixture.json>              # prints re-serialised JSON
 *   ff_roundtrip <fixture.json> --arena-size 268435456   # 256 MB default
 *
 * Exit code: 0 on success, non-zero on error (diagnostics to stderr).
 */

#include <FastFHIR.hpp>
#include <FF_Ingestor.hpp>
#include "FF_AllTypes.hpp"

#include <openssl/evp.h>

#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <array>
#include <vector>

#include "FFHR_test_checksum.hpp"

using namespace FastFHIR;

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + path);
    auto size = f.tellg();
    f.seekg(0);
    std::string buf(static_cast<size_t>(size), '\0');
    f.read(buf.data(), size);
    return buf;
}


int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ff_roundtrip <fixture.json> [--arena-size N] [--debug]"
                     " [--debug-indent N] [--workers N] [--dump-sealed PATH]\n";
        return 1;
    }

    const std::string fixture_path = argv[1];
    size_t arena_size = 256 * 1024 * 1024; // 256 MB default
    bool debug_json = false;
    int debug_indent = 0;
    // 0 = the production default (hardware concurrency). This harness drives the
    // 342-fixture corpus gate, so it MUST take the concurrent ingest path by
    // default: AR-3 was a load-sensitive consumer latch that only ever
    // reproduced under CPU contention, and a serial-only gate cannot see that
    // class at all. It was pinned to 1 as an "A23 diagnostic" and never unpinned,
    // which silently made every corpus measurement a single-worker measurement.
    uint32_t workers = 0;
    // Byte-level inspection of the sealed stream, off unless asked for. This was
    // an unconditional write to a fixed /tmp path -- the corpus tools run many
    // harness processes at once, so they raced on one file, and Windows has no
    // /tmp at all.
    std::string dump_sealed;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--arena-size") == 0 && i + 1 < argc) {
            arena_size = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (std::strcmp(argv[i], "--debug") == 0) {
            debug_json = true;
        } else if (std::strcmp(argv[i], "--debug-indent") == 0 && i + 1 < argc) {
            debug_json = true;
            debug_indent = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (std::strcmp(argv[i], "--dump-sealed") == 0 && i + 1 < argc) {
            dump_sealed = argv[++i];
        }
    }
#ifdef NDEBUG
    if (debug_json) {
        std::cerr << "--debug requires a Debug build (to_debug_json is compiled out under NDEBUG)\n";
        return 2;
    }
#endif
    (void)debug_indent;

    try {
        // 1. Load fixture
        const std::string json_str = slurp(fixture_path);

        // 2. Allocate arena + stream
        auto mem = Memory::create(arena_size);
        FF_StreamCreateInfo stream_info;
        stream_info.arena = std::make_shared<Memory>(mem);
        stream_info.version = FHIR_VERSION_R5;
        FF_Stream stream;
        if (!FF_CreateStream(stream_info, stream)) {
            std::cerr << "create stream failed\n";
            return 1;
        }
        FF_IngestorCreateInfo ingestor_info;
        ingestor_info.concurrency = workers;   // 0 = hardware concurrency
        FF_Ingestor ingestor;
        if (!FF_CreateIngestor(ingestor_info, ingestor)) {
            std::cerr << "create ingestor failed\n";
            return 1;
        }

        // 3. Ingest
        Reflective::ObjectHandle root_handle;
        Size resource_count = 0;
        auto result = FF_Ingest(FF_IngestInfo{
            .ingestor = ingestor,
            .stream = stream,
            .source_type = FF_SOURCE_FHIR_JSON,
            .extension_filter = FF_ExtensionFilterMode::FILTER_NONE,
            .payload = json_str,
        }, root_handle, resource_count);
        if (result.failed()) {
            std::cerr << "Ingest failed: " << result.message << "\n";
            return 1;
        }
        // A successful ingest can still carry warnings (out-of-profile resource
        // types are retained as opaque JSON, so they round-trip but are not
        // typed-queryable). stdout is the round-trip document, so warnings go to
        // stderr — but they must go somewhere, or the limitation is silent.
        if (!result.message.empty()) {
            std::cerr << result.message << "\n";
        }

        // 4. Seal
        if (!FF_StreamSetRoot(FF_StreamSetRootInfo{
                .stream = stream,
                .root = root_handle,
            })) {
            std::cerr << "set_root failed\n";
            return 1;
        }
        Memory::View view;
        if (!FF_StreamFinalize(FF_StreamFinalizeInfo{
                .stream = stream,
                .algorithm = FF_CHECKSUM_SHA256,
                .hasher = ff_test::sha256,
            }, view)) {
            std::cerr << "finalize failed\n";
            return 1;
        }
        if (view.empty()) {
            std::cerr << "finalize returned empty view\n";
            return 1;
        }
        if (!dump_sealed.empty()) {
            std::ofstream out(dump_sealed, std::ios::binary);
            if (!out) {
                std::cerr << "cannot open --dump-sealed path: " << dump_sealed << "\n";
                return 1;
            }
            out.write(reinterpret_cast<const char*>(view.data()),
                      static_cast<std::streamsize>(view.size()));
        }

        // 5. Re-parse
        auto reparse_root = Parser(mem).root();
        if (!reparse_root) {
            std::cerr << "re-parsed root is null\n";
            return 1;
        }

        // 6. Serialise to JSON on stdout
#ifndef NDEBUG
        if (debug_json) {
            // Not FHIR and not round-trip input -- a lens on the bytes. See
            // Node::to_debug_json.
            reparse_root.to_debug_json(std::cout, debug_indent);
            std::cout << "\n";
            return 0;
        }
#endif
        reparse_root.print_json(std::cout);
        std::cout << "\n";

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
