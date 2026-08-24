/**
 * @file FF_Ingest.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @version 0.1
 * @date 2026-03-20
 * * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * * @brief FastFHIR Ingestor CLI implementation with auto-format detection
 */

#include <FastFHIR.hpp>
#include <FF_Ingestor.hpp>

#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <iterator>
#include <filesystem>
#include <algorithm>
#include <openssl/evp.h>

using namespace FastFHIR;
namespace fs = std::filesystem;

enum class ClinicalFormat
{
    UNKNOWN,
    FHIR_JSON,
    FHIR_XML,
    HL7_V2,
    HL7_V3
};

void print_usage()
{
    std::cerr << "=================================================\n";
    std::cerr << " FastFHIR Ingestor Engine\n";
    std::cerr << "=================================================\n";
    std::cerr << "Usage: ff_ingest [input | -] [-o output.ff]\n\n";
    std::cerr << "Arguments:\n";
    std::cerr << "  input          Path to the clinical data file. Use '-' or omit to read from stdin.\n";
    std::cerr << "  -o output      Path to save the FastFHIR binary file.\n";
    std::cerr << "                 (If omitted, binary is piped to standard output).\n";
    std::cerr << "  -h, --help     Show this help message and exit.\n\n";
    std::cerr << "Examples:\n";
    std::cerr << "  curl -s \"https://s3.aws.com/hospital/bundle.json.gz\" | gzip -d | ./ff_ingest | next_tool\n";
    std::cerr << "  cat patient.json | ./ff_ingest -o patient.ffhir\n";
}

// ---------------------------------------------------------
// Clinical Format Auto-Detection (The "Sniffer")
// ---------------------------------------------------------
ClinicalFormat detect_format(std::string_view payload)
{
    if (payload.empty())
        return ClinicalFormat::UNKNOWN;

    // Strip leading whitespace
    size_t start = payload.find_first_not_of(" \t\n\r\xEF\xBB\xBF");
    if (start == std::string_view::npos)
        return ClinicalFormat::UNKNOWN;

    std::string_view peek = payload.substr(start, 512); // Peek at first 512 bytes

    // 1. FHIR JSON
    if (peek.front() == '{')
        return ClinicalFormat::FHIR_JSON;

    // 2. HL7 v2
    if (peek.substr(0, 4) == "MSH|" || peek.substr(0, 4) == "FHS|" || peek.substr(0, 4) == "BHS|")
    {
        return ClinicalFormat::HL7_V2;
    }

    // 3. XML Formats
    if (peek.front() == '<')
    {
        if (peek.find("urn:hl7-org:v3") != std::string_view::npos)
            return ClinicalFormat::HL7_V3;
        if (peek.find("http://hl7.org/fhir") != std::string_view::npos)
            return ClinicalFormat::FHIR_XML;
    }

    return ClinicalFormat::UNKNOWN;
}

