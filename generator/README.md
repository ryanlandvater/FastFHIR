# `generator/` — the FastFHIR code generator

Turns official HL7 StructureDefinitions into the typed C++ that FastFHIR
compiles. Run it with:

```bash
python -m generator                    # writes generated_src/ and dictionaries/
python -m generator --output-dir DIR   # write the C++ elsewhere (dictionaries/ is unaffected)
```

## Two outputs, two very different contracts

| Output | Contract |
|---|---|
| `dictionaries/` | **Permanent numbering.** Committed to git. A code ID here is a wire constant that decodes every `.ffhr` archive ever written. The generator may only **append**. See [`dictionaries/README.md`](../dictionaries/README.md). |
| `generated_src/` | **Freely regenerable.** Gitignored, rebuilt from the HL7 packages at every CMake configure. Nothing here is permanent — but its *vtable layout* is, which is what `tests/generator/test_wire_format.py` guards. |

Getting this distinction wrong is how commit `118d6ad` renumbered the entire
code dictionary and silently invalidated every stored archive. Read
`dictionaries/README.md` before touching anything that assigns an ID.

## Module map

```
__main__.py          CLI entry point (python -m generator)
pipeline.py          Stage orchestration. No code generation lives here.
specs.py             Downloads + extracts the HL7 NPM packages.
library.py           The hub: per-resource generation, field keys, reflection.
utilities.py         Namespace helpers, recovery-tag parsing, and the three
                     post-emit VALIDATORS -- validate_recovery_tags (every tag
                     the emitters referenced is declared), validate_recovery_bands
                     (no tag outside its band, no duplicate value), and
                     validate_codesystem_enums (every parse_X/serialize_X the
                     emitted sources call has an enum class FF_X behind it).
                     Each turns a wall of C++ "undeclared identifier" errors into
                     one named generate-time failure.

model/               Pure data. No string emission.
  structure.py         StructureDefinition -> field layout
  merge.py             Reconciles R4 vs R5 into one block
  type_map.py          FHIR type -> C++ type, PRODUCTION_TYPES
  types.py             Typed Field/Block adapter over the layout dicts.
                       Not yet wired into the emitters -- it is the migration
                       target for that work, and is covered by
                       tests/generator/test_model.py.

emit/                model -> C++ strings.
  -- the three ledger emitters; see "Ledgers vs projections" below --
  code_ids.py          Owns code NUMBERING (permanent). Scans packages, appends
                       to dictionaries/master_codes.json, emits the string table
                       and per-version lookup tables into the output dir.
  code_names.py        Owns code NAMING (source-level only). Mints C++
                       identifiers and emits FF_Codes.hpp into the output dir.
  recovery_tags.py     Owns tag NUMBERING (permanent). Reconciles
                       dictionaries/master_tags.json and emits
                       FF_RecoveryTags.hpp into the output dir. No naming
                       counterpart: a tag's name is mechanically its FHIR path.
  codesystems.py       Bounded code enums (FF_CodeSystems.hpp)
  store.py             size_fields / store_fields
  deserialize.py       Eager struct deserializers
  ingest_mappings.py   simdjson _from_json deserializers (the heaviest emitter).
                       Which simdjson accessor a scalar primitive gets is the
                       _SCALAR_INGEST table, keyed on SCALAR_PRIMITIVE_TYPES and
                       shared by the 0..1 and 0..* paths -- never an if/elif
                       chain with a catch-all arm. A catch-all cannot tell a type
                       it planned for from one nobody wired up, which is how
                       every `decimal` in the spec spent its life being read with
                       get_uint64() and discarded.
  views.py             Lazy view structs, reflection dispatch
  traits.py            Resource trait headers
  header.py            auto_header banner, write_if_changed
  extensions_known.py  Known-extension filter table
  extensions_wasm.py   WASM codec emitter. Not wired into pipeline.py yet --
                       it belongs to the Block D extension subsystem in TASKS.md.

bindings/            Python binding emission.
  python_fields.py     Writes <output_dir>/python/fields/ -- a PACKAGE, one
                       module per resource (patient.py, bundle_entry.py, ...)
                       plus a .pyi stub each, base.py, and the PEP 561
                       py.typed marker. Imported as `fastfhir.fields`.
```

Everything under `emit/` is reachable from `pipeline.py` except the two modules
noted above, which are deliberately staged for planned work rather than dead.

## Pipeline stages

