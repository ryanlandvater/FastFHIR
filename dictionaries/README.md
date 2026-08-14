# `dictionaries/` — the permanent wire ledgers

**Everything in this directory is a wire constant. The numbers here decode every
`.ffhr` archive that has ever been written.**

Two ledgers live here, under identical rules:

- **`master_codes.json`** — dictionary code IDs (what a clinical code *is*).
- **`master_tags.json`** — `RECOVERY_TAG` values (what a block *is*), projected
  into `include/FF_Recovery.hpp`.

Both are committed, both are append-only, and both are generated-and-reviewed
rather than hand-edited. `master_tags.json` was moved here from `generator/` in
2026-08 — a ledger is not generator machinery, it is the wire format.

Change what a number means and every stored archive silently decodes to the
wrong clinical code — no error, no migration path, a lab result simply becomes a
different lab result. Read this page before touching anything here or in
`generator/emit/code_ids.py`.

---

## What may live here

**FastFHIR's contribution is the numbering, not the terminology.** Only code
values FastFHIR can redistribute belong in this directory:

| Source | Shipped here? |
|---|---|
| HL7 FHIR CodeSystems | yes |
| UCUM unit codes | yes |
| SNOMED CT, LOINC, RxNorm, ICD, CPT, NDC, EDQM, … | **no** |

External terminologies are still fully supported — they travel as
`FF_CODEABLE_CONCEPT` blocks carrying the code the *user* supplied, under the
user's own licence (see the system registry near the bottom of this file).
FastFHIR encodes them; it does not distribute them.

Enforced by `_assert_redistributable()` in `generator/emit/code_ids.py`: a
code from an unlisted system stops the build. This is a check rather than a
convention because the convention already failed — an old value-set expansion
pulled 13,310 LOINC/SNOMED/EDQM codes in here and nothing objected. They were
removed in the alpha reset described below.

No display names or definitions are committed: `FF_Dictionary_Strings.cpp` holds
code strings only, and identifiers are derived from the code string, never from
publisher display text. See `THIRD_PARTY_NOTICES.md`.

---

## The one rule

> **A code's ID is permanent. New codes append at the end. Nothing else moves.**

| Situation | What must happen |
|---|---|
| HL7 adds a code | Append it at `_next_id`. |
| HL7 removes a code | **Keep its ID forever.** Drop it from the version lists only — old archives still cite it. |
| A code moves between R4 and R5 | Version membership changes; the ID does not. |
| An ID is retired | Never reused. The slot stays a `nullptr` hole. |
| You want to re-sort the dictionary | You don't. Sorting renumbers, and renumbering is data loss. |

Enforced by `assign_ids()` and `_verify_ledger()` in
`generator/emit/code_ids.py`, and gated by `tests/generator/test_code_ids.py`.

### This has gone wrong before

Commit `118d6ad` renumbered the ledger from 1, dropped 16,436 codes, and added
none. Every ID changed meaning — `"!="` went from 1 to 3294. It went unnoticed
because the ID-stability test compared two fresh generator runs against *each
other* rather than against the committed ledger, so both renumbered identically
and the test passed. Hence: `assign_ids()` can only append, and the test now
compares against `master_codes.json`.

### The one deliberate exception

The ledger was reset **once**, during early alpha, to remove 13,310
LOINC/SNOMED/EDQM codes that an old value-set expansion had absorbed. The
remaining HL7/UCUM codes were renumbered from 1, taking the dictionary from
21,070 codes to 4,634. That was a considered trade: no production archives
existed, and the alternative was continuing to redistribute terminology the
project does not own.

**The append-only rule applies from that point forward.** Do not read this as
precedent — it was a licensing correction taken while the cost was zero, and
that window is closed.

---

## What each file is

**This directory holds the ledgers — the SOURCE. Everything projected from them
is generator output and lives in `generated_src/`, gitignored and rebuilt at
configure time.** A projection is not a second source of truth: committing both
meant 1.2 MB of derived C++ shadowing a 460 KB ledger, with two things to keep
in step instead of one. Review wire changes on the ledger diff.

| File | Direction | Role |
|---|---|---|
| `master_codes.json` | — | **The code ledger.** Source of truth for every dictionary ID. Committed, append-only, human-readable. Everything below is generated from it. |
| `master_tags.json` | — | **The recovery-tag ledger.** Source of truth for every `RECOVERY_TAG` value. Same rules; projected into `include/FF_Recovery.hpp` by `generator/emit/recovery_tags.py`. Covers the whole FHIR spec, so the emitted header does not vary with the build profile. |
| `generated_src/FF_Dictionary_Strings.cpp` | ID → string | The one backing store. **Array index _is_ the ID**, so slot order is the wire format. Retired IDs remain as `nullptr` holes. |
| `generated_src/FF_Codes.hpp` | name → ID | Named C++ constants, scoped by terminology source then CodeSystem (see below). Compile-time ergonomics only; never consulted at runtime. |
| `generated_src/FF_R4_Dictionary.cpp`<br>`generated_src/FF_R5_Dictionary.cpp`<br>`generated_src/FF_UCUM_Dictionary.cpp` | string → ID | Ingest lookup, one table per FHIR revision. Rows reference `FF_Codes.hpp` constants **symbolically**, so a rename or deletion breaks the *compile* instead of silently drifting. That is the one wire-safety property a C++ compiler can enforce for us. |

