/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Round-trip guards for ENCODE_FF_CODE / FF_DECODE_CODEABLE_CONCEPT.
//
// The two functions lay out the same block independently -- one switch per
// system on each side, ten systems, no shared layout descriptor. Nothing else
// exercises most of them: external_system_map is never populated (TASKS.md A8),
// so at runtime every code currently takes the UNKNOWN branch and the other
// six encoders are dead until that lands.
//
// This also pins the contract write_cc_header_() has to preserve now that the
// seven branches share it: header bytes written, and child_off advanced by
// exactly HEADER_SIZE + payload_len.

#include <FF_Primitives.hpp>
#include <FF_Dictionary.hpp>

#include <algorithm>

#include <cstdio>
#include <string>
#include <vector>

using S = FF_CodeableConceptSystem;

static int failures = 0;

static void CHECK(bool ok, const std::string& what)
{
    printf("  %-56s %s\n", what.c_str(), ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

int main()
{
    std::vector<BYTE> arena(64 * 1024, 0);
    BYTE* base = arena.data();

    struct Case {
        S system;
        const char* code;
        uint8_t expect_payload;  // bytes after the 12-byte header
    };
    const std::vector<Case> cases = {
        {S::SNOMED_CT, "73211009", 8},
        {S::IDMP, "999999999999", 8},
        {S::MED_RT, "4021840", 8},
        {S::RXNORM, "860975", 4},
        {S::MDC, "150456", 4},
        {S::DICOM, "0008103E", 4},
        {S::CPT, "99213", 4},  // Q12: widened 2 -> 4 bytes; 99213 no longer truncates
        {S::CVX, "300", 2},    // Q12: widened 1 -> 2 bytes; 300 no longer truncates
        {S::LOINC, "8867-4", 6},
        {S::UCUM, "hlm", 3},
        {S::NDC, "0002-8215-01", 12},
        {S::UNKNOWN, "some-local-code", 17},  // 2-byte url index + 15
    };

    for (const auto& c : cases) {
        // Precondition: the code must NOT be a dictionary entry, or
        // ENCODE_FF_CODE returns the dictionary id and never reaches the
        // CodeableConcept branch this test exists to exercise.
        if (FF_GetDictionaryCode(c.code, FHIR_VERSION_R5) != FF_CODE_NULL) {
            CHECK(false, std::string(c.code) +
                             ": test fixture is a dictionary code -- pick another");
            continue;
        }

        Offset child_off = 1024;
        const Offset before = child_off;
        std::fill(arena.begin(), arena.end(), BYTE{0});  // no stale bytes between cases

        uint32_t packed = ENCODE_FF_CODE(base, /*block_offset=*/512, child_off,
                                         std::string(c.code), FHIR_VERSION_R5, c.system);

        const std::string tag = std::string(c.code);

        // The slot must carry the CodeableConcept flag, not a dictionary id.
        CHECK((packed & FF_CODEABLE_CONCEPT_FLAG) != 0, tag + ": slot flagged as CodeableConcept");

        // write_cc_header_ must advance the cursor by header + payload, exactly.
        const Offset consumed = child_off - before;
        const Offset expect = FF_CODEABLE_CONCEPT::HEADER_SIZE + c.expect_payload;
        CHECK(consumed == expect,
              tag + ": child_off advanced " + std::to_string(consumed) + " (expect " +
                  std::to_string(expect) + ")");

        // Header bytes.
        CHECK(base[before + FF_CODEABLE_CONCEPT::SYSTEM] == static_cast<uint8_t>(c.system),
              tag + ": SYSTEM byte");
        CHECK(base[before + FF_CODEABLE_CONCEPT::LENGTH] == c.expect_payload,
              tag + ": LENGTH byte");
        CHECK(FF_GET_RECOVERY_TAG(base, before) == RECOVER_FF_CODEABLE_CONCEPT,
              tag + ": RECOVERY tag");

        // And it must decode back to the code we put in.
        auto decoded = FF_DECODE_CODEABLE_CONCEPT(base, before, FHIR_VERSION_R5);
        CHECK(decoded.system == c.system, tag + ": decoded system matches");
        CHECK(decoded.label == c.code,
              tag + ": round-trips (got '" + std::string(decoded.label) + "')");
    }

    // Fixed-width payloads must REFUSE a code they cannot hold. Narrowing
    // silently is data corruption: CPT is 2 bytes but real CPT codes reach
    // 99499, so a cast would store 99213 as 33677 and the decoder would
    // faithfully report the wrong procedure.
    {
        const std::vector<std::pair<S, const char*>> overflow = {
            {S::CPT, "4294967296"},  // > uint32
            {S::CVX, "65536"},       // > uint16
        };
        for (const auto& [sys, code] : overflow) {
            Offset child_off = 8192;
            bool threw = false;
            try {
                ENCODE_FF_CODE(base, 512, child_off, std::string(code), FHIR_VERSION_R5, sys);
            } catch (const std::runtime_error&) {
                threw = true;
            }
            CHECK(threw, std::string(code) + ": out-of-range fixed-width code refused");
        }
    }

    // Oversized codes must be refused, not silently truncated into the uint8
    // LENGTH field -- a wrap there makes the decoder compute len-2 on 0.
    {
        Offset child_off = 4096;
        bool threw = false;
        try {
            ENCODE_FF_CODE(base, 512, child_off, std::string(300, 'x'), FHIR_VERSION_R5,
                           S::UNKNOWN);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw, "oversized UNKNOWN code refused (no uint8 LENGTH wrap)");
    }

    // Every system in the enum must have a codec row. A missing row makes the
    // encoder silently fall back to UNKNOWN and the decoder return nothing --
    // which is what a per-system switch used to do by omission.
    {
        const std::vector<S> all = {
            S::UNKNOWN, S::UCUM,     S::SNOMED_CT, S::RXNORM,   S::LOINC,  S::DICOM,
            S::CPT,     S::CVX,      S::NDC,       S::ICD_9_CM, S::ICD_10, S::ISO_3166,
            S::MDC,     S::UNII,     S::MED_RT,    S::PCLOCD,   S::IDMP,
        };
        for (S sys : all) {
            Offset child_off = 16384;
            std::fill(arena.begin(), arena.end(), BYTE{0});
            // "9" parses under base 10 and 16, fits every width, and is not
            // a dictionary entry (which would take the fast path instead).
            ENCODE_FF_CODE(base, 512, child_off, std::string("9"), FHIR_VERSION_R5, sys);
            const auto d = FF_DECODE_CODEABLE_CONCEPT(base, 16384, FHIR_VERSION_R5);
            CHECK(d.system == sys && !d.label.empty(),
                  "system " + std::to_string(static_cast<int>(sys)) +
                      " has a codec row (encode+decode agree)");
        }
    }

    printf("%s\n", failures ? "FAILURES" : "all CodeableConcept round-trips hold");
    return failures ? 1 : 0;
}
