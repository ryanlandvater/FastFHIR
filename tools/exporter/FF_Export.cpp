/**
 * @file FF_Export.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @version 0.1
 * @date 2026-03-18
 * 
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * 
 * @brief FastFHIR Exporter — Convert FastFHIR binary stream to minified JSON.
 * 
 * This tool reads a FastFHIR binary file, parses it using the FastFHIR parser, 
 * and outputs the contained FHIR resource(s) as minified JSON. It supports both file input and output, 
 * as well as streaming via standard input and output for flexible integration into pipelines.
 */

#include "FastFHIR.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include <stdexcept>

using namespace FastFHIR;

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
        
        std::vector<BYTE> stdin_buffer;

        // 2. Resolve Input Strategy
        //    A path is mapped by the library itself, read-only: exactly the
        //    bytes on disk, never written. This tool used to carry its own
        //    mmap wrapper, one of two copies of the same 50 lines.
        if (input_file.empty()) {
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
        FastFHIR::Parser parser;
        FF_Result parse_result = input_file.empty()
            ? FastFHIR::FF_Parse(FastFHIR::FF_ParseInfo{
                  .buffer = parse_buffer,
                  .size = parse_size,
              }, parser)
            : FastFHIR::FF_Parse(FastFHIR::FF_ParseInfo{
                  .memory = FastFHIR::Memory::openReadOnly(input_file),
              }, parser);
        if (!parse_result)
        {
            std::cerr << "FastFHIR Export Error: " << parse_result.message << "\n";
            return 1;
        }

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
