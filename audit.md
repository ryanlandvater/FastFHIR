> **SUPERSEDED (2026-07-06)** — Pending items from this document were re-verified and
> consolidated into [`TASKS.md`](TASKS.md); stale items were dropped there with rationale.
> This file is retained as a historical record only. Do not work from its checklists.

# Generator Refactor — Audit

> Scope: the untracked `generator/` package (decomposition of the deleted
> `tools/generator/ffc.py` monolith), its tests, `pyproject.toml`, and the
> three tracked text edits (`CMakeLists.txt`, `README.md`,
> `project_progress.prompt.md`).
>
> Invariant being judged: **wire-convention correctness and internal
> consistency** (alpha-stage — *not* byte-identity with the old tree).

---

## Verdict (UPDATED)

> **Current status:** The structural refactor is complete and `fastfhir_obj`
> compiles with **zero errors** on MSVC 19.51 (VS 2026, C++20). The two critical
> codegen bugs (C-1, C-2) identified in this audit have been fixed. See
> `refactor_history.md` for the complete bug list and fixes applied.

---

## Original Verdict

The structural refactor is faithful to `generator_refactor_plan.md` and the
module boundaries are clean (`model/` pure, `emit/` `model → str`, orchestrator
emitter-free). **But the change as it currently sits is not shippable**, for two
reasons that are independent of style:

1. Two **real codegen bugs** in `emit/dictionary.py` produce C++ that will not
   compile (one is a missing f-string prefix; one is N-fold method
   redefinition). At least one is a regression introduced by the relocation.
2. The change is **untracked**, so the staged commit is a pure deletion of the
   generator + its build target with the replacement invisible to git.

Fix those and it's in good shape.

---

## 🔴 Critical — generated C++ will not compile

### C-1. `FF_{v_name}_Resolve` leaks the literal `{v_name}` placeholder
`generator/emit/dictionary.py` ~L173–182. The function is opened with an
f-string, but three body lines are **plain** strings missing the `f` prefix:

```python
hpp += f"inline const char* FF_{v_name}_Resolve(uint32_t code) {{\n"
hpp += "    int lo = 0, hi = FF_{v_name}_CODE_COUNT;\n"        # <- not an f-string
hpp += "        uint32_t mc = FF_{v_name}_TABLE[mid].code;\n"  # <- not an f-string
hpp += "        if (mc == code) return FF_{v_name}_TABLE[mid].label;\n"  # <- not an f-string
```

Generated output contains the literal text `FF_{v_name}_CODE_COUNT` /
`FF_{v_name}_TABLE` instead of `FF_R5_CODE_COUNT` / `FF_R5_TABLE`. Every
`FF_*_Dictionary.hpp` fails to compile. This is a wire-adjacent function
(code → string resolution) so it is not cosmetic.

**Fix:** add the `f` prefix to all three lines (or make the whole block one
f-string). Trivial, but must be caught — exactly the class of bug the wire gate
is *supposed* to catch but currently can't (see G-1).

### C-2. `FF_Dictionary::resolve` is redefined once per version
`generate_master_dictionary` ~L223–231 emits, inside a single `struct
FF_Dictionary { ... }`, one `static inline const char* resolve(uint32_t)` per
active version:

```cpp
struct FF_Dictionary {
    static inline const char* resolve(uint32_t code) { if (FF_R4_Resolve(code)) ...; return nullptr; }
    static inline const char* resolve(uint32_t code) { if (FF_R5_Resolve(code)) ...; return nullptr; }  // redefinition
};
```

Same signature twice in one class → C++ redefinition error. Even if it compiled,
each body only consults its own version then `return nullptr`, so a hit in R5
would never be reached after the R4 method returned null. The intended behavior
(try each version, fall through) requires **one** method whose body chains the
per-version lookups:

```cpp
static inline const char* resolve(uint32_t code) {
    if (const char* r = FF_R4_Resolve(code)) return r;
    if (const char* r = FF_R5_Resolve(code)) return r;
    return nullptr;
}
```

**Fix:** hoist the method open/close outside the per-version loop; emit only the
`if (const char* r = FF_<v>_Resolve(code)) return r;` line inside the loop.

---

## 🟡 Wire-convention consistency

