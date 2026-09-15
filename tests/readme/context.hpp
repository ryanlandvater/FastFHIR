// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Copyright (c) 2025 Ryan Landvater
//
// ─────────────────────────────────────────────────────────────────────────────
// Context stanzas for the README compile gate.
//
// A README code block is prose. It elides whatever the surrounding paragraph
// already established -- "patient_handle" appears in four blocks under
// "Code Assignment Semantics" and is constructed in none of them, because a
// reader has just read Step 3. That elision is correct for a reader and fatal
// for a compiler, so the gate injects the missing declarations instead of
// demanding the README spell them out.
//
// Each stanza is a macro named FF_README_CTX_<NAME>, selected per fence with
// `<!-- ff-compile: needs=<name> -->` in README.md. They are macros rather than
// a header of declarations for one reason: a stanza expands INSIDE the wrapper
// function, so a block that declares its own `mem` gets no injection and no
// redefinition. Only what a fence asks for is injected.
//
// These declare and never dereference. The gate is `-fsyntax-only`: nothing
// here runs, no arena is mapped, and every handle is deliberately null.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef FF_README_CONTEXT_HPP
#define FF_README_CONTEXT_HPP

#include <FastFHIR.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// An anonymous arena. `mem` is the name the README uses throughout.
#define FF_README_CTX_ARENA                                                    \
    auto mem = FastFHIR::Memory::create();                                     \
    (void)mem;

// A sealed stream plus the reader over it. `view` is the window the finalize
// call fills; `parser` and `root` are what the read-path blocks navigate from.
#define FF_README_CTX_PARSER                                                   \
    FastFHIR::Memory::View view;                                              \
    FastFHIR::Parser parser(view.data(), view.size());                         \
    auto root = parser.root();                                                 \
    (void)root;

// The mutable write-path handles. Null by construction -- ObjectHandle is
// documented default-constructible to exactly this state (FF_Builder.hpp:545).
#define FF_README_CTX_HANDLES                                                  \
    FastFHIR::Reflective::ObjectHandle patient_handle;                         \
    FastFHIR::Reflective::ObjectHandle obs_handle;                             \
    FastFHIR::Reflective::ObjectHandle observation_handle;                     \
    (void)patient_handle;                                                      \
    (void)obs_handle;                                                          \
    (void)observation_handle;

// A read lens positioned on an Observation, for the choice ([x]) read block.
#define FF_README_CTX_OBSROOT                                                  \
    FastFHIR::Reflective::Node observation_root;                               \
    (void)observation_root;

// One EXT_REF word, for the extension predicate listing. The predicates take
// uint32_t (FF_Primitives.hpp:1579), so this is the wire type, not a wrapper.
#define FF_README_CTX_EXTREF                                                   \
    uint32_t ref = FF_EXT_REF_NULL;                                            \
    (void)ref;

// The file reader that "Step 1 -- Parse raw bytes" defines at file scope.
// Later blocks call it without redefining it, which is correct for a reader
// working through the page in order. A lambda rather than a declaration
// because a stanza expands inside the wrapper function.
#define FF_README_CTX_READFILE                                                 \
    auto open_read_only_file =                                                 \
        [](const char*) -> std::vector<uint8_t> { return {}; };                \
    (void)open_read_only_file;

#endif // FF_README_CONTEXT_HPP
