/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file FF_Export.h
 * @brief FF_EXPORT — the one symbol-visibility macro, shared by both surfaces.
 *
 * Pure preprocessor, so a C compiler and a C++ compiler both read it. That is
 * the whole point: FF_EXPORT was defined inside FF_Primitives.hpp, which is a
 * C++ header the C ABI cannot include, so the C side needed a second copy of
 * the same platform ladder. Two definitions of one macro, maintained apart,
 * with nothing tying them together.
 *
 * Both FastFHIR.hpp and FastFHIR.h include this file, so there is one ladder.
 */
#ifndef FF_EXPORT_H
#define FF_EXPORT_H

#ifndef FF_EXPORT
#if defined(_WIN32) || defined(_WIN64)
#if defined(FF_BUILDING_DLL)
#define FF_EXPORT __declspec(dllexport)
#else
#define FF_EXPORT // consumers: link against the import lib; no annotation needed
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define FF_EXPORT __attribute__((visibility("default")))
#else
#define FF_EXPORT
#endif
#endif

#endif /* FF_EXPORT_H */
