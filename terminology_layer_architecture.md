# FastFHIR — Runtime Terminology Validation & Translation Layer

> **Status:** IMPLEMENTED — See `include/FF_Primitives.hpp`, `src/FF_Primitives.cpp`
> **Date:** 2025-07-20
> **Scope:** Single-flag (Bit 31) CodeableConcept architecture with variable-length
> payloads and per-system discriminator byte.

---

## Table of Contents

1. [Problem Statement](#1-problem-statement)
2. [Current Architecture Survey](#2-current-architecture-survey)
3. [Revised Bit Layout for `FF_FIELD_CODE`](#3-revised-bit-layout-for-ff_field_code)
4. [`FF_CODEABLE_CONCEPT` — Block Specification](#4-ff_codeable_concept--block-specification)
5. [`FF_ExternalCodeSystem` — Permanent Registry](#5-ff_externalcodesystem--permanent-registry)
6. [Pre-Configured Validator Dispatch](#6-pre-configured-validator-dispatch)
7. [Memory Alignment & Arena Safety](#7-memory-alignment--arena-safety)
8. [Builder & Parser Integration](#8-builder--parser-integration)
9. [Generator Changes](#9-generator-changes)
10. [Performance Budget](#10-performance-budget)
11. [Extensibility](#11-extensibility)
12. [Implementation Sequence](#12-implementation-sequence)

---

## 1. Problem Statement

FastFHIR compiles bounded FHIR ValueSets (those with `"strength": "required"` and
a small, closed set of codes) into native C++ `enum class` artifacts via
`generator/emit/codesystems.py`. These enums are stored inline in the V-Table
and resolved through the permanent 31-bit dictionary (`FF_Dictionary.hpp`).

However, the generator explicitly **excludes** five ValueSet fragments
(`codesystems.py:24–29`):

```python
EXCLUDED_VALUESET_FRAGMENTS: set[str] = {
    "all-languages",
    "mimetypes",
    "ucum-units",
    "bcp:47",
    "languages",
}
```

These represent **open-ended vocabularies** — code systems with thousands,
millions, or conceptually infinite members:

| Code System | Canonical URI | Scale | Validation Complexity |
|---|---|---|---|
| **UCUM** | `http://unitsofmeasure.org` | ~300 base atoms, infinite combos | Grammar-based (DFA), algebraic normalization |
| **SNOMED CT** | `http://snomed.info/sct` | >350,000 active concepts | Numeric identifier |
| **LOINC** | `http://loinc.org` | ~100,000 codes | Structured numeric + check digit |
| **BCP-47** | `urn:ietf:bcp:47` | ~8,000 subtags, infinite combos | RFC 5646 grammar |
| **MIME Types** | `urn:ietf:bcp:13` | ~2,500 registered | RFC 6838 grammar |

Currently, these vocabularies bypass **all** validation. A `Quantity.code` of
`"mg/dL/[Hz]"` is stored verbatim as a raw `FF_STRING` via the
`FF_CUSTOM_STRING_FLAG` path with no syntactic or semantic checks.

This document specifies a **self-describing wire format** for external codeable
concepts and a **pre-configured validator dispatch** that integrates with the
existing `ENCODE_FF_CODE` / read-path pipeline.

---

## 2. Current Architecture Survey

### 2.1 The `ENCODE_FF_CODE` Dual Path

Defined in `src/FF_Primitives.cpp:331–348`:

```
ENCODE_FF_CODE(base, block_offset, child_off, code_str, version)
  │
  ├─ code_str.empty()? → FF_CODE_NULL
  │
  ├─ FF_GetDictionaryCode(code_str, version) → found?
  │    └─ YES → return 31-bit dictionary index (MSB = 0)
  │
  └─ FALLBACK (custom string):
       ├─ Write FF_STRING block at child_off
       ├─ Advance child_off
       ├─ Compute 31-bit signed relative offset from block_offset
       └─ Return relative_offset | FF_CUSTOM_STRING_FLAG (MSB = 1)
```

For UCUM/SNOMED codes the dictionary lookup always returns `FF_CODE_NULL`. Every
such code becomes a custom string — one `FF_STRING` block per occurrence. There
is no way to distinguish "this is a SNOMED code" from "this is an unknown ad-hoc
code" by inspecting the wire format alone.

### 2.2 The Read Path

`src/FF_Parser.cpp:475–482`:

```cpp
case FF_FIELD_CODE: {
    uint32_t raw = LOAD_U32(base + slot);
    if (raw == FF_CODE_NULL) { /* null */ }
    if (raw & FF_CUSTOM_STRING_FLAG) {
        Offset str_off = parent_offset + (raw & ~FF_CUSTOM_STRING_FLAG);
        // read FF_STRING ... 
    } else {
        const char* resolved = FF_ResolveCode(raw, version);  // dictionary
    }
}
```

### 2.3 How Open Codes Reach the Wire

From `STORE_FF_CODING` (`FF_DataTypes.cpp:230`):

```cpp
std::string __code_str = std::string(data.code);
STORE_U32(__ptr + FF_CODING::CODE,
          ENCODE_FF_CODE(__base, hdr_off, child_off, __code_str, __version));
// system is stored *after* the code:
if (!data.system.empty()) {
    STORE_U64(__ptr + FF_CODING::SYSTEM, child_off);
    child_off += STORE_FF_STRING(__base, child_off, data.system);
}
```

The system URI is a sibling field — the `CODE` slot alone carries no information
about which terminology system it belongs to.

---

## 3. Bit Layout for `FF_FIELD_CODE` (IMPLEMENTED)

The 4-byte `FF_FIELD_CODE` slot uses a single flag bit at Bit 31:

```
Bit 31 (MSB, 0x80000000) = FF_CODEABLE_CONCEPT_FLAG  → 31-bit signed relative offset
Bits 30–0                  = Dictionary index          → ~2.14B entries
```

Single constant in `include/FF_Primitives.hpp`:

```cpp
constexpr uint32_t FF_CODEABLE_CONCEPT_FLAG = 0x80000000;  // Bit 31
constexpr uint32_t FF_CODE_PAYLOAD_MASK     = 0x7FFFFFFF;  // Lower 31 bits
constexpr uint32_t FF_CODE_DICTIONARY_MAX   = 0x7FFFFFFF;
```

### 3.1 The 8-byte sibling: `FF_DATETIME` (live; date/time ARRAYS still pending)

The packed date/time slot is the same mechanism one width up, and belongs beside
this table so the two are read together rather than discovered separately.

**Status:** DT-2 routed `date`/`dateTime`/`instant`/`time` off `STRING_TYPES` and
onto `DATETIME_TYPES` in the model, store, deserialize and view emitters, so
**scalar slots and choice (`[x]`) variants are live on the wire** —
`Patient.birthDate` emits `ENCODE_FF_DATETIME` under `RECOVER_FF_DATE`. What
remains is **array-typed** date/time fields, which three emitters still send down
the string-array branch, so `Timing.event` and `Timing.repeat.timeOfDay` are
still stored as `FF_STRING` (TASKS.md **DT-2.4**).

| | `FF_FIELD_CODE` (4 B) | `FF_DATETIME` (8 B) |
|---|---|---|
| Discriminator | Bit 31, `FF_CODEABLE_CONCEPT_FLAG` | Bit 63, `FF_DATETIME_FALLBACK_FLAG` |
| MSB = 0 | 31-bit dictionary index | 63-bit packed civil date/time |
| MSB = 1 | 31-bit signed relative offset → `FF_CODEABLE_CONCEPT` | 63-bit signed relative offset → `FF_STRING` |
| Payload mask | `FF_CODE_PAYLOAD_MASK` (`0x7FFFFFFF`) | `FF_DATETIME_PAYLOAD_MASK` (`0x7FFF'FFFF'FFFF'FFFF`) |
| Null | `FF_CODE_NULL` (all ones) | `FF_DATETIME_NULL` (all ones) |
| Sign-extension | `FF_ResolveCodeableConceptOffset` | `FF_ResolveDateTimeOffset` |

```
Bit 63 (MSB) = FF_DATETIME_FALLBACK_FLAG → 63-bit signed relative offset to FF_STRING
Bits 62–41   = civil days from 0001-01-01, unsigned (years 0001..9999)
Bits 40–36   = hour     Bits 35–30 = minute     Bits 29–24 = second (60 legal)
Bits 23–14   = millisecond          Bits 13–3  = UTC offset, signed minutes
Bits  2–0    = precision (YEAR, YEAR_MONTH, DATE, SECOND, FRAC1, FRAC2, FRAC3)
```

Read path — the same binary branch as the code slot, testing the null sentinel
first because all-ones has the flag bit set in both:

```cpp
case FF_FIELD_DATETIME: {                       // one kind, all four date/time tags
    uint64_t raw = LOAD_U64(base + slot);
    if (raw == FF_DATETIME_NULL) { /* absent */ }
    if (FF_DATETIME_IS_FALLBACK(raw)) {
        Offset str_off = FF_ResolveDateTimeOffset(raw, parent_offset);  // sign-extend 63-bit
        return /* the original text in that FF_STRING */;
    }
    return FF_FORMAT_DATETIME(FF_UNPACK_DATETIME(raw), tag);
}
```

Unlike the code slot, the inline form carries no terminology at all — a
date/time is not a coded concept and never consults the dictionary, so none of
the CodeSystem machinery in this document applies to it. It shares the *slot
contract* and nothing else. Full rationale is in `architecture.md` §6.3.

**Read path** — binary branch with sign-extension (see `include/FF_Parser.hpp`,
`include/FF_Utilities.hpp::FF_ResolveCodeableConceptOffset`):

```cpp
case FF_FIELD_CODE: {
    uint32_t raw = LOAD_U32(base + slot);
    if (raw == FF_CODE_NULL) { /* null */ }
    if (raw & FF_CODEABLE_CONCEPT_FLAG) {
        int32_t rel_off = static_cast<int32_t>(raw << 1) >> 1;  // sign-extend 31-bit
        Offset cc_off = parent_offset + static_cast<Offset>(static_cast<int64_t>(rel_off));
        return FF_DECODE_CODEABLE_CONCEPT(base, cc_off, version);
    }
    return FF_ResolveCode(raw, version);  // Dictionary (31-bit index)
}
```

**Why shift-based sign extension.** The old XOR-bias technique required knowing
which bit was the sign bit. The shift-based approach works uniformly:
left-shift by 1 puts bit 30 in the sign position of `int32_t`, arithmetic
right-shift sign-extends the full 31-bit signed range (±1 GB).

**`parent_offset` above is the containing block, and that is not negotiable.**
The offset is block-relative, so resolving it needs an operand the value itself
does not carry. `Reflective::Entry` still has it (`parent_offset` +
`vtable_offset`); `Reflective::Node` does not — a node knows only its own
offset. Every path that turns a code slot into a node therefore resolves through
`ParserOps::code_node()`, which does the arithmetic at construction and returns
a node already pointing at the `FF_CODEABLE_CONCEPT`. Code that defers it has
already lost the operand: `Node::as<std::string_view>()` used to resolve against
the node's own offset, which for a choice (`[x]`) variant is the *slot*, and
returned an empty label instead of the code — silently.

**The same rule already applies to date/time variants, and is implemented.**
DT-2 made `resolve_choice` return a real `FF_FIELD_DATETIME` node, and DT-3
resolves the bit-63 fallback offset while `parent_offset` is still in hand,
exactly as `code_node()` does for codes. This is not future work — reading it as
future work is how the code path gets written a second time, deferred, and
against the wrong base.

---

## 4. `FF_CODEABLE_CONCEPT` — Block Specification

### 4.1 Layout

A fixed-stride block padded to 24 bytes for clean 8-byte boundary alignment on
ARM and other platforms that fault on unaligned 64-bit loads.

```
Offset  0– 7 : VALIDATION  (uint64_t)     — standard DATA_BLOCK
Offset  8– 9 : RECOVERY    (uint16_t)     — RECOVER_FF_CODEABLE_CONCEPT (0x0009)
Offset 10    : SYSTEM      (uint8_t)      — FF_ExternalCodeSystem enum value
Offset 11–17 : CODE        (7 bytes)      — packed code payload, system-specific
Offset 18–23 : _PADDING    (6 bytes)      — alignment to 24-byte stride
──── header: 24 bytes ────
```

Defined in `FF_Primitives.hpp` following the existing pattern:

```cpp
## 4. `FF_CODEABLE_CONCEPT` — Block Specification (IMPLEMENTED)

### 4.1 Layout

Variable-length block. No fixed padding — `FF_Ops.hpp` handles unaligned ARM access.

```
Offset  0– 7 : VALIDATION  (uint64_t) — standard DATA_BLOCK
Offset  8– 9 : RECOVERY    (uint16_t) — RECOVER_FF_CODEABLE_CONCEPT (0x0009)
Offset 10    : SYSTEM      (uint8_t)  — FF_CodeableConceptSystem discriminator
Offset 11    : LENGTH      (uint8_t)  — payload byte count
Offset 12+   : PAYLOAD     (variable) — LENGTH bytes
```

Defined in `include/FF_Primitives.hpp`:

```cpp
struct FF_EXPORT FF_CODEABLE_CONCEPT : DATA_BLOCK {
    static constexpr char type[] = "FF_CODEABLE_CONCEPT";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_CODEABLE_CONCEPT;

    enum vtable_offsets {
        VALIDATION = 0,
        RECOVERY   = 8,
        SYSTEM     = 10,
        LENGTH     = 11,
        PAYLOAD    = 12,
        HEADER_SIZE = 12,  // VALIDATION(8) + RECOVERY(2) + SYSTEM(1) + LENGTH(1)
    };

    explicit FF_CODEABLE_CONCEPT(Offset off, Size total_size, uint32_t ver)
        : DATA_BLOCK(off, total_size, ver) {}

    FF_CodeableConceptSystem system(const BYTE* base) const noexcept {
        return static_cast<FF_CodeableConceptSystem>(base[__offset + SYSTEM]);
    }
    uint8_t length(const BYTE* base) const noexcept {
        return base[__offset + LENGTH];
    }
    const BYTE* payload(const BYTE* base) const noexcept {
        return base + __offset + PAYLOAD;
    }
};
```

### 4.2 Payload Encoding Per System

| System | LENGTH | Payload |
|---|---|---|
| **DICOM** (`0x05`) | 4 (always) | `STORE_U32` of hex-parsed tag (e.g., `"0008002A"` → `0x0008002A`) |
| **SNOMED CT** (`0x02`) | 8 (always) | `STORE_U64` of decimal-parsed concept ID (e.g., `"1006005"`) |
| **UCUM** (`0x01`) | variable | Raw ASCII UCUM expression string |
| **UNKNOWN** (`0x00`) | variable | 2-byte `STORE_U16` URL index + raw ASCII code string |
| **LOINC** (`0x03`) | variable | Raw ASCII LOINC code (reserved) |
| **BCP_47** (`0x04`) | variable | Raw ASCII language tag (reserved) |
| **MIME** (`0x06`) | variable | Raw ASCII MIME type (reserved) |

**Why variable-length.** The old 7-byte fixed packing could not represent
SNOMED concept IDs > 2^56 or arbitrary UCUM compositional expressions without
a pre-compiled expression table. The current design stores composable
strings as-is (variable LENGTH) and fixed-width integers at their natural
size (4 for DICOM, 8 for SNOMED).

## 5. `FF_CodeableConceptSystem` — Permanent Registry (IMPLEMENTED)

One-byte discriminator stored in the `SYSTEM` field of each
`FF_CODEABLE_CONCEPT` block. Defined in `include/FF_Primitives.hpp`:

```cpp
enum class FF_CodeableConceptSystem : uint8_t {
    UNKNOWN           = 0x00,  // payload: uint16_t URL index + raw code string
    UCUM              = 0x01,  // payload: raw ASCII UCUM expression
    SNOMED_CT         = 0x02,  // payload: 8-byte native-endian concept ID
    LOINC             = 0x03,  // payload: raw ASCII LOINC code (reserved)
    BCP_47            = 0x04,  // payload: raw ASCII language tag (reserved)
    DICOM             = 0x05,  // payload: 4-byte native-endian tag
    MIME              = 0x06,  // payload: raw ASCII MIME type (reserved)
    // 0x07–0xFF  reserved for future terminologies
};
```

The old `FF_ExternalCodeSystem` enum and `FF_ExternalCodeSystemEntry` metadata
table were **deleted**. System URI resolution now happens in the parser via
`FF_DECODE_CODEABLE_CONCEPT` which dispatches on this single byte.
    BCP_47         = 0x04,  // urn:ietf:bcp:47
    MIME           = 0x05,  // urn:ietf:bcp:13
    // ── Append only ──
    // MAX_VALUE intentionally not defined — the table length is the bound.
};
```

### 5.2 Metadata Table

```cpp
struct FF_ExternalCodeSystemEntry {
    FF_ExternalCodeSystem  id;          // enum value = array index
    const char*            identifier;  // "UCUM"
    const char*            description; // "Unified Code for Units of Measure"
    const char*            url;         // "http://unitsofmeasure.org"
};

extern const FF_ExternalCodeSystemEntry FF_EXTERNAL_CODE_SYSTEM_TABLE[];
extern const size_t                      FF_EXTERNAL_CODE_SYSTEM_COUNT;

// O(1) — array index
inline const FF_ExternalCodeSystemEntry& FF_GetExternalCodeSystemInfo(
    FF_ExternalCodeSystem sys) noexcept {
    return FF_EXTERNAL_CODE_SYSTEM_TABLE[static_cast<uint8_t>(sys)];
}

// O(log n) — binary search on sorted URL strings (ingest-time only)
FF_ExternalCodeSystem FF_ResolveExternalCodeSystem(
    std::string_view url) noexcept;
```

### 5.3 Generated Table

Emitted into `generated_src/FF_ExternalCodeSystems.cpp` (analogous to
`FF_R4_Dictionary.cpp`):

```cpp
const FF_ExternalCodeSystemEntry FF_EXTERNAL_CODE_SYSTEM_TABLE[] = {
    {FF_ExternalCodeSystem::UCUM,      "UCUM",      "Unified Code for Units of Measure",
     "http://unitsofmeasure.org"},
    {FF_ExternalCodeSystem::SNOMED_CT, "SNOMED CT", "SNOMED Clinical Terms",
     "http://snomed.info/sct"},
    {FF_ExternalCodeSystem::LOINC,     "LOINC",     "Logical Observation Identifiers Names and Codes",
     "http://loinc.org"},
    {FF_ExternalCodeSystem::BCP_47,    "BCP-47",    "IETF Language Tags",
     "urn:ietf:bcp:47"},
    {FF_ExternalCodeSystem::MIME,      "MIME",      "Internet Media Types",
     "urn:ietf:bcp:13"},
};
const size_t FF_EXTERNAL_CODE_SYSTEM_COUNT =
    sizeof(FF_EXTERNAL_CODE_SYSTEM_TABLE) / sizeof(FF_ExternalCodeSystemEntry);
```

---

## 6. Pre-Configured Validator Dispatch

### 6.1 Validator Function Table

A parallel array indexed by the same `FF_ExternalCodeSystem` enum value. No
virtual dispatch, no hash lookup — one array index, one indirect call.

```cpp
// include/FF_Terminology.hpp  (new file)

namespace FastFHIR {

/**
 * @brief Validator signature: code string → result.
 *
 * All validators MUST be:
 *   - Stateless (no mutable state accessed during validation)
 *   - Reentrant (safe to call from any thread)
 *   - Allocation-free (no heap, no std::string)
 *   - Non-throwing (return FF_Result with error code)
 */
using FF_CodeValidator = FF_Result (*)(std::string_view code) noexcept;

/**
 * @brief Parallel to FF_ExternalCodeSystem — one function pointer per system.
 *
 * Indexed by `static_cast<uint8_t>(system)`.  Entry 0 (NULL_SYSTEM) is always
 * nullptr.  Entries for systems without built-in validators are nullptr
 * (pass-through).
 */
extern const FF_CodeValidator FF_EXTERNAL_VALIDATOR_TABLE[];

}  // namespace FastFHIR
```

### 6.2 Validator Implementations

Each validator lives in its own header and is assigned a slot in the table.

```cpp
// include/FF_UCUMValidator.hpp  (new file, header-only)

namespace FastFHIR {

/**
 * @brief UCUM syntactic + atomic validator.
 *
 * Validates that a UCUM expression conforms to the UCUM grammar and that every
 * atom exists in the pre-compiled UCUM atom table.  Does NOT perform
 * dimensional analysis (opt-in via normalize_code).
 *
 * State: constexpr atom table (~300 entries × 12 bytes ≈ 3.6 KB in .rodata).
 * DFA: 4 local variables (p, end, slash_count, in_annotation).
 */
FF_Result validate_ucum(std::string_view code) noexcept;

}  // namespace FastFHIR
```

```cpp
// include/FF_SNOMEDValidator.hpp  (new file, header-only)

namespace FastFHIR {

/**
 * @brief SNOMED CT concept ID structural validator.
 *
 * Validates that the code is an all-digit string in range [6, 18] characters.
 * Optional Verhoeff check-digit subclass left as a future extension.
 *
 * Cycles: ~35 (isdigit loop, max 18 chars).
 */
FF_Result validate_snomed(std::string_view code) noexcept;

}  // namespace FastFHIR
```

### 6.3 Generated Validator Table

Emitted into `generated_src/FF_ExternalCodeSystems.cpp`:

```cpp
#include "../include/FF_UCUMValidator.hpp"
#include "../include/FF_SNOMEDValidator.hpp"

const FF_CodeValidator FF_EXTERNAL_VALIDATOR_TABLE[] = {
    /* [0x00] NULL_SYSTEM */ nullptr,
    /* [0x01] UCUM        */ &validate_ucum,
    /* [0x02] SNOMED_CT   */ &validate_snomed,
    /* [0x03] LOINC       */ nullptr,  // future
    /* [0x04] BCP_47      */ nullptr,  // future
    /* [0x05] MIME        */ nullptr,  // future
};
```

### 6.4 Dispatch Macro

Hot-path invocation collapses to:

```cpp
// Inside ENCODE_FF_CODE or read-path decode:
FF_ExternalCodeSystem sys = cc->system(base);
if (auto* validator = FF_EXTERNAL_VALIDATOR_TABLE[static_cast<uint8_t>(sys)]) {
    FF_Result result = validator(code_str);
    if (!result) return FF_CODE_NULL;  // reject invalid code
}
```

Three instructions: load, index, call. ~5 cycles on a modern OoO core.

---

## 7. Memory Alignment & Arena Safety

### 7.1 FF_CODEABLE_CONCEPT Allocation

The block is 18 bytes. It is allocated via `Memory::claim_space(18)` — the
same atomic `fetch_add` path used by every other block type. Alignment follows
the arena convention: `claim_space` returns offsets aligned to the allocation
size's natural boundary. For 18 bytes, the returned offset is at least 2-byte
aligned; the `VALIDATION` field at offset 0 is an `STORE_U64` which is
explicitly unaligned-safe (the store macro uses `memcpy` under the hood on
platforms that require it).

### 7.2 No Fragmentation Risk

The arena is **write-once, append-only**. There are no frees, no reallocations,
no holes. The `FF_CODEABLE_CONCEPT` block is written alongside its parent block's
trailing payload — exactly where an `FF_STRING` would have been written in the
old custom-string path. The 30-bit offset encoding (±512 MB from parent) is
trivially satisfied because all child blocks are allocated shortly after their
parent in the same contiguous region.

### 7.3 Offset Range

| Flag | Bits | Range from Parent Block |
|---|---|---|
| `FF_CUSTOM_STRING_FLAG` (bit 31) | 31-bit signed | ±1 GB |
| `FF_CODEABLE_CONCEPT_FLAG` (bit 30) | 30-bit signed | ±512 MB |
| Dictionary index (bits 29–0) | 30-bit unsigned | 0..1B entries |

The 30-bit offset is sufficient because `FF_CODEABLE_CONCEPT` blocks are always
allocated within the same `STORE_FF_*` call that writes the parent — the parent
header and all its children are laid out sequentially in a single pass.

---

## 8. Builder & Parser Integration

### 8.1 Builder — Write Path

The Builder carries a validation policy (enum, not a service pointer):

```cpp
enum class ValidationPolicy : uint8_t {
    PASSTHROUGH = 0,  // No validation (current behavior, backward compatible)
    STRICT      = 1,  // Reject invalid codes → FF_CODE_NULL
    WARN        = 2,  // Log warning, store anyway
};
```

`ENCODE_FF_CODE` is extended with a system parameter and validation is
performed inline:

```cpp
uint32_t ENCODE_FF_CODE(BYTE* __base, Offset block_offset, Offset& child_off,
                         const std::string& code_str, uint32_t version,
                         FF_ExternalCodeSystem system = FF_ExternalCodeSystem::NULL_SYSTEM,
                         ValidationPolicy policy = ValidationPolicy::PASSTHROUGH)
{
    if (code_str.empty()) return FF_CODE_NULL;

    // ── Validation (hot path: predictable branch when policy == PASSTHROUGH) ──
    if (policy != ValidationPolicy::PASSTHROUGH && system != FF_ExternalCodeSystem::NULL_SYSTEM) {
        auto idx = static_cast<uint8_t>(system);
        if (auto* validator = FF_EXTERNAL_VALIDATOR_TABLE[idx]) {
            FF_Result result = validator(code_str);
            if (!result) {
                if (policy == ValidationPolicy::STRICT) return FF_CODE_NULL;
                // WARN: log and continue
            }
        }
    }

    // ── External codeable concept path ──
    if (system != FF_ExternalCodeSystem::NULL_SYSTEM) {
        return _encode_codeable_concept(__base, block_offset, child_off,
                                         code_str, system, version);
    }

    // ── Dictionary path (unchanged) ──
    uint32_t dict_code = FF_GetDictionaryCode(code_str, version);
    if (dict_code != FF_CODE_NULL) return dict_code;

    // ── Custom string fallback (unchanged) ──
    Offset string_offset = child_off;
    child_off += STORE_FF_STRING(__base, string_offset, code_str);
    Offset relative_offset = string_offset - block_offset;
    if (relative_offset > 0x7FFFFFFF)
        throw std::runtime_error("FastFHIR: Custom string relative offset exceeds 2GB.");
    return static_cast<uint32_t>(relative_offset) | FF_CUSTOM_STRING_FLAG;
}
```

### 8.2 `_encode_codeable_concept` — Internal Helper

```cpp
static uint32_t _encode_codeable_concept(BYTE* __base, Offset block_offset,
                                          Offset& child_off,
                                          const std::string& code_str,
                                          FF_ExternalCodeSystem system,
                                          uint32_t version)
{
    Offset cc_offset = child_off;
    child_off += FF_CODEABLE_CONCEPT::HEADER_SIZE;

    auto* ptr = __base + cc_offset;
    STORE_U64(ptr + FF_CODEABLE_CONCEPT::VALIDATION, cc_offset);
    STORE_U16(ptr + FF_CODEABLE_CONCEPT::RECOVERY,
              FF_CODEABLE_CONCEPT::recovery);
    STORE_U8(ptr + FF_CODEABLE_CONCEPT::SYSTEM,
             static_cast<uint8_t>(system));

    // Pack the code into 7 bytes (system-specific encoding)
    uint8_t packed[7] = {};
    _pack_code(system, code_str, packed);
    std::memcpy(ptr + FF_CODEABLE_CONCEPT::CODE, packed, 7);

    // Store 30-bit relative offset with flag
    Offset rel = cc_offset - block_offset;
    if (rel > 0x3FFFFFFF)
        throw std::runtime_error("FastFHIR: CodeableConcept offset exceeds 512MB.");
    return static_cast<uint32_t>(rel) | FF_CODEABLE_CONCEPT_FLAG;
}
```

### 8.3 Parser — Read Path

The parser decodes `FF_CODEABLE_CONCEPT` blocks into `std::string_view` via
a system-specific unpack function stored in a parallel decode table:

```cpp
using FF_CodeUnpacker = std::string_view (*)(const uint8_t (&packed)[7],
                                              char* buffer, size_t buf_size) noexcept;

extern const FF_CodeUnpacker FF_EXTERNAL_UNPACKER_TABLE[];

std::string_view FF_CODEABLE_CONCEPT::decode(const BYTE* base, Offset offset,
                                              uint32_t version) {
    FF_CODEABLE_CONCEPT cc(offset, 0, version);
    auto sys = cc.system(base);
    uint8_t packed[7];
    cc.code_bytes(base, packed);

    // Thread-local or stack buffer for string reconstruction
    thread_local char buf[64];
    auto idx = static_cast<uint8_t>(sys);
    if (auto* unpacker = FF_EXTERNAL_UNPACKER_TABLE[idx]) {
        return unpacker(packed, buf, sizeof(buf));
    }
    return {};  // unknown system
}
```

---

## 9. Generator Changes

### 9.1 `codesystems.py` Modifications

The generator already identifies open-ended fields via `EXCLUDED_VALUESET_FRAGMENTS`.
The change: instead of falling through to a raw `std::string_view` slot, the
generator emits code that resolves the system URI to an `FF_ExternalCodeSystem`
enum and passes it to `ENCODE_FF_CODE`.

For `Coding.code`:

```cpp
// ── Emitted by generator ──
FF_ExternalCodeSystem __ecs = FF_ExternalCodeSystem::NULL_SYSTEM;
if (!data.system.empty()) {
    __ecs = FF_ResolveExternalCodeSystem(data.system);
}
STORE_U32(__ptr + FF_CODING::CODE,
    ENCODE_FF_CODE(__base, hdr_off, child_off,
                   std::string(data.code), __version, __ecs,
                   m_validation_policy));
```

For `Quantity.code` (same pattern):

The generator emits the system resolution inline. If `data.system` is
`"http://unitsofmeasure.org"`, `FF_ResolveExternalCodeSystem` returns
`FF_ExternalCodeSystem::UCUM` — a single binary search in a 5-element table.

### 9.2 New Generated File

`generated_src/FF_ExternalCodeSystems.cpp` — permanent, auditable, committed:

- `FF_EXTERNAL_CODE_SYSTEM_TABLE[]` — metadata entries
- `FF_EXTERNAL_VALIDATOR_TABLE[]` — validator function pointers
- `FF_EXTERNAL_UNPACKER_TABLE[]` — code unpacking function pointers

A one-time Python script `tools/extract_code_systems.py` reads the FHIR
CodeSystem bundles and emits the table. This follows the same pattern as
`FF_R4_Dictionary.cpp` — generated once, committed permanently, never silently
regenerated.

---

## 10. Performance Budget

### 10.1 Hot Path — `ENCODE_FF_CODE` with System

| Operation | Cycles (est.) |
|---|---|
| Null check (`system == NULL_SYSTEM`) | 1 (predictable branch, trained to `true` for bounded codes) |
| Policy check (`policy == PASSTHROUGH`) | 1 (predictable branch) |
| Validator table lookup + call | 5 (load + indirect call) |
| UCUM validation (`mg/dL`) | ~150 (DFA + 2 atom lookups) |
| SNOMED validation (`22298006`) | ~35 (isdigit loop) |
| Pack code into 7 bytes | ~20 (system-specific switch) |
| STORE_U64/U16/U8 + memcpy | ~30 (4 store instructions) |
| Offset computation + return | 3 |

**Total for UCUM code field:** ~210 cycles (~70 ns at 3 GHz).  
**Total for SNOMED code field:** ~95 cycles (~32 ns).  
**Total when system is NULL_SYSTEM (bounded code):** +2 cycles vs. current (the
two predictable branches).

### 10.2 Read Path — `FF_CODEABLE_CONCEPT::decode`

| Operation | Cycles |
|---|---|
| Load validation + system byte | 3 |
| Load 7-byte payload | 2 |
| Unpacker table lookup | 3 |
| Unpacker call (system-specific) | 20–50 |
| **Total** | ~30–60 |

### 10.3 Memory Overhead

| Component | Size | Location |
|---|---|---|
| `FF_CODEABLE_CONCEPT` block (wire) | 18 bytes per occurrence | Arena |
| `FF_EXTERNAL_CODE_SYSTEM_TABLE[]` | ~5 entries × 40 bytes = 200 B | `.rodata` |
| `FF_EXTERNAL_VALIDATOR_TABLE[]` | ~5 entries × 8 bytes = 40 B | `.rodata` |
| `FF_EXTERNAL_UNPACKER_TABLE[]` | ~5 entries × 8 bytes = 40 B | `.rodata` |
| UCUM atom table | ~300 entries × 12 bytes = 3.6 KB | `.rodata` |
| `m_validation_policy` in Builder | 1 byte | `Builder` object |

**Total runtime overhead:** <4 KB of read-only memory, 1 additional byte in
Builder.

### 10.4 Wire Overhead Comparison

For a `Coding` with `system = "http://snomed.info/sct"` and
`code = "22298006"`:

| Path | Wire Bytes |
|---|---|
| **Current** (custom string): FF_STRING header (14) + "22298006" (8) + system FF_STRING header (14) + URL (25) = **61 bytes** |
| **New** (codeable concept): FF_CODEABLE_CONCEPT (18) + system FF_STRING (14 + 25 = 39 — system still stored as sibling for Coding.text readability) = **57 bytes** |
| **New** (without redundant system): FF_CODEABLE_CONCEPT (18) = **18 bytes** |

The last row is achievable for `Quantity.code` where the system is implied by
the `Quantity` type itself. For `Coding`, the system string is still needed for
display/interop, but the code no longer needs a separate FF_STRING allocation.

---

## 11. Extensibility

### 11.1 Adding a New Code System

1. Add enum value to `FF_ExternalCodeSystem` (append only).
2. Add metadata row to `FF_EXTERNAL_CODE_SYSTEM_TABLE`.
3. Implement validator function (`FF_Result validate_*(std::string_view)`).
4. Implement packer/unpacker for the 7-byte payload.
5. Wire into `FF_EXTERNAL_VALIDATOR_TABLE` and `FF_EXTERNAL_UNPACKER_TABLE`.
6. Run `tools/extract_code_systems.py` to regenerate the `.cpp` file.
7. Commit — the table is permanent from this point forward.

### 11.2 User-Defined Code Systems

Users who need a code system not in the compiled-in registry can:

- Use the `NULL_SYSTEM` path (custom string fallback) — backward compatible.
- OR: patch `FF_ExternalCodeSystem.hpp` and the generated table — the enum
  space is `uint8_t` (256 slots). Values 0x80–0xFF are reserved for user
  extensions.

### 11.3 Future: SNOMED Membership Validation

SNOMED CT concept IDs can be structurally valid (all digits) but not active in
the current release. A future `FF_SNOMEDMembershipValidator` could carry a
compiled-in bloom filter or minimal perfect hash of active concept IDs. This
would fit in the same validator function pointer slot — the interface is
opaque to the dispatch mechanism.

---

## 12. Implementation Sequence

### Phase 1: Wire Format & Primitives (1 day)

1. Add `FF_CODEABLE_CONCEPT_FLAG` and `FF_CODE_PAYLOAD_MASK` to
   `FF_Primitives.hpp`.
2. Define `FF_CODEABLE_CONCEPT` block struct in `FF_Primitives.hpp`.
3. `RECOVER_FF_CODEABLE_CONCEPT = 0x0009` in `dictionaries/master_tags.json`
   (projected into `generated_src/FF_Recovery.hpp`).
4. Implement `_encode_codeable_concept` and `_pack_code` in
   `FF_Primitives.cpp`.
5. Update read path in `FF_Parser.cpp` to handle the three-way branch.
6. **Test:** Roundtrip a manually constructed `FF_CODEABLE_CONCEPT`.

### Phase 2: ExternalCodeSystem Registry (0.5 days)

1. Create `include/FF_ExternalCodeSystem.hpp` with enum and metadata struct.
2. Generate `generated_src/FF_ExternalCodeSystems.cpp` with the initial 5 entries.
3. Implement `FF_ResolveExternalCodeSystem` (binary search on URL).

### Phase 3: Validators (2 days)

1. Extract UCUM atom table from `ucum-essence.xml` → `constexpr` array.
2. Implement `validate_ucum` DFA in `include/FF_UCUMValidator.hpp`.
3. Implement `validate_snomed` in `include/FF_SNOMEDValidator.hpp`.
4. Implement pack/unpack functions for UCUM and SNOMED 7-byte payloads.
5. Unit tests: valid/invalid UCUM expressions, SNOMED IDs.
6. Performance benchmark.

### Phase 4: Generator Integration (1 day)

1. Extend `codesystems.py` to emit system resolution + `ENCODE_FF_CODE` with
   `FF_ExternalCodeSystem` parameter.
2. Regenerate `FF_DataTypes.hpp/.cpp` and all resource stubs.
3. Integration test: ingest FHIR JSON with UCUM + SNOMED codes.

### Phase 5: Read-Path Decode (0.5 days)

1. Implement `FF_CODEABLE_CONCEPT::decode` with unpacker dispatch.
2. Thread through `CODINGView::get_code()` and `QUANTITYView::get_code()`.
3. Test roundtrip: write with codeable concept → read → identical string.

---

## Appendix A: UCUM Atom Table Extraction

One-time script `tools/extract_ucum_atoms.py` parses `ucum-essence.xml`:

```python
for unit in xml.findall(".//unit"):
    symbol  = unit.get("Code")          # e.g., "m", "g", "[in_i]"
    is_metric = unit.get("isMetric") == "yes"
    value_elem = unit.find("value")
    factor = float(value_elem.get("value", "1"))
    # Emit: {"m", 1.0, METRIC_ATOM}, {"[in_i]", 0.0254, NONMETRIC_ATOM}, ...
```

Generates `generated_src/FF_UCUMAtoms.inc` — a `constexpr` sorted array
included by `FF_UCUMValidator.hpp`.  ~300 entries, ~3.6 KB.

## Appendix B: Interaction with Compactor

No changes. The `FF_CODEABLE_CONCEPT` block is fixed-stride (18 bytes),
identical in treatment to any other DATA_BLOCK. The compactor already handles
18-byte blocks via the existing compact path.

## Appendix C: Interaction with WASM Extensions

WASM extension payloads (`RECOVER_FF_WASM_PAYLOAD`) that need terminology
validation should encode codes as `FF_CODEABLE_CONCEPT` blocks before WASM
processing. The WASM module can call `FF_CODEABLE_CONCEPT::decode` and
`FF_GetExternalCodeSystemInfo` via the host binding layer — both are plain C
functions with no virtual dispatch.
