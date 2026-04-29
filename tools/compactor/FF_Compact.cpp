/**
 * @file FF_Compact.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @version 0.1
 * @date 2026-04-29
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file in the project root for terms.
 *
 * @brief FastFHIR Compactor CLI — compact and re-seal a sealed .ffhr stream.
 *
 * Reads a sealed FastFHIR binary stream (.ffhr), runs Compactor::archive() to produce
 * a dense presence-bitmap compact archive, and writes the result to:
 *
 *   • stdout               — when reading from stdin, or when -o - is given
 *   • <stem>.compact.ffhr  — derived automatically from the input filename
 *   • <path>               — when -o <path> is given explicitly
 *
 * The compact archive is a new file; the input stream is never modified.
 * By default the compact archive is re-sealed with a SHA-256 checksum (requires OpenSSL at
 * build time). Pass --no-checksum to suppress the checksum seal.
 *
 * Usage:
 *   ff_compact [input.ffhr | -]  [-o output.compact.ffhr | -o -]  [--no-checksum]
 *   ff_compact -h | --help
 *
 * Pipeline example:
 *   cat large_bundle.ffhr | ff_compact | ff_export > bundle.json
 */

#include "FF_Parser.hpp"
#include "FF_Compactor.hpp"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <stdexcept>

// Cross-platform includes for memory mapping
#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
    #include <fcntl.h>
#else
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

#ifdef FASTFHIR_HAS_OPENSSL
    #include <openssl/evp.h>
#endif

using namespace FastFHIR;
namespace fs = std::filesystem;

// =====================================================================
// Cross-Platform Memory Mapper (RAII, read-only)
// =====================================================================
class MemoryMappedFile {
    const BYTE* m_data = nullptr;
    size_t      m_size = 0;

#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMap  = NULL;
#else
    int fd = -1;
#endif

public:
    explicit MemoryMappedFile(const std::string& filepath) {
#ifdef _WIN32
        hFile = CreateFileA(filepath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Failed to open file: " + filepath);

        LARGE_INTEGER size;
        if (!GetFileSizeEx(hFile, &size))
            throw std::runtime_error("Failed to get file size: " + filepath);
        m_size = static_cast<size_t>(size.QuadPart);

        hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hMap)
            throw std::runtime_error("Failed to create file mapping: " + filepath);

        m_data = static_cast<const BYTE*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
        if (!m_data)
            throw std::runtime_error("Failed to map view of file: " + filepath);
#else
        fd = open(filepath.c_str(), O_RDONLY);
        if (fd == -1)
            throw std::runtime_error("Failed to open file: " + filepath);

        struct stat sb;
        if (fstat(fd, &sb) == -1)
            throw std::runtime_error("Failed to get file size: " + filepath);
        m_size = static_cast<size_t>(sb.st_size);

        m_data = static_cast<const BYTE*>(mmap(nullptr, m_size, PROT_READ, MAP_PRIVATE, fd, 0));
        if (m_data == MAP_FAILED)
            throw std::runtime_error("Failed to mmap file: " + filepath);
#endif
    }

    ~MemoryMappedFile() {
#ifdef _WIN32
        if (m_data) UnmapViewOfFile(m_data);
        if (hMap)   CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
#else
        if (m_data && m_data != reinterpret_cast<const BYTE*>(MAP_FAILED))
            munmap(const_cast<BYTE*>(m_data), m_size);
        if (fd != -1) close(fd);
#endif
    }

    // Non-copyable
    MemoryMappedFile(const MemoryMappedFile&)            = delete;
    MemoryMappedFile& operator=(const MemoryMappedFile&) = delete;

    const BYTE* data() const { return m_data; }
    size_t      size() const { return m_size; }
};

// =====================================================================
// CLI Utility Functions
// =====================================================================
static void print_usage() {
    std::cerr << "=================================================\n"
              << " FastFHIR Compactor Engine\n"
              << "=================================================\n"
              << "Usage: ff_compact [input.ffhr | -]  [-o output | -o -]  [--no-checksum]\n\n"
              << "Arguments:\n"
              << "  input              Path to a sealed FastFHIR stream (.ffhr).\n"
              << "                     Use '-' or omit to read from stdin (binary mode).\n"
              << "  -o output          Write the compact archive to this path.\n"
              << "                     Omit to auto-derive <stem>.compact.ffhr from input.\n"
              << "                     Use '-o -' to force output to stdout.\n"
              << "  --no-checksum      Skip SHA-256 re-seal (checksum field left zero).\n"
              << "  -h, --help         Show this help message.\n\n"
#ifdef FASTFHIR_HAS_OPENSSL
              << "Checksum: SHA-256 enabled (OpenSSL present at build time).\n\n"
#else
              << "Checksum: disabled (build with OpenSSL to enable SHA-256 re-sealing).\n\n"
#endif
              << "Examples:\n"
              << "  ff_compact patient.ffhr\n"
              << "      -> writes patient.compact.ffhr\n\n"
              << "  ff_compact patient.ffhr -o archive.compact.ffhr\n"
              << "      -> writes to named output file\n\n"
              << "  cat bundle.ffhr | ff_compact | ff_export > bundle.json\n"
              << "      -> stdin -> compact -> JSON pipeline\n";
}

/// Derive output path: strip the last extension and append .compact.ffhr.
/// e.g. "patient.ffhr"      -> "patient.compact.ffhr"
///      "data/bundle"       -> "data/bundle.compact.ffhr"
static std::string derive_output_path(const std::string& input_path) {
    fs::path p(input_path);
    // stem() strips one extension level, preserving parent directory
    fs::path out = p.parent_path() / p.stem();
    out += ".compact.ffhr";
    return out.string();
}

