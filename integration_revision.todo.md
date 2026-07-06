> **SUPERSEDED (2026-07-06)** — Pending items from this document were re-verified and
> consolidated into [`TASKS.md`](TASKS.md); stale items were dropped there with rationale.
> This file is retained as a historical record only. Do not work from its checklists.

# Integration Revision: Round-Trip DOM Parity

## Goal

Verify that **FHIR JSON → ingest → FFHR → `print_json`** produces a JSON DOM
structurally equivalent to the original input — proving zero data loss and
semantic fidelity on real-world patient bundles.

## Missing Today

The Synthea test (`test_readme.cpp` Example 12) only pipeline-smokes:
- `ingest()` succeeds
- seal + re-parse succeeds
- root handle is non-null

It never calls `print_json`, never compares against the original input, and
would pass if an entire field category silently vanished.

---

## Test Infrastructure

### A. Normalised JSON DOM comparison

We need a comparator that is tolerant of cosmetic differences that are **not**
data-loss:

| Difference | Accept? | Rationale |
|---|---|---|
| Key ordering within a JSON object | ✅ Yes | FHIR JSON has no required key order |
| Whitespace, indentation | ✅ Yes | Formatting-only |
| Trailing zeros on decimals (`2.0` vs `2.00`) | ✅ Yes | FHIR `decimal` is double; round-trip may normalise |
| Empty arrays vs absent arrays | ❌ **Flag** | Loss of cardinality intent |
| Null vs absent | ❌ **Flag** | Semantic difference in FHIR |
| Extra fields in output | ⚠️ **Warn** | May indicate phantom data |
| Missing fields in output | ⚠️ **Warn** | Potential data loss |
| Numeric type mismatch (`"0"` vs `0`) | ❌ **Flag** | Schema violation |
| Code/value mismatch | ❌ **Flag** | Data corruption |

**Approach**: Use Python (`json.loads` → recursive key diff) for the first pass
— fast to write, easy to iterate.  Move to a C++ JSON diff library later if
the Python test harness becomes a bottleneck.

**Rejected alternatives:**
- Naive string diff (`diff`): whitespace and key-order noise drown out real diffs.
- C++ JSON lib: higher friction for a first pass; defer until Python infra is proven.

### B. Test fixture design

Each test case operates on a single Synthea patient bundle JSON file.

```
For each Synthea JSON fixture F:
  1. Load F as string → input_dom = json.loads(F)
  2. Ingest F into a Memory arena via Ingest::Ingestor
  3. Seal the arena (builder.finalize)
  4. Re-parse via Parser
  5. Capture: output_json = parser.print_json(oss)
  6. Load: output_dom = json.loads(output_json)
  7. diff_doms(input_dom, output_dom)
  8. Report: PASS / FIELD_MISMATCH / VALUE_MISMATCH / EXTRA_KEYS / MISSING_KEYS
```

The diff function must **not** short-circuit on the first mismatch — collect all
differences and report them in a structured list so we can triage systematically.

---

## Implementation Plan

### Phase 1 — Python harness (lowest friction)

| # | Task | File | Done |
|---|---|---|---|
| 1.1 | Write `diff_doms(a, b, path="")` → `list[DiffEntry]` | `tests/python/roundtrip_diff.py` | ✅ |
| 1.2 | Write C++ harness (`ff_roundtrip`) + Python runner | `tests/cpp/ff_roundtrip.cpp` + `tests/python/test_roundtrip.py` | ✅ |
| 1.3 | Write `test_roundtrip_all()` — discover fixtures, iterate | `tests/python/test_roundtrip.py` | ✅ |
| 1.4 | Register in `CMakeLists.txt` as `py_roundtrip` (depends on ff_roundtrip target + Synthea fixtures) | `CMakeLists.txt` | ✅ |

**`DiffEntry` schema:**
```python
@dataclass
class DiffEntry:
    path: str           # JSON pointer, e.g. "/Patient/name/0/given"
    kind: str           # "missing_key" | "extra_key" | "value_mismatch" | "type_mismatch"
    expected: Any       # value from input DOM
    actual: Any         # value from output DOM
```

### Phase 2 — Synthea fixture management

| # | Task | File | Done |
|---|---|---|---|
| 2.1 | Verify `FASTFHIR_SYNTHEA_DIR` lands at least one `.json` bundle at configure time | `CMakeLists.txt` (already present) | ☐ |
| 2.2 | Add a Synthea CMake download target that fetches a **known-version** bundle (not just "first .json in directory") to make test results reproducible | `CMakeLists.txt` | ☐ |
| 2.3 | Pin a specific Synthea release (e.g. `synthea-default-2025-08-01`) and reference its known SHA-256 in the test | `tests/python/test_roundtrip.py` | ☐ |

Known-version fixtures are critical — if Synthea changes its output format,
test failures from format drift must be distinguishable from real regressions.

### Phase 3 — C++ test (full speed, no Python dependency)

Once the Python harness has proven the diff approach and triaged all current
mismatches, port the test to C++ to eliminate the Python interpreter dependency
from CI.

| # | Task | File | Done |
|---|---|---|---|
| 3.1 | Add a JSON DOM node class or use an existing lightweight JSON parser (e.g. `simdjson` — already a dependency) to load & compare | `tests/cpp/` | ☐ |
| 3.2 | Implement `recursive_diff(const simdjson::dom::element& a, const simdjson::dom::element& b)` | `tests/cpp/` | ☐ |
| 3.3 | Write `test_synthea_roundtrip_dom()` — uses same fixture as Example 12 but validates full DOM | `test_readme.cpp` new Example 13 | ☐ |

### Phase 4 — Field-level regression suite

For each field that the `print_json` round-trip differs from the input, decide:

| Disposition | Action |
|---|---|
| **Legitimate difference** | Document why (e.g. "code system enum serialises as integer, not URI") and add to the allow-list in the diff function |
| **Bug** | File a fix, add a focused unit test that reproduces the specific mismatch |
| **Generator gap** | Add an integration test that will pass once the fix lands, mark `XFAIL` |

---

## Known Open Questions

1. **Empty arrays** — Synthea emits `[]` for many empty collections. Does
   `print_json` preserve `[]` or omit them?  If omitted, is that acceptable?
2. **`id` format** — Synthea uses UUIDs (`123e4567-...`).  Does the FFHR
   dictionary encode these as dictionary codes or inline strings?  Round-trip
   must preserve the original UUID text.
3. **Extensions** — Synthea heavily uses extensions (race, ethnicity,
   birthplace, etc.).  Are they preserved through `print_json` or stripped?
   This is a key quality metric.
4. **Contained resources** — Synthea sometimes emits `contained`.  Does ingest
   handle `contained` and does `print_json` reproduce them?
5. **Narrative** — `Resource.text` (Narrative) is present in Synthea output.
   Is it ingested and re-emitted faithfully?
6. **CodeableConcept text** — Many CodeableConcepts have a `text` property plus
   `coding[]`.  Does re-emission preserve both?

---

## Success Criteria

- [ ] All 5 open questions above are answered with evidence from a round-trip test.
- [ ] Python round-trip test runs in CI and reports PASS/FAIL per fixture.
- [ ] No unexpected field-level differences between Synthea input and `print_json` output.
- [ ] Every expected difference is documented in the allow-list with rationale.
- [ ] C++ round-trip test (Phase 3) matches Python test results exactly.
