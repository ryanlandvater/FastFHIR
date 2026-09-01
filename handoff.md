# FastFHIR handoff — 2026-09-01 (rev 3, post-REC-20 + bounds contract)

**Read this before touching recovery, the benchmark, or the build.** It replaces the
2026-08-24 note that used to live at this path (that one was addressed to
`../FastFHIR-benchmark`, pinned head at `a9fd4e9`, and is now six commits stale). That
note was read in full before being replaced; everything still true in it is either
restated below or already in `CLAUDE.md`, and the benchmark repo keeps its own,
longer `handoff.md` which is the live one for that side.

Head is `d0066ff` on `fix/recovery-holes-and-bounds`. PR #7 is **merged**. The FastFHIR
tree is **clean**; the benchmark repo has uncommitted work — see §0.

**Rev 3 adds:** the read-path bounds contract (§2.12), the block-extent contract restored
to the Iris design (§2.13), REC-20 (§2.14), and a build-system defect that makes recovery
results differ between CMake and Bazel (§2.15 — read this one before trusting any hole
number).

**Rev 2 note.** `src/FF_Recovery.cpp` was rebuilt end-to-end under **REC-19** (two
producers on two threads, top→bottom call stack, `recover_follow_ref_chain`). The rewrite
kept every principle below and reorganised the file; all §5.1 line references were
re-checked against it. It shipped with one regression, now fixed — see §2.7.

This document has two audiences and tries to serve both:

- **Ryan** — §0 through §4 are prose. Read those and you will know what state the repo is
  in and what is still wrong.
- **A future agent session** — §5 onward carries file:line references, reproduction
  commands, and the reasoning behind each decision so it is not re-litigated. Everything
  claimed here was measured; where it was not, it says so.

---

## 0. State of the working tree, right now

**FastFHIR is clean** at `d0066ff` on `fix/recovery-holes-and-bounds`, 11 commits ahead of
`main` and not yet in a PR.

| Repo | File | Status |
|---|---|---|
| FastFHIR-benchmark | `bench/corruption_probe.cpp` | **uncommitted** — `--mode extract` / `--mode verify`, the four-outcome content check (§3.1) |
| FastFHIR-benchmark | `bench/bench_test_5.hpp`, `handoff.md` | **uncommitted, NOT MINE** — pre-existing WIP from an earlier session |

Verification at the moment of writing:

```
cmake --build build --target build_all -j     # 0 errors
ctest --preset ninja                          # 44/44
bazelisk test //...                           # 17/17  (test_recovery + test_views added)
pytest tests/generator -q                     # 48 passed
black --check generator tests/generator       # clean
bazelisk build //bench:corruption_probe       # clean (benchmark repo)
python3 scripts/recovery_sweep.py             # completes; 881 rows, 4 formats (§2.9)
```

**Always force a rebuild before believing a recovery result.** Rev 1 of this document was
written after a run that reported 6/12 recovery failures from a *stale binary*; a forced
rebuild gave 12/12. CLAUDE.md documents the same trap in the opposite direction (a stale
binary printing PASS). Touch the sources, rebuild, then measure.

---

## 1. What already landed (PR #7, merged as `a36e6f1`)

Five commits. Each is independently reviewable; the split was deliberate.

| Commit | What |
|---|---|
| `1c75759` | Install fixes |
| `fcc8e67` | Generated Python fields made deterministic |
| `d9dbe6b` | `FF_Ops.hpp` made internal; named wire accessors |
| `6e08b67` | TASKS.md REC-18 completion record |
| `460abe4` | Profile-stamp guard on `generated_src/` |

### 1.1 `FF_Ops.hpp` is now internal

`FF_Ops.hpp` was reachable from `FastFHIR.hpp` via `FF_Parser.hpp` and `FF_Utilities.hpp`,
so any consumer could hand-decode the wire with raw `LOAD_U*` and vtable offsets. Both
headers dropped the include, and the byte-level reads got names
(`include/FF_Primitives.hpp:1721`):

```cpp
RECOVERY_TAG     FF_GET_RECOVERY_TAG   (const BYTE* base, Offset block_offset);
uint64_t         FF_GET_VALIDATION     (const BYTE* base, Offset block_offset);
uint32_t         FF_GET_STRING_LENGTH  (const BYTE* base, Offset string_offset);
std::string_view FF_GET_STRING_VIEW    (const BYTE* base, Offset string_offset);
uint8_t          FF_GET_CONCEPT_LENGTH (const BYTE* base, Offset concept_offset);
```

Three decisions worth not re-opening:

- **Free functions, not members.** Almost every read is "what is at THIS offset" with no
  block in hand. A member forces `DATA_BLOCK(off, 0, 0).recovery_tag(base)`, and those
  zeros are not cosmetic — `FF_STRING::validate_full` and `FF_ARRAY::validate_full` bound
  their checks against `__size`, so a block conjured to peek at two bytes is a block that
  passes its own validation.
