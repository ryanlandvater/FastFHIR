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
