# FastFHIR — Consolidated Task Backlog

> **This file is the single source of truth for pending work.** It absorbed and replaced
> the former checklist docs (`audit.md`, `audit.todo.md`, `audit.repomap.md`,
> `integration_revision.todo.md`, `project_progress.prompt.md`,
> `generator_refactor_plan.md`, `unification.plan.md`, `refactor_history.md`), which have
> been deleted; their content survives in git history. Every item below was re-verified
> against the tree on 2026-07-06.
>
> Read `CLAUDE.md` first — it defines the invariants you must not break.

---

# ▶ OPEN TOPIC — READ-PATH TRAVERSAL THROUGHPUT — INVESTIGATION COMPLETE 2026-08-19

Written 2026-08-19 out of XP-2.3. The investigation is done; what remains is
the §C fix and the §D.1/3/4/5 decisions (two of them Ryan's call).

**The claim under threat (resolved).** README:56 states *"Reading a FastFHIR
stream requires 0 heap allocations... enabling nanosecond read times..."* The
claim holds for optimized builds: at `-O3` the whole-document walk is 10.4 ms
over ~18.3M slots (~0.6 ns/slot) and `entries()` materializes 31k entries in
36 µs (~1.2 ns each). The 107 ms walk that motivated this topic was a Debug
(`-O0`) build artifact — the `ninja` preset configures `CMAKE_BUILD_TYPE=Debug`
— not a read-path defect. §C's `reflected_fields()` allocation is still a real
(if Debug-inflated) defect; see the correction below.

> **Correction (2026-08-19) — the §A numbers are Debug-build measurements.**
> The `ninja` preset configures `CMAKE_BUILD_TYPE=Debug`, so `libfastfhir.dylib`
> in `build/` is `-O0`; the report's "-O2" label does not match the build the
> numbers came from. Re-measured on a Release (`-O3 -DNDEBUG`, `build-opt/`)
> build of the same tree and fixture (min of 7):
>
> | Measurement | Debug `-O0` | Release `-O3` |
> |---|---|---|
> | `validate_FFHR_stream()` | 107.5 ms (0.46 GiB/s) — reproduces the original report | **10.4 ms (4.9 GiB/s)** |
> | `Bundle.entry.entries()` (31,042 elements) | 856 µs (`push_back`) | **36 µs** |
> | reflective walk (public API, 975k visits) | 116 ms | 25.6 ms |
> | `print_json()` → null sink | 734 ms | 197 ms |
> | `Compactor::archive()` | 908 ms | 159–191 ms |
> | `root().size()` (§C) | 0.16 µs | 17.5 ns |
>
> Consequences: (1) the "~30× gap" between the walk and shuffled header access
> (§A) is **2.8× at `-O3`** — per-slot cost is ~0.6 ns (~2 cycles), near the
> floor, and README:56's "nanosecond read times" is defensible for optimized
> builds. (2) A one-shot-fill `standard_node_entries()` (`vector(count)` +
> `out[i] = Node(...)`) was tried: at `-O0` it wins 856→525 µs, but at `-O3`
> it **regresses 36→58 µs** (the value-init + assign double-write loses once
> libc++'s container-annotation chain inlines to nothing). Reverted — the
> per-element `push_back` is correct. (3) §B's relative wins (memoise,
> visited-set bitmap, `FF_Result`→`bool`) were all measured at `-O0`; re-derive
> at `-O3` before acting on them. §C's defect is real but Debug-inflated.

---

## §A — Fixture facts (timings superseded — see the correction above)

The measurements that opened this topic were Debug (`-O0`) numbers and are
superseded by the correction above; the fixture facts still stand:

- Fixture: largest Synthea bundle → 50.8 MB `.ffhr`; **913,809 blocks** reachable
  from the root, 46 block types, **~18.3M declared slots** (blocks × ~20 fields).
- Header touch (Debug-era; re-derive at `-O3` if needed): 0.8 ms sequential /
  3.7 ms shuffled — even at `-O0` the walk was not memory-latency-bound.

**Slot-kind distribution** (from `generated_src/FF_FieldKeys.hpp`, 1,611 declared
slots across all blocks): 641 ARRAY, 454 BLOCK, 298 STRING, 102 CODE, 52 CHOICE,
3 RESOURCE = **1,550 offset-bearing (96.2%)**; 23 BOOL, 19 FLOAT64, 19 UINT32 =
61 inline scalar (3.8%). Consequence: **you cannot speed this up by skipping
slot kinds.** Almost every slot must be followed.

---

## §B — What was already tried, and what it bought

Do not repeat these. Two of three plausible hypotheses were **wrong**, which is
the main lesson: measure, do not reason.

| Change | Result | Verdict |
|---|---|---|
| Memoise the per-block field table | 551 → 477 ms | 13% — the `std::vector` copy was **not** the main cost, contrary to expectation |
| `reserve()` the visited set | 477 → 379 ms | rehashing was 20% |
| Visited set → bit-per-offset | 379 → 192 ms | node-based `unordered_set` was ~half the remaining time |
| Hoist per-field bounds check out of the loop | 192 → 186 ms | **noise — not a factor** |
| Recursion returns `bool`, not `FF_Result` | 186 → **108 ms** | **largest single win** |
| `FF_Result` threaded by reference vs. parked on the struct | 105–107 ms both | **identical** — pick on clarity |
| Skip inline-scalar slots in the structural pass | no change | only 3.8% of slots are skippable (§A) |
| **Locality hypothesis** (cache misses are the floor) | **refuted** | shuffled access is 3.7 ms vs the walk's 107 ms |

**Why `FF_Result`-by-value was so expensive:** it carries a `std::string`.
Returning one from every level of a recursion that visits ~18.3M slots meant
millions of string constructions to report *success*. This pattern is worth
grepping for elsewhere in the read path.

---

## §C — A known, live defect (start here; it is concrete)

`reflected_fields(uint16_t recovery)` in `generated_src/FF_Reflection.cpp`
returns **`std::vector<FF_FieldInfo>` by value**:

```cpp
template <typename T_Block>
std::vector<FF_FieldInfo> fields_for_block() {
    return std::vector<FF_FieldInfo>(T_Block::FIELDS, T_Block::FIELDS + T_Block::FIELD_COUNT);
}
```

`T_Block::FIELDS` is already a `static const FF_FieldInfo[FIELD_COUNT]`. Every
call therefore heap-allocates and copies `FIELD_COUNT × 16` bytes to hand back
data that is already sitting in static storage.

**A zero-copy replacement already exists and is generated:**
`reflected_fields_view(uint16_t) -> std::span<const FF_FieldInfo>`, emitted by
`generator/emit/views.py`. **Only `validate_FFHR_stream()` uses it.** These
callers still use the allocating version:

| Caller | Why it matters |
|---|---|
| `src/FF_Parser.cpp` — `Node::fields()` | the documented public read API |
| `src/FF_Parser.cpp` — `node_size()` | **builds an entire vector just to call `.size()`** — `FIELD_COUNT` is a compile-time constant |
| `src/FF_Parser.cpp` (~line 91) | reflection dispatch |
| `python/FF_PythonBindings.cpp` ×3 (lines 289, 406, 431) | the entire Python read path |

`node_size()` is the most clearly wrong and the cheapest to fix. **Fixing these
is not the same as fixing §A** — the memoisation experiment showed the field
table was only ~13% — but it is a real allocation on a path documented as
allocation-free, and it must be measured, not assumed.

> **Measured (2026-08-19):** at `-O3` the defect is Debug-inflated —
> `root().size()` is 17.5 ns/call, `root().fields()` ~17 ns/call on the
> 31,042-entry bundle. Still the right zero-cost fix; no longer a headline.

> **DONE (2026-08-19, uncommitted).** All six `reflected_fields()` callers
> migrated to `reflected_fields_view()` (span): `standard_node_size`,
> `compact_node_lookup_field`, `Node::fields()`, and the three Python binding
> sites. `Node::fields()` / `ObjectHandle::fields()` /
> `MutableEntry::fields()` now return `std::span<const FF_FieldInfo>` (public
> API change — benchmark-parity note required; `read_path_bench` in the
> companion repo compiles unchanged). The allocating `reflected_fields()` and
> `fields_for_block()` are deleted from the emitter (`generator/emit/views.py`)
> and regenerated out of `generated_src/FF_Reflection.*`. Verified: build
> green, `ctest --preset ninja` 33/34, `pytest tests/generator` 46/46.
>
> **Wire-gate fix in the same change set (see A4):** the gate was red because
> `generator/emit/code_names.py` wrote `FF_Codes.hpp` to the repo
> `generated_src/` regardless of `--output-dir`, so the tmp-tree witness found
> an empty codes section. All three hardcoded-path emitters (`code_names`,
> `code_ids` dictionary tables, `recovery_tags`) now take `output_dir`, and the
> witness reads `FF_Recovery.hpp` from the witnessed tree like it already did
> for `FF_Codes.hpp`. Gate is green and honest for both families.

---

## §D — Directions to investigate (nothing here is decided)

1. **Migrate every `reflected_fields()` caller to `reflected_fields_view()`**,
   starting with `node_size()`. **Measured 2026-08-19** (see §C): at `-O3` the
   allocation is ~17 ns/call — the migration is a zero-cost alloc/cleanliness
   fix, not a throughput lever. Then decide whether `reflected_fields()` should
   exist at all, or stay for binding code that genuinely needs an owning copy.
2. **Measure the other whole-document paths the same way** — `print_json()`
   export, `ff_export`, the Python iteration path, and `Compactor::archive()`.
   **DONE 2026-08-19** (min of 7, 50.8 MB fixture, `-O3`): reflective walk
   25.6 ms, `print_json()` 197 ms, `Compactor::archive()` 159–191 ms, deep
   validate ≈ shallow validate. Numbers live in the correction above; the
   canonical instrument is `read_path_bench` in the companion benchmark repo.
3. **Does `Node`/`Entry` construction allocate or touch memory it need not?**
   architecture.md §8.1 claims a `Node` is built "in CPU registers". Verify that
   is still true — count instructions or check for spills.
4. **Parallelism.** The walk splits cleanly across `Bundle.entry` subtrees, and
   a Synthea bundle is ~913k blocks under one entry array. This is the only
   identified route to a large multiple, rather than a percentage. Requires
   deciding whether the read path may use threads at all (it currently does not)
   — ⚠ that is Ryan's call, not an implementation detail.
5. **Reduce blocks, not per-block cost.** 913,809 blocks for a 50.8 MB stream is
   ~58 bytes/block. Ask whether the encoding is over-fragmenting — e.g. small
   strings each getting a 14-byte-header block. This is a wire-format question
   and therefore DT-adjacent; do not act on it without Ryan.

---

## §E — How to reproduce (exact, so no time is lost re-inventing it)

```bash
cmake --build --preset ninja
F=$(ls -S build/synthea_fhir_r4/*.json | head -1)
./build/ff_ingest "$F" -o /tmp/big.ffhr        # ~50.8 MB
```

Then a small `-O2` program linked against a **Release** `libfastfhir`
(`-Iinclude -Igenerated_src -Lbuild-opt -lfastfhir`, run with
`DYLD_LIBRARY_PATH=build-opt`; `build-opt` = `-DCMAKE_BUILD_TYPE=Release` —
`build/` from the `ninja` preset is **Debug `-O0`** and reproduces the old
numbers, see the correction above) that constructs a `Parser` over the bytes
and times the call under test. **Take the minimum of ≥5 runs**, not the mean —
run-to-run spread is ~5 ms and has already produced one false conclusion.

Sampling profiler that worked (macOS):

```bash
./yourbench /tmp/big.ffhr & PID=$!; sleep 2; sample $PID 3 -f /tmp/prof.txt; wait $PID
```

Canonical instrument: `//bench:read_path_bench` in the companion benchmark repo
(Bazel, opt) — same measurements, per-Bundle-entry averages, 50 µs/entry gate.

---

## §F — Rules for whoever takes this

1. **Every claim needs a number.** Two of three intuitions were wrong here.
   State before/after and the fixture.
2. **Do not make validation automatic.** XP-2.4 settled this: construction is
   ~1.7 µs and the walk 107 ms on the Debug build — 0.125 µs / 10.4 ms at
   `-O3` (see the correction above). `Builder::query()` constructs a `Parser`
   over a *build in progress* and must stay free.
3. **Bounds-check before indexing anything by an offset.** A guard ordered after
   an offset-indexed lookup was a live out-of-bounds read in
   `validate_FFHR_stream()` for one build, and it presented as a
   *non-deterministic* test failure. See the comment in `DeepValidator::walk`.
4. **Do not change the wire format for speed** without Ryan. Direction 5 above
   is the only one that would, and it is flagged ⚠ for that reason.
5. Keep `ctest --preset ninja` at its current state (33/34; `py_roundtrip` is a
   known red from A23–A27, unrelated to this topic).

---

# ▶ WIRE WORK ORDER — DT: pack date/time; RT: stop the diff cascade

Written 2026-08-19. Two related orders. **RT-1 first** — it is the measuring
instrument for everything else and has no wire impact. **DT-* is a breaking
wire change** and is deliberate: Q13 says the format is NOT frozen, and this is
the class of change that is free during alpha and impossible after the freeze.

**Sequencing against the P0s below.** XP-1.2 and XP-2 remain P0 and are not
displaced by this block. RT-1 is independent of both and can be taken at any
time. DT-* should not start until XP-2 lands, because DT changes how a slot is
interpreted and XP-2 is what bounds-checks the graph a bad slot would reach.

**Rules: the same ones as the cross-pollination orders below** — one task ID per
session, run *Locate* first and STOP on a mismatch, do not commit, never
hand-edit anything under `generated_src/` (which now includes `FF_Recovery.hpp`), and ⚠ marks a decision
a flash model must not take alone.

## Priority summary

**XP-2 is closed (2026-08-19), so there is no open P0.** DT is unblocked and
**all three of its ⚠ decisions are now answered** (tag breakdown and DT-1.2 /
DT-4.4, 2026-08-20 — see each task). Nothing gates DT-1.

| ID | Priority | Task | Why | Status |
|---|---|---|---|---|
| RT-1 | P1 | Align round-trip entries by identity before diffing | 79% of reported diffs are cascade from one dropped resource | **DONE 2026-08-19** (eb008e2) |
| DT-0 | P1 | Band correction + header relocation (prerequisites) | `FF_CODE` was misbanded, silently disabling a compactor path | **DONE 2026-08-19** (eb008e2) |
| DT-1 | P1 | `FF_DateTime` packed representation + primitives | 4 tags sit reserved and unused; dateTime is a string today | open |
| DT-2 | P1 | Generator: route date/dateTime/instant/time off STRING_TYPES | 306 elements across 120 types | **scalar slots + choice variants DONE** (working tree); **DT-2.4 arrays open** |
| DT-3 | P1 | Ingest + export paths | where the encode/decode actually happens | open |
| DT-4 | P1 | Tests, then re-baseline the wire witness | the gate must move deliberately, in the same commit | open |
| AR-1 | P1 | Array readers dispatch on the header tag, not the kind bits | every scalar array exports as `[]` — 136,006 of the 195,708 remaining round-trip diffs | open |
| AR-2 | P2 | `FF_FieldKeys.hpp` disagrees with the wire on 6 `code` array fields | a consumer navigating by the public constant mis-reads those arrays | open |

---

## RT-1 — Align round-trip entries by identity (P1)

**Why.** `tests/python/test_roundtrip.py` compares Bundle entries **positionally**.
Out-of-profile resources are dropped at ingest, so the entry list shifts and every
comparison past the first drop is between two unrelated resources.

Measured 2026-08-19 over 12 Synthea fixtures: **28,002 positional diffs → 5,890
when entries are paired by identity. 79% is cascade.** On one fixture: 47 entries
in, 43 out; the four missing are exactly `Claim`×2 and `ExplanationOfBenefit`×2,
and the first divergence is at index 28 where the first `Claim` sat.

The harness is right to fail — resources really are dropped. The *attribution* is
wrong, and it buries A27's real signal under millions of lines.

### Locate

```bash
cd /Users/ryanlandvater/GitHub/FastFHIR
grep -n "def diff_doms" -A 12 tests/python/roundtrip_diff.py
grep -n "class DiffKind" -A 8 tests/python/roundtrip_diff.py
```

**Expect:** `diff_doms` recursing arrays by index, and a `DiffKind` enum with no
member for a dropped or added resource.

### RT-1.1 — Pair entries before diffing
In `roundtrip_diff.py`, special-case `Bundle.entry`: build an index of output
entries keyed by `(resourceType, id)`, and diff each input entry against its
matched counterpart rather than against the same index.

> **Correction (2026-08-19, during implementation).** This step originally said
> to key on `fullUrl` first and fall back to `(resourceType, id)`. That is
> backwards and would have matched **nothing**: the exporter emits no `fullUrl`
> at all (A26), so every input entry would key on `fullUrl` and every output
> entry on `(resourceType, id)`, reporting the whole bundle as dropped AND
> added. `(resourceType, id)` is the primary key precisely because it is
> intrinsic to the resource and therefore derivable on both sides; `fullUrl` is
> the fallback for an entry whose resource carries no `id`.

### RT-1.2 — Two new diff kinds
Add `DROPPED_RESOURCE` and `ADDED_RESOURCE`. An input entry with no counterpart
is one finding naming the type and id — **not** a subtree walk. Same in reverse.

### RT-1.3 — Report the drop summary first
`format_diff_report` leads with the dropped/added counts by resourceType, then
the field diffs. The current report makes a 4-resource drop look like 20,000
field defects.

**Done when:** the 12-fixture sample reports ~9,328 field diffs plus an explicit
`DROPPED_RESOURCE` line per missing resource, and deleting one entry from a
fixture's output produces exactly one new `DROPPED_RESOURCE`, not a cascade.

> **The 9,328 figure supersedes the ~5,890 first written here.** 5,890 was
> measured by diffing `entry[i].resource` subtrees only. The harness correctly
> also diffs the entry *wrapper*, which adds `fullUrl` + `request` once per
> matched entry — 1,719 matched entries x 2 = 3,438, and 5,890 + 3,438 = 9,328
> exactly. Both numbers are right about different things; the harness's is the
> one to assert against.

> **DONE (2026-08-19).** `_entry_identity` / `_is_entry_array` /
> `_diff_entry_array` in `tests/python/roundtrip_diff.py`; `DROPPED_RESOURCE`
> and `ADDED_RESOURCE` added to `DiffKind`; `format_diff_report` leads with the
> structural summary. Unmatched entries are reported as one finding and never
> walked as a subtree — that walk *is* the cascade.
>
> Corpus-wide (342 fixtures): field diffs **6,453,617 -> 1,844,523 (-71%)**, and
> the log went from 12,909,302 lines to 3,692,122. `VALUE_MISMATCH` collapsed
> **2,125,763 -> 7,123** and `ARRAY_LENGTH` **139,925 -> 0**, which is the
> signature of the cascade: those two kinds were almost entirely the artefact of
> comparing unrelated resources. 88,260 dropped resources are now stated once
> each instead of being inferred from the wreckage — `Claim` 36,614,
> `ExplanationOfBenefit` 36,614, `SupplyDelivery` 8,117,
> `MedicationAdministration` 5,471, `ImagingStudy` 1,444. Every one is
> out-of-profile for `us-core`, which is A27's signal, previously invisible.
>
> Red-green: removing one matched entry from the middle of a fixture's output
> yields exactly **1** new `DROPPED_RESOURCE` (net diff delta -3). With
> `_is_entry_array` forced to False the same edit yields **2,405 diffs instead
> of 833 — 1,572 spurious**. `py_roundtrip` stays red, correctly: the remaining
> 1.84M diffs are real defects (A24/A25/A26/A27), not measurement error.

---

## DT-0 — Band correction and header untracking (P1) — ✅ DONE 2026-08-19

Two prerequisites that fell out of speccing DT-1, both landed before it starts.

### DT-0.1 — `RECOVER_FF_CODE` rebanded, primitive band compacted
`RECOVER_FF_CODE` moved from `0x0003` (primitive) to **`0x010B`** (scalar), and
the primitive band was then **compacted to close the hole** rather than leaving
`0x0003` retired — pre-alpha, so the band is re-cut clean instead of carrying a
gap forever:

```
RESOURCE          0x0004 -> 0x0003        OPAQUE_JSON       0x0008 -> 0x0007
CHECKSUM          0x0005 -> 0x0004        WASM_PAYLOAD      0x0009 -> 0x0008
URL_DIRECTORY     0x0006 -> 0x0005        CODEABLE_CONCEPT  0x000A -> 0x0009
MODULE_REGISTRY   0x0007 -> 0x0006        primitive _next_id -> 0x000A
```

The band is now contiguous `0x0000..0x0009`, verified no duplicates anywhere in
the ledger. **Append-only resumes from here** — this was the last free re-cut.

This was not tidying. The scalar band membership test
`(tag & 0xFF00) == RECOVER_FF_SCALAR_BLOCK` is what routes inline scalars, and
`FF_CODE` is an inline scalar that sat outside it. Two branches written for it
were therefore unreachable:

* `Recovery_to_Kind` — a `case RECOVER_FF_CODE:` inside the scalar switch, dead
  because `0x0003 & 0xFF00 == 0x0000`. Harmless: a duplicate in the second
  switch did the work.
* `Compactor::write_choice_slot` — `if (FF_IsScalarBlockTag(tag)) { if (tag ==
  RECOVER_FF_CODE) { ... } }`. **Not harmless.** `FF_IsScalarBlockTag(0x0003)`
  is false, so a choice slot holding a code never took the inline path and fell
  through to the block path instead. **11 FHIR choice fields carry a `code`
  variant** (`ElementDefinition.defaultValue[x]`, `.fixed[x]`, `.pattern[x]`,
  `.example.value[x]`, `Parameters.parameter.value[x]`, …), so this was a live
  defect, not a theoretical one.

Both are now live. `Recovery_to_Kind`'s `FF_CODE` case moved into the scalar
switch and out of the second; verified `FF_IsScalarBlockTag(RECOVER_FF_CODE)`
is now true and every tag still resolves to its previous `FF_FieldKind`.
`tests/cpp/test_primitives.cpp` pins the new values and the new band membership.
Wire witness refreshed with `--force` in two passes — `RECOVER_FF_CODE: 3 -> 267`,
then the seven primitive shifts, each named in the override banner.
`dictionaries/README.md`'s CodeableConcept block layout updated `0x000A -> 0x0009`.
Pre-release re-cut, no archives exist (verified: nothing tracked in git, no
pinned corpus, benchmark repo generates at runtime, the only `.ffhr` on disk are
build artifacts the suite regenerates).

### DT-0.2 — `include/FF_Recovery.hpp` untracked
Generated at configure time and now gitignored. The committed record of every
tag value is the ledger plus `tests/generator/golden/wire_witness.json`; a third
copy was one more thing to keep in step, which is the same argument
`dictionaries/README.md` already makes for not committing the code projections.
Nothing detected a **stale** committed header, and XP-3.2 (`--check`) — the
thing that would have — is still unbuilt.

Docs updated: `CLAUDE.md` (repo map + invariant 1), `dictionaries/README.md`.

### DT-0.3 — `FF_Recovery.hpp` relocated to `generated_src/`
Done 2026-08-19. The header now sits with the other projections rather than in
`include/`, which is what "generated" means everywhere else in this tree.
`generated_src/` was already gitignored and already on the include path, so
`#include "FF_Recovery.hpp"` resolves unchanged.

Repointed: `HEADER_PATH` in `emit/recovery_tags.py`, six paths in
`generator/utilities.py`, four in `library.py`, `model/type_map.py`, the path
construction in `tests/generator/wire_witness.py` and `test_recovery_tags.py`,
`test_wire_format.py`, and `CMakeLists.txt`'s install list
(`FASTFHIR_INCLUDE_DIR` -> `FASTFHIR_GENERATED_DIR`). The `.gitignore` entry for
the old location is gone — `generated_src/` covers it.

**Verified by deleting it and rebuilding from the ledger alone**, not by
inspection: with no copy in either location, `generate_recovery_tags()`
reconciled 978 tags, appended 0, and emitted 74,365 bytes to
`generated_src/FF_Recovery.hpp` carrying the full BAND MAP — which now reads
`Core Primitives … 10 used` and `Inline Scalars … 12 used`, reflecting DT-0.1.
`src/FF_Primitives.cpp` was then forced to recompile and did so cleanly with
**no copy in `include/`**, proving the include resolves from the new location.
`ctest` 33/34 (the known `py_roundtrip` red), generator gate 46/46.

---

## DT-1 — `FF_DateTime`: the packed representation (P1)

**Why.** `date`, `dateTime`, `instant` and `time` are in `STRING_TYPES`
(`generator/model/type_map.py:276`), so each is an 8-byte slot pointing at an
`FF_STRING` block: 8 + 14 + ~25 = **47 bytes and a pointer chase** per value.
Measured on one 2.7 MB Synthea bundle: 2,467 date/time values, 118 KB, of which
**~93 KB (79%) is recoverable**. Comparison also becomes an integer compare
instead of a string compare.

`generated_src/FF_Recovery.hpp` already reserves `RECOVER_FF_DATE = 0x0107`,
`RECOVER_FF_DATETIME = 0x0108`, `RECOVER_FF_TIME = 0x0109`,
`RECOVER_FF_INSTANT = 0x010A` in the scalar band, annotated **"Reserved for
bit-packing"**, referenced by nothing. The numbering is already done and
permanent; this order spends it.

### A time_point is the wrong model — do not use one

From the R4 StructureDefinitions, all four constraints are load-bearing:

| Constraint | Consequence |
|---|---|
| `dateTime` is a union of gYear, gYearMonth, date, dateTime | `"2024"` ≠ `"2024-01-01T00:00:00Z"`. Precision must round-trip. "Born in 1970" is not "born 1 Jan 1970". |
| `date` has no timezone, ever | It is a civil date. Encoding as epoch-UTC invents an offset. |
| `time` has no date | Duration since midnight; there is no instant to hold. |
| seconds may be `60` | Leap seconds are legal FHIR. `chrono` normalises to the next second — a silent value change. |

So: **packed civil time + precision + offset**, not an instant.

### The layout — 8 bytes, keeping `TYPE_SIZE_OFFSET`

Staying at 8 bytes is the point: **no vtable offset arithmetic changes**, so the
R4/R5 field-offset stability property of architecture.md §4.2 is untouched and
the diff is confined to a field's *kind* and *interpretation*.

```
bit  63      discriminator  0 = packed inline, 1 = offset to FF_STRING fallback
bits 62..41  civil days from 0001-01-01, UNSIGNED (22) — spans years 0001..9999
bits 40..36  hour   (5)
bits 35..30  minute (6)
bits 29..24  second (6)   — 60 is representable, so leap seconds survive
bits 23..14  millisecond (10)
bits 13..3   UTC offset, signed minutes (11)
bits  2..0   precision (3)
```

Exactly 64 bits. Days-from-epoch rather than separate y/m/d is what buys the
discriminator bit back (22 bits vs 23 for y/m/d).

**The epoch is 0001-01-01 and the field is unsigned — this is not a free choice.**
A signed count from the usual 1970 epoch does not fit: 1970→9999 is 2,932,896
days and signed 22 bits reach only 2,097,151, so the representable ceiling would
be year 7711 while FHIR permits 9999. Measured from 0001-01-01 the full span is
3,652,058 days against an unsigned 22-bit capacity of 4,194,303. Anything
outside 0001..9999 is not legal FHIR and takes the fallback.

**Byte-exact round-trip is achievable in this budget.** Two results that make it
fit, both worth stating because they are not obvious:

1. **`Z` vs `+00:00`.** The offset field needs 1,681 distinct values (−840..+840)
   and 11 bits give 2,048 — so 367 codes are spare. One spare code means
   "offset 0, written as `Z`". No extra bit.
2. **Fractional digits.** `.5`, `.500` and `.500000` are one instant and three
   texts. The precision enum does **not** need to encode which FHIR type this is
   — the recovery tag already does — so it only expresses within-type variation:
   `YEAR, YEAR_MONTH, DATE, SECOND, FRAC1, FRAC2, FRAC3` = 7 values, 3 bits.
   FHIR's own regex makes seconds mandatory when `T` is present, so there is no
   MINUTE precision to store.

Anything that still does not fit — more than 3 fractional digits, a year outside
0001..9999 — sets bit 63 and stores the original text in an `FF_STRING` block at
a **signed relative offset**, exactly as a non-dictionary code does.

### `FF_DATETIME` is `FF_CODE`, widened — say so everywhere

This is not merely "a similar idea". It is the same slot contract at 8 bytes
instead of 4, and every document that describes one must describe the other in
the same terms, so a reader who has understood code slots already understands
date/time slots:

|  | `FF_CODE` (4 bytes) | `FF_DATETIME` (8 bytes) |
|---|---|---|
| Discriminator | bit 31 (`FF_CODEABLE_CONCEPT_FLAG`) | bit 63 |
| Flag **clear** | 31-bit dictionary ID | 63-bit packed civil date/time |
| Flag **set** | 31-bit **signed relative** offset to an `FF_CODEABLE_CONCEPT` block | 63-bit **signed relative** offset to an `FF_STRING` block |
| Offset is relative to | the containing block | the containing block (identical rule) |
| Sign-extension helper | `FF_ResolveCodeableConceptOffset` | `FF_ResolveDateTimeOffset` (to write) |
| Null sentinel | `FF_CODE_NULL` = `0xFFFFFFFF` | `FF_NULL_OFFSET` = `0xFFFF'FFFF'FFFF'FFFF` |

Two properties are worth stating explicitly because they are what make the
parity exact rather than approximate:

1. **Relative, not absolute.** The fallback offset is signed and relative to the
   containing block, the same convention `ENCODE_FF_CODE` already uses. Absolute
   offsets would have worked and would have been wrong here: a second convention
   for the same job is the thing a reader has to memorise instead of transfer.
2. **The null sentinel lives inside the flag-set space and is reserved out of
   it.** `FF_CODE_NULL` has bit 31 set, so it would otherwise read as a relative
   offset of `0x7FFFFFFF`; it is reserved instead. All-ones does the same for
   `FF_DATETIME` — an impossible relative offset, so it is free to mean "absent".
   Same trick, same reasoning, and both need the same sentence in the docs.

**Documentation this must land in (DT-1.4):** `architecture.md` (a subsection
beside the code-slot description, not a separate chapter), `README.md`'s "Code
Assignment Semantics" — which gains a sibling "Date/Time Assignment Semantics"
written to the same shape — and the `FF_Primitives.hpp` comment block, which
should cross-reference the code slot by name. `terminology_layer_architecture.md`
§3's VTable-slot bit-layout table gets the 8-byte row alongside the 4-byte one.

### The tag value — already assigned; do not move it

`RECOVER_FF_DATETIME` is **`0x0108`**, in the **scalar** band, and stays there.
It has been assigned since the 2026-08-14 band re-cut, together with its
siblings `RECOVER_FF_DATE` `0x0107`, `RECOVER_FF_TIME` `0x0109` and
`RECOVER_FF_INSTANT` `0x010A`, all four sitting directly after
`RECOVER_FF_FLOAT64`. No ledger renumbering is required by this work order, and
none should be attempted.

> **DECIDED (2026-08-19, Ryan):** keep the current breakdown in
> `dictionaries/master_tags.json` lines 114–133 exactly as assigned — the four
> 8-byte inline scalar tags DATE `0x0107`, DATETIME `0x0108`, TIME `0x0109`,
> INSTANT `0x010A` in the scalar band, notes unchanged. DT-1 is implementable
> on this layout. **Reconfirmed 2026-08-20.** (DT-1.2 and DT-4.4, the other two
> ⚠ items, were both answered 2026-08-20 — see each task. No ⚠ remains in DT.)

**The band is functional, not decorative.** `Recovery_to_Kind` routes on
`(base & 0xFF00) == RECOVER_FF_SCALAR_BLOCK` — that test *is* the "this is an
inline scalar" decision, and a packed date/time is an inline scalar of exactly
the same class as `FF_FLOAT64`. `0x0108` is where it must live for dispatch to
work; anywhere in the primitive band and it falls out of that branch.

⚠ **Rejected alternative, recorded so it is not re-proposed:** relocating
`FF_DATETIME` to `0x0004` so it sits beside `RECOVER_FF_CODE` in the emitted
enum. It fails on three counts, in increasing order of importance:

1. `0x0004` is `RECOVER_FF_RESOURCE`, so the insert shifts **seven** assigned
   tags (`RESOURCE`, `CHECKSUM`, `URL_DIRECTORY`, `MODULE_REGISTRY`,
   `OPAQUE_JSON`, `WASM_PAYLOAD`, `CODEABLE_CONCEPT`).
2. It splits the date/time family across two bands, leaving `DATE`, `TIME` and
   `INSTANT` behind in the scalar band.
3. **It moves a scalar out of the scalar band**, which is the one that actually
   matters. `FF_CODE` at `0x0003` is the anomaly here, not the model: it behaves
   like a value type while being banded with the structural blocks. That
   confusion had already produced a bug — an unreachable `case RECOVER_FF_CODE:`
   inside the scalar-band switch, dead because `0x0003 & 0xFF00 == 0x0000`.
   Copying `FF_CODE`'s banding would propagate the mistake rather than the
   pattern.

> **Note (2026-08-19):** the dead case is now deleted, and the branch carries a
> comment explaining why `FF_CODE` must not be named there and why packed
> date/time must. Verified behaviour-preserving: `FF_CODE` still resolves to
> `FF_FIELD_CODE` through the second switch, and all nine scalar/structural tags
> dispatch unchanged.

The FF_CODE parity that motivated the proposal is delivered instead by the table
above, by DT-1.4's documentation pass, and by the ledger `note` fields — which
now state the shared contract at the point of definition, so `FF_Recovery.hpp`
explains itself:

```
RECOVER_FF_DATETIME = 0x0108,  // 8-byte inline scalar, same slot contract as
                               // RECOVER_FF_CODE: MSB clear = packed civil
                               // date/time, MSB set = signed relative offset to
                               // an FF_STRING. Precision field says how much of
                               // the value is populated.
```

### One layout, four tags

There is a **single packed representation**, not four. The recovery tag says
which FHIR type a slot holds; the 3-bit precision field says how much of the
value is populated (year / year-month / date / second / fractional). That split
is what keeps the precision enum inside 3 bits, and it is why `date`, `time`,
`dateTime` and `instant` can share one encoder, one decoder and one set of
pack/unpack helpers while still validating against their own FHIR rules —
`date` rejecting a timezone, `time` rejecting a date, `instant` requiring both.

⚠ **Do not fold the four tags into one.** In a choice (`[x]`) slot the 2-byte
recovery tag is the ONLY thing identifying the active variant, and **20 choice
fields mix two or more date/time variants** — 7 of them carry all four
(`ElementDefinition.defaultValue[x]`, `.fixed[x]`, `.pattern[x]`, `.minValue[x]`,
`.maxValue[x]`, `.example.value[x]`, `Parameters.parameter.value[x]`), plus
`QuestionnaireResponse.item.answer.value[x]` and
`Questionnaire.item.enableWhen.answer[x]` at three each. With one shared tag the
exporter cannot choose between `valueDate`, `valueDateTime`, `valueInstant` and
`valueTime` — which is the `effectiveDateTime -> effectiveString` defect DT
exists to remove, reproduced at finer grain. Precision cannot substitute: it is
ambiguous in both directions, since `precision = DATE` is equally a `date` or a
`dateTime` at date precision (FHIR permits `2024-01-15` for `dateTime`), and
`precision = SECOND` with an offset is equally a `dateTime` or an `instant`.

### DT-1.1 — Constants and helpers in `FF_Primitives.hpp`
Field widths/shifts as symbolic sums (never literal offsets), a `precision` enum,
the `Z` sentinel code, `FF_PACK_DATETIME` / `FF_UNPACK_DATETIME`, and the
discriminator predicate. Civil-days conversion is the standard days-from-civil
algorithm; keep it branch-free and `constexpr`.

Name and site these against their code-slot counterparts so the parity is visible
in the source, not only in prose: put the block adjacent to the `FF_CODE`
helpers, mirror their naming, and have `ENCODE_FF_DATETIME` take the same
`(base, block_offset, child_offset, ...)` shape as `ENCODE_FF_CODE` so the
relative-offset arithmetic is written once in each and reads identically.

> **DONE (2026-08-20, `d9fc00a`).** `FF_Primitives.hpp` gained
> the slot-contract table against `FF_CODE`, `FF_DATETIME_FALLBACK_FLAG` /
> `_PAYLOAD_MASK` / `_NULL`, the `FF_DateTimeBits` symbolic-sum enum
> (`static_assert(FF_DT_FLAG == 63)`), `FF_DateTimePrecision`, the
> `FF_DATETIME_OFFSET_Z` sentinel, `FF_DateTimeParts`, `constexpr`
> `ff_days_from_civil` / `ff_civil_from_days` (Hinnant) with the epoch shift
> isolated in `ff_datetime_days_from_civil` / `ff_datetime_civil_from_days`,
> `FF_PACK_DATETIME` / `FF_UNPACK_DATETIME`, `ff_datetime_fits`, and
> `FF_DATETIME_IS_FALLBACK`. `FF_Utilities.hpp` gained `FF_ResolveDateTimeOffset`
> beside its 31-bit counterpart. `FF_Primitives.cpp` gained the parser, the
> formatter, and the `SIZE_`/`STORE_`/`ENCODE_FF_DATETIME` triple mirroring the
> code emitters. **Nothing calls them yet — DT-2 wires the generator up.**
>
> Verified (Debug build): every DT-1.3 case round-trips text-in == text-out —
> all seven precision levels, `Z` vs `+00:00`, ±offsets to the ±14:00 limit,
> leap second `23:59:60`, pre-1970, year 0001 and 9999, leap day, time-only,
> 1–3 fractional digits. Fallback (not an exception) for: 4- and 6-digit
> fractions, a `date` carrying a time, an `instant` without one, `T` without a
> timezone, `2024-02-31`, `2023-02-29`, unpadded `2024-1-5`, month 13, hour 25,
> offset `+15:00`, and garbage. The arena path was checked separately: packable
> text leaves `child_off` untouched, unfittable text writes an `FF_STRING` whose
> bytes match the input exactly and whose flagged relative offset resolves back
> through `FF_ResolveDateTimeOffset` in both directions. The civil conversion was
> checked against an independent day-by-day calendar walk: **3,652,059 days,
> 0 mismatches.** `ctest --preset ninja` 33/34 (`py_roundtrip` red as before,
> on `fullUrl`/`request` — A26, unrelated), `pytest tests/generator` 46/46, wire
> witness unmoved (correct: nothing generated changed).
>
> Two decisions taken inside the task, both recorded in the source comments:
> (1) a parse failure takes the **fallback**, not an exception, matching the code
> slot — preserving the bytes that arrived is always defensible, and judging FHIR
> legality belongs to ingest (DT-3); (2) `_pack_datetime_offset` **rejects a
> relative offset of −1**, which would encode to all-ones and collide with
> `FF_DATETIME_NULL`. It cannot occur (the smallest block is larger than one
> byte), but the sentinel's claim to that bit pattern is that no real offset
> produces it. `FF_CODE` has the identical latent case at 31 bits and no such
> guard.

### DT-1.2 — `FF_FieldKind`
> **DECIDED (2026-08-20, Ryan): one value, `FF_FIELD_DATETIME = 13`,** appended
> after `FF_FIELD_CHOICE` in `include/FF_Primitives.hpp:304`. **One kind for all
> four tags** — the recovery tag says which FHIR type the slot holds, the kind
> says only "inline 8-byte packed date/time", exactly as "One layout, four tags"
> above requires. `Recovery_to_Kind` maps `RECOVER_FF_DATE` / `DATETIME` / `TIME`
> / `INSTANT` all to this single value inside the existing scalar-band branch,
> and `ff_slot_width` gains one `case` returning `TYPE_SIZE_UINT64`. Do not add
> per-type kinds; a later split would be additive and is not needed.
>
> This value is **not a wire constant**: `FF_FieldKind` is never serialized — it
> is derived from the recovery tag by `Recovery_to_Kind` and otherwise lives only
> in the static `FF_FieldInfo` tables, `FF_FieldKey`, and `ff_slot_width`. It is
> an ABI/source choice, so it appends rather than renumbers for ABI stability,
> not for wire permanence.

> **DONE (2026-08-20, `dc72669`).** `FF_FIELD_DATETIME` appended to
> `FF_FieldKind` — value 13 by append position, not written as an explicit
> initialiser; the `[Layout]` test asserts the 13 so the ABI value is pinned
> somewhere that fails loudly. Plus the five places that decide *what a kind is*
> — leaving any of them inconsistent is what turns a new enumerator into a
> silent hole:
> - `ff_slot_width` → `TYPE_SIZE_UINT64`. The compact slot tables and the
>   generated V-Table `static_assert`s are emitted as calls to this function, so
>   the compactor inherits the width with no second edit.
> - `Recovery_to_Kind` — all four tags collapse to the one kind, inside the
>   existing scalar-band branch (the band test is a range check, so `0x0107`–
>   `0x010A` already reach it).
> - `RecoveryTraits<>` — four specializations, the compile-time twin of the
>   above. Two independent mappings over the same facts is the shape that
>   drifts, so a `static_assert` now pins them equal, and a second pins the
>   width at 8 bytes.
> - `FF_IsFieldEmpty` — joins the `FF_FIELD_FLOAT64` arm (8 inline bytes,
>   all-ones null). **This one was load-bearing**: that switch's `default`
>   returns `true`, so an omitted case would have reported every date/time field
>   as absent and dropped it on export — no crash, no warning.
> - `Node::is_scalar()` — a packed date/time is an inline value like
>   `FF_FIELD_CODE`; that a flagged one can point at an `FF_STRING` no more
>   makes it a string than a fallback CodeableConcept makes a code a block.
>
> **`Kind_to_Recovery` deliberately gets NO case**, and the omission is pinned by
> a test so nobody "completes" the table later. One kind names four tags, so any
> single answer is wrong three times in four, and the wrong answer surfaces as a
> `date` exported as `valueDateTime` — the exact defect class DT exists to
> remove. All three call sites use it only as a fallback when
> `FF_FieldKey::child_recovery` is `UNDEFINED`, and a generated date/time key
> always carries its specific tag, so the fallback must not fire; `UNDEFINED`
> keeps "I do not know" honest rather than laundering it into a plausible guess.
>
> **Deliberately NOT touched**: value rendering (`print_json`, `Entry::as<T>`,
> the Python bindings) is DT-3, and `slot_carries_offset` / `walk_fields` is
> DT-1.5 — **now unblocked**. Both are safe to defer because nothing emits the
> kind yet: the generator still routes these types through `STRING_TYPES`, so
> `FF_FIELD_DATETIME` is unreachable at runtime until DT-2.
>
> Verified: `ff_test_datetime` 32 tests / 0 failures (a `[Layout]` group now
> covers the value, both widths, the four-to-one collapse, the `Kind_to_Recovery`
> omission, and `FF_IsFieldEmpty` on null / a packed value / `0001-01-01`, whose
> mostly-zero bits must still read present). `ctest --preset ninja` 34/35
> (`py_roundtrip` the same known red), `pytest tests/generator` 46/46, wire
> witness unmoved, and no new compiler warnings — every warning in a clean build
> comes from simdjson. Red-green on both new `static_assert`s: making the
> compile-time trait disagree, and setting the width to 4 bytes, each fails the
> build with its stated message.

### DT-1.3 — Round-trip unit tests before any generator change
`tests/cpp/test_datetime.cpp`: every precision level; `Z` vs `+00:00`; a leap
second; a negative (pre-1970) date; year 0001 and 9999; 1–3 fractional digits;
a 6-digit fraction taking the fallback. Assert **text in == text out**.

**Done when:** the pack/unpack pair is byte-exact for every case above, and the
fallback triggers only for the cases listed.

> **DONE (2026-08-20, `d9fc00a`).** `tests/cpp/test_datetime.cpp`,
> registered as `cpp_ff_test_datetime` in all four `CMakeLists.txt` lists (the
> `add_ff_cpp_test` call, the ctest `foreach`, `_BUILD_ALL`, and the two IDE
> folder/scheme lists — omitting `_BUILD_ALL` is what produced A20's silent
> "Not Run"). 28 tests, 0 failures, 0.05 s in ctest; suite now 34/35 with
> `py_roundtrip` the same known red.
>
> **Every case runs through an arena slot**, not parse-then-format: `ENCODE_
> FF_DATETIME` into a slot, then decode that slot the way a reader must (null,
> then discriminator, then packed value or the `FF_STRING` the relative offset
> names). A parse/format pair would exercise neither the discriminator, the
> relative offset, nor the fallback block. `decode_slot()` in the test is the
> reference DT-3's export path should match.
>
> **Dates are sampled, precisions are enumerated** (Ryan, 2026-08-20): 1,500
> random dates per bucket either side of 1970 × every precision each tag admits
> (YEAR→FRAC3 for `dateTime`, YEAR→DATE for `date`, SECOND→FRAC3 for `instant`
> and `time`) = 18 legal tag×precision cells, plus 12 named boundary days
> (0001-01-01, first leap year, the 100/400 century rules, 1900-02-28,
> 1969-12-31, 1970-01-01, 2000-02-29, 9999-12-31) at every precision. The
> sampler asserts its own coverage — every legal cell must be reached and no
> illegal one — so a hole cannot hide behind a green run. Seed is fixed and
> printed (`--seed` to vary); green on seeds 1, 42, 99999, 20260820,
> 4294967295. Time-of-day and UTC offset stay exhaustive because those spaces
> are small: all 24×60×61 = 87,840 times including leap seconds, all 1,000
> millisecond values, all 1,681 legal offsets.
>
> **Red-green, and it corrected a wrong belief.** The first mutation tried —
> deleting the negative-`era` branch of Hinnant's `civil_from_days` — changed
> **nothing**: that function adds 719,468 before dividing, so within years
> 0001..9999 its internal `z` is always positive and the branch is unreachable.
> The epoch straddle is a **signedness** boundary, not a branch. The mutation
> that does bite is computing the epoch shift in unsigned arithmetic
> (`days - (uint32_t)FF_DATETIME_CIVIL_EPOCH`), which wraps for every pre-1970
> date and turns 0001-01-01 into 9222-01-21: **119 failures, all pre-1970,
> every post-1970 case still green.** Both findings are now recorded in the
> test's header comment and beside `ff_civil_from_days`.

### DT-1.4 — The documentation pass
Land the parity table and the two properties above in `architecture.md`,
`README.md`, `FF_Primitives.hpp` and `terminology_layer_architecture.md` per the
list in this section. This is not a follow-up: a reader meeting an 8-byte slot
with a high-bit discriminator and no stated relationship to the 4-byte one will
assume they are unrelated, which is the confusion this whole framing exists to
prevent.

**Done when:** each of the four documents describes the date/time slot in the
same terms it uses for the code slot, and names the other.

> **DONE (2026-08-20, `dc72669`).** All four, each written to
> the shape that document already uses for the code slot and naming it:
> - **`architecture.md` §6.3** — new subsection *"MSB-discriminated value slots
>   — `FF_CODE` and `FF_DATETIME`"*, placed beside the other primitives (§6.1
>   `FF_STRING`, §6.2 the 10-byte wrappers) rather than as its own chapter. The
>   parity table, the two properties, the bit layout, the FHIR constraints that
>   force civil-time-plus-precision, and a **Mermaid flowchart** of the shared
>   encode/decode decision (style guide requires a diagram where prose would
>   describe a shape). §3.1's slot-width table gained a pointer to it.
> - **`README.md`** — *"Date/Time Assignment Semantics"* as a sibling of *"Code
>   Assignment Semantics"*, same 1/2/3 + read-path structure, plus a TOC entry.
> - **`terminology_layer_architecture.md` §3.1** — the 8-byte row beside the
>   4-byte one, with the read-path branch written the same way, and an explicit
>   statement that a date/time shares the *slot contract* and nothing else: it
>   never consults the dictionary, so none of that document's CodeSystem
>   machinery applies to it.
> - **`FF_Primitives.hpp`** — the parity table landed with DT-1.1 and
>   cross-references `FF_CODE` by name.
>
> Also documented, since DT-1.3 produced rules that outlive it: `CLAUDE.md`
> gained a **Date/time** bullet beside **Codes**, the four-places C++ test
> registration rule (the A20 trap), and the seeded-randomised-suite convention.
>
> **Every document carries a status banner** naming exactly what does and does
> not exist yet, so README cannot advertise a working feature. The banners were
> **refreshed when DT-1.2 landed** (2026-08-20): they had said `FF_FieldKind` has
> no `FF_FIELD_DATETIME`, which stopped being true the moment the kind shipped.
> They now say the slot, the kind and the tests exist, the generator still routes
> these types through `STRING_TYPES`, and no stream contains a packed date/time.
> **A status banner is a dated claim about the tree, so it goes stale silently —
> re-read all four whenever a DT task lands.**
> The README examples were corrected during review: they originally named
> `OBSERVATION::EFFECTIVE_DATE_TIME`, which **does not exist** (the real key is
> `OBSERVATION::EFFECTIVE`, an `FF_FIELD_CHOICE`). They now use
> `PATIENT::BIRTH_DATE` and `OBSERVATION::ISSUED`, both verified present in
> `generated_src/FF_FieldKeys.hpp` and both `FF_FIELD_STRING` today, which makes
> the status banner concrete. Verified: no test parses `README.md` at runtime
> (`tests/cpp/test_readme.cpp` is a hand-written mirror), all fences balanced,
> the Mermaid block parses, and the TOC anchor matches the heading.