- **Inline in the header, assembling bytes rather than calling `LOAD_U16`.** `FF_Ops.hpp`
  includes `FF_Primitives.hpp`, so it cannot be included from it; and defining these in
  `FF_Primitives.cpp` would put an un-inlinable call on the hot read path, because this
  build has **no LTO**. The wire is little-endian by definition, so the byte order in
  those bodies *is* the format.
- **Measured, not assumed.** Release `-O3`, 50 MB Synthea bundle, `main` built in a
  detached worktree, 8 runs per arm each a min-of-7:

  | | before | after |
  |---|---|---|
  | `validate_FFHR_stream()` | 12.48 ms | 12.78 ms |
  | `Bundle.entry.entries()` | 55.0 µs | 54.2 µs |

  `main`'s own spread was 12.48–13.55 ms. No signal either way.

Also removed as dead: `FF_ArrayHeader` (`FF_Utilities.hpp`) and `Decode::array_header`
(`FF_Ops.hpp`). And `Decode::choice` was reading its tag at a literal `+8` instead of
`DATA_BLOCK::RECOVERY`, which invariant 1 forbids — the accessor removed the literal.

### 1.2 The `generated_src/` clobbering footgun

**This is the one to remember, because it silently broke the tree on 2026-08-29 and cost
the first build of this session.**

`FASTFHIR_PRODUCTION_PROFILE` is a `CACHE` variable → remembered **per build directory**
(`CMakeLists.txt:67`). `FASTFHIR_GENERATED_DIR` is under `CMAKE_CURRENT_SOURCE_DIR` →
**shared by all of them** (`CMakeLists.txt:81`). The generator runs on every configure.

So configuring a *second* build directory without naming the profile regenerates the
shared tree at the `us-core` default and breaks the first one — and the command that does
it says nothing about profiles:

```bash
cmake -S . -B build-opt -DCMAKE_BUILD_TYPE=Release -DFASTFHIR_BUILD_INGESTOR=ON
```

That was CLAUDE.md's own Release recipe. `build-opt/CMakeCache.txt` had held `us-core`
since 21 Aug.

What it leaves is not a clean narrow tree but a **mixture**: `write_if_changed` never
deletes, so resources the narrower profile does not emit keep their wide-profile `.cpp` at
the old mtime, while `FF_CodeSystems.hpp` (emitted under every profile) comes back short —
72 enums instead of 80. The `CONFIGURE_DEPENDS` glob then compiles the orphans against it:
`unknown type name 'FF_Use'`, hundreds of lines from the cause.

`validate_codesystem_enums` (`generator/utilities.py:170`) caught the mixture but runs
*after* the write — it aborts the configure that did the damage without undoing it, and
the tree that was hurt is the other one.

**Fixed** by a stamp at `CMakeLists.txt:94`: `generated_src/.profile` records the profile
that produced the tree, and the configure refuses to regenerate under a different one,
naming both and giving two ways out. Only a *successful* generation is stamped. All four
paths verified, including both escapes the error message advertises.

**Consequence worth knowing: two build directories can no longer hold different profiles
at once.** They never really could; they clobbered each other silently instead. A narrow
tree beside a wide one is now a separate checkout.

