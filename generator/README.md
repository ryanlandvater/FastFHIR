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
utilities.py         Namespace helpers, recovery-tag parsing.

model/               Pure data. No string emission.
  structure.py         StructureDefinition -> field layout
  merge.py             Reconciles R4 vs R5 into one block
  type_map.py          FHIR type -> C++ type, PRODUCTION_TYPES
  types.py             Typed Field/Block adapter over the layout dicts.
                       Not yet wired into the emitters -- it is the migration
                       target for that work, and is covered by
                       tests/generator/test_model.py.

emit/                model -> C++ strings.
  dictionary.py        THE ID LEDGER. Scans packages, appends new codes, emits
                       dictionaries/*.cpp. Owns numbering.
  codes_header.py      Emits dictionaries/FF_Codes.hpp, scoped by source then
                       CodeSystem. Owns naming (source-level only).
  codesystems.py       Bounded code enums (FF_CodeSystems.hpp)
  store.py             size_fields / store_fields
  deserialize.py       Eager struct deserializers
  views.py             Lazy view structs, reflection dispatch
  traits.py            Resource trait headers
  header.py            auto_header banner, write_if_changed
  extensions_known.py  Known-extension filter table
  extensions_wasm.py   WASM codec emitter. Not wired into pipeline.py yet --
                       it belongs to the Block D extension subsystem in TASKS.md.

bindings/            Python binding emission.
  python_fields.py     fastfhir/fields.py, .pyi stubs, py.typed
```

Everything under `emit/` is reachable from `pipeline.py` except the two modules
noted above, which are deliberately staged for planned work rather than dead.

## Pipeline stages

1. **Fetch** — `specs.py` pulls `hl7.fhir.r4.core` / `r5.core` from
   packages.fhir.org into `fhir_packages/<version>/package/`. Cached; needs
   network only on first run.
2. **Reconcile IDs** — `emit/dictionary.py` scans the packages and appends any
   new codes to `master_codes.json`. **Append-only.**
3. **Emit `dictionaries/`** — `codes_header.py` writes `FF_Codes.hpp`;
   `dictionary.py` writes the string table and per-version lookup tables.
4. **Code systems** — `emit/codesystems.py` writes `FF_CodeSystems.hpp`.
5. **Library** — `library.py` emits the per-resource C++, field keys,
   reflection, and Python bindings.
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
