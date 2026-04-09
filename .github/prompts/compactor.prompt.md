# FastFHIR Compaction Architecture Summary

This document outlines the agreed-upon architecture for the "Compacted" stream state, designed to eliminate sparse V-Table overhead for terminal storage and datalake export.

1. Core Paradigm: Mutable vs. Terminal

The engine will support two distinct stream states, separated to avoid architectural confusion and maintain predictable performance characteristics.

* Flat Mode (Mutable): The default working memory state. Uses fixed-width V-Tables with absolute offsets for O(1) random-access reads and lock-free worker thread patching.

* Compacted Mode (Terminal): A read-only, dense storage format. Inline mutability is disabled. This is a one-way transformation intended for export, cold storage, or analytical pipelines where memory density is prioritized over zero-copy mutation.

2. Stream-Level Global State

Compaction will be handled at the stream level rather than per-block, ensuring the Parser makes a single architectural branch upon initialization.

* The Header Flag: The global 10-byte FF_HEADER will be extended to include FLAGS bytes array depending on the number of entries within the particular DATA_BLOCK.

* State Check: Setting the FF_STREAM_COMPACTED bit within the HEADER::VERSION (e.g., bit 0) will signal the Parser and all child Node objects to execute sparse-offset logic instead of absolute V-Table hops.

* No Mixed Streams: A stream is either entirely mutable or entirely compacted.

3. Compacted Block Geometry

When the stream is compacted, the DATA_BLOCK V-Table geometry is discarded in favor of a dynamic presence bitset.

  1. Tag (2 Bytes): The standard RECOVERY_TAG remains at offset +0 to identify the block type.

  2. Presence Bitfield (Variable): A bitmask indicating which fields are populated. The compiler (ffc.py) will generate a BITFIELD_BYTES constant for each struct, sized dynamically based on the field count ((FIELD_COUNT + 7) / 8).

  3. Packed Data Lanes: Only the fields marked 1 in the bitfield are stored. They are written sequentially in 8-byte/10-byte increments immediately following the bitfield.

4. SIMD-Accelerated Access (O(PopCount))

To minimize the performance penalty of abandoning fixed offsets, the Parser will use hardware-accelerated instructions to calculate physical memory addresses dynamically.

* Masking: To locate field N, the parser masks out all bits above N−1: masked_bitfield = current_bitfield & ((1ULL << N) - 1).

* Popcount: The number of set bits in the mask equals the number of packed lanes preceding the target field. This will be calculated using __builtin_popcountll (or _mm_popcnt_u64 for resources exceeding 64 fields).

* Resolution: Physical Address = Base + Tag_Size + Bitfield_Size + (PopCount * Lane_Width).

5. The Compactor (Copy-Move Builder)

Compaction cannot be done in place due to the need to shift downstream memory. A specialized Compactor utility will handle the transformation.

* Input: A finalized, Flat-mode FastFHIR memory buffer.

* Traversal: It performs a full recursive walk of the stream.

* Synthesis: For each block, it evaluates the existing V-Table, constructs the presence bitset for non-null/non-empty slots, and appends only the valid data lanes to a new buffer.

* Finalization: The Compactor writes the new FF_HEADER with the FF_STREAM_COMPACTED flag engaged.