The structurally correct fix — moving `generated_src/` into `${CMAKE_BINARY_DIR}` — was
deliberately **not** taken: `BUILD.bazel:17-26` (this repo's) globs `generated_src/*.cpp` out
of the source tree, Bazel never runs the generator, and the benchmark repo symlinks this working
tree and builds it through Bazel. The output path would become build-directory-dependent
with no single path to point the globs at. That is a coordinated two-repo change. See §4.

### 1.3 Generated Python fields were nondeterministic

Two identical configures produced 147 differing files under
`generated_src/python/fields/`. `emit_python_ast` appends (a resource module deliberately
holds two classes from two emitters), but the field pass does not write every module the
AST pass touches: a backbone path carries an underscore its CamelCase key does not
(`Bundle.entry` → `bundle_entry.py` here, `BundleEntry` → `bundleentry.py` there). 144
modules had no truncating writer and restacked their class every configure — 36 copies of
`BUNDLE_ENTRY_PATH` in a long-lived tree. The `.pyi` side had the mirror bug.

Fixed in `generator/bindings/python_fields.py` (first touch truncates, later touches
append; `truncated_by` is keyword-only with no default). Three consecutive configures now
hash identically.

---

## 2. The recovery findings — the substance of this session

### 2.1 How it started: a benchmark number that could not be true

`bench/corruption_probe.cpp` is "Instrument G test 5", the cross-format corruption/recovery
comparison that feeds `scripts/recovery_sweep.py` → `fig8_recovery`.

Its two halves counted **different atoms**:

| mode | what it returned |
|---|---|
| `--mode count` (clean) | `Bundle.entry`'s array length |
| `--mode recover` (damaged) | recovered parent→child **block references** |

The sweep divides one by the other. On the shipped 1.05 MB artifact, on an **undamaged**
file:

```
--mode count   →  1473
--mode recover → 16071      →  1091% recovered at zero bits corrupted
```

`--mode recover` had been re-pointed at `FastFHIR::Recovery` on 2026-08-27; `--mode count`
was left on the pre-recovery unit. The JSON, protobuf and HL7v2 arms count the same unit in
both modes and are self-consistent — **only the FFHR arm was broken, which is the one being
showcased.**

**Every FFHR point on the existing fig8 curve is invalid.** Not pessimistic — arithmetically
meaningless. Do not cite any of them.

Fixed: `--mode count` now uses `Recovery::reachable_blocks()`
(`bench/corruption_probe.cpp:229`), the library's designated clean-baseline entry point,
which enumerates exactly the atom `recover()` reconciles. Baseline and undamaged recovery
are now both 16071 → 100.00% at zero damage.

### 2.2 Then: why was there ANY loss at one flipped bit?

Ryan's instinct, and it was right: the format stores a block's identity **twice** — the
block's own self-offset, and the parent slot naming its address and its type — so a single
flip is exactly what the redundancy exists to absorb. The observed curve sloped from bit
one, which meant something was not absorbing it.

Diagnosis, by correlating where each flip landed against what it cost, across 15
single-bit seeds:

| refs lost | flip lands in | block tag |
|---|---|---|
| **3** | VALIDATION word | 0x1012 |
| **3** | RECOVERY tag | 0x1012 |
| **0** | body +50…+58 | 0x2005 |

Perfect correlation. And in *every* trial `ambiguous=0, unrecovered=0` — **recovery never
failed.** It restored everything it enumerated. The loss was that three references stopped
being *enumerated*: whenever the flip landed in a block's own 10-byte header, that block
left the census and took ~3 references with it (its inbound reference plus its own
outbound ones). The ratio held at scale: 1 hole → 3 lost, 2 → 6, 23 → 77.

That is why it was invisible. The single `recovered=` number cannot distinguish "recovery
failed" from "the reference was never enumerated to be repaired." `--mode report` was added
to the probe (`bench/corruption_probe.cpp:256`) precisely to separate those, and it is what
made the rest of this session possible. **Keep it.**

### 2.3 Five defects, all the same mistake

Every one treated *one damaged witness* as *an absent block*.

| # | Where | What it did |
|---|---|---|
| 1 | `src/FF_Recovery.cpp:618` (`walk_chain`) | Refused to descend into a child unless it was **fully** intact — one damaged witness and the whole subtree below went unreached. |
| 2 | `src/FF_Recovery.cpp:756` (`recover`) | Built its reference universe from the **scan census alone**. Scan finds blocks by self-offset, so a VALIDATION-flipped block contributed no outgoing references. The reference *to* it was still classified and repaired — which is exactly why the report showed zero failures. |
| 3 | `src/FF_Recovery.cpp:481` (`enumerate_block_refs`) | Dropped every **element** of an array whose header self-offset was damaged, even after the walk had recovered the array itself. |
| 4 | `src/FF_Recovery.cpp:888` (`recover`) | Built the **orphan buckets before** closing the holes, so the repoint hypothesis (`H_off` — "find a unique unclaimed orphan of the declared type") searched a pool that structurally excluded the blocks recovery had just recovered. A corrupt parent offset *and* a corrupt child self-offset could not reconcile even though each side had the witness the other needed. **This was Ryan's call, and it was correct.** |
| 5 | `src/FF_Recovery.cpp:919` (`recover`) | A block whose **tag** was the damaged half was never re-read under the type its parent still declared, so its subtree stayed lost even after the reference was classified `TagRepaired`. |

Defect 3 was found *by the new test*, after the fix looked finished.

### 2.4 Two wrong turns, both caught by measurement

Recorded because both are tempting and both are wrong.

**Wrong turn 1 — trusting the parent's declared type whenever the child's tag disagrees.**
Invented **13 references and 15 unrecovered verdicts from one flipped bit.**
`self_ok && !tag_ok` is genuinely two situations wearing one face: the child's tag was
flipped, *or* the parent's **offset** was flipped onto an innocent, perfectly valid block
of another type. Nothing local separates them.

**Wrong turn 2 — deferring to the ranker's preference alone.** Better, still invented
references. The ranker's preference is evidence, not proof.

What works is making the expansion **self-verifying** (`src/FF_Recovery.cpp:970`):
enumerate into scratch under the candidate type, then require every child it produces to
still corroborate something. A correct re-typing yields well-formed children; a wrong
V-Table yields offsets with no witness at all. Fails → discard, keep the reported
`TagRepaired`, leave the subtree for `apply()`.

> **The principle, stated once so it is not lost:** a missing reference is reported and
> therefore honest. A fabricated one is *believed*. Never trade the first for the second.

### 2.5 The other discriminators that turned out to matter

- **The flip budget gates descent** (`src/FF_Recovery.cpp:637`). "Tag corroborates but
  self-offset does not" is a real child whose VALIDATION took the flip *only when the
  stored word is a Hamming neighbour of the address*. If the parent's offset took the flip
  and now names arbitrary bytes whose two tag bytes happen to match, the distance is large.
  Same `FF_RECOVERY_MAX_FLIPS` discipline the classifier already uses, so the walk and the
  verdict cannot disagree about what is repairable.
- **The wire tag names the V-Table and carries the array bit**; `declared` is an *element*
  type. Enumerating an array under its element's V-Table reads the wrong shape entirely.
  Cost me a failing test (`expected 27 got 36`) before it was obvious.
- **`IsTypedOffsetKind` vs `IsTupleKind`** (`src/FF_Recovery.cpp:40-49`). For a typed-offset
  slot the expected child type is compiled into the V-Table and **cannot be corrupted**.
  For a tuple (resource/choice) the type is stored inline on the wire and is a witness like
  any other. This distinction was explored, then superseded by the coherence check in
  §2.4 — which subsumes it and is strictly better. Do not re-add the kind gate on top; it
  costs real recoveries for no additional safety.

### 2.6 Result

```
bits=    0  100.0000%  invented=0        bits=  128   99.7331%  invented=0
bits=    1  100.0000%  invented=0        bits=  256   99.1712%  invented=0
bits=    2   99.9981%  invented=0        bits=  512   87.3972%  invented=0
bits=    8   99.9876%  invented=0        bits= 1024   66.6412%  invented=0
bits=   16   99.9838%  invented=0
```

Flat, then an edge, then rapid loss — the shape Ryan predicted before any of it was
measured. Zero invented references at every level.

**Be precise about the residual.** A 40-trial single-bit sweep leaves **2 of 40** still
losing 3 references, where the coherence check declines conservatively. "Lossless at one
bit" is **38/40 measured, not proven** — the 10-seed curve above happened to draw all
clean. Before this work it was ~21 of 40. Do not publish "lossless at 1 bit" without
re-running a larger sweep (§5.3).

---

### 2.7 The REC-19 rewrite regressed invention, and why

The rewrite added REC-19.7 "reapply": after a reference is repaired, re-enumerate the
block under the **corrected** type so its subtree is not lost. Right idea — it is the
mechanism §3.3 said was missing. It shipped with two defects, and together they fabricated
references in **5 of 40** single-bit trials where the previous code fabricated none.

1. **It followed the rejected address.** `Corroborated` means the *repoint* hypothesis won
   — the ranker decided the parent's stored offset is the damaged half and the real child
   is the candidate it found. The loop re-enumerated `v.block.child` for every repair
   class, so for a `Corroborated` verdict it walked the very offset the ranker had just
   rejected, under the declared V-Table, and lifted nonsense out of it.
2. **It kept whatever that produced.** Its own comment claimed to be "wrong-turn-2's
   self-verification made continuous", but it classified the resulting refs and pushed them
   into `rep.blocks` instead of discarding an incoherent batch. So a wrong repair became
   believed verdicts.

Every one of the five inventing seeds was the same damage class — a flip in the **parent's
reference slot** (+50…+52 inside a bundle-entry block), never a block header. That
signature is what localised it.

**Fixed** by `repaired_target()` (`src/FF_Recovery.cpp:193`), which returns the ranker's
candidate for `Corroborated` and the slot's address for the in-place classes, and by
routing both the expansion and the reapply through one shared `refs_are_coherent()`
(`src/FF_Recovery.cpp:167`) instead of one inline copy and one absent check.

Restored to parity with the pre-rewrite engine, and then some:

| | pre-rewrite (`c4ca82c`) | rewrite as delivered | rewrite + fix |
|---|---|---|---|
| deviating / 40 | 2 | 7 | **2** |
| **inventing / 40** | **0** | **5** | **0** |

A wider **200-trial** single-bit sweep on a different seed sequence: **14 deviating, 0
inventing — 93.0% fully lossless.** That 93% is the honest figure; the earlier "38/40" came
from a narrower draw. The curve in §2.6 is unchanged to four decimal places.

### 2.8 Bazel was not building the recovery engine at all

`ff_test_recovery` ran under ctest (`tests/tests.cmake:127`) and had **no Bazel target**.
The whole recovery subsystem — two-sided reconciliation, hole analysis, repair ranker —
was covered on exactly one of the two build systems, and the Bazel arm is the one the
benchmark repo links. A defect reachable only through Bazel would have been invisible.
Registered in `tests/tests.bzl`; `bazel test //...` is now 16/16.

Separately, `bazel test //...` was descending into `.claude/worktrees/` and building a
duplicate copy of the whole repo (30 targets instead of 15). A new `.bazelignore` covers
`.claude` and the CMake build directories.

### 2.9 The recovery engine segfaulted on heavy damage — a corrupted size it trusted

Running the real four-format sweep (`scripts/recovery_sweep.py`) crashed the probe with
**SIGSEGV** at 256 bits, trial 14. It was never a rewrite regression: the pre-rewrite
engine at `c4ca82c` crashes on the same input. Nobody had run the full 20-trial sweep at
that damage level before.

Cause, and it is a good one. `Memory::size()` reads the arena's write head, and the head
lives at byte 8 of the arena (`Memory::STREAM_CURSOR_OFFSET`) — **the same 8 bytes as
`FF_HEADER::STREAM_SIZE`**. That identity is deliberate: sealing a stream parks the head
at the payload size, so it becomes the declared file size. The consequence is that on a
*damaged* stream those 8 bytes are just corrupted bytes, and `size()` reports whatever
they say. Measured on the failing artifact:

```
file on disk   = 1,071,990
mem.capacity() = 1,071,990
mem.size()     = 8,591,006,582      <- 8 GiB + the file size, straight off the wire
```

`Recovery` took `m_size = memory.size()` and the byte census walked 8 GiB of unmapped
sparse address space — in the one class whose entire purpose is surviving bytes it does
not trust, and whose header promises every read is bounds-checked.

The fix is to bound the engine by an extent the stream cannot influence. `capacity()` is
not that on the real read path: `createFromFile(path, capacity)` takes capacity as a
parameter (4 GiB by default) and it is a sparse RESERVATION, saying nothing about how many
bytes exist. So `Memory` now retains what the OS reports for an existing backing file
(`m_disk_size`, exposed as `Memory::disk_size()`, 0 when unknown), and `Recovery` bounds
itself by `min(size(), disk_size() ?: capacity())`.

Anonymous arenas — which is how the benchmark and every test wrap raw bytes — keep the
`capacity()` fallback, and there capacity IS the byte count, so the bound is exact.

**Watch for this shape elsewhere.** Any reader that takes its extent from `Memory::size()`
is reading a wire value. `Recovery` is fixed; nothing else was audited.

### 2.10 Holes are now matched back to their broken references (REC-18.6, rebuilt)

A hole is not an absence — it is an under-determined block, and matching y holes to x
broken references is an over-determined assignment, not a guess. Three independent
bit-distances constrain each pairing:

1. a block encodes its OWN offset, so the damaged word at position p still reads close to p;
2. the 2 bytes after it are the RECOVERY tag, still close to what the parent expects
   (compiled V-Table type for a typed-offset slot, stored tag half for a choice/resource tuple);
3. the parent's damaged offset still reads close to p.

The previous code used **only** distance 3, and admitted a hole only when its LENGTH
equalled the declared type's size — weak evidence that fails outright when a hole holds
more than one lost block, and which assumes the block starts at the hole's first byte.
Now a pre-pass sweeps every byte of every hole for a *near-validation signature*
(`popcount(stored XOR pos)`), and the classifier scores candidates on all three distances
under one flip budget.

**Calibrating the signature band mattered more than the matching.** Measured over 12,227
hole bytes on a 512-flip artifact:

```
self_cost:  0:0  1:35  2:14  3:7  4:15  5:14  6:15  7:10  8:10
```

A sharp spike at 1–2 (the real lost blocks — a hole needs roughly one flip on its
VALIDATION) sitting on a flat coincidence floor from 3 upward. In *random* bytes even 8
bits would be astronomically unlikely, but hole bytes are not random: the arena is full of
8-byte offset words that naturally share high bits with their own position. Admitting that
floor actively cost repairs — it tied against correct orphan repoints and turned 11 clean
verdicts Ambiguous. The band is therefore **2 bits** (`kHoleSignatureFlips`), not the full
budget.

Two more links were needed to turn a match into recovered data: a reconstructed block must
be **admitted to the census** (otherwise the hole it filled is still counted), and the
reapply must record a verdict for **every** reference the block owns, not only the still-
damaged ones (healthy children were being walked and never counted). `find_gaps` then
re-runs last, so `holes` means "still missing after recovery".

Result on the 512-flip artifact: 44 holes → **17**, references 15,843 → **15,946**
(+103 of the 228 the holes had taken), 29 hole matches won, 0 invented. Curve gain
+0.45% at 512, +0.09% at 256; unchanged at ≤64 where there were few holes to recover.

The remaining 17 holes are the genuinely over-budget cases — both witnesses damaged beyond
the signature band. That is the two-flip limit of duplicated redundancy and needs a third
witness or erasure coding, not better matching.

### 2.11 Test 5 is not a valid cross-format comparison yet

The arms do not measure comparable things, and the percentage curve flatters the wrong
format. Normalized per flipped bit:

| bits | fastfhir | hl7v2 | json | protobuf |
|---:|---:|---:|---:|---:|
| 1 | **0.000** | 1.000 | 1.399 | 39.7 |
| 8 | **0.100** | 1.000 | 1.040 | 21.9 |
| 512 | **0.719** | 0.978 | 0.818 | 2.6 |

**HL7v2 recovers nothing.** Measured without the CSV's one-decimal rounding it loses
exactly 1.000 segments per flip at every level from the first bit — the theoretical
maximum under its own damage model. It looks like 94% at 512 bits only because 8,939 is a
large denominator. Its metric is *label survival*: `hl7v2_recover` counts lines whose first
three characters still form a known segment name. The corruptor targets `'\r'` and those
three characters and **never touches a pipe**, so the failure mode HL7v2 is notorious for
is neither injected nor detected. Its damage is also independent by construction — each
structural byte belongs to one segment, segments have no cross-references, so one flip can
never cost more than one unit.

FastFHIR is scored far more strictly (every parent→child edge reconciled against both
witnesses) on a linked structure where one lost block takes its subtree (~3.7 refs). Before
publishing anything: fix the units, add pipe corruption to the HL7v2 arm, and prefer the
per-flip table over the percentage curve.

### 2.12 The read path had no bounds discipline at all

Ten sites lifted a child pointer out of a slot and dereferenced it to read the child's
tag. The only check was against `FF_NULL_OFFSET`, which catches "absent" and nothing else.
On a damaged stream that pointer is corrupted bytes: measured, one flipped bit produced a
child offset of **9,049,496,092,671** in a 1,071,990-byte stream and the reader walked off
the arena — SIGSEGV, from `Parser`, on exactly the input it exists to survive.

Fixed by one predicate, `FF_BLOCK_IN_BOUNDS`, applied at every hop. Also:
`FF_DECODE_CODEABLE_CONCEPT` had no arena parameter so it *could not* check — it now takes
one, and it is required rather than defaulted, because a caller that cannot say how big
the buffer is has no business dereferencing into it.

### 2.13 `__size` was vestigial — the Iris contract had decayed

FastFHIR's `DATA_BLOCK` carries the same four members as an Iris generated block
(`__offset`, `__size`, `__version`, `__engine_version`). But:

```
constructions passing a real extent :     5
constructions passing literal 0     : 1,647   (1,645 generated, 2 hand-written)
```

**99.7% of blocks were constructed with a fabricated extent.** `__size` was written by
every constructor and read by essentially nothing — `validate_full()` is its only consumer,
called five times, all on `FF_HEADER` at file-open. A field nobody reads can hold anything.

It was never abandoned deliberately. The old generator (`tools/generator/ffc.py`) threads
`__size` correctly in the deserializers and fabricates `0` in the lazy views, **in the same
file**: the deserializers are free functions with the size in scope, and the view struct was
declared `{base, offset}` with no extent, so `0` was the only thing that compiled. The
June 2026 package refactor carried those three lines over verbatim.

Restored in the order that made it a no-op:
1. the view gains `const Size stream_size` and threads it — 1,645 sites, one emitter;
2. the three hand-written sites pass the real extent;
3. `DATA_BLOCK::operator bool()` becomes `fits(HEADER_SIZE)`.

Step 3 surfaced **nothing**, because steps 1–2 had already removed everything it would have
caught. Enforced first it would have taken down `py_roundtrip` and every view accessor at
once. **Order the cleanup before the rule.**

The views also had **zero tests** — which is the whole explanation for how they drifted.
`tests/cpp/test_views.cpp` now covers them.

### 2.14 REC-20 — ranked, cross-referenced hole matching

Implemented; see TASKS.md `^# ▶ REC-20` for the design. Isolated against `b7b6dcb` by
stashing the change and re-running the same artifacts:

| artifact | refs | holes | ambiguous | unrecovered |
|---|---:|---:|---:|---:|
| 256 flips | 16009 → 16009 | 15 → **12** | 5 → **1** | 3 → **2** |
| 512 flips | 15946 → 15946 | 35 → **30** | 10 → **2** | 4 → **2** |

Reference counts are flat; **the gain is certainty** — ambiguous edges fall 5x. 40
single-bit trials: 2 deviating, **0 inventing**.

Two things not to relearn. **The match must stay inside `classify_one`**, competing with
the orphan repoint on one metric: moved to a pass after classification it saw 4 refs
against 44 holes, because the orphan path had already resolved everything else. And
**scoring the offset term against the candidate's exact position beats tuple-against-tuple**
(15946 refs vs 15944): the position is noise-free, and comparing one noisy observation to a
known value beats comparing two noisy ones.

### 2.15 CMake and Bazel compile DIFFERENT ENGINE VERSIONS — read before trusting a hole count

```
header default (what Bazel compiles) : engine 2026.1
CMake's -D (CMakeLists.txt:201,348)  : engine 2026.0
```

`include/FF_Version.hpp` falls back to `MAJOR 2026 / MINOR 1`; `CMakeLists.txt` sets
`MINOR 0` and passes it as a compile definition. **`BUILD.bazel` defines neither**, so it
takes the header default. The two build systems therefore produce libraries that disagree
about the engine version — and `find_gaps` classifies a gap as `VersionSkew` (benign,
expected trailing bytes from a newer writer) rather than `Hole` based on exactly that
comparison.

Consequence, measured on the same artifact with both builds:

| build | `rep.gaps` | `rep.holes` |
|---|---:|---:|
| CMake (2026.0, older than the stream) | 30 | **8** |
| Bazel (2026.1, same as the stream) | 30 | **30** |

Same source, same input, same `recover()`. The benchmark links the **Bazel** build; the
test suite runs the **CMake** build. Every hole figure in this document from the
`corruption_probe` is the Bazel reading; the 8 is what `ctest` sees.

Neither is wrong given its version — the skew allowance is deliberate, and Ryan named it in
the REC-20 brief ("if the decoder is older than the builder we can expect some small bytes
at the end as allowed holes"). What is wrong is that the two builds disagree at all. The
engine version is written into every stream's `FF_HEADER`, so **which value is correct is a
wire decision and Ryan's alone** — do not just make one match the other.

## 3. What is still not good enough

Ordered by how much it distorts a claim someone might make.

### 3.1 The recovery metric still cannot detect misattachment — **highest priority**

`recovered / baseline` is a **ratio of counts**. A resync that reattaches a block to the
*wrong* parent still counts as recovered. This is flaw F in the benchmark repo's own
`handoff.md`, and the fix is already designed and already written:
`bench/bench_test_5.hpp` uses a parent-anchored `UnitRef{parent, offset, tag}` where all
three must corroborate, plus a third `--check` process holding the baseline so the
recoverer never sees the clean artifact.

**That file is dead code.** No `.cpp` includes it, there is no BUILD target, and the
`bench_test_5.cpp` driver its own header comment references does not exist. Until it runs,
treat every number in §2.6 as *"the right unit, unverified attachment."*

### 3.2 The corruption model is far too narrow

Across 15 independent single-bit seeds the corruptor hit exactly **two** structures: a
0x1012 block header, or offset +50…+58 inside a 0x2005 block. The structural-position list
is nothing like "the format's structure." Whatever fig8 measures, it is resilience to one
very specific injury.

This is independent of the recovery work and must be fixed before publishing any curve.

### 3.3 `apply()` (REC-15) does not exist

`recover()` is `const`. It recovers the **census** and reports verdicts; it never writes
the repaired VALIDATION word back. So some repairs are *reported* rather than *realised*,
and a block whose tag was the damaged half keeps its subtree unenumerated until a repair is
applied and the stream re-scanned. This is the direct cause of part of the residual in
§2.6.

### 3.4 The residual 2/40

See §2.6. Both are cases where the coherence check declines. Worth understanding whether
they are genuinely undecidable or merely conservatively rejected — the check is currently
all-or-nothing per block, and a partial acceptance (keep the coherent refs, drop the rest)
may be sound. **Unexamined.**

### 3.5 `generated_src/` still lives in the source tree

The stamp closes the silent path; it does not remove the coupling. Relocating it to
`${CMAKE_BINARY_DIR}` is the real fix and is a coordinated change with `BUILD.bazel` and
the benchmark repo. See §1.2 and §4.

### 3.6 The benchmark's `harness.hpp` depends on a transitive include

It uses `LOAD_U64` in 7 places across `bench/corruption_probe.cpp`,
`bench/resilience_test.cpp` and `bench/bench_test_5.hpp`, and includes `FF_Ops.hpp`
**nowhere**. It compiles today only because the generated resource headers pull `FF_Ops.hpp`
in — incidental, not designed. Add an explicit `#include <FF_Ops.hpp>` to
`bench/harness.hpp`.

---

## 4. Cross-repo coupling (execution contract rule 9)

`../FastFHIR-benchmark/.external/FastFHIR` is a **symlink to this live working tree**. The
benchmark is not building a pinned copy — whatever state this checkout is in is what it
measures. Bazel does **not** run the generator; `BUILD.bazel` globs `generated_src/*.cpp`,
which CMake produced, at whatever profile CMake last used. **Record the profile alongside
every published result.**

Do not commit into `../FastFHIR` from the benchmark repo.

---

## 5. For the next session

### 5.1 Read these first

| Path | Why |
|---|---|
| `CLAUDE.md` | Invariants. Especially "wire constants are permanent" and the `generated_src/` warnings. |
| `include/FF_Recovery.hpp` | The recovery API contract, the damage-class table, and the bit-flip-only threat model. |
| `src/FF_Recovery.cpp:280` | `recover()` — the entrance. The file reads top→bottom from here (REC-19.1). |
| `src/FF_Recovery.cpp:167` | `refs_are_coherent()` — the self-verification, shared by both expansion sites. |
| `src/FF_Recovery.cpp:193` | `repaired_target()` — why a repaired ref is not always followed to the address the slot names. |
| `src/FF_Recovery.cpp:199` | `recover_follow_ref_chain()` — the per-reference judgement. |
| `src/FF_Recovery.cpp:315` | The admission pass. |
| `src/FF_Recovery.cpp:409` | Why orphan bucketing runs last. |
| `src/FF_Recovery.cpp:439`, `:516` | The expansion and its coherence gate. |
| `src/FF_Recovery.cpp:700` | REC-19.7 reapply — read §2.7 before touching it. |
| `tests/cpp/test_recovery.cpp:640` | `one_damaged_witness_costs_nothing` — asserts the whole report shape, not one verdict, because the original defect was invisible in the numbers it was absent from. |
| `../FastFHIR-benchmark/handoff.md` §"Test 5 — known flaws" | Flaws A–F. F is §3.1 above. |

### 5.2 Reproduce the diagnosis

```bash
# from ../FastFHIR-benchmark
bazelisk build //bench:corruption_probe
P=./bazel-bin/bench/corruption_probe
$P --mode count  --format fastfhir --in artifacts/fastfhir.bin    # baseline (16071)
$P --mode report --format fastfhir --in artifacts/fastfhir.bin    # full reconciliation breakdown
$P --mode corrupt --format fastfhir --bits 1 --seed 20260827 \
   --in artifacts/fastfhir.bin --out /tmp/d.bin
$P --mode report --format fastfhir --in /tmp/d.bin
```

`--mode report` prints `blocks_total intact corroborated tag_repaired position_repaired
extent_derived ambiguous unrecovered holes version_skew`. Compare `blocks_total` against
the clean baseline: **a shortfall there with `unrecovered=0` means references were never
enumerated, not that recovery failed.** That distinction is the whole diagnosis.

To locate where a flip landed, rebuild the scan census in Python — a block is any offset
whose 8-byte LE word equals itself — then bisect the differing byte into it. That is how
the table in §2.2 was produced.

### 5.3 Before claiming losslessness

Run ≥200 single-bit trials, not 40, and report both directions (loss **and** invention).
The invention count matters more than the loss count. Current measured figure: **200
trials, 14 deviating, 0 inventing (93.0% fully lossless)** — quote that, not "lossless".

### 5.4 Do not

- Do not re-introduce `FF_Ops.hpp` into public headers.
- Do not add a slot-kind gate on top of the coherence check (§2.5).
- Do not let the walk out-guess the Hamming ranker (§2.4).
- Do not quote fig8 FFHR numbers from before 2026-08-31 (§2.1).
- Do not enable the `imaging` grouping in `CMakePresets.json` — 1,444 Synthea
  `ImagingStudy` resources exercise the opaque-JSON path on every `py_roundtrip` run, and
  that is deliberate coverage.
- Do not configure a second build directory without `-DFASTFHIR_PRODUCTION_PROFILE` (§1.2
  — the stamp will now stop you, but the habit is the point).

---

## 6. Open questions I could not answer

1. **Are the 2/40 residual cases undecidable, or just conservatively rejected?** (§3.4)
2. **Should the coherence check be per-reference rather than per-block?** Keeping the
   coherent refs and dropping the rest may be sound and would shrink the residual.
3. **What should the corruption model actually target?** (§3.2) Answering this needs a
   view on what damage is being claimed resilience against — bit rot, truncation, partial
   overwrite. The current threat model in `FF_Recovery.hpp` says bit-flip only.
4. **Does `walk_chain`'s new descent rule change the clean-stream baseline?** It should
   not — on a clean stream every child has both witnesses — and `reachable_blocks()` on
   clean input is unchanged in every test. But it was not proven over the full Synthea
   corpus, only over the fixtures and the 1.05 MB artifact.