### DT-1.5 — The fallback offset must be validated like a code's

**Rule (Ryan, 2026-08-20): a slot whose MSB flags an offset is validated as an
offset.** `validate_FFHR_stream()` skips inline scalars because they cannot aim
the reader at memory it does not own — but a date/time slot with bit 63 set is
not inline data, it is an edge, and the walk must follow it. This is the same
variant the code slot already gets: `FF_FIELD_CODE` with
`FF_CODEABLE_CONCEPT_FLAG` set is sign-extended, resolved relative to the
containing block, and walked against `RECOVER_FF_CODEABLE_CONCEPT`
(`src/FF_Parser.cpp:573–590`). Parity here is not decoration; a kind that can
point somewhere and is missing from `slot_carries_offset` is a hole in the
validator.

- **DT-1.5.1** Add `FF_FIELD_DATETIME` to `slot_carries_offset`
  (`src/FF_Parser.cpp:343`), with the same "only when the flag is set" comment
  the `FF_FIELD_CODE` line carries.
- **DT-1.5.2** Add a `case FF_FIELD_DATETIME:` to `DeepValidator::walk_fields`
  mirroring `case FF_FIELD_CODE:` exactly: read the 8 bytes, `break` on
  `FF_DATETIME_NULL`, `break` when bit 63 is clear (packed value — structurally
  inert; the `_deep()` pass may range-check the fields), otherwise sign-extend
  the 63-bit relative offset and `walk(..., RECOVER_FF_STRING, ...)`.
- **DT-1.5.3** Extend `tests/cpp/ff_test_graph_bounds.cpp` with a date/time slot
  whose fallback offset points out of bounds, and one pointing at a block whose
  tag is not `RECOVER_FF_STRING`. Both must fail validation naming the field.

> **DONE (2026-08-20, working tree, uncommitted).** DT-1.5.1 and DT-1.5.2 as
> written: `FF_FIELD_DATETIME` added to `slot_carries_offset`, and a
> `case FF_FIELD_DATETIME:` in `walk_fields` mirroring `case FF_FIELD_CODE:`
> line for line — null first, then the flag; a packed value breaks out (the
> `_deep()` pass range-checks it with `ff_datetime_fits`), a flagged one
> sign-extends through `FF_ResolveDateTimeOffset` and walks against
> `RECOVER_FF_STRING`.
>
> **DT-1.5.3 could not be written as specified, and the reason is worth
> keeping.** The branch is unreachable from any stream, corrupt or not: a scan
> of all 141 generated block field tables finds **1,611 slots, 0 of kind
> `FF_FIELD_DATETIME`** (102 are `FF_FIELD_CODE`), because the generator still
> routes these types through `STRING_TYPES`. There is no byte you can corrupt to
> reach it until DT-2. What landed instead:
> - **The same two corruption cases against `FF_FIELD_CODE`**, which is the
>   reachable half of the identical mechanism: a flagged offset pointing past
>   the end of the stream, and one pointing at a real block that is not an
>   `FF_CODEABLE_CONCEPT`. Plus a **control** — the same slot with the flag
>   clear must still validate — which is what proves the discriminator rather
>   than the slot kind is what makes it an edge. **This path had no regression
>   test at all before now**, for codes either.
> - **A tripwire** that scans every field table and fails the moment any block
>   declares an `FF_FIELD_DATETIME` slot, with a message naming the two cases to
>   write and instructing that the tripwire be deleted in the same commit. It
>   converts "we forgot" into a red build at exactly the right moment.
>
> ### A pre-existing validator defect, found by the code test and fixed here
>
> Case (b) failed on first run, and not because the test was wrong:
> `if (seen(off)) return true;` sat **above** the recovery-tag check, so a block
> already validated under one expected tag was accepted under **any** tag on
> every later edge. Only the first edge to each block was ever type-checked.
> `include/FF_Parser.hpp` claimed the opposite in its contract.
>
> Impact: **type confusion, not memory unsafety** — bounds are checked before
> the memo, so nothing reads out of the stream, but a crafted file could aim a
> code slot at any already-visited block and have the reader decode an
> `Identifier` as an `FF_CODEABLE_CONCEPT`. Fix: hoist the self-offset and tag
> checks above the memo. The memo is a claim about a **block's subtree**; the
> expected tag is a claim about the **edge**, and a block has many edges.
>
> Cost, measured because this is the hot loop (A/B on the 50.8 MiB Synthea
> fixture, Release `-O3`, each figure a min of 7): **pre-fix 10.30 ms, post-fix
> 10.24 / 10.35 / 10.36 / 10.40 / 10.51 ms across five runs.** The pre-fix
> number sits inside the post-fix build's own spread, so the correction is free
> at the resolution this fixture can measure.
>
> Verified: `ff_test_graph_bounds` all pass (was 2 failures before the fix),
> `ctest --preset ninja` 34/35 with `py_roundtrip` the same known red, no new
> warnings. The `Parser` docstring now states the per-edge property and the
> flagged-slot rule explicitly.
**Done when:** a corrupted date/time fallback offset is rejected by
`validate_FFHR_stream()` with the offending offset and field named, exactly as a
corrupted CodeableConcept offset already is.

