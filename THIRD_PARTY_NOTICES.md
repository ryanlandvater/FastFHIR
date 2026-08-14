# Third-party notices

FastFHIR is licensed under MPL-2.0 (see `LICENSE`). This file covers material FastFHIR does **not** own but does redistribute, and material it deliberately does **not** redistribute.

---

## Where the line is drawn

FastFHIR's contribution is the **numbering** — the permanent, reproducible assignment of a code string to a 32-bit ID (similar to DICOM's Tag) — and the binary encoding around it. The terminology itself belongs to its publishers.

| | Shipped in this repository? |
|---|---|
| HL7 FHIR code values (from HL7's own CodeSystems) | **Yes** — `dictionaries/` |
| UCUM unit codes | **Yes** — `dictionaries/` |
| SNOMED CT, LOINC, RxNorm, ICD-9/10, CPT, NDC, EDQM, MED-RT, UNII, and every other external terminology | **No** |

Codes from terminologies FastFHIR does not redistribute are still fully supported at runtime. They travel as `FF_CODEABLE_CONCEPT` blocks carrying the literal code **you** supplied, under **your** license with that terminology's publisher. FastFHIR encodes them; it does not distribute them. The per-system encodings are listed in `dictionaries/README.md`.

This boundary is enforced by `_assert_redistributable()` in `generator/emit/code_ids.py`: a code arriving from a system outside the allowlist stops the build. Tests `test_generator_refuses_non_redistributable_sources` and `test_committed_codes_carry_no_foreign_terminology` keep it that way.

**No display names, descriptions, or definitions from any terminology are committed.** `generated_src/FF_Dictionary_Strings.cpp` (projected from `dictionaries/master_codes.json`) holds code strings only, and every C++ identifier in `FF_Codes.hpp` is derived mechanically from the code string — never from publisher display text.

---

## HL7 FHIR

Code values in `dictionaries/` originate from the official HL7 FHIR R4 and R5
NPM packages (`hl7.fhir.r4.core`, `hl7.fhir.r5.core`), fetched from
<https://packages.fhir.org>.

HL7®, FHIR® and the FHIR ® mark are registered trademarks of Health Level Seven
International. Use of these trademarks does not constitute an endorsement by
HL7. FastFHIR is not affiliated with, nor endorsed by, HL7 International.

FHIR specification material is published by HL7 International. Consult
<https://www.hl7.org/fhir/license.html> for the terms that apply to the
specification and its artifacts.

One code retained here, `100000155699` in
`http://hl7.org/fhir/regulated-authorization-case-type`, originates with EDQM
and is republished by HL7 within their own CodeSystem. It is included because
HL7 publishes it at an HL7 URL; if EDQM's terms require otherwise, add that
CodeSystem to `_EXPLICIT_BLOCK` in `generator/emit/code_ids.py` and
regenerate.

## UCUM

Unit codes originate from the UCUM value sets distributed in the HL7 FHIR
packages. UCUM is maintained by the Regenstrief Institute, Inc. and the UCUM
Organization.

The UCUM specification and tables are subject to the UCUM Copyright Notice and
License (<https://ucum.org/trac>). Under those terms a modified or derived unit
table may not be called "UCUM". FastFHIR does not modify the unit codes; it
assigns each an internal integer ID and stores the original code string
verbatim.

## Build-time and test dependencies

Not redistributed in this repository; fetched during build.

| Component | Role | Licence |
|---|---|---|
| simdjson | JSON ingest | Apache-2.0 |
| OpenSSL / BoringSSL | SHA-256 checksums | Apache-2.0 |
| pybind11 | Python bindings | BSD-3-Clause |
| Asio | networking examples in tests | Boost Software License 1.0 |
| WAMR | WASM extension codecs (optional) | Apache-2.0 with LLVM exception |

## Design lineage

The FastFHIR binary architecture draws on the Iris File Extension
(Ryan Landvater) and FlatBuffers (Wouter van Oortmerssen and the Google Fun
Propulsion Labs team, Apache-2.0). See `NOTICE`.

---

## If you are deploying FastFHIR

Obtaining any licence you need for the terminologies **your** data uses —
SNOMED CT affiliate licensing, LOINC terms of use, CPT licensing, and so on — is
your responsibility. FastFHIR ships none of that content, so using FastFHIR
neither grants nor requires those licences; encoding a SNOMED code you are
already licensed to use does not change your obligations either way.

This file is a good-faith summary, not legal advice. Confirm the terms with each
publisher before distributing a product built on FastFHIR.
