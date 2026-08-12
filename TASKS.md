# FastFHIR — Consolidated Task Backlog

> **This file is the single source of truth for pending work.** It absorbed and replaced
> the former checklist docs (`audit.md`, `audit.todo.md`, `audit.repomap.md`,
> `integration_revision.todo.md`, `project_progress.prompt.md`,
> `generator_refactor_plan.md`, `unification.plan.md`, `refactor_history.md`), which have
> been deleted; their content survives in git history. Every item below was re-verified
> against the tree on 2026-07-06.
>
> Read `CLAUDE.md` first — it defines the invariants you must not break.

## Execution contract (read before claiming anything)

1. **Claim exactly one task ID (e.g. A2.3) per session.** Do not batch tasks unless a task
   explicitly says "do together with".
2. **Line numbers drift.** Every task quotes the code it expects to find. Before editing,
   run the task's *Locate* command. If the output does not match the *Current state* shown,
   STOP — do not improvise. Leave the box unchecked and add a note under the task:
   `> STALE (date): <what you found instead>`.
3. **A task marked `Blocked on Q#` must not be started** until that question in
   [Questions for Ryan](#questions-for-ryan) has text after `> Answer:`.
4. **Acceptance criteria are mandatory.** A task is done only when every listed criterion
   is true and the *Verify* command exits 0.
5. **Never renumber wire constants** (RECOVERY_TAG values, dictionary code IDs in
   `generator/master_codes.json`, vtable offsets, `FF_HEADER` layout). If a task seems to
   require it, the task is wrong — stop and flag it.
6. **Never hand-edit generated files** (`generated_src/`, `dictionaries/*.cpp`,
   `dictionaries/FF_Codes.hpp`). Fix the emitter in `generator/emit/` instead.
7. One task = one commit. Commit message: `TASKS <ID>: <one-line summary>`. Check the box
   in TASKS.md in the same commit and append the short hash: `- [x] ... (abc1234)`.
8. Build prerequisites: first `cmake -S . -B build ...` configure needs network access
   (generator downloads FHIR bundles). If configure fails with a download error, report it
   — do not stub the generator.
9. **Benchmark parity:** if your task changes the public API or the wire format, note the
   change for the companion benchmark repo
   (<https://github.com/ryanlandvater/FastFHIR-benchmark>) in your PR description so its
   Bazel targets can be re-synced and results re-run.

---

## IFE audit (2026-08-12) — provenance for A15–A18, B7, E8–E12, I1.6

`LessonsFromIFE.md` reports what the Iris File Extension migration cost when a wire detail
was got wrong, and nominates two items as the highest-value work here. Auditing every
lesson against this tree produced the tasks listed above; each carries the evidence in its
**Context**. Two of the lessons' own nominations resolved as follows:

- **"Enforce append-only mechanically"** — the machinery exists (A4) and is the right
  shape. It is mis-aimed (A15), inverted in the one direction that matters (A16), and rests
  on an unstated ordering invariant (A17).
- **"Audit every packed/masked read under a sanitizer"** — the audit came back **clean**:
  `LOAD_U8/16/32/64` (`include/FF_Ops.hpp:49-71`) `memcpy` exactly 1/2/4/8 bytes, there is
  no `u24`/`u40` equivalent, and no load-wider-and-mask anywhere in `include/` or `src/`.
  The bug class that cost IFE the most is absent. **Do not re-audit this.** The sanitizer
  half remains open as G2, and the byteswap branches remain untested (E1).

Also verified clean, recorded so nobody repeats the work: prefix/index composition uses no
`|`-with-index pattern (IFE A3); `tests/generator/test_cross_language_constants.py` already
supplies the C++/Python redundancy IFE B1 argues for; the `.hpp`/`.cpp`/`_internal.hpp`
split IFE had to be corrected back into is already the norm here; and FHIR package
*versions* are pinned at `generator/specs.py:20-21` (their content is not — E8).

---

## Block A — Build & correctness fixes (highest priority, all independent)

### A1. Fix `ff_ingest` source-file case mismatch (build blocker)

**Context:** The source file on disk is `tools/ingestor/FF_Ingest.cpp` (capital letters).
Both build systems reference a lowercase name that does not exist. On case-sensitive
filesystems (Linux), any build with `FASTFHIR_BUILD_INGESTOR=ON` fails.

- [x] A1.1 In `CMakeLists.txt`, find the line (currently line 178):
  ```cmake
  add_executable(ff_ingest tools/ingestor/ff_ingest.cpp)
  ```
  Change it to:
  ```cmake
  add_executable(ff_ingest tools/ingestor/FF_Ingest.cpp)
  ```
- [x] A1.2 In `BUILD.bazel`, find the line (currently line 49):
  ```python
  srcs = ["tools/ingestor/ff_ingest.cpp"],
  ```
  Change it to:
  ```python
  srcs = ["tools/ingestor/FF_Ingest.cpp"],
  ```
- Locate: `grep -rn 'ff_ingest.cpp' CMakeLists.txt BUILD.bazel` (expect exactly 2 hits).
- Do NOT rename the file itself to lowercase — all other tool sources are capitalized
  (`FF_Export.cpp`, `FF_Compact.cpp`); the build files are what's wrong.
- Acceptance: `grep -rn 'ff_ingest.cpp' CMakeLists.txt BUILD.bazel` → no matches.
- Verify:
  ```bash
  cmake -S . -B build -DFASTFHIR_BUILD_INGESTOR=ON
  cmake --build build --target ff_ingest -j
  ```

### A2. Normalize `#include` paths to bare names

**Context:** Some sources use `"../include/X.hpp"` / `"../generated_src/X.hpp"`. Both
directories are already on the compiler include path (CMake: `FASTFHIR_INCLUDE_DIR` and
`FASTFHIR_GENERATED_DIR` via `target_include_directories`; Bazel: `includes`). Relative
paths break out-of-tree layouts and must become bare: `#include "X.hpp"`.

**Rule for every subtask:** change ONLY the path inside the quotes. Keep the header name,
keep the order of includes, keep any trailing comment on the line.

- [x] A2.1 `src/FF_Parser.cpp` — lines 13–17. Current state:
  ```cpp
  #include "../include/FF_Utilities.hpp"
  #include "../include/FF_Parser.hpp"
  #include "../include/FF_SIMD.hpp"
  #include "../include/FF_Dictionary.hpp"
  #include "../generated_src/FF_Reflection.hpp"
  ```
  Becomes:
  ```cpp
  #include "FF_Utilities.hpp"
  #include "FF_Parser.hpp"
  #include "FF_SIMD.hpp"
  #include "FF_Dictionary.hpp"
  #include "FF_Reflection.hpp"
  ```
- [x] A2.2 `src/FF_Compactor.cpp` — top of file: `"../include/FF_Compactor.hpp"`,
  `"../include/FF_Queue.hpp"`, `"../include/FF_Utilities.hpp"`,
  `"../generated_src/FF_Reflection.hpp"` → bare names, same pattern as A2.1.
- [x] A2.3 `src/FF_Ingestor.cpp` — lines 6–11: `"../include/FF_Queue.hpp"`,
  `"../include/FF_SIMD.hpp"`, `"../include/FF_Utilities.hpp"`,
  `"../generated_src/FF_Bundle.hpp"`, `"../generated_src/FF_IngestMappings.hpp"`,
  `"../generated_src/FF_KnownExtensions.hpp"` → bare names.
- [x] A2.4 `src/FF_Extensions.cpp` line 7 (`"../include/FF_Extensions.hpp"`) and
  `src/FF_Dictionary.cpp` lines 16–17 (`"../include/FF_Dictionary.hpp"`,
  `"../include/FF_Primitives.hpp"` — keep its trailing comment) → bare names.
- [x] A2.5 Generator emitters must also emit bare includes, or the problem returns on
  regeneration. Known instance — `generator/emit/master_dictionary.py` line 103:
  ```python
  cpp = '#include "../include/FF_Dictionary.hpp"\n\nstatic const FF_CodeEntry k{}Table[] = {{\n'.format(v_name)
  ```
  Change `"../include/FF_Dictionary.hpp"` to `"FF_Dictionary.hpp"` (inside the Python
  string; keep everything else on the line identical). Then sweep for others:
  `grep -rn '"\.\./' generator/` and fix each the same way. After fixing, regenerate
  (`python -m generator`) and confirm the regenerated `dictionaries/` files changed only
  in their include lines (`git diff dictionaries/`).
- [x] A2.6 Full rebuild + test after A2.1–A2.5 are all merged (do this task last):
  clean-configure, `cmake --build build --target build_all -j`,
  `ctest --test-dir build --output-on-failure`.
- Locate (whole block): `grep -rn '"\.\./' src/ include/ generator/`
- Acceptance: that grep returns nothing; build and ctest pass.

### A3. Fix stale API examples in `include/FastFHIR.hpp` doc comment

**Context:** The two `@code` Quick Start blocks at `include/FastFHIR.hpp` lines ~17–73
show an API that no longer exists and will mislead every reader:
`FastFHIR::Parser::create(...)` (Parser has a public constructor, no `create`),
`.value().as_string()` (no such methods — use `.as<std::string_view>()`),
`FastFHIR::FieldKeys::Observation::STATUS` (namespace is `FastFHIR::Fields`, resource
constants are ALL-CAPS: `Fields::OBSERVATION::STATUS`), and `FastFHIR::Builder builder;`
(Builder requires `Builder(memory, FHIR_VERSION_R5)`).

- [ ] A3.1 Rewrite Example 1 in the doc comment using this verified pattern (adapted from
  `tests/cpp/test_readme.cpp` and README "Step 3"):
  ```cpp
  auto mem = FastFHIR::Memory::create();
  FastFHIR::Builder builder(mem, FHIR_VERSION_R5);
  ObservationData obs{};
  obs.status = "final";
  auto root = builder.append_obj(obs);
  builder.set_root(root);
  auto view = builder.finalize(FF_CHECKSUM_SHA256, [](const unsigned char* p, size_t n) {
      std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);
      SHA256(p, n, hash.data());
      return hash;
  });
  FastFHIR::Parser parser(view.data(), view.size());
  std::string_view status = parser.root()[FastFHIR::Fields::OBSERVATION::STATUS];
  ```
- [ ] A3.2 Fix Example 2 (concurrent generation) the same way: `Builder` must be
  constructed with a `Memory` (`FastFHIR::Memory::create(2ULL*1024*1024*1024)`), not a raw
  size. Keep the thread-pool structure of the example.
- [ ] A3.3 Compile-check both snippets: create a scratch file `tests/cpp/scratch_doc.cpp`
  with `main()` wrapping each snippet, add it temporarily via `add_ff_cpp_test`, build,
  then REMOVE the scratch file and its CMake line before committing. The commit must
  contain only the `FastFHIR.hpp` comment change.
- Acceptance: no `Parser::create`, `FieldKeys::`, or `.value().as_string()` remains in
  `include/FastFHIR.hpp`; both snippets compiled at least once locally.
- Verify: `grep -n 'Parser::create\|FieldKeys::\|as_string' include/FastFHIR.hpp` → empty.

### A4. Arm the wire-format gate (currently proves nothing)

**Context:** `tests/generator/test_wire_format.py` is meant to pin wire constants
(recovery tags, dictionary codes, vtable layout) against a committed baseline. Today the
baseline file `tests/generator/golden/wire_witness.json` does not exist, so every gate
test calls `pytest.skip` — a green pytest run is meaningless. Additionally
`tests/generator/conftest.py` falls back to a stale in-repo `generated_src/` when the
generator fails, converting "generator broken" into "tests pass".

- [x] A4.1 Generate and commit the baseline:
  ```bash
  python -m generator                      # writes generated_src/ (needs network)
  python -m tests.generator.wire_witness generated_src tests/generator/golden/wire_witness.json
  ```
  Inspect the JSON before committing: it must contain non-empty `recovery_tags`,
  dictionary code entries, and vtable data. Commit ONLY the JSON (remember
  `generated_src/` is gitignored and must stay so).
  > **This criterion was not met (found 2026-08-12).** The committed golden holds
  > `{'codes': 0, 'tags': 0, 'vtables': 141}` — vtables only. The two empty sections are
  > structurally unfillable by the current witness, so the JSON could not have satisfied
  > this line. **A15 fixes it.** Leave this box checked (the vtable half is real and A4.2 /
  > A4.4 stand); A15 owns the remainder.
- [x] A4.2 Remove the fallback in `tests/generator/conftest.py`. Current state (lines
  ~61–64):
  ```python
  fallback = _REPO_ROOT / "generated_src"
  if not fallback.is_dir():
      pytest.skip("no generated_src/ and `python -m generator` unavailable")
  return fallback
  ```
  Replace the whole fallback branch with a hard failure:
  ```python
  raise RuntimeError(
      "`python -m generator` failed and no fallback is permitted post-cutover; "
      "fix the generator before running the wire gate"
  )
  ```
  Also make `_try_regenerate` raise (with the subprocess stderr in the message) instead of
  returning `None` on non-zero exit. Update the module docstring (lines ~3–11), which
  still describes the fallback.
- [ ] A4.3 Add a generated-C++ compile smoke test, `tests/generator/test_compiles.py`:
  regenerate into `tmp_path` (reuse the `regenerated_dir` fixture), write a one-line TU
  `#include "FF_Dictionary.hpp"` (plus one resource header, e.g. `FF_Patient.hpp`), and run
  `c++ -std=c++20 -fsyntax-only -I include -I <tmp> tu.cpp` via `subprocess`. Skip (with
  reason) only when `shutil.which("c++")` is None. Rationale: the witness reads constants
  with regex and cannot detect emitter bugs that produce non-compiling C++ — this class of
  bug has shipped before.
- [x] A4.4 Add a determinism test, `tests/generator/test_determinism.py`: run
  `python -m generator --output-dir <tmpA>` and `--output-dir <tmpB>`, then assert the
  trees are byte-identical (`filecmp.dircmp` recursive, assert no diff_files/left_only/
  right_only). If `--output-dir` is not a supported flag, check
  `generator/pipeline.py` / `generator/__main__.py` for the actual mechanism first.
- Acceptance: `pytest tests/generator -q` shows the wire tests RUNNING (not skipped) and
  passing; deleting a key from the golden JSON makes them fail.
- Verify: `pytest tests/generator -q -rs` — confirm no `SKIPPED` lines for
  `test_wire_format.py`.

### A5. Verify dictionary code-ID integrity guards (master-codes path)

**Context:** Dictionary code IDs are wire values: once a stream is written with ID N for
code string S, N must mean S forever.

**This was not merely unproven — it was already broken.** Commit `118d6ad` renumbered the
ledger from 1, dropped 16,436 codes, and added none; every ID changed meaning (`"!="` went
1 → 3294). The restore rebuilt `generator/master_codes.json` from `b1d8b36`, cross-verified
byte-for-byte against `dictionaries/FF_Dictionary_Strings.cpp` (whose array index *is* the
ID), and made `assign_ids()` append-only. See `dictionaries/README.md`.

- [x] A5.1 **Uniqueness:** confirm two distinct code strings can never receive the same ID.
  Read `generator/emit/dictionary.py` (`generate_master_codes`) and
  `generator/emit/master_dictionary.py`; if any path can assign a duplicate ID, add a
  fail-loud guard: build a `dict` of id→label during emission and
  `raise RuntimeError(f"code ID collision: {id} maps to {a!r} and {b!r}")` on clash.
- [x] A5.2 **Stability:** confirm regeneration never reassigns an ID already committed in
  `generator/master_codes.json` (i.e. existing entries are loaded and preserved; only new
  labels get new IDs). If not enforced, add the guard and a clear error message.
- [x] A5.3 **Reserved values:** confirm no assignable ID can equal `0xFFFFFFFF`
  (`FF_CODE_NULL`) or have bit 31 set (`FF_CODEABLE_CONCEPT_FLAG = 0x80000000` marks
  custom-string references — see README "Code Assignment Semantics"). The max-ID guard at
  `dictionary.py:137` may already cover this; verify the constant it checks is
  `< 0x80000000`, and add a comment stating WHY (bit 31 is the CodeableConcept flag).
- [x] A5.4 Add `tests/generator/test_code_ids.py` with three tests: duplicate-label input
  handling, committed-ID stability across two runs, and reserved-bit exclusion. Document
  the three guarantees in the `dictionary.py` module docstring.
- Acceptance: all three properties either demonstrated by existing code (cite line in the
  test's docstring) or newly guarded; pytest passes.
- Verify: `pytest tests/generator/test_code_ids.py -q`.

---

### A6. Concurrent bundle ingest — FIXED

`src/FF_Ingestor.cpp` passed a `task_payloads` vector to `Bundle_from_json`
expecting it to slice the entry array; the generated function only threaded its
`concurrent_queue` parameter to children and never pushed into it, so the count
guard always fired. The vector was vestigial — workers already index
`entry_chunks[idx]`. Removed it and took the count from `entry_chunks`.

- [x] A6.1 Contract decided: `entry_chunks` is the single source of truth.
- [x] A6.2 `task_payloads` deleted; `Bundle_from_json` called for metadata only.
- [x] A6.3 Misleading comment corrected.
- Result: bundles ingest ("bundle ingested : 2 resources"). Tests 5/9/11 now
  fail on three *different*, deeper issues — see A9, A10, A11.

### A7. Compact read path returned wrong codes — FIXED

`generator/model/structure.py:_compact_slot_size` re-derived the dense slot
width from `fhir_type`/`data_type` heuristics while the compactor
(`src/FF_Compactor.cpp:48 compact_slot_size`) switched on `FF_FieldKind`. They
disagreed on **1,393 of 1,611 slots** — the writer laid out 8-byte slots where
the reader's table said 0, so every field after the first string/array/block was
read from the wrong address.

Fixed structurally: `_compact_slot_size` now keys off `_field_kind_expr()`, the
same `FF_FieldKind` the C++ switch receives, so the two cannot drift apart. Note
the `return 0` fallback was only 1,366 of the mismatches — 27 more were
arrays-of-Resource, where the Python checked `fhir_type == "Resource"` before
`is_array` and returned 10 against the writer's 8.

- [x] A7.1–A7.3 Fixed and verified; `cpp_test_7` and `cpp_test_8` pass.
- Follow-up: add a generator test asserting every emitted `COMPACT_SLOT_SIZES`
  entry equals `compact_slot_size(kind)` for its `FF_FieldInfo` kind.

### A8. CodeableConcept system discriminator is never set

**Context:** `generator/emit/codesystems.py:148` initialises
`external_system_map` and returns it at `:286` **without ever populating it**.
So all 102 generated `ENCODE_FF_CODE` call sites take the no-system branch
(`store.py:287`) and `system` defaults to `FF_CodeableConceptSystem::UNKNOWN`.

Every code that misses the dictionary is therefore encoded as UNKNOWN (2-byte
URL index + raw string) regardless of its actual system. The per-system
encodings documented in `dictionaries/README.md` — SNOMED as 8-byte uint64,
RxNorm 4-byte, CPT 2-byte, CVX 1-byte — are specified but not wired up.

**This is now load-bearing.** The licensing boundary routes *all* external
terminology (SNOMED, LOINC, RxNorm, ICD, CPT, NDC) through this path, so it has
to work and to round-trip.

- [ ] A8.1 Populate `external_system_map` in `generate_code_systems` by mapping
      a field's bound ValueSet/CodeSystem URL to its `FF_CodeableConceptSystem`.
- [ ] A8.2 Verify `SIZE_FF_CODE` (`src/FF_Primitives.cpp:304`) agrees with what
      `ENCODE_FF_CODE` writes — it currently sizes a dictionary miss as a plain
      `FF_STRING` while the encoder writes an `FF_CODEABLE_CONCEPT` block.
      Different layouts, same allocation.
- [ ] A8.3 Round-trip test per system: ingest → export → compare.
- Verify: `ctest --test-dir build -R cpp_test_9 --output-on-failure`.

### A9. `insert_at_field` rejected `telecom` (cpp_test_5) — FIXED

**Not a missing feature. The guard was reading a flag that lied.**
`insert_at_field_json` refused any array whose `FF_FieldKey::array_entries_are_offsets`
was non-zero. That flag comes from `structure.py:_array_entries_are_offsets_expr`,
which returns `true` for every block-typed child and `false` for string/code —
exactly inverted from what `emit/store.py` actually writes: block children are
`FF_ARRAY::INLINE_BLOCK`, and string/code are the only `FF_ARRAY::OFFSET` arrays.
So `telecom` (ContactPoint) was flagged as an offset array and rejected.

The guard was also unnecessary. The ArrayField path never touches individual
entries: the generated `*_from_json` writes the whole array block and only its
offset is patched into the parent slot. Element layout is re-derived from the
wire by every reader (`FF_ARRAY::entries_are_pointers`, consumed in
`ParserOps::standard_entry_as_node` — which overrides the schema flag, and is why
the read path was always correct despite the inverted value).

- [x] A9.1 Guard deleted; `insert_at_field` now accepts any FF_FIELD_ARRAY target.
- [ ] A9.2 Follow-up: `array_entries_are_offsets` is now inert everywhere —
      `standard_node_lookup_field` drops it and `as_node()` re-reads the wire, so
      the parser (`FF_Parser.cpp:372,589`) and compactor (`FF_Compactor.cpp:180`)
      pass a value nothing consumes. It is a wrong second source of truth for
      something `FF_ARRAY::entry_kind()` already states on the wire. Remove it from
      `FF_FieldInfo`/`FF_FieldKey` and from `emit/views.py`.

### A10. Code fields round-tripped as garbage — FIXED

**Root cause was a dangling `std::string_view`, not the CodeableConcept path.**
`generator/emit/ingest_mappings.py` emitted, for every `code` field without a
bound enum:

```cpp
data.code = std::string(c);   // field is std::string_view
```

The temporary `std::string` dies at the end of the statement, leaving the view
pointing at freed memory. The store pass then read whatever was there. Traced by
instrumenting both sides: the offsets were correct all along
(`cc_off=504 block_off=419` matching on encode and decode), but `ENCODE_FF_CODE`
received `'xIG'` instead of `'8867-4'` — the corruption was already in the POD.

Sibling fields were never affected because `string`/`uri` fields assign the view
directly (`data.system = s;`). Only the `code` branch wrapped it in a temporary.
Two emitter sites (scalar at ~line 329, array at ~line 243); the `code_enum`
variants were always safe because the temporary is consumed producing an enum.

The ledger reset did not cause this — it exposed it, by moving LOINC and other
external codes off the dictionary fast path onto the branch that reads the POD.

- [x] A10.1 Both emitter sites now assign the view directly.
- Result: `cpp_test_9` passes; `py_roundtrip` passes; OMB race codes
  (`2106-3`, `2186-5`) and LOINC (`8867-4`) round-trip exactly.

### A13. CPT and CVX payloads widened (Q12) — DONE

**Was:** CPT stored in 2 bytes and CVX in 1, against real ranges of 00100–99499
and up to ~320. `99213` became `33677`; `300` became `44`. Silent, and latent
only because A8 keeps every code on the UNKNOWN branch.

**Q12 (Ryan): widen, keep fixed-width numeric.** CPT is now 4 bytes, CVX 2.
Free to do because no CodeableConcept block has ever been written with those
systems; it would not have been once A8 landed.

- [x] A13.1 CPT 2 -> 4 bytes, CVX 1 -> 2 bytes.
- [x] A13.2 Done before A8, per the sequencing note.
- [x] A13.3 Root cause removed: encode and decode were two independent
      per-system switches, so the widening needed both edited and the decode
      side still carried a `char buf[4]` sized for the old `uint8` CVX --
      enough to render 65535 as "655". Both now drive from one `FF_CC_CODECS`
      table in `src/FF_Primitives.cpp`, and `Entry::print_scalar_json` calls
      the shared decoder instead of carrying a third switch that handled only
      4 of the 17 systems.
- Verify: `ctest --test-dir build -R cpp_ff_test_cc --output-on-failure`
  (104 assertions: per-system round-trip, header bytes, cursor advance,
  out-of-range rejection, and a row-exists check over every enum value).

### A12. `Reference.reference` truncates and corrupts on export

**Context:** one Synthea fixture still round-trips to invalid UTF-8:

```
expected : "reference":"urn:uuid:13472219-c176-990a-641f-14cf9d4d8480"
actual   : "reference":"urn:uuid:13472219-c1<garbage>"
```

Distinct from A10 — `Reference.reference` already assigns its view directly
(`data.reference = s;`), so the dangling-temporary fix does not apply.
Reproduces on the single-resource path, so it is not bundle-specific.
8 of 9 Synthea fixtures round-trip cleanly; this is the ninth.

**Hypothesis, unproven:** simdjson ondemand's `get_string()` returns a view into
the parser's internal string buffer, which is reused as the parser advances.
A view captured from a nested sub-object (`Reference_from_json(sub.value_unsafe(),
...)`) may therefore dangle by the time the store pass runs. The truncation
pattern — correct prefix, garbage tail — is consistent with buffer reuse. Verify
before acting on it.

- [ ] A12.1 Confirm or refute the simdjson buffer-reuse hypothesis: log the
      `string_view` data pointer at ingest and again at store, and see whether
      it still points inside the live document buffer.
- [ ] A12.2 If confirmed, this affects every `std::string_view` field reached
      through a nested object, not just `Reference.reference` — audit the whole
      zero-copy view strategy against simdjson ondemand's buffer lifetime.
- Repro: `./build/ff_roundtrip <the AllergyIntolerance entry>` — see
  `cpp_test_5`'s fixture set.

### A11. Shared-prefix extension URL not reconstructed (cpp_test_11) — FIXED

**Root cause was a dangling `std::string_view`, not the prefix scheme.** The trie,
the prior chain and `get_url()` were all correct; they were fed garbage.

`collect_extension_urls_pipeline` captured the URL with simdjson **ondemand**
`get_string()`, which unescapes into the *parser's* internal string buffer. That
buffer is reused by the very next string parsed from the document — including the
`unescaped_key()` calls in the remaining iterations of the same loop, before
`push_url()` is even reached — and is destroyed when `scan_chunk_producer()`
returns, well before the consumer thread reads the batch.

Every URL therefore arrived blank at the correct length. With no `/` in the view,
`insert_url_to_trie` split nothing, so the directory held one junk row per URL
(`prior=-1`, segment = a run of spaces) instead of a shared-prefix chain.

Fixed by taking the URL from `raw_json_token()`, which points into the chunk the
caller owns for the whole predigest call, with the JSON quotes stripped and
backslash-escaped URLs skipped (no zero-copy source representation). The directory
now holds 8 entries for the fixture, with `alpha` and `beta` as siblings under one
shared `shared-prefix` parent.

Same defect class as A10, and the mechanism A12 hypothesised — now proven here.

- [x] A11.1 Traced; root cause was the cross-thread view, not prefix storage.
- [x] A11.2 `cpp_test_11` extended with structural assertions: alpha and beta must
      be distinct leaves sharing one parent entry, so storing whole URLs per entry
      can no longer pass by satisfying `get_url()` alone.

### A13. simdjson reads now always use a padded buffer — FIXED

`ingest_fhir_json`'s root routing parse called
`parser.iterate(data, size, size + SIMDJSON_PADDING)`, asserting 64 readable bytes
past the *caller's* `string_view` — padding the library never owned. simdjson reads
up to `SIMDJSON_PADDING` past the logical end, so a caller buffer ending near a page
boundary was an out-of-bounds read. The bundle splitter then made a second padded
copy of the same bytes.

Fixed **without adding a copy**. `simdjson::padded_string` always allocates and
memcpy's (`allocate_padded_buffer` + `memcpy`); `simdjson::padded_string_view` is the
zero-copy form — a `string_view` plus a capacity, i.e. a promise about the caller's
buffer. `IngestRequest::payload_capacity` lets the caller make that promise, and the
payload is then parsed in place. Left at 0 it falls back to one padded copy, which is
logged at Info so the slow path is findable. `ff_ingest` already held a
`simdjson::padded_string`, so it now declares capacity and copies nothing.

- [x] A13.1 Root parse and splitter share one payload view.
- [x] A13.2 `IngestRequest::payload_capacity` added; zero-copy when set, logged copy
      when not. Covered by `ff_test_bundle`, which ingests the same bundle down both
      paths and asserts the streams agree.
- [ ] A13.3 Remaining copy: `build_bundle_entry_chunks` still copies every bundle
      entry into its own `padded_string`. Each entry lies inside the padded payload,
      so every entry already has ≥ SIMDJSON_PADDING readable bytes after it and the
      chunk vector could hold `padded_string_view`s instead — removing N copies per
      bundle. Deliberately not done in the same change as A13.2: that vector is
      consumed by the worker path implicated in A14, and changing its lifetimes while
      a live memory-corruption bug sits there would confuse the diagnosis.

### A14. Intermittent worker-thread crash during bundle ingest — OPEN

Found while diagnosing A11; **pre-existing** (reproduced before the A11 fix) and
**not** fixed by A13. A minimal valid 2-entry bundle crashes ingest on roughly
two thirds of runs. Not triggered by any current test fixture, so the suite is
green — this needs a dedicated reproducer.

Evidence (lldb, `EXC_BAD_ACCESS`, several runs):
- worker threads inside `ingest_fhir_json`'s lambda fault at address `0xffffffff`
  — a `FF_NULL_UINT32`/`TRIE_NULL` sentinel being used as an address or index;
- the main thread faults freeing `simdjson::internal::dom_parser_implementation`
  at `0x656372756f7365ba` — a live heap pointer overwritten with the ASCII bytes
  `"esource"` from `"resource"` in the payload, i.e. JSON text written over an
  unrelated heap allocation.

- [ ] A14.1 Build a standalone reproducer and run it under ASan/TSan.
- [ ] A14.2 Audit `claim_space()` failure handling on the worker path: a
      `FF_NULL_OFFSET` return used as an offset would match the `0xffffffff` fault.

---

### A15. Re-arm the two vacuous sections of the wire gate

**Context:** `tests/generator/test_wire_format.py` calls itself "the ONE hard gate", but two
of its three sections compare an empty dict against an empty dict and pass unconditionally:

```bash
python3 -c "import json;print({k:len(v) for k,v in json.load(open('tests/generator/golden/wire_witness.json')).items()})"
# {'codes': 0, 'tags': 0, 'vtables': 141}
```

Two independent structural reasons, both verified:

1. `witness()` (`tests/generator/wire_witness.py:99`) scans only
   `generated_dir.rglob("*.hpp")`. Tag definitions exist in exactly one file in the repo —
   `include/FF_Recovery.hpp` — which is not under `generated_src/`
   (`grep -rl "RECOVER_FF_[A-Z_]* *= *0x" --include='*.hpp' .` returns that file alone).
2. The `_CODE` regex (`wire_witness.py:34`) matches `FF_[A-Z0-9]+_CODE_[A-Z0-9_]+\s*=\s*0x…`
   and its docstring gives `FF_R5_CODE_PERCENT = 0x1CF1F3BB`, "hash-based uint32". Nothing
   in the repo is named or valued that way any more: codes are emitted as
   `FF_CODE_DEF PERCENT = 2;` inside `namespace FastFHIR::FF_CODE`
   (`dictionaries/FF_Codes.hpp:13-19`) — scoped names, sequential ledger IDs. The regex
   encodes a superseded design, and an empty match set is indistinguishable from a pass.

**What is and is not covered elsewhere.** Code IDs are genuinely gated by
`tests/generator/test_code_ids.py` against the committed ledger. Recovery tags are **not**:
`test_recovery_tags.py` checks that emitted tag *names* resolve, that the check is wired
into the pipeline, and that no field falls through to `FF_RECOVER_UNDEFINED` — it never
compares a *value*. The only value pinning anywhere is four hand-written assertions at
`tests/cpp/test_primitives.cpp:195-198` (`EXTENSION`, `PATIENT`, `OBSERVATION`, `BUNDLE`).
`include/FF_Recovery.hpp` declares 168 enumerators. **164 permanent wire values are
unguarded** — editing one is caught by code review alone.

- [ ] A15.1 Extend `witness()` to read tag values from `include/FF_Recovery.hpp` and code
  values from `dictionaries/FF_Codes.hpp`, in addition to the generated tree. Both are
  committed, so the witness stops depending on a network regeneration for those sections.
  Signature change: pass the repo root (or both explicit paths) alongside `generated_dir`;
  update the two call sites (`test_wire_format.py`, the `__main__` block) and the module
  docstring, which currently describes only the generated tree.
- [ ] A15.2 Replace the `_CODE` regex with one matching the real emission —
  `FF_CODE_DEF <NAME> = <int>;` qualified by its enclosing `namespace` so
  `FF_CODE::UCUM::MMHG` and `FF_CODE::FHIR::…::MALE` are distinct keys. Values are decimal,
  not hex. Fix the docstring example in the same edit.
- [ ] A15.3 Add `test_witness_sections_are_non_empty` asserting every section of a freshly
  built witness has entries, with a message naming the regex that stopped matching. This is
  the check whose absence let A4.1 be marked done against an empty golden; without it, any
  future emitter rename silently re-empties a section.
- [ ] A15.4 Regenerate the golden and commit it with this change (per `CLAUDE.md`, a golden
  update needs a corresponding generator or test change in the same commit — A15.1/A15.2
  are that change). The diff must be **additions only**: `vtables` unchanged, `tags` and
  `codes` populated from nothing.
- [ ] A15.5 While in this file: the permanence error at `wire_witness.py:181` tells the user
  to "use `--force` to override", but `__main__` (`:197`) takes exactly two positional
  arguments and `grep -rn '\-\-force' generator tests` finds only that message. Either
  implement the flag or rewrite the message to describe the real procedure (documented in
  `CLAUDE.md` → Build & test).
- [ ] A15.6 While in this file: `_OFFSET_FIELD` (`wire_witness.py:45`) requires a line
  ending in `,` or end-of-line, so a vtable entry carrying a trailing `// comment` would
  drop out of the captured field `order` and the gate would still pass on a shorter list.
  Current output is safe — `generator/model/merge.py:301` emits no trailing comment — so
  either tolerate comments in the regex or add a comment at that emitter line stating that
  adding one is a wire-gate change.
- Locate: `python3 -c "import json;print({k:len(v) for k,v in json.load(open('tests/generator/golden/wire_witness.json')).items()})"`
  — if `tags` or `codes` is already non-zero, STOP; someone has done this.
- Acceptance: all three sections of the golden non-empty; changing one digit of any tag
  value in `include/FF_Recovery.hpp` fails `pytest tests/generator/test_wire_format.py`
  (verify by doing it, per the red-green rule this task exists to restore); reverting the
  edit makes it pass again.
- Verify: `pytest tests/generator -q -rs` — no `SKIPPED` for `test_wire_format.py`.

### A16. Assert the shipped prefix, not the current total, in the vtable gate

**Context:** `test_vtable_layout_stable` (`tests/generator/test_wire_format.py:57`) asserts
`set(current) == set(expected)`, then per block `current[block]["order"] ==
expected[block]["order"]`. Adding a new resource block fails the first; appending a field to
an existing block fails the second. Both are **legal, expected changes** — FastFHIR models 28
of ~145 R4 resources, so growth is the normal case.

`_check_permanence` (`tests/generator/wire_witness.py:111`) already implements the correct
rule (changes and deletions rejected, additions allowed), and
`test_permanence_accepts_addition` (`:93`) explicitly asserts additions are legal. The two
halves of one file disagree about what the rule is. A gate that fires on every legitimate
addition gets its golden regenerated reflexively — which is exactly the "golden update
without a corresponding change" that `CLAUDE.md` calls a red flag. The gate as written
trains the reviewer to ignore it.

- [ ] A16.1 Rewrite `test_vtable_layout_stable` to assert the shipped **prefix**: every
  golden block still present (new blocks allowed); every golden field present, in golden
  order, at the head of `current[block]["order"]` (appended fields allowed); every golden
  entry in `sizes` unchanged (new entries allowed); `header_sizes` unchanged for every
  version already in the golden. Prefer delegating to `_check_permanence` over
  re-implementing — the duplication is what let the two rules diverge.
- [ ] A16.2 Add the negative cases as tests, each verified by making the change on a copy of
  the witness dict: reordering two fields fails; removing a field fails; changing a
  `size_const` fails; **appending a field passes**; **adding a block passes**.
- Acceptance: the five cases in A16.2 behave as stated; `pytest tests/generator -q` green.
- Verify: `pytest tests/generator/test_wire_format.py -q`.

### A17. Pin the R4-prefix invariant the version contract depends on

**Context:** `architecture.md:118` specifies that a reader compiled against R5 reads an R4
stream "by clamping access to the smaller header". That is sound only if every R4 field sits
below `HEADER_R4_SIZE` — i.e. R5-only fields are strictly appended. 43 of 141 blocks have
differing R4/R5 header sizes:

```bash
python3 -c "import json;d=json.load(open('tests/generator/golden/wire_witness.json'))['vtables'];print(len([k for k,v in d.items() if len(set(v['header_sizes'].values()))>1]))"
# 43
```

The property holds today **only as a side effect of iteration order**.
`merge_fhir_versions` (`generator/model/merge.py:57`) walks `schemas_by_version` in list
order and lays out each field on first sight (`if field_name not in blk["seen"]:`,
`merge.py:77`). That order comes from `generator/library.py:59` iterating `versions`, which
is `versions = ["R4", "R5"]` — a bare list literal at `generator/pipeline.py:42` with
nothing marking it as load-bearing. Reorder it, insert R6 ahead of R5, or parallelise the
version loop, and every R4 field offset in those 43 blocks moves. The failure is silent,
total, and unfixable once streams exist.

- [ ] A17.1 Add a comment at `generator/pipeline.py:42` stating that the order of `versions`
  is a wire invariant, not a preference: earlier revisions must be laid out first so that
  each revision's field set is a prefix of the next. Name the test from A17.2 in the comment.
- [ ] A17.2 Add `tests/generator/test_version_prefix.py`: for every block where
  `HEADER_R4_SIZE != HEADER_R5_SIZE`, assert every field at an offset below
  `HEADER_R4_SIZE` was introduced in R4. `merge.py` already records `first_version_idx` and
  `first_version_name` per field, so drive the test from the model rather than re-parsing
  the emitted C++ (the witness deliberately captures no literal offsets). Assert the block
  count is non-zero so the test cannot silently become vacuous.
- [ ] A17.3 Red-green it: temporarily set `versions = ["R5", "R4"]`, confirm the new test
  fails, revert. Note the result in the commit message.
- Acceptance: A17.2 passes on the current tree, fails under the A17.3 mutation.
- Verify: `pytest tests/generator/test_version_prefix.py -q`.

### A18. Fail loudly when R4 and R5 disagree about a field

**Context:** `generator/model/merge.py:77` — `if field_name not in blk["seen"]:` guards the
entire field-entry construction, including `is_array` (`el.get("max") == "*"`), `fhir_type`,
and the resulting `size` / `size_const` / `cpp_type`. On a repeat sighting the loop falls
through to the running-total update at `merge.py:139`. There is no comparison against the
stored entry and no diagnostic. So a field that is `0..1` in R4 and `0..*` in R5 is laid out
with the R4 scalar mapping and cannot hold the R5 value; a retyped field keeps the R4
mapping. The generator, the emitted C++, the Python bindings and the docs then all agree on
a representation that cannot hold the data — consistently, and therefore invisibly. This
also violates `CLAUDE.md` invariant 3 (`raise` over silent fallback).

Whether such a field exists in 4.0.1 vs 5.0.0 is not currently determinable — it needs a
generator run with network. The check answers it; that is the point of adding it.

- [ ] A18.1 In `merge_fhir_versions`, on a repeat sighting of a field name, compare
  `is_array` and the sanitized `fhir_type` against the stored entry. On divergence
  `raise RuntimeError` naming the block path, field name, both revisions and both values.
  Do not attempt to reconcile automatically — a real divergence needs a deliberate decision.
- [ ] A18.2 Run `python -m generator` (needs network) and record the outcome here as a note
  under this task: either "no divergence in 4.0.1 vs 5.0.0" or the list of offending fields.
- [ ] A18.3 If A18.2 finds divergences, do NOT widen layouts in this task — file one Block A
  task per divergent field with the R4 and R5 shapes, since each may need its own decision
  (widen to the R5 shape, or an explicit `BLOCK_FIELD_OVERRIDES` entry). Widening a field
  that has already shipped is a wire change and needs the A16 gate to review it.
- Acceptance: the guard exists and the generator either runs clean or fails with a message
  naming a specific field; A18.2's note is written.
- Verify: `python -m generator --output-dir /tmp/ff_gen && pytest tests/generator -q`.

---

## Block B — Test coverage

All new C++ tests copy the harness pattern from `tests/cpp/test_primitives.cpp`: a
self-contained `main()`, the `TEST_GROUP` / `CHECK` / `CHECK_EQ` macros (copy them
verbatim from that file's top), exit code = number of failures. Register each new
executable in `CMakeLists.txt` next to the existing ones using the `add_ff_cpp_test`
helper (search for `add_ff_cpp_test(ff_test_primitives` and mirror it), which also creates
the `cpp_<name>` ctest entry.

### B1. Builder unit tests — new file `tests/cpp/test_builder.cpp`

Write these test groups (each is one `TEST_GROUP`). Construct with
`auto mem = FastFHIR::Memory::create(); FastFHIR::Builder builder(mem, FHIR_VERSION_R5);`.

- [ ] B1.1 **claim/append basics:** `builder.append(PatientData{})` returns an offset
  `>= FF_HEADER::HEADER_SIZE`; a second append returns a strictly larger offset;
  `mem.size()` grows monotonically.
- [ ] B1.2 **amend_pointer contract** (see `src/FF_Builder.cpp` `Builder::amend_pointer`):
  (a) amending an unassigned slot succeeds and the stored u64 reads back;
  (b) amending the SAME slot again throws `std::runtime_error` whose message contains
  `"already assigned"`;
  (c) out-of-bounds `object_offset` throws with message containing
  `"Pointer amendment out of bounds"`.
- [ ] B1.3 **amend_resource / amend_variant contracts:** same three cases; additionally
  assert `amend_resource` writes the 2-byte tag at `offset + DATA_BLOCK::RECOVERY` and
  `amend_variant` stores the raw 8-byte payload + tag (read back via `LOAD_U64`/`LOAD_U16`).
- [ ] B1.4 **finalize gates:** after `builder.finalize(...)` begins, further `amend_*` and
  `set_root` throw with message containing `"finalizing"`; `set_root` on a handle with
  UNDEFINED recovery throws `std::invalid_argument`.
- [ ] B1.5 **hydration round-trip:** build+finalize a Patient into a `Memory`, then
  construct a SECOND `Builder(mem, FHIR_VERSION_R5)` on the same memory; assert
  `builder2.root_handle()` is truthy and its recovery tag equals `RECOVER_FF_PATIENT`;
  assert constructing a Builder on a COMPACT archive (make one via
  `Compactor::archive`) throws with message containing `"compact archive"`.
- [ ] B1.6 **checksum finalization:** `finalize(FF_CHECKSUM_SHA256, hasher)` with a
  stub hasher returning 32 fixed bytes; re-parse and assert
  `parser.checksum().expected_checksum` equals those bytes.
- [ ] B1.7 **concurrent append:** 8 threads × 1000 `append_obj(ObservationData{})` into one
  Builder; join; assert all returned offsets are unique and non-overlapping
  (sort offsets, assert each `offset[i] + size <= offset[i+1]`), and no crash/tear.
  Model on the existing 8-thread test in `tests/cpp/test_memory.cpp` (`concurrent claim`).
- [ ] B1.8 Register as `ff_test_builder` in CMakeLists.txt.
- Verify: `ctest --test-dir build -R cpp_ff_test_builder --output-on-failure`.

### B2. Parser unit tests — new file `tests/cpp/test_parser.cpp`

Fixture: build one Patient (id, gender, active, two names) + one Observation with
`valueQuantity` in `SetUp`-style helper, finalize, then parse.

- [ ] B2.1 **navigation:** `root()[Fields::PATIENT::ID]` truthy;
  string-key lookup `root()["id"]` yields the same bytes; absent field
  (`Fields::PATIENT::DECEASED` unset) is falsy and does NOT throw.
- [ ] B2.2 **typed extraction:** `as<std::string_view>()`, `as<bool>()`, implicit
  `operator std::string_view`; wrong-type extraction (e.g. `as<bool>()` on a string node)
  behavior — assert whatever the contract is (check `Node::as_scalar` in
  `include/FF_Parser.hpp:230` — it validates against an expected RECOVERY_TAG) and
  document it in the test comment.
- [ ] B2.3 **arrays:** `entries()` count matches what was built; per-entry field reads;
  iteration order matches insertion order.
- [ ] B2.4 **choice resolution:** `root()[Fields::OBSERVATION::VALUE]` on the
  valueQuantity fixture → `kind()` is `FF_FIELD_BLOCK`, materializes to `QuantityData`;
  rebuild with `valueBoolean` → `kind()` is `FF_FIELD_BOOL`, `as<bool>()` correct.
- [ ] B2.5 **typed root checks:** `parser.is_root<RESOURCETYPE::PATIENT>()` true,
  `is_root<RESOURCETYPE::OBSERVATION>()` false; `root_resource_type()`.
- [ ] B2.6 **metadata:** `has_url_directory()` false on a plain stream; ingest a resource
  with an unknown extension (through `Ingest::Ingestor`) and assert it flips true and
  `url_directory().entry_count(...)` ≥ 1. (Requires `FASTFHIR_BUILD_INGESTOR=ON` — guard
  with the same `#ifdef` the readme test uses, or make it its own group.)
- [ ] B2.7 **print_json:** `parser.print_json(oss)` output parses as JSON and contains the
  fixture's id/gender values (plain substring checks are acceptable here; full DOM parity
  is B5's job).
- [ ] B2.8 Register as `ff_test_parser`.
- Verify: `ctest --test-dir build -R cpp_ff_test_parser --output-on-failure`.

### B3. Golden pipeline integration test — new file `tests/cpp/test_pipeline.cpp`

**Context:** exercises the full chain on a FIXED input with byte-level assertions.
Distinct from `tests/cpp/ff_roundtrip.cpp`, which is a passive harness binary driven by
Python. Use a small hand-written Patient+Observation bundle JSON embedded as a raw string
literal in the test (do NOT depend on Synthea here — that's B5; this test must be
hermetic).

- [ ] B3.1 Stage 1: ingest the embedded JSON via `Ingest::Ingestor` → finalize → assert
  parse succeeds, root is a Bundle, entry count is exact.
- [ ] B3.2 Stage 2: field-by-field walk asserting every value in the embedded JSON is
  reachable via typed keys.
- [ ] B3.3 Stage 3: `print_json` → re-ingest the OUTPUT → walk again; assert the same
  values (semantic round-trip without byte-golden fragility).
- [ ] B3.4 Stage 4: `Compactor::archive` → parse compacted stream → assert identical field
  values through the same walk, and compacted size < standard size.
- [ ] B3.5 Register as `ff_test_pipeline` (requires `FASTFHIR_BUILD_INGESTOR=ON`; wrap the
  CMake registration in the existing `if(FASTFHIR_BUILD_INGESTOR)` block).
- Verify: `ctest --test-dir build -R cpp_ff_test_pipeline --output-on-failure`.

### B4. Pin Synthea fixtures (reproducibility)

**Context:** `CMakeLists.txt` downloads
`.../downloads/latest/synthea_sample_data_fhir_latest.zip` (search for `synthea` in
CMakeLists.txt, currently ~line 313). "latest" is unpinned: upstream changes can silently
alter test inputs, making regressions indistinguishable from fixture drift.

- [ ] B4.1 Download the current zip once, record `sha256sum` of it.
- [ ] B4.2 Change the `file(DOWNLOAD ...)` call to a versioned URL if upstream offers one;
  if only `latest` exists, keep the URL but add
  `EXPECTED_HASH SHA256=<recorded-hash>` to the `file(DOWNLOAD)` call so a silent upstream
  change fails configure loudly instead of silently changing inputs.
- [ ] B4.3 Add a comment above the download recording the date and hash provenance, and
  update `tests/python/test_roundtrip.py`'s module docstring to state the pinned hash.
- Acceptance: reconfiguring from a clean build dir succeeds; tampering one byte of a
  cached zip and reconfiguring fails with a hash mismatch.
- Verify: `rm -rf build && cmake -S . -B build -DFASTFHIR_BUILD_TESTS=ON -DFASTFHIR_BUILD_INGESTOR=ON && ctest --test-dir build -R py_roundtrip`.

### B5. Round-trip DOM parity triage `Blocked on Q3`

**Context:** the Python DOM-diff infrastructure already exists
(`tests/python/roundtrip_diff.py` produces `DiffEntry(path, kind, expected, actual)`
records; `tests/python/test_roundtrip.py` drives the `ff_roundtrip` C++ harness over
Synthea fixtures). What's missing is the triage: classifying every reported difference.
The comparison policy (carried from the deleted `integration_revision.todo.md`):

| Difference | Policy |
|---|---|
| Key order / whitespace / trailing decimal zeros | Accept silently |
| Empty array `[]` in → absent out | FLAG (pending Q3) |
| `null` vs absent | FLAG (pending Q3) |
| Extra fields in output | WARN (phantom data) |
| Missing fields in output | WARN (potential loss) |
| `"0"` vs `0` type mismatch | FLAG |
| Code/value mismatch | FLAG |

- [ ] B5.1 Run `ctest --test-dir build -R py_roundtrip` (after B4) and capture the full
  DiffEntry list to a file.
- [ ] B5.2 For each distinct diff class, write one row in a new triage table at the bottom
  of this file (section "B5 triage results"): JSON path pattern, class
  (legitimate / bug / generator gap), rationale, disposition. The six known question areas
  to specifically check: empty arrays, UUID `id` text preservation, extensions
  (race/ethnicity/birthplace), `contained` resources, `Resource.text` narrative,
  CodeableConcept `text`+`coding` both preserved.
- [ ] B5.3 Legitimate differences → add to an explicit allow-list structure in
  `roundtrip_diff.py` (a module-level `ALLOWED_DIFFS: list[AllowRule]` with a comment per
  rule). Bugs → file one new task per bug under Block A in this file with a minimal repro.
  Generator gaps → add an `xfail` test naming the gap.
- [ ] B5.4 (After triage stabilizes) Port the diff to C++ with simdjson so CI needs no
  Python for this gate: `recursive_diff(simdjson::dom::element, simdjson::dom::element)`
  in a new `tests/cpp/test_dom_parity.cpp`, same allow-list semantics.
- Acceptance: `py_roundtrip` green with zero un-triaged diffs; every allow-rule has a
  written rationale.

### B6. Recovery/corruption tests (write BEFORE Block C implementations; expect xfail)

New file `tests/cpp/test_recovery.cpp`. Build a valid sealed Patient stream in memory,
then deliberately corrupt copies of it:

- [ ] B6.1 Corrupt header magic (byte 0) → `Parser` constructor must throw; `Builder`
  constructor must treat it as fresh memory (documented current behavior) — assert both.
- [ ] B6.2 Corrupt ROOT_OFFSET (bytes 16–23 per the header layout at
  `include/FF_Primitives.hpp:540`) to point past `size()` → parsing must fail cleanly
  (no crash/UB under ASAN), reads return falsy.
- [ ] B6.3 Corrupt a block's RECOVERY tag → typed access (`is_root<...>`, struct
  materialization) must fail cleanly, not misread.
- [ ] B6.4 Corrupt checksum footer bytes → `parser.checksum()` validation reports mismatch.
- [ ] B6.5 Once C1/C2 land: strict-fail vs attempt-repair vs read-only-salvage behavior
  per policy; in-place enrich succeeds after a successful repair without JSON re-ingest.
  Until then mark these cases `// XFAIL(C1)` and skip them at runtime with a printed note.
- [ ] B6.6 Register as `ff_test_recovery`; run it under ASAN locally at least once
  (`-DCMAKE_CXX_FLAGS=-fsanitize=address`).
- Verify: `ctest --test-dir build -R cpp_ff_test_recovery`.

### B7. Test against bytes, not against another description

**Context:** every gate in this repo compares a *description* to a *description*. The wire
witness compares generated C++ to a JSON summary of generated C++. `test_cross_language_
constants.py` compares Python's `TYPE_MAP` to a C++ header. `test_code_ids.py` compares the
ledger to the emitted string table. None would catch a correct-offset/wrong-load error — a
field at the right place read at the wrong width, or an enum cast from the wrong type — and
none proves that a stream written last year still parses today. The only check that does is
bytes produced by a shipped encoder, read back through the current reader. (`LessonsFromIFE.md`
C6 lists this as IFE's own standing gap, so it is a shared one; A4.3's compile smoke test is
the nearest existing relative and is complementary, not a substitute.)

- [ ] B7.1 Produce one small sealed `.ffhr` from the current builder: a Patient plus an
  Observation, exercising at least one of each field kind that has a distinct on-wire
  representation — inline scalar, offset block, `FF_STRING`, dictionary code, a
  `FF_CODEABLE_CONCEPT` block with `FF_CODEABLE_CONCEPT_FLAG` set, a choice slot, an array,
  and an extension. Keep it under a few KiB.
- [ ] B7.2 Commit it as `tests/cpp/fixtures/wire_v1.ffhr` **plus** a sibling
  `wire_v1.expected.json` recording the values a reader must recover. Record in a README
  next to them: the engine version, the FHIR revision, the date, and the commit that wrote
  the fixture. This file is a permanent artifact — it is never regenerated to make a test
  pass. If it stops parsing, that is the finding.
- [ ] B7.3 New `tests/cpp/test_wire_fixture.cpp`: mmap the fixture, walk it with the Node
  API, assert every value in the expected JSON. Assert `FF_HEADER` magic, revision and
  engine version explicitly. Register as `ff_test_wire_fixture` (hermetic — no ingestor, no
  network, so it must run in every configuration).
- [ ] B7.4 Add a second fixture written by the compactor (`FF_STREAM_LAYOUT_COMPACT`) and
  assert the same values through the same walk — the compact read path had a real
  wrong-code bug (A7) that a description-to-description gate could not have caught.
- Acceptance: both fixtures parse and match; `git log` shows the fixture bytes have never
  been rewritten; the test fails if `FF_Ops.hpp`'s `LOAD_U32` is mutated to `LOAD_U16`
  (verify by doing it, then revert).
- Verify: `ctest --test-dir build -R cpp_ff_test_wire_fixture --output-on-failure`.

---

## Block C — Archive recovery subsystem

**Reality check (verified 2026-07-06):** the old progress doc claimed
`**RECOVERY_GATE**`/`**RECOVERY_REQUIRED**` markers exist in the code — **they do not**
(`grep -rn 'RECOVERY_GATE\|RECOVERY_REQUIRED' src/ include/ python/` is empty). The actual
"gates" today are plain throw/degrade sites:
- `src/FF_Builder.cpp` constructor (~lines 33–100): hydrates root metadata from an existing
  archive inside a `try { Parser p(m_memory); ... } catch (...) { /* treat as new */ }`,
  and throws on compact archives (`"Cannot open Builder on a compact archive"` at ~line 85).
- `Builder::finalize` preflight and the `amend_*` guards
  (`"already assigned"`, `"out of bounds"`, `"finalizing"` throws).

Order matters: C1 → C2 → C3…C8. `Blocked on Q1` for C1.

- [ ] C1. **`recover_archive(...)` orchestrator** `Unblocked` (Q1 answered: copy-swap per element)
  Add `FF_RecoveryReport recover_archive(Memory&, FF_RecoveryPolicy)` (free function or
  Builder static — decide and document) in a new `src/FF_Recovery.cpp` +
  declaration in `include/FF_Recovery.hpp` (which today holds only tags/constants — keep
  the wire-constant section untouched at the top, add an API section below).
  It must be the single path that decides recover-or-fail when `Builder`'s constructor
  catch-block fires on memory that `looks_like_fastfhir_header()` says was once a stream
  (see `src/FF_Memory.cpp:194`). Establish the error-marker convention here: every
  recovery-related exception message starts with `"FastFHIR RECOVERY_REQUIRED:"` so callers
  and the Python layer (C7) can detect it — this convention was planned but never landed.
- [ ] C2. **Policy surface:** `enum class FF_RecoveryPolicy { STRICT_FAIL, ATTEMPT_REPAIR,
  READ_ONLY_SALVAGE };` plus an optional policy argument on the `Builder` constructor
  (default `STRICT_FAIL` = today's behavior). Document each mode's contract in the header:
  STRICT_FAIL = no mutation, throw; ATTEMPT_REPAIR = per Q1's answer; READ_ONLY_SALVAGE =
  best-effort parse, guaranteed zero writes.
- [ ] C3. **Header repair helper:** given a buffer whose magic is valid but whose
  root/checksum offsets fail validation, attempt bounded reconstruction (scan for the root
  block by validating `DATA_BLOCK` headers within `size()`). Wire it into the two TODO
  sites at `src/FF_Memory.cpp:196` and `:297` ("If header magic/version/offsets are
  plausible, attempt bounded recovery before zeroing") — today those paths
  `memset(base, 0, HEADER_SIZE)` and lose the evidence; under STRICT_FAIL they must warn
  and NOT zero (zeroing is itself a mutation of a faulted stream).
- [ ] C4. **Root reconciliation:** when header root metadata is missing/mismatched but a
  unique plausible root block exists, ATTEMPT_REPAIR may re-point ROOT_OFFSET/ROOT_RECOVERY;
  ambiguity (0 or ≥2 candidates) → fail with a `RECOVERY_REQUIRED` message listing
  candidates. Hook: `Builder::root_handle()` null-return path and finalize preflight.
- [ ] C5. **Mixed-version guard:** Builder currently silently degrades `m_fhir_rev` to the
  archive's version on hydration (constructor, ~line 62). Keep the degrade (documented
  intent) but add an explicit throw with actionable text if a caller then attempts to
  append data tagged for a different FHIR version. Locate where version enters append
  paths before designing this — if version is only header-level, document that instead.
- [ ] C6. **Amend-path atomic CAS** `Unblocked` (Q9 answered): in `amend_pointer` /
  `amend_resource` / `amend_variant`, replace the non-atomic load/check/store with an
  atomic CAS. Use `std::atomic_ref<uint64_t>` with `compare_exchange` from `FF_NULL_OFFSET`
  to the new target offset. Remove the author-flagged NOTE at `src/FF_Builder.cpp:164`
  (`"NOTE: I don't like this. It's not concurrency protected"`). Concurrent enrichment is
  now a supported operation — document this in `include/FF_Builder.hpp` above the amend
  declarations. Also add pre-write target-block RECOVERY tag validation when policy is
  ATTEMPT_REPAIR (from C2).
- [ ] C7. **Python exception mapping:** in `python/FF_PythonBindings.cpp` (PyStream is
  defined at ~line 64), register a custom exception
  `fastfhir.RecoveryRequired` via `py::register_exception` /
  `py::register_exception_translator` that catches C++ exceptions whose `what()` starts
  with `"FastFHIR RECOVERY_REQUIRED:"`. Export it from `python/fastfhir/__init__.py`.
  Add a Python test that opens a corrupted buffer and asserts
  `pytest.raises(fastfhir.RecoveryRequired)`.
- [ ] C8. **Telemetry:** route every recovery attempt/outcome through `FF_Logger`
  (`include/FF_Logger.hpp`) with structured fields (policy, fault class, repaired: bool).
  No new logging framework.
- Verify (whole block): `ctest -R cpp_ff_test_recovery` — B6.5 cases un-xfailed and green.

---

## Block D — WASM extension subsystem

All in `src/FF_Extensions.cpp` / `include/FF_Extensions.hpp` unless noted. D0 gates D1–D3.
`Blocked on Q5` for D0–D3.

**Wire invariant (never violate):** two SHA-256 roles — `sha256(url)` is ONLY a disk
metadata filename (`meta/<url_hash_hex>.meta`, see `meta_path_for_url` at
`src/FF_Extensions.cpp:342`); `sha256(wasm_bytes)` is module identity, written to
`FF_MODULE_REGISTRY` entries (`REG_ENTRY_MODULE_HASH`, offset 24, 32 bytes) and naming the
cached binary (`<binary_hash_hex>.wasm`). Never use one where the other is expected.

- [ ] D0. **`FF_ExtensionRegistry` interface** `Blocked on Q5`: a small struct/class owning
  (a) configurable base URL (constructor arg or setter; the default may reference
  `https://registry.fastfhir.org` but must not be hardcoded inside fetch functions),
  (b) path template `{base}/v1/modules/{url_hash_hex}/latest` (confirm against Q5's
  answer), (c) an explicit error contract — decide throw vs `std::optional` empty vs
  cached-fallback for network failure and non-200, and write it in the header comment.
  Thread it through `resolve_or_fetch_module` (`src/FF_Extensions.cpp:448`).
- [ ] D1. **`http_get_manifest()`** — currently a stub at `src/FF_Extensions.cpp:408`
  returning nothing useful. Implement a real GET via D0's interface; parse the manifest to
  extract the latest binary hash.
- [ ] D2. **`http_get_wasm()`** — currently plain TCP to port 80
  (`src/FF_Extensions.cpp:393–406`; the comment at :393 admits it: "Uses plain TCP on port
  80 to a redirecting CDN; a proper implementation should use TLS"). Rewrite to download
  by content hash via D0. After download, verify `sha256(bytes) == requested hash` before
  caching or loading — a content-addressed fetch that doesn't verify the hash is a supply
  chain hole.
- [ ] D3. **TLS transport** for D1/D2. Use OpenSSL BIO (`BIO_new_ssl_connect`) — OpenSSL is
  already a dependency when the ingestor is enabled; guard the fetch path with the same
  CMake condition, and make registry fetch a no-op returning "unavailable" when built
  without OpenSSL.
- [ ] D4. **`FF_IsKnownExtension()` / `FF_IsNativeExtension()`** — implement as binary
  search over the sorted generated table in `generated_src/FF_KnownExtensions.hpp`
  (regenerate to inspect its shape; emitter: `generator/emit/extensions_known.py`). Called
  from the predigestion pass (`FF_PredigestExtensionURLs` — grep for it in `src/`).
- [ ] D5. **`FF_ExtensionFilterMode`** — `enum { FILTER_ALL_KNOWN, FILTER_NONE }` applied
  in the predigestion hot path; default `FILTER_ALL_KNOWN` (matches README → Extensions
  Condition 3 table). Setting must be per-ingest, not global mutable state.
- [ ] D6. **`Parser::unresolved_extensions()`** — return the list of URL-directory entries
  whose EXT_REF is URL_IDX (MSB=0), i.e. extensions that were retained but have no module;
  enables offline-fallback audits. Add to `include/FF_Parser.hpp` next to
  `url_directory()`.
- [ ] D7. **Path A ingest dispatch** — invoke registered module codecs
  (`FF_WasmExtensionSize` / `FF_WasmExtensionStore`) from the concurrent ingest workers
  when EXT_REF has MSB=1. Respect the sandbox: data crosses only via the staging
  ping-pong buffers already built in `FF_WasmExtensionHost`.
- [ ] D8. **Path B round-trip export** — during `print_json`, emit the stored raw JSON of
  passive (URL-retained) extensions verbatim from the `VALUE` ChoiceEntry, restoring the
  original `extension` array member. NOTE: README ("Condition 2") currently states the
  pipeline does NOT preserve full unknown-extension payloads — implementing D8 changes
  that; update the README paragraph in the same commit.
- [ ] D9. **End-to-end Synthea verification test** — ingest one Synthea bundle; assert:
  URL directory present and deduplicating prefixes, zero blocks written for known/filtered
  extensions, module-registry entries carry 32-byte binary hashes.
- [ ] D10. **Routing unit tests** — EXT_REF MSB=0/1 classification, `ff_ext_ref_is_module`
  / `ff_ext_ref_is_url` / `ff_ext_ref_index` predicates (`include/FF_Primitives.hpp`),
  AOT enqueue (`enqueue_resolve` at `src/FF_Extensions.cpp:622`), Path A/B round-trips.
- [ ] D11. **First real codec module** — geolocation extension via wasi-sdk, as the
  reference for module authors; check in source + build script under a new
  `examples/wasm_codecs/` directory, not the core build.
- [ ] D12. **Cache GC** — evict superseded `.wasm` files from the disk cache. MUST NOT
  evict a hash referenced by any loaded `FF_MODULE_REGISTRY` in a live Parser/Builder:
  design the refcount/generation-lock first, write it as a comment block, get it reviewed
  (flag in PR), then implement.
- Verify (block): new tests in D9/D10 green; `grep -n 'port 80\|plain TCP' src/FF_Extensions.cpp` → empty after D2/D3.

---

## Block E — Hygiene & infrastructure

- [ ] E1. **CI workflow** `Blocked on Q6` — none exists (`.github/` holds only `prompts/`).
  Template: mirror the `cmake-linux-CI.yml` / `cmake-macos-CI.yml` / `cmake-win64-CI.yml`
  structure from Ryan's [Iris-Codec](https://github.com/IrisDigitalPathology/Iris-Codec)
  repo (proven on the same kind of C++/CMake project). Per job: checkout, install deps
  (Linux: `libssl-dev`; mac: system; Windows: vcpkg openssl), configure with
  `-DFASTFHIR_BUILD_INGESTOR=ON -DFASTFHIR_BUILD_TESTS=ON`, build `build_all`, run
  `ctest --output-on-failure`, run `pytest tests/generator -q` and
  `ruff check generator tests/generator`. Cache `fhir_specs/` and the Synthea zip
  (`actions/cache`) — the generator needs network otherwise (see Q7). Portability lessons
  already paid for (from the deleted refactor history — MSVC rejects
  `std::vector<IncompleteType>`; cp1252 terminals crash on Unicode in print; no Perl on
  Windows runners): Windows job is the strictest and most valuable.
  - [ ] E1.1 **Big-endian leg** (add to the matrix, don't defer to a follow-up): one
        `s390x` job under qemu (`uraimo/run-on-arch-action` or a qemu-user container),
        building and running `ctest` only — no packaging. Rationale: the
        `requires_byteswap` branches at `include/FF_Ops.hpp:57`, `:63`, `:69`, `:80`, `:85`
        and `:89` execute on no machine anyone here owns, and they are load-bearing for the
        claim "FastFHIR is strictly Little-Endian on the wire" (`FF_Ops.hpp:28`). IFE's
        equivalent header states the consequence exactly (`src/IFE_Bytes.hpp:148`): the
        big-endian CI job is *the only thing testing that code*, and IFE shipped a wrong
        big-endian branch twice before it existed. B7's fixture is what makes this leg
        meaningful — a byte file written little-endian and read on a big-endian host.
- [ ] E2. **`.clang-format`** — derive from `include/FF_Primitives.hpp` as the reference
  (4-space indent, ~100+ col lines tolerated, aligned trailing comments, attached braces
  in functions). Add the file + a CI step that checks ONLY files touched by the PR
  (`git clang-format --diff`). Do NOT reformat the tree wholesale — generated and
  hand-tuned files must not churn.
- [ ] E3. **Switch-case exhaustiveness audit** — for each function below, either make the
  switch exhaustive over its enum with no `default:` (so `-Wswitch` flags new enumerators)
  or add an explicit `default:` that fails loudly (throw/assert), never silently returns
  null. History: a missing `FF_FIELD_CODE` branch in `Compactor::archive_node` and an
  `is_choice` misclassification were real silent-fall-through bugs of this class.
  One sub-commit per file:
  - [ ] E3.1 `src/FF_Parser.cpp`: `print_json` helpers, `standard_node_entries`,
        `node_lookup_field`, `standard_entry_as_node`.
  - [ ] E3.2 `src/FF_Compactor.cpp`: all helpers beyond `archive_node`.
  - [ ] E3.3 `include/FF_Parser.hpp`: `Node::as<T>()` dispatch.
  - [ ] E3.4 `include/FF_Utilities.hpp`: `FF_IsFieldEmpty`.
  - [ ] E3.5 `src/FF_Builder.cpp`: `MutableEntry::operator=` overload set.
  - Verify: build with `-Wswitch -Werror` (add to a local test configure, not committed
    globally) — zero warnings in the audited files.
- [ ] E4. **Dead code:** `struct FF_ArrayHeader` at `include/FF_Utilities.hpp:36` has zero
  call sites (`grep -rn 'FF_ArrayHeader' src/ include/ tools/ tests/ python/` → only the
  definition). Delete the struct. While there, run the same zero-call-site check on other
  structs/functions in `include/FF_Utilities.hpp` and list findings in the PR description
  (do not delete others without confirmation).
- [ ] E5. **Doc staleness sweep:**
  - [ ] E5.1 `README.md` "Profile Selection" subsection still documents deleted modules
        (`fetch_specs.py`, `ffd.py`, `ffcs.py`, `ffc.py`, `make_lib.py`) and says resource
        scope constants live "in `ffc.py`" — they live in `generator/model/type_map.py`.
        Rewrite that subsection against the real tree; the module table just above it is
        already correct — deduplicate.
  - [ ] E5.2 `README.md` states unknown-extension JSON is NOT preserved for re-emission
        (Extensions "Condition 2") — keep until D8 lands, then update (D8's commit owns it).
  - [ ] E5.3 Run every snippet in `python/README.md` against a freshly built
        `fastfhir` package; fix or annotate any that fail.
- [ ] E6. **Dictionary unification — final sweep:** the unification is essentially done
  (`FF_UCUM_Concepts.cpp` deleted; `master_codes.json` is source of truth;
  `generator/emit/dictionary.py` evolved into the master-codes producer and stays).
  Remaining checks:
  - [ ] E6.1 `grep -rn 'FF_UCUM_STRINGS\|kUCUMTable\|FF_UCUM_CODES' src/ include/ dictionaries/ generator/`
        — delete any dead remnants found (expect: possibly none).
  - [ ] E6.2 Verify the generated dictionaries carry the planned `static_assert` guards:
        string-table size consistency in `dictionaries/FF_Dictionary_Strings.cpp`, and
        last-entry-code < string-table-size in `FF_R4_Dictionary.cpp` /
        `FF_R5_Dictionary.cpp`. If absent, add them to the emitters
        (`generator/emit/master_dictionary.py`), regenerate, commit both.
- [ ] E7. **Umbrella header decision** `Blocked on Q2` — `include/FastFHIR.hpp` currently
  includes only `FF_Version.hpp`, `FF_Parser.hpp`, `FF_Builder.hpp`, `FF_Compactor.hpp`
  (deliberately excluding `FF_FieldKeys.hpp`; `FF_Memory.hpp` and `FF_Ingestor.hpp` arrive
  transitively or must be included explicitly — verify which before writing). Implement
  whichever option Q2 selects and update README examples if the include set changes.
- [ ] E8. **Record FHIR package provenance** — `generator/specs.py:20-21` pins exact
  tarball URLs (`hl7.fhir.r4.core-4.0.1`, `hl7.fhir.r5.core-5.0.0`), which is already better
  than a moving ref, but nothing checksums them and nothing records them in the output
  (`grep -n "sha256\|hashlib" generator/specs.py` → empty). "Regenerate the R4 layout" is
  therefore reproducible only for as long as packages.fhir.org never re-publishes 4.0.1
  content — and an errata release would change the wire format silently. Same shape as B4
  (Synthea), same fix:
  - [ ] E8.1 Record `sha256` of each downloaded tarball; verify on every download and fail
        loudly on mismatch, naming both hashes.
  - [ ] E8.2 Emit the package version **and** hash into the generated header banner
        (`generator/emit/header.py`'s `auto_header`), plus the generator's own version. A
        generated artifact should state what produced it; once the format is specified (I1)
        this is what makes "regenerate the ratified layout" a reproducible operation.
        Note: this changes every generated file's banner — confirm it does not perturb the
        wire witness (it should not; the witness reads constants, not comments) and that
        `test_determinism.py` still passes.
  - [ ] E8.3 Do **not** put a clock in the banner. A rolling date or `datetime.today()`
        makes output depend on build date, breaks byte-identical regeneration, and would
        fail A4.4. Source any year from the package metadata.
- [ ] E9. **State and guard the recovery-tag family ceilings** — `include/FF_Recovery.hpp:19-25`
  declares the family convention (core `0x0000`–`0x00FF`, scalars `0x0100`–, data types
  `0x0200`–, resources `0x0300`–, sub-elements `0x0400`–) but never says these are hard
  ceilings, nor what crossing one costs. Current occupancy: 11, 11, 29, 29, **86** — one
  entry per family being the family marker itself, so 28 data types, 28 resources and 85
  sub-element blocks, which reconciles exactly with the golden's 141 vtables (28+28+85).
  That is ~3.0 sub-element blocks per resource, so the 170 free slots in `0x04xx` admit
  roughly 56 more resources — exhausting at ~84 total, against ~145 resources in R4.
  **The sub-element family runs out at roughly half of FHIR coverage**, i.e. during normal
  completion of the existing roadmap, and crossing it is a design decision because the read
  path dispatches on the high byte (`(entry.tag & 0xFF00) == RECOVER_FF_SCALAR_BLOCK`,
  `include/FF_Ops.hpp:188`).
  - [ ] E9.1 Write the ceiling into the convention block: 256 per family, what the high-byte
        dispatch requires, and that a second sub-element family is a format decision needing
        a note in SPEC.md (I1.6) — not a routine addition.
  - [ ] E9.2 Add a test asserting no family exceeds 240 entries, so the wall arrives as a
        build failure with room to plan rather than as a merge conflict over the next free
        number. Parse `include/FF_Recovery.hpp` directly; assert the parse found >100
        enumerators so the test cannot become vacuous.
- [ ] E10. **`FF_Ops.hpp` leaks three unqualified names onto the public include path** —
  `include/FastFHIR.hpp:92` → `include/FF_Parser.hpp:24` → `include/FF_Ops.hpp`, which
  defines `bswap16` / `bswap32` / `bswap64` as object-like macros (`FF_Ops.hpp:34-40`) with
  no `#undef` anywhere in the file, plus `constexpr bool requires_byteswap` and
  `is_ieee754` at global namespace scope (`:43-44`). `bswap32` is a common enough spelling
  that a consumer including FastFHIR alongside another byte-order header gets a macro
  collision with no workaround short of `#undef` after the include. Move the constants into
  `namespace FastFHIR`; convert the macros to `constexpr` function templates (preferred —
  `std::byteswap` is C++23, so a small `FastFHIR::detail::bswap<T>` is the C++20 stand-in),
  or failing that prefix them `FF_` and `#undef` at end of header. Nothing outside the file
  uses them (`grep -rn "bswap" include src tools python generator` → only `FF_Ops.hpp`).
  Alpha and pre-consumer is exactly when this costs nothing.
- [ ] E11. **Close two latent traps in the scalar templates** — neither is reachable today;
  both fail silently the day they are:
  - [ ] E11.1 `Decode::scalar<float>` (`include/FF_Ops.hpp:166`) dispatches on `sizeof(T)`,
        so `float` takes the `sizeof(T) == 4` branch and `static_cast<float>`s an integer
        bit pattern — a numeric conversion — never reaching `LOAD_F32` (`:142`). Not
        reachable: `grep '"cpp":' generator/model/type_map.py` yields only `ChoiceEntry`,
        `Offset`, `ResourceReference`, `double`, `uint32_t`, `uint64_t`, `uint8_t`.
  - [ ] E11.2 `Encode::scalar` (`:222`) handles `bool`, `double`, `sizeof==4` and
        `sizeof==8`; an `int16_t` matches no branch and the function returns having written
        nothing. Not reachable: the string `Encode::scalar` appears nowhere in `include/`,
        `src/`, `tools/`, `python/`, `generator/` or `tests/`.
  - [ ] E11.3 Fix both with a `static_assert(false)`-style final `else` (a dependent-false
        helper, since a bare `static_assert(false)` in a discarded branch is ill-formed
        before C++23), converting each into a compile error the day someone adds `float` or
        `int16_t` to `TYPE_MAP`. This also replaces the unreachable
        `throw std::runtime_error` at `:180`, which defers to runtime what the compiler can
        settle. Add `float` and a 2-byte type to `TYPE_MAP` locally to confirm both now fail
        to compile, then revert.
- [ ] E12. **Reconcile the endianness wording** — `include/FF_Ops.hpp:28` states the wire is
  strictly little-endian, while `include/FF_Primitives.hpp` describes several code payloads
  as "native-endian" (lines 94, 98, 106, 110, 114, 134, 142, 150). These are reconcilable —
  the payloads are written through `STORE_U*`, which normalises — but the contradiction sits
  in the one file a reader consults for the wire layout, and I1 will inherit the wording.
  Wording pass only; no code change. Confirm the reconciliation is true before rewording
  (i.e. that every one of those payloads really does go through `STORE_U*`), and if any does
  not, that is a Block A bug, not a comment fix.

---

## Block F — Benchmark publication & performance evidence

**Context:** benchmarks exist in a separate repo —
<https://github.com/ryanlandvater/FastFHIR-benchmark> — built with Bazel so the comparison
targets build identically across systems; results are machine-generated by running it and
show orders-of-magnitude improvements. This repo's README, however, still makes
unquantified performance claims ("wildly fast", "fundamentally outpacing", "nanosecond
read times") with no citation. Skeptical readers discount uncited claims; the fix is to
cite, quantify, and regression-guard.

- [ ] F1. **Add a "Benchmarks" section to README.md** (place it directly after the
  "Why FastFHIR?" section): link the benchmark repo, state what is measured (ingest
  throughput, cold random field access, full traversal, concurrent append — confirm the
  actual suite contents against the benchmark repo before writing), state that Bazel is
  used so competitor libraries build with identical flags, and give the one-command
  reproduction (`bazel run ...` — copy the exact invocation from that repo's README).
- [ ] F2. **Publish concrete numbers** `Blocked on Q12`: a results table in the new README
  section with median and p99 per operation, dataset identity (Synthea bundle, size),
  hardware description, and competitor library versions. Every number must be
  reproducible by running the benchmark repo at a stated commit. Replace the README's
  bare adjectives with citations to this table where they occur (do NOT delete the claims
  — anchor them).
- [ ] F3. **In-repo perf smoke guard:** add `tests/cpp/test_perf_smoke.cpp` — parse +
  fully traverse a fixed committed `.ffhr` fixture 1,000× and print ns/op to stdout;
  register in ctest but assert only a very generous ceiling (e.g. 100× the expected cost)
  so it catches catastrophic regressions (accidental O(N²), heap allocation on the read
  path) without being flaky on shared CI runners. Full comparative benchmarking stays in
  the benchmark repo; this is a tripwire, not a benchmark.
- [x] F4. **Keep the benchmark repo honest:** when a task in this file changes the public
  API or wire format, note the change for FastFHIR-benchmark (open an issue there or
  re-run it). Codified as Execution contract rule 9 above. (151e025)
- Verify (block): README benchmark section renders with a working repo link;
  `ctest -R cpp_test_perf_smoke` passes; every performance adjective in README has a
  citation or was scoped.

---

## Block G — Security hardening & trust

**Context:** README claims hardware-level safety on "untrusted input streams" and
"cryptographic sealing", but nothing adversarial exercises the parser, and a checksum
footer detects corruption — not tampering (an attacker who can modify payload bytes can
recompute the SHA-256 footer). For a healthcare data parser these claims must be earned.
**Priority note (Ryan, 2026-07-08): fuzzing has not been done and is required — treat
G1/G2 as high priority; they have no blockers.**

- [ ] G1. **Fuzz targets** — new directory `tests/fuzz/` with three libFuzzer harnesses:
  - [ ] G1.1 `fuzz_parser.cpp`: `LLVMFuzzerTestOneInput(data, size)` → construct
        `FastFHIR::Parser(data, size)` inside try/catch, and if it constructs, walk
        `root()` recursively (`entries()`, every field via string-key iteration) and call
        `print_json` to a null sink. Any crash/ASAN report = finding. Exceptions are fine.
  - [ ] G1.2 `fuzz_ingestor.cpp`: bytes → treat as JSON → `Ingest::Ingestor::ingest` into
        a fresh anonymous arena (guard behind `FASTFHIR_BUILD_INGESTOR`).
  - [ ] G1.3 `fuzz_compact.cpp`: bytes → Parser with compact-layout header stamped, full
        walk (the compact read path has its own offset arithmetic).
  - [ ] G1.4 CMake option `FASTFHIR_BUILD_FUZZERS` (default OFF) adding the three targets
        with `-fsanitize=fuzzer,address,undefined`; document usage in a
        `tests/fuzz/README.md`. Seed corpus: generate 3–5 valid `.ffhr` files from the
        test fixtures into `tests/fuzz/corpus/`.
- [ ] G2. **Sanitizer CI leg** (extends E1, do after it): one Linux job building with
  `-fsanitize=address,undefined` and running the full ctest suite, plus a 5-minute
  bounded run of each fuzzer from the seed corpus.
- [ ] G3. **`SECURITY.md` threat model:** trust boundaries (untrusted stream bytes,
  untrusted FHIR JSON, third-party WASM codecs, registry network I/O); what the checksum
  footer does and does NOT provide (integrity vs authenticity — be explicit); the WASM
  sandbox guarantees and their limits; vulnerability reporting channel (email). Keep it
  one page; link from README.
- [ ] G4. **Signature footer (authenticity)** `Blocked on Q11`: add an asymmetric
  signature option (Ed25519) alongside the checksum algorithms so sealed archives can be
  *authenticated*, not just integrity-checked. Requires a new permanent algorithm
  constant next to `FF_CHECKSUM_*` in `include/FF_Primitives.hpp` — wire-constant
  allocation needs Ryan's sign-off (Q11). API shape: same
  `finalize(algo, callback)` pattern with the callback signing instead of hashing;
  `Parser::checksum()` gains a verified-against-public-key path. Until this lands, I3
  scopes the README's "cryptographic sealing" wording to integrity.
- [ ] G5. **WASM supply chain:** require the registry manifest (D1) to carry a signature
  over `{url, binary_hash, version}` and verify before trusting a fetched module; design
  belongs in D0's interface contract — add it there when D0 executes (this task is the
  reminder + review gate; do together with D0). `Blocked on Q5`.
- Verify (block): fuzzers build and run 5 minutes clean from seeds; SECURITY.md exists
  and is linked; sanitizer CI leg green.

---

## Block H — Packaging & distribution

**Context:** today the only way to consume FastFHIR is a from-source build that needs
network at configure time. Each packaging channel removes an adoption barrier.
**Template:** Ryan already ships a C++ library this way — the
[Iris-Codec](https://github.com/IrisDigitalPathology/Iris-Codec) repo has working
GitHub Actions for all of this (`build-linux.yml`, `build-macos.yml`, `build-win.yml`,
`build-wasm.yml`, `cmake-{linux,macos,win64}-CI.yml`, `distribute-pypi.yml`,
`distribute-releases.yml`, `distribute-npm.yml`) plus a separate conda-forge feedstock.
**Mirror those workflows rather than designing from scratch** — copy the structure, swap
in FastFHIR targets.

- [ ] H1. **PyPI wheels** `Blocked on Q7`: port Iris-Codec's `distribute-pypi.yml`
  mechanics to this repo:
  - `cibuildwheel` for Linux (custom manylinux 2.34 image, `CIBW_BUILD`/`CIBW_SKIP`
    matrix), native jobs for macOS (`macos-latest` + `macos-13`) and `windows-latest`;
    Linux runners `ubuntu-latest` + `ubuntu-24.04-arm`; Python 3.11–3.13.
  - Versioning via setuptools-scm with `SETUPTOOLS_SCM_PRETEND_VERSION` extracted from
    the release tag.
  - Publish with OIDC trusted publishing (`pypa/gh-action-pypi-publish@release/v1`),
    publish job gated on release events only; consolidate wheel artifacts from all build
    jobs first.
  - FastFHIR-specific prerequisite (Q7): the sdist/build must vendor a pinned
    `generated_src/` snapshot so wheel builds never hit the network for HL7 bundles.
  - Acceptance: `pip install fastfhir` in a clean venv → `import fastfhir;
    fastfhir.Memory` works; `tests/python/test_readme.py` passes against the wheel.
- [ ] H2. **Prebuilt CLI releases:** port `distribute-releases.yml` + the per-OS
  `build-*.yml` pattern: on tag push, build `ff_ingest`/`ff_export`/`ff_compact` for
  Linux/macOS/Windows, inject the version via the `FASTFHIR_VERSION_*` env vars already
  honored by `include/FF_Version.hpp` (tag `v1.2.3` → 1/2/3), attach binaries + SHA-256
  sums to the GitHub Release.
- [ ] H3. **conda-forge feedstock** (unblocked — license is now MPL-2.0, OSI-approved):
  submit to conda-forge `staged-recipes` following the same path used for Iris-Codec's
  feedstock (separate feedstock repo, recipe consuming the release tarball; recipe
  `license: MPL-2.0`, `license_file: LICENSE`). Needs a tagged release first (H2).
- [ ] H4. **Offline-build path** `Blocked on Q7`: whichever mechanism Q7 selects
  (committed `generated_src/` snapshot under `third_party/` or a release-attached
  tarball + `FASTFHIR_GENERATED_SNAPSHOT=<path>` CMake option), configure must succeed
  with networking disabled. Acceptance: `cmake -S . -B build -DFASTFHIR_...` completes in
  a network-isolated container. Do FIRST in this block — H1 depends on it.
- [ ] H5. **vcpkg / Conan recipe** (lower priority than H1–H3; unblocked — MPL-2.0 is
  registry-friendly): write a port/recipe consuming a release tarball. Needs a tagged
  release first (H2).

---

## Block I — Specification, claims alignment & governance

- [ ] I1. **Normative wire-format specification** `Blocked on Q13`: new `docs/SPEC.md`
  describing the format independently of the C++ — header layout (seed from the comment
  block at `include/FF_Primitives.hpp:540`), DATA_BLOCK anatomy, vtable rules, array
  kinds, FF_STRING, choice slots, code encoding (dictionary ID vs
  `FF_CODEABLE_CONCEPT_FLAG` block), compact layout, checksum footer, and a
  format-version + compatibility-guarantee statement (Q13 decides the freeze wording).
  Seed heavily from `architecture.md` §4–§6 but write it as a spec (MUST/SHOULD), not a
  tour. This is the bus-factor mitigation: a third party must be able to read a `.ffhr`
  from this document alone.
  - [ ] I1.6 The spec MUST state the numbering ceilings, not just the current assignments:
        recovery-tag families are 256 values each and the read path dispatches on the high
        byte (E9), engine MAJOR is 14 bits and MINOR 16 (`architecture.md:118`), and code
        IDs exclude bit 31 (`FF_CODEABLE_CONCEPT_FLAG`) and `0xFFFFFFFF`. Every derived
        numbering has a ceiling; an unstated one gets crossed by someone adding "just one
        more". State the append-only rule and deprecation as the only retirement path in
        the same section.
- [ ] I2. **Spec/format licensing statement** (Q10 decided: spec text under CC-BY-4.0):
  add the CC-BY-4.0 notice to `docs/SPEC.md` plus a pointer to `TRADEMARK.md` (anyone may
  implement from the spec; only conformant implementations may claim the name). Do
  together with I1 when SPEC.md is created.
- [ ] I3. **README claims alignment sweep** (one commit per bullet):
  - [ ] I3.1 Scope "Concurrent Mutex-Free Generation" to the append path: appends are
        lock-free; `amend_*`/finalize are single-threaded by contract (until C6/Q9 says
        otherwise). One added sentence, not a rewrite.
  - [ ] I3.2 Mark the WASM registry sections as **experimental** until D0–D3 land
        (registry fetch is currently a plain-TCP stub — see D2).
  - [ ] I3.3 State profile coverage explicitly near the top (27 US Core / 22 UK Core
        resources) and what happens to out-of-profile resources in a bundle (verify the
        actual ingest behavior first — skipped? error? — and document exactly that).
  - [ ] I3.4 Clarify "Cryptographic Sealing" to integrity-not-authenticity wording until
        G4 lands (then G4's commit reverts this).
  - [ ] I3.5 Replace/anchor unquantified performance adjectives with citations to the F2
        benchmark table (do together with F2).
- [x] I4. **Contribution surface** — done 2026-07-08 alongside I5: `CONTRIBUTING.md`
  (build prerequisites, two style regimes, wire invariants, TASKS.md claim workflow,
  DCO sign-off; the FF-SSL right-to-repair path is obsolete under MPL), GitHub issue +
  PR templates under `.github/`, and a README roadmap link (in the License section).
- [x] I5. **License migration to MPL-2.0** — done 2026-07-08 per Q10's answer:
  1. ✔ `LICENSE` replaced with canonical MPL-2.0 text (373 lines, verbatim).
  2. ✔ Every FF-SSL source-header notice replaced with the MPL Exhibit A notice
     (`grep -rn 'FF-SSL' src/ include/ tools/ python/ tests/ generator/` → empty).
     Note: the generator's `auto_header` emitter carries no license line, so generated
     files (`dictionaries/`, `generated_src/`) needed no change.
  3. ✔ README badge + License section rewritten; CLAUDE.md license text updated.
     `pyproject.toml` has no `[project]` table yet — **H1 must set
     `license = "MPL-2.0"` when it creates the packaging metadata.**
  4. ✔ H3/H5 unblocked (MPL-2.0 is OSI-approved, registry-friendly).
  5. ✔ `TRADEMARK.md` conformance policy + `NOTICE` attribution file added.
  6. ✔ DCO requirement in `CONTRIBUTING.md`; the enforcing PR check lands with CI (E1).
- Verify (block): docs render; a reviewer unfamiliar with the code can describe the
  header byte layout from SPEC.md alone; README contains no unscoped claims flagged in I3.

---

## Block J — External code systems: generated headers + optional validation layers

> **Status: specification stub.** Nothing here is implemented. Q14 and Q16 are answered
> (2026-07-30) and their decisions are folded in below. Q15 (acquisition) has research
> recorded under it but still needs Ryan's pick. Read this whole preamble before touching
> anything — the licensing and ledger constraints are why the feature is shaped this way
> and are not negotiable by an implementer.

### J0. What this is, and why it cannot break anything

External code systems (LOINC, SNOMED CT, RxNorm, ICD-10, …) get support in **two
separable halves**. They ship independently and neither requires the other:

1. **Compile-time: generated constant headers.** `#include <FastFHIR/ExternalCodes/LOINC.hpp>`
   gives `FF_EXTERNAL_CODE::LOINC::SODIUM_MOLAR` — the same shape as the generated
   `Fields::` keys, with full IDE type assist. Pure `constexpr`; no runtime dependency,
   no layer required. **This is the half that prevents mistakes**, per Q16: a named
   constant cannot be mistyped, a hand-written string can.
2. **Runtime: optional validation layers.** A layer is a dylib discovered at runtime,
   modelled on the Vulkan layer loader (Q14). Present → codes for that system are
   validated against the real release **as they are written** (J4), inline at the encode
   site. Absent → the checks simply do not run. Absence is never an error and never fails
   an ingest, and a failed check warns loudly (J5) but never rejects the write.

   The two halves reinforce each other: a code written from a `FF_EXTERNAL_CODE::…`
   constant passes by construction, because the constant came from the release the layer
   validates against. The inline check exists to catch the hand-typed string.

Both halves are built on the user's machine from a release **they** are licensed to use.

**FastFHIR natively does not adjudicate these codes and never ships their values.** Both
halves are a convenience over data the user already has rights to. This is the same
boundary `generator/master_codes.json` already enforces via `_assert_redistributable`.

**Coverage commitment: every CodeableConcept system FastFHIR supports gets external code
validation.** Not a favoured subset. `FF_CodeableConceptSystem`
(`include/FF_Primitives.hpp:86`) is the definitive list, and every value in it must have a
declared validation story before this block is done:

| Systems | Validation story |
|---|---|
| SNOMED CT, RxNorm, LOINC, DICOM, CPT, CVX, NDC, ICD-9-CM, ICD-10, ISO 3166, MDC, UNII, MED-RT, pCLOCD, IDMP (15) | Membership layer + generated `FF_EXTERNAL_CODE` header |
| UCUM | **Built-in, not a layer — and fully validated.** Expressions are composable (`mg/dL`, `10*6/uL`), so the check is a grammar parse plus atom membership rather than a table lookup (J4.7). UCUM is formally specified and publishes its own conformance suite, so this is the *strongest* validation in the block, not a weaker one. Needs no user-supplied data. |
| UNKNOWN | Sentinel for "no system identified" — nothing to validate against, by definition. |
| FHIR_DICTIONARY | Already validated by construction: the code resolved to a permanent ledger ID. |

Two things this commitment does **not** mean, and both must stay clear in any user-facing
wording (I3 claims alignment applies):

- It does not mean FastFHIR supplies the data. For most of these the user brings the
  release (J2, Tier A). CPT in particular is AMA-licensed and paid — we can never ship or
  fetch it, so its layer only exists for a user who already holds a licensed release.
- It does not mean a layer must exist for a system to be *usable*. Codes for every system
  encode and round-trip today with no layer at all; a layer adds checking, never
  capability.

The enforcing mechanism is J8.5, not good intentions: a test that fails when a value is
added to `FF_CodeableConceptSystem` without a validation story. This codebase has already
been bitten once by exactly this — `FF_CC_CODECS` covered all 17 systems while
`print_scalar_json` covered 4, and nothing caught the gap.

Three properties make this safe, and an implementer must preserve all three:

1. **Nothing here adds wire format or can invalidate a stored stream.** The wire already
   carries external codes natively: `FF_CodeableConceptSystem`
   (`include/FF_Primitives.hpp:86`) has 17 permanent systems, and `FF_CC_CODECS`
   (`src/FF_Primitives.cpp:362`) gives each one its payload encoding — SNOMED CT is an
   8-byte native concept ID, RxNorm 4-byte, DICOM 4-byte hex, LOINC/NDC/ICD variable
   ASCII. Those codes are *self-encoding*; they need no FastFHIR-assigned ID. Headers and
   layers therefore allocate nothing on the wire. A stream written with a layer loaded
   must be byte-identical to one written without it (J7.3).
2. **Nothing here touches the permanent ledger.** `generator/master_codes.json` and
   `dictionaries/` stay HL7 FHIR + UCUM only. No entry, no `_next_id` consumption, not
   produced by `python -m generator`. Output is a build artifact like `generated_src/`,
   gitignored, never committed. (Execution contract rule 5 and `dictionaries/README.md`
   already forbid the alternative.)
3. **This project never redistributes the data.** Release files, generated headers and
   compiled layers all stay on the user's machine. This is not optional caution: SNOMED
   CT redistribution requires *FastFHIR* to be an Affiliate **and** to issue and track a
   sublicence for every downstream user (see Q15 research). We will not take that on.

**Naming.** The runtime dylibs are **layers** (Vulkan's term, per Q14). The compile-time
headers are **external code headers**, namespace `FF_EXTERNAL_CODE`. Do *not* call either
an "extension" in code or docs — that word is already taken twice here: FHIR `Extension`
elements (`FF_EXTENSION` blocks, `Extension.url`, `FF_URL_DIRECTORY`) and Block D's WASM
extension codec modules (`EXT_REF`, the module registry). A third meaning would be a bug
factory.

**Relationship to existing design.** `terminology_layer_architecture.md` §6 already
specifies a validator dispatch table (`FF_CodeValidator`,
`FF_EXTERNAL_VALIDATOR_TABLE`, `include/FF_Terminology.hpp`). Those validators are
*syntactic* — format and check-digit only. A layer is the *membership* check that sits
behind the same table: "is this actually a LOINC code in release 2.77", plus the display
name. Extend §6; do not invent a parallel mechanism. Note that §6 calls the enum
`FF_ExternalCodeSystem` while the implemented enum is `FF_CodeableConceptSystem` —
reconcile the doc when you touch it.

**Sequencing.** J4 and J5 depend on **A8** (`external_system_map` is never populated, so
every code currently encodes as `UNKNOWN`). Until A8 lands there is no reliable way to
route a field to the right layer at runtime. J1, J2, J3, J6 and J8 can proceed before A8.

### J1. Layer model — **Q14 answered: Vulkan-style runtime layers**

Runtime-loaded dylibs discovered like Vulkan validation layers: available → used; not
present → the checks are skipped, silently and successfully. The analogy is apt in a
second way worth preserving — Vulkan validation layers are a development-time correctness
aid, not a production hot-path feature. Terminology validation should carry the same
expectation.

- [ ] J1.1 Layer discovery: manifest files (JSON, naming the system, release version,
      licence and dylib path) found on a search path, with an env-var override
      (`FASTFHIR_LAYER_PATH`, mirroring `VK_LAYER_PATH`). Decide implicit (auto-enable
      what is found) vs explicit (host must ask by name). Vulkan supports both; implicit
      matches "if it's available we can use it".
- [ ] J1.2 Stable C ABI for the layer boundary, versioned. A layer built against one
      FastFHIR release must not silently misbehave against another — refuse to load on
      ABI mismatch and log it (see J5).
- [ ] J1.3 Absence must be a no-op, never an error: no layer for a system means codes for
      that system are simply not membership-checked. Never fail an ingest because a layer
      is missing. Test this explicitly.
- [ ] J1.4 `terminology_layers.md` (new doc) capturing the model and J0's three
      invariants. Link from `CLAUDE.md`'s repo map.

### J2. Acquisition — **Q15 answered: local data only; no server layer for now**

Sources differ enough that one policy cannot cover them. Citations under Q15.

**Decision (Ryan, 2026-07-30): Tier A is the default and the only tier built for now.
Tier D (terminology server) is deferred, not deleted.**

- [ ] J2.1 Per-system manifest declaring: acquisition tier, release version, licence
      identifier, checksum, and whether unattended download is permitted **at all**.
- [ ] J2.2 Build **Tier A** now; leave B and C as manifest-declared options to add when
      a user actually needs them.
      - **Tier A — user-supplied release file (DEFAULT).** The user points at a release
        they already hold. Works offline, works air-gapped, and is the only tier that
        covers sources which can never be automated (CPT is AMA-licensed and paid).
      - **Tier B — authenticated download with the user's own credentials.** NLM's UTS
        Download API issues per-user API keys and can fetch SNOMED CT, RxNorm and UMLS
        releases in one command. FastFHIR never holds the key or the data. Convenience
        only — Tier A already covers these sources.
      - **Tier C — unauthenticated download.** Legitimate only for public-domain sources
        (ICD-10-CM from CMS/NCHS, NDC from FDA, CVX from CDC).
- [ ] J2.3 **Tier D — terminology-server layer. DEFERRED.** Do not build it now, and do
      not design it out either. Rationale for deferring: it makes ingest depend on a
      network service, it is unusable in the air-gapped hospital deployments that are a
      core target, there is no production-grade public server (HL7 states tx.fhir.org
      "is not suitable for use as a production terminology server"), and measured server
      quality varies enormously (composite scores 100% → 6% across five servers in the
      Health Samurai TX benchmark).
      **Note the cost of deferring:** a non-Affiliate SNOMED user cannot legally hold the
      release, so Tier A is unavailable to them and a server layer is their only route.
      They get no SNOMED support until Tier D exists.
      **Why a server is disqualified rather than merely slower:** validation runs *inline
      on the write path* (J4), so a server-backed layer would put a network round-trip
      inside `ENCODE_FF_CODE`. That is not a tuning problem, it is the wrong shape. A
      local table lookup in the same position is fine. If Tier D is ever revisited it
      cannot reuse the inline call site and would need a separate out-of-band mode — which
      is a different feature, not a swap.
      Concrete requirement on J1.2 today: keep the layer C ABI a *call* ("is this code
      valid"), not a data handoff ("give me your sorted array"). That keeps the layer
      implementation free to change without touching the boundary.
- [ ] J2.3 Build step that turns a release file into generated C++ under the build tree
      (mirroring how the generator writes `generated_src/`). Deterministic — two runs
      byte-identical — and **fail loud** when the file is missing or the checksum does
      not match. Never silently emit an empty table; that is precisely the failure mode
      `dictionaries/FF_SNOMED_Concepts.cpp` has today (J8).
- [ ] J2.4 Gitignore all generated headers and layer binaries; CI check that none is ever
      committed.

### J3. Generated external code headers (compile-time half — no A8 needed)

This is the half Q16 calls "impossible to mess up". It works with no layer loaded.

- [ ] J3.1 Emit one header per system at `FastFHIR/ExternalCodes/<SYSTEM>.hpp`, namespace
      `FF_EXTERNAL_CODE::<SYSTEM>::<NAME>`, following the generated `Fields::` keys as the
      precedent for a large generated constant header.
- [ ] J3.2 Constant *values* are the terminology's own codes, in the **same representation
      the wire uses** for that system so no re-parse is needed: SNOMED `uint64_t`, RxNorm
      `uint32_t`, LOINC ASCII. Derive that from `FF_CC_CODECS` — never re-derive
      per-system widths in a second place. That exact duplication caused A7 and A9.
- [ ] J3.3 Reuse `emit/codes_header.py`'s `assign_identifier` ladder and `RESERVED_MACROS`
      guard rather than writing a second identifier sanitiser.
- [ ] J3.4 State in every generated header that constant *names* are source-level only and
      may change between releases, while *values* belong to the terminology. Neither is a
      wire constant. Nobody should mistake these for the ledger.
- [ ] J3.5 **Split large systems by hierarchy, do not curate a subset (Ryan,
      2026-07-30).** Emitting all ~350k SNOMED concepts in one header would wreck IDE type
      assist, but curating a global subset would just make the useful code the one that is
      missing. Instead split along the terminology's own hierarchy and let the caller
      include only what they need — e.g.
      `FastFHIR/ExternalCodes/SNOMED/ClinicalFinding.hpp`. Requirements: the split must be
      derived from the release's own hierarchy (never hand-partitioned), an umbrella
      `SNOMED.hpp` should exist for callers who genuinely want everything, and the same
      constant must not appear in two sub-headers with different names. Systems small
      enough (LOINC, RxNorm, CVX) stay a single header. Measure compile time per
      sub-header.

### J4. Validation on the write path — *needs A8*

**Validation runs during writes when a layer is present and linked (Ryan, 2026-07-30).**
Not post-seal, not out-of-band. The code is checked at the point it is encoded, so the
warning names the field being written while that context still exists.

This is affordable because of what it is checking against: a local table lookup, and — for
anyone using the `FF_EXTERNAL_CODE::…` constants — a check that passes by construction,
since the constants were generated from the same release the layer validates against. The
cost is paid to catch hand-typed codes, which is exactly where the risk is (Q16).

- [ ] J4.1 Hook the check at the `ENCODE_FF_CODE` call site, routed through
      `terminology_layer_architecture.md` §6's existing table, so a loaded layer upgrades a
      system from syntactic to membership validation and an absent one degrades to today's
      syntactic check. One dispatch path, not two.
- [ ] J4.2 Layer lookup resolved once per system, not per code — an indirect call per code
      is acceptable, a dylib symbol lookup per code is not.
- [ ] J4.3 The check must be allocation-free and must not throw; it runs on the write hot
      path. A failure produces a warning (J5), never an exception, never a rejected write.
- [ ] J4.4 Benchmark the inline cost with a layer loaded vs not, on a bundle with many
      distinct codes, and record it. Per the benchmark rule, do not assert "negligible" —
      measure it. If it proves material, the fallback is a per-system enable flag, not
      moving validation off the write path.
- [ ] J4.5 Concurrency: `claim_space()` appends are lock-free and validation sits beside
      them, so a layer's validate entry point must be reentrant and thread-safe. State
      this in the layer ABI contract (J1.2) — a layer that is not is a layer that will
      corrupt a concurrent ingest.
- [ ] J4.6 **Cover all 15 external systems**, per the J0 coverage commitment — not just
      the well-known four. Priority order by real-world frequency (LOINC, SNOMED CT,
      RxNorm, ICD-10 first) is fine, but the block is not done until every value in
      `FF_CodeableConceptSystem` has a layer or a documented reason it needs none.
      Several are small and public-domain (CVX, ISO 3166, MDC), so they are cheap wins,
      not afterthoughts.
- [ ] J4.7 **UCUM: built-in validation, no layer and no user-supplied data.** UCUM is not
      an enumerable set — `mg/dL`, `10*6/uL`, `{beats}/min` are constructed — but it is
      fully specified, so expressions *can* be validated. UCUM publishes a formal grammar
      (LL(*), expressed in ANTLR) and a machine-readable definition file.
      Validation is **two things, not one**:
      1. **Grammar parse** of the expression: `.` multiply, `/` divide (including a leading
         `/` as in `/min`), integer exponents (`m2`, `cm3`), `*` exponent form (`10*6`),
         parentheses for grouping, numeric factors, `[...]` non-metric atoms (`[in_i]`,
         `[pH]`), and `{...}` annotations which are syntactically required to balance but
         semantically void (`{cells}`).
      2. **Atom membership** for every atom the parse yields, plus the prefix rule: **only
         metric atoms may take a prefix** — `kW` is legal, kilo-feet is not. Case matters
         (`Ms` megasecond vs `ms` millisecond); `emit/codes_header.py:146` already records
         this for naming.
- [ ] J4.8 **The data for J4.7 is not in the repo yet.** The 1,384 UCUM constants in
      `dictionaries/FF_Codes.hpp` (namespace `UCUM`, lines 18–1403) are *whole expressions*
      harvested from FHIR value sets — `PERCENT`, `PERCENT_PER_100WBC` — each mapped to a
      permanent ledger ID. They are the wrong shape for parsing and cover only what FHIR
      happens to use. Grammar validation needs the UCUM **atom and prefix** tables (~300
      atoms, 24 prefixes) from `ucum-essence.xml`, which the generator does not currently
      fetch. Add that fetch, and emit atoms/prefixes as a separate table.
      **This does not touch the ledger:** atoms are parser inputs, not codes, and get no
      ID. The existing 1,384 expression IDs stay exactly as they are — see J0 invariant 2
      and `_assert_redistributable`, which already permits UCUM as in-scope.
- [ ] J4.9 Honour J4.3 in the parser: allocation-free and non-throwing, since it runs on
      the write path. A recursive-descent parser over a `string_view` with a bounded stack
      satisfies this; do not reach for regex or build an AST on the heap.
- [ ] J4.10 Out of scope for now, worth recording: UCUM also supports canonicalisation, so
      a future check could verify a unit is *commensurable* with what a field expects (a
      body-weight `Quantity` should be a mass, not a volume). That is a stronger and more
      useful check than well-formedness, but it needs conversion factors, not just atoms.

### J5. Failure policy — **Q16 answered: loud logged warning, never a drop**

Store the code, never reject the write, never silently discard clinical data — but the
warning has to be impossible to miss. Emitted inline from the write path (J4), so it can
name the field being written and not just the code.

- [ ] J5.1 `ConcurrentLogger` (`include/FF_Logger.hpp:33`) currently exposes a single
      `log(std::string_view)`; severity is convention only, expressed as a `"[Info] "`
      prefix in the message text. There is no way to be loud. Add real severity levels
      (at least Info / Warning / Critical) so a terminology failure can be surfaced
      distinctly and counted.
- [ ] J5.2 Surface a per-ingest count of failed codes in the result, so a caller sees
      "1,412 codes failed LOINC membership" without scraping the log text.
- [ ] J5.3 Message must name the system, the offending code, and the release version the
      layer was built from — a code that is valid in LOINC 2.80 and absent from 2.77 is a
      version problem, not a data problem, and the message should make that obvious.
- [ ] J5.4 Never let validation failure alter what is written. The stream must be
      byte-identical either way (J7.3).

### J6. Display lookup — *needs A8*

- [ ] J6.1 `code → display` lookup served by a loaded layer, returning
      `std::string_view` into the layer's static table. No allocation, matching the
      read-path rule in CLAUDE.md. With no layer loaded the lookup returns empty rather
      than failing.

### J7. Licensing gate

- [ ] J7.1 Building a system's header or layer requires explicit opt-in naming the
      licence (e.g. `-DFASTFHIR_EXTERNAL_CODES_SNOMED=ON` plus an acknowledgement
      variable). Never default to ON.
- [ ] J7.2 Record each enabled system, its release version and its licence in
      `THIRD_PARTY_NOTICES.md` at configure time — as *user-side* notices, clearly
      distinct from what FastFHIR itself ships.
- [ ] J7.3 Counsel review before publishing. The Q15 research says LOINC is royalty-free
      and redistributable with attribution, while SNOMED CT redistribution requires
      Affiliate status plus sublicence issuance and tracking. Confirm that build-time
      compilation of a user's own licensed release, on the user's own machine, is clear
      of both. Do not ship on an assumption.

### J8. Tests

- [ ] J8.1 A synthetic fake system (a handful of invented codes under a test-only
      `FF_CodeableConceptSystem` value) so the whole mechanism — header generation, layer
      discovery, load, absence — is testable in CI with no licensed data at all.
- [ ] J8.2 Assert the ledger invariant directly: enabling any external code system must
      leave `generator/master_codes.json` byte-identical. This is the guard that stops a
      future change from quietly routing external codes into the permanent ledger.
- [ ] J8.3 Assert a stream written with a layer loaded is byte-identical to one written
      without it, for the same input. Layers validate and look up; they never encode.
- [ ] J8.4 Assert the missing-layer path: no layer present → ingest succeeds, no warning
      about validity, no crash (J1.3).
- [ ] J8.5 **UCUM: use the official conformance suite, do not invent test cases.** UCUM
      publishes `UcumFunctionalTests.xml` (Eclipse Public License 1.0) — the same suite
      other implementations certify against. Wire it into the test run so the parser is
      measured against the specification's own cases rather than the ones we happened to
      think of. Check the EPL-1.0 terms before vendoring the file; fetching it at
      configure time like the FHIR packages avoids the question entirely.
      Include the negative cases: `kft` (prefix on a non-metric atom) and an unbalanced
      `{annotation` must both be rejected.
- [ ] J8.6 **Coverage gate for the J0 commitment.** Enumerate `FF_CodeableConceptSystem`
      from `include/FF_Primitives.hpp` and assert every value is accounted for in the
      validation registry — a membership layer, the built-in UCUM grammar (J4.7), or an
      explicit documented exemption (`UNKNOWN`, `FHIR_DICTIONARY`). Adding a system to the
      enum without a validation story must fail this test.
      Model it on `tests/generator/test_compact_layout.py::test_ff_slot_width_covers_every_field_kind`,
      which does exactly this for `FF_FieldKind`. The failure mode it prevents is the one
      that already happened: `FF_CC_CODECS` handled all 17 systems while
      `print_scalar_json` handled 4, silently, because nothing compared the two lists.
      Note this gate must pass with **no licensed data present**, so it checks registry
      wiring, not table contents.

### J9. Retire the misleading placeholder

- [ ] J9.1 `dictionaries/FF_SNOMED_Concepts.cpp` is a 19-line stub with an empty array,
      compiled into the library, whose header comment says "Populate from SNOMED CT RF2
      release data". That instruction now contradicts the scope rule in
      `master_codes.json:_scope` — SNOMED values must never live in `dictionaries/`.
      Either delete the file and its build entry, or reduce it to a comment pointing here.

- Verify (block): per task. J3 and J8 are the first that can produce a running artifact,
  since neither needs licensed data (J8.1's fake system) or A8.

---

## Questions for Ryan

Answers unblock the tasks referencing them. Write answers inline after `> Answer:`.

- **Q1 (blocks C1, C2):** `recover_archive` atomicity — must repairs be all-or-nothing on
  the original archive, or should recovery always operate on a copy and swap on success?
  Copy-and-swap is safer but doubles peak disk/arena for large bundles.
  > Answer (Ryan, 2026-07-08): **Copy-and-swap, parallelized per data element.**
  > Treat every data element as a unique entity — rebuild while fixing in parallel,
  > then `mv` the recovered archive to overwrite the original corrupted file.

- **Q2 (blocks E7):** `include/FastFHIR.hpp` — (a) expand to a true umbrella covering all
  public headers (Memory, Ingestor, FieldKeys), (b) keep the current minimal set and
  document it, or (c) deprecate it in favor of explicit includes?
  > Answer: **Option (b)** — keep FastFHIR.hpp minimal, exposing only the core public API.
  > A small surface area prevents IDE type-assist overload. Users who need granular control
  > can add explicit `#include`s for individual headers. Document this explicitly in the
  > header and in README.

- **Q3 (blocks B5):** Round-trip JSON semantics — is omitting empty arrays (`[]` in,
  absent out) acceptable, and must the null-vs-absent distinction survive round-trip?
  > Answer: Empty arrays are omitted with null-offset entries indicating the optional
  > entry is absent. The only reason to preallocate empty arrays is when the size is
  > known in advance and entries will be filled later (e.g. asynchronously). The
  > null-vs-absent distinction does not need to survive round-trip.

- **Q4:** Is Bazel first-class (CI runs it, gates merges) or best-effort? Decides whether
  E1 includes Bazel jobs and how much A-class work keeps BUILD.bazel in sync.
  > Answer (Ryan, 2026-07-08): **Bazel and CMake are equal priority** — both must be
  > supported, both must be kept in sync, and CI must run both.

- **Q5 (blocks D0–D3):** Is `https://registry.fastfhir.org` a real endpoint you operate
  (or will before release), and what is its actual API shape? If aspirational, should
  D1–D3 target a local/file-based registry first with HTTP as a pluggable backend?
  > Answer (Ryan, 2026-07-08): **The registry is aspirational at this point.** Keep the
  > endpoint as a TODO; we will plan its architecture as the library develops. D1–D3
  > should not target a live HTTP backend yet — build a local/file-based registry
  > abstraction first that can be swapped for HTTP later.

- **Q6 (blocks E1):** CI platform matrix — Linux + macOS + Windows/MSVC from day one, or
  Linux-only first? (MSVC has historically caught real generator bugs GCC/Clang missed.)
  > Answer (Ryan, 2026-07-08): **All three from day one** — Linux, macOS, Windows/MSVC.
  > First-class support for all OS and architecture. Use GitHub Actions. Do not defer
  > any platform; backtracing new errors later is not acceptable.

- **Q7:** Should a known-good `generated_src/` snapshot ever be committed so builds/CI
  work without network access to HL7/packages.fhir.org, or is network-at-configure an
  accepted requirement?
  > Answer (Ryan, 2026-07-08): **Network at configure time is the accepted requirement.**
  > HL7 owns the FHIR specification; FastFHIR only controls the binary translation of it.
  > We will not snapshot or redistribute FHIR definitions.

- **Q8:** Priority between Block C (recovery) and Block D (WASM registry) if capacity is
  limited — which lands first?
  > Answer (Ryan, 2026-07-08): **Block C (recovery) first.** Block D is nice infrastructure
  > but not as critical to the initial rollout.

- **Q9 (blocks C6):** `src/FF_Builder.cpp:164` — is single-threaded mutation (`amend_*`)
  an accepted API contract (then: document it and delete the NOTE), or should the
  already-assigned check become an atomic CAS so concurrent enrichment is safe?
  > Answer (Ryan, 2026-07-08): **Implement atomic compare-and-swap.** The already-assigned
  > check in `amend_pointer`/`amend_resource`/`amend_variant` must become an atomic CAS
  > so concurrent enrichment is safe. Delete the NOTE comment once done.

- **Q10 (blocks I2, I5, H3, H5):** License decision. Ryan has approved changing the
  license in principle to ensure adoption (2026-07-08). Stated threat model: a large EHR
  vendor (e.g. Epic) copying the work into a privately divergent derivative that
  fragments the format. Note: no code license prevents cleanroom reimplementation of a
  wire format (formats/interfaces aren't copyrightable) — anti-fragmentation comes from
  the trademark + conformance-suite layer, not copyright.
  **Standing recommendation (Claude, 2026-07-08):**
  (1) Code → **MPL-2.0**: proprietary products may link/embed freely (adoption,
  registry-friendly for H3/H5), but any shipped modification to FastFHIR source files
  must be published — a private divergent fork of the core is not possible;
  (2) Trademark "FastFHIR" + conformance policy: only implementations passing the
  official conformance suite (seeded by the A4 wire gate + B5 round-trip corpus) may use
  the name or claim compatibility — this, not the license, is the Epic defense;
  (3) `docs/SPEC.md` → CC-BY-4.0 so anyone can implement a *conforming* reader;
  (4) adopt a CLA/DCO now (single-author moment) to preserve future dual-licensing.
  Rejected: AGPL (deters the adopters, not just Epic), LGPL (static-link relinking
  friction for C++), plain Apache-2.0/MIT (no fork-publication obligation).
  > Answer (Ryan, 2026-07-08): **MPL-2.0, with the full recommendation package** —
  > trademark/conformance policy, CC-BY spec posture, DCO. Implemented in I5.

- **Q11 (blocks G4):** Approve allocating a new permanent algorithm constant next to
  `FF_CHECKSUM_*` in `include/FF_Primitives.hpp` for an Ed25519 signature footer
  (authenticity, not just integrity)? This is a wire-constant allocation, so it needs
  your explicit value assignment.
  > Answer (Ryan, 2026-07-08): **Deferred.** Needs more discussion. See explanation below.

- **Q12 (blocks F2, I3.5):** Which benchmark results are publishable now — on what
  hardware were the canonical numbers produced, against which competitor library
  versions, and at which FastFHIR-benchmark commit? The README table must be
  reproducible from a stated commit of
  <https://github.com/ryanlandvater/FastFHIR-benchmark>.
  > Answer (Ryan, 2026-07-08): **Review the benchmark repo at `../FastFHIR_Performance/`**
  > for context. Benchmarks will be published with the specification paper, not now.

- **Q13 (blocks I1):** Wire-format stability statement — is the format already frozen
  (R4/R5 streams written today will parse forever), or is there a planned
  format-freeze milestone that ends the alpha caveat? SPEC.md's compatibility section
  needs the exact wording.
  > Answer (Ryan, 2026-07-08): **The wire format is NOT frozen.** We are in active alpha
  > development. The alpha caveat stays until a formal format-freeze milestone.

- **Q14 (blocks J1):** Terminology pack granularity and link model — one pack per
  CodeSystem (`fastfhir_loinc`, `fastfhir_snomed`, matching `FF_CodeableConceptSystem`
  1:1), or coarser? And static library per pack behind a CMake option, or runtime-loaded
  plugin? A static library keeps the named constants compile-time and costs nothing when
  unlinked, which is what "type assist" needs; a plugin would push constants to runtime
  lookup. Also confirm the term "pack" — "extension" collides with FHIR `Extension`
  elements and with Block D's WASM extension codecs.
  > Answer: I would like to do a runtime loaded plugin like a dylib but I want to use the layer model used by Vulkan. If the layer is available we can use it but if it's not loaded at runtime ignore the checks. The same is true for these external code extension public headers. We will have them generated so they can work like field keys FF_EXTERNAL_CODE::LOINC::SODIUM_MOLAR_whatever... can be done programatically by including #include FastFHIR/ExternalCodes/LOINC.hpp

- **Q15 (blocks J2):** Acquisition. LOINC and SNOMED CT both require a registered account,
  so neither can be downloaded unattended. Should the default path be **user-supplies-the-
  release-file** (FastFHIR only compiles what is already on disk), with automated download
  offered solely for sources that permit it (e.g. ICD-10 CM from CMS)? Or should there be
  no download path at all?
  > Answer: This is tough. I guess they should have to point to the spec. Please do more research on solutions to this.
  >
  > Answer (Ryan, 2026-07-30, after the research below): **Tier A — the user points at a
  > release file they already hold. Local data only.** A terminology server is rejected as
  > the default: it slows things down and makes ingest depend on a network service.
  > Tier D is deferred rather than designed out (see J2.3), because a non-Affiliate SNOMED
  > user has no other route and may need it later.
  >
  > The decisive constraint is in J4: validation runs **inline on the write path** when a
  > layer is linked, so a server-backed layer would mean a network round-trip inside
  > `ENCODE_FF_CODE`. Wrong shape, not merely slow.

  **Research (Claude, 2026-07-30) — findings behind that decision.**

  The sources are not uniform, so one policy cannot cover them:

  - **LOINC** is royalty-free and *may* be redistributed, including in commercial
    software, but any database or application using it must display the copyright
    notice and licence acknowledgement. Download still requires accepting the terms.
    Regenstrief also runs its own FHIR terminology service.
  - **SNOMED CT** is the opposite. Redistributing it inside a product requires the
    *distributor* to hold an Affiliate licence **and** to issue sublicences to every
    downstream user and report them to SNOMED International. Free in Member countries
    (US via NLM), chargeable elsewhere. FastFHIR must never take this on — which is
    exactly why J0 invariant 3 exists.
  - **NLM UTS Download API** solves the "unattended" problem *for the user*: a UTS
    account yields a personal API key that can fetch SNOMED CT, RxNorm and UMLS
    releases in a single command. The key and the data stay with the user; FastFHIR
    holds neither.
  - **Public domain**: ICD-10-CM (CMS/NCHS), NDC (FDA), CVX (CDC) can be fetched with
    no credentials.
  - **CPT** is AMA-licensed and paid — user-supplied file only, never downloadable.
  - **Terminology servers are HL7's own answer.** The FHIR spec states that code system
    *contents* are not distributed via FHIR resources; they are assumed known to the
    server, which exposes `$validate-code`. A server-backed layer therefore needs no
    local data and no licence on our side at all. Cost: network I/O, so it cannot sit
    inline on the hot path.

  **Recommendation:** default to **Tier A (user points at their own release file)** — it
  is the only option that works for every source including CPT, and it matches your
  instinct that they should point to the spec. Offer Tier B (their own UTS key) as
  convenience for SNOMED/RxNorm, Tier C only for public-domain sources, and Tier D
  (terminology server) as a distinct layer implementation for users who would rather
  not hold data locally. Confirm and I will fold the choice into J2.2.

  Sources: [LOINC copyright and licence](https://loinc.org/kb/license/),
  [Getting LOINC](https://loinc.org/get-started/getting-loinc/),
  [SNOMED CT licensing (SNOMED International)](https://docs.snomed.org/snomed-ct-practical-guides/vendor-introduction-to-snomed-ct/7-licensing),
  [SNOMED CT Affiliate License (NLM)](https://www.nlm.nih.gov/research/umls/knowledge_sources/metathesaurus/release/license_agreement_snomed.html),
  [UMLS UTS automating downloads](https://documentation.uts.nlm.nih.gov/automating-downloads.html),
  [FHIR terminology service](https://hl7.org/fhir/R4/terminology-service.html),
  [LOINC FHIR terminology service](https://loinc.org/fhir/)

- **Q16 (blocks J4.2):** Policy when a code fails membership validation against a linked
  pack — reject the ingest, warn and store, or store and flag on the block? Ingest is
  permissive today. Silently dropping clinical data is not acceptable, so the real choice
  is between hard failure and a logged warning.
  > Answer: Logged warning. We aren't the adjudicators but it should be loud. There should be critical warnings that are impossible to miss. Using our FF_EXTERNAL_CODE::LOINC::SODIUM_MOLAR_whatever should make it impossible to mess up but others could if they're just hot typing in the code.

---

## Execution plan (current session)

**Principle:** one task per commit, verify before moving on. Blocks executed in dependency order.

| Phase | Block | Tasks | Status |
|---|---|---|---|
| 1 — Build hygiene | A2 | Normalize `#include` paths (A2.1→A2.5), full rebuild (A2.6) | Starting now |
| 1 — Build hygiene | A3 | Fix stale API examples in `FastFHIR.hpp` doc comment | After A2 |
| 1 — Build hygiene | A4 | Implement wire-format gate (`tests/generator/test_wire_format.py`) | After A3 |
| 1 — Wire gate | A15–A18 | Re-arm the vacuous witness sections, prefix-based vtable check, R4-prefix invariant, R4/R5 divergence guard (IFE audit 2026-08-12) | After A4 — A15 and A16 first; they protect data already on disk |
| 2 — Round-trip | B5 | JSON round-trip fidelity triage (B5.1, B5.2) | After A4 |
| 2 — Round-trip | B1–B4, B6 | Remaining B-block tests | After B5 |
| 3 — Recovery | C1–C2, C6 | Recovery orchestrator + policy + CAS | After B-block |
| 3 — Recovery | C3–C5, C7–C8 | Header repair, root reconciliation, version guard, Python, telemetry | After C1/C2/C6 |
| 4 — CI | E1 | GitHub Actions multi-platform CI | After A-block |
| 4 — Hygiene | E7 | `FastFHIR.hpp` minimal umbrella per Q2 | After E1 |
| 5 — Spec | I1 | SPEC.md with alpha caveat per Q13 | After B5 |
| 6 — WASM | D0–D12 | Local registry first per Q5 | After C-block |
| 7 — Strategic | F, G, H | Benchmarks, security, packaging | After CI + spec |

---

## B5 triage results

*(populated by task B5.2 — leave empty until then)*

| JSON path pattern | Class | Rationale | Disposition |
|---|---|---|---|