### DT-1.6 — The same rule inside a choice (`[x]`) slot — ✅ DONE 2026-08-20

**Found while writing DT-1.5's tests.** A choice slot is 8 raw bytes plus a
2-byte tag naming the active variant. `walk_fields` decided inertness by band
membership alone — `if (tgt == FF_RECOVER_UNDEFINED || FF_IsScalarBlockTag(tgt))
break;` — but band membership does not imply inline: `RECOVER_FF_CODE` is in the
scalar band (`0x010B`) and carries the same MSB discriminator as a dedicated
code slot, so those 8 bytes may be a **fallback offset**. Every such variant was
skipped by the validator while `Node::print_json` still dereferenced it.

Fixed: the choice case now routes a `RECOVER_FF_CODE` variant and any
date/time-tagged variant through the same checks the dedicated slots use.
Because each of those types is now reachable through **two** slot shapes, the
bodies were extracted to `DeepValidator::check_code_value` /
`check_datetime_value` and both shapes call them — writing the check twice is
how the choice path came to be unvalidated in the first place.

Tests (`ff_test_graph_bounds` section 6b): a flagged code variant with an
out-of-bounds offset is rejected, plus **two controls** — an unflagged
(dictionary-id) code variant must still validate, and a genuinely inert
`RECOVER_FF_FLOAT64` variant must still be skipped — so the first case cannot
pass merely because the validator started rejecting all code variants.
Red-green: reverting the one-line band test fails exactly the two new
assertions and leaves both controls passing.

Cost: none measurable. `validate_FFHR_stream()` on the 50.8 MiB fixture at
`-O3` reads 10.46 ms, inside the 10.24–10.51 ms spread of the DT-1.5 build.

### DT-1.7 — `Node::as<string_view>()` resolved a code against the wrong base — ✅ DONE 2026-08-20

**Four sites spell the fallback-offset arithmetic; one disagreed.**

| Site | Base used | |
|---|---|---|
| `ENCODE_FF_CODE` (writer) | containing block | ✅ |
| `write_choice_slot` (compactor) | containing block (`entry.parent_offset`) | ✅ |
| `Entry::operator std::string_view()` | containing block (`parent_offset`) | ✅ |
| `Entry::print_scalar_json()` | containing block (`parent_offset`) | ✅ |
| **`Node::as<std::string_view>()`** | **`m_node_offset`** | ❌ |

For an ordinary code field the two coincided, which is why it never showed. For
a **choice** variant they do not: `resolve_choice` sets `m_node_offset` to the
*slot*, so the read landed one V-Table width away from the real block. Worse,
**both** `resolve_choice` call sites passed `slot_offset` as the `parent_offset`
argument, so that parameter had never once carried the containing block.

**Observed pre-fix**, not inferred: a choice slot holding a genuine
`ENCODE_FF_CODE`-written fallback for `"org-local-code-91827"` decoded to
**`''`** — silent data loss, no crash, no warning. `print_json` omitted the code
entirely. Post-fix both return the code.

**Fix — resolve where the operand still exists.** A `Node` carries only its own
offset, so any arithmetic deferred past construction has already lost the block.
`ParserOps::code_node()` now performs it at the one point where the block is
known, and returns a node already pointing at the `FF_CODEABLE_CONCEPT` (tagged
`RECOVER_FF_CODEABLE_CONCEPT`, kind still `FF_FIELD_CODE` so `print_json` treats
it as a coded leaf). All three producers route through it — both
`entry_as_node` implementations and `resolve_choice` — and both call sites now
pass `e.parent_offset`. `Node::as<string_view>()` gains a branch for the
already-resolved case and **throws** on the old one, which is now unreachable:
the parent is genuinely unknown there, so any answer would be a guess, and a
wrong address reads plausible garbage rather than failing.

This is the same shape as DT-1.6's lesson: the check (or here, the arithmetic)
belongs in **one** place that every path calls, because a value type reachable
through several slot shapes will otherwise be handled correctly in some and not
others.

**Note for DT-3:** a date/time variant in a choice slot needs exactly this
treatment — `Observation.effective[x]` admits `effectiveDateTime`, and an
unfittable value takes an `FF_STRING` fallback with the same block-relative
convention. `code_node` is the pattern to copy.

Tests: `ff_test_graph_bounds` section 6c builds the fallback with the real
`ENCODE_FF_CODE` (not a hand-packed offset, so the convention under test is the
writer's own), then asserts both the `Node` path and `print_json` return the
code. Red-green: reverting the reader branch *and* the eager resolution makes
both assertions fail with `got ''`.

Cost: `print_json` over the 50.8 MiB fixture is **190.8 ms** against the 197 ms
recorded baseline — no regression on the path this touches. (`validate_FFHR_
stream()` read 11.02–11.18 ms in the same session versus 10.24–10.51 ms earlier;
DT-1.7 changes no code that walk executes, so that shift is code layout or
machine drift and was **not** isolated.)

---

## DT-2 — Generator: stop treating them as strings (P1)

> ⚠ **IN PROGRESS 2026-08-21 — the tree is in an inconsistent state. Read this
> before configuring.** `generator/model/type_map.py` has DT-2.1 applied, but
> `generated_src/` still holds pre-DT-2 output, so the build is green only
> because nothing has regenerated yet. **The next `cmake --preset ninja`
> configure runs the generator and the build will fail** (four emitters still
> emit date/time as if it were a string; see below). Either finish DT-2.2/2.3
> or revert `type_map.py` before configuring.
>
> **DT-2.1 — DONE.** `date`/`dateTime`/`instant`/`time` removed from
> `STRING_TYPES`; new `DATETIME_TYPES` dict maps each to its permanent tag;
> `_scalar_recovery_tag` gained the four (so the tag flows through the existing
> scalar machinery); `TYPE_MAP` gained a `date` descriptor — `cpp: uint64_t`,
> `data_type: std::string`, `TYPE_SIZE_UINT64`, plus `encode`/`decode` keys —
> and the other three are projected from it rather than copy-pasted.
>
> **Verified by regenerating into a scratch tree and diffing (165 files
> differ).** Three things are already correct: the slot is
> `ISSUED_S = TYPE_SIZE_UINT64` (**same 8 bytes as the offset it replaces, so no
> V-Table offset moves** — the DT-1.2 `static_assert` holds), the data-struct
> member became `std::string`, and the per-field tag is `RECOVER_FF_INSTANT`.
>
> **DT-2.2 / DT-2.3 — OPEN. The regeneration named exactly five defects:**
> | Site | Emits now | Must emit |
> |---|---|---|
> | reflection + `COMPACT_SLOT_SIZES` + `static_assert` | `FF_FIELD_UNKNOWN` | `FF_FIELD_DATETIME` |
> | `emit/store.py` size pass | contribution dropped entirely | `SIZE_FF_DATETIME` (the fallback `FF_STRING` needs reserved space) |
> | `emit/store.py` store pass | `STORE_U64(slot, data.issued)` — a `std::string` into a `u64`, **does not compile** | `ENCODE_FF_DATETIME` |
> | `emit/deserialize.py` | `data.issued = Decode::scalar<uint64_t>(...)` — **does not compile** | `FF_FORMAT_DATETIME` |
> | `emit/views.py` accessor | `Decode::scalar<uint64_t>` behind `auto` | decode to text |
>
> `FF_FIELD_UNKNOWN` is the dangerous one: `ff_slot_width(FF_FIELD_UNKNOWN)` is
> also 8, so the width `static_assert` **passes by coincidence** while the
> reflection table is wrong. It would not have failed loudly.
>
> **Plus one suspected regression to investigate first:** `Observation.effective`
> is a choice field, and its emitted `child_recovery` changed from
> `RECOVER_FF_STRING` to `RECOVER_FF_DATETIME` — the choice path picking up its
> first variant's tag. **RESOLVED 2026-08-21:** the static `child_recovery` for a
> choice only ever names the first variant, and is *not* the design's error
> sentinel (`FF_RECOVER_UNDEFINED` stays reserved as an error/absent marker and
> is never emitted for a real field). The reader now derives the Entry's
> `target_recovery` from the **runtime variant tag in the slot** (both
> `standard_node_lookup_field` and `compact_node_lookup_field`), so
> `print_json`'s `get_choice_suffix` labels `valueDecimal`/`valueCoding`/etc.
> correctly. Confirmed on the round-trip fixture: extension `value[x]` now emits
> the right suffix. The static `child_recovery` value remains harmless (it is
> only a fallback for absent choices, which are rejected earlier).
>
> **Two decisions the next session must take, neither of them mechanical:**
> 1. **What the view accessor returns.** Text costs an allocation on a path
>    documented as zero-copy; the raw packed word pushes the fallback-offset
>    resolution onto the caller, which is exactly the DT-1.7 trap (the accessor
>    knows the containing block; the caller may not). Recommend: accessor
>    resolves and returns text, and the zero-copy path stays `Node`/`Entry`.
> 2. **Consumer fallout is unbounded until measured.** `std::string_view` ->
>    `std::string` on 306 elements across 120 types ripples into tests, tools
>    and the Python bindings. Build after the emitters are fixed and count.
>
> Then DT-4.3: the witness moves (it records the `TYPE_SIZE_*` constant per
> field, and `TYPE_SIZE_OFFSET` -> `TYPE_SIZE_UINT64` is exactly the kind of
> change it exists to catch), so re-baseline it **in the same commit**.

### Locate
```bash
grep -n "STRING_TYPES" -A 14 generator/model/type_map.py
grep -rn "STRING_TYPES" generator/ | grep -v "def \|^generator/model/type_map.py:276"
```
**Expect:** `date`, `dateTime`, `instant`, `time` inside `STRING_TYPES`, and
every consumer testing membership rather than `== "string"`.

- **DT-2.1** Remove the four from `STRING_TYPES`; map them to the new kind and
  their reserved tags. **Read CLAUDE.md's portability note first**: string-like
  types must be tested via `fhir_type in STRING_TYPES`, never `== "string"` —
  the same rule now applies in reverse, so audit every membership test rather
  than assuming the set is only read in one place.
- **DT-2.2** `emit/store.py`, `emit/deserialize.py`, `emit/views.py`: inline
  scalar slot, no `FF_STRING` child, no offset.
- **DT-2.3** Field keys and reflection carry the new kind so `Node::as<>` and the
  JSON exporter dispatch on it.
- **DT-2.4** **Array-typed date/time fields — OPEN, and the reason DT-2's "done
  when" does not yet hold.** DT-2.1 removed the four types from `STRING_TYPES`,
  which broke three string-array branches. They were kept compiling by appending
  `or f["fhir_type"] in DATETIME_TYPES` to the branch condition, which restores
  the **pre-DT-2** layout verbatim instead of porting it:

  | Site | Emits now | Must emit |
  |---|---|---|
  | `emit/store.py:62` (SIZE pass) | `SIZE_FF_STRING` per element | `SIZE_FF_DATETIME` (fallback `FF_STRING` still needs reserved space) |
  | `emit/store.py:236` (STORE pass) | `FF_ARRAY::OFFSET` + `STORE_FF_STRING` | `FF_ARRAY::INLINE_BLOCK`, stride `TYPE_SIZE_UINT64`, `ENCODE_FF_DATETIME` per element |
  | `emit/deserialize.py:66` | reads each element back through `FF_STRING` | `FF_FORMAT_DATETIME` off the inline slot |

  The comment above `emit/store.py:236` is now false — it claims "dateTime,
  markdown, uri and id share this layout," and dateTime has not shared it since
  DT-2.1. `emit/deserialize.py:70` already documents the symptom: *"DT-2 datetime
  arrays hold `std::vector<std::string>`."*

  **Scope: two fields in the whole spec** — `Timing.event` (`dateTime`) and
  `Timing.repeat.timeOfDay` (`time`). They are 2 of the 31 `FF_ARRAY::OFFSET`
  sites; the other 29 are genuine strings.

  **No effect on `py_roundtrip`** — all 342 Synthea fixtures contain zero
  `Timing` array elements (`timeOfDay`, `dayOfWeek`, `when`, `timing.event` all
  absent), so this is correctness work that will not move the diff count. Do it
  as its own commit, not folded into the round-trip push.

  ⚠ **This activates a dormant path.** An inline 8-byte date/time element with
  bit 63 set holds an offset *relative to its containing block*, and inside an
  array the containing block is the array. The array reader must resolve it
  against the array's own offset while it still holds it — the treatment
  `ParserOps::code_node` gives code slots. Today that is unreachable only
  because these arrays are strings. See CLAUDE.md's two offset invariants and
  architecture.md §5.5; the resolution must land in the **same** commit as the
  emitter change, never after it.

**Done when:** `python -m generator` is deterministic across two runs and no
generated file mentions `FF_STRING` for a date/time field. **DT-2.1–2.3 hold for
scalar slots and choice variants as of 2026-08-22; DT-2.4 is what still fails
the second half of that sentence.**

---

## AR-1 — Array readers must dispatch on the header tag (P1)

**Biggest single item left in `py_roundtrip`: 136,006 of 195,708 diffs**, all
`ARRAY_LENGTH` on `Claim.item.{information,procedure,diagnosis}Sequence`. Source
`"diagnosisSequence": [1]` exports as `[]`. Bytes on disk are correct; the read
path loses the elements.

### Locate
```bash
grep -n "FAST PATH: INLINE ARRAY" src/FF_Parser.cpp          # standard_node_entries fallback
grep -rn "FF_ARRAY::SCALAR" generator/ src/ include/ generated_src/ tools/
```
**Expect:** the fallback hardcodes `FF_FIELD_BLOCK`, and `FF_ARRAY::SCALAR`
appears exactly once in the whole tree — the `case` label in
`standard_node_lookup_index`. If either has changed, STOP.

### Mechanism (verified 2026-08-22)
`print_json` walks arrays through `entries()` -> `standard_node_entries`, which
has three branches: polymorphic tuple (`child_recovery == RECOVER_FF_RESOURCE`),
pointer array (`m_array_entries_are_offsets`), and an unconditional fallback
labelled *"FAST PATH: INLINE ARRAY (Structs)"* that hardcodes `FF_FIELD_BLOCK`.
A `uint32` array matches neither of the first two, so every element becomes a
Node claiming to be a struct. `is_empty()` then takes its `FF_FIELD_BLOCK`
branch, calls `fields()` -> `reflected_fields_view(RECOVER_FF_UINT32)` -> `{}`
(that switch only has cases for block types), reads the empty field list as "no
members present," and returns `true`. The array loop skips every element.

⚠ **The 2026-08-22 handoff blamed the wrong line.** It named
`standard_node_lookup_index`'s `case FF_ARRAY::SCALAR` (the `n.m_kind`
argument). That branch is real but is **not** this bug: `print_json` never calls
`operator[](size_t)`, and no emitter has ever written `FF_ARRAY::SCALAR`, so the
branch is dead. The one-line fix it proposed changes nothing. Do not re-apply it.

- **AR-1.1** `standard_node_entries`: replace the hardcoded `FF_FIELD_BLOCK`
  with a dispatch on `GetTypeFromTag(array header RECOVERY)` — scalar band ->
  `Recovery_to_Kind(tag)` over the inline value, `RECOVER_FF_RESOURCE` -> the
  10-byte tuple, `RECOVER_FF_STRING` -> the pointer table, any block tag ->
  inline block header. Read the tag from the header, **not** from
  `m_child_recovery` (see AR-2 for why the schema copy is unsafe).
- **AR-1.2** `standard_node_lookup_index`: same dispatch, so `node[i]` and
  `entries()` cannot disagree. This also fixes a live over-read — the
  `INLINE_BLOCK` branch does `LOAD_U16(base + item_off + DATA_BLOCK::RECOVERY)`,
  8 bytes into a 4-byte element.
- **AR-1.3** Delete `case FF_ARRAY::SCALAR`. Keep `SCALAR = 0x0000` reserved in
  the enum (it is a wire value no stream uses) or collapse `EntryKind` to
  `INLINE`/`OFFSET`; ⚠ that collapse is a wire decision — Ryan's alone.
- **AR-1.4** Test: a scalar array round-trips its elements, and a `code` array
  (`AllergyIntolerance.category`) still round-trips as strings.

**Done when:** the 342-fixture aggregate drops from 195,708 to ~59,700 with no
new bucket appearing, and no new diffs on `/entry/N/resource/category`.

---

## AR-2 — `FF_FieldKeys.hpp` disagrees with the wire on 6 array fields (P2)

An array's element type is declared in three places. Two agree with the bytes;
one does not.

| Source | `AllergyIntolerance.category` |
|---|---|
| array header `RECOVERY` tag (wire) | `RECOVER_FF_STRING` ✓ |
| `reflected_fields_view` table | `RECOVER_FF_STRING` ✓ |
| `generated_src/FF_FieldKeys.hpp` | `RECOVER_FF_CODE` ✗ |

Six fields, all `code` arrays serialised to `FF_STRING` blocks:
`AllergyIntolerance.category`, `daysOfWeek` on `Availability.availableTime` /
`Location.hoursOfOperation` / `PractitionerRole.availableTime`, and
`Timing.repeat.{dayOfWeek,when}`. Counts reconcile exactly — FieldKeys has 23
`STRING` + 6 `CODE` array fields, the reflection tables have 29 `STRING` and 0
`CODE`.

`print_json` builds its keys from the reflection tables, so the export path is
unaffected. A consumer reaching for the public `FF_ALLERGYINTOLERANCE::CATEGORY`
constant gets `RECOVER_FF_CODE` and would walk the array as codes.

- **AR-2.1** Fix the FieldKeys emitter to record the **stored** element type,
  matching the reflection emitter. One fact, two emitters, one of them stopping
  a step early.
- **AR-2.2** Add a generator test asserting the two emitters agree on
  `child_recovery` for every array field — the divergence class, not the six
  instances.

**Done when:** the two emitters produce identical `child_recovery` for all 934
array fields, and `pytest tests/generator` covers it.

---

## DT-3 — Ingest and export (P1)

- **DT-3.1** Ingest: parse the FHIR text to the packed form; on any parse failure
  take the fallback rather than dropping the field. Never fabricate a value.
- **DT-3.2** Export: unpack to canonical FHIR text. This is also where
  **`effectiveDateTime` stops being `effectiveString`** — that bug exists only
  because dateTime collapses onto `FF_STRING`, so the exporter reads
  `RECOVER_FF_STRING` and names the choice suffix "String". This is **2,808 of
  the 5,890** real diffs measured on 2026-08-19 — 1,404 affected fields, each
  reported twice (a missing key plus an extra key) — across `effective` (1,120),
  `occurrence` (198) and `performed` (86). Nearly half the remaining real diffs
  fall out of DT-2 as a side effect. It does **not** fix `valueQuantity → value`,
  which is the block-with-no-name half and needs its own task.

**Done when:** a fixture containing all four types round-trips byte-identically
through `ff_ingest | ff_export`.

---

## DT-4 — Tests and the wire baseline (P1)

- **DT-4.1** Extend `tests/cpp/test_compactor.cpp`: a packed date/time survives
  compaction inline, with no deferred slot and no `FF_STRING` block emitted.
- **DT-4.2** Re-run the 12-fixture round-trip; the `effective*`/`occurrence*`/
  `performed*` families should be **gone**, not merely reduced. Record the new
  count here.
- **DT-4.3** Refresh the wire witness **in the same commit** as the generator
  change:
  ```bash
  python -m tests.generator.wire_witness generated_src \
      tests/generator/golden/wire_witness.json
  ```
  A golden update without a corresponding source change is a red flag; this one
  has the change, so state it in the commit message.

**DT-4.4 — the compatibility statement. CLOSED (2026-08-20, Ryan): none is
required.** The project is **pre-alpha and has never been used in the wild**, so
there is no historical archive to be compatible with. No README/SPEC caveat, no
engine-version gate, no migration path — do not write one, and do not
re-litigate this at the next breaking wire change while the pre-alpha status
holds. Nothing on disk is at risk either: **no `.ffhr` is tracked in git**, and
the ones under `build/tests/cpp/` are written by the tests themselves on each
run (verified 2026-08-20).

> **The original framing was wrong and is corrected here for the record.** It
> said a pre-DT archive "becomes unreadable". It does not — it reads
> *successfully and wrongly*. After DT-2 a date slot is an **inline** scalar, so
> the 8 bytes that used to hold an offset are decoded as a packed civil
> date/time; an ordinary offset has bit 63 clear, so the discriminator says
> "packed" and a plausible wrong date comes back. `validate_FFHR_stream()` does
> not catch it: the structural pass deliberately skips inline scalars
> (`slot_carries_offset`), because scalars cannot aim the reader at memory it
> does not own. The `Parser` reads `engine_version` but never rejects on it
> (`src/FF_Parser.cpp:257–292`), so nothing else catches it either.
>
> That is harmless under the pre-alpha decision above. It is recorded because it
> generalises past this task: **any future change to how an inline scalar slot
> is interpreted is silent — no validator, no version check, and no exception
> stands between it and a wrong value.** Whether that is worth a gate is a
> question for whoever decides the alpha freeze (Q13/I1), not for DT.

**Verify (whole block):**
```bash
cmake --build --preset ninja && ctest --preset ninja
FASTFHIR_GENERATED_DIR=generated_src python3 -m pytest tests/generator -q
```

---

# ▶ CROSS-POLLINATION WORK ORDERS — start here

Written 2026-08-18 after a joint review of this repository and the Iris File
Extension (`../Iris-File-Extension`), which share an architecture —
offset-addressed blocks in a mapped arena, recovery tags, a Python generator
emitting a gitignored C++ layer, append-only wire ledgers — and have solved
overlapping problems in different orders. **These are the items IFE already
has and this repository does not.** The reciprocal list (what this repository
has and IFE lacks — chiefly the wire witness) is at the top of
`../Iris-File-Extension/MIGRATION.md`.

**These precede the IMMEDIATE WORK ORDERS below.** XP-1 is **done** (2026-08-19);
**XP-2 is now the only open P0** and is what the DT block is gated behind.

## Rules (override anything else in this file)

1. **One task ID per session** (e.g. `XP-1.2`). Do not batch.
2. **Run the *Locate* block first.** If its output does not match *Expect*,
   **STOP** and report what you saw instead. Do not improvise.
3. **Do not commit.** Ryan commits. Leave changes in the working tree.
4. **Never edit generated files** (`generated_src/`, which since 2026-08-19 includes `FF_Recovery.hpp`).
   Fix `generator/emit/` instead.
5. **Never change a wire constant.** None of these tasks needs to; a step that
   seems to is a step written wrong — STOP.
6. Build with `cmake --preset ninja && cmake --build --preset ninja`.
7. ⚠ marks a decision a flash model must not make alone. Produce the analysis
   and STOP.

## Priority summary

| ID | Priority | Task | Why | Status |
|---|---|---|---|---|
| XP-1 | **P0** | Bound and cycle-check the stored-graph traversal | Unbounded recursion over attacker-controlled offsets — stack overflow, not slowness | **DONE** (1.1+1.3 2026-08-18, 1.2 verified 2026-08-19) |
| XP-2 | **P0** | Deep-validate the offset graph on open; fix the Parser's overclaim | `Parser` validates the header only, while its docstring says "file structure" | **DONE 2026-08-19** — XP-2.1/2.2 (87ae434), XP-2.3 `validate_FFHR_stream()`; XP-2.4 answered: explicit call, not automatic |
| XP-3 | P1 | Add `--check` and `--validate` to the generator | No drift or consistency gate exists | open |
| XP-4 | P1 | Port IFE's `portability_lint.py` | Six mechanical checks, each bought with a CI round-trip there | open |
| XP-5 | P1 | Add CI workflows | `.github/` has templates and no workflows; nothing is gated | open |
| XP-6 | P2 | Explicit `<cstring>`; range-check narrowing casts | Two classes IFE hit and fixed this week | open |

---

## XP-1 — Bound and cycle-check the stored-graph traversal (P0)

> STATUS (2026-08-18): **XP-1.1 implemented in the working tree (uncommitted —
> Ryan commits).** The "no depth bound and no visited set" text below describes
> the pre-fix state; `ArchiveContext` now carries `path` + `done` +
> `MAX_NODE_DEPTH` and every recursive entry funnels through the guarded
> `archive_node`. XP-1.2 and XP-1.3 remain open.


**Why first.** `archive_node` → `archive_object` / `archive_array` → back
through `process_pending_write` is mutual recursion over offsets read from the
stored arena, and **there is no depth bound and no visited set anywhere in this
repository** — `grep -rn "MAX_DEPTH\|max_depth\|visited\|cycle" src/ include/`
returns nothing. A `.ffh` whose offset graph contains a cycle recurses until
the stack is gone.

IFE hit the same shape this week in a milder form: it *had* a depth bound, so a
crafted file made validation run forever rather than crash. A 4,921-byte file
of 13 levels each naming 40 offsets into the next took 40^13 visits; the fix
was a visited set recorded on completion, and it brought that file to 0.04 ms.
Here there is no bound at all, so the same file is a segfault.

**Read first**
- `../Iris-File-Extension/src/IFE_Runtime.cpp` — `VisitedBlocks`,
  `validate_nested_attributes`, `note_attributes`, `lift_attributes`. Note the
  comment on why the set is recorded on *completion* and never on entry.
- `../Iris-File-Extension/generated_source/IFE_Blocks.hpp` — `VisitPath`, and
  why ancestry rather than a global set.
- `src/FF_Compactor.cpp:299` and the three functions it recurses through.

### Locate

```bash
cd /Users/ryanlandvater/GitHub/FastFHIR
grep -n "archive_node\|archive_object\|archive_array" src/FF_Compactor.cpp | head
grep -rn "MAX_DEPTH\|max_depth\|visited\|VisitPath" src/ include/ | head
```

**Expect:** `archive_node` at ~299 with a `switch` on `node.kind()`, and the
second grep **empty**.

### XP-1.1 — A path and a visited set

- [x] **XP-1.1 DONE (2026-08-18, working tree, uncommitted).** `ArchiveContext` gained `std::vector<Offset> path`, `std::unordered_map<Offset, Offset> done`
  (map, not set — step 3 must "return the recorded offset", a set cannot hold it), and
  `MAX_NODE_DEPTH = 64` (comment cites the measured 8-block deepest chain in the generated
  model and the uncapped recursive types: Extension.extension, QuestionnaireResponse.item.item,
  PlanDefinition.action.action). `archive_node` applies the four checks in order; `done` is
  recorded on completion, never on entry. Node identity (`Node::offset()`) is protected,
  friend-granted to `ArchiveContext`; the shared-subtree test `tests/cpp/test_compactor.cpp`
  asserts via slot bytes and output size. Also shipped this session (untracked elsewhere):
  sealing tail consolidated into `seal_stream()` in `include/FF_Memory.hpp`, used by
  `Builder::finalize` and `Compactor::archive` — byte-identical output verified.


Add to `src/FF_Compactor.cpp`'s `ArchiveContext` (or beside it):

- `std::vector<Offset> path` — the ancestry of the node being archived.
- `std::unordered_set<Offset> done` — nodes fully archived.
- `static constexpr std::size_t MAX_NODE_DEPTH` — pick from the FHIR data
  model, not from a round number, and say in the comment which resource
  justifies it.

Order inside `archive_node`, exactly as IFE learned it:

1. depth over the bound → distinct error
2. offset already on `path` → cycle error
3. offset in `done` → return the recorded offset immediately
4. push, archive, pop, **then** insert into `done`

