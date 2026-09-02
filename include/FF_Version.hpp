/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#pragma once

// =====================================================================
// FastFHIR version macros
//
// CMake/CI can inject FASTFHIR_VERSION_* at compile time from release tags.
// These fallbacks keep local and offline builds functional.
// =====================================================================
#ifndef FASTFHIR_VERSION_MAJOR
#  define FASTFHIR_VERSION_MAJOR 2026
#endif
#ifndef FASTFHIR_VERSION_MINOR
// MUST match CMakeLists.txt's default (CMake is primary; CLAUDE.md:170). It
// said 1 while CMake said 0, and BUILD.bazel defined neither -- so the two
// build systems compiled DIFFERENT ENGINE VERSIONS from one source. That is
// not cosmetic: the engine version is written into every stream's FF_HEADER,
// and Recovery::find_gaps classifies a gap as benign VersionSkew rather than
// as damage by comparing the stream's version against the reader's. Measured
// on one artifact, the CMake build reported 8 holes and the Bazel build 30.
#  define FASTFHIR_VERSION_MINOR 0
#endif
#ifndef FASTFHIR_VERSION_BUILD
#  define FASTFHIR_VERSION_BUILD 0
#endif

#define FASTFHIR_STRINGIFY_IMPL(x) #x
#define FASTFHIR_STRINGIFY(x)      FASTFHIR_STRINGIFY_IMPL(x)
#define FASTFHIR_VERSION_STRING    FASTFHIR_STRINGIFY(FASTFHIR_VERSION_MAJOR) "." \
                                   FASTFHIR_STRINGIFY(FASTFHIR_VERSION_MINOR) "." \
                                   FASTFHIR_STRINGIFY(FASTFHIR_VERSION_BUILD)