1. **Fetch** — `specs.py` pulls `hl7.fhir.r4.core` / `r5.core` from
   packages.fhir.org into `fhir_packages/<version>/package/`. Cached; needs
   network only on first run.
2. **Reconcile IDs** — `emit/code_ids.py` scans the packages and appends any
   new codes to `master_codes.json`. **Append-only.**
3. **Project the ledger** — `emit/code_names.py` writes `FF_Codes.hpp` into the
   output dir; `emit/code_ids.py` writes the string table and per-version lookup
   tables, also into the output dir. (Both, plus `recovery_tags.py`, honor
   `--output-dir`; the wire gate regenerates into a tmp dir and witnesses that
   tree.)
4. **Code systems** — `emit/codesystems.py` writes `FF_CodeSystems.hpp`.
5. **Library** — `library.py` emits the per-resource C++, field keys,
   reflection, and Python bindings, then **validates what it just emitted**
   against the headers it emitted it beside (`utilities.py`, above). The
   configure log shows all three:

   ```
   -- Validated 990 RECOVERY_TAG references against generated_src/FF_RecoveryTags.hpp
   -- Validated RECOVERY_TAG band membership and uniqueness
   -- Validated 61 code-system enum references against FF_CodeSystems.hpp
   ```

   These check the tree in `--output-dir`, never the repo's `generated_src/` —
   the wire gate regenerates into a temp directory, and validating it against an
   unrelated header checks nothing.
6. **Known extensions** — `emit/extensions_known.py`.

## Style

Enforced by `pyproject.toml` (`ruff` + `black`, line length 100), full type
hints, and fail-loud: `raise` over a silent fallback. A generator that quietly
emits an incomplete library is worse than one that stops — `library.py` raises
if a production resource is missing from every package, rather than skipping it.

```bash
ruff check generator tests/generator && black --check generator tests/generator
pytest tests/generator -q
```

The generated C++ has its own hand-tuned style and is out of scope for these
tools.

### Two rules the test suite now enforces

**A test must never write into the working tree.** Generate into a
`tempfile.TemporaryDirectory` and pass it as `--output-dir`. This is not a
preference: `test_regeneration_preserves_every_committed_id` once ran the
generator with no output dir, so `pytest tests/generator` regenerated the repo's
own `generated_src/` at the `us-core` default — leaving a short
`FF_CodeSystems.hpp` beside resource sources from a wider profile, which read as
an intermittent generator flake for two days. `tests/conftest.py` hashes
`generated_src/` and `dictionaries/` around every session and fails if either
moved.

**The wire gate pins its profile.** `tests/generator/conftest.py` reads
`FASTFHIR_PRODUCTION_PROFILE` from `CMakePresets.json` rather than inheriting the
environment. The `vtables` section of the witness is derived from the *emitted*
resource headers, so an unpinned run witnessed a 141-block `us-core` tree against
a 209-block build and left every billing/supply/medication-admin V-Table ungated.
Widen the profile → re-baseline the golden in the same commit.


## Ledgers vs projections

Three emitters own permanent wire numbers. They are split along one axis, and it
is worth knowing which:

| module | owns | ledger | emits | permanent? |
|---|---|---|---|---|
| `emit/code_ids.py` | code **numbering** | `dictionaries/master_codes.json` | `generated_src/FF_Dictionary_Strings.cpp` + the three lookup tables | **yes** |
| `emit/code_names.py` | code **naming** | (reads the ledger's `scopes`) | `generated_src/FF_Codes.hpp` | no — a rename breaks a recompile and nothing else |
| `emit/recovery_tags.py` | tag **numbering** | `dictionaries/master_tags.json` | `generated_src/FF_RecoveryTags.hpp` | **yes** |

Codes need two modules because a code's *name* is a real decision:
`FF_CODE::FHIR::ADMINISTRATIVE_GENDER::MALE` involves scoping and collision
handling (`CO`/`co`, `T`/`t`, `PHF`/`PhF` come from different systems).
Separating naming from numbering is what makes "a rename is allowed, a renumber
is not" enforceable rather than aspirational.

Recovery tags have no such axis — the name is mechanically the path
(`Bundle.entry` -> `RECOVER_FF_BUNDLE_ENTRY`), so there is nothing to mint and
no second module.

**The ledgers live in `dictionaries/` and are committed. Everything projected
from them is generator output.** A projection is not a second source of truth.