Step 4's order is the whole correctness argument: marking on entry would let a
node that reaches itself find its own entry and report success.

**Done when:** archiving a document with a shared subtree visits each node
once (assert with a counter in a test), and `ctest --preset ninja` is green.

### XP-1.2 — Distinct errors, not one code

IFE merged "too deep" and "cycle" into one code and paid for it twice: a
too-deep file reported a cycle that did not exist, and a test could not tell
which guard had fired — it passed with **both** guards disabled. Give the two
conditions distinct errors here from the start.

**Done when:** the two failures produce different messages, and each
red-greens **independently** — disable one guard, only its own test fails.

> **DONE (2026-08-19) — satisfied by the XP-1.3 work; verified, not assumed.**
> The two conditions already throw distinct messages:
> `"cycle in the stored graph at node offset N"` and
> `"node nesting exceeds the maximum depth of N"`, and
> `tests/cpp/ff_test_graph_bounds.cpp` asserts each names its own condition.
>
> Independence re-verified by disabling each guard in turn:
> * **depth guard off** -> only the two depth assertions fail; both cycle
>   assertions and the shared-DAG case still pass. Clean isolation.
> * **cycle guard off** -> the cycle case does not fail cleanly, it **hangs**
>   and is killed, so no assertions report at all. That is still unambiguously
>   red, and it is the behaviour XP-1.3 anticipated when it required a ctest
>   `TIMEOUT` ("without the visited set the sharing case does not fail, it
>   hangs; the timeout is what turns that into a reported failure"). The
>   `TIMEOUT 60` on the target is therefore load-bearing, not decorative —
>   do not remove it.

### XP-1.3 — The regression test

Add `tests/cpp/ff_test_graph_bounds.cpp`:

- a cycle → rejected, message names the cycle
- a chain past `MAX_NODE_DEPTH` → rejected, message names the depth
- a legal DAG with heavy sharing → **accepted**, and fast

Register it in `CMakeLists.txt` **and in `_BUILD_ALL`** — WO-1 in this file
exists because tests registered but not built report "Not Run".

Set a ctest `TIMEOUT`. Without the visited set the sharing case does not fail,
it hangs; the timeout is what turns that into a reported failure.

> **DONE (2026-08-18):** `tests/cpp/ff_test_graph_bounds.cpp` ships the three
> cases; registered in all five CMake lists with `TIMEOUT 60`.
>
> Making the cycle case red exposed that the XP-1.1 path check could not fire in
> the queue-based traversal (an ancestor's frame pops before its children
> process) and the `done` memo silently absorbed cycles: a node referenced back
> by one of its own descendants is already in `done`, so an unconditional
> memo-return skipped the very enqueue that closes the loop. Fix:
> `PendingWrite` snapshots the enqueuing node's full ancestry; the cycle guard
> and the archived-once memo both run at **enqueue** time (where the ancestry is
> live); and the process-time memo is conditional — it returns early only when
> no direct child of the re-processed node sits on the restored path
> (`child_touches_path`), so a re-walk that could newly close a cycle always
> reaches the enqueue-time throw. Arrays also route through `archive_node` so an
> array block gets the same guards.
>
> Verified: cycle and depth tests fail red when their guard is removed, pass
> green with it; the 200-parent shared DAG is accepted and fast; `ff_test_
> compactor` (shared subtree) and the full suite stay green except the
> pre-existing `py_roundtrip` red (A23/A24/A25/A26).

**Done when:** all three pass, and with the visited set removed the sharing
case times out rather than passing.

---

## XP-2 — Deep-validate the graph on open (P0)

**Why.** `Parser::Parser` calls `header.validate_full()`, which checks magic,
FHIR revision, layout flag and the checksum footer — and nothing else. It does
not bounds-check `ROOT_OFFSET` before storing it, and never walks the graph.
`FF_Parser.hpp:49` nonetheless tells the reader "`Parser` validates file
structure at creation time," which is the sentence that stops anyone asking.

IFE's `validate_file_structure` is the model: walk every offset edge, confirm
each block's self-offset and recovery tag, bound the depth, detect cycles.

### Locate

```bash
sed -n '45,55p' include/FF_Parser.hpp
sed -n '233,250p' src/FF_Parser.cpp
grep -rn "validate_deep\|validate_graph" src/ include/ | head
```

**Expect:** the docstring claim at ~49, `m_root_offset = header.get_root(...)`
with no bounds check, third grep **empty**.

### XP-2.1 — Bounds-check the root — ✅ DONE 2026-08-19
Reject a root offset that is not within `[HEADER_SIZE, m_size)` before storing
it. One comparison; it is the entry point to every traversal.

> **DONE.** `checked_root_offset()` in `src/FF_Parser.cpp`, applied in **both**
> constructors (the buffer+size one and the `Memory` one — the task text and its
> Locate block only showed the first; they had drifted apart before and a helper
> is what stops that recurring).
>
> Two refinements over the literal spec, both deliberate:
> * `FF_NULL_OFFSET` is **accepted**. It is the "no root" sentinel that
>   `m_root_offset` is default-initialised to, so rejecting everything outside
>   `[HEADER_SIZE, size)` would have broken every rootless stream.
> * The upper bound is `size - DATA_BLOCK::HEADER_SIZE`, not `size`. `off < size`
>   alone admits an offset in the last nine bytes, where the universal 10-byte
>   block header cannot fit — addressable but unreadable.
>
> Regression added to `tests/cpp/ff_test_graph_bounds.cpp` (already registered,
> so no new CMake wiring): valid root accepted, out-of-bounds rejected with a
> message naming `ROOT_OFFSET`, and the no-room-for-a-header case rejected.
> Red-greened — with the guard disabled exactly those three fail and the
> valid-root case still passes.

### XP-2.2 — Correct the docstring first — ✅ DONE 2026-08-19
Change `FF_Parser.hpp:49` to say what the code does today — header and
checksum — before adding anything. A comment that overstates coverage is worse
than no comment, and this one already cost a reviewer a wrong assumption.

> **DONE.** The claim "validates file structure at creation time" is replaced
> with what `FF_HEADER::validate_full` actually does, read out of the source
> rather than assumed: magic bytes, FHIR revision, the stream-layout flag, and
> the checksum block's **structural** integrity when present.
>
> Two things the old wording hid, now stated explicitly: the constructor does
> not walk the offset graph, and **it does not verify the checksum digest** —
> `FF_CHECKSUM::validate_full` only confirms the block is where it says it is
> and is not truncated; `Parser::checksum()` exposes the stored digest for a
> caller that wants to check it. The docstring now says every other offset
> remains untrusted after construction, and points at XP-2.3.

### XP-2.3 — `Parser::validate_FFHR_stream()` — ✅ DONE 2026-08-19

> **DONE.** Named `validate_FFHR_stream()` (Ryan, 2026-08-19 — after
> `validate_deep` and `validate_file_structure` were each tried; the final name
> is the one in the code). Declared in `include/FF_Parser.hpp`, implemented in
> `src/FF_Parser.cpp`.
>
> **Two responsibilities, exactly as specified:**
> 1. **The offset chain, including recovery.** For every reachable non-null
>    offset: the block's `VALIDATION` word must equal its own offset, and its
>    `RECOVERY` must equal the type the *schema* declares for that slot. A
>    `Patient.meta` holding a well-formed block that is not a `Meta` is
>    rejected — `FF_FIELD_BLOCK` carries `child_recovery = RECOVER_FF_META`,
>    and arrays are checked as `ToArrayTag(child_recovery)` so the element type
>    is enforced too.
> 2. **Byte ranges.** Every offset is bounds-checked *before* the bytes at it
>    are addressed, with room for a full block header; strings and arrays are
>    additionally checked against their declared length/stride×count so a
>    truncated payload cannot overrun the buffer.
>
> **It reads raw offsets, never Node/Entry.** The lens assumes the offsets it is
> given are trustworthy — precisely the assumption under test. The per-block
> checks mirror `DATA_BLOCK::validate_offset` but are open-coded so a failure
> names the offset *and the field it was reached through*.
>
> Cycle and depth guards use XP-1's policy, with `MAX_VALIDATION_DEPTH` set to
> the same 64 as the compactor's `MAX_NODE_DEPTH` and a comment saying they must
> not drift. `done` is recorded on completion, never on entry.
>
> **Measured cost — this is why it stays explicit.** On the largest Synthea
> fixture (50.8 MB, 913,809 blocks): `Parser` construction **1.7 µs**, the walk
> **108 ms**. Automatic validation would have made every open, including
> `Builder::query()`, pay ~0.1 s on a 50 MB stream. The XP-2.4 reversal was
> correct.
>
> **Optimised 551 ms -> 108 ms (5.1x) after profiling.** Two of the three
> hypotheses were wrong, which is worth recording so nobody re-derives them:
>
> | Change | Time | Verdict |
> |---|---|---|
> | first working version | 551 ms | — |
> | memoise the per-block field table | 477 ms | 13% — the `std::vector` copy was **not** the main cost |
> | `reserve()` the visited set | 379 ms | rehashing was 20% |
> | visited set -> bit-per-offset | 192 ms | the node-based `unordered_set` was ~half |
> | hoist the per-field bounds check | 186 ms | noise — **not** a factor |
> | recursion returns `bool`, not `FF_Result` | **108 ms** | the single largest win |
>
> The last one is the lesson: `FF_Result` carries a `std::string`, and the walk
> returned one from every level for ~18M field visits. Building millions of
> strings to say "fine" cost more than everything else combined. The message is
> now built once, on the failing path only.
>
> **Locality was measured and ruled out**, against a hypothesis that it was the
> floor: touching all 913,809 block headers sequentially costs **0.8 ms**, and in
> **shuffled** order **3.7 ms**. The walk was 50x slower than worst-case random
> access, so it was never a memory-bandwidth problem — it was our own code.
>
> **Two entry points, split on what an attacker can do (Ryan, 2026-08-19).**
> `validate_FFHR_stream()` is the anti-attack pass: it follows only slots that
> can carry an offset. `validate_FFHR_stream_deep()` additionally inspects
> inline scalar values (a bool outside {0,1,sentinel}; a dictionary code id that
> resolves to nothing). A nonsense scalar is not an attack — the byte sits
> inside a V-Table the structural pass has already bounds-checked — so it does
> not belong in the fast path.
>
> **The split buys correctness of scope, not speed, and the measurement says
> why: 96.2% of declared slots are offset-bearing** (1,550 of 1,611 — 641 ARRAY,
> 454 BLOCK, 298 STRING, 102 CODE, 52 CHOICE, 3 RESOURCE) against just 61 inline
> scalars (23 BOOL, 19 FLOAT64, 19 UINT32). Skipping scalars can therefore
> remove at most 3.8% of visits, and both passes measure ~107 ms on the 50 MB
> fixture. FHIR's shape defeats the optimisation: it is overwhelmingly strings,
> arrays, blocks and codes, every one of which must be followed.
>
> **Throughput measured in bytes understates it.** Work is proportional to field
> slots, not stream size: ~18.3M slots in 108 ms is **~170M field slots/sec**,
> about 6 ns (~20 cycles) each for a switch dispatch, a load and a compare.
> Getting materially below that needs *fewer visits*, not faster ones — a
> generated per-tag table of only the offset-bearing fields would skip the
> scalar slots entirely. Logged as a follow-up, not done.
>
> **`reflected_fields_view()` was added to the generator** (`emit/views.py`) —
> a `std::span` over the static `FIELDS` array, no allocation, no copy. The
> pre-existing `reflected_fields()` still returns by value and is still used by
> `Node::fields()`, `node_size()` (which allocates a whole vector just to read
> `.size()`) and the Python bindings — all on the documented read path, against
> README's "Reading a FastFHIR stream requires 0 heap allocations". Switching
> those over is its own task.
>
> **One bug this shook out, worth remembering:** the visited-bitset lookup was
> ordered *before* the bounds check, so a hostile offset indexed the bitset out
> of range — an out-of-bounds read inside the very function meant to reject it.
> It presented as a *non-deterministic* test failure, which is how heap
> overflows usually present. Bounds are now checked first, with a comment
> saying why the order is load-bearing.
>
> Ten cases in `tests/cpp/ff_test_graph_bounds.cpp` (already registered):
> clean stream passes; broken self-offset, wrong recovery tag, out-of-bounds
> child, and a cycle each rejected with a naming message; plus the
> `Patient.meta` schema-type case in both directions.

<details><summary>Original task text</summary>

#### `Parser::validate_deep()` — explicit, not automatic

**Ryan's decision (2026-08-19), which also answers XP-2.4: validation is an
explicit call the caller makes, NOT something the constructor does.** This was
briefly written up the other way and reversed the same day — the reversal is the
decision, and matches IFE, which this repository's split was modelled on:
`IrisCodec::validate_file_structure(ptr, size)` is a method the caller invokes,
and IFE's README pairs it with a blunt warning that skipping it means "uncaught
runtime exceptions will be thrown". Loud expectation, caller's choice.

**Why opt-in wins here.** Making it automatic would have cost three things that
are cheap to keep:

* `Builder::query()` (`include/FF_Builder.hpp:183`) constructs a `Parser` over a
  **build in progress**. Its docstring promises "nearly zero-cost as it only
  populates CPU registers", and architecture.md §4.3 says readers of it must
  accept transient `FF_NULL_OFFSET`s. Validating a deliberately incomplete graph
  is both expensive and semantically wrong.
* `src/FF_Builder.cpp:54` constructs a `Parser` inside a try/catch to hydrate
  from an existing archive — another internal path that wants no walk.
* Open stays **O(1)**. README's "nanosecond read times from the instant the
  message hits RAM" and architecture.md §8.1 stay true, and no benchmark
  re-sync is triggered (execution-contract rule 9).

None of those need a carve-out mechanism if the walk is never implicit.

**What `validate_deep()` checks** — walk the offset graph from the root and, for
every edge whose offset is not `FF_NULL_OFFSET`:

1. the offset is in bounds, with room for a full block header (the same rule
   XP-2.1 applies to the root);
2. the block's `VALIDATION` word equals **its own offset** — the self-offset
   chain;
3. the block's `RECOVERY` tag equals what the referring slot said it would be.

**Most of this already exists — do not rewrite it.**
`DATA_BLOCK::validate_offset(base, type_name, recovery_tag)` in
`src/FF_Primitives.cpp` already performs checks 2 and 3 exactly, with messages
naming the type and both tags. XP-2.3 is the **traversal** that applies it to
every reachable block, not a new validator. Cycle and depth protection come from
XP-1's `path` / `done` / `MAX_NODE_DEPTH` machinery in `src/FF_Compactor.cpp`,
which is built and tested — lift it somewhere shared rather than writing a
second copy that can drift apart from it.

**The documentation duty is the whole risk of opt-in.** A check nobody knows to
call protects nobody. `FF_Parser.hpp`'s class comment (corrected in XP-2.2)
already states that offsets remain untrusted after construction and points here;
README and architecture.md need the same sentence, in IFE's register: anyone
reading a stream they did not produce should call `validate_deep()` first.

**Done when:** hand-corrupted fixtures — an out-of-bounds child offset, a wrong
self-offset, and a wrong recovery tag — are each rejected with a message naming
the block and the offset; every Synthea fixture passes; `Builder::query()` and
normal `Parser` construction are measurably unchanged; and the docs tell callers
when to invoke it.

</details>

---

## XP-3 — `--check` and `--validate` for the generator (P1)

`generator/__main__.py` takes only `--output-dir` and `--keep-specs`. There is
no way to ask "is `generated_src/` current?" or "is the ledger self-consistent?"
IFE runs both on every build and in CI.

- **XP-3.1** `--validate`: ledger self-consistency — unique tag values, unique
  code IDs, every referenced tag defined, no gaps that would renumber.
- **XP-3.2** `--check`: regenerate in memory, compare against
  `generated_src/`, exit non-zero on drift. Reuse `tests/generator/wire_witness.py`
  for the comparison rather than a text diff — this repository already learned
  that the gate is wire stability, not source text.

**Done when:** both exit 0 on a clean tree; `--check` goes red after touching
an emitter and green after regenerating; `--validate` goes red on a duplicated
tag value.

---

## XP-4 — Port `portability_lint.py` (P1)

Copy `../Iris-File-Extension/tools/portability_lint.py` and retarget it. Each
of its six checks was bought with a CI round-trip there:

| Check | What it catches |
|---|---|
| platform macros | `windows.h` macros (`IN`, `OUT`, `ERROR`, `PLANES`) used as identifiers |
| SAL annotations | `__in`/`__out` as parameter names — `<yvals.h>` defines them in **every** MSVC TU |
| paths in string literals | a filesystem path through a C string literal, where `\a` is a bell |
| ctest without `-C` | multi-config generators find no executables |
| Bazel-declared headers | a public header the sandbox hides because no filegroup names it |
| workflow include paths | hand-compiled CI jobs on a stale `-I` layout |

Two apply immediately: this repository has `windows.h` under `_WIN32` in
`src/FF_Builder.cpp`, and a `BUILD.bazel` with filegroups that can drift the
same way.

**Do not** copy the `generated_source` special case blindly — port the
*lesson*: IFE's SAL check treated a missing generated directory as clean,
so on a fresh clone it reported the generated layer clean without reading it.
Make an absent `generated_src/` a finding here, not a pass.

**Done when:** `python3 tools/portability_lint.py` exits 0 on the tree, and
red-greens on a deliberately introduced `__out` parameter name.

---

## XP-5 — CI (P1)

`.github/` holds issue templates and a PR template. There are **no workflows**.
Nothing gates a push: not the build, not `ctest`, not the wire gate that
already exists in `tests/generator/`.

Port the shape of `../Iris-File-Extension/.github/workflows/ci.yml`:

- **XP-5.1** build + `ctest --preset ninja` on Linux and macOS
- **XP-5.2** the existing `tests/generator/` pytest wire gate — it is the best
  thing in this repository and nothing runs it automatically
- **XP-5.3** sanitizers: `cmake --preset xcode-asan`, or an ASan/UBSan Linux leg
- **XP-5.4** `tools/portability_lint.py` once XP-4 lands
- **XP-5.5** a big-endian leg (s390x under qemu). IFE added one after two
  independent hand-written big-endian packed-width implementations proved
  wrong, neither reachable by any test on any developer machine. This
  repository has `LOAD_U*`/`STORE_U*` primitives with the same exposure.

⚠ **XP-5.6 — network in CI.** First configure downloads FHIR packages, so a CI
job needs either network or a cached bundle. Decide which, and say what happens
when `packages.fhir.org` is down. Produce the analysis and STOP.

---

## XP-6 — Includes and narrowing (P2)

- **XP-6.1** `src/FF_Builder.cpp`, `src/FF_Ingestor.cpp` and
  `src/FF_Primitives.cpp` use `memcpy`/`memset` and get `<cstring>` only
  through `FF_Memory.hpp`/`FF_Ops.hpp`. Include it directly. IFE shipped the
  same defect this week in a test writer.
- **XP-6.2** Range-check narrowing before it reaches the wire.
  `static_cast<uint32_t>(x.size())` appears at six sites in
  `src/FF_Compactor.cpp` and `src/FF_Extensions.cpp`. IFE's parallel case: a
  key over 65535 bytes wrote a truncated length beside untruncated bytes and
  `store()` reported success — every later read then sliced from the wrong
  boundary. Reject at the writer, where the payload is in hand.

**Done when:** the includes are explicit, and an oversized input is rejected
with a message rather than silently truncated. Red-green the truncation case.

---


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
   `dictionaries/master_codes.json`, vtable offsets, `FF_HEADER` layout). If a task seems to
   require it, the task is wrong — stop and flag it.
6. **Never hand-edit generated files** (`generated_src/` — including `FF_Codes.hpp`,
   the dictionary tables, and `FF_Recovery.hpp`, which moved there from `include/` on
   2026-08-19). Fix the emitter in `generator/emit/` instead. `dictionaries/` now holds
   only the JSON ledgers.
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

# ▶ IMMEDIATE WORK ORDERS — start here

Written 2026-08-12 for an agent picking this up cold. Five work orders, in order. **Do one
per session, in the order given.** Each is self-contained: exact commands, exact edits,
exact expected output.

**Rules that override anything else in this file:**

1. **Run the *Precheck* first.** If its output does not match what is written, **STOP** and
   report what you saw instead. Do not improvise, do not "fix" the mismatch, do not
   continue to the next step. A mismatch means the tree moved and the work order is stale.
2. **Do not commit.** Ryan commits. Leave changes in the working tree and report.
3. **Never edit generated files** (`generated_src/`, which since 2026-08-19 includes `FF_Recovery.hpp`). If a fix
   seems to need one, the fix belongs in `generator/emit/` — stop and say so.
4. **Never change a wire constant** (recovery tag values, dictionary IDs, vtable field
   order, `FF_HEADER` layout). If a step seems to require it, STOP.
5. Build with `cmake --preset ninja && cmake --build --preset ninja` unless told otherwise.
   First configure needs network and takes ~60 s (it downloads FHIR packages).
6. If a command takes more than ~15 minutes, it has hung. Stop and report.

**Status (2026-08-13):**

| WO | Task | Status |
|---|---|---|
| WO-1 | A20 + A22 — trustworthy test signal | DONE — 0 "Not Run" (was 6); A20.1/A22.1 shipped in commit `6f7c9aa`; A20.2, A22.2 open |
| WO-2 | A14 — small-input ingest | DONE — 1 MiB arena floor; A14.1/A14.3 ticked; A14.2, A14.4 open |
| WO-3 | A16 — wire gate prefix rule | DONE — gate delegates to `_check_permanence`; A16.1/A16.2 ticked |
| WO-4 | A21 — runnable Python tests | DONE — readme py_* 11/11; **py_roundtrip red until A23/A24/A25/A26** (was passing vacuously — see A23 finding 4) |
| WO-5 | E13 — lint debt (315 violations) | OPEN — zero *new* violations added by the WO-2/3/4 work |

---

## WO-5 — Clear the lint debt (task E13)

**Why:** the documented lint command **fails today**. It examines all 37 files and reports
**315 ruff violations**. E1's CI recipe runs this exact command, so CI would be red from its
first commit — and a step that is red on day one gets ignored or wrapped in
`continue-on-error`, which is how a gate dies. Land this before CI.

> **Re-measured 2026-08-18** with the same tooling as the original count (ruff 0.15.1 /
> black 26.5.1), so the movement is code drift, not a tool upgrade. Ruff went **307 → 315**
> (`E501` 143 → 150, `I001` 9 → 10; everything else unchanged). **black is now clean** —
> the 2 files it wanted to reformat no longer need it, so step 2 below is a no-op and the
> `black --check` half of the gate already passes. The generator suite is **46** tests, not
> the 40 recorded below.

**Read task E13 in Block E first**, including the `> Unreproduced observation` note at the
end of it. Do **not** rewrite the `pyproject.toml` `include` patterns: an earlier session
recorded the command appearing to check zero files, that has never reproduced, and changing
the config on the strength of it would be acting on evidence nobody can reproduce.

### Precheck

```bash
cd /Users/ryanlandvater/GitHub/FastFHIR
ruff check generator tests/generator 2>&1 | grep -E "Found [0-9]+ error|No Python files"
black --check generator tests/generator 2>&1 | tail -1
```

Expect `Found 315 errors.` and `37 files would be left unchanged.` (black is already
clean — see the re-measurement note above.) If you instead see `No Python files found
under the given path(s)`, **STOP** —
that is the unreproduced condition; capture the full output plus `ruff check --show-files`
and report it, because that evidence is what E13's note is asking for.

### Steps

1. `ruff check --fix generator tests/generator` — clears 89 automatically. Inspect the
   diff before going further; `--fix` touches imports and f-strings.
2. `black generator tests/generator` — **no-op as of 2026-08-18**; black already reports
   all 37 files unchanged. Run it anyway to confirm, but expect no diff.
3. Re-run `FASTFHIR_GENERATED_DIR=generated_src python3 -m pytest tests/generator -q`.
   **Must still be 46 passed.** The autofixes touch generator code; if any test breaks,
   revert that specific fix rather than adapting the test.
4. Fix the remaining ~226 by hand. Breakdown: `E501` (line >100 chars) 150, `F541`
   (f-string with no placeholder) 56, `ANN001/201/202` (missing type hints) 70, `F401`
   (unused import) 21, `I001` (import order) 10, `B007` (unused loop var) 4, `UP015`/`F841`
   4. **No `F821`** — nothing here indicates a live bug; this is style debt against the
   project's own declared standard, not defect triage.
5. Keep the autofixes and the manual fixes as **two separate commits** so review stays
   tractable.

### Verify

```bash
ruff check generator tests/generator && black --check generator tests/generator
echo "exit=$?"
FASTFHIR_GENERATED_DIR=generated_src python3 -m pytest tests/generator -q
```

**Pass condition:** `exit=0`, and 46 generator tests still pass. Confirm the wire witness
did not move: `git diff --stat tests/generator/golden/wire_witness.json` must be empty.

---

**After WO-5:** the suite is green, the gates are real, and the linter works — that is the
point at which CI (task **E1**, unblocked, Q6 answered) is worth standing up, followed by
the sanitizer leg (**G2**) and then Block **K**, the conformance validation layer.

---

## Block A — Build & correctness fixes (highest priority, all independent)

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

> **Gate is green and honest for all three families (2026-08-19).** A4.1's 08-12 note
> said the codes/tags sections were "structurally unfillable by the current witness" —
> that was true *then*; A15 fixed the witness to read the regenerated tree, which made
> the golden's codes/tags sections fillable (5,796 codes at eb008e2). One half of the
> chain was still broken: `generator/emit/code_names.py` and
> `generator/emit/code_ids.py` wrote their output to the repo `generated_src/` via
> hardcoded module paths, **ignoring `--output-dir`**, so the witness's tmp-tree
> regeneration produced an empty codes section and `test_dictionary_codes_stable`
> reported all 5,796 golden codes as DELETED. Fix (with §C, same change set): all three
> hardcoded-path emitters (`code_names`, `code_ids` dictionary tables, `recovery_tags`)
> now take `output_dir` from the pipeline; the witness reads `FF_Recovery.hpp` from the
> witnessed tree exactly as it already did for `FF_Codes.hpp` (previously tags were read
> from the repo path — the asymmetry that masked the same bug for tags). Verified:
> `pytest tests/generator -q` = **46/46**, and a fresh `python -m generator
> --output-dir <tmp>` tree now contains `FF_Codes.hpp`, `FF_Recovery.hpp`, and
> `FF_Dictionary_Strings.cpp` (previously missing).

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
- [ ] A8.2 Make `SIZE_FF_CODE` (`src/FF_Primitives.cpp`) agree with `ENCODE_FF_CODE` **by
      construction**. Verified 2026-08-14: they agree *today* only by numeric coincidence,
      and only on the one branch currently reachable. `SIZE_FF_CODE` sizes a dictionary
      miss as `FF_STRING::HEADER_SIZE + len` = **14 + len**; `ENCODE_FF_CODE` writes an
      `FF_CODEABLE_CONCEPT`, consuming `FF_CODEABLE_CONCEPT::HEADER_SIZE + payload` =
      **12 + payload**. On the `UNKNOWN` branch payload is `2 + len`, and `12 + 2 == 14`.
      Two independently-defined constants happening to sum correctly is not an invariant.
      **Sequencing: A8.1 must not land before this is fixed.** `external_system` is emitted
      **zero** times in `generated_src/` today, so the divergent branches are unreachable;
      populating `external_system_map` arms them all at once — SNOMED payload 8, RxNorm/CPT/
      DICOM/MDC 4, CVX 2, and every variable-ASCII system (UCUM, LOINC, NDC, ICD, ISO, UNII,
      pCLOCD) at `12 + len` against a `14 + len` claim. Note SIZE takes no `system` argument
      while STORE does, so the two cannot agree until the signature carries it.
      With A23.5 in place these now throw instead of corrupting — loud, but every
      externally-coded field fails to ingest.
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