// =====================================================================
// Main Execution
// =====================================================================
int main(int argc, char* argv[]) {
    std::string input_path;
    std::string output_path;
    bool to_stdout   = false;
    bool no_checksum = false;

    // 1. Parse Arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else if (arg == "--no-checksum") {
            no_checksum = true;
        } else if (arg == "-o" && i + 1 < argc) {
            output_path = argv[++i];
            if (output_path == "-") to_stdout = true;
        } else if (arg != "-o" && (arg == "-" || (arg[0] != '-' && input_path.empty()))) {
            input_path = arg;
        } else {
            std::cerr << "[ff_compact] Unknown or incomplete argument: " << arg << "\n";
            print_usage();
            return 1;
        }
    }

    // Reading from stdin implies stdout output unless -o overrides.
    const bool reading_stdin = (input_path.empty() || input_path == "-");
    if (reading_stdin && output_path.empty())
        to_stdout = true;

    try {
        // -----------------------------------------------------------------
        // 2. Resolve Input Strategy
        // -----------------------------------------------------------------
        const BYTE*                       parse_buffer = nullptr;
        size_t                            parse_size   = 0;
        std::unique_ptr<MemoryMappedFile> mapped_file;
        std::vector<BYTE>                 stdin_buffer;

        if (!reading_stdin) {
            fs::path p(input_path);
            if (!fs::exists(p)) {
                std::cerr << "[ff_compact] Error: file not found: " << input_path << "\n";
                return 1;
            }
            if (!fs::is_regular_file(p)) {
                std::cerr << "[ff_compact] Error: not a regular file: " << input_path << "\n";
                return 1;
            }
            mapped_file  = std::make_unique<MemoryMappedFile>(input_path);
            parse_buffer = mapped_file->data();
            parse_size   = mapped_file->size();
        } else {
            // Binary stdin
#ifdef _WIN32
            _setmode(_fileno(stdin), _O_BINARY);
#endif
            std::cerr << "[ff_compact] Reading from stdin...\n";
            std::istreambuf_iterator<char> beg(std::cin), end;
            stdin_buffer.assign(beg, end);
            if (stdin_buffer.empty()) {
                std::cerr << "[ff_compact] Error: no input data received from stdin.\n";
                return 1;
            }
            parse_buffer = stdin_buffer.data();
            parse_size   = stdin_buffer.size();
        }

        // -----------------------------------------------------------------
        // 3. Parse and validate the source stream
        // -----------------------------------------------------------------
        Parser source(parse_buffer, parse_size);

        if (source.stream_layout() == FF_STREAM_LAYOUT_COMPACT) {
            std::cerr << "[ff_compact] Error: input stream is already a compact archive.\n"
                      << "             Compact archives cannot be re-compacted.\n";
            return 1;
        }

        // -----------------------------------------------------------------
        // 4. Build optional SHA-256 hasher
        // -----------------------------------------------------------------
        FF_Checksum_Algorithm  algo = FF_CHECKSUM_NONE;
        Compactor::HashCallback hasher;

#ifdef FASTFHIR_HAS_OPENSSL
        if (!no_checksum) {
            algo = FF_CHECKSUM_SHA256;
            hasher = [](const unsigned char* data, Size size) -> std::vector<BYTE> {
                // Pre-allocate to the compile-time upper bound
                std::vector<BYTE> hash(FF_MAX_HASH_BYTES, 0);
                unsigned int out_len = 0;

                EVP_MD_CTX* ctx = EVP_MD_CTX_new();
                if (ctx != nullptr) {
                    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
                    EVP_DigestUpdate(ctx, data, size);
                    EVP_DigestFinal_ex(ctx, hash.data(), &out_len);
                    EVP_MD_CTX_free(ctx);
                } else {
                    std::cerr << "[ff_compact] Warning: failed to initialise OpenSSL context. "
                                 "Checksum will be empty.\n";
                }

                // Shrink to actual digest length
                hash.resize(out_len);
                return hash;
            };
        }
#else
        if (!no_checksum) {
            std::cerr << "[ff_compact] Note: built without OpenSSL — archive will not include "
                         "a SHA-256 checksum. Pass --no-checksum to suppress this notice.\n";
        }
#endif

        // -----------------------------------------------------------------
        // 5. Compact into a fresh arena
        //    A compact archive is always <= source size; use source size as
        //    the capacity upper-bound so the arena never needs to grow.
        // -----------------------------------------------------------------
        auto dest_mem = Memory::create(parse_size);
        Memory::View compact_view = Compactor::archive(source, dest_mem, algo, hasher);

        if (compact_view.empty()) {
            std::cerr << "[ff_compact] Error: compaction produced an empty result.\n";
            return 1;
        }

        // -----------------------------------------------------------------
        // 6. Write Output
        // -----------------------------------------------------------------
        const std::string_view compact_bytes = compact_view;

        if (to_stdout) {
#ifdef _WIN32
            _setmode(_fileno(stdout), _O_BINARY);
#endif
            std::cerr.flush();
            std::cout.write(compact_bytes.data(), static_cast<std::streamsize>(compact_bytes.size()));
            std::cout.flush();
        } else {
            if (output_path.empty())
                output_path = derive_output_path(input_path);

            std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                std::cerr << "[ff_compact] Error: cannot open output file for writing: "
                          << output_path << "\n";
                return 1;
            }
            out.write(compact_bytes.data(), static_cast<std::streamsize>(compact_bytes.size()));
            out.close();

            const size_t reduction = (parse_size > 0)
                ? (100u - (compact_bytes.size() * 100u / parse_size))
                : 0u;
            std::cerr << "[ff_compact] Compact archive written to " << output_path
                      << " (" << compact_bytes.size() << " bytes"
                      << ", source " << parse_size << " bytes"
                      << ", " << reduction << "% reduction)\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "[ff_compact] Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