These are correct *today* but are the load-bearing invariants — flagging for
explicit review since the alpha goal is "follows byte convention."

### W-1. `_code_hash` collisions are silent (data-loss risk)
`emit/dictionary.py::_code_hash` truncates SHA-256 to 31 bits. With ~thousands of
codes across all ValueSets the birthday bound makes a collision plausible
(`p ≈ 50%` near ~54k strings; non-negligible well before that). On collision,
two distinct code strings map to the **same** uint32 wire value:

- The enum gets two constants with the same value (legal C++, silently wrong).
- The sorted `FF_*_TABLE` gets two entries with equal `code`; binary-search
  `resolve` returns whichever the search lands on — the *other* string is
  unrecoverable from the wire.

The docstring claims stability ("same string → same code") but says nothing
about uniqueness. The old `audit.plan.md §2` reportedly called this
"wire-critical." There is **no collision guard**. Recommend: during
`code_assignments`, detect a hash clash within a version and `raise` (fail-loud,
per the style guide) so a collision forces a deliberate resolution rather than
silent corruption. This is the single most important wire-correctness gap.

### W-2. `_FF_CODE_NULL` is duplicated as a magic constant in two languages
`0xFFFFFFFF` lives in `emit/dictionary.py` (`_FF_CODE_NULL`) and the comment says
it "MUST match" `include/FF_Primitives.hpp` and the wire witness uses the same
literal. Three independent copies of one wire constant. If `FF_CODE_NULL` ever
moves, three places drift. Convention-wise this is the kind of cross-language
constant that should be parsed from the C++ header (like `_parse_recovery_tags`
already does for recovery tags) rather than re-declared. The recovery-tag path
does this right; the code-null path does not. Make them consistent.

### W-3. Recovery tags: good — parsed, not hardcoded
`_parse_recovery_tags` reads `include/FF_Recovery.hpp` as the single source of
truth, and `_scalar_recovery_tag` only maps FHIR type → tag *name* (not value).
This is the correct pattern: the wire values live in the C++ header, Python
references them by name. No drift possible. This is the model W-2 should follow.

### W-4. V-Table offsets are symbolic — model correctly omits `offset`
`model/types.py::Field` deliberately stores no literal `offset`; position is
`order + per-field size constant + HEADER_*_SIZE`, resolved by the C++ compiler
via symbolic sums (`RECOVERY = VALIDATION + VALIDATION_S`). The witness captures
exactly that triple. This is sound and keeps the Python from re-deriving (and
potentially diverging from) the C++ offset arithmetic. No action.

---

## 🟡 Test harness — the one hard gate is currently inert

### G-1. The wire gate skips instead of running (no baseline committed)
`tests/generator/test_wire_format.py` is described as "THE ONE hard gate," but:

- The baseline `tests/generator/golden/wire_witness.json` **does not exist**.
- On a missing baseline the `baseline` fixture calls `pytest.skip(...)`, so all
  three gate tests (`recovery_tags`, `dictionary_codes`, `vtable_layout`)
  **skip silently**. A green `pytest -q` proves nothing.

Until that JSON is committed (generated from a known-good `generated_src/`), the
refactor has **zero** automated wire coverage — and bugs C-1/C-2 would sail
through. Generate and commit it:

```
python -m tests.generator.wire_witness generated_src tests/generator/golden/wire_witness.json
```

…but only *after* C-1/C-2 are fixed, otherwise you bake broken output (well,
C-1/C-2 are in source text the witness ignores — see G-2 — but regenerate
post-fix regardless to be safe).

### G-2. The witness can't see C-1/C-2 — gate scope gap
`wire_witness.py` extracts tags, `FF_*_CODE_*` values, and V-Table structure via
regex. It does **not** parse function bodies, so neither the `{v_name}` leak
(C-1) nor the duplicated `resolve` (C-2) would ever be caught by the gate even
once the baseline exists. The gate proves *wire constants* are stable; it does
**not** prove the generated C++ *compiles*. That's an acceptable scope for a wire
gate, but it means a compile smoke-test (regenerate → `cmake`/`tcc`-parse one
dictionary header) is needed as a second, cheap gate. Right now nothing checks
compilability of generated output in the Python suite.