> **Root cause identified (2026-08-12, Phase 0): the CLI sizes the arena at 2× the input
> JSON length, with no floor and no growth path.**
>
> ```cpp
> // tools/ingestor/FF_Ingest.cpp:179-182 — the comment states the assumption that fails
> // HEURISTIC: Clinical JSON is heavy on syntax (quotes, braces, keys).
> // FastFHIR binary is dense. 2x input size is a safe "one-and-done" allocation.
> size_t capacity_hint = json_buffer.size() * 2;
> auto memory = Memory::create(capacity_hint);
> ```
>
> The heuristic holds asymptotically and fails at the bottom: the 54-byte `FF_HEADER` and
> the per-block vtables are *fixed* overhead, so for a small document they dominate and 2×
> the input is not enough. "One-and-done" is the part to revisit — there is no fallback when
> the estimate is wrong.
>
> FastFHIR's binary form is *larger* than the JSON for small, lean inputs — a 54-byte
> `FF_HEADER` plus vtables and blocks easily exceeds twice a 66-byte Patient. So the arena is
> undersized and the failure is deterministic, not intermittent. Two symptoms, one cause:
>
> | Input | Arena (2×) | Result |
> |---|---|---|
> | Single Patient, 66–152 B | 132–304 B | `FastFHIR VMA Capacity Exceeded` (`src/FF_Memory.cpp:393`), rc=1 |
> | Single Patient, ≥ 153 B | ≥ 306 B | succeeds |
> | 2-entry bundle, 201 B | 402 B | **`Ingestion aborted due to worker thread crash`**, rc=1 |
> | Same bundle padded to 411 B | 822 B | succeeds |
> | 50-entry bundle, 3 649 B | 7 298 B | worker thread crash — bundles need more per byte |
> | 44 MB Synthea bundle | 88 MB | succeeds, 12/12 |
>
> Padding the JSON with one junk string inflates `json_buffer.size()`, hence the arena,
> hence success — which is what makes the sizing the culprit rather than any property of the
> data. Reproducer:
>
> ```bash
> # 190 bytes -> 380-byte arena -> worker thread crash, 5/5 runs
> printf '{"resourceType":"Bundle","type":"collection","entry":[{"resource":{"resourceType":"Patient","id":"p1","active":true}},{"resource":{"resourceType":"Observation","id":"o1","status":"final"}}]}' > /tmp/tiny.json
> ./build/ff_ingest /tmp/tiny.json /tmp/tiny.ffhr   # rc=1, every time
> ```
>
> Which of the two symptoms you get depends on how far under capacity you are: a 119-byte
> 1-entry bundle raises `VMA Capacity Exceeded` before any worker starts, while 190 bytes
> gets far enough to crash a worker. Both are the same shortfall; use the 190-byte input
> when you want the A14 symptom specifically.
>
> **This is a CLI-scoped defect, which is why the suite is green** — all 20 C++ tests pass
> and `ff_test_bundle` passed 25/25 consecutive runs, because those tests size their own
> arenas. It also fits A14.2's original hypothesis exactly: under-capacity, `claim_space()`
> returns `FF_NULL_OFFSET`, and a worker uses `0xffffffff` as an offset. The clean abort
> seen today is a guard catching what used to be `EXC_BAD_ACCESS`.
>
> Corrects an earlier note in this file that read the single-resource size cliff (152/153 B)
> as *the* trigger: that cliff is the same root cause seen through the single-resource path,
> and bundles fail well above it.

- [x] A14.1 Give `capacity_hint` a floor and a growth path. A minimum arena (not a magic
      literal — derive it from `FF_HEADER` size plus a stated minimum block budget, with a
      comment) plus either a retry-on-capacity or a first-class grow. Decide whether 2× is
      the right multiplier at all, or whether the estimate should come from the parsed
      token/element count rather than raw byte length; record the reasoning next to the
      constant so the next reader does not have to re-derive it.
- [ ] A14.2 Audit `claim_space()` failure handling on the worker path regardless of A14.1: a
      `FF_NULL_OFFSET` return used as an offset is the `0xffffffff` fault, and under-capacity
      must surface as a clean diagnostic naming the shortfall, never as a crash. This is the
      defect that outlives the sizing fix.
- [x] A14.3 Check every other `Memory::create` call site for the same pattern —
      `grep -rn "Memory::create" src/ tools/ python/ tests/` — including the Python bindings,
      which are the next most likely place a user hits it.
- [ ] A14.4 Add the tiny-bundle reproducer as a checked-in test (pairs naturally with B7's
      fixture work) and run it under ASan. Without it this regresses silently: no current
      fixture is small enough to catch it.
- Acceptance: the reproducer above exits 0; a deliberately undersized arena produces a
  diagnostic naming the capacity shortfall, not a crash.
- Verify: `./build/ff_ingest /tmp/tiny.json /tmp/tiny.ffhr && echo OK`

> **Result (2026-08-13, WO-2):** Fix applied and verified. `tools/ingestor/FF_Ingest.cpp`
> now floors the arena at `FF_MIN_ARENA = 1 MiB`
> (`std::max(json_buffer.size() * 2, FF_MIN_ARENA)`), reasoning recorded in the comment.
> Hypothesis confirmed under lldb before editing: `capacity_hint = 132` (2× the 66-byte
> input) against a 245-byte fixed minimum (54-byte `FF_HEADER` + 191-byte `FF_PATIENT`
> vtable). Verified: the 66-byte Patient ingests rc=0; a size sweep (73–5 083 bytes) all
> `ok`; 19/19 `cpp_*` tests pass. Regression test added to `tests/cpp/test_bundle_ingest.cpp`
> (tiny Patient through the same Builder/Ingestor/Parser pipeline, asserts `id == "p1"`);
> `cpp_ff_test_bundle` green. Growth path deliberately not added: with the floor, inputs
> < 512 KiB get 1 MiB (ample for the ~245 B fixed overhead) and inputs ≥ 512 KiB keep the
> asymptotically-ample 2×, so the failure region is closed without one. A14.3 audit: the
> only other same-class sites are `tools/compactor/FF_Compact.cpp:334`
> (`Memory::create(parse_size)` — compact output can exceed input for tiny streams) and the
> Python bindings (`FF_PythonBindings.cpp:43`, caller-supplied capacity); `ff_roundtrip`
> (256 MB default) and the tests use fixed sizes. A14.2 (clean `claim_space()` failure
> diagnostics) and A14.4 (ASan reproducer) remain open.

---

### A19. Configure fails on a clean tree — no Python version floor

**Context:** `CMakeLists.txt:66` called `find_package(Python3 REQUIRED COMPONENTS Interpreter)`
with no version requirement, so CMake takes whatever `python3` it finds first. On macOS that
is the Xcode Command Line Tools 3.9.6, ahead of any newer interpreter on `PATH`. The
generator then dies at import:

```
File "generator/emit/codesystems.py", line 86, in <module>
  ) -> tuple[str | None, set[str]]:
TypeError: unsupported operand type(s) for |: 'type' and 'NoneType'
CMake Error at CMakeLists.txt:75 (message): Code generator failed
```

PEP 604 (`X | None`) in an *evaluated* annotation needs 3.10+. Three modules use it without
`from __future__ import annotations`: `emit/codesystems.py`, `emit/code_ids.py`,
`emit/extensions_known.py`. 15 of the 25 generator modules lack that import, so any of them
can join the list silently. `pyproject.toml` already declares the intended floor
(`target-version = py311` for ruff and black); CMake simply never enforced it.

- [x] A19.1 `find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)` at `CMakeLists.txt:66`,
  with a comment recording why. Verified: configure then selects 3.14.6 and succeeds.
  *(Applied during Phase 0; commit alongside the rest of this task.)*
- [ ] A19.2 Add `from __future__ import annotations` to the 15 modules that lack it, so the
  floor is a declared property of the code rather than an accident of which interpreter
  CMake found. `grep -L "from __future__ import annotations" $(find generator -name '*.py')`
  lists them. Import-only change; the wire witness must not move.
- [ ] A19.3 State the floor once more where a human reads it: a `requires-python = ">=3.11"`
  line in `pyproject.toml` (it has no `[project]` table yet — coordinate with H1, which
  creates one) and one line in `CONTRIBUTING.md`'s build prerequisites.
- Locate: `grep -n "find_package(Python3" CMakeLists.txt` (three call sites; only the first
  needs the floor — CMake caches `Python3_EXECUTABLE` for the other two).
- Acceptance: `rm -rf build && cmake -S . -B build …` succeeds on a machine whose default
  `python3` is 3.9.
- Verify: `cmake -S . -B /tmp/ff_cfg_probe -DFASTFHIR_RUN_GENERATOR=ON 2>&1 | grep "Found Python3"`
  → reports a version ≥ 3.11.

### A20. `build_all` does not build the test binaries

**Context:** `_BUILD_ALL` (`CMakeLists.txt:444-452`) is `fastfhir ff_export ff_compact`, plus
`fastfhir_ingestor ff_ingest`, `ff_test_readme` and `fastfhir_python` under their options. The
six standalone unit-test executables created by `add_ff_cpp_test` (`:274-281`) and the
`ff_roundtrip` harness (`:264`) are **not in the list**, but they *are* registered as ctest
entries (`:287-289`). So the documented sequence in `CLAUDE.md` —
`cmake --build build --target build_all -j` then `ctest` — reports six tests "Not Run"
(`Unable to find executable: build/ff_test_primitives`) and `py_roundtrip` failing on a
missing harness. All six pass once built: measured 20/20 C++ green after
`cmake --build build` with no target.