The version tables are **membership sets** ("which codes may be ingested for
this FHIR revision"), not independent mappings. One code, one ID, one string —
possibly two version memberships.

### How FF_Codes.hpp is scoped

By terminology **source**, then by CodeSystem within FHIR:

```cpp
FastFHIR::FF_CODE::UCUM::MMHG
FastFHIR::FF_CODE::FHIR::FDI_SURFACE::L
FastFHIR::FF_CODE::FHIR::ADMINISTRATIVE_GENDER::MALE
```

**FHIR revision (R4/R5) is deliberately not a namespace axis.** That is version
membership, and it already lives in the three lookup tables. Making it a
namespace too would only create overlap — 908 codes are in both R4 and R5, and
under shared-ID keying they are the same constant either way.

CodeSystem scoping is what actually kills name collisions: the clashing codes
(`CO`/`co`, `T`/`t`, `PHF`/`PhF`) come from different systems, so scoping
separates them where a flat namespace could not.

**Names are per scope, values are not.** One code has one ID, but each scope
names it the way its own system does:

```cpp
UCUM::LITER            = 15024;   // UCUM: litre
FHIR::FDI_SURFACE::L   = 15024;   // FDI:  lingual — same ID, same wire bytes
```

A `LEGACY` namespace appears only if a code that once had an ID is no longer
claimed by any current HL7 package. Such codes keep their IDs forever because
stored archives still cite them, but no source grouping survives for them. It is
empty today and is omitted from the header entirely when empty.

### Numbering vs naming

Two different things; only one is permanent.

- **Numbering** (`generator/emit/code_ids.py`) — the uint32 written to disk.
  **Permanent.**
- **Naming** (`generator/emit/code_names.py`) — what a code is *called* in
  C++. Source-level only; a rename breaks a recompile and nothing else.

A rename is allowed. A renumber is not. Identifiers come out of the ledger's
`scopes` map, so the header is a pure projection of committed state; only
genuinely new codes get a name minted.

---

## Regenerating

```bash
python -m generator
```

Then confirm the ledger only grew:

```bash
git diff --stat dictionaries/master_codes.json dictionaries/master_tags.json
pytest tests/generator -q
```

**Review checklist for any diff touching this directory:**

1. `git diff dictionaries/master_codes.json` shows **only additions** to `ids` (and
   `master_tags.json` only additions to `tags`). A
   changed line for an existing code is a renumber — stop.
2. `_next_id` went up, never down.
3. `FF_Dictionary_Strings.cpp` is no longer committed — the equivalent check is
   item 1: a changed `ids` entry *is* a shifted slot, because the array index is
   the ID. Regenerate and confirm the wire gate passes.
4. `pytest tests/generator -q` passes with **no skips**.

Never hand-edit the generated files in `generated_src/`. Fix the emitter and
regenerate; if a generated file and its emitter disagree, the emitter wins. The
ledgers here are hand-editable only to *append* — and even that is normally the
generator's job.

---

## Reserved values

| Constant | Value | Meaning |
|---|---|---|
| `FF_CODE_NULL` | `0xFFFFFFFF` | No code present. |
| `FF_CODEABLE_CONCEPT_FLAG` | `0x80000000` | Bit 31 set → the slot is an offset to an `FF_CODEABLE_CONCEPT` block, not a dictionary ID. |
| ID `0` | — | Reserved null slot; never assigned. |

Usable ID space is `1 .. 0x7FFFFFFF`; `_verify_ledger()` refuses any assignment
with bit 31 set. At ~21,000 codes this is ~100,000× below the cap.

Forty identifiers carry a `_CODE` suffix (`DOMAIN_CODE`, `M_E_CODE`,
`TRUE_CODE`, `I_CODE`, …) because the bare names are C/C++ standard-library
macros the preprocessor would eat — and the preprocessor ignores scoping, so a
namespace does not save you. That is a naming decision, not a numbering one; no
ID is affected. The alternative, `#undef`-ing them in a public header, would
sabotage consumers' own math code.

---

## VTable slot encoding

Each `FF_FIELD_CODE` slot is a 32-bit value:

```
MSB = 0  → 31-bit dictionary index (FF_ResolveCode)
MSB = 1  → 31-bit signed relative offset to CodeableConcept block
            (sign-extended via FF_ResolveCodeableConceptOffset)
```

## CodeableConcept system discriminator

When a code is not in the permanent dictionary (MSB=0), it is stored as a
CodeableConcept block (MSB=1, `FF_CODEABLE_CONCEPT_FLAG`). The block carries a
1-byte system discriminator at offset 10 telling the decoder how to interpret
the variable-length payload.

```
Offset  0– 7 : VALIDATION  (uint64_t)
Offset  8– 9 : RECOVERY    (uint16_t) — RECOVER_FF_CODEABLE_CONCEPT (0x000A)
Offset 10    : SYSTEM      (uint8_t)  — FF_CodeableConceptSystem
Offset 11    : LENGTH      (uint8_t)  — payload byte count
Offset 12+   : PAYLOAD     (variable) — LENGTH bytes
```

### System registry

**These byte values are wire constants too — append only, never renumber.**

The payload width, numeric base and output format for each system live in
**one place**: the `FF_CC_CODECS` table in `src/FF_Primitives.cpp`. Both
`ENCODE_FF_CODE` and `FF_DECODE_CODEABLE_CONCEPT` drive from it, and
`Entry::print_scalar_json` calls the decoder rather than carrying its own
switch. Adding a system is one row; changing a width is one number.

They used to be three independent switches. The decoder handled all 17 systems,
the JSON printer only 4 — so CPT, CVX, RxNorm, MDC, MED-RT and IDMP had their
binary payloads printed as if they were ASCII. And CPT at 2 bytes silently
stored `99213` as `33677`, because a `uint16` cannot hold the CPT code space.
Both were invisible until the systems became reachable.

| Byte | System | URI | Encoding | Encoder | Decoder |
|---|---|---|---|---|---|
| 0x00 | UNKNOWN | (any) | 2-byte URL idx + string | `STORE_U16` + `memcpy` | `LOAD_U16` + `string_view` |
| 0x01 | UCUM | unitsofmeasure.org | raw ASCII variable | `memcpy` string | `string_view` |
| 0x02 | SNOMED_CT | snomed.info/sct | 8-byte uint64_t | `STORE_U64` | `LOAD_U64` / `%llu` |
| 0x03 | RXNORM | nlm.nih.gov/umls/rxnorm | 4-byte uint32_t | `STORE_U32` | `LOAD_U32` / `%u` |
| 0x04 | LOINC | loinc.org | raw ASCII variable | `memcpy` string | `string_view` |
| 0x05 | DICOM | dicom.nema.org | 4-byte uint32_t | `STORE_U32` / hex parse | `LOAD_U32` / `%08X` |
| 0x06 | CPT | ama-assn.org/go/cpt | 4-byte uint32_t | table-driven | table-driven |
| 0x07 | CVX | hl7.org/fhir/sid/cvx | 2-byte uint16_t | table-driven | table-driven |
| 0x08 | NDC | hl7.org/fhir/sid/ndc | raw ASCII variable | `memcpy` string | `string_view` |
| 0x09 | ICD_9_CM | hl7.org/fhir/sid/icd-9-cm | raw ASCII variable | `memcpy` string | `string_view` |
| 0x0A | ICD_10 | hl7.org/fhir/sid/icd-10 | raw ASCII variable | `memcpy` string | `string_view` |
| 0x0B | ISO_3166 | urn:iso:std:iso:3166 | raw ASCII variable | `memcpy` string | `string_view` |
| 0x0C | MDC | urn:iso:std:iso:11073:10101 | 4-byte uint32_t | `STORE_U32` | `LOAD_U32` / `%u` |
| 0x0D | UNII | fdasis.nlm.nih.gov | raw ASCII variable | `memcpy` string | `string_view` |
| 0x0E | MED_RT | va.gov/terminology/medrt | 8-byte uint64_t | `STORE_U64` | `LOAD_U64` / `%llu` |
| 0x0F | PCLOCD | infoway-inforoute.ca/pCLOCD | raw ASCII variable | `memcpy` string | `string_view` |
| 0x10 | IDMP | medicinal product codes | 8-byte uint64_t | `STORE_U64` | `LOAD_U64` / `%llu` |
| 0x11–0xFF | (reserved) | — | — | — | — |

## Encoding rationale

- **Fixed-width numeric**: SNOMED, IDMP, RXNORM, CPT, CVX, MDC, MED_RT are
  stored as native-endian integers, not ASCII. Saves bytes and avoids string
  parsing on read.
- **Variable-length string**: UCUM, LOINC, NDC, ICD, ISO codes are raw ASCII —
  they carry hyphens, dots, or composable grammars that don't fit fixed-width
  packing.
- **NPM source**: all codes come from the official HL7 FHIR NPM packages
  (`packages.fhir.org`). No ZIP downloads, no bundle files.
