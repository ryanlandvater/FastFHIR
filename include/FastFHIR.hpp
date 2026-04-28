/**
 * @file FastFHIR.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file in the project root for terms.
 * @version 0.1
 *
 * @brief FastFHIR — Public API
 *
 * This is the only header consumers of the FastFHIR library need to include.
 *
 * 
 * 
 * =====================================================================
 * Quick Start Example 1: Standard Build and Parse
 * =====================================================================
 * @code
 * #include <FastFHIR.hpp>
 * #include <openssl/sha.h>
 *
 * // 1. Build a stream
 * FastFHIR::Builder builder;
 * ObservationData obs;
 * * auto root = builder.append_obj(obs); 
 * root["status"] = "final";
 * builder.set_root(root); 
 *
 * // Seal the file with a lambda crypto callback
 * auto payload = builder.finalize(FF_CHECKSUM_SHA256, []
 *  (const unsigned char* byte_start, Size bytes_to_hash) -> std::vector<BYTE> {
 *      std::vector<BYTE> hash(SHA256_DIGEST_LENGTH);
 *      SHA256(byte_start, bytes_to_hash, hash.data());
 *      return hash;
 * });
 *
 * // 2. Parse the zero-copy stream
 * auto parser = FastFHIR::Parser::create(payload.data(), payload.size());
 * 
 * // 3. Access the root resource and its fields with zero-copy accessors
 * // Access by generated field key for better performance and safety:
 * auto status = parser.root()[FastFHIR::FieldKeys::Observation::STATUS].value().as_string();
 * // Or access by string key:
 * auto status = parser.root()["status"].value().as_string();
 * 
 * @endcode
 *
 * =====================================================================
 * Quick Start Example 2: Lock-Free Concurrent Generation
 * =====================================================================
 * @code
 * #include <FastFHIR.hpp>
 * #include <thread>
 * #include <vector>
 *
 * FastFHIR::Builder builder(2ULL * 1024 * 1024 * 1024); // 2GB Virtual Arena
 * std::vector<std::thread> pool;
 *
 * // 32 threads simultaneously serializing AI inferences into the same stream
 * for (int i = 0; i < 32; ++i) {
 *      pool.emplace_back([&builder, i]() {
 *      // 1. Thread-local work (AI inference, data fetching, etc.)
 *      
 *      ObservationData local_obs;
 *      local_obs.status = "preliminary";
 *      // 2. Lock-free 1-clock-cycle atomic claim and concurrent write
 *      // No mutexes. No heap allocations. No pointer invalidation.
 *      auto handle = builder.append_obj(local_obs);
 *      // 3. (Optional) push handle.offset() to a lock-free queue to link to a Bundle later
 * });
 * }
 *
 * for (auto& thread : pool) thread.join();
 * @endcode
 */

#pragma once

// =====================================================================
// Version — injected at compile time by CMake, which reads
// FASTFHIR_VERSION_MAJOR / _MINOR / _BUILD environment variables
// (set by CI from the GitHub release tag, e.g. v1.2.3 → 1, 2, 3).
// The #ifndef guards keep local / offline builds working without CI.
// =====================================================================
#ifndef FASTFHIR_VERSION_MAJOR
#  define FASTFHIR_VERSION_MAJOR 2026
#endif
#ifndef FASTFHIR_VERSION_MINOR
#  define FASTFHIR_VERSION_MINOR 1
#endif
#ifndef FASTFHIR_VERSION_BUILD
#  define FASTFHIR_VERSION_BUILD 0
#endif

#define FASTFHIR_STRINGIFY_IMPL(x) #x
#define FASTFHIR_STRINGIFY(x)      FASTFHIR_STRINGIFY_IMPL(x)
#define FASTFHIR_VERSION_STRING    FASTFHIR_STRINGIFY(FASTFHIR_VERSION_MAJOR) "." \
                                   FASTFHIR_STRINGIFY(FASTFHIR_VERSION_MINOR) "." \
                                   FASTFHIR_STRINGIFY(FASTFHIR_VERSION_BUILD)

// NOTE:
// Generic FF_* key registry is intentionally not included by default to avoid
// accidental use in C++ mutation paths. Include <FF_FieldKeys.hpp> explicitly
// ONLY when you need generic reflective keys. It's confusing and not recommended. 

// Include the public headers
#include "FF_Parser.hpp"
#include "FF_Builder.hpp"
#include "FF_Compactor.hpp"