int main(int argc, char *argv[])
{
    std::string input_file = "";
    std::string output_file = "";

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc)
        {
            output_file = argv[++i];
        }
        else if (arg == "-h" || arg == "--help")
        {
            print_usage();
            return 0;
        }
        else if (input_file.empty())
        {
            input_file = arg;
        }
    }

    try
    {
        simdjson::padded_string json_buffer;

        // 1. Determine Source & Validate Path
        if (input_file.empty() || input_file == "-")
        {
            std::cerr << "[FastFHIR Injest CLI] Reading from stdin...\n";
            std::istreambuf_iterator<char> begin(std::cin), end;
            std::string temp_data(begin, end);
            json_buffer = simdjson::padded_string(temp_data);
        }
        else
        {
            // Standard Library Path Validation
            fs::path p(input_file);
            if (!fs::exists(p))
            {
                std::cerr << "[FastFHIR Injest CLI] Error: Path does not exist: " << input_file << "\n";
                return 1;
            }
            if (!fs::is_regular_file(p))
            {
                std::cerr << "[FastFHIR Injest CLI] Error: Not a regular file: " << input_file << "\n";
                return 1;
            }
            auto load_result = simdjson::padded_string::load(input_file);
            if (load_result.error())
            {
                std::cerr << "[FastFHIR Injest CLI] Error: Failed to load file: " << input_file << "\n";
                return 1;
            }
            json_buffer = std::move(load_result).value_unsafe();
        }

        // Execute the Sniffer
        std::string_view payload(json_buffer.data(), json_buffer.size());
        ClinicalFormat format = detect_format(payload);
        FF_SourceType source_type = FF_SOURCE_FHIR_JSON;

        switch (format)
        {
        case ClinicalFormat::FHIR_JSON:
            std::cerr << "[FastFHIR Injest CLI] Format Detected: FHIR JSON\n";
            source_type = FF_SOURCE_FHIR_JSON;
            break;
        case ClinicalFormat::HL7_V2:
            std::cerr << "[FastFHIR Injest CLI] Format Detected: HL7 v2\n";
            std::cerr << "[FastFHIR Injest CLI] Error: HL7 v2 ingestion is not yet fully implemented in FastFHIR.\n";
            source_type = FF_SOURCE_HL7_V2;
            return 1;
        case ClinicalFormat::HL7_V3:
            std::cerr << "[FastFHIR Injest CLI] Format Detected: HL7 v3 / CDA\n";
            std::cerr << "[FastFHIR Injest CLI] Error: HL7 v3 ingestion is not yet fully implemented in FastFHIR.\n";
            source_type = FF_SOURCE_HL7_V3;
            return 1;
        case ClinicalFormat::FHIR_XML:
            std::cerr << "[FastFHIR Injest CLI] Format Detected: FHIR XML\n";
            std::cerr << "[FastFHIR Injest CLI] Error: FastFHIR currently requires FHIR JSON. XML parser pending.\n";
            return 1;
        default:
            std::cerr << "[FastFHIR Injest CLI] Format Detected: Unknown\n";
            std::cerr << "[FastFHIR Injest CLI] Error: Unrecognized clinical data stream signature.\n";
            return 1;
        }

        // ---------------------------------------------------------
        // Core FastFHIR Ingestion Pipeline
        // ---------------------------------------------------------
        // HEURISTIC: Clinical JSON is heavy on syntax (quotes, braces, keys) and
        // FastFHIR binary is dense, so 2x the input is ample for any real document.
        // It is not ample at the bottom: FF_HEADER is 54 bytes and a resource vtable
        // is up to ~250 (FF_PATIENT is 191), and neither scales with input size. A
        // 66-byte Patient asked for 132 bytes and needed 245. The floor covers the
        // fixed overhead; because the arena is reserved virtual memory, not committed
        // pages, over-reserving here costs nothing.
        // FastFHIR::Ingest::FF_MIN_ARENA — one definition, shared with the
        // regression test that pins this floor (tests/cpp/test_bundle_ingest.cpp).
        size_t capacity_hint =
            std::max(json_buffer.size() * 2, FastFHIR::Ingest::FF_MIN_ARENA);

        FF_StreamCreateInfo stream_info;
        stream_info.capacity = capacity_hint;
        FF_Stream stream;
        FF_Result result = FF_CreateStream(stream_info, stream);
        if (!result)
        {
            std::cerr << "[FastFHIR Injest CLI] Fatal Arena/Stream Error: " << result.message << "\n";
            return 1;
        }

        FF_IngestorCreateInfo ingestor_info;
        FF_Ingestor ingestor;
        result = FF_CreateIngestor(ingestor_info, ingestor);
        if (!result)
        {
            std::cerr << "[FastFHIR Injest CLI] Fatal Ingestor Error: " << result.message << "\n";
            return 1;
        }

        // json_buffer is a simdjson::padded_string, so the SIMDJSON_PADDING slack the
        // parser needs is already allocated. Declaring the capacity lets the ingestor
        // parse this buffer in place instead of memcpy'ing the whole document.
        FF_IngestInfo ingest_info{
            .ingestor = ingestor,
            .stream = stream,
            .source_type = source_type,
            .payload = payload,
            .payload_capacity = json_buffer.size() + simdjson::SIMDJSON_PADDING};

        Reflective::ObjectHandle root_handle;
        Size parsed_count = 0;

        result = FF_Ingest(ingest_info, root_handle, parsed_count);

        if (result.failed())
        {
            std::cerr << "[FastFHIR Injest CLI] Fatal Ingestion Error: " << result.message << "\n";
            return 1;
        }
        // Warnings (e.g. out-of-profile resources discarded) keep the run alive.
        if (result.is_warning())
            std::cerr << "[FastFHIR Injest CLI] Warning: " << result.message << "\n";

        std::cerr << "[FastFHIR Injest CLI] Successfully parsed " << parsed_count << " resources.\n";

        result = FF_StreamSetRoot(FF_StreamSetRootInfo{
            .stream = stream,
            .root = root_handle,
        });
        if (!result)
        {
            std::cerr << "[FastFHIR Injest CLI] Fatal Root Assignment Error: " << result.message << "\n";
            return 1;
        }

        Memory::View view;
        result = FF_StreamFinalize(FF_StreamFinalizeInfo{
                                       .stream = stream,
                                       .algorithm = FF_CHECKSUM_SHA256,
                                       .hasher = [](const unsigned char *data, Size size) {
                                           // Pre-allocate strictly to the compile-time upper bound
                                           std::vector<BYTE> hash(FF_MAX_HASH_BYTES, 0);
                                           unsigned int out_len = 0;

                                           EVP_MD_CTX *ctx = EVP_MD_CTX_new();
                                           if (ctx != nullptr) {
                                               EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
                                               EVP_DigestUpdate(ctx, data, size);
                                               EVP_DigestFinal_ex(ctx, hash.data(), &out_len);
                                               EVP_MD_CTX_free(ctx);
                                           } else {
                                               std::cerr << "[FastFHIR Injest CLI] Warning: Failed to initialize OpenSSL context. Hash will be empty.\n";
                                           }

                                           // Shrink if a smaller algorithm was somehow used, otherwise a no-op
                                           hash.resize(out_len);
                                           return hash;
                                       },
                                   },
                                   view);
        if (!result)
        {
            std::cerr << "[FastFHIR Injest CLI] Fatal Finalize Error: " << result.message << "\n";
            return 1;
        }

        if (output_file.empty())
        {
            std::cerr.flush();
            std::cout.write(view.data(), static_cast<std::streamsize>(view.size()));
            std::cout.flush();
        }
        else
        {
            std::ofstream outfile(output_file, std::ios::binary);
            if (!outfile.is_open())
            {
                std::cerr << "[FastFHIR Injest CLI] Error: Could not open output file " << output_file << " for writing.\n";
                return 1;
            }
            outfile.write(view.data(), static_cast<std::streamsize>(view.size()));
            outfile.close();
            std::cerr << "[FastFHIR Injest CLI] Binary saved to " << output_file << " (" << view.size() << " bytes)\n";
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "[FastFHIR Injest CLI] Unhandled Engine Exception: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
