# CLAUDE.md — FastFHIR

FastFHIR is a C++20 zero-copy **binary serialization format for HL7 FHIR** (R4/R5):
offset-based data blocks in a memory-mapped arena, lock-free concurrent building, JSON
ingest/export, optional compact archives, and a Python code generator that emits the typed
C++ from official HL7 StructureDefinitions. **Pre-alpha** — see *Breaking the wire format
is currently allowed* below. Licensed under **MPL-2.0**; the
"FastFHIR" name and compatibility claims are governed by the conformance policy in
`TRADEMARK.md`, and attribution lives in `NOTICE`. Never modify `LICENSE`, `NOTICE`, or
`TRADEMARK.md`, and never strip the MPL header notice from source files, without explicit
maintainer direction. New source files copy the MPL header from any file in `src/`.

**Pending work lives in `TASKS.md`** — read its Execution contract before claiming a task.
Blocks A–E are engineering debt (build fixes, tests, recovery, WASM, hygiene); Blocks F–I
are strategic (benchmark publication, security hardening, packaging, specification).
Blocks J and K are **planned, unstarted**. Block J is external code systems (LOINC, SNOMED, …) in
two halves: generated compile-time headers (`FF_EXTERNAL_CODE::LOINC::X`) and optional
runtime validation layers discovered Vulkan-style, where a missing layer just means the
check does not run. Neither ever enters the permanent ledger or the wire format. Q14 and
Q16 are answered; Q15 (acquisition) is still open. Read its J0 preamble before writing
any of it. Block K is the **conformance validation layer** — generated from the
StructureDefinitions, attached to the `Builder`, linked only on request, modelled on the
Iris File Extension's. It rests on one split: **structural validation is inline and
mandatory** (offsets, recovery tags, bounds — wrong means unreadable bytes), **conformance
validation is optional and attachable** (cardinality, required fields, bound ValueSets —
wrong means a valid file a FHIR server rejects). A stream written with the layer attached
must be byte-identical to one written without it. Do K before J: K's hooks struct is the
interface J's discovered layers should populate, not a second mechanism. Read K0 first.
The former checklist/plan docs (audit, integration-revision, project-progress,
generator-refactor, unification, refactor-history) were consolidated into TASKS.md and
deleted; consult git history if you need them.

## Repo map

| Path | Role |
|---|---|
| `include/` | Public + internal headers. `FastFHIR.hpp` is the consumer entry point. `FF_Primitives.hpp` defines wire constants — **hand-maintained, values permanent**. `FF_Recovery.hpp` used to live here; it moved to `generated_src/` on 2026-08-19. |
| `src/` | Core library: Memory (VMA), Builder, Parser, Compactor, Ingestor, Dictionary, Extensions (WASM), Primitives. |
| `dictionaries/` | **The permanent wire ledgers — two JSON files and a README, nothing else.** `master_codes.json` (dictionary code IDs) and `master_tags.json` (`RECOVERY_TAG` values). Committed; every number is a wire constant that decodes stored archives. Append-only — **read `dictionaries/README.md` before touching this or anything that assigns an ID or a tag.** The C++ they project into is generator output in `generated_src/`, not committed. |
| `generator/` | Python code generator — see `generator/README.md` for the module map. `pipeline.py` orchestrates; `model/` pure data, `emit/` model→str, `bindings/` Python emission. `emit/code_ids.py` owns **numbering** (permanent), `emit/code_names.py` owns **naming** (source-level only), `emit/recovery_tags.py` projects the tag ledger into `generated_src/FF_Recovery.hpp`. Both committed ledgers live in `dictionaries/`, not here — a ledger is wire format, not generator machinery. |
| `generated_src/` | Generator output (~75 C++ files), including `FF_Codes.hpp`, the dictionary tables projected from `dictionaries/*.json`, and `FF_Recovery.hpp` projected from `master_tags.json`. **Gitignored** — produced at CMake configure time. Most of it requires network (HL7 / packages.fhir.org); the dictionary projection does not, needing only the committed ledger. |
| `python/` | pybind11 bindings (`FF_PythonBindings.cpp` → `_core`) + `fastfhir` package. `fastfhir.fields` is a generated **package** (`<build>/python/fields/`, one module + `.pyi` per resource, plus `py.typed`), emitted by `generator/bindings/python_fields.py` at build time — it is not a single `fields.py`, and it is not written into the source tree. |
| `tools/` | CLI tools: `ingestor/FF_Ingest.cpp`, `exporter/FF_Export.cpp`, `compactor/FF_Compact.cpp`. |
| `tests/` | `cpp/` (standalone-main tests via ctest), `python/` (README/round-trip suites via ctest `py_*`), `generator/` (pytest wire-format gate). |
| `architecture.md` | Deep reference for the binary format, VMA, builder, and read path. Read it before touching wire-format code. |
| `terminology_layer_architecture.md` | CodeableConcept / code-system encoding design. |

