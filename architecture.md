# FastFHIR — Architecture Reference

> **Scope.** This document is the authoritative architectural reference for the
> FastFHIR engine. It is the document against which code revisions must be
> measured: any change that violates the invariants laid out here is, by
> definition, a regression. The descriptions below are synthesised directly
> from the canonical headers (`include/FF_Primitives.hpp`,
> `include/FF_Memory.hpp`, `include/FF_Builder.hpp`, `include/FF_Parser.hpp`)
> and the generator (`tools/generator/`).
>
> **Audience.** Engine maintainers, code-generator authors, and reviewers.
> Application-level usage examples belong in the README; this document is
> mechanical.

---

## Table of Contents

1. [System Philosophy & Design Invariants](#1-system-philosophy--design-invariants)
2. [Memory Architecture: The Virtual Memory Arena (VMA)](#2-memory-architecture-the-virtual-memory-arena-vma)
3. [The Dual-Layer Type System](#3-the-dual-layer-type-system)
4. [Binary Wire Format: `DATA_BLOCK` Anatomy](#4-binary-wire-format-data_block-anatomy)
5. [The Array Subsystem: Inline vs. Offset](#5-the-array-subsystem-inline-vs-offset)
6. [High-Performance Primitives](#6-high-performance-primitives)
7. [Concurrent Builder Mechanics](#7-concurrent-builder-mechanics)
8. [Zero-Copy Read Path (`Reflective::Node`)](#8-zero-copy-read-path-reflectivenode)
9. [Code Generation Pipeline (`make_lib.py`)](#9-code-generation-pipeline-make_libpy)

---

## 1. System Philosophy & Design Invariants

FastFHIR is a binary container format and execution engine for HL7 FHIR
resources. It is engineered around four hard, non-negotiable invariants. Every
data structure, allocator, generator, and accessor in the codebase exists in
service of these invariants; any proposed change that contradicts one must be
rejected at review time.

### 1.1 Zero-Copy Random Access — O(1) field navigation

A field on any FHIR object must be reachable by **pure pointer arithmetic** on
the underlying byte arena, never by parsing, scanning, or dictionary lookup at
read time. Concretely:

- Every block has a fixed-stride V-Table immediately after its 10-byte
  universal header (see §4). A field's byte offset within that V-Table is a
  compile-time constant baked into a generated `FF_FieldKey` (see
  `FF_Primitives.hpp::FF_FieldKey`).
- An array element's address is `entries() + index * stride`; stride is a
  compile-time invariant of the element class (see §5).
- A scalar field is read in-place from the V-Table slot. A block-typed field
  stores an 8-byte arena offset; one indirection reaches the child.

**Why.** FHIR documents are fan-in: a single FHIR Bundle is read by many
consumers, often concurrently, often in latency-bound paths (FHIR routers,
CDS hooks, real-time analytics). Read costs dominate. Encoding the document
once with O(1) navigation amortises to a multiplicative speed-up over JSON-
or XML-derived shapes that recompute structure per read.

**Consequence.** The format never compresses field offsets out of header
slots, never re-orders fields by frequency, and never relies on hashing for
the hot-path. It pays the price of fixed slots — including null sentinels for
absent fields — to preserve the invariant.

### 1.2 Lock-Free Concurrency — Atomic space reservation

Multiple writer threads must be able to ingest into a single arena
simultaneously without mutexes, condition variables, or producer/consumer
queues on the hot path.

- Space is reserved by a single `fetch_add` on a `std::atomic<uint64_t>`
  write-head (`Memory::claim_space`, `FF_Memory.hpp:74`). The returned offset
  is the writer's exclusive slice for the call's duration; no other writer
  can ever observe that range as available.
- Once written, the slice is published to readers via the release-semantics
  load on `Memory::size()` (`FF_Memory.hpp:307–309`).

**Why.** FHIR ingestion is naturally embarrassingly parallel — Bundle entries,
batched messages, and parallel decoders all need to land in the same
addressable arena to preserve cross-resource references (`ResourceReference`,
§6). A mutex-guarded heap allocator would serialise that fan-in and erase the
benefit of multi-core decode.

**Consequence.** All append paths are forbidden from holding any lock other
than the implicit hardware fence on `fetch_add`. Block layouts are forbidden
from requiring back-references to siblings written by other threads;
back-patching is restricted to the parent the current writer owns
(`Builder::amend_*`, §7).

### 1.3 Memory-Mapped Substrate — Sparse, high-capacity VMA

The arena is **virtual memory**, not heap memory. By default it is a 4 GiB
sparse mapping (`FF_Memory.hpp:52, 60`); only the touched pages are committed
by the OS, so the cost is paid as data is actually written.

- Three flavours coexist: anonymous RAM, POSIX SHM (cross-process), and
  file-backed (`Memory::create`, `Memory::createFromFile`).
- Sparse-by-default means the high-capacity reservation is essentially free
  and means that the lock-free `fetch_add` allocator never has to grow,
  rebase, or invalidate pointers.

**Why.** Two distinct properties fall out of one decision:

1. **Pointer stability.** Because the mapping never moves, raw arena offsets
   recorded in V-Tables and arrays remain valid forever — no fix-up pass is
   needed before reads, and the `Reflective::Node` lens (§8) can hold raw
   pointers safely.
2. **Process boundary erasure.** SHM-backed arenas are addressable from
   sibling processes (compactor, exporter, language bindings) and file-backed
   arenas are addressable from a future process — i.e. the live in-memory
   format and the on-disk archive format are the same bytes. There is no
   serialise/deserialise step.

**Consequence.** All offsets are 64-bit (`Offset = uint64_t`,
`FF_Primitives.hpp:41`) even though current arenas are 4 GiB; future growth
will not require schema changes.

### 1.4 Version Awareness — Forward and backward compatibility

FastFHIR encodes both the engine layout version and the FHIR schema revision
(R4/R5/…) into `FF_HEADER`. V-Tables are **dynamically sized** per FHIR
revision: each generated block exposes a `get_header_size()` accessor that
returns the field-area length appropriate to the stream's revision. A reader
compiled against R5 can read an R4 stream by clamping access to the smaller
header; a reader compiled against R4 can ignore R5-only fields by treating
them as out-of-bounds null sentinels.

- `FF_HEADER::FHIR_REV` (bytes 6–7) — the FHIR revision tag (`FHIR_VERSION_R4
  = 0x0400`, `FHIR_VERSION_R5 = 0x0500`).
- `FF_HEADER::VERSION` (bytes 50–53) — encoded engine version. The top
  `FF_STREAM_LAYOUT_BITS` (=2) bits are reserved for stream-layout flags
  (`FF_STREAM_LAYOUT_STANDARD`, `FF_STREAM_LAYOUT_COMPACT`); the remaining
  bits are the engine version. Encoding/decoding are
  `FF_ENCODE_HEADER_VERSION` / `FF_HEADER_ENGINE_VERSION` /
  `FF_HEADER_STREAM_LAYOUT` (`FF_Primitives.hpp:80–91`).

**Why.** FHIR is a moving target. The cost of breaking on-disk compatibility
on every R-bump is unacceptable for any system with persisted data. The
version-aware V-Table is the explicit mechanism that makes the on-disk shape
invariant under FHIR revision drift.

---

## 2. Memory Architecture: The Virtual Memory Arena (VMA)

`include/FF_Memory.hpp`. Namespace `FastFHIR`. Two types: `Memory` (Handle)
and `FF_Memory_t` (Body). This is a textbook Handle/Body pattern lifted into
shared ownership semantics.

### 2.1 `Memory` & `FF_Memory_t` — Handle / Body

- **`FF_Memory_t`** holds the OS-level resources: the base pointer
  (`m_base`), the capacity, the file descriptor / OS handle, the SHM/file
  name, and crucially `m_head_ptr` — a raw pointer into the first eight bytes
  of the mapping that holds the atomic write-head. Constructed only via the
  private constructor; lifetime is `std::shared_ptr<FF_Memory_t>`.
- **`Memory`** is a thin wrapper around `std::shared_ptr<FF_Memory_t>`. It is
  copyable, movable, and trivially passable across threads. All public API
  forwards inline to the body (`FF_Memory.hpp:300–309`).

**Why the pattern.** Multiple subsystems (`Builder`, `Parser`,
`Memory::View`, language bindings, network sinks) need long-lived references
to the *same* mapping without any one of them dictating its lifetime.
`shared_ptr` makes "the mapping is alive iff any consumer still cares" a
first-class invariant; the destructor of the last surviving handle unmaps the
arena. Handle/Body keeps the user-facing surface (`Memory`) cheap while
isolating the resource (`FF_Memory_t`) behind a non-constructible private
type.

### 2.2 Lock-Free Claiming — `claim_space()`

The atomic write-head is a `uint64_t` co-located inside the `FF_HEADER`
region of the arena, accessed via `std::atomic_ref<uint64_t>` against
`*m_head_ptr`. Specifically, it occupies the eight bytes at
`STREAM_CURSOR_OFFSET = 8` — the same byte range that `FF_HEADER::STREAM_SIZE`
occupies in the sealed file. During building, those bytes carry the live
cursor; on `finalize()`, the cursor is replaced by the canonical
`STREAM_SIZE` value. The remainder of the header (bytes 0–7 carrying
`MAGIC + RECOVERY + FHIR_REV`, and bytes 16 onwards carrying
`ROOT_OFFSET`, `ROOT_RECOVERY`, etc.) is staged separately during raw
ingest — see `STREAM_PAYLOAD_OFFSET = 16` and the `StreamHead` discussion in
§2.4. `Memory::base()` returns `m_base` directly; arena offsets stored in
V-Tables are relative to `m_base` (offset 0 = start of `FF_HEADER`).

`claim_space(bytes)` performs a single `fetch_add(bytes,
memory_order_acq_rel)` on the write-head, returning the offset the caller now
exclusively owns. Capacity overflow throws `std::runtime_error`; the addition
itself is uncontended in the success path because every concurrent caller
gets a distinct offset by construction.

#### The `STREAM_LOCK_BIT` (bit 63)

```
constexpr uint64_t STREAM_LOCK_BIT = 1ULL << 63;
constexpr uint64_t OFFSET_MASK     = ~STREAM_LOCK_BIT;
```

The high bit of the 64-bit write-head doubles as an exclusion lock for raw
network ingestion. `try_acquire_stream()` attempts to set this bit via CAS;
if it succeeds, the caller receives a `StreamHead` RAII guard, which is the
*only* writer permitted to advance the head until released. Concurrent
`claim_space` calls observe a head with the lock bit set and refuse (or wait,
depending on policy) — `OFFSET_MASK` is applied on every read so that the
*observed offset* is always the lower 63 bits and is never polluted by the
lock bit.

**Why bit 63 specifically.** It is unreachable in practice — exhausting the
lower 63 bits requires an 8-EiB arena — and it allows acquisition,
publication, and the data-offset payload to coexist in a single 64-bit word.
There is no separate mutex to mis-order against the cursor, and there is no
double-word atomic to require special platform support.

### 2.3 Lifetime Safety — `Memory::View`

`Memory::View` is a non-owning lens over the *committed* portion of the
arena, exposed as `data()`, `size()`, and an implicit conversion to
`std::string_view`. Its critical property: it holds a *copy* of the
`std::shared_ptr<FF_Memory_t>` (`FF_Memory.hpp:170`).

```
const std::shared_ptr<FF_Memory_t> m_vma_ref;
```

That single `shared_ptr` is the difference between safe and undefined.
`Memory::View` is the canonical type used to hand the sealed FastFHIR byte
stream to asynchronous sinks: ASIO socket writes, OS background flushes,
SHA-256 hashing on a worker pool. Any of those can outlive the originating
`Builder`/`Parser`. Because the View holds shared ownership, the mapping
cannot be unmapped until the async operation drops the View — eliminating
use-after-free as an architectural class of bug.

**Implicit conversion to `std::string_view`** is provided for ergonomics, but
is documented to drop the lifetime guarantee (`FF_Memory.hpp:144–146`). The
caller is responsible for keeping the parent View alive.

### 2.4 RAII Stream Management — `StreamHead`

`StreamHead` is the exclusive ingestion token returned by
`try_acquire_stream()`.

- **Non-copyable, move-only.** Models exclusive ownership.
- `write_ptr()` returns the live write edge — a mutable pointer into the
  arena where the next byte should land. Exposed as a destination for raw
  socket reads (zero-copy NIC → arena DMA path).
- `available_space()` returns the contiguous bytes remaining before
  `m_capacity`.
- `commit(bytes_written)` advances the atomic head with release semantics
  (publishing the freshly-written bytes to readers) and **keeps the lock
  held**. A single `StreamHead` therefore serves contiguous multi-chunk TCP
  streams without re-acquiring the lock between chunks.
- The destructor (`~StreamHead`) calls `release()`, which clears
  `STREAM_LOCK_BIT` via `release_stream_lock()`.

#### The staged-header trick

The first `STREAM_HEADER_SIZE` bytes of an incoming raw stream contain the
`FF_HEADER`. The bytes at offsets 8–15 of that header (`STREAM_SIZE` in the
sealed-file layout) cannot be written directly to the arena because those
same bytes hold the live atomic write-head during building — overwriting them
would corrupt in-progress concurrent allocations. `StreamHead` therefore
stages the header in a private `m_staged_header[STREAM_HEADER_SIZE]` buffer,
and on `release()` copies the staged bytes back into the arena *around* the
cursor word (`FF_Memory.hpp:362–373`):

```
memcpy(m_base, m_staged_header, STREAM_CURSOR_OFFSET);                     // bytes 0..7  → MAGIC + RECOVERY + FHIR_REV (FF_HEADER's own layout)
memcpy(m_base + STREAM_PAYLOAD_OFFSET,
       m_staged_header + STREAM_PAYLOAD_OFFSET,
       STREAM_HEADER_SIZE - STREAM_PAYLOAD_OFFSET);                        // bytes 16..  → ROOT_OFFSET, …
```

Bytes 8–15 (the cursor itself) are deliberately *not* copied; the live
atomic head value already occupies those bytes and is the source of truth.

> **Note.** `FF_HEADER` is a special block: it inherits from `DATA_BLOCK`
> for the C++ runtime descriptor (`__offset`, `__size`, `__version`) but
> deliberately **shadows** `DATA_BLOCK`'s `vtable_offsets`. Its first eight
> bytes are *not* the universal `VALIDATION` field of §4.1 — they are
> `MAGIC (4) + RECOVERY (2) + FHIR_REV (2)`. The universal 10-byte
> `[VALIDATION | RECOVERY]` header applies to every other block in the
> arena; `FF_HEADER` is the sole, intentional exception, because it must be
> identifiable by file-magic alone before any structural assumptions can be
> made.

---

## 3. The Dual-Layer Type System

FastFHIR distinguishes two orthogonal type questions:

- **Physical**: How do I parse the bytes at this V-Table slot? — answered by
  `FF_FieldKind`.
- **Semantic**: What FHIR concept is in those bytes? — answered by
  `RECOVERY_TAG`.

The two layers compose: a 10-byte slot of kind `FF_FIELD_RESOURCE` may carry
*any* recovery tag identifying the actual referenced resource type. A reader
uses kind to decode and recovery to dispatch.

### 3.1 `FF_FieldKind` — Physical Discriminant

`FF_Primitives.hpp:156–171`.

```
enum FF_FieldKind : uint16_t {
    FF_FIELD_UNKNOWN = 0,
    FF_FIELD_STRING,    FF_FIELD_ARRAY,    FF_FIELD_BLOCK,
    FF_FIELD_CODE,      FF_FIELD_BOOL,
    FF_FIELD_INT32,     FF_FIELD_UINT32,
    FF_FIELD_INT64,     FF_FIELD_UINT64,
    FF_FIELD_FLOAT64,
    FF_FIELD_RESOURCE,  FF_FIELD_CHOICE,
};
```

Kind tells the parser the **storage class** of a slot, not its meaning:

| Kind                         | Slot width     | Interpretation                                   |
|------------------------------|----------------|--------------------------------------------------|
| `FF_FIELD_BOOL`              | 1 B            | 0/1 byte                                         |
| `FF_FIELD_INT32` / `_UINT32` | 4 B            | LE integer                                       |
| `FF_FIELD_INT64` / `_UINT64` | 8 B            | LE integer                                       |
| `FF_FIELD_FLOAT64`           | 8 B            | LE IEEE-754                                      |
| `FF_FIELD_CODE`              | 4 B            | Code dictionary index (top bit = custom string)  |
| `FF_FIELD_STRING`            | 8 B            | `Offset` to `FF_STRING` block                    |
| `FF_FIELD_BLOCK`             | 8 B            | `Offset` to nested block                         |
| `FF_FIELD_ARRAY`             | 8 B            | `Offset` to `FF_ARRAY` block                     |
| `FF_FIELD_RESOURCE`          | 10 B           | `ResourceReference` (offset + recovery tag)      |
| `FF_FIELD_CHOICE`            | 10 B           | `ChoiceEntry` raw 8 B + 2 B recovery tag         |

Slot widths are exactly the values in `TYPE_SIZE`
(`FF_Primitives.hpp:124–137`). Note in particular that resource and choice
slots are **10 bytes**, not 8 — they carry an inline recovery tag because
their concrete type is not statically determinable from the parent V-Table.

### 3.2 `RECOVERY_TAG` — Semantic Identifier

A 16-bit (`uint16_t`) ID embedded at bytes 8–9 of every block (immediately
after the 8-byte `VALIDATION` word). Generated into
`generated_src/FF_Recovery.hpp` from the FHIR StructureDefinitions; the
inclusion is at `FF_Primitives.hpp:119`.

The tag space is partitioned by high byte:

- `RECOVER_FF_SCALAR_BLOCK = 0x0100` — primitive-block tags
  (`RECOVER_FF_BOOL`, `RECOVER_FF_INT32`, …).
- `RECOVER_FF_DATA_TYPE_BLOCK = 0x0200` — the inclusive lower bound for
  generic FHIR data-type blocks (e.g. `RECOVER_FF_STRING`, complex
  datatypes). The runtime check `base >= RECOVER_FF_DATA_TYPE_BLOCK`
  (`FF_Primitives.hpp:221`) groups data-types and concrete resources
  together as the `FF_FIELD_BLOCK` family; specific resources occupy values
  above the data-type range allocated by the generator.

The runtime tag dispatcher `Recovery_to_Kind` (`FF_Primitives.hpp:203–223`)
implements exactly this partition.

#### The `0x8000` Array Bit

```
constexpr uint16_t RECOVER_ARRAY_BIT  = 0x8000;
constexpr uint16_t RECOVER_TYPE_MASK  = 0x7FFF;
inline constexpr bool         IsArrayTag    (RECOVERY_TAG t) { return (t & RECOVER_ARRAY_BIT) != 0; }
inline constexpr RECOVERY_TAG GetTypeFromTag(RECOVERY_TAG t) { return RECOVERY_TAG(t & RECOVER_TYPE_MASK); }
inline constexpr RECOVERY_TAG ToArrayTag    (RECOVERY_TAG b) { return RECOVERY_TAG(b | RECOVER_ARRAY_BIT); }
```

(Defined in `tools/generator/ffc.py:1168–1172`; emitted into
`FF_Recovery.hpp`.)

The array bit is an **orthogonal modifier**, not a separate enumerator. The
recovery tag stamped into an `FF_ARRAY` header for an array of `Observation`
is `ToArrayTag(RECOVER_FF_OBSERVATION)`. A reader inspects the bit with
`IsArrayTag()` to decide whether to descend into element-by-element decode;
it strips the bit with `GetTypeFromTag()` to recover the element type for
type-checking against `TypeTraits<T>::recovery`.

**Why a single bit instead of paired enumerators.** Doubling the tag space
(one entry per type, one per array-of-type) doubles the generated enum, halves
its dispatch density, and forces the generator to keep two tags in sync per
type. A single bit gives O(1) "is this an array of X?" with zero generator
duplication. The `0x8000` choice is mechanically convenient: the resulting
masked value is still a valid 15-bit type tag, so existing dispatch tables
do not need a special case beyond `GetTypeFromTag()`.

The validator (`src/FF_Primitives.cpp:213–214`) enforces the invariant: an
`FF_ARRAY` block whose recovery does *not* have the array bit set is
rejected.

### 3.3 `TypeTraits<T>` — Compile-Time Bridge

`TypeTraits<T>` is the explicit point of contact between strongly-typed C++
data structures and the binary wire format. For each generated FHIR type
(e.g. `PatientData`, `BundleData`), the generator emits a specialisation
exposing:

- `static constexpr RECOVERY_TAG recovery` — the canonical recovery tag for
  `T`; checked at every read/write boundary.
- `static Size size(const T& v, FHIR_VERSION rev)` — total bytes required to
  encode `v` for the given revision (used by `Builder::append` to size the
  `claim_space` reservation).
- `static void store(BYTE* base, Offset off, const T& v, FHIR_VERSION rev)` —
  serialise `v` into the arena at `off`. Must respect kind-and-stride for any
  array fields and back-patch any nested block offsets.
- `static T read(const BYTE* base, Offset off, Size size, uint32_t version)`
  — materialise `T` from arena bytes (used by the `Node::as<T>()` and
  `Entry::operator T()` paths).

The `HasTypeTraits<T>` concept (`FF_Parser.hpp:32–35`) requires `read` only;
the trait is therefore valid for read-only consumer types if `store`/`size`
are not generated.

**Why TypeTraits and not virtuals/RTTI.** Every overhead paid here is paid
on the hot path. A virtual call per field would defeat O(1) navigation; RTTI
would defeat the recovery-tag scheme. Compile-time specialisation collapses
the entire bridge to inlined memcpys on the call site.

---

## 4. Binary Wire Format: `DATA_BLOCK` Anatomy

Every block in the arena — `FF_HEADER`, `FF_CHECKSUM`, `FF_ARRAY`,
`FF_STRING`, `FF_URL_DIRECTORY`, every generated FHIR resource block —
inherits from `DATA_BLOCK` and shares a universal 10-byte header.

### 4.1 The Universal Header (10 bytes)

`FF_Primitives.hpp:321–355`.

```
enum vtable_offsets {
    VALIDATION  = 0,             // 8 bytes (0..7)
    RECOVERY    = VALIDATION + 8, // 2 bytes (8..9)
    HEADER_SIZE = RECOVERY + 2,   // 10 bytes total
};
```

- **Bytes 0–7 — `VALIDATION`.** A 64-bit value carrying the *canonical block
  size* (i.e. the number of bytes in the entire block including its V-Table
  and trailing payload). This serves three purposes simultaneously:
  1. **Bounds checking.** Any read against the block masks the read offset
     against this value before hitting the arena.
  2. **Recovery from corruption.** A scanner walking the arena can advance by
     `VALIDATION` bytes per block, re-synchronising after a damaged region.
  3. **Cheap structural validation.** `validate_offset` (declared at
     `FF_Primitives.hpp:350`) verifies that `__offset + __size` lies within
     the arena and that the bytes at `RECOVERY` match the expected tag.

- **Bytes 8–9 — `RECOVERY`.** The block's `RECOVERY_TAG` (§3.2). This is the
  semantic discriminant on which polymorphic dispatch hangs.

#### `DATA_BLOCK`'s in-memory shadow

`DATA_BLOCK` is *both* a description of bytes in the arena *and* a small
runtime descriptor:

```
Offset   __offset  = FF_NULL_OFFSET;   // arena offset of the block
Size     __size    = 0;                // canonical size (mirror of VALIDATION)
uint32_t __version = 0;                // engine version (for V-Table sizing)
```

These are **not** stored in the arena; they are populated by the parser when
materialising a `Node`. They are what makes O(1) navigation feasible
without re-reading the validation word on every call.

> **`__remote` / `__response` (Emscripten only).** Under
> `__EMSCRIPTEN__`, two extra fields are present (`FF_Primitives.hpp:336–
> 337`) carrying a remote URL handle and an asynchronous response slot.
> These exist solely to support partial fetches of remote arenas from a
> browser; on native targets they are absent and add zero overhead.

### 4.2 The V-Table Architecture

A FastFHIR block's bytes are laid out as:

```
[ VALIDATION (8) | RECOVERY (2) | <V-TABLE: fixed-size field slots> | <trailing payload> ]
```

Field slots come in **fixed sizes** drawn from `TYPE_SIZE` (§3.1).
**Crucially, slots are ordered and statically allocated even for absent
fields.** A field that is absent in a particular instance is encoded as the
canonical null sentinel (`FF_NULL_OFFSET = 0xFFFFFFFFFFFFFFFF` for offset
fields; `FF_NULL_UINT32` for codes; `FF_CODE_NULL` for code-typed primitives;
etc., enumerated in `FF_Primitives.hpp:45–57`).

**Why fixed-stride slots.** Two reasons, both in service of §1.1:

1. The byte offset of every field is then a compile-time constant, baked
   into a generated `FF_FieldKey` (`FF_Primitives.hpp:234–310`) — a struct
   that carries *both* the C-string field name and the precomputed
   `field_offset`, so name-based lookups (e.g. from JSON or Python bindings)
   collapse to a single switch in the reflection table without traversing
   the block at all.
2. It makes the V-Table self-describing: knowing the type (via `RECOVERY`)
   and the version is sufficient to compute every field's location.

#### Version-Aware Offsets (`get_header_size()`)

Each generated FHIR struct exposes `inline Size get_header_size() const`
(`tools/generator/ffc.py:992`). The function consults the block's recorded
`__version` and returns the V-Table size appropriate to that revision.

For example, an R4 `Patient` block has fewer fields than its R5
counterpart; the generator emits the R4 fields with the same byte offsets in
both revisions, and the R5-only fields appended after. `get_header_size()`
returns the R4 value when `__version` reports R4. Reads against R5-only
slots in an R4 block detect the slot is past `get_header_size()` and yield
the null sentinel rather than reading into adjacent payload.

**Why this is correct under both forward and backward reads.**

- **R5 reader, R4 stream.** The R5-only fields are absent in the stream's
  V-Table; the bounded read returns null; consumer treats them as
  "unspecified", matching FHIR semantics.
- **R4 reader, R5 stream.** R4 reader knows nothing of R5-only fields and
  never asks for them; the additional bytes are skipped via
  `get_header_size()` when computing the start of the trailing payload (such
  as inline blocks for arrays). The R4 reader cannot misinterpret R5 fields
  as part of its known schema because field lookup is by compile-time
  offset, not by name.

### 4.3 Back-Patching — `Builder::amend_pointer`

A parent block is allocated *before* its children; the child offsets are not
known at allocation time. The Builder's solution is **back-patching**: write
the parent with `FF_NULL_OFFSET` placeholders, then once the child is
appended, overwrite the parent's V-Table slot with the child's offset.

`Builder::amend_pointer(parent_off, vtable_off, child_off)`
(`FF_Builder.hpp:174`) is the canonical API; specialisations exist for
polymorphic resource fields (`amend_resource`, includes a recovery tag) and
choice variants (`amend_variant`, includes raw bits + recovery), and a
template `amend_scalar<T>` (`FF_Builder.hpp:489–518`) handles fixed-width
scalars with size dispatch on `sizeof(T)`.

The patch is a `STORE_U64` (or smaller for scalars) at
`m_base + parent_off + vtable_off`. It is *not* atomic: a partially-written
patch may be observed by a concurrent reader. This is acceptable because:

- Concurrent reads are scoped to the `Memory::View` returned by
  `Builder::finalize` (§7.4) — i.e. by contract a reader does not observe
  the arena until the writer has finalised. The `m_finalizing` /
  `m_active_mutators` interplay enforces this.
- Mid-build queries are explicitly opt-in via `Builder::query()`, which
  returns a fresh `Parser` but is documented as a snapshot of "the current
  state" — readers must accept that fields may transiently report
  `FF_NULL_OFFSET`.

---

## 5. The Array Subsystem: Inline vs. Offset

`FF_ARRAY` is the workhorse for every list-typed field in FHIR. Its design
hinges on a single observation, which we call the Indirection Paradox.

### 5.1 The Indirection Paradox

Random access — `array[i]` in O(1) — requires that `address(i) = base + i *
stride` for some *constant* `stride`. If `stride` is not constant (i.e. if
the elements are variable-length), random access is no longer O(1) without a
side-band index.

Two element classes exist:

- **Fixed-stride elements** (bools, doubles, fixed-layout blocks): stride is
  trivially the element's size. Store inline.
- **Variable-stride elements** (strings of arbitrary length, polymorphic
  resource references where each entry could be a different concrete type
  with a different size): stride is *not* constant. To preserve O(1), the
  array stores **offsets** to the actual data; the offsets themselves are
  fixed-stride (8 bytes each), and the variable-length data lives elsewhere
  in the arena, reached by one indirection.

The cost of that indirection — one extra pointer chase — is unavoidable; the
alternative is to lose O(1). The architecture is explicit that this trade
is worth it: random access dominates iteration in FHIR consumer workloads,
and the indirection is one cache line at most when the arena is sequentially
allocated.

### 5.2 `FF_ARRAY` Layout

`FF_Primitives.hpp:466–508`.

```
HEADER (16 bytes):
  VALIDATION    (8)   bytes 0..7
  RECOVERY      (2)   bytes 8..9   — array recovery tag (bit 15 / `RECOVER_ARRAY_BIT` always set; low 15 bits = element type)
  KIND_AND_STEP (2)   bytes 10..11 — packed: bits 15..14 = EntryKind, bits 13..0 = stride bytes
  ENTRY_COUNT   (4)   bytes 12..15
ENTRIES:
  ENTRY_COUNT × stride bytes, immediately following the header.
```

The **packed `KIND_AND_STEP` field** is the layout's most distinctive
detail. Two top bits encode the `EntryKind`; the remaining 14 bits hold the
stride. 14 bits supports strides up to 16 KiB per element, which exceeds any
realistic FHIR block size.

```
KIND_MASK = 0xC000   // bits 15..14
STEP_MASK = 0x3FFF   // bits 13..0
```

**Why pack them.** The header would otherwise need a separate 16-bit kind
slot, growing it to 18 bytes and breaking 8-byte alignment of `ENTRY_COUNT`.
Packing keeps the header at exactly 16 bytes — a power-of-two header size
and one cache line — and exposes both fields in a single 16-bit load.

### 5.3 Array Kinds

Defined as `FF_ARRAY::EntryKind` (`FF_Primitives.hpp:476–481`):

#### `SCALAR (0x0000)` — Packed primitives

Each entry is one of: `bool` (1 B), `int32`/`uint32`/`float32` (4 B),
`int64`/`uint64`/`double` (8 B). Stride = `sizeof(T)`. Zero indirection;
`array[i]` is a single dereference.

#### `INLINE_BLOCK (0x8000)` — Contiguous fixed-size blocks

Each entry is a complete sub-block (V-Table + payload) of the element type,
written contiguously. Stride = the element type's `HEADER_SIZE`. Used when
the element type is a fixed-layout struct — e.g. an array of
`HumanName`-style records.

`array[i]` is a single dereference + V-Table walk, no extra indirection.

The generator emits `STORE_FF_ARRAY_HEADER(__base, child_off,
FF_ARRAY::INLINE_BLOCK, T::HEADER_SIZE, n, ToArrayTag(T::recovery))` for this
case (see `tools/generator/ffc.py:782, 800`).

#### `OFFSET (0x4000)` — Mandatory indirection

Each entry is an 8-byte arena offset (`Offset`) pointing at the actual
element block elsewhere in the arena. Stride = 8.

This is the **only** legal kind for:

- **Strings** — `FF_STRING` is variable-length by definition (its `LENGTH`
  field is part of its header).
- **Resources / polymorphic elements** — different elements may resolve to
  different concrete recovery tags and therefore different sizes. (For
  resource arrays, the generator instead emits `INLINE_BLOCK` carrying
  fixed-size 10-byte `ResourceReference` records — see
  `tools/generator/ffc.py:764` and the wrapper definition in §6.2 — but the
  *target* of each reference is reached by offset indirection.)
- **Arrays whose elements would otherwise violate the fixed-stride
  invariant** for any other reason.

The generator emits `STORE_FF_ARRAY_HEADER(__base, child_off,
FF_ARRAY::OFFSET, TYPE_SIZE_OFFSET, n, ToArrayTag(...))` for offset arrays
(`tools/generator/ffc.py:747`).

### 5.4 Reading Arrays

`FF_ARRAY::entries(base)` returns a `const BYTE*` to byte 16 (the start of
the entry region). `entry_step` and `entry_kind` decode the packed
`KIND_AND_STEP` field; `entries_are_pointers` is a convenience for
`entry_kind == OFFSET`. The validator
(`FF_ARRAY::validate_full`) checks that `HEADER_SIZE + entry_count *
entry_step == VALIDATION` and that `RECOVERY` has the array bit set.

---

## 6. High-Performance Primitives

### 6.1 `FF_STRING` — 14-byte header, zero-copy view

`FF_Primitives.hpp:518–546`.

```
HEADER (14 bytes):
  VALIDATION (8)   bytes 0..7
  RECOVERY   (2)   bytes 8..9   — RECOVER_FF_STRING
  LENGTH     (4)   bytes 10..13 — payload byte count
PAYLOAD:
  LENGTH bytes of UTF-8, immediately after the header. NO null terminator.
```

`read_view(base)` returns a `std::string_view` constructed directly over the
arena bytes — no copy, no allocation. `read(base)` is the fallback for code
paths that need owned storage (dictionary parsing, language bindings).

**Why no null terminator.** UTF-8 may legally contain embedded `0x00`
bytes; null-termination would be ambiguous. More importantly, the absence of
a terminator means an `FF_STRING` block is exactly 14 + LENGTH bytes — no
padding, no special-case end byte — preserving exact `VALIDATION`-driven
bounds.

### 6.2 Polymorphic 10-Byte Wrappers

Two structures occupy V-Table slots of size 10 (= `TYPE_SIZE_RESOURCE` =
`TYPE_SIZE_CHOICE`).

#### `ResourceReference` — typed pointer

`FF_Primitives.hpp:614–620`.

```
struct ResourceReference {
    Offset       offset   = FF_NULL_OFFSET;   // 8 bytes
    RECOVERY_TAG recovery = FF_RECOVER_UNDEFINED; // 2 bytes
};
```

The 8-byte offset locates the target block in the arena; the 2-byte recovery
tag identifies its concrete FHIR type (`RECOVER_FF_OBSERVATION`,
`RECOVER_FF_PATIENT`, …). This is the on-the-wire shape of fields like
`Bundle.entry.resource` — i.e. anywhere FHIR allows "any resource".

**Why inline the recovery tag** rather than relying on the target block's
own `RECOVERY` field. Reading the target's recovery requires the
indirection. Inlining it in the reference allows the reader to dispatch
without the fetch — important in tight loops over heterogeneous arrays
(e.g. iterating Bundle.entry).

#### `ChoiceEntry` — FHIR `[x]` field

`FF_Primitives.hpp:623–637`. The build-time staging form is:

```
struct ChoiceEntry {
    RECOVERY_TAG tag;                        // 2 bytes — chosen variant
    std::variant<monostate, bool, i32, u32, i64, u64, double, string_view> value;
};
```

On the wire, the 10 bytes are laid out as **8 raw bytes** + **2-byte
recovery**. The 8 raw bytes are interpreted by the recovery tag:

- For scalar variants (`RECOVER_FF_BOOL`, `_INT32`, …, `_FLOAT64`), the raw
  bits *are* the value, padded to 8 bytes.
- For string and block variants, the raw bits are an `Offset` into the
  arena.

The Builder side is `amend_variant(parent_off, vtable_off, raw_bits, tag)`
(`FF_Builder.hpp:184`); the read side resolves with `Node::resolve_choice`
(`FF_Parser.hpp:305`).

**Why 8 raw bits + 2 tag bytes.** It is the smallest representation that
fits both inline scalars (which avoid the indirection of an offset slot) and
heap-resident polymorphic targets in the same fixed-stride 10-byte slot,
preserving the array invariant of §5.

---

## 7. Concurrent Builder Mechanics

`include/FF_Builder.hpp`. The Builder owns an arena (via `Memory`) and
exposes a thread-safe append/amend API.

### 7.1 Mutation Safety — `try_begin_mutation()` / `m_finalizing`

Two atomics gate the mutation path (`FF_Builder.hpp:49–54`):

```
std::atomic<bool>     m_finalizing;
std::atomic<uint64_t> m_active_mutators;
```

The protocol (implemented in `try_begin_mutation` /  `end_mutation` —
declared at `FF_Builder.hpp:53–54`, defined in `src/FF_Builder.cpp`) is the
classic reader-writer phase split:

- `try_begin_mutation()` checks `m_finalizing`; if set, returns false. If
  unset, increments `m_active_mutators` and returns true.
- `end_mutation()` decrements `m_active_mutators`.
- `finalize()` sets `m_finalizing` first, then **spins on
  `m_active_mutators` until zero** before computing the checksum. New
  mutations are rejected; in-flight mutations are allowed to complete.

`MutationGuard` (a stack-local scope guard, `FF_Builder.hpp:105–108,
137–141, 494–497`) ensures `end_mutation()` is called even on exceptions.

**Why a counter rather than a single bool.** Multiple writer threads must
make progress simultaneously. A single bool would force sequential
ingestion. The counter allows N parallel mutators to coexist; the finaliser
pays the cost of waiting only at sealing time, which is a single event.

**Why fail-fast on `m_finalizing` rather than block.** Finalisation is a
terminal state — once started, the arena will be sealed and further appends
would invalidate the checksum. Throwing `std::runtime_error` immediately
("FastFHIR: Builder is finalizing; append is no longer allowed.",
`FF_Builder.hpp:102, 135, 491`) is the correct behaviour: a writer that
arrives after finalisation has begun has a logical bug to fix, not a wait
to perform.

### 7.2 `ObjectHandle` & `MutableEntry` — Thin Coordinate Handles

`FF_Builder.hpp:264–381`. Both types live in `namespace
FastFHIR::Reflective`.

- **`ObjectHandle`** (~24 bytes: `Builder*`, `Offset`, `RECOVERY_TAG`).
  Identifies *one* parent object in the build. Returned by `append_obj`. The
  user-facing API is `handle[FF_FieldKey] = value;` for V-Table assignments
  and `handle.as_node()` for read-back.
- **`MutableEntry`** (~48 bytes: `Builder*`, base pointer, parent offset,
  vtable offset, recovery tag, kind). The proxy returned by
  `ObjectHandle::operator[]`. Crucially, `MutableEntry` is **ephemeral** —
  it carries the coordinates of one slot but holds no resources — and
  supports the full assignment vocabulary: scalars, structs (via
  `TypeTraits<T>::store`), other `ObjectHandle`s, and `std::vector<Offset>`
  for arrays.

#### Lazy materialisation — `as_node()`

Neither handle eagerly constructs a `Reflective::Node`. The lens is built
only when the user asks for read access:

```
Node ObjectHandle::as_node() const;                     // FF_Builder.hpp:363
Node MutableEntry::as_node() const { return as_handle().as_node(); }  // FF_Builder.hpp:389
```

The point is that during pure write traffic — `handle[Patient::active] =
true; handle[Patient::name] = name_arr;` — no Node is ever materialised. The
chain is `ObjectHandle::operator[]` → `MutableEntry::operator=` →
`Builder::amend_*`, with no intermediate lens construction. Lens
construction is reserved for read paths and pays its (small) cost only
there.

**Why the proxy pattern at all.** A naive API would have `Builder` itself
expose `operator[]`, making cross-thread sharing of a Builder unsafe (two
threads writing to the same field race). The thread-local `ObjectHandle`
binds the operation to a specific parent, so concurrent writers must each
hold their own handle and concurrent writes hit different V-Tables by
construction.

### 7.3 Append Path — `Builder::append<T>`

`FF_Builder.hpp:99–121`.

```
template<typename T_Data>
Offset append(const T_Data& data) {
    if (!try_begin_mutation()) throw …;
    MutationGuard guard{this};

    Size   data_size = TypeTraits<T_Data>::size (data, m_fhir_rev);
    Offset offset    = m_memory.claim_space(data_size);
    TypeTraits<T_Data>::store(m_base, offset, data, m_fhir_rev);
    return offset;
}
```

Three steps, each lock-free:

1. **Size** computed from the typed value via `TypeTraits<T>::size`
   (compile-time inlined, no virtual dispatch).
2. **Reserve** via the `claim_space` `fetch_add` — N threads issuing
   simultaneously each receive a distinct, non-overlapping slice.
3. **Write** via `TypeTraits<T>::store` — into the writer's exclusive slice.
   Because the slice is exclusive, no atomicity is needed within the write.

The published bytes become visible to readers when the next acquire-load
of the head observes the new value (which happens implicitly on the next
`fetch_add` or on `Memory::size()`).

The variant overload `append(const std::vector<Offset>&, RECOVERY_TAG)`
(`FF_Builder.hpp:133–159`) is the strongly-typed path for offset arrays;
it bypasses TypeTraits and writes the array header inline.

### 7.4 Finalisation & Sealing — `Builder::finalize`

`FF_Builder.hpp:207–214`.

```
Memory::View finalize(FF_Checksum_Algorithm algo = FF_CHECKSUM_NONE,
                      const HashCallback& hasher = nullptr);
```

The sealing sequence:

1. Set `m_finalizing = true`. New `append`/`amend_*` calls now throw.
2. Spin on `m_active_mutators` until 0. All in-flight mutations complete.
3. **Bake `FF_HEADER`.** Call `STORE_FF_HEADER(...)` at arena offset 0
   (`FF_Primitives.hpp:424–429`), patching in `STREAM_SIZE`,
   `ROOT_OFFSET`, `ROOT_RECOVERY`, `CHECKSUM_OFFSET`, `URL_DIR_OFFSET`,
   `MODULE_REG_OFFSET`, and the encoded `VERSION`.
4. **Allocate `FF_CHECKSUM` block.** `STORE_FF_CHECKSUM_METADATA` writes the
   `VALIDATION`, `RECOVERY`, and `ALGORITHM` fields and returns a writable
   pointer to the 32-byte hash buffer (`FF_Primitives.hpp:463`).
5. **Compute the digest.** Invoke the user-supplied `HashCallback` over the
   payload range (everything from the header through to immediately before
   the checksum block) and copy the result into the hash buffer. The
   default `algo == FF_CHECKSUM_NONE` skips this step.
6. Return a `Memory::View` over the sealed bytes — exactly the bytes that a
   future `Parser` will validate.

**Why the hash is computed via callback rather than in-engine.** FastFHIR
deliberately does not embed a cryptographic library. Algorithm choice is a
deployment decision (FIPS, BoringSSL, OpenSSL, an in-tree
implementation…); accepting `std::function<std::vector<BYTE>(…)>` keeps the
engine free of crypto dependencies and lets the integrator pick.

---

## 8. Zero-Copy Read Path (`Reflective::Node`)

`include/FF_Parser.hpp`. The read path is *exclusively* via lightweight
value types: `Parser`, `Node`, `Entry`. None of them own memory; all of them
hold raw pointers into the VMA, which is kept alive by the `Memory` handle
embedded in the `Parser`.

### 8.1 The Lens Pattern — `Reflective::Node`

`FF_Parser.hpp:268–440`.

A `Node` is a coordinate plus enough metadata to interpret it:

```
const BYTE*      m_base;                       // arena base
Offset           m_node_offset;                // block offset
Size             m_size;                       // arena size (bounds check)
uint32_t         m_version;                    // engine version
RECOVERY_TAG     m_recovery;                   // semantic tag
RECOVERY_TAG     m_child_recovery;             // for arrays: element type
FF_FieldKind     m_kind;                       // physical kind
bool             m_array_entries_are_offsets;
const ParserOps* m_ops;                        // narrowed-offset dispatch table
```

That's everything. There is no allocation, no virtual table, no vector of
children. The Node is constructed in CPU registers; reads against it are
inlined pointer arithmetic. `Parser::query()` (the Builder's mid-stream
inspection hook, `FF_Builder.hpp:90–92`) advertises this property as "nearly
zero-cost as it only populates CPU registers".

### 8.2 Field Lookup

#### Compile-time — `FF_FieldKey`

For every field of every generated struct, the generator emits a
`constexpr FF_FieldKey` carrying:

- `owner_recovery` — the parent block's recovery tag
- `kind`, `child_recovery`, `array_entries_are_offsets`
- `field_offset` — the V-Table byte offset
- `name`, `name_len` — the field's wire name

`Node::operator[](FF_FieldKey)` is therefore O(1): it constructs an `Entry`
at `(m_node_offset, key.field_offset, key.child_recovery, key.kind)` and
returns it. No string compare, no hash, no dispatch.

#### Runtime — Reflection tables

For dynamic clients (Python bindings, JSON exporter), generated reflection
tables map runtime string keys to the same `FF_FieldKey` values. Lookup is a
single switch over `RECOVERY_TAG` (the parent's type), then a small
switch over field name within that block — both generated by
`tools/generator/ffc.py` (e.g. the dispatcher built around line 1734).

### 8.3 Entry → Node delegation

`Reflective::Entry` (`FF_Parser.hpp:182–255`) is the V-Table-slot coordinate
returned by `Node::operator[]`. It holds:

```
const BYTE*      base;
Offset           parent_offset;
uint32_t         vtable_offset;
RECOVERY_TAG     target_recovery;
FF_FieldKind     kind;
Size             m_size, m_version;
const ParserOps* m_ops;
```

`Entry`'s implicit conversions are split for performance:

- **Inline scalars** (`int32_t`, `uint32_t`, `int64_t`, `uint64_t`,
  `double`) convert directly via `as_scalar<T>` — *no* `as_node()` call,
  *no* pointer hop. The bytes are read in place from the parent's V-Table.
- **`std::string_view`** — for `FF_FIELD_CODE`, the 4-byte code is
  in-place; for true strings (`RECOVER_FF_STRING`), one indirection through
  `as_node()` is required to find the `FF_STRING` block.
- **`HasTypeTraits<T>` structs** — go through `as_node()` and dispatch to
  `TypeTraits<T>::read`.

Implementations are at `FF_Parser.hpp:443–489`. The split is the
optimisation phase commonly referenced as "narrowed offsets and unified
delegation chains": scalar reads short-circuit; structural reads delegate
once and only once to `as_node()`. Per-instance overhead is therefore
*exactly* the size of the type being read, with no Node-construction tax for
the scalar fast paths.

### 8.4 The `ParserOps` table

`Reflective::ParserOps` (forward-declared at `FF_Parser.hpp:38`) is a small
function-pointer table — one per FHIR resource — used to dispatch
operations whose implementation depends on the concrete resource type
(`fields()`, `keys()`, `print_json()`). It is a compile-time substitute for
virtual dispatch: the generator emits one ops table per resource and the
parser threads the pointer through every Node/Entry created from that
resource. This avoids a global lookup-by-recovery on every reflective call
while still preserving the no-virtuals invariant on `Node` itself.

---

## 9. Code Generation Pipeline (`make_lib.py`)

`tools/generator/make_lib.py`. The generator is the single source of truth
for the engine's type universe; the C++ runtime is intentionally *empty* of
hand-written FHIR resource code.

### 9.1 Pipeline Stages

`make_lib.py:main()`:

1. **`fetch_specs.fetch_fhir_specs()`** — pull HL7 StructureDefinitions for
   every supported FHIR revision into a local `fhir_specs/` tree.
2. **`ffc._discover_versions(...)`** — enumerate the revisions found
   (R4, R5, …); the version tuple drives version-aware codegen.
3. **`ffc.resolve_production_resources(...)`** — produce the closed set of
   FHIR resource and datatype definitions to compile.
4. **`ffd.generate_master_dictionary(...)`** — emit the master code
   dictionary (`FF_Dictionary.hpp`), which maps FHIR `code` values to small
   integer indices. The dictionary is the substrate for
   `FF_FIELD_CODE`'s 4-byte slot (top bit reserved as
   `FF_CUSTOM_STRING_FLAG` for code values not in the dictionary).
5. **`ffcs.generate_code_systems(...)`** — emit per-CodeSystem C++
   `enum class` definitions (`FF_CodeSystems.hpp`) so callers get type-safe,
   IDE-completable code constants.
6. **`ffc.compile_fhir_library(...)`** — the main code-generation pass.
   Emits, per resource:
    - `FF_DataTypes.hpp` — the POD struct definitions used at the C++ API
      surface (e.g. `PatientData`, `BundleData`).
    - `FF_FieldKeys.hpp` — the compile-time `FF_FieldKey` constants for
      every field of every type (the substrate for §8.2).
    - `FF_Reflection.{hpp,cpp}` — the runtime reflection tables and
      `ParserOps` instances per resource.
    - `FF_Recovery.hpp` — the master `RECOVERY_TAG` enumeration, including
      `RECOVER_FF_SCALAR_BLOCK = 0x0100`, `RECOVER_FF_DATA_TYPE_BLOCK =
      0x0200`, `RECOVER_ARRAY_BIT = 0x8000`, and the `IsArrayTag` /
      `GetTypeFromTag` / `ToArrayTag` helpers (`ffc.py:1125–1172`).
    - The `TypeTraits<T>` specialisations — `size()`, `store()`, `read()`,
      and `recovery` constant — that bridge each generated POD type to the
      wire format (§3.3).
    - Per-resource `get_header_size()` accessors that select R4 vs R5 vs …
      V-Table sizes (`ffc.py:992`), the realisation of the version-aware
      header invariant (§4.2).
7. **Cleanup** — `fhir_specs/` is removed once generation succeeds.

### 9.2 Why the generator is the architecture

Three design properties depend on the generator and would be impossible to
maintain by hand:

- **Field-offset stability across revisions.** The generator places R4
  fields at the same byte offsets in R5 V-Tables, appending only the
  R5-novel fields. Hand-written code would inevitably drift.
- **Type-tag uniqueness.** `RECOVERY_TAG` values are dense, mutually
  exclusive, and partitioned by category (scalar / datatype / resource /
  array). The generator owns the allocation of new tags as new FHIR
  resources arrive.
- **Reflection / TypeTraits parity.** Every generated struct gets a matching
  `FF_FieldKey` table, a matching `ParserOps` instance, and a matching
  `TypeTraits<T>` specialisation in lockstep. There is no mechanism for
  these to disagree because they all derive from the same StructureDefinition
  pass.

The runtime (`include/FF_*.hpp`, `src/FF_*.cpp`) therefore contains *only*
the engine — allocator, primitives, reader, builder, reflection plumbing —
and *no* FHIR-specific code at all. Bumping FHIR revisions is a
generator-only change.

---

## Appendix A — Invariant Cheatsheet

The following invariants are mandatory; any patch that violates one without
explicit, documented architectural review is a regression.

| # | Invariant                                                                                                   | Enforced by                                              |
|---|-------------------------------------------------------------------------------------------------------------|----------------------------------------------------------|
| 1 | Field reads are O(1) pointer arithmetic; no parsing, no scanning.                                            | Fixed-stride V-Tables (§4.2), generator (§9)             |
| 2 | Append paths take no mutex; only `fetch_add` on the write-head.                                              | `Memory::claim_space` (§2.2)                             |
| 3 | Mappings are sparse virtual memory; no realloc / no rebase.                                                  | `FF_Memory_t` / `Memory::create*` (§2.1)                 |
| 4 | Every block starts with `VALIDATION (8) | RECOVERY (2)` — no exceptions.                                     | `DATA_BLOCK::vtable_offsets` (§4.1)                      |
| 5 | Array recovery tags carry `RECOVER_ARRAY_BIT (0x8000)`; element type recovered with `GetTypeFromTag()`.      | `FF_ARRAY::validate_full`; generator (§3.2)              |
| 6 | Variable-stride elements are always `OFFSET`-kind arrays. Fixed-stride elements are `SCALAR` or `INLINE_BLOCK`.| `FF_ARRAY::EntryKind` (§5.3)                             |
| 7 | `Memory::View` participates in the `shared_ptr` ownership of the arena; raw `string_view` does not.          | `Memory::View::m_vma_ref` (§2.3)                         |
| 8 | `Builder::finalize` waits for `m_active_mutators == 0` before sealing.                                       | `try_begin_mutation` / `m_finalizing` (§7.1)             |
| 9 | FHIR resource code lives only in generated headers; the runtime is FHIR-agnostic.                            | `tools/generator/make_lib.py` (§9)                       |