### G-3. `conftest.py` fallback masks a broken generator (post-cutover)
`regenerated_dir` resolution ends by falling back to the in-repo
`generated_src/` when `python -m generator` fails — and `_try_regenerate`
swallows a non-zero return (prints to stderr, returns `None`). Combined with G-1
this means: generator broken → fixture silently tests the **stale committed
tree against itself** → green. The fallback was justified "during migration,"
but S6 cutover is done and `tools/generator/` is deleted, so the fallback now
converts "generator is broken" into "tests pass." Post-cutover it should
**hard-fail** if `python -m generator` produces no output. Delete the
`generated_src` fallback branch.

---

## 🟡 Repo hygiene

### H-1. The replacement is untracked — staged commit is a pure deletion
`git status`: `generator/`, `tests/generator/`, `pyproject.toml`,
`generator_refactor_plan.md`, `audit.repomap.md` are **untracked**; the only
staged changes delete `tools/generator/*` and repoint CMake to
`python -m generator`. A checkout of this commit has a build that invokes a
package that isn't in the tree. **Stage the new package in the same atomic commit
as the deletions + CMake repoint.**

### H-2. `__pycache__` is committed/untracked alongside sources
`generator/**/__pycache__/*.pyc` show up in the tree. Add to `.gitignore`
(`__pycache__/`, `*.pyc`). Never commit bytecode.

### H-3. False "path bridge" comment in `generator/__init__.py`
The header still claims symbols "resolve to the legacy tools/generator modules
via the path bridge in pipeline.py" and that "S1 imports point at old modules."
`tools/generator/` is deleted and there is no path bridge — the comment is now
false and will mislead. The `Relocated from ffc.py lines NNNN` provenance
comments scattered through `model/`/`emit/` are fine as history, but this one
describes live behavior that no longer exists. Correct it.

### H-4. Branch is behind origin
`git status`: "behind 'origin/main' by 1 commit, can be fast-forwarded." Rebase
before committing this work to avoid a needless merge.

---

## 🟢 What's right

- Architecture matches the plan: `model/` is pure (no C++ strings), `emit/` is
  `(model) -> str` with no I/O, `pipeline.py` carries no emitters (SV-4 fixed —
  `generate_known_extensions` is now `emit/extensions_known.py`).
- SV-1 fixed: one `write_if_changed` / `auto_header` in `emit/header.py`;
  consumers import it (verified in `dictionary.py`).
- `python -m generator` is path-independent; CMake repoint is the correct call,
  and `--output-dir` threading enables the temp-dir regeneration the test
  fixture relies on.
- `pyproject.toml` correctly scopes black/ruff to `generator/` + tests and
  excludes generated C++ — respects the two-style-regime rule. Ruff rule set
  (E,F,I,UP,B,ANN) enforces the style guide's hard rules.
- `Field`/`Block` dataclasses with `extra` passthrough are a sound, loss-free
  adapter; `to_dict`/`from_dict` round-trip keeps emitters dict-fed during
  migration without forcing a big-bang rewrite.
- Determinism is handled: `mappings` sorted by string, code table sorted by
  code, version configs sorted by `_version_sort_key` — output is stable
  regardless of bundle entry order. Good.
- README registry-configurability edits and CMake invocation edit are accurate
  and internally consistent.

---

## Fix order (blocking → nice-to-have)

1. **C-1** add missing `f` prefixes in `FF_*_Resolve` body. *(compile)*
2. **C-2** single `resolve` method chaining per-version lookups. *(compile)*
3. **W-1** collision guard in `_code_hash` assignment — `raise` on clash. *(data integrity)*
4. **H-1** stage the new package atomically with the deletions. *(repo)*
5. **G-1** generate + commit `wire_witness.json` (post-fix). *(arm the gate)*
6. **G-3** drop the `generated_src` fallback in `conftest.py`; hard-fail. *(arm the gate)*
7. **W-2** parse `FF_CODE_NULL` from the C++ header instead of re-declaring. *(de-dup constant)*
8. **H-2/H-3/H-4** gitignore `__pycache__`, fix the false `__init__.py` comment, rebase.
9. **G-2** add a generated-C++ compile smoke test as a second gate. *(catches C-1/C-2 class)*