**Companion repo:** performance benchmarks live in
[FastFHIR-benchmark](https://github.com/ryanlandvater/FastFHIR-benchmark) (separate repo,
Bazel-built so FastFHIR and competitor libraries compile with identical flags across
systems; results are generated by running it). If you change the public API or wire
format here, flag it for that repo (TASKS.md execution contract rule 9). Performance
claims in README/docs must cite benchmark results, never be asserted bare.

## Build & test

`CMakePresets.json` defines the three supported configurations — prefer them over ad-hoc
flags, since they encode the options each workflow needs:

```bash
cmake --preset ninja       && cmake --build --preset ninja        # CLI / CI (build/)
cmake --preset xcode       && cmake --build --preset xcode        # Xcode IDE (build-xcode/)
cmake --preset xcode-asan  && cmake --build --preset xcode-asan   # + ASan/UBSan
ctest --preset ninja       # or: ctest --preset xcode
```

**Xcode (Ryan's preferred IDE).** Open `build-xcode/FastFHIR.xcodeproj`, pick a scheme, set
the configuration to **Debug**, and run. Xcode is multi-config, so `CMAKE_BUILD_TYPE` is
ignored — the scheme's configuration is what selects Debug. Debug builds are `-O0` with
`dwarf-with-dsym`, so locals are readable and breakpoints bind on the first try.

The project navigator mirrors the repo — `src/`, `include/`, `dictionaries/`,
`generated_src/`, `tools/{exporter,compactor,ingestor}/`, `tests/cpp/`, plus `generator/`
and the project docs under a non-building `project_files` target so the Python is editable
in the IDE. The generated headers and sources stay in their own top-level `generated_src/`
tree instead of being folded into a generic source bucket. Targets are grouped into
**Libraries / Tools / Tests / ThirdParty / CMake**, and build products land in
**Products**. All of that comes from the IDE-layout block at the bottom of `CMakeLists.txt`; it
is presentation only and must never change what is built.

Schemes are generated for the runnable targets only: the two core library products,
`fastfhir` and `fastfhir_static`, the ingestor wrapper, the CLI tools, the tests, and
`build_all`. `RUN_TESTS` and `ALL_BUILD` get no scheme: the Xcode generator creates them
after this file is configured, so the property cannot be set on them. Run the suite with
`ctest --preset xcode`, or set `CMAKE_XCODE_GENERATE_SCHEME` back to `ON` in
`CMakePresets.json` for a scheme per target.

The project is regenerated by CMake; **do not commit it** (`build-xcode/` is gitignored).
Re-run the configure preset after editing `CMakeLists.txt` or adding sources.

If `cmake --preset xcode` fails with *"tool 'xcodebuild' requires Xcode"*, the active
developer directory is the Command Line Tools. Fix it once (needs your password):

```bash
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
```

Without switching, prefix any command with `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`.

```bash
# Manual configure, if you are not using a preset
cmake -S . -B build -DFASTFHIR_BUILD_INGESTOR=ON -DFASTFHIR_BUILD_TESTS=ON \
      -DFASTFHIR_BUILD_PYTHON_BINDINGS=ON
cmake --build build --target build_all -j

# C++ + Python integration tests (names: cpp_*, py_*)
ctest --test-dir build --output-on-failure

# Generator tests (wire-format gate)
# The gate regenerates under the profile in CMakePresets.json (pinned in
# tests/generator/conftest.py), NOT the generator's `us-core` default. That is
# load-bearing: `vtables` is derived from the emitted resource headers, so an
# unpinned run witnessed 141 blocks against a 209-block build and left every
# billing/supply/medication-admin V-Table ungated. Widen the profile → widen the
# golden in the same commit; the gate now fails if any emitted constant is
# missing from it.
pytest tests/generator -q

# If the wire format has intentionally changed (new block field, new vtable
# offset, new recovery tag, new code ID), the golden reference must be
# committed alongside the change:
#   python -m tests.generator.wire_witness generated_src \
#       tests/generator/golden/wire_witness.json
# Then `git add tests/generator/golden/wire_witness.json` in the same commit.
# A golden update without a corresponding generator or C++ change is a red
# flag — the gate exists to catch unintended drift. The witness script
# refuses to overwrite if the output matches the existing golden.

# Python lint/format (generator code only — generated C++ is out of scope)
ruff check generator tests/generator && black --check generator tests/generator
```

**A green suite here has meant less than it looks (2026-08-22).** Three defects landed in
one day inside code the suite covers, and `ctest` stayed at 36/37 throughout. The worst:
`validate_FFHR_stream()` was **rejecting all 342 Synthea bundles** with "the offset chain is
broken", while `ff_test_graph_bounds` — the test that exists for that function — passed. It
builds streams by hand, so it only validates byte patterns someone thought to write, never
a stream the writer actually produced.

The structural hole is that **no test feeds writer output to the reader and asserts on the
bytes**: `ff_roundtrip` never validates, and the unit tests never ingest. So the
writer → validator → reader path, where all three bugs lived, was covered by nothing. When
you add a test here, prefer one that goes through the real pipeline over one that
hand-builds a buffer; a synthetic fixture proves the reader agrees with your idea of the
format, not with the writer. TASKS.md **COV-1**.

**A new C++ test needs registering in FOUR places in `CMakeLists.txt`**, not one:
`add_ff_cpp_test(...)`, the ctest `foreach(_standalone ...)`, the `_BUILD_ALL` list, and
the two IDE folder/scheme lists. `add_ff_cpp_test` only creates the target — a test
registered with ctest but missing from `_BUILD_ALL` builds nothing and reports **"Not
Run"**, which is task A20 and is easy to reintroduce.

**Randomised suites pin their seed.** `ff_test_datetime` samples dates rather than
enumerating them, so it fixes a default seed, prints it on every run, and takes
`--seed <n>` to vary it — a suite that flakes is a suite that gets ignored, and a red
log has to name the command that reproduces it. Where a space is small enough to cover
completely (all 87,840 h:m:s, all 1,681 UTC offsets) it is enumerated instead. Prefer
that shape for new property-style tests.

Key CMake options: `FASTFHIR_PRODUCTION_PROFILE` (comma-separated union of
`us-core` (default) | `uk-core` | `billing` | `medication-admin` | `supply` |
`imaging` | `all`; `us`/`uk` are accepted aliases),
`FASTFHIR_BUILD_INGESTOR`
(needs simdjson; also gates `ff_ingest` and OpenSSL), `FASTFHIR_BUILD_TESTS`,
`FASTFHIR_BUILD_PYTHON_BINDINGS`, `FASTFHIR_RUN_GENERATOR` (default ON, at configure time).
Bazel targets mirror the CMake ones (`MODULE.bazel`/`BUILD.bazel`) but CMake is primary.
Windows: OpenSSL via vcpkg (see README → Windows Build Prerequisites).

### Performance measurements: Release only (2026-08-19)

All three presets (`ninja`, `xcode`, `xcode-asan`) configure **Debug (`-O0`)**
builds. Timings against `build/libfastfhir.dylib` are Debug numbers — TASKS.md's
OPEN TOPIC §A/§B tables were measured that way and their "-O2" label is wrong.
Whole-document read-path costs drop ~10× in Release:

| Path (50.8 MiB bundle, min of 7) | Debug `-O0` | Release `-O3` |
|---|---|---|
| `validate_FFHR_stream()` | 107.5 ms | **10.4 ms** |
| `Bundle.entry.entries()` (31,042 elements) | 856 µs | **36 µs** |
| `print_json()` → null sink | 734 ms | 197 ms |

Measure performance against a Release build (keep the Debug preset for
debugging):

```bash
cmake -S . -B build-opt -DCMAKE_BUILD_TYPE=Release -DFASTFHIR_BUILD_INGESTOR=ON
cmake --build build-opt -j
# link: -Iinclude -Igenerated_src -Lbuild-opt -lfastfhir, run with DYLD_LIBRARY_PATH=build-opt
```

Do not micro-optimize the read path from Debug measurements: a one-shot
`vector(count)` + `out[i]=` fill of `standard_node_entries()` won 856→525 µs at
`-O0` but **regressed 36→58 µs at `-O3`** (libc++ container annotations inline
to nothing at `-O3`; per-element `push_back` writes each 48-byte `Node` once,
the fill writes twice). Reverted — the `push_back` version stands. The
companion benchmark repo pins `--compilation_mode=opt` in its `.bazelrc`; keep
it there (see its README, "the Debug trap").

## Architecture in brief

- **Memory (VMA)** — `Memory::create()` / `createFromFile()` reserve a sparse virtual arena
  (mmap / VirtualAlloc). All structures live at stable offsets inside it; `Memory::View` is
  the lifetime-safe window; `StreamHead` is the exclusive raw-ingest cursor.
- **Builder** — appends `DATA_BLOCK`s via lock-free `claim_space()` (atomic bump); fields are
  fixed vtable slots back-patched by `amend_pointer` / `amend_resource` / `amend_variant`.
  `finalize()` writes the `FF_HEADER` (54-byte layout documented at
  `include/FF_Primitives.hpp:537`) and optional checksum footer.
- **Parser / `Reflective::Node`** — zero-copy read lens over the raw bytes; `root()[KEY]`
  navigation, `node.as<T>()` typed extraction, `print_json()` export. No heap allocation on
  the read path.
- **Dual type system** — every block carries a 2-byte `RECOVERY_TAG` (semantic identity,
  `generated_src/FF_Recovery.hpp`) and fields carry an `FF_FieldKind` (physical layout). Choice
  (`[x]`) fields are a 10-byte slot: 8-byte value/offset + 2-byte tag.
- **Codes** — assignment tries the dictionary (`FF_GetDictionaryCode` → permanent uint32 ID
  from `master_codes.json`), else writes an `FF_CODEABLE_CONCEPT` block and sets
  `FF_CODEABLE_CONCEPT_FLAG` (`0x80000000`) in the slot. `FF_CODE_NULL` = `0xFFFFFFFF`.
  Named constants are scoped by terminology source then CodeSystem
  (`FF_CODE::FHIR::ADMINISTRATIVE_GENDER::MALE`, `FF_CODE::UCUM::MMHG`); FHIR
  revision is *not* a namespace axis — that lives in the per-revision lookup tables.
- **Date/time** — the same slot contract as `FF_CODE`, widened to 8 bytes: bit 63 clear
  is a packed civil date/time (days from 0001-01-01, h/m/s, ms, signed UTC offset,
  3-bit precision), bit 63 set is a signed relative offset to an `FF_STRING` holding the
  original text, `FF_DATETIME_NULL` = all ones. **Civil time plus precision, never an
  instant** — `"2024"` must not round-trip as `"2024-01-01T00:00:00Z"`, `date` has no
  timezone, `time` has no date, and `:60` is a legal leap second. One layout, four tags
  (`RECOVER_FF_DATE`/`DATETIME`/`TIME`/`INSTANT`) — the tag names the FHIR type, the
  precision field says how much is populated, and the single `FF_FIELD_DATETIME` kind
  says only "inline 8 bytes". `Recovery_to_Kind` maps all four tags onto that kind;
  `Kind_to_Recovery` deliberately maps it back to **nothing**, because one kind naming
  four tags is not a function and a guess there exports a `date` as `valueDateTime`.
  Primitives, the kind, and the tests are in (`ff_test_datetime`). **Live on the wire for
  scalar slots and choice variants** since DT-2 (`Patient.birthDate` emits
  `ENCODE_FF_DATETIME` under `RECOVER_FF_DATE`); `DATETIME_TYPES` in
  `generator/model/type_map.py` replaced the old `STRING_TYPES` routing. **Array-typed
  date/time fields are the remaining gap** — three emitters still send them down the
  string-array branch, so `Timing.event` and `Timing.repeat.timeOfDay` are stored as
  `FF_STRING` (TASKS.md DT-2.4). Full layout: architecture.md §6.3.
- **Opaque JSON** — `RECOVER_FF_OPAQUE_JSON` (`0x0007`) is an `FF_STRING` block byte for
  byte, but its payload is already-serialized JSON that `print_json` splices in
  **unquoted**. A resource outside the compiled profile is retained this way instead of
  being dropped, so any FHIR document round-trips byte-exactly whatever the profile;
  what is lost is typed *access* (no V-Table → no `Node` navigation, no query, no interior
  compaction), which `Ingestor` reports on the `FF_Result`. Readers share one path via
  `FF_IsStringLayoutTag(tag)`; only the two render sites test the tag itself.
  **A resource slot's kind follows the tag beside its offset** — three of the four sites
  that build a Node from a 10-byte resource tuple used to hardcode `FF_FIELD_BLOCK`, which
  drops an opaque block silently (the `reflected_fields_view` → `{}` → "no members" shape).
  `CMakePresets.json` deliberately omits the `imaging` grouping so 1,444 real Synthea
  `ImagingStudy` resources exercise this path on every `py_roundtrip` run; enabling it
  would retire that coverage. architecture.md §6.1a.
- **Extensions** — per-extension `EXT_REF` word routes to a registered WASM codec module
  (MSB=1), a retained URL in `FF_URL_DIRECTORY` (MSB=0), or suppression (`0xFFFFFFFF`).
- **Compactor** — post-finalize rewrite of a sealed stream into a presence-bitmask compact
  layout; output is read-only, traversed with the same Node API. **Arrays keep the standard
  `FF_ARRAY` geometry**, so `archive_array` obeys the same array invariant the writer does:
  an inline-scalar array holds no offsets and is copied verbatim (self-offset rewritten),
  and only variable-length elements get an offset table. The **URL intern table is copied
  and its `SEG_OFFSET`s rewritten** — every `FF_FIELD_URL` slot is an index into it, so
  dropping it silently anonymises every `Extension.url` and `fullUrl` in the document. The
  **module registry is still dropped** (WASM-only, uncovered — a known gap). `ff_test_compact_roundtrip`
  requires the compact `print_json` to be byte-identical to the standard one; it found all
  three of those defects on its first run (TASKS.md CMP-1 / COV-1.5).

## Hard invariants — breaking these corrupts data on the wire

**Breaking the wire format is currently allowed (pre-alpha, decided 2026-08-20).** The
format has never been used in the wild, so there is no stored archive to stay compatible
with. A change to how a slot is *interpreted* — DT-1's packed date/time is the live example
— needs no compatibility statement, no engine-version gate, and no migration path, and no
reader-side shim for a superseded layout may be written. Do not add one, and do not
re-open the question at the next breaking change while this status holds. Nothing is at
risk on disk: no `.ffhr` is tracked in git, and the ones under `build/` are written by the
tests on each run.

This licenses **representation** changes, not renumbering. Invariant 1 below stands
unchanged: a `RECOVERY_TAG` or dictionary ID that has been assigned still never moves.

> **The two band re-cuts are closed history, not precedent.** Resources moved
> `0x0300`→`0x1000` on 2026-08-14, and `RECOVER_FF_CODE` moved to the scalar band
> (`0x010B`) on 2026-08-19 with the primitive band compacted behind it. Both predate the
> ledger's append-only regime, both are recorded in `dictionaries/master_tags.json`
> `_provenance`, and both were taken when no archive existed. **The values they produced
> are the permanent ones** — `test_primitives.cpp` asserts them precisely so they cannot
> drift again. Reviewers (including automated ones) read those assertions as *authorising*
> renumbering and file it as a violation; they are the opposite, and this paragraph is the
> answer to that review comment. No further re-cut is permitted under the pre-alpha
> allowance; that would be a separate decision, and it is Ryan's alone.
That discipline costs nothing to keep now, and it is the habit that has to already be in
place when the format freezes — which is precisely when it stops being recoverable.
Relaxing invariant 1 would be a separate decision, and it is Ryan's alone.

**A flagged offset is an offset, whatever slot it lives in.** `validate_FFHR_stream()`
skips inline scalars because they cannot aim the reader at memory it does not own — but a
scalar slot whose MSB flags a *fallback offset* is not inline data, and it is validated
like any other edge. That is the rule, not an exception to it: `FF_FIELD_CODE` with
`FF_CODEABLE_CONCEPT_FLAG` set is already sign-extended, resolved relative to the
containing block, and walked against `RECOVER_FF_CODEABLE_CONCEPT`
(`src/FF_Parser.cpp:573–590`). Every future MSB-discriminated slot owes the same — DT's
8-byte date/time slot with bit 63 set must be walked as a signed relative offset to an
`FF_STRING` (TASKS.md DT-1.5). A slot kind that can point somewhere and is not in
`slot_carries_offset` is a hole in the validator.

**A block-relative offset must be resolved while the parent is still in hand.** Both
fallback offsets are signed and relative to the *containing block*, so resolving one takes
two operands — and `Reflective::Node` carries only its own offset, never its parent's.
`Reflective::Entry` is the type that still has both (`parent_offset` + `vtable_offset`), so
the arithmetic belongs there or at node construction, never later. `ParserOps::code_node()`
is that single place for code slots; every producer of a code node calls it, and the node
it returns already points at the `FF_CODEABLE_CONCEPT`. Deferring it is not a style
preference — `Node::as<std::string_view>()` used to resolve against the node's own offset,
which for a choice (`[x]`) variant is the *slot*, and so read one V-Table width past the
block and returned an empty label: a silently dropped code, no crash, no warning. DT-3 owes
the date/time slot the same treatment.

**An array holds its entries, and its header tag says what they are.** Fixed-width
elements are stored inline — raw scalars at `sizeof(T)`, block headers at `T::HEADER_SIZE`,
polymorphic resources as inline 10-byte `{offset, tag}` tuples. **Variable length is the
only thing that forces an offset table**, and `FF_STRING` is the only element type that has
it, so every `FF_ARRAY::OFFSET` in the tree is a string array; complexity and polymorphism
do *not* force indirection. The element type comes from the array header's `RECOVERY` tag
(`GetTypeFromTag`, bit 15 = `RECOVER_ARRAY_BIT`) — that is the copy written by the same call
that laid out the bytes, so it cannot drift from the layout. **Never derive array layout
from a schema-side copy**: `FF_FieldKeys.hpp` disagrees with the wire on 6 `code` array
fields. `FF_ARRAY::EntryKind` answers only *whether the entry is a pointer*
(`entries_are_pointers`) — never *what the element is*: `INLINE_BLOCK` is stamped on
scalars, block headers and resource tuples alike, and its `SCALAR` value no emitter has
ever written. Asking the kind bits for element shape made every scalar array export as
`[]` **and** made `validate_FFHR_stream()` reject every valid stream holding one. A
resource tuple is `{offset(8), tag(2)}` — the same field positions as a `DATA_BLOCK`
header, but +0 is the *target's* offset, so it must be followed, never walked in place.
Full contract: architecture.md §5.

The residue that remains genuinely unchecked is narrow: a slot holding *only* inline bytes
(bit 63 clear), reinterpreted by a later format change. The `Parser` reads `engine_version`
but never rejects on it, so such a stream reads back *successfully and wrongly* rather than
failing — harmless while pre-alpha, and the reason the freeze (TASKS.md Q13/I1) has to
decide whether a version gate is wanted.

1. **Wire constants are permanent.** Never renumber or reorder: `RECOVERY_TAG` values
   (`dictionaries/master_tags.json` — the committed tag ledger; `generated_src/FF_Recovery.hpp` is
   generated from it at configure time and is **not** committed. Append-only, same rule as
   the code ledger: a new FHIR release takes the next free value in its band and nothing
   assigned ever moves. It covers
   the whole spec, so the header is byte-identical for every build profile), dictionary code IDs (`dictionaries/master_codes.json` — the committed ledger;
   **existing IDs are never reassigned and retired IDs are never reused; new codes append
   at `_next_id`** — this rule has been broken once, in `118d6ad`, silently invalidating
   every stored archive; see `dictionaries/README.md`), vtable offset arithmetic
   (symbolic sums in headers — never introduce literal offsets), `FF_HEADER` layout,
   `FF_CODEABLE_CONCEPT_FLAG`, `FF_CODE_NULL`, `FF_NULL_OFFSET`.
2. **Never hand-edit generated files** (`generated_src/` — which now includes `FF_Codes.hpp`
   and the dictionary tables — plus `generated_src/FF_Recovery.hpp` and the generated
   `fastfhir.fields` package).
   Fix the emitter in `generator/emit/` and regenerate. If a generated file and its emitter
   disagree, the emitter wins. Note the two JSON ledgers in `dictionaries/` are generated
   **and committed** — they are permanent wire artifacts, reviewed in diffs.
   `generated_src/FF_Recovery.hpp` is a *projection* of one of them and is **not** committed
   (moved out of `include/` and gitignored 2026-08-19): the ledger and
   `tests/generator/golden/wire_witness.json` are the committed record of every tag value.
3. **Two style regimes.** Python in `generator/` + `tests/generator/`: ruff/black enforced
   (pyproject.toml), full type hints, fail-loud (`raise` over silent fallback). C++ (and
   generated C++): match surrounding hand-tuned style; not subject to the Python tooling.
4. **Includes are bare names** (`#include "FF_Parser.hpp"`), never `"../include/..."` —
   both include dirs are on the compiler path. (Legacy violations are being removed —
   TASKS.md A2.)
5. **Errors are exceptions** (`std::runtime_error` / `std::system_error`) with actionable
   messages prefixed `"FastFHIR: "` on the write path; the read path returns falsy Nodes /
   null sentinels instead of throwing on absent fields. (A structured
   `"FastFHIR RECOVERY_REQUIRED:"` message convention is planned in TASKS.md Block C — it
   does not exist in the code yet; don't invent it outside that block.)
6. **Concurrency contract:** `claim_space()` appends are lock-free and thread-safe;
   pointer amendments and finalize are not concurrency-protected (see TASKS.md Q9). Don't
   introduce mutexes into the append hot path.
   **`FIFO::Queue` is lockless and safe for any number of concurrent consumer
   threads** — each entry is single-delivery via the `PENDING->READING` CAS, so
   consumers never serialize. The hazard is not sharing; it is the **zero-consumer
   window**: the chain collapses when no consumer handle exists, so at least one
   consumer must be alive from before the first push until the drain completes.
   Concretely: **create consumers before the first push, on the spawning
   thread, and move them into their workers.** `get_consumer()` latches the current head node;
   once a node fills (`NODE_ENTRIES` = 2000) the producer advances and the retirement path
   moves `_weak_head` past it, so a consumer that latches late starts mid-stream and
   silently loses every earlier node. That cost 2,000 bundle entries per affected ingest —
   `FF_SUCCESS`, no warning, a valid but truncated document — and only ever reproduced
   under CPU contention, because that is what delays the worker's first instruction past
   the producer's first node advance (TASKS.md AR-3). The predigest pool has always done
   this correctly and says so at `FF_Ingestor.cpp:727`; copy that pattern. Both pools
   run the same Iris-style dispatch (a `PoolStatus` atomic with park/notify instead of
   yield-spin; see the machinery at the top of `FF_Ingestor.cpp`). The queue
   enforces the convention with a **debug-only canary, not at runtime**: `Node`'s
   destructor (`#if FASTFHIR_DEBUG`, `FF_Queue.hpp`) scans the node's entries on
   deletion and **records a violation** on the queue's `debug_violations()`
   counter when any is still `ENTRY_PENDING`/`ENTRY_WRITING`/`ENTRY_READING` —
   it must NOT throw: a throw from `~Node` runs inside the retire path after the
   slot was exchanged, stranding the caller's `NodeRef` on a dead slot (the
   "Double-free detected" chain) and terminating inside the noexcept `~NodeRef`
   on every destructor route, so a counter the owner polls is the only reliable
   report. `ff_test_queue` asserts it. Release builds compile the scan out: the
   chain **collapses by design** (TASKS.md AR-4 decision) — no consumers, no
   reason for a node to live, un-consumed entries are freed and nothing leaks.
   Debug builds are where misuse must surface; the latch-before-push ordering is
   still the caller's duty, and no `FIFO::Queue` call site may assume the type
   protects it.
7. **The two SHA-256 roles in the WASM registry are never conflated:** `sha256(url)` is a
   disk metadata filename only; `sha256(wasm_bytes)` is module identity on the wire.

## Working a task from TASKS.md

1. Pick one unchecked task (or subsection) whose `Blocked on Q#` (if any) has an answer.
2. Re-verify every file:line reference with `grep` — line numbers drift.
3. Make the change; keep the diff scoped to that task.
4. Run the task's Verify command plus the affected suite (`ctest`, `pytest tests/generator`).
5. Check the box in TASKS.md with the commit hash, commit both together.

Common pitfalls for agents: `generated_src/` won't exist until you configure with network;
`ctest` Python tests use `.venv/bin/python` if present, else the system interpreter.

⚠ **`generated_src/` is never cleaned, so CHANGING THE PROFILE leaves a broken tree.**
The generator only writes; it never deletes output it no longer emits, and
`write_if_changed` leaves untouched files at their old mtime. Configure the same
`generated_src/` under a narrower profile and the orphaned `FF_<Resource>.{hpp,cpp}` stay,
the `CONFIGURE_DEPENDS` glob still compiles them, and they reference enums the freshly
regenerated `FF_CodeSystems.hpp` no longer contains:

```
generated_src/FF_SupplyDelivery.hpp:35:5: error: unknown type name 'FF_SupplyDeliveryStatus'
```

**`rm -rf generated_src` after any profile change** (reproduced 2026-08-23; a clean
regenerate fixes it every time).

✅ **GEN-1 is FIXED (2026-08-24), and it was never generator nondeterminism.**
`pytest tests/generator` was **rewriting the repo's own `generated_src/`**:
`test_regeneration_preserves_every_committed_id` ran `python -m generator` with no
`--output-dir`, and with `FASTFHIR_PRODUCTION_PROFILE` unset it regenerated the working
tree at the `CMakeLists.txt:67` default of `us-core`. So a tree built as
`us-core,billing,…` came back with a **72-enum** `FF_CodeSystems.hpp` while
`write_if_changed` left every billing source untouched at its older mtime — hence
`unknown type name 'FF_Use'` in generated code nobody had regenerated, "the next configure
fixed it", and an apparent flake that was in fact **once per test-suite run**. The test now
generates into a temp directory, and **three** guards stand behind that:
`validate_codesystem_enums` fails the configure by name if a short header ever appears
again (it is what caught this); `tests/conftest.py` hashes `generated_src/` **and**
`dictionaries/` around every pytest session and fails if a test modified either
(TASKS.md GEN-1.5 — **tests must never write into the working tree**; generate into a
`tempfile.TemporaryDirectory`, as `tests/generator/conftest.py` has always done); and the
profile-change rule above still applies. Historical note, because it cost two days:
the incident also printed 8/8 PASS from a **stale test binary** while its build was failing
with 10 errors — always confirm the build succeeded before believing a result.

## Portability lessons (paid for on MSVC and Xcode — don't relearn them)

- **A CMake target whose only sources are `$<TARGET_OBJECTS:...>` produces nothing under
  the Xcode generator**, and the build still reports success — dependents then fail at link
  with `no such file or directory: libfastfhir.dylib`. Xcode requires at least one real
  source file per target. `fastfhir` and `fastfhir_ingestor` are both object-only, so
  `CMakeLists.txt` hands them a generated empty TU (`_FF_XCODE_ANCHOR`) when the generator
  is Xcode. Any new object-only library needs the same treatment. Ninja and Makefiles are
  unaffected, which is exactly why this is invisible until someone opens the IDE.
- **CMake picks the first `python3` it finds unless given a floor.** On macOS that is the
  Command Line Tools 3.9, which cannot import the generator (PEP 604 annotations). The
  `find_package(Python3 3.11 REQUIRED ...)` floor is load-bearing, not cosmetic.

- MSVC rejects `std::vector<IncompleteType>` (GCC/Clang accept it). Self-referential
  generated structs (e.g. `Extension.extension`) must use `std::vector<Offset>`
  indirection; the `is_self_ref` flag must be set once at layout construction and honored
  by **every** emitter (store, size, deserialize, view) — a single field-type change
  propagates through all emission paths.
- No Unicode in generator print/output statements — cp1252 Windows terminals crash on it.
- Nothing in the build may require Perl (Windows runners lack it); OpenSSL comes from
  vcpkg on Windows.
- String-like FHIR types must be tested via `fhir_type in STRING_TYPES`
  (`generator/model/type_map.py`), never `== "string"` — ad-hoc checks have silently
  dropped `id`/`uri`/`markdown` fields before.
