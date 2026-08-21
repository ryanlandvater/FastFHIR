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

static std::vector<BYTE> sha256(const unsigned char *data, Size len)
{
    std::vector<BYTE> hash(EVP_MAX_MD_SIZE);
    unsigned int out_len = 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash.data(), &out_len);
    EVP_MD_CTX_free(ctx);

    hash.resize(out_len);
    return hash;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ff_roundtrip <fixture.json> [--arena-size N]\n";
        return 1;
    }

    const std::string fixture_path = argv[1];
    size_t arena_size = 256 * 1024 * 1024; // 256 MB default
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--arena-size") == 0 && i + 1 < argc) {
            arena_size = static_cast<size_t>(std::stoul(argv[++i]));
        }
    }

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
        // A23 diagnostic: force a single worker to test the concurrency hypothesis.
        FF_IngestorCreateInfo ingestor_info;
        ingestor_info.concurrency = 1;
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
            .payload = json_str,
        }, root_handle, resource_count);
        if (result.code != FF_SUCCESS) {
            std::cerr << "Ingest failed: " << result.message << "\n";
            return 1;
        }
        // A successful ingest can still have discarded entries (out-of-profile
        // resource types). stdout is the round-trip document, so this goes to
        // stderr — but it must go somewhere, or the loss is silent again.
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
                .hasher = sha256,
            }, view)) {
            std::cerr << "finalize failed\n";
            return 1;
        }
        if (view.empty()) {
            std::cerr << "finalize returned empty view\n";
            return 1;
        }
        // A23 diagnostic: dump the sealed stream for byte-level inspection.
        {
            std::ofstream out("/tmp/sealed.ffhr", std::ios::binary);
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
        reparse_root.print_json(std::cout);
        std::cout << "\n";

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