The comment at `:285` records the previous half of this bug ("These were built but never
registered, so they compiled and never ran"). The registration was fixed; the build side was
not, producing the exact inverse.

- [x] A20.1 Append the six unit-test targets and `ff_roundtrip` to `_BUILD_ALL` inside the
  `if(FASTFHIR_BUILD_TESTS)` guard. Reuse the same target list the `foreach` at `:287` walks
  rather than writing it twice — a third copy is how this bug recurs. (6f7c9aa)
- [ ] A20.2 Extend the comment at `:285` to say that a target must be in **both** places, and
  that `add_ff_cpp_test` does neither for you.
- Acceptance: from a clean build dir, `cmake --build build --target build_all -j` followed by
  `ctest` produces zero "Not Run".
- Verify: `rm -rf build && cmake -S . -B build -DFASTFHIR_BUILD_TESTS=ON -DFASTFHIR_BUILD_INGESTOR=ON && cmake --build build --target build_all -j && ctest --test-dir build -N | tail -1`
  then `ctest --test-dir build -R cpp_ --output-on-failure`.

### A22. `DiffKind` is undefined on the round-trip harness error path

**Context:** `tests/python/test_roundtrip.py:33-38` imports `diff_doms`,
`filter_allowlisted`, `format_diff_report` and `DiffEntry` from `roundtrip_diff` — but not
`DiffKind`, which the module also defines (`roundtrip_diff.py:19`). Both exception handlers
in `run_roundtrip_test` construct `DiffEntry(kind=DiffKind.VALUE_MISMATCH, …)`, so the
error path raises `NameError: name 'DiffKind' is not defined` and destroys the diagnostic it
was written to produce:

```
tests/python/test_roundtrip.py:107, in run_roundtrip_test
    path="", kind=DiffKind.VALUE_MISMATCH,
NameError: name 'DiffKind' is not defined
```

Observed masking a real failure: the harness was genuinely missing (A20), and instead of the
intended "C++ harness not found: … Build with: cmake --build . --target ff_roundtrip", the
run died with a `NameError`. Classic error-path-never-executed bug.

- [x] A22.1 Add `DiffKind` to the import list. (6f7c9aa)
- [ ] A22.2 Add a test that exercises both handlers — point the harness path at a
  nonexistent binary, and at a command that sleeps past the timeout — asserting the intended
  message reaches the caller. Without it the fix is unverified: neither branch has ever run.
- Acceptance: with `FASTFHIR_ROUNDTRIP_HARNESS` set to a nonexistent path, the suite reports
  the "harness not found" message and not a `NameError`.
- Verify: `ctest --test-dir build -R py_roundtrip --output-on-failure`.

---

### A23. Synthea Bundle streams are corrupted — reader/writer layout disagreement; crashes are the tip

**Context (revised 2026-08-13 by the A23.1 bisect):** every one of the 111 Synthea
fixtures is a *Bundle* (patient-bundle exports — there are no per-resource files). The
"21 crashing / 90 passing" split from the initial scan is a size artifact: ~21 large
bundles (>600 KB) SIGSEGV the harness (rc=139), but the "passing" ones are corrupt too —
`Alberto639_…json` exits 0 and emits **invalid JSON** (`"period":,` at char 3928). The
crash is the tip of systemic corruption in the bundle encode path, not a single element
shape.

**What the bisect established:**

1. **Intermittence is a race on top of a deterministic bug.** With the default multi-
   threaded ingestor a bundle crashes ~9/10 runs and occasionally exits 0 or 1. With
   `Ingestor(…, concurrency=1)` the corruption is **byte-for-byte deterministic**: rc=1,
   `Error: FastFHIR: Node is not a string or code.`, 14344 B of partial JSON. The race only
   escalates the same corruption into the segfault (the ASCII-as-offset `LOAD_U16`).
2. **The writer stores the data; the reader cannot reach it.** The sealed stream contains
   270 timestamp strings, including both Encounter period bounds, yet **all 44 `period`
   blocks in the output are empty** (`"period":,`). The "truncated" reference
   (`urn:uuid:a172af40-3e` + garbage) is the reader taking a length/offset from the wrong
   byte position — the same disease.
3. **The corrupted blocks are exactly the R4≠R5-divergent ones.** From the golden witness:
   `FF_ENCOUNTER` R4=250/R5=338, `FF_ENCOUNTER_PARTICIPANT` 58/66, `FF_CARETEAM_PARTICIPANT`
   66/76. This is the A17 class: layout depends on revision, and the R4-prefix invariant
   "holds only as a side effect of iteration order" (A17, open). Bundles are the first
   fixtures to push R4 JSON through the R5 model at scale — the C++ suite's hand-built
   patients never touch these blocks.
4. **`py_roundtrip` was passing vacuously** (2026-08-13 — agent error, corrected): a
   temporary Bundle skip in `discover_fixtures` excluded *all* fixtures — every fixture is
   a Bundle — so the gate ran nothing and reported PASS. The skip is reverted;
   `py_roundtrip` is honestly red until this task lands.
5. **`print_json` has a robustness bug of its own:** it emits `"period":,` (invalid JSON)
   for a truthy-but-empty block node instead of `{}` or skipping — every corrupt stream
   becomes a parse error instead of a diff.

**Hypothesis (strong, not yet proven):** the generated per-version layouts for the
R4≠R5-divergent blocks disagree between the write path (ingestor's `*_from_json` store
emit) and the read path (vtable) — most likely the merge precedence A17 warns about
(`merge.py` lays out fields on first sight, so revision order is load-bearing). Next step:
walk one Encounter's `period` slot in a sealed stream against both the R4 and R5 vtables
and name the divergent field.

- [x] A23.1 Minimal repro + attribution (DONE 2026-08-13). Repro is now trivial: any
      Synthea bundle with the 1-worker ingestor is deterministic; with N workers ~90% crash.
      The element-level bisect (entry[] halving, leave-one-out, singles) found **no single
      entry that crashes alone** — the corruption is not entry-local, it is the bundle
      encode path itself.
- [ ] A23.2 Walk one Encounter's `period` slot in a sealed stream against the R4/R5 vtables
      (`FF_Encounter_internal.hpp`, `FF_DataTypes_internal.hpp`), name the divergent field,
      and fix the generator (likely `generator/model/merge.py` layout precedence — A17's
      actual fix) or the reader. Land A17.1/A17.2 in the same change:
      `test_version_prefix.py` is the regression gate this bug class needs.

> **Progress (2026-08-14):** two deterministic corruption bugs found and FIXED (details and
> full root-cause analysis in `~/Documents/fixes/FastFHIR-bundle-encoding-root-cause.md`):
> - **Bug A — dateTime fields dropped at ingest.** `generator/emit/ingest_mappings.py`
>   emitted a "requires Builder context; skipping" stub for every `unique_ptr`-stored
>   string-like field (dateTime/instant/date/time fall through TYPE_MAP to the DEFAULT
>   complex-block mapping → `data_type="Offset"` → `unique_ptr<std::string_view>` POD
>   member), so `Period.start/end`, `recordedDate`, and 60 more fields were never parsed.
>   Fixed: the emitter now parses `data_type == "Offset"` via
>   `make_unique<std::string_view>`. Stub count 63 → 28 (remaining are url_idx/extensions).
> - **Bug B — code[] arrays undercounted (A8.2 class, proven).** The generated SIZE for
>   `code[]` arrays used dictionary-aware `SIZE_FF_CODE` (0 for dict hits) while the STORE
>   always wrote an `FF_STRING` (14+len) — every resource with a dictionary-backed category
>   (`AllergyIntolerance.category = ["medication"]`) was claimed 24 bytes short, so the next
>   resource's write overwrote the previous one's tail ("RhinoJ" = 5 real bytes + the next
>   block's validation). Fixed in `generator/emit/store.py`: SIZE now uses `SIZE_FF_STRING`.
> - Verified: harness rc 1 → 0; output 14,530 → 183,572 bytes on one bundle.
> - **Fixed (A23.3, 2026-08-14):** the READER emitted `"period":{"start":,"end":}` (invalid
>   JSON) for Period blocks whose sealed slots are full — `ParserOps::standard_entry_as_node`
>   kept the schema kind (`FF_FIELD_BLOCK`) after the pointer hop even when the stored block
>   is an `FF_STRING`, so `is_empty()` walked the string as a field-less block → declared
>   empty → `print_json` emitted `"key":` with no value. Fixed at the source: the default
>   branch now re-derives the node kind from the actual recovery tag
>   (`RECOVER_FF_STRING` → `FF_FIELD_STRING`), the same convention `resolve_choice` and
>   `standard_node_entries` already used — so `is_empty()`/`print_json`/`is_string()` all
>   agree without adding recovery-dispatch branches. Same latent mismatch also fixed in
>   `standard_node_lookup_index` (OFFSET/INLINE_BLOCK array paths, reachable from the
>   Python bindings). The multi-threaded race (Bug D) is still open.

> **Progress (2026-08-14, second session) — the contract is now enforced, and enforcing it
> found three more defects.** Bug B was possible because the single load-bearing invariant
> of the arena design — *`claim_space(SIZE_FF_X(pod))` then `STORE_FF_X` must consume
> exactly that many bytes* — was never asserted. It is now (A23.5). Turning it on
> immediately proved Bug B's fix incomplete: **the same undercount was live in three more
> places**, because `SIZE_FF_STRING("")` returned 0 while `STORE_FF_STRING(base, off, "")`
> writes — and returns — a full 14-byte `FF_STRING` header. Reproduced end-to-end:
>
> | Input | claimed | stored | verdict |
> |---|---|---|---|
> | `"period": {"start": "", "end": ""}` | 398 | 426 | 28-byte overrun (2 × 14) |
> | `"given": ["", "John"]` | 370 | 384 | 14-byte overrun |
>
> The first was **newly armed by Bug A's own fix**: before it, the `unique_ptr` was always
> null and the trap was unreachable; after it, `""` parses as `simdjson::SUCCESS` into a
> *non-null pointer to an empty view*, so SIZE tested `!= nullptr` and added 0 while STORE
> tested `!= nullptr` and wrote 14. The array case cannot be fixed by a call-site guard —
> skipping an element would change the element count — so the fix is at the source: a size
> function reports what the store writes, and "absent" stays the caller's `!empty()` guard.
> No wire-format change: those 14 bytes were already being written, just not claimed.
> **0 of the 111 Synthea fixtures contain a single empty string**, which is why the corpus
> could never have caught this. See `~/Documents/fixes/FastFHIR-bundle-encoding-root-cause.md`
> for the original two bugs; that doc's claim that the *scalar*-code case "actually agrees"
> is true only by coincidence — see A8.2, now re-scoped.

- [x] A23.3 Fix `print_json`'s empty-block emission (`"period":,` → skip or `{}`).
      DONE 2026-08-14 — root-cause kind fix in `src/FF_Parser.cpp` (see above). Verified:
      all 111 fixtures emit valid JSON (`for f in build/synthea_fhir_r4/*.json; do
      ./build/ff_roundtrip "$f" | python3 -m json.tool >/dev/null`); Rodrigo fixture
      183,572 → 215,274 bytes with `"period":{"start":"1994-01-17T16:25:04+00:00",...}`;
      299 timestamps emitted. `py_roundtrip` still red on all 111 — now honestly, with
      real structural diffs (dictionary-`code` value mismatches and choice-type
      mismatches, write-side, tracked separately); previously the invalid-JSON emission
      masked the comparison entirely.
- [x] A23.5 Enforce the SIZE/STORE contract in `Builder::append_obj` (2026-08-14).
      `TypeTraits<T>::store` now returns the absolute end offset (`generator/model/merge.py`
      for generated blocks, `generator/library.py` for the hand-written specialisations),
      and `include/FF_Builder.hpp` throws when `end != offset + data_size`, naming the
      claimed bytes, the consumed bytes and the recovery tag. Same check on the
      offset-array append path. **Keep this unconditional** — one comparison per resource
      against silent cross-resource corruption is not a cost worth optimising away.
      Note it is *detection*, not *containment*: it fires after the overrun bytes are
      written, so under the multi-worker ingestor a neighbouring claim may already have
      been issued. Containment (redzone claim + canary in a hardened build) is A23.8.
- [x] A23.6 Fix the empty-string SIZE/STORE divergence (2026-08-14).
      `src/FF_Primitives.cpp` — `SIZE_FF_STRING` no longer special-cases `""`.
      `generator/emit/store.py` — the choice branch's SIZE dropped its `!arg.empty()`
      guard, which the STORE side never had (52 regenerated sites). `SIZE_FF_CODE`,
      `FF_Compactor::archive_string` and the ingestor's URL-segment path all short-circuit
      on empty *before* delegating, so they are unaffected. Verified: both repro fixtures
      `rc=0`, controls unchanged, `ctest` 31/32 (`py_roundtrip` red per A23.4).
- [x] A23.7 Surface the worker fault cause (2026-08-14). `src/FF_Ingestor.cpp` —
      `fatal_log_lines()` lifts the `[Fatal]` lines out of `ConcurrentLogger` into the
      returned `FF_Result`. Workers cannot propagate an exception across the thread
      boundary, so they log it and raise `m_engine_faulted`; nothing drained that buffer,
      so A23.5's precise message reached every tool as "Ingestion aborted due to worker
      thread crash. Check ingestor engine logs for error details." A fail-loud check that
      fails into an unread buffer is a fail-silent check with extra steps.
- [ ] A23.8 *(optional, decide before hardening further)* Contain rather than detect:
      have `append_obj` claim `data_size + REDZONE`, canary the redzone, and verify it
      after the store, so an overrun lands in dead space instead of the next resource.
      Debug/ASan presets only — it changes sealed-stream byte offsets, so it must never be
      on for a build that produces archives.
- [ ] A23.4 Verify every Synthea fixture round-trips: harness rc=0, valid JSON stdout,
      `diff_doms` clean per the B5 allow-list; `py_roundtrip` green with the 111 fixtures
      as the permanent corpus. **Blocked on A24, A25, A26** — with the writer no longer
      corrupting the arena, `py_roundtrip` runs to completion and reports 0/111, dominated
      by three defect classes that are not bundle-encode bugs at all (data fabrication,
      decimal loss, entry loss). Do not chase the raw diff count: `/entry` is 250 in /
      209 out on the first fixture, and that single misalignment cascades into most of the
      ~1.9M reported diffs.
- [ ] A23.9 Add the adversarial fixtures the Synthea corpus cannot provide: empty string in
      a `string[]` element, empty `dateTime`, a resource carrying only an `id`, and a
      `Quantity` with no `comparator`. The corpus has **zero** empty strings and populates
      nearly every code field, which is precisely why A23.6, A24 and A25 all survived it.
- Acceptance: `py_roundtrip` passes on all 111 fixtures from a clean build.
- Verify: `ctest --test-dir build -R py_roundtrip --output-on-failure`, then
  `for f in build/synthea_fhir_r4/*.json; do ./build/ff_roundtrip "$f" >/dev/null || echo "FAIL $f"; done`.

---

### A24. Every absent `code` enum is written as a real clinical value — silent data fabrication

**Severity: highest open defect in the repo.** This is a *write-path* bug: the fabricated
code goes on the wire, so a downstream FHIR server has no way to tell it from data the
source system actually asserted. Unlike A23's corruption, which announced itself with a
segfault, this produces schema-valid, plausible, silently wrong FHIR.

**Mechanism.** Generated code enums have no unset state, and enum value 0 is always a real
code. The POD member defaults to it, and nothing in `*_from_json` overwrites the default
when the field is absent from the input, so `STORE` encodes a genuine dictionary code:

```cpp
// generated_src/FF_CodeSystems.hpp
enum class FF_AdministrativeGender : uint8_t { Female, Male, Other, Unknown };
//                                             ^^^^^^ value 0
// generated_src/FF_Patient.hpp:33
FF_AdministrativeGender gender = static_cast<FF_AdministrativeGender>(0);
```

**Repro** — a Patient with nothing but an `id` acquires a gender:

```bash
echo '{"resourceType":"Bundle","type":"transaction","entry":[{"resource":
{"resourceType":"Patient","id":"only-an-id"}}]}' > /tmp/bare.json
./build/ff_roundtrip /tmp/bare.json
# {"resourceType":"Patient","id":"only-an-id","gender":"female"}
```

**Blast radius: 63 POD members across 72 enums.** The specific zero-values are bad ones:

| Enum | Value 0 | Consequence |
|---|---|---|
| `FF_QuantityComparator` | `<` | **every** unqualified measurement becomes a bounded one — 83 fabrications in one Synthea bundle |
| `FF_AllergyIntoleranceCriticality` | `High` | an allergy with no recorded criticality reads as life-threatening |
| `FF_HTTPVerb` | `DELETE` | a Bundle entry with no request method reads as a delete |
| `FF_MedicationDispenseStatusCodes` | `Cancelled` | dispensed medication reads as cancelled |
| `FF_EncounterStatus` | `Arrived` | — |
| `FF_AdministrativeGender` | `Female` | — |

> **DONE 2026-08-14.** Implemented as a pinned sentinel rather than a reordering, so no
> existing enumerator moved. `UNSET_ENUMERATOR = "FF_UNSET"` / `UNSET_ENUM_VALUE = 255` live
> in `generator/model/type_map.py` because both `emit/codesystems.py` (declares the enums)
> and `model/merge.py` (defaults the POD members) need the same spelling, and merge.py must
> never import from `emit/`. The store path needed **no** change: `serialize_*()` has no
> case for the sentinel, so it falls through to `default: return ""`, and `ENCODE_FF_CODE`
> already maps `""` to `FF_CODE_NULL` while `SIZE_FF_CODE` already sizes it as 0 — SIZE and
> STORE stay in agreement (A23.6). `parse_*()`'s fallback was changed from
> `static_cast<enum>(0)` to the sentinel, which was the read-side half of the same bug: an
> unrecognised code silently became a real one. Verified: 72/72 enums carry the sentinel,
> 63/63 POD members default to it, **0** `static_cast<FF_*>(0)` defaults remain; `ctest`
> 31/32 (`py_roundtrip` per A23.4); zero new ruff violations (136 before, 136 after).
>
> **Corpus effect, and a second finding.** Across the 111 fixtures: `EXTRA_KEY` **479,784 →
> 463,050 (-16,734)** and `TYPE_MISMATCH` **7,425 → 7,023 (-402)**. `comparator`
> fabrications in one bundle: **83 → 0**. But `MISSING_KEY` rose by **725**, concentrated in
> three paths — `identifier.use` (+319), `priority` (+402), `reaction.severity` (+4). That is
> **unmasking, not regression**: `FF_IdentifierUse` value 0 is `Official`, which is exactly
> what Synthea writes, so the fabricated default was *coincidentally correct* and had been
> hiding the fact that the pipeline never stores the field at all. The `priority` count
> moved out of `TYPE_MISMATCH` and into `MISSING_KEY` — same defect, new symptom. Fabricated
> defaults were inflating the apparent pass rate wherever they happened to guess right.

- [x] A24.1 Emit an explicit unset sentinel for every generated code enum
      (`generator/emit/codesystems.py`), defaulting the POD member to it
      (`generator/model/merge.py`). No existing enumerator moved: the sentinel is **pinned
      at 255**, not appended, so adding a code to a ValueSet cannot shift it. Nothing on the
      wire depends on the ordinal — the wire carries the *dictionary code* from
      `ENCODE_FF_CODE` — so this is a source-level change only. `codesystems.py` now raises
      if a ValueSet reaches 255 codes rather than silently colliding (largest today:
      `FF_FHIRTypes` at 231).
- [x] A24.2 *(no change required — verified, not edited.)* `generator/emit/store.py` already
      does the right thing for the sentinel: `serialize_*()` has no case for it and returns
      `""`, `ENCODE_FF_CODE("")` returns `FF_CODE_NULL` having consumed 0 bytes, and
      `SIZE_FF_CODE("")` returns 0. SIZE and STORE therefore stay in lockstep (A23.6) with
      no new branch. The read-side half *did* need a change: `parse_*()`'s fallback returned
      `static_cast<enum>(0)` for an unrecognised code, silently promoting it to a real one.
- [x] A24.3 Array form (`code[]`) verified to agree — a sentinel element serialises to `""`
      and takes the same `FF_CODE_NULL` path.
- [x] A24.4 Reader verified: `print_json` already omits the key for `FF_CODE_NULL`. No edit
      needed — the bare-Patient repro now emits exactly its input.
- [x] A24.6 Make the enum's underlying type adaptive (2026-08-14, found *by* A24.1's guard).
      `enum_underlying_type()` in `generator/model/type_map.py` picks `uint8_t` below 255
      codes and `uint16_t` above; `codesystems.py` uses it for both the declaration and the
      sentinel value. This was a **pre-existing latent corruption**, not sentinel fallout:
      `FF_SPDXLicense` carries **346** codes, so under `profile=all` a `uint8_t` enum would
      wrap values 256..345 onto 0..89 and silently alias distinct licences. The A24.1 guard
      turned it into a build-time `ValueError` naming the enum and the count. No effect on
      the default profile — all 72 enums remain `uint8_t`, `ctest` 31/32.
- [ ] A24.5 Chase what the fabrication was masking: `identifier.use`, `priority` and
      `reaction.severity` are not stored at all (319 / 402 / 4 sites), and only looked
      correct because enum value 0 happened to equal the value Synthea writes. Likely the
      same root cause as A26.3 (`fullUrl`/`request` never stored) — check whether these
      fields reach `*_from_json` at all before assuming a store-side gap.
- Acceptance: the bare-Patient repro round-trips to exactly its input; `comparator` and
  `use` no longer appear in `py_roundtrip` output as `EXTRA_KEY`.
- Verify: `./build/ff_roundtrip /tmp/bare.json | python3 -c "import json,sys;
  d=json.load(sys.stdin); assert d['entry'][0]['resource'] == {'resourceType':'Patient','id':'only-an-id'}, d"`

---

### A25. `Quantity.value` does not round-trip — every decimal reads back as the null sentinel

**Context.** A decimal written into a `Quantity` comes back as `1.84467e+19` — that is
`FF_NULL_OFFSET` (`0xFFFFFFFFFFFFFFFF`) reinterpreted as a double, so the value is not
being stored, not being read, or being read from the wrong slot. 74 occurrences in a
single Synthea bundle. Present on both the choice path (`valueQuantity`) and a plain
nested one (`referenceRange.low`), so it is not choice-specific.

```bash
echo '{"resourceType":"Bundle","type":"transaction","entry":[{"resource":
{"resourceType":"Observation","id":"o","status":"final","referenceRange":
[{"low":{"value":3.5,"unit":"mmol/L"}}]}}]}' > /tmp/qty.json
./build/ff_roundtrip /tmp/qty.json
# "low": {"value": 1.84467e+19, "comparator": "<", "unit": "mmol/L"}
#                  ^^^^^^^^^^^ FF_NULL_OFFSET as a double   ^^^ A24
```

- [ ] A25.1 Determine which side drops it — check `*_from_json` populates `QuantityData.value`
      (decimal is a `SCALAR_PRIMITIVE_TYPE`, so it should be the inline-scalar path, not the
      `Offset`/`unique_ptr` path that A23 Bug A had to fix), then the STORE slot arithmetic,
      then `print_json`'s `FF_FIELD_FLOAT64` read.
- [ ] A25.2 Fix at the source and regenerate. If it is an ingest-mapping gap it is the same
      class as Bug A — check whether `decimal` is in `TYPE_MAP` at all, since Bug A was
      caused by `dateTime` being absent from it (`generator/model/type_map.py`).
- [ ] A25.3 Reader guard: a `FF_FIELD_FLOAT64` slot holding the null sentinel must emit no
      key, never a number. Printing `FF_NULL_OFFSET` as `1.84467e+19` is the numeric twin of
      the `"key":,` bug (A23.3).
- Acceptance: `/tmp/qty.json` round-trips `3.5` exactly.

---

### A26. Bundle entries are silently dropped; `fullUrl` and `request` are never stored

**Context.** First Synthea fixture: `/entry` is **250 in, 209 out** — 41 resources vanish
with no error, `rc=0`. Separately, `entry.fullUrl` and `entry.request` are absent from
every output entry (103,195 sites each across the corpus), so even surviving entries lose
their identity and their transaction semantics.

This is the single largest contributor to `py_roundtrip`'s diff count, and most of it is
cascade: once entry *N* misaligns, every path below it mismatches. Fix this before reading
the histogram again.

```bash
python3 -c "
import json,subprocess
f='build/synthea_fhir_r4/Alberto639_Tromp100_aa1a2074-ad05-6d4f-0063-6188c4f25a12.json'
src=json.load(open(f)); out=json.loads(subprocess.run(['./build/ff_roundtrip',f],
  capture_output=True,text=True).stdout)
print(len(src['entry']), '->', len(out['entry']))
print(list(src['entry'][0]), '->', list(out['entry'][0]))"
# 250 -> 209
# ['fullUrl', 'resource', 'request'] -> ['resource']
```

- [x] A26.1 **Root cause found 2026-08-14 — it is neither the race nor a sizing bug.** The
      drop is deterministic (the harness already runs `concurrency=1`) and falls on whole
      *resource types*, not on individual entries: Claim 16, ExplanationOfBenefit 16,
      SupplyDelivery 6, ImagingStudy 2, MedicationAdministration 1 — **100% of each**. None
      of the five is in `US_CORE_RESOURCES` (28 entries,
      `generator/model/structure.py:resolve_production_resources`), which is the default
      `FASTFHIR_PRODUCTION_PROFILE=us` set, so the generator never emits a `_from_json` for
      them. `dispatch_resource` falls through its `else if` chain, logs a warning that named
      *neither the type nor the reason*, and returns `FF_NULL_OFFSET`;
      `patch_Bundle_entry_from_json` then simply skips `wrapper[…RESOURCE] = child` and the
      ingest returns `FF_SUCCESS`. The warning went to `ConcurrentLogger`, which nothing
      drains — the A23.7 failure mode again, one layer down.
      *(The earlier hypothesis list here — race, pre-filter count — was wrong; kept in git
      history. The tell was that the losses were exactly type-aligned.)*
- [x] A26.2 Make the loss loud (2026-08-14) — **partially: it now reports, it does not yet
      refuse.** `generator/emit/ingest_mappings.py` tags the line `[Skipped]` and names the
      type and the reason; `src/FF_Ingestor.cpp:skipped_summary()` aggregates by type and
      returns it in the `FF_Result` message on the success path; `tests/cpp/ff_roundtrip.cpp`
      prints a non-empty message to stderr. Verified — stdout still carries only the
      document, and a bundle with no out-of-profile types produces **zero** stderr bytes:
      ```
      FastFHIR: 41 bundle entries were DISCARDED — resource type not in this build's
      profile: Claim x16, ExplanationOfBenefit x16, ImagingStudy x2,
      MedicationAdministration x1, SupplyDelivery x6
      ```
- [x] A26.2b **Policy decided by Ryan, 2026-08-14: preserve unknown resources verbatim.**
      "We can't silently drop clinical data. That's a never event." The intended end state
      is a lookup — consult a module registry for a generated handler for the type, and
      **fall back to verbatim JSON when there is none** — which is the same shape as the
      existing `EXT_REF` routing in `FF_Extensions` (registered WASM codec / retained URL /
      suppression) and the Vulkan-style discovery planned in Block J. Planned in **A27**.
      Ryan also wants the default profile moved to `all`; see A27's cost note — that is a
      separate and much larger action, and it does *not* remove the never-event.
- [ ] A26.3 Store and emit `entry.fullUrl` and `entry.request`. **Root cause found:** this
      is not a store-side gap — those fields are never parsed. The bundle-entry patcher is a
      hardcoded string in `generator/emit/ingest_mappings.py` (~line 495) whose loop body is
      a single `if (key == "resource")`; `fullUrl`, `request`, `search`, `response` and
      `link` have no branch at all. Fix by delegating the non-resource fields to the
      generated `Bundle_entry_from_json` rather than by growing the hardcoded string — the
      patcher exists only because the entry array is pre-allocated and patched
      concurrently, and that is orthogonal to which fields get parsed.
- Acceptance: entry counts match for all 111 fixtures **or** the discard is reported and
  declared (A26.2b); `fullUrl`/`request` survive.
- Verify: the snippet above prints `250 -> 250`, or the run prints the DISCARDED summary
  accounting for exactly the difference.

---

### A27. Verbatim passthrough for out-of-profile resources — PLAN (unstarted)

**Decision (Ryan, 2026-08-14):** a resource FastFHIR has no generated type for must be
**preserved verbatim**, never discarded. Intended architecture: look the type up in a
module registry (does not exist yet); if no module provides it, store the original JSON
byte-for-byte. Read A26 first — it establishes that the loss is deterministic, type-aligned,
and currently only *reported* (A26.2), not prevented.

**Why passthrough comes before `FASTFHIR_PRODUCTION_PROFILE=all`, measured not assumed:**

| | `profile=all` | verbatim passthrough |
|---|---|---|
| Permanent wire constants to append | **884 recovery tags** (measured — see below) | **1** |
| Resources generated | 275 (from 28) | unchanged |
| Removes the never-event? | **No** — any type outside the compiled set still falls off the same `else` | **Yes**, for every type, forever |

The 884 is real, not an estimate: a trial run reached full emission (921 Python field
modules) and failed only at the tag gate.

```bash
FASTFHIR_PRODUCTION_PROFILE=all python -m generator --output-dir /tmp/gen_all
# RuntimeError: 884 RECOVERY_TAG(s) were emitted that include/FF_Recovery.hpp does not
# declare:  RECOVER_FF_ACCOUNT (first seen in FF_Account.hpp) ...
```

Those are permanent wire constants in a hand-maintained header — appending 884 of them is a
deliberate ledger action needing maintainer sign-off, not a side effect of flipping a
default. `all` remains worth doing (it is what makes Claim/EOB *first-class* rather than
merely preserved), but it is A27.5, after the safety net exists.

**On the US Core question (Ryan: "I think those are supposed to be in the US production??"):**
the 28-entry `US_CORE_RESOURCES` list looks defensible. Claim and ExplanationOfBenefit are
**not** US Core profiles — they belong to the CARIN Blue Button IG, which is why a Synthea
bundle (a full synthetic record including financials) exceeds US Core. SupplyDelivery,
ImagingStudy and MedicationAdministration are likewise not US Core profiles in the recent
versions. Worth confirming against the exact US Core version FastFHIR targets before
treating the list as final — that target version is not currently written down anywhere,
which is itself worth fixing.

- [ ] A27.1 Reserve **one** new recovery tag for the passthrough block (e.g.
      `RECOVER_FF_OPAQUE_RESOURCE`) in `include/FF_Recovery.hpp`. Append only — never
      renumber. This is a permanent wire constant: get sign-off, and record the decision in
      `dictionaries/README.md` alongside the other ledger rules.
- [ ] A27.2 Define the block: validation offset + recovery tag + `resourceType` string +
      raw JSON payload with its length. Keep it a *dumb byte container* — no parsing, no
      field extraction, no dictionary interaction. It must never enter the permanent code
      ledger (same rule Block J states for external code systems).
- [ ] A27.3 Route to it in `generator/emit/ingest_mappings.py`: `dispatch_resource`'s final
      `else` stops returning `FF_NULL_OFFSET` and instead stores the raw object. Note the
      ingestor parses with simdjson **on demand**, so capturing the original bytes needs the
      source span for the entry — `entry_chunks[idx]` in `FF_Ingestor.cpp` already holds
      exactly that, which is the natural seam.
- [ ] A27.4 Read path: `print_json` re-emits the stored bytes verbatim, so a bundle
      round-trips to its input. This is what finally makes A23.4 / A26 acceptance reachable
      (`250 -> 250`).
- [x] A27.5 **Composable resource groupings, not `profile=all`** (2026-08-14). Ryan:
      "let's make the IG field an array … we can have a number of accepted groupings."
      `FASTFHIR_PRODUCTION_PROFILE` now takes a comma-separated list and the generator
      compiles the **union** — real deployments compose (a payer needs US Core *and*
      claims), which the old mutually-exclusive `us|uk|all` could not express.
      `RESOURCE_GROUPINGS` in `generator/model/type_map.py` is the single place a grouping
      is defined; `us`/`uk` stay as aliases. Groupings: `us-core` (28), `uk-core` (23),
      `billing` (5 — EOB, Claim, ClaimResponse, PaymentNotice, PaymentReconciliation),
      `all` (275, absorbs everything). CMake option and README updated.
      Verified: `us-core` alone emits **byte-identical C++** to the old `us` (only
      `FF_IngestMappings.cpp` differs, by exactly the 1 line A26.2 intended); union
      deduplicates; names are case- and whitespace-insensitive; an unknown name fails
      naming the valid set. Order is first-seen rather than sorted precisely so the default
      profile's output cannot shift — the generator is deterministic and a diff between
      runs must mean a real change.
      **Why not `all`:** measured, `billing` costs **61** new recovery tags against 884 for
      `all`, and `all` does not remove the never-event — any type outside whatever is
      compiled still hits the same `else`. A27.1–A27.4 remain the actual fix.
- [x] A27.5c **Band map re-cut for the whole FHIR spec — DONE 2026-08-14.**
      Ryan: *"we need bound checks for the resource vs scalar tags to make sure we're not
      overflowing into each other by accident… NO ONE USES FFHR YET. Now is the time."*

      | band | range | slots | used | headroom |
      |---|---|---|---|---|
      | Core Primitives | `0x0000–0x00FF` | 256 | 10 | 26× |
      | Inline Scalars | `0x0100–0x01FF` | 256 | 10 | 26× |
      | Data Types | `0x0200–0x0FFF` | 3,584 | 66 | 54× |
      | Resources | `0x1000–0x1FFF` | 4,096 | 178 | 23× |
      | Sub-elements | `0x2000–0x7FFF` | 24,576 | 711 | 35× |

      "used" is the whole spec (R4 ∪ R5), not the compiled profile. Note the corrected
      counts: **178** concrete resource types and 711 BackboneElement paths, not the 275
      reported earlier — `_discover_resource_names` counts 98 profiles/constraints
      (`derivation == "constraint"`: `ActualGroup`, `CDSHooksGuidanceResponse`,
      `CQF-Questionnaire`…) as if they were resource types. That over-count is its own bug,
      filed as A29.1.

      Resources moved `0x0300 → 0x1000` (30 tags), Sub-elements `0x0400 → 0x2000` (87).
      Primitives and Inline Scalars did **not** move, so the four open-coded
      `(x & 0xFF00) == RECOVER_FF_SCALAR_BLOCK` tests stayed valid. `FF_IsResourceTag`
      became a range check — as a high-byte test it would have returned false for every
      resource above `0x03FF`, i.e. all but 29 of them. Added `FF_IsBackboneTag`.

      **Bound checks, both layers:**
      - C++ `static_assert`s in `FF_Recovery.hpp`: bands partition `0x0000–0x7FFF` with no
        gap or overlap; each block base sits in its own band; each band is ≥ what the spec
        already needs (178 / 711 / 66). These fired for real during the work — a blanket
        regex rewrote the boundary constants and left `RESOURCE_FIRST=0x1000` with
        `RESOURCE_LAST=0x0FFF`; the ordering assert caught it.
      - `generator/utilities.py:validate_recovery_bands()`, wired into `library.py` and run
        every generate: every tag inside a band, and **no duplicate values** — a C++ enum
        accepts two enumerators with the same value silently, which would make two block
        types indistinguishable on the wire. Verified against injected faults: an
        out-of-band tag and a duplicate value are both caught; the real header passes.

      Verified: `ctest` 31/32, `pytest tests/generator` 46 passed, generator clean.
      `cpp_ff_test_primitives` pinned the old values and was updated
      (`RECOVER_FF_PATIENT` `0x0314 → 0x1014`), plus a new
      `test_recovery_band_classification` covering band edges and the array-bit path.
- [x] A27.5d **`FF_Recovery.hpp` is now generated from a committed tag ledger — DONE
      2026-08-14.** Ryan's spec: *"FF_Recovery is generated when a new release of FHIR (like
      R6) drops. It checks to make sure no drift occurred that causes corruption but yes it
      stays IN THE REPO. It should keep it all."*

      `dictionaries/master_tags.json` is the ledger — the same model `master_codes.json`
      already uses for the 5,796 dictionary IDs, which recovery tags had no equivalent of.
      `generator/emit/recovery_tags.py` reconciles it against the FHIR packages and emits the
      header; `pipeline.py` runs it before anything that references a tag.

      - **Keeps it all: 166 → 978 tags**, the whole spec (R4 ∪ R5), not the compiled
        profile — 66 datatypes, 178 resources, 711 backbone paths, plus FastFHIR's own
        primitives/scalars which are hand-seeded (nothing in a StructureDefinition implies
        `FF_HEADER` or `FF_CHECKSUM`).
      - **Stays in the repo**, committed and diff-reviewed, like `dictionaries/`.
      - **Profile-independent**: byte-identical md5 under `us-core`, `us-core,billing` and
        `all`. This closes the asymmetry found in A15 — dictionaries were already
        profile-independent while tags were not. A permanent wire artifact must not depend
        on build configuration.
      - **Drift = corruption, and it is checked**: `assert_no_drift()` compares each run
        against the committed ledger. Verified against injected faults — a renumbered tag
        (`RECOVER_FF_PATIENT 0x1014 → 0x1099`) and a deleted tag are both caught; an
        *append* correctly passes. `reconcile_tag_ledger()` refuses rather than spilling
        past a band boundary, which would silently mis-classify every tag beyond it.
      - **Deterministic**: two runs produce an identical header; re-running appends 0.
      - Values are stored as hex strings keyed by enumerator, per Ryan's
        `"Coding": 202` — `RECOVER_FF_CODING` is `0x0202`, unchanged.

      All 166 pre-existing values are unchanged (verified name-by-name), so this was a pure
      addition: the wire golden went 166 → 978 tags and updated **without** `--force`,
      which is the append-only story proving itself. `ctest` 31/32, `pytest tests/generator`
      46 passed. CLAUDE.md updated — the header is no longer hand-maintained, and is listed
      under "never hand-edit generated files".
- [ ] A27.5e Profile-filtered emission is deliberately NOT implemented. Ryan offered it
      ("if that makes compilation more palatable"), but a 978-entry enum costs a compiler
      nothing, and a header whose contents varied with the profile would reintroduce exactly
      the build-configuration dependency this task removed. Revisit only if the enum ever
      becomes a measurable compile cost — and if so, filter the generated *C++ structs*,
      which is already what the profile does, not the tag registry.
- [x] A27.5f Everything above is *capacity and identity*. The tags for the other 150 resources and ~625
      backbone paths still have to exist before `all` can be selected. At ~900 hand-written
      entries this is no longer a hand-maintenance job — decide whether
      `include/FF_Recovery.hpp` stays hand-maintained (CLAUDE.md's current rule, with the
      generator only validating) or becomes generated with the *values* pinned by a
      committed ledger like `master_codes.json`. The ledger model already solved exactly
      this problem for 5,796 dictionary codes; recovery tags have no equivalent.
      > **RESOLVED — it became generated, pinned by a ledger.** Decided and shipped by
      > A27.5c/d (see the DONE note above): `dictionaries/master_tags.json` is the
      > committed tag ledger, `generator/emit/recovery_tags.py` reconciles it against the
      > packages append-only and emits `include/FF_Recovery.hpp`, and `assert_no_drift`
      > fails if an existing value moved or vanished — exactly the `master_codes.json`
      > model this item asked for. Discovery is whole-spec, so the header no longer
      > varies with the profile and the tags for every grouping already exist: 978 in the
      > ledger, including `RECOVER_FF_ACCOUNT` (0x101D), `RECOVER_FF_CLAIM` (0x1030) and
      > `RECOVER_FF_EXPLANATIONOFBENEFIT` (0x1053) — the very tags A27's error message
      > cites as missing. Ticked 2026-08-18 as a record of a decision already taken, not
      > new work.
- [ ] A27.5c-old **superseded — kept for the reasoning.** Original note: settle the band
      layout before appending ANY tag. Tags are permanent, so a band cannot be re-cut later; appending billing tags at
      `0x03xx` now and discovering the band is too small afterwards is unrecoverable.

      **`include/FF_Recovery.hpp` covers 28 of the 275 concrete R4/R5 resources — exactly
      `US_CORE_RESOURCES`, nothing more.** 165 tags total: 71 top-level (28 resources + the
      datatypes/primitives) and 94 backbone. It tracks the compiled profile, not the spec.
      Unlike `master_codes.json`, which is profile-*independent* (built from the packages,
      complete at 4,634 IDs regardless of profile), the tag header is profile-*dependent*.

      **And the documented banding cannot hold all of R4/R5:**

      | band | capacity | used | free | needed for `all` | |
      |---|---|---|---|---|---|
      | Resources `0x0300–0x03FF` | 256 | 29 | 227 | **235** | overflows |
      | Sub-elements `0x0400–0x04FF` | 256 | 86 | 170 | **649** | overflows badly |

      The tag *width* is not the problem: `RECOVER_TYPE_MASK = 0x7FFF` leaves 32,767 type
      values and all of R4/R5 needs ~1,049. It is purely the 256-slot band layout.

      **The bands are not merely documentation — three of them are load-bearing high-byte
      predicates**, so widening a band silently changes behaviour rather than failing to
      compile:
      - `FF_IsResourceTag()` — `include/FF_Utilities.hpp:64` — `(tag & 0xFF00) == 0x0300`
      - `FF_IsScalarBlockTag()` — `include/FF_Utilities.hpp:75` — `(tag & 0xFF00) == 0x0100`
      - the same scalar test open-coded at `include/FF_Primitives.hpp:403`,
        `include/FF_Ops.hpp:188`, `src/FF_Parser.cpp:538`

      Decide and document: keep 8-bit bands and accept that `all` is unreachable; or re-cut
      the map (e.g. resources `0x0300–0x0FFF`, sub-elements `0x1000–0x7FFF`) and replace the
      `& 0xFF00` predicates with range checks in the five sites above. The scalar band must
      keep its identity either way. Nothing dispatches on the sub-element band, so it is the
      cheapest to widen. Whatever is chosen, write it into the header's Convention comment —
      that comment is currently the only specification of the layout.
- [x] A27.5b Append the 61 `billing` recovery tags so `us-core,billing` can be selected.
      > **OBSOLETE (2026-08-19) — done by the ledger, not by this task.** Tag discovery is
      > profile-independent: `dictionaries/master_tags.json` covers the whole R4 ∪ R5 spec
      > (978 tags), so the billing tags already exist and no manual append is needed.
      > Verified: `RECOVER_FF_CLAIM = 0x1030`, `RECOVER_FF_EXPLANATIONOFBENEFIT = 0x1053`.
      > The original text below is kept for the reasoning only.
      > ~~Permanent wire constants — **append, never
      renumber**, and get sign-off first. Until then the grouping is defined but
      unselectable: the generator's tag gate refuses it and names all 61
      (`FASTFHIR_PRODUCTION_PROFILE=us-core,billing python -m generator` to list them).
      Note Synthea's other three dropped types (SupplyDelivery, ImagingStudy,
      MedicationAdministration) belong to no grouping — they are exactly the case A27.1–4
      exists for, which is why the passthrough is the fix and the grouping is a convenience.
- [ ] A27.7 Derive the groupings from the published IG packages instead of transcribing
      them. `US_CORE_RESOURCES`/`UK_CORE_RESOURCES` are hand-maintained lists carrying no IG
      **version**, so drift against a republished IG is undetectable, and the version
      FastFHIR targets is written down nowhere. HL7 ships these as NPM packages on
      packages.fhir.org — the same registry the generator already pulls
      `hl7.fhir.r4.core`/`hl7.fhir.r5.core` from — so `hl7.fhir.us.core` and
      `hl7.fhir.us.carin-bb` can be fetched by the existing mechanism.
- [ ] A27.6 Registry hook (the part Ryan described): before falling back to verbatim, ask a
      module registry whether a handler for this `resourceType` is available. Design it as
      the **same discovery mechanism as Block K's hooks struct and Block J's layers** — a
      missing registry means the check does not run and the verbatim path takes over. Do not
      invent a second mechanism; read K0 first.
- Acceptance: no bundle can lose a resource. `250 -> 250` on the A26 snippet with the
  discard summary empty, for every fixture.
- Verify: `for f in build/synthea_fhir_r4/*.json; do ./build/ff_roundtrip "$f" 2>&1 >/dev/null; done`
  prints nothing.

---

### A28. A standalone generator run does not reproduce `generated_src/python/fields/`

**Found 2026-08-14 while verifying A27.5's determinism claim; pre-existing — reproduced with
the code stashed, so it is not A27 fallout.** The C++ tree reproduces byte-for-byte. The
Python field modules do not: **85 of 228 files differ** between a fresh
`python -m generator --output-dir <tmp>` and what the CMake configure path leaves in
`generated_src/python/fields/`.

