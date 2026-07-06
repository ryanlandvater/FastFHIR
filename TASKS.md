# FastFHIR — Consolidated Task Backlog

> **This file is the single source of truth for pending work.** It consolidates and
> supersedes the checklists in `audit.md`, `audit.todo.md`, `integration_revision.todo.md`,
> `project_progress.prompt.md`, `generator_refactor_plan.md`, and `unification.plan.md`
> (all retained as historical records). Every item below was re-verified against the tree
> on 2026-07-06; stale items from the old docs were dropped or reworded.
>
> Read `CLAUDE.md` first — it defines the invariants you must not break.

## Execution contract (for agents working these tasks)

1. **Claim one block (or one lettered subsection) per session.** Blocks are ordered but
   independent unless a task says otherwise; A must land before B4/B5 make sense.
2. **Verify before editing.** Line numbers below were correct on 2026-07-06 and may drift.
   Re-locate each reference with `grep` before changing anything. If the code no longer
   matches the task's "Current state," stop and flag the task instead of guessing.
3. **A task marked `Blocked on Q#` must not be started** until that question in the
   [Questions for Ryan](#questions-for-ryan) section has a written answer.
4. **Run the task's Verify command** (and the full relevant test suite) before checking a box.
5. **Never renumber wire constants** (RECOVERY_TAG values, dictionary code IDs, vtable
   offsets) as part of any task here. See CLAUDE.md → Hard invariants.
6. Check the box, note the commit hash beside it, and stop. One logical change per commit.

---

## Block A — Build & correctness fixes (do first)

### A1. Fix `ff_ingest` source-file case mismatch (build blocker)
- [ ] The file on disk is `tools/ingestor/FF_Ingest.cpp`, but both build systems reference
      lowercase `ff_ingest.cpp`, breaking builds on case-sensitive filesystems whenever
      `FASTFHIR_BUILD_INGESTOR=ON`.
  - Files: `CMakeLists.txt:178` (`add_executable(ff_ingest tools/ingestor/ff_ingest.cpp)`)
    and `BUILD.bazel:49` (`srcs = ["tools/ingestor/ff_ingest.cpp"]`).
  - Change both to `tools/ingestor/FF_Ingest.cpp`.
  - Verify: `grep -rn 'ff_ingest.cpp' CMakeLists.txt BUILD.bazel` returns nothing;
    `cmake -S . -B build -DFASTFHIR_BUILD_INGESTOR=ON && cmake --build build --target ff_ingest`
    succeeds (requires network for the generator on first configure).

### A2. Normalize `#include` paths to bare names
Relative `"../include/..."` and `"../generated_src/..."` includes defeat the include-dir
configuration and break out-of-tree layouts. Both include dirs are already on the compiler
path (`FASTFHIR_INCLUDE_DIR`, `FASTFHIR_GENERATED_DIR` in CMakeLists.txt; `includes` in
BUILD.bazel). One subtask per file; each is independently committable.

- [ ] A2.1 `src/FF_Parser.cpp` — lines 13–17: `"../include/FF_Utilities.hpp"` →
      `"FF_Utilities.hpp"`, same for `FF_Parser.hpp`, `FF_SIMD.hpp`, `FF_Dictionary.hpp`,
      and `"../generated_src/FF_Reflection.hpp"` → `"FF_Reflection.hpp"`.
- [ ] A2.2 `src/FF_Compactor.cpp` — lines 1–5 (`FF_Compactor.hpp`, `FF_Queue.hpp`,
      `FF_Utilities.hpp`, `FF_Reflection.hpp`).
- [ ] A2.3 `src/FF_Ingestor.cpp` — lines 6–11 (`FF_Queue.hpp`, `FF_SIMD.hpp`,
      `FF_Utilities.hpp`, `FF_Bundle.hpp`, `FF_IngestMappings.hpp`, `FF_KnownExtensions.hpp`).
- [ ] A2.4 `src/FF_Extensions.cpp:7` and `src/FF_Dictionary.cpp:16–17`.
- [ ] A2.5 Generator emitters must emit bare includes too, or regeneration reintroduces the
      problem: `generator/emit/master_dictionary.py:103` emits
      `#include "../include/FF_Dictionary.hpp"`. Sweep all emitters:
      `grep -rn '"\.\./' generator/` and fix each occurrence.
- [ ] A2.6 Rebuild everything and run the full C++ test suite after A2.1–A2.5.
  - Verify (whole block): `grep -rn '"\.\./' src/ include/ generator/` returns nothing;
    clean configure + `cmake --build build --target build_all` + `ctest --test-dir build`.

### A3. Fix stale API in `include/FastFHIR.hpp` doc comment
- [ ] The Quick Start examples at `include/FastFHIR.hpp:17–73` use an API that no longer
      exists (`FastFHIR::Parser::create(...)`, `.value().as_string()`,
      `FastFHIR::FieldKeys::Observation::STATUS`, `FastFHIR::Builder builder;` with no
      Memory arg). Current API (per README.md and `tests/cpp/test_readme.cpp`):
      `FastFHIR::Parser parser(data, size)`, `root[FastFHIR::Fields::PATIENT::ID]`,
      `node.as<std::string_view>()`, `Builder(mem, FHIR_VERSION_R5)`.
  - Rewrite both `@code` blocks to compile against the current API; source the snippets from
    working examples in `tests/cpp/test_readme.cpp`.
  - Verify: paste each snippet into a scratch `.cpp` in `tests/cpp/`, compile it against
    `fastfhir_obj`, then delete the scratch file.

### A4. Arm the wire-format gate (currently inert — a green pytest proves nothing)
The generator test suite's "one hard gate" (`tests/generator/test_wire_format.py`) skips
silently because no baseline exists, and its fixture falls back to a stale tree on failure.

- [ ] A4.1 Generate and commit the baseline:
      `python -m tests.generator.wire_witness <generated_src_dir> tests/generator/golden/wire_witness.json`
      from a known-good generation (`python -m generator` first; needs network).
- [ ] A4.2 Delete the `generated_src/` fallback in `tests/generator/conftest.py` (lines
      ~43–64): post-cutover, a broken generator must fail the suite, not silently test the
      committed tree against itself. `_try_regenerate` must hard-fail (raise) on non-zero exit.
- [ ] A4.3 Add a generated-C++ compile smoke test: the wire witness only checks constants via
      regex, so an emitter bug that produces non-compiling C++ passes the gate. Add a pytest
      that regenerates into a temp dir and syntax-checks at least the dictionary +
      one resource header (e.g. `c++ -std=c++20 -fsyntax-only -I include -I <tmp>` on a TU
      that includes them). Mark it `skipif` when no compiler is on PATH.
- [ ] A4.4 Confirm deterministic output: run `python -m generator` twice into two temp dirs,
      diff them; add this as a pytest (carries forward S0.4 / M0 from
      `generator_refactor_plan.md` — the rest of that plan's checklist is historical).
  - Verify (whole block): `pytest tests/generator -q` runs the wire tests **without skips**
    and fails if `python -m generator` is broken.

### A5. Verify dictionary code-ID integrity guards in the current master-codes path
- [ ] The old audit (audit.md W-1/W-2) flagged silent hash collisions and a re-declared
      `FF_CODE_NULL` in a `dictionary.py` that has since been rewritten
      (`generator/emit/dictionary.py` now assigns sequential IDs with an overflow guard at
      line 137; `master_codes.json` is the committed source of truth). Confirm in the current
      pipeline: (a) two distinct code strings can never receive the same ID; (b) committed
      IDs in `master_codes.json` are never reassigned on regeneration (drift detection);
      (c) `0xFFFFFFFF` / `FF_CODE_NULL` and `FF_CODEABLE_CONCEPT_FLAG` (`0x80000000`) can
      never be produced as an ID. Add a `raise` guard + a pytest for any hole found; document
      the guarantees in a docstring in `generator/emit/dictionary.py`.
  - Verify: targeted pytest in `tests/generator/` exercising duplicate labels and the
    ID-stability path.

---

## Block B — Test coverage

Carried from `audit.todo.md` P0 (unchecked) and `integration_revision.todo.md` Phases 2–4.
Model the new C++ suites on `tests/cpp/test_primitives.cpp` (standalone `main()`, registered
via `add_ff_cpp_test` in CMakeLists.txt).

### B1. Builder unit tests — `tests/cpp/test_builder.cpp` (new)
- [ ] claim_space atomicity and write-head progression
- [ ] `amend_pointer`, `amend_resource`, `amend_variant` (success + already-assigned throw +
      out-of-bounds throw — see `src/FF_Builder.cpp:150–190`)
- [ ] Extension URL directory population
- [ ] Checksum finalization (SHA-256 callback)
- [ ] Builder hydration from an existing archive (Parser → Builder round-trip)
- [ ] Concurrent multi-threaded append (lock-free contract validation)
  - Verify: register in CMakeLists.txt via `add_ff_cpp_test`; `ctest -R cpp_test_builder`.

### B2. Parser unit tests — `tests/cpp/test_parser.cpp` (new)
- [ ] Node navigation by FF_FieldKey and by string key
- [ ] Entry value extraction: scalar, string, offset, resource reference
- [ ] Array traversal: count, index access, stride
- [ ] Choice type resolution (`resolve_choice`, kind/recovery dispatch)
- [ ] Stream-level metadata: URL directory, module registry
- [ ] `print_json` output round-trip against an expected string
  - Verify: `ctest -R cpp_test_parser`.

### B3. Golden round-trip integration test — `tests/cpp/test_roundtrip.cpp` (new)
- [ ] Ingest FHIR JSON → Builder → finalize; Parser walk; export JSON; Compactor archive →
      Parser on the compacted stream — each stage asserted against a golden hash, using the
      Synthea data CMake already downloads.
  - Note: distinct from the existing `tests/cpp/ff_roundtrip.cpp`, which is a harness binary
    driven by `tests/python/test_roundtrip.py`, not a self-asserting test.
  - Verify: `ctest -R cpp_test_roundtrip`.

### B4. Pin Synthea fixtures (reproducibility)
- [ ] CMakeLists.txt currently downloads
      `synthea_sample_data_fhir_latest.zip` ("latest" — unpinned). Pin a specific release
      URL and record its SHA-256; verify the hash at configure time (`file(DOWNLOAD ...
      EXPECTED_HASH ...)`). Update `tests/python/test_roundtrip.py` to state the pinned
      version. (integration_revision Phase 2.)
  - Verify: reconfigure from a clean build dir; ctest `py_roundtrip` still passes.

### B5. Round-trip DOM parity — close out the open questions
- [ ] Run the existing Python round-trip suite (`tests/python/test_roundtrip.py` +
      `roundtrip_diff.py`) against the pinned fixtures and produce a triage table for each
      difference class: empty arrays, UUID `id` preservation, extensions, `contained`
      resources, `Resource.text` narrative, CodeableConcept `text`+`coding`. Classify each
      as legitimate (→ allow-list with rationale), bug (→ file a Block-A/C task + focused
      test), or generator gap (→ XFAIL test). `Blocked on Q3` for the accept/flag policy on
      empty-array and null-vs-absent semantics.
- [ ] Port the diff to C++ using simdjson once triage is complete (integration_revision
      Phase 3) so CI needs no Python for this gate.
  - Verify: `ctest -R py_roundtrip` green with a documented allow-list; success criteria in
    the superseded `integration_revision.todo.md` all demonstrably met.

### B6. Recovery/corruption tests (pairs with Block C; write test skeletons first)
- [ ] Corrupted header, missing root, mismatched recovery tag, checksum-footer corruption
- [ ] Strict-fail mode vs attempted-repair mode (once C2 lands)
- [ ] In-place enrich works after recovery without JSON re-ingest
- [ ] Python: `RECOVERY_REQUIRED` maps to the expected Python exception (once C7 lands)

---

## Block C — Archive recovery subsystem

Carried intact from `project_progress.prompt.md` (Recovery Routine Backlog). Order matters:
C1/C2 define the API the rest implement. `Blocked on Q1` (atomicity strategy) for C1.

- [ ] C1. `recover_archive(...)` orchestrator — single internal recover-or-fail path when
      parser hydration fails. Location: `src/FF_Builder.cpp` near the `Builder::Builder`
      and `Builder::finalize` recovery gates (grep `RECOVERY_GATE`). **Blocked on Q1**:
      must it complete atomically (all repairs or none) or operate on a copy?
- [ ] C2. Recovery policy surface — enum/options on `Builder` (`include/FF_Builder.hpp`):
      `strict_fail` | `attempt_repair` | `read_only_salvage`. Replaces hardcoded behavior.
- [ ] C3. Header validation + repair helper for stream metadata (root, root_type, checksum
      footer pointer). Location: `src/FF_Parser.cpp` near parser constructors. Also resolves
      the two TODOs at `src/FF_Memory.cpp:196` and `:297` (attempt bounded recovery when a
      plausible FastFHIR header is found, instead of zeroing the provisional header).
- [ ] C4. Root reconciliation helper for mismatched/missing root metadata
      (`include/FF_Builder.hpp` `root_handle()` fallback + finalize preflight).
- [ ] C5. Explicit mixed-version guard with actionable error text (Builder constructor
      hydration + finalize recovery block).
- [ ] C6. Pointer/vtable safety sweep before mutation in `amend_pointer` / `amend_resource`
      / `amend_variant`. Also address the author-flagged concern at `src/FF_Builder.cpp:164`
      ("not concurrency protected") — the already-assigned check is a non-atomic
      read-then-write; decide and document whether amendments are single-threaded by
      contract or make the check atomic (CAS on the offset slot).
- [ ] C7. Map `RECOVERY_REQUIRED` native errors to structured Python exceptions
      (`python/FF_PythonBindings.cpp`, PyStream root getter / finalize wrappers).
- [ ] C8. Structured logging/metrics for recovery attempts and outcomes (route through
      `include/FF_Logger.hpp`).
  - Verify (whole block): B6 tests pass in both strict and repair modes.

---

## Block D — WASM extension subsystem

Carried from `project_progress.prompt.md` (Open TODOs table). D0 is a prerequisite for
D1–D3. `Blocked on Q5` (registry endpoint reality) for D0–D3.

- [ ] D0. Define the `FF_ExtensionRegistry` interface: configurable base URL (no hardcoded
      hostname), URL path template (e.g. `{base}/v1/modules/{url_hash_hex}/latest`), and an
      explicit error contract for network failure / non-200 (throw vs null vs cached
      fallback). **Blocked on Q5.**
- [ ] D1. `http_get_manifest()` — real TLS GET via the D0 interface
      (current stub: `src/FF_Extensions.cpp:408`).
- [ ] D2. `http_get_wasm()` — TLS download by content hash
      (current stub uses plain TCP port 80 — `src/FF_Extensions.cpp:393–406`).
- [ ] D3. TLS transport for both (OpenSSL BIO is the natural choice — already a dependency
      when the ingestor is enabled).
- [ ] D4. `FF_IsKnownExtension()` / `FF_IsNativeExtension()` — string binary search in
      predigestion.
- [ ] D5. `FF_ExtensionFilterMode` enum applied in the predigestion hot path
      (`FILTER_ALL_KNOWN` / `FILTER_NONE`, per README → Extensions §Condition 3).
- [ ] D6. `Parser::unresolved_extensions()` — offline-fallback skip log.
- [ ] D7. Path A ingest dispatch in concurrent workers (`FF_WasmExtension{Size,Store}`).
- [ ] D8. Path B round-trip export — emit stored raw JSON verbatim from `VALUE` ChoiceEntry.
- [ ] D9. End-to-end Synthea ingest verification: URL directory populated, zero known-ext
      blocks written, binary hash present in registry.
- [ ] D10. Phase 9 verification tests — EXT_REF MSB=0/1 routing, AOT enqueue, Path A/B
      round-trip.
- [ ] D11. First real WASM codec module (geolocation extension, wasi-sdk) as the reference
      example.
- [ ] D12. Binary-file GC for the disk cache — must never evict a hash referenced by a live
      `FF_MODULE_REGISTRY` in any open Parser/Builder; design the refcount/generation lock
      before implementing.
  - Invariant reminder: two SHA-256 roles, never conflated — `sha256(url)` is only a disk
    metadata filename; `sha256(wasm_bytes)` is module identity on the wire
    (`REG_ENTRY_MODULE_HASH`, offset 24, 32 bytes).

---

## Block E — Hygiene & infrastructure

- [ ] E1. CI workflow (none exists — `.github/` contains only `prompts/`). GitHub Actions:
      configure + build + `ctest` on Linux/macOS/Windows, plus `pytest tests/generator` and
      `ruff check generator tests/generator`. The generator needs network access to
      packages.fhir.org / HL7 at configure time — cache `fhir_specs/` between runs.
      **Blocked on Q6** (target platform matrix) and benefits from A1/A2 landing first.
- [ ] E2. Add `.clang-format` capturing the existing C++ style (4-space indent, aligned
      trailing comments, Allman-ish braces in headers — derive from `include/FF_Primitives.hpp`
      as the reference file) and a CI check. Do NOT reformat existing files wholesale in the
      same change; enforce on touched lines only.
- [ ] E3. Switch-case exhaustiveness audit (from `project_progress.prompt.md` Code Quality
      Backlog): `src/FF_Parser.cpp` (`print_json`, `standard_node_entries`,
      `node_lookup_field`, `standard_entry_as_node`), `src/FF_Compactor.cpp` helpers beyond
      `archive_node`, `include/FF_Parser.hpp` (`Node::as<T>()`), `include/FF_Utilities.hpp`
      (`FF_IsFieldEmpty`), `src/FF_Builder.cpp` (MutableEntry `operator=` overloads).
      Goal: no silent fall-through on a new `FF_FieldKind`; prefer exhaustive switches with
      no `default:` (or a static-assert-style fail) so the compiler flags new enumerators.
- [ ] E4. Dead code: `FF_ArrayHeader` in `include/FF_Utilities.hpp:36` has zero call sites —
      delete it, or add a comment stating it is reserved for external consumers. Re-grep for
      other zero-call-site symbols in `include/` while there.
- [ ] E5. Doc staleness sweep: (a) FastFHIR.hpp examples — covered by A3; (b) README.md
      "Generator Architecture" section still describes the pre-refactor `fetch_specs.py`/
      `ffd.py`/`ffcs.py`/`ffc.py` files under "Profile Selection" (those modules are gone —
      the module table above it is the current truth); (c) `audit.todo.md` P3 items reference
      a `.arbiter/` directory that no longer exists (dropped — do not carry forward);
      (d) confirm `python/README.md` examples run against a fresh build.
- [ ] E6. Unification plan Phase 4 status check: `dictionaries/FF_UCUM_Concepts.cpp` is
      already deleted and `master_codes.json` exists, but `generator/emit/dictionary.py`
      still exists and is live in `pipeline.py` (it now *produces* master codes rather than
      the old per-version tables — the plan's "delete dictionary.py" step is superseded).
      Confirm no other Phase-4 leftovers: grep for `FF_UCUM_STRINGS`, `kUCUMTable`,
      `FF_UCUM_CODES` and remove any dead remnants; verify the static_assert guards listed
      in `unification.plan.md §8` exist in the generated dictionaries.
- [ ] E7. Decide the fate of `include/FastFHIR.hpp` as an umbrella header (currently pulls
      in Parser/Builder/Compactor only, and deliberately excludes `FF_FieldKeys.hpp`).
      **Blocked on Q2.**

---

## Questions for Ryan

Answers unblock the tasks referencing them. Edit answers inline under each question.

- **Q1 (blocks C1):** `recover_archive` atomicity — must repairs be all-or-nothing on the
  original archive, or should recovery always operate on a copy and swap on success?
  Copy-and-swap is safer but doubles peak disk/arena for large bundles.
  > Answer:

- **Q2 (blocks E7):** `include/FastFHIR.hpp` — (a) expand to a true umbrella covering all
  public headers (Memory, Ingestor, FieldKeys), (b) keep the current minimal set and document
  it, or (c) deprecate it in favor of explicit includes?
  > Answer:

- **Q3 (blocks B5):** Round-trip JSON semantics — is omitting empty arrays (`[]` in, absent
  out) acceptable, and is null-vs-absent distinction required to survive round-trip? The
  integration plan flags both, but the binary format may not distinguish them.
  > Answer:

- **Q4:** Is Bazel a first-class supported build (should CI run it, should it gate merges),
  or a best-effort convenience? This decides whether Block E1 includes Bazel jobs and how
  much effort A-class fixes spend keeping BUILD.bazel in sync.
  > Answer:

- **Q5 (blocks D0–D3):** Is `https://registry.fastfhir.org` a real endpoint you operate (or
  plan to before release), and what is its actual API shape? If aspirational, should D1–D3
  target a local/file-based registry first with HTTP as a pluggable backend?
  > Answer:

- **Q6 (blocks E1):** CI platform matrix — Linux + macOS + Windows/MSVC from day one, or
  Linux-only first? (refactor_history shows MSVC has caught real bugs GCC/Clang missed.)
  > Answer:

- **Q7:** Should a known-good `generated_src/` snapshot ever be committed (e.g. under
  `tests/generator/golden/`) so builds and CI work without network access to HL7/
  packages.fhir.org, or is network-at-configure-time an accepted requirement?
  > Answer:

- **Q8:** Priority between Block C (recovery) and Block D (WASM registry) if agent capacity
  is limited — which lands first? (Recovery hardens existing promises; WASM completes a
  headline feature.)
  > Answer:

- **Q9:** `src/FF_Builder.cpp:164` — is single-threaded mutation (amend_*) an accepted API
  contract (document it), or should already-assigned checks become atomic CAS so concurrent
  enrichment is safe? Affects C6's scope.
  > Answer:
