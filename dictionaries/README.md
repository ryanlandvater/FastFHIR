# FastFHIR Code Dictionaries

Auto-generated from NPM FHIR packages (`CodeSystem-*.json` + `ValueSet-ucum-*.json`).

## Files

| File | Contents |
|---|---|
| `FF_Codes.hpp` | `FF_CODES::FHIR::SYSTEM::CODE` — compound-key code constants |
| `generator/master_codes.json` | Master code data: systems, entries, version-organized IDs |

## Generation Pipeline

```
python -m generator
  1. generator/specs.py         → downloads NPM .tgz from packages.fhir.org
  2. generator/emit/dictionary.py  → scans packages, produces master_codes.json
  3. generator/emit/codes_header.py → generates FF_Codes.hpp
```

## Code Structure

```cpp
namespace FF_CODES {
    #define FF_CODE_DEF static inline constexpr uint32_t

    class Token { /* type-safe wrapper, implicit from uint32_t */ };

    namespace FHIR {
        struct ADMINISTRATIVE_GENDER {
            FF_CODE_DEF MALE   = 200;
            FF_CODE_DEF FEMALE = 199;
            FF_CODE_DEF OTHER  = 201;
            FF_CODE_DEF UNKNOWN = 202;
        };
        // ... 661 systems, 5,800+ constants
    }

    namespace UCUM_COMMON {
        FF_CODE_DEF PERCENT = 3149;
        // ...
    }
}
```

## CodeableConcept System Discriminator

When a code is not found in the permanent dictionary (MSB=0), it is stored as
a CodeableConcept block (MSB=1, `FF_CODEABLE_CONCEPT_FLAG`). The block carries
a 1-byte system discriminator at offset 10 that tells the decoder how to
interpret the variable-length payload.

### Block Layout

```
Offset  0– 7 : VALIDATION  (uint64_t)
Offset  8– 9 : RECOVERY    (uint16_t) — RECOVER_FF_CODEABLE_CONCEPT (0x000A)
Offset 10    : SYSTEM      (uint8_t)  — FF_CodeableConceptSystem
Offset 11    : LENGTH      (uint8_t)  — payload byte count
Offset 12+   : PAYLOAD     (variable) — LENGTH bytes
```

### System Registry

| Byte | System | URI | Encoding | Encoder | Decoder |
|---|---|---|---|---|---|
| 0x00 | UNKNOWN | (any) | 2-byte URL idx + string | `STORE_U16` + `memcpy` | `LOAD_U16` + `string_view` |
| 0x01 | UCUM | unitsofmeasure.org | raw ASCII variable | `memcpy` string | `string_view` |
| 0x02 | SNOMED_CT | snomed.info/sct | 8-byte uint64_t | `STORE_U64` | `LOAD_U64` / `%llu` |
| 0x03 | RXNORM | nlm.nih.gov/umls/rxnorm | 4-byte uint32_t | `STORE_U32` | `LOAD_U32` / `%u` |
| 0x04 | LOINC | loinc.org | raw ASCII variable | `memcpy` string | `string_view` |
| 0x05 | DICOM | dicom.nema.org | 4-byte uint32_t | `STORE_U32` / hex parse | `LOAD_U32` / `%08X` |
| 0x06 | CPT | ama-assn.org/go/cpt | 2-byte uint16_t | `STORE_U16` | `LOAD_U16` / `%u` |
| 0x07 | CVX | hl7.org/fhir/sid/cvx | 1-byte uint8_t | direct byte | `payload[0]` / `%u` |
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

## VTable Slot Encoding

Each `FF_FIELD_CODE` slot is a 32-bit value:

```
MSB = 0  → 31-bit dictionary index (FF_ResolveCode)
MSB = 1  → 31-bit signed relative offset to CodeableConcept block
            (sign-extended via FF_ResolveCodeableConceptOffset)
```

Dictionary max: `0x7FFFFFFF` (2,147,483,647). At current scale (~6,000 codes)
this is ~350,000× below the cap.

## Key Design Decisions

- **Compound keys**: `"administrative-gender|male"` → unique ID. Prevents
  cross-system collisions (e.g., `"active"` in `AccountStatus` vs
  `EncounterStatus` have different clinical meanings).
- **Fixed-width numeric encoding**: SNOMED, IDMP, RXNORM, CPT, CVX, MDC,
  MED_RT are stored as native-endian integers, not ASCII strings. Saves
  bytes and avoids string parsing on read.
- **Variable-length string encoding**: UCUM, LOINC, NDC, ICD codes, ISO
  codes are stored as raw ASCII. These have hyphens, dots, or composable
  grammars that don't fit fixed-width packing.
- **NPM source**: All codes come from the official HL7 FHIR NPM packages
  (`packages.fhir.org`). No ZIP downloads, no bundle files.