```bash
python -m generator --output-dir /tmp/gen && diff -rq /tmp/gen/python generated_src/python | wc -l
# 85
wc -c /tmp/gen/python/fields/bundle_entry.py generated_src/python/fields/bundle_entry.py
#   834   (ASTNode class only)
# 16511   (Field constants AND the ASTNode class)
```

`emit_python_fields` and `emit_python_ast` (`generator/bindings/python_fields.py`) write the
**same filename** in the same directory. In a standalone run the second overwrites the
first; the committed tree somehow carries both. Whatever reconciles them is not in the
generator, which means the generator alone cannot reproduce its own output — the property
`tests/generator/test_determinism.py` exists to protect. Related to `755f97a`
("Fix Python staging…"); confirm that commit did not paper over this.

- [ ] A28.1 Determine which emitter is authoritative for `python/fields/<name>.py` and give
      them distinct filenames or an explicit merge, so one run produces the final content.
- [ ] A28.2 Extend `test_determinism.py` to cover `python/`, not just the C++ tree — it
      would have caught this.
- Verify: `diff -rq /tmp/gen/python generated_src/python` prints nothing after a configure.

---

### A29. Orphaned, broken test file: `tests/test_ff_dictionary.py`

**Found 2026-08-14 while renaming the emit modules; pre-existing.** The file imports
`generate_master_dictionary`, which **does not exist** and did not exist at `HEAD` either
(`git show HEAD:generator/emit/dictionary.py | grep -c generate_master_dictionary` → 0).
The real name is `generate_master_codes`.

Nobody noticed because nothing runs it: it sits in `tests/` rather than `tests/generator/`,
so `pytest tests/generator` never collects it, and it is registered in neither
`CMakeLists.txt` nor CI. Run directly it is 2 failed / 1 passed.

```bash
python -m pytest tests/test_ff_dictionary.py -q     # 2 failed, 1 passed
```

This is the A15/A20 failure class once more — a check that is never executed is
indistinguishable from one that passes. The rename to `emit/code_ids.py` updated its import
path, so it is no *more* broken than before, but it is still dead.

- [ ] A29.1 Decide: fix it against the real API and register it with ctest/pytest, or delete
      it. Do not leave a third state. If it is fixed, it belongs in `tests/generator/` with
      the other ledger gates, where `test_code_ids.py` already covers ID stability — check
      for overlap before reviving it.
- [ ] A29.2 Add a collection guard so an unreferenced test file cannot sit unrun again:
      either fold `tests/*.py` into the pytest paths or assert in CI that every `test_*.py`
      is reachable from some harness.
- Verify: `python -m pytest tests/test_ff_dictionary.py -q` exits 0, or the file is gone.

---

### A15. Re-arm the two vacuous sections of the wire gate — FIXED 2026-08-14

> **DONE. A15.1/A15.2 below had already named both causes correctly** — this note records
> the fix and the measured result, not a re-diagnosis. The two sections were empty because
> **`witness()` scanned only `generated_src/`, and neither constant family lives there:**
> recovery tags are *defined* in `include/FF_Recovery.hpp` (165 of them — generated_src only
> ever *references* them, without a `= value`, so the regex could not match), and the
> dictionary IDs live in `dictionaries/FF_Codes.hpp` (5,796), which is a committed tree
> outside `generated_src/` entirely. The `codes` regex was additionally written against
> `FF_R5_CODE_PERCENT = 0x1CF1F3BB` — a flat, hash-based, revision-prefixed naming scheme
> that no longer exists; names are now scoped by terminology source then CodeSystem
> (`UCUM::PERCENT`, `FHIR::FDI_SURFACE::B`) with sequential ledger IDs. So the section was
> doubly dead and would have stayed empty even pointed at the right file.
>
> **Consequence, stated plainly:** the 5,796 dictionary IDs that decode every `.ffhr`
> archive ever written had *zero* regression protection — including at the moment `118d6ad`
> renumbered them and silently invalidated every stored archive. The gate that exists to
> catch exactly that was comparing `{}` to `{}`.
>
> Fixed in `tests/generator/wire_witness.py`: `witness()` now reads all three trees and
> `_dictionary_codes()` parses the namespaced form (brace-depth tracked, since scopes are a
> mix of `namespace` and `struct`) — 5,796 parsed, 0 name collisions. Golden regenerated:
> `{'codes': 0, 'tags': 0, 'vtables': 141}` → **`{'codes': 5796, 'tags': 166, 'vtables': 141}`**.
> `test_recovery_tags_stable` / `test_dictionary_codes_stable` switched from
> `_symmetric_diff` to `_check_permanence` — with the sections populated, equality would
> have rejected a legal append (a new HL7 code, or A27.5b's 61 billing tags); permanence
> accepts additions and rejects mutation and deletion, matching the ledger's own `_rule`.
> `_symmetric_diff` is now unused and removed. Three tests added: one asserting the sections
> are non-empty so they can never silently go vacuous again, one proving a renumber is
> rejected, one proving an append passes. `pytest tests/generator` 43 → **46 passed**.
>
> **The golden diff in this commit is a baseline being established, not a wire change** —
> no constant moved. CLAUDE.md's "a golden update without a corresponding change is a red
> flag" is exactly right and this is the documented exception.

> **Why this is not cosmetic (2026-08-14).** A23, A24, A25 and A26 are all caught by
> `py_roundtrip` on its first fixture. It is the only gate in the repo that compares an
> input document to its output; everything else compares generated structure against
> itself, which no amount of corruption or fabrication can disturb — a SIZE/STORE
> disagreement regenerates byte-identically forever, so the witness stays green.
> The two failure modes below are the same failure mode: **a check that passes when it
> measures nothing.** `wire_witness.json` carries `{'codes': 0, 'tags': 0, 'vtables': 141}`,
> and `tests/python/test_roundtrip.py` returns `0` when `discover_fixtures` finds nothing.
> Both should be errors unless explicitly opted out.

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

- [x] A15.1 Extend `witness()` to read tag values from `include/FF_Recovery.hpp` and code
  values from `dictionaries/FF_Codes.hpp`, in addition to the generated tree. Both are
  committed, so the witness stops depending on a network regeneration for those sections.
  Signature change: pass the repo root (or both explicit paths) alongside `generated_dir`;
  update the two call sites (`test_wire_format.py`, the `__main__` block) and the module
  docstring, which currently describes only the generated tree.
- [x] A15.2 Replace the `_CODE` regex with one matching the real emission —
  `FF_CODE_DEF <NAME> = <int>;` qualified by its enclosing `namespace` so
  `FF_CODE::UCUM::MMHG` and `FF_CODE::FHIR::…::MALE` are distinct keys. Values are decimal,
  not hex. Fix the docstring example in the same edit.
- [x] A15.3 Add `test_witness_sections_are_non_empty` asserting every section of a freshly
  built witness has entries, with a message naming the regex that stopped matching. This is
  the check whose absence let A4.1 be marked done against an empty golden; without it, any
  future emitter rename silently re-empties a section.
- [x] A15.4 Regenerate the golden and commit it with this change (per `CLAUDE.md`, a golden
  update needs a corresponding generator or test change in the same commit — A15.1/A15.2
  are that change). The diff must be **additions only**: `vtables` unchanged, `tags` and
  `codes` populated from nothing.
- [x] A15.5 DONE 2026-08-14 — `--force` implemented, and needed immediately: the
      A27.5c band re-cut is a legitimate pre-release wire change and the tool had no way to
      record one. Without it, refuses and lists every violation; with it, prints
      `!! --force: OVERRIDING 115 permanence violation(s) !!` plus each one and a warning
      that prior archives are now undecodable. Original note: the permanence error tells the user
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
- [ ] B4.4 **The download runs on every configure.** The guard is
  `if(FASTFHIR_DOWNLOAD_SYNTHEA AND NOT EXISTS "${_SYNTHEA_DIR}/fhir")`, but the zip extracts
  111 `.json` files directly into `${_SYNTHEA_DIR}` — there is no `fhir/` subdirectory, so
  the condition is always true and the archive is re-fetched and re-extracted every time
  (measured 2026-08-12). Fix the guard to test something the extraction actually produces
  (a stamp file written after `ARCHIVE_EXTRACT`, which also survives an upstream layout
  change). Do together with B4.2 — the `EXPECTED_HASH` and the guard are one edit.
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
bytes produced by a shipped encoder, read back through the current reader. (IFE C6 — see the
IFE audit (2026-08-12, in git history) — records this as IFE's own standing gap, so it is a shared one; A4.3's
compile smoke test is the nearest existing relative and is complementary, not a substitute.)

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
  Builder static — decide and document) in a new `src/FF_Recovery.cpp` + declaration in a
  **new hand-written header**.
  > **STALE (2026-08-19): this step's original plan is no longer possible.** It said to add
  > an API section to `include/FF_Recovery.hpp` below the wire constants. That file is now
  > GENERATED from `dictionaries/master_tags.json` into `generated_src/` and is overwritten
  > at every configure — a hand-written declaration in it would be silently destroyed.
  > Put the recovery API in its own header (e.g. `include/FF_Archive.hpp`); do **not**
  > extend the generated tag header. The name collision between `src/FF_Recovery.cpp` and
  > the generated `FF_Recovery.hpp` is also worth avoiding while naming this.
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
  - [ ] E5.4 `CLAUDE.md`'s repo map says "`fastfhir/fields.py` is generated at build time".
        No such file is produced. `generator/bindings/python_fields.py:14` writes one module
        per resource to `generated_src/python/fields/<resource>.py` plus `.pyi` stubs — 285
        files, none of them named `fields.py` and none under `python/`. Correct the line
        after A21 settles where the package is assembled from, so the map describes the
        arrangement that ends up shipping.
- [ ] E6. **Dictionary unification — final sweep:** the unification is essentially done
  (`FF_UCUM_Concepts.cpp` deleted; `master_codes.json` is source of truth;
  `generator/emit/code_ids.py` evolved into the master-codes producer and stays).
  Remaining checks:
  - [ ] E6.1 `grep -rn 'FF_UCUM_STRINGS\|kUCUMTable\|FF_UCUM_CODES' src/ include/ dictionaries/ generator/`
        — delete any dead remnants found (expect: possibly none).
  - [ ] E6.2 Verify the generated dictionaries carry the planned `static_assert` guards:
        string-table size consistency in `dictionaries/FF_Dictionary_Strings.cpp`, and
        last-entry-code < string-table-size in `FF_R4_Dictionary.cpp` /
        `FF_R5_Dictionary.cpp`. If absent, add them to the emitters
        (`generator/emit/code_ids.py`), regenerate, commit both.
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
- [ ] E9. **State and guard the recovery-tag family ceilings**
  > **LARGELY STALE (2026-08-19) — re-scope before starting.** This item describes the
  > pre-re-cut layout (resources `0x0300`–, sub-elements `0x0400`–) and occupancy
  > (11, 11, 29, 29, 86). Both are gone. The 2026-08-14 band re-cut moved resources to
  > `0x1000` and backbones to `0x2000`, and current occupancy is 10, 12, 61, 179, 716.
  > What E9 asked for now largely exists: the bands are declared as
  > `RECOVER_BAND_*_FIRST/LAST` constants, the emitted header carries a BAND MAP with live
  > counts, `static_assert`s catch a boundary edit that overlaps or under-sizes a band, and
  > `generator/utilities.py:validate_recovery_bands()` checks every tag on every run. What
  > may remain is documenting the *cost* of crossing a ceiling. Re-verify before claiming.
  > Original text: `include/FF_Recovery.hpp:19-25`
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
- [ ] E13. **The lint gate is red: 315 ruff violations** (black is clean) —
  first measured 2026-08-12 as 307 violations + 2 files black would reformat, re-measured
  2026-08-18 with the *same* tooling (ruff 0.15.1 / black 26.5.1), stable across repeated
  runs and identical whether the paths are passed as directories or as an explicit file
  list. So the `CLAUDE.md` command `ruff check generator tests/generator && black --check
  generator tests/generator` still fails today — on the ruff half only — and E1's CI recipe
  would fail with it on day one. Breakdown:
  `E501` 150, `F541` 56, `ANN001/201/202` 70, `F401` 21, `I001` 10, `B007` 4, `UP015`/`F841` 4.
  **No `F821`** — nothing here indicates a live bug, so this is style debt against the
  project's own declared standard (`CLAUDE.md` invariant 3), not defect triage.
  - [ ] E13.1 `ruff check --fix` clears 89 automatically. Do those as one commit and the
        remaining ~226 as a second, so review stays tractable. Generated output must not
        move: re-run `pytest tests/generator -q` and confirm the wire witness is unchanged.
  - [ ] E13.2 Land this **before** E1, or E1 ships with a step that is red from the first
        commit and gets ignored or `continue-on-error`'d — which is how a gate dies.
  - Verify: `ruff check generator tests/generator && black --check generator tests/generator`
    exits 0.
  > **Unreproduced observation, recorded and NOT actioned.** The first run of this command in
  > the Phase 0 session printed `warning: No Python files found under the given path(s)` and
  > `All checks passed!` (black: `No Python files are present to be formatted`), i.e. the gate
  > appeared to pass while examining nothing. It has not reproduced since — not with a cold
  > `.ruff_cache`, not across repeated runs, not before or after a build. No explanation was
  > found, and the config was not modified in between. It is written down only so that if
  > anyone sees it again there is a prior sighting to match against; do **not** rewrite the
  > `pyproject.toml` `include` patterns on the strength of it. If it does recur, capture the
  > full output and `ruff check --show-files` at that moment — that is the missing evidence.
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
  - **Amendment (2026-08-18): generate the document, do not hand-write it.**
    Added after reviewing the Iris File Extension's spec pipeline
    (`../Iris-File-Extension/spec/`), which renders a normative PDF and HTML
    from a machine-readable source with zero hand-written layout tables. Most
    of what I1 lists is already machine-readable here — `master_tags.json`,
    `master_codes.json`, and the vtable enums — so a hand-written SPEC.md would
    *transcribe* those numbers, and a transcribed number is one that can
    disagree with the format. I1.6's own warning ("an unstated ceiling gets
    crossed by someone adding just one more") applies equally to a stated
    number that quietly goes stale.
    Three scoping decisions govern I1.7–I1.12, and they are not negotiable
    without re-opening this amendment:
    1. **Scope is the container format, never the FHIR payload.** HL7 owns the
       resource schemas (see Q11's answer); FastFHIR owns `FF_HEADER`,
       `DATA_BLOCK`, vtable rules, array kinds, `FF_STRING`, choice slots, code
       encoding, compaction, the checksum footer, tags and ceilings. Do not
       emit tables for FHIR resources — that is regenerating HL7's
       specification per version, and it is unbounded.
    2. **C++ stays the source of truth.** IFE's JSON *drives* its C++, which is
       why its whole document generates from the schema. Here
       `FF_Primitives.hpp` is hand-maintained with permanent values (CLAUDE.md),
       and inverting that is a refactor of frozen wire constants for no gain.
       The document is generated *from* the C++ and the committed ledgers, not
       the other way round.
    3. **Offsets come from the compiler, never from Python.** See I1.7 — this
       is the decision the rest depends on.
  - [ ] I1.7 **Emit resolved offsets from C++.** The vtable offsets are
        symbolic sums (`RECOVERY = MAGIC + MAGIC_S`) and the literal byte
        ranges exist only in hand-maintained comments
        (`// 2 bytes (6-7)`). Anything that recomputes those sums in Python is a
        *second* derivation that can disagree with the compiler, silently,
        forever. Instead add `tools/wire_offsets/FF_WireOffsets.cpp`: a tiny
        program that includes `FF_Primitives.hpp` and prints every block's
        `type`, `recovery`, and each field's name / size / **resolved offset**
        as JSON on stdout. The compiler computes; the tool reports.
        *Locate:*
        ```bash
        cd /Users/ryanlandvater/GitHub/FastFHIR
        sed -n '/enum vtable_offsets/,/};/p' include/FF_Primitives.hpp | head -14
        ```
        *Expect:* offsets written as sums, byte ranges only in `//` comments.
        *Done when:* the program builds under both presets and its JSON gives
        `FF_HEADER.FHIR_REV.offset == 6`, matching the comment on that line —
        and a deliberate reorder of two `vtable_sizes` entries changes the JSON
        without anyone editing a comment.
  - [ ] I1.8 **Feed the witness with it.** `tests/generator/wire_witness.py`
        states in its own docstring that it captures field order and size
        constants rather than offsets, because it cannot resolve the sums.
        I1.7 removes that limit: have the witness consume I1.7's JSON so the
        append-only gate compares **real offsets**. Do this before I1.9 — the
        document should render the same numbers the gate enforces, from one
        source, or the two can drift and the drift is invisible.
        *Done when:* `tests/generator/test_wire_format.py` fails when a field's
        offset moves while its order and width are unchanged — a case the
        current witness cannot see. Red-green it.
  - [ ] I1.9 **Convert the narrative to AsciiDoc with generated includes.**
        `architecture.md` §4–§6 is the spec basis and stays hand-written; it
        becomes `docs/spec/ff_spec.adoc`, and every layout table, tag table and
        code table becomes an Asciidoctor `include::` of a file generated from
        I1.7's JSON and the two ledgers. Use AsciiDoc's native `include::` —
        **no preprocessor, no `{{...}}` marker syntax of our own**; that is the
        mechanism Khronos uses for Vulkan and IFE adopted after rejecting a
        homegrown templating layer. Emit **one file per table**, not one per
        section, so moving a section never drags unrelated tables with it.
        Convert the prose as a format change only — rewriting normative text is
        a content change and belongs in its own commit.
        *Done when:* `docs/spec/` contains no hand-written offset, tag value or
        code ID, and `grep -rn "0x55\|offset [0-9]" docs/spec/ff_spec.adoc`
        returns only prose references, never a table.
  - [ ] I1.10 **One command, both outputs.** Add `docs/spec/build_document.sh`
        rendering HTML and PDF from the one source (`asciidoctor` and
        `asciidoctor-pdf`). Two traps IFE has already paid for, both of which
        this script must handle:
        - **Asciidoctor exits 0 on a missing include**, writing "Unresolved
          directive" into the output instead. The exit code is therefore not
          the gate — the script must grep the rendered output for it and fail.
        - **An orphaned generated table stays included silently.** A renamed
          block leaves its old `.adoc` on disk, the narrative keeps including
          it, and the document publishes a table with no source. Regeneration
          must delete orphans and the check must fail on them.
        Stamp provenance into the document: the engine version and the tool
        version that produced the build. A ratified document that cannot be
        reproduced later is not reproducible in any useful sense.
        *Done when:* one invocation produces both files, the output contains
        zero occurrences of "Unresolved directive", and pointing an include at
        a renamed block fails the script. Red-green both.
  - [ ] I1.11 **CI job.** Render the document on every push so a wire change
        that breaks it fails before publication, and upload the PDF as an
        artifact. Depends on XP-5 (this repository currently has no workflows
        at all). Watermark the PDF as draft until Q13 answers the freeze
        wording.
        *Done when:* the job renders both outputs and fails when I1.10's
        unresolved-include check trips.
  - ⚠ **I1.12 Toolchain dependency — decide before starting I1.9.**
        The pipeline needs Ruby ≥ 3.2 with `asciidoctor` and `asciidoctor-pdf`.
        IFE evaluated pandoc and rejected it on a measured fact: pandoc has no
        AsciiDoc *reader* — `asciidoc` appears only among its output formats —
        so adopting it means reverting the source to Markdown and writing the
        preprocessor this design exists to avoid, plus a LaTeX engine.
        `asciidoctor -b docbook | pandoc` works and is the documented fallback
        for anyone on an older Ruby. Confirm the dependency is acceptable for
        contributors and CI, or say which fallback ships. Produce the analysis
        and STOP.
  - **Note on ordering.** I1.7 → I1.8 → I1.9 → I1.10 → I1.11 is a hard
    sequence: each consumes the previous one's output. I1.12 gates I1.9. None
    of them unblocks Q13 — a specification that cannot state its own stability
    guarantee is not finished no matter how it is produced — so I1 stays
    `Blocked on Q13` for its *content* while I1.7–I1.8 can proceed now, since
    both are wire-gate work that stands on its own.
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
boundary `dictionaries/master_codes.json` already enforces via `_assert_redistributable`.

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
2. **Nothing here touches the permanent ledger.** `dictionaries/master_codes.json` and
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
- [ ] J3.3 Reuse `emit/code_names.py`'s `assign_identifier` ladder and `RESERVED_MACROS`
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
         (`Ms` megasecond vs `ms` millisecond); `emit/code_names.py:146` already records
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
      leave `dictionaries/master_codes.json` byte-identical. This is the guard that stops a
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

## Block K — Conformance validation layer (attachable, generated, not linked by default)

### K0. What this is, and how it differs from Block J

**Planned, unstarted.** Modelled on the Iris File Extension's validation layer
(`Iris-File-Extension` @ `3cd0fa0`, `generated_source/IFE_Validation.{hpp,cpp}` +
`examples/validation_layer.cpp`), which is the same architecture applied to a smaller
format and is worth reading in full before writing any of this.

**The split this rests on**, and the thing to get right before writing code:

1. **Structural** validation is inline and mandatory. A block sits at its own offset,
   carries the right `RECOVERY_TAG`, and fits inside the arena. FastFHIR already does this
   (`Builder::_amend_prepare` bounds checks, `FF_HEADER` magic, recovery tags) and it must
   stay unconditional. Wrong here means *unreadable bytes*.
2. **Conformance** validation is optional and attachable. `Patient.name` cardinality,
   required-field presence, code membership in a bound ValueSet, FHIR invariants like
   `pat-1`. These say nothing about whether the bytes parse. Wrong here means *a valid
   file that a FHIR server will reject*.

Conformance policy is a development-time aid, not a production dependency — exactly the
Vulkan framing. A shipped product links the layer only if it wants it; a detached
`append_obj` costs **one null check**.

**How this differs from Block J.** J1 specifies runtime-discovered dylibs with a stable C
ABI, manifests and a `FASTFHIR_LAYER_PATH` — the full Vulkan loader mechanism, because
terminology data is separately licensed and cannot ship in-tree. Block K is the *simpler,
more C++* half: an ordinary struct of function pointers, generated from the
StructureDefinitions, in a separately-linked static library. **No loader, no trampolines,
no manifest discovery** — IFE deliberately borrowed the shape and not the machinery.

The two must share one interface. If K's hooks struct is the boundary, then J1.2's
"stable C ABI" *is* that struct, and J's discovery becomes an optional way to populate it
rather than a parallel mechanism. **Do K first**; then re-read J1 and delete whatever K
made redundant. Do not build two hook systems.

**Never:** conformance failure must never corrupt or block a write path that structural
validation accepted, and must never enter the wire format. A stream written with the layer
attached and one written without it must be **byte-identical** — the layer observes, it
does not encode. K5.4 tests exactly this.

### K1. The types: `Conformance::Status` and `ValidationHooks`

**Context:** IFE's `Status` (`generated_source/IFE_Blocks.hpp:69`) is a deliberate POD —
`Check code; const char* block; const char* field; uint64_t found, expected; Offset at;`
with `explicit operator bool()`. No allocation, `noexcept` throughout, formatting left to
the caller. FastFHIR's existing `FF_Result` (`include/FF_Primitives.hpp:221`) carries a
`std::string` **by value**, so constructing one allocates. That is fine at the ingest API
boundary where it is used today; it is wrong for a check that may run per field.

- [ ] K1.1 New `include/FF_Conformance.hpp`: `enum class Check : uint8_t` (`OK`,
  `CARDINALITY`, `REQUIRED_MISSING`, `FIXED_VALUE`, `CODE_NOT_IN_VALUESET`,
  `INVARIANT`, …) and a POD `Status` mirroring IFE's, plus the FHIR-specific citation
  fields — the invariant key (`"pat-1"`) and the resource URL — as `const char*` pointing
  at generated static storage. `noexcept`, no allocation, trivially copyable; assert both
  with `static_assert(std::is_trivially_copyable_v<Status>)`.
- [ ] K1.2 `struct ValidationHooks` with `const ValidationHooks* next` (layers chain) and
  `std::string* diagnostic` (caller-owned sink; the layer assigns, the caller clears
  before a write and checks non-empty after). IFE moved from a
  `void(*)(const char*, void*)` callback + `void* user` to the plain `std::string*` in
  `3cd0fa0` and the diff is worth reading — it removed a type-erased pointer pair from a
  public struct for no loss of capability. Do not reintroduce the callback.
- [ ] K1.3 **Dispatch — decide before writing the emitter, `Blocked on Q17`.** IFE names
  one function pointer per block (18 members). FastFHIR has 141 blocks, so a named member
  per block is a 141-line struct that changes every time a resource is added. The
  alternative is a table indexed by `RECOVERY_TAG` with a type-erased signature, cast back
  through `TypeTraits<T_Data>` — type erasure confined to one call site, and provably
  correct because the tag comes from `TypeTraits<T_Data>::recovery`
  (`generated_src/FF_Patient.hpp:104`). Recommendation: the tag-indexed table, because it
  scales with the resource count and because `append_obj` is already templated on
  `T_Data`. Q17 records the decision.
- Acceptance: header compiles standalone (`c++ -std=c++20 -fsyntax-only`); `Status` is
  trivially copyable; no `std::string` by value anywhere in it.

### K2. Attachment point: the Builder, at the API boundary

**Context:** IFE threads `const ValidationHooks*` through every generated
`store(base, offset, info, hooks)`. FastFHIR should **not** copy that. Its write path is
`Builder::append_obj(data)` (`include/FF_Builder.hpp:594`) →
`TypeTraits<T>::store` → `STORE_FF_PATIENT` (`generated_src/FF_Patient.cpp:1205`), and the
generated `STORE_*` functions are the hot path. Checking in `append_obj`, before `store`,
gets the same coverage with:

- no signature change to any generated `STORE_*` (smaller diff, no hot-path cost, and the
  wire witness cannot move);
- the check running against the typed `*Data` struct, which is where the FHIR-level rules
  are actually expressible — IFE's own comment says the layer "validates at the API
  boundary, as a Vulkan layer does, never against generated internals";
- one attachment for the whole stream instead of a parameter at every call.

- [ ] K2.1 `Builder::attach_layer(const ValidationHooks* hooks) noexcept` — a single
  member, null by default. Document that the pointer is borrowed, must outlive the
  Builder, and that the struct is copied by the caller before `diagnostic`/`next` are set
  (IFE returns `const ValidationHooks&` from `conformance_layer()` and tells the caller to
  copy; mirror that).
- [ ] K2.2 In `append_obj`, before `TypeTraits<T_Data>::store`: if a layer is attached and
  has a check for `TypeTraits<T_Data>::recovery`, run it. Detached, this is one null test.
  Measure it: a benchmark run with and without the layer attached must show no difference
  outside noise on the detached path (flag for the benchmark repo per execution-contract
  rule 9).
- [ ] K2.3 **Failure policy, two modes.** `CLAUDE.md` invariant 5 says the write path
  throws. J5 (answered) says terminology failures are a *loud logged warning, never a
  drop*. Both are right for their case, so make it explicit:
  `LayerPolicy::Throw` (default — `std::runtime_error` prefixed `"FastFHIR: "`, message
  formatted from `Status` + citation) and `LayerPolicy::Report` (fill the sink, return,
  continue). Conformance defaults to Throw; a J terminology layer sets Report. Convert
  `Status` → exception **only** at this boundary, so the layer itself stays `noexcept`.
- [ ] K2.4 Decide and document whether the read path gets hooks too. Recommendation: **no,
  not in K.** The read path returns falsy Nodes rather than throwing (invariant 5) and is
  the zero-copy hot path; a reader-side conformance check is a separate tool (closer to a
  linter over a finished archive) and should not be smuggled into `Node`.

### K3. Generator: emit checks from what the StructureDefinitions already carry

**Context:** the conformance data is already in the packages and currently unused. The
generator reads `el.get("max")` for array-ness (`generator/model/merge.py:76`) and
`el.get("binding")` for code systems (`generator/emit/codesystems.py:159`), and reads
**neither `min` nor `constraint`**. FHIR StructureDefinition elements carry `min`/`max`
(cardinality), `constraint[]` (each with `key` like `pat-1`, `severity`, `human`,
`expression`), `binding.strength` + `valueSet`, and `fixed[x]`/`pattern[x]`. That is the
FastFHIR analogue of IFE's hand-authored `conformance: {level, clause, requirement}`
blocks — with the advantage that it is normative and upstream, and the disadvantage that
`expression` is FHIRPath, a full language.

**Cap the expressiveness in writing, per IFE B6 (see the IFE audit of 2026-08-12, in git history).** The generator
emits checks for a **closed allow-list** of shapes and nothing else:

- [ ] K3.1 Allow-list, in this order of value: (a) `min >= 1` → required-field presence;
  (b) `max` bounds on arrays; (c) `fixed[x]` / `pattern[x]` exact-value; (d)
  `binding.strength == "required"` → membership in the bound ValueSet, which reuses the
  dictionary and is the natural bridge to Block J.
- [ ] K3.2 **Everything else is recorded, not skipped.** Any `constraint[]` whose
  `expression` the emitter does not implement must be emitted as a listed, queryable
  "unimplemented" entry naming its `key` and `human` text — IFE A5: a blank must not be
  ambiguous between "checked and passed", "deliberately not checked", and "forgotten". A conformance report that cannot say what it did not check is worth
  much less than one that can.
- [ ] K3.3 No FHIRPath evaluator. If one is ever wanted it is its own project with its own
  task; writing a partial one inside the emitter is how the schema becomes a programming
  language. Say so in the emitter's module docstring.
- [ ] K3.4 Every diagnostic cites its source: the invariant `key` and `human` text
  verbatim where there is one, else the element path and the rule that produced the check,
  plus the resource's canonical URL. IFE's diagnostics end "Per the IFE specification,
  clause ife-layer-extents"; the FastFHIR equivalent cites FHIR, not FastFHIR — **we do
  not invent normative requirements**, we enforce HL7's. (Once I1's SPEC.md exists it may
  add FastFHIR-specific clauses; those are separate and must be marked as such.)
