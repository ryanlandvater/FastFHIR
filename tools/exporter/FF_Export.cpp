/**
 * @file FF_Export.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @version 0.1
 * @date 2026-03-18
 * 
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file in the project root for terms.
 * 
 * @brief FastFHIR Exporter — Convert FastFHIR binary stream to minified JSON.
 * 
 * This tool reads a FastFHIR binary file, parses it using the FastFHIR parser, 
 * and outputs the contained FHIR resource(s) as minified JSON. It supports both file input and output, 
 * as well as streaming via standard input and output for flexible integration into pipelines.
 */

#include "FF_Parser.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include <stdexcept>

// Cross-platform includes for Memory Mapping
#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

using namespace FastFHIR;

// =====================================================================
// Cross-Platform Memory Mapper (RAII)
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
    MemoryMappedFile(const std::string& filepath) {
#ifdef _WIN32
        hFile = CreateFileA(filepath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) throw std::runtime_error("Failed to open file.");
        
        LARGE_INTEGER size;
        if (!GetFileSizeEx(hFile, &size)) throw std::runtime_error("Failed to get file size.");
        m_size = static_cast<size_t>(size.QuadPart);

        hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hMap) throw std::runtime_error("Failed to create file mapping.");

        m_data = static_cast<const BYTE*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
        if (!m_data) throw std::runtime_error("Failed to map view of file.");
#else
        fd = open(filepath.c_str(), O_RDONLY);
        if (fd == -1) throw std::runtime_error("Failed to open file.");

        struct stat sb;
        if (fstat(fd, &sb) == -1) throw std::runtime_error("Failed to get file size.");
        m_size = static_cast<size_t>(sb.st_size);

        m_data = static_cast<const BYTE*>(mmap(nullptr, m_size, PROT_READ, MAP_PRIVATE, fd, 0));
        if (m_data == MAP_FAILED) throw std::runtime_error("Failed to mmap file.");
#endif
    }

    ~MemoryMappedFile() {
#ifdef _WIN32
        if (m_data) UnmapViewOfFile(m_data);
        if (hMap) CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
#else
        if (m_data && m_data != MAP_FAILED) munmap(const_cast<BYTE*>(m_data), m_size);
        if (fd != -1) close(fd);
#endif
    }

    const BYTE* data() const { return m_data; }
    size_t size() const { return m_size; }
};

// =====================================================================
// CLI Utility Functions
// =====================================================================
void print_help() {
    std::cerr << "Usage: ff_export [OPTIONS]\n"
              << "Converts a FastFHIR binary stream to minified JSON.\n\n"
              << "Options:\n"
              << "  -i <file>   Input FastFHIR file (default: read from stdin)\n"
              << "  -o <file>   Output JSON file (default: write to stdout)\n"
              << "  -h          Show this help message\n";
}

std::vector<BYTE> read_stream_to_buffer(std::istream& in) {
    // Read the entire stream into a dynamic vector.
    // Note: This consumes RAM equivalent to the stream size.
    return std::vector<BYTE>(std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>());
}

// =====================================================================
// Main Execution
// =====================================================================
int main(int argc, char** argv) {
    std::string input_file;
    std::string output_file;

    // 1. Parse Arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        } else if (arg == "-i" && i + 1 < argc) {
            input_file = argv[++i];
        } else if (arg == "-o" && i + 1 < argc) {
            output_file = argv[++i];
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            print_help();
            return 1;
        }
    }

    try {
        const BYTE* parse_buffer = nullptr;
        size_t parse_size = 0;
        
        std::unique_ptr<MemoryMappedFile> mapped_file;
        std::vector<BYTE> stdin_buffer;

        // 2. Resolve Input Strategy
        if (!input_file.empty()) {
            mapped_file = std::make_unique<MemoryMappedFile>(input_file);
            parse_buffer = mapped_file->data();
            parse_size = mapped_file->size();
        } else {
            // No input file provided; read from standard input
            std::ios_base::sync_with_stdio(false); // Speed up stdin
            std::cin.tie(NULL);
            stdin_buffer = read_stream_to_buffer(std::cin);
            
            if (stdin_buffer.empty()) {
                std::cerr << "Error: No input data received from stdin.\n";
                return 1;
            }
            parse_buffer = stdin_buffer.data();
            parse_size = stdin_buffer.size();
        }

        // 3. Mount the Parser
        auto parser = FastFHIR::Parser::create(parse_buffer, parse_size);

        // 4. Resolve Output Strategy
        if (!output_file.empty()) {
            std::ofstream out_stream(output_file, std::ios::binary);
            if (!out_stream) throw std::runtime_error("Failed to open output file for writing.");
            parser.print_json(out_stream);
            out_stream << "\n";
        } else {
            // Write to stdout
            parser.print_json(std::cout);
            std::cout << "\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "FastFHIR Export Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}