- [ ] K3.5 New emitter `generator/emit/conformance.py` producing
  `generated_src/FF_Conformance_Layer.{hpp,cpp}` and a `conformance_layer()` returning a
  shared immutable `const ValidationHooks&`. Follows the `emit/` boundary rule: no
  arithmetic on byte offsets, text only.
- Acceptance: the emitted layer compiles; the wire witness is **unchanged** (this emits no
  wire constants — if the witness moves, something is wrong); `pytest tests/generator` green.

### K4. Build: a separate library nobody links by accident

- [ ] K4.1 `fastfhir_conformance` static library, `FASTFHIR_BUILD_CONFORMANCE` default
  **OFF**, not in `_BUILD_ALL` unless enabled. IFE keeps its layer deliberately outside
  the main library and says so in the generated header's comment; mirror that, including
  the comment explaining *why* it is outside.
- [ ] K4.2 Add to the `xcode` preset (`FASTFHIR_BUILD_CONFORMANCE=ON`) so it is present
  while debugging and absent in a default release build — the whole point of the split.
  Give it a `Tests`/`Libraries` FOLDER per the IDE-layout block.
- [ ] K4.3 Confirm the object-only-target trap does not apply (it has real sources), and
  that a build with the option OFF links and runs unchanged.

### K5. Tests — red-green, and one that pins the wire

- [ ] K5.1 Detached: a spec-violating but structurally valid `PatientData` writes and
  reports success. This is the first test IFE's example makes, and it is the one that
  proves conformance is opt-in.
- [ ] K5.2 Attached: the same input fails, the `Status` names the right field, and the
  sink holds the citation. Assert the citation text, not just that it is non-empty.
- [ ] K5.3 Chaining: a second hooks struct with `next` set is reached only when the first
  passes; assert both orderings.
- [ ] K5.4 **Byte-identity:** write the same valid resource with and without the layer,
  `memcmp` the two arenas. Any difference means the layer is encoding something, which K0
  forbids. Pairs naturally with B7's byte fixture.
- [ ] K5.5 Red-green every check the emitter produces, per `LessonsFromIFE.md` C2: for
  each allow-listed shape, construct input that violates it and confirm the layer fires.
  A generated check that has never fired is not evidence (C1).

### K6. Docs and the Block J reconciliation

- [ ] K6.1 `examples/conformance_layer.cpp` — a worked example in the shape of IFE's
  `examples/validation_layer.cpp`: detached, attached, chained, with `expect()` throwing so
  a demo cannot silently pass. Register it as a test so it cannot rot.
- [ ] K6.2 Re-read J1 with K1's hooks struct in hand and cut what is now redundant —
  specifically J1.2's separate C ABI. Record the outcome under J1 rather than silently
  editing it.
- [ ] K6.3 `CLAUDE.md`: one line in the repo map for the layer, and the K0 split
  (structural mandatory / conformance attachable) added near invariant 5, which currently
  describes only the exception convention.
- [ ] K6.4 README: state plainly that FastFHIR is a serialization library, not a FHIR
  server — no REST, no SMART on FHIR, no OAuth (verified absent 2026-08-12). The
  conformance layer checks *resources*, not *interactions*, and readers will otherwise
  assume a validation layer implies server-side validation. Do together with I3.

---

## Questions for Ryan

Answers unblock the tasks referencing them. Write answers inline after `> Answer:`.

- **Q17 (blocks K1.3):** Conformance-hook dispatch — a named function pointer per block
  (IFE's shape: explicit, type-safe, but 141 members here and a struct that changes with
  every new resource), or a table indexed by `RECOVERY_TAG` with one type-erased signature
  cast back through `TypeTraits<T_Data>` (scales with resource count, type erasure confined
  to one call site, correct by construction because the tag comes from the traits)?
  Recommendation: the tag-indexed table. IFE could afford named members at 18 blocks;
  FastFHIR cannot at 141.
  > Answer:

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
  > Answer (Ryan, 2026-07-08): **Review the benchmark repo at `../FastFHIR-benchmark/`**
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

---

# ▶ WORK ORDER — FF_* EXTERNAL API: asciidoc generation + README sweep

> **Structural review against `../Iris-File-Extension/spec/` (2026-08-21).**
> The question was how much of IFE's rendered spec comes from source versus
> hand-written code blocks. Measured:
>
> | | `docs/api.adoc` | `spec/ife_spec.adoc` |
> |---|---|---|
> | lines | 356 | 898 |
> | code blocks | **12** | **0** |
> | lines inside code blocks | **158 (44%)** | **0** |
> | `include::` directives | **0** | **41** |
>
> **IFE's spec contains no hand-written declarations at all.** Every layout and
> value table is an `include::` of a fragment generated from three JSON files
> (`ife_constants.json`, `ife_fields.json`, `ife_header.json`) extracted from
> source — 42 fragments. The `.adoc` is prose plus include directives.
>
> The fragments render as **AsciiDoc tables, not code blocks**, and that is the
> reviewability difference: a table diff is one row per changed field, whereas
> `api.adoc`'s 40-line `FF_Result` block diffs as a wall. Each fragment carries
> a `DO NOT EDIT` banner, the generating command, and a **sha256 wire witness**
> that `--check` verifies against the spec. Tables cross-reference each other
> (`<<ife-const-recovery-codes,recovery codes>>`) instead of repeating values,
> and offsets are *derived from field order and width, never stored* — the same
> rule as this repository's symbolic vtable sums.
>
> **This is the argument for I1.9, now with evidence.** 44% of `api.adoc` is
> C++ transcribed by hand from `FastFHIR.hpp`, and the review below found
> **nine defects in it**, including an example that does not compile. That is
> the expected failure rate for hand-copied declarations; it is not a reason to
> proofread harder. `api.adoc` should become prose + generated includes, with
> the declarations extracted from the header the same way I1.7 extracts the
> wire tables. Until then the hand-checking is the cost of the current shape.
>
> **Adopted into `docs/build_docs.sh` immediately — a third gate.** IFE's
> script checks something mine did not: `include::` is a directive ONLY at
> column 0; indented anywhere else, Asciidoctor prints it as literal text, exits
> 0, and the "Unresolved directive" check stays silent for the worst reason —
> nothing was left unresolved because nothing was ever resolved. **Reproduced on
> this tree** (indented include → rendered as text, every existing gate passed
> it), then fixed and red-greened. IFE shipped six value tables that way before
> anyone noticed.
>
> **Still worth taking from IFE's script, not yet adopted:**
> - Regenerate the fragments *before* rendering, so a stale table cannot ship.
>   Not applicable until FastFHIR has a doc generator (I1.7/I1.9).
> - A PDF theme that left-aligns body text: the default justifies, which spaces
>   words badly around long identifiers — and this document is made of them
>   (`ife-pdf-theme.yml` is 677 bytes; copying the approach is cheap).
> - Treat the PDF as optional and HTML as the gate, so a machine without
>   `asciidoctor-pdf` still runs the checks. Mine currently hard-fails on both.
>
> > **Review of `docs/api.adoc` (2026-08-21).** Checked claim-by-claim against
> `include/FastFHIR.hpp`. Corrections applied to the doc; two findings are
> **code/build defects, not documentation defects**, and are listed below.
>
> **1. The installed header set does not compile.** `install(FILES ...)` ships
> seven headers, but the closure is missing five: `FastFHIR.hpp` includes
> `FF_Version.hpp`, and `FF_Parser.hpp` includes `FF_Dictionary.hpp`,
> `FF_Memory.hpp`, `FF_Ops.hpp` and `FF_Utilities.hpp`. So `#include
> <FastFHIR.hpp>` against an installed prefix fails. `FF_Ops.hpp` was
> additionally documented as "internal, not installed" while being a hard
> dependency of an installed header. **Belongs in Block H (packaging).** The
> decision is which of the five become public headers versus getting their
> public parts hoisted into `FF_Primitives.hpp`; that is a design call, so it is
> recorded rather than fixed.
>
> **2. `FF_Compact` sizes its destination arena from the source** on the
> assumption that compaction never grows a stream. The dense layout drops absent
> slots but adds a presence bitmask per block, so a fully-populated resource is
> not obviously smaller, and nothing asserts it. It fails safely —
> `claim_space` throws, `FF_Compact` returns `FF_CAPACITY_EXCEEDED` — so this is
> a documentation-and-assertion gap, not a corruption risk. Worth a test that
> compacts a fully-dense resource and pins the outcome.
>
> **3. The two Mermaid diagrams do not render as diagrams.** `asciidoctor
> docs/api.adoc` exits 0 with no warnings, but both blocks emit as
> `<code class="language-mermaid">` — highlighted source, not a picture. The
> Markdown-style ``` fences are fine (Asciidoctor accepts them), and so is the
> rest of the Markdown-in-AsciiDoc the file uses; what is missing is a diagram
> toolchain. I1.9 pins `asciidoctor` + `asciidoctor-pdf` but not
> `asciidoctor-diagram`, which is what turns a `[mermaid]` block into an image
> (and needs mermaid-cli, or Kroki to avoid the Node dependency). Since the
> style guide requires diagrams in architecture docs, whichever route is chosen
> has to be pinned in I1.9 before this document is generated rather than
> hand-written. Not changed here — it is a pipeline decision.
>
> Doc corrections made: the mutation example did not compile (`= "final"`
> instantiates `TypeTraits<char[6]>`; a string value must be a
> `std::string_view`) and applied a `Fields::PATIENT` key to an `Observation`
> handle; `FF_Result_Code`'s explicit `0x100` failure banding was missing;
> `FF_Result`'s non-explicit one-argument constructor was undocumented; the
> "single Info struct" convention has four exceptions, not one; the
> `append_obj(offsets, tag)` overload is public, not removed; the lifecycle
> diagram named a struct where a function belongs.
>
> **Method note:** `.arbiter/` was used as the map. It is partly stale — it
> still indexes `include/FF_API.hpp` and `src/FF_API.cpp` as separate files,
> and that header no longer exists (its contents were folded into
> `FastFHIR.hpp`). Every claim was therefore re-derived from source, and the two
> examples were compiled rather than read.

Written 2026-08-21 after the FF_* external API landed (see `include/FastFHIR.hpp`,
`src/FF_API.cpp`, `docs/api.adoc`). The API itself is done; what remains is
keeping the docs honest.

## WO-A — Generate `docs/api.adoc` from source

**Status: open.** `docs/api.adoc` is currently hand-written to mirror
the `FF_*` block of `include/FastFHIR.hpp`. The header is the source of truth
and the two WILL drift.

- [ ] 1. Add a generator module (e.g. `generator/emit/api_doc.py`) that parses
   `include/FastFHIR.hpp` (the `FF_*` free functions, `FF_*Info` structs with
   defaulted members, enums, handle typedefs) and emits the asciidoc sections
   of `docs/api.adoc`.
- [ ] 2. The generator must be deterministic and idempotent; run it in the same
   configure-time step as the main generator (or as a ctest wire-gate-style
   check that fails if `docs/api.adoc` is stale — see the golden-file pattern
   in `tests/generator/`).
- [ ] 3. Keep the hand-written prose (design conventions, lifecycle diagram,
   "intentionally not part of this surface") above a marker like
   `// GENERATED-FROM include/FastFHIR.hpp — do not edit below this line`.
- [ ] 4. Do not use Doxygen/Breathe unless already in the toolchain — a small
   dedicated parser keeps the build dependency-free.

## WO-B — README.md API-example sweep

**Status: open.** `README.md` (61 sections) still shows the pre-FF_* usage
(`FastFHIR::Builder`, `builder.finalize(...)`, `Compactor::archive(...)`,
`Ingestor.ingest(...)`). `tests/cpp/test_readme.cpp` — the compiled contract
for those examples — already migrated to the FF_* surface; the README prose
must follow so the docs and the test agree.

- [ ] 1. Replace every code block that constructs `Builder`/`Parser`/`Ingestor`
   directly with the `FF_*` equivalent (mirror `tests/cpp/test_readme.cpp`,
   which now compiles the exact FF_* spellings).
- [ ] 2. Update the "Memory / Stream / Ingestor" API tables to the FF_* names and
   point readers at `docs/api.adoc` for the reference.
- [ ] 3. Add a note that the mutation path (`handle["field"] = value`) is unchanged.

**Rules:** do not commit; do not edit `generated_src/`; verify with
`cmake --build --preset ninja && ctest --preset ninja` (py_roundtrip is a
pre-existing failure — see the profile/allowlist note in that test).

---

# ▶ WORK ORDER — FF_* out-params are not cleared on the argument-check path

Written 2026-08-21, found from the Iris side: `Iris::create_memory_arena`
(`Iris-Headers/priv/IrisMemory.hpp`) was written by copying `FF_CreateMemory`'s
shape, inherited this ordering with it, and a probe over the new function's
branches caught it there. The same probe run against these would fail the same
way. Iris fixed it by clearing first; this is the same fix upstream.

**The defect.** Every out-param entry point promises the handle is emptied when
the call fails — `@p out_memory is null on failure` (`FastFHIR.hpp:187`),
`@p out_stream is null on failure` (`:211`), `@p out_parser is invalid (bool
false) on failure` (`:269`). The clear happens *after* the argument checks, so
the `FF_Invalid` early return skips it. A caller reusing a handle across calls
keeps the object it believes was just replaced, and `FF_INVALID_ARGUMENT` is
precisely the case where a caller is already confused about what it passed.

Nothing is corrupted and nothing leaks — the handles are `shared_ptr` and value
types. It is a contract the header states and the code does not keep.

**Affected — seven, in two files.** `src/FF_API.cpp`: `FF_CreateMemory` (54
before 55), `FF_CreateStream` (88/90 before 91), `FF_StreamFinalize` (116
before 117), `FF_StreamQuery` (126 before 127), `FF_Parse` (137 before 138),
`FF_Compact` (148 before 149). `src/FF_Ingestor.cpp`: `FF_Ingest` (1446–1449
before 1450–1451, both out-params).

**Not affected.** `FF_CreateIngestor` (`FF_Ingestor.cpp:1433`) already clears
first — **it is the precedent, so this is a consistency fix rather than a new
convention**. `FF_MemoryReset`, `FF_StreamSetRoot` and `FF_IngestInsertAtField`
take no out-param.

### Precheck

```bash
cd /Users/ryanlandvater/GitHub/FastFHIR
awk '/^FF_Result FF_CreateMemory/,/^}/' src/FF_API.cpp
awk '/^FF_Result FF_CreateIngestor/,/^}/' src/FF_Ingestor.cpp
```

**Expect:** in the first, `return FF_Invalid(...)` *above* `out_memory.reset();`.
In the second, `out_ingestor.reset();` as the **first** statement of the body.
If they already agree, the fix has landed — stop and say so.

### The edit

Move the clear above the argument checks in all seven, so it is the first thing
every one of them does. One line moved per function; no logic changes, and
`FF_Guard` is untouched.

- [x] `FF_CreateMemory` — clear first (src/FF_API.cpp)
- [x] `FF_CreateStream` — clear first (src/FF_API.cpp)
- [x] `FF_StreamFinalize` — clear first (src/FF_API.cpp)
- [x] `FF_StreamQuery` — clear first (src/FF_API.cpp)
- [x] `FF_Parse` — clear first (src/FF_API.cpp)
- [x] `FF_Compact` — clear first (src/FF_API.cpp)
- [x] `FF_Ingest` — clear both out-params first (src/FF_Ingestor.cpp)
- [x] Contract test — `tests/cpp/test_api.cpp` (`ff_test_api` ctest target):
  passes an already-populated handle into each of the seven with deliberately
  invalid arguments and asserts the handle is empty afterwards. **All DONE
  2026-08-21** — verified by `ctest -R ff_test_api`.

**Done when:** a test passes an already-populated handle into each of the seven
with deliberately invalid arguments — `FF_CreateMemory` with both `shm_name`
and `filepath` set, `FF_Parse` with a null buffer, and so on — and asserts the
handle is empty afterwards. **Red before the move, green after**; a test that
passes both ways is testing nothing, which is how this survived review three
times (the `docs/api.adoc` sweep above went claim-by-claim over these same
functions and did not catch it, because the header and the doc agree — it is
only the code that disagrees with both).

**Rules:** do not commit; do not edit `generated_src/`; verify with
`cmake --build --preset ninja && ctest --preset ninja` (py_roundtrip is a
pre-existing failure).

---

# ▶ WORK ORDER — TEST REGISTRATION EXTRACTED TO tests/tests.cmake + tests/tests.bzl

Written 2026-08-21. All test registration lived inline in `CMakeLists.txt`
(~200 lines: executables, CTest entries, dependency chains, resource locks, the
Python suite), making the root build file long and hard to read. The Bazel side
(`BUILD.bazel`) had the same problem in miniature — and only 4 of the 10 C++
unit suites.

## Done (2026-08-21)

- [x] `tests/tests.cmake` — the entire `if(FASTFHIR_BUILD_TESTS)` block moved
  verbatim (self-gating include): `add_ff_cpp_test` helper, ASIO FetchContent,
  Synthea download, `ff_test_readme` + `ff_roundtrip` executables, the 10 unit
  suites, all CTest entries (standalone foreach, `_add_cpp_test` README
  sub-tests, dependency chains, resource locks), and the full Python suite
  (`py_setup`, `py_test_1..10`, `py_roundtrip`, PYTHONPATH, locks).
- [x] `CMakeLists.txt` — the block replaced with `include(tests/tests.cmake)`;
  457 lines (was ~700).
- [x] `tests/tests.bzl` — `fastfhir_tests(copts)` defines all 10 C++ unit
  suites + `test_readme` + the `ff_roundtrip` cc_binary. Labels are
  root-package forms (`//:fastfhir`, `//:tests/cpp/test_x.cpp`) — `tests/` has
  no BUILD file, so `//tests:...` labels are invalid, and .bzl labels resolve
  against the containing package (`//tests`) anyway.
- [x] `BUILD.bazel` — inline `cc_test` block replaced with
  `load("//:tests/tests.bzl", "fastfhir_tests")` + `fastfhir_tests(copts =
  _COPTS)`; test set completed (added the 6 missing suites: test_amend,
  test_cc, test_bundle, test_compactor, test_graph_bounds, test_datetime,
  test_api).

## Verified

- CMake: `cmake --preset ninja && cmake --build --preset ninja` clean; spot
  `ctest` runs pass (ff_test_api, ff_test_graph_bounds, cpp_test_1,
  py_getting_started).
- Bazel: `bazel query 'kind(cc_test, //:*)'` resolves all 11 suites;
  `bazel build //:test_primitives` compiles.

## Open follow-ups

- [ ] Wire the Python suites into Bazel (py_* tests currently run under CTest
  only; they need the staged `_core` extension and pytest).
- [ ] `ff_test_readme` under Bazel runs as one test; CTest splits it into 13
  filtered sub-tests. If Bazel parallelism matters, mirror the `--filter`
  split there.

---

# ▶ WORK ORDER — py_roundtrip DOM parity failure: diagnosis + defect list

**Status: diagnosed 2026-08-21; NOT fixed.** `py_roundtrip` fails on every
Synthea fixture (≈4,200 diffs per fixture). The failure is **not a regression
from the FF_* / FF_Result / DT-2 work** — the Aug-19 committed baseline
reproduces it identically.

## Debugging findings (evidence trail — do not re-litigate)

1. **The committed baseline fails too.** Built commit `9f86069` (2026-08-19)
   in a git worktree with the same Aug-18 Synthea fixtures and the same diff
   tool: identical 4,213 diffs, same kinds, same paths. `generated_src/` is
   gitignored, so "diff vs HEAD" on generated files proves nothing — the
   regenerated tree is the DT-2 output either way.
2. **Why "it didn't fail before" is misleading.** `test_roundtrip.py` returns
   `0` (PASS) when `discover_fixtures()` finds nothing. The Synthea fixtures
   landed Aug 18; any run before that (or with a failed download) passed
   vacuously without testing anything.
3. **The diff tool is not the cause.** `roundtrip_diff.py` is unchanged since
   `eb008e2` (Aug 19). It demands **zero** diffs after an **empty** allowlist
   (`ALLOWED_DIFFS` contains only commented examples).
4. **Wrong-fixture trap.** The harness runs the sorted-first fixture
   (`ls | head -1`) but Python `glob.glob(...)[0]` returns readdir order —
   comparing them mixes fixtures (Adrian111 output vs Newton741 source). Early
   "diff counts" (4213 vs 1063, "zero datetime diffs") were artifacts of this
   and are discarded. Always pin the fixture explicitly.
5. **FF_RECOVER_UNDEFINED is the error/absent sentinel by design.** It is
   never emitted as a real field's `child_recovery`. The reader fix below
   derives choice `target_recovery` from the **runtime tag in the slot**
   instead — the sentinel stays reserved.

## Already fixed (2026-08-21)

- **Choice `value[x]` mislabeling** (`valueString` for `valueDecimal`): both
  `standard_node_lookup_field` and `compact_node_lookup_field` now read the
  runtime variant tag from the slot (`slot + DATA_BLOCK::RECOVERY`) for
  `FF_FIELD_CHOICE`, so `get_choice_suffix` labels correctly. Verified:
  extension `value[x]` now emits `"valueDecimal": 42.1426`; fixture diffs
  4,213 → 4,189. (The earlier proposal to emit `FF_RECOVER_UNDEFINED` as the
  choice `child_recovery` was wrong — see finding 5.)

## Remaining defects (≈4,189 diffs per fixture)

| # | Defect | Diffs | Evidence | Fix location |
|---|---|---|---|---|
| 1 | Bundle entry `fullUrl`/`request` never emitted | 1,444 | `BundleentryData.fullurl` is `std::string_view` (FF_Bundle.hpp:75), ingest fills it, `STORE_FF_BUNDLE_ENTRY` never writes it | Wire into the URL trie (see below) |
| 2 | Choice **datetime** variants written as strings (`effectiveDateTime` → `effectiveString`, `onset`, `performedPeriod`) | ~934 | `FF_IngestMappings.cpp`: `data.effective.tag = RECOVER_FF_STRING` for the DateTime suffix — pre-DT-2 emitter | `generator/emit/ingest_mappings.py`: datetime choice variants must write the packed-datetime variant (per-type tag + encoded value); the choice store must encode it |
| 3 | Complex choice variants emit bare `value` (`valueQuantity`, `valueCodeableConcept`) | ~578 | `get_choice_suffix` default returns `reflected_resource_type(tag)`, empty for data types (Quantity, CodeableConcept, Period) | `src/FF_Parser.cpp` `get_choice_suffix`: add data-type tag→suffix map |
| 4 | Extension `url` = 0 | 14 TYPE_MISMATCH | Ingest skips the url write entirely (`"requires Builder context; skipping."`); reader treats url as `FF_FIELD_UINT32` (prints raw code) | Same URL-trie wiring as #1 (below) |
| 5 | Double precision truncated in JSON output (`42.142567166419695` → `42.1426`) | 11 | `print_json` streams doubles at the ostream default precision (6 sig figs); the stored double is exact | `out << std::setprecision(std::numeric_limits<double>::max_digits10)` in the FLOAT64 print path |
| 6 | Out-of-profile resources dropped (Claim/EOB/SupplyDelivery vs `us` profile) | 135 DROPPED_RESOURCE | By design (profile filtering) | Policy call: allowlist entry or broader profile |

## Design intent for #1 + #4: URLs through the trie

`fullUrl` and extension `url` were designed to go through the **FF_URL_DIRECTORY
radix trie** (prefix-sharing — `urn:uuid:`, `http://hl7.org/fhir/...` share
prefixes; `cpp_test_11` proves shared segments collapse onto one entry). The
trie machinery works (`FF_PredigestExtensionURLs` runs before ingest,
`get_url(idx)` reconstructs by walking the prior-chain). The gap is that
neither URL was ever wired in:

- [ ] Intern `fullUrl` (and write extension urls) into the URL directory
  during predigestion.
- [ ] Store the directory ref: `BundleentryData.fullurl` becomes a ref slot;
  `STORE_FF_BUNDLE_ENTRY` + the extension store emit it.
- [ ] Reconstruct at export: `print_json` resolves the ref via `get_url()`.

**Wire-format note:** #1/#4 change the stream (fullUrl goes from absent to a
URL-directory ref) — a witness-visible change like DT-2; re-baseline
`tests/generator/golden/wire_witness.json` in the same change. Suggested
start: the extension `url` write (the ref slot already exists there) to
validate intern→store→reconstruct against the roundtrip, then mirror for
`fullUrl`.

**Priority order:** #2 + #3 are the largest correctable chunk (~1,500 diffs);
#5 is a one-liner; #1/#4 are the URL-trie feature; #6 is a policy call.
Verify with `ctest --preset ninja -R py_roundtrip` after each.
