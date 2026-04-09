# FastFHIR Hybrid-Ingestion Architecture

## 1. Core Paradigm: Locality vs. Throughput
The ingestion pipeline transitions from deep, single-threaded recursion to a "Hybrid-Deferred" model. This maximizes CPU-to-Memory bandwidth by balancing L1/L2 cache locality for small elements with multi-core throughput for heavy sub-trees.

* Proximal Inlining (Immediate): Small, frequently accessed elements remain close to their parent in memory.
* Deferred Branching (Parallel): Heavy, complex sub-trees are offloaded to worker threads and built in separate memory regions.

## 2. The Parallel Threshold (Cutoffs)
The generator (ffc.py) determines at compile-time which FHIR elements are built immediately and which are deferred to the FIFO::Queue.

| Strategy | Target Elements | Execution |
| :--- | :--- | :--- |
| **Immediate** | Primitives (`bool`, `uint32`), Strings (`string_view`), Codes, and small `DataTypes` (e.g., `Coding`, `Quantity`). | Parsed synchronously by the main thread during the parent's `_from_json` execution. |
| **Deferred** | Complex `BackboneElements` (e.g., `Patient.contact`) and nested `Resources`. | Main thread reserves a V-Table slot and pushes a build task to the queue. |
| **Array-Sliced** | Large arrays of complex types (e.g., `Bundle.entry`). | Main thread iterates the JSON array, slicing each item into an independent queue task. |


## 3. The Task Payload
Worker threads operate entirely independently of the main thread's simdjson iterator state to avoid race conditions. Tasks are initialized using the raw JSON std::string_view, not the stateful simdjson::ondemand::object.

The IngestTask payload consists of:
* parent_handle (FastFHIR::MutableEntry): The atomic slot in the parent's V-Table.
* json_fragment (std::string_view): The independent JSON string slice.
* target_tag (RECOVERY_TAG): The specific block to build.

```cpp
  struct IngestTask {
      FastFHIR::MutableEntry parent_handle;     // Atomic slot in the parent's V-Table
      std::string_view json_fragment;           // Independent JSON string slice
      RECOVERY_TAG target_tag;                  // The specific block to build
  };
```

## 4. Execution Flow
1. The Skid: The main parser thread navigates the top-level JSON structure.
2. Reservation: When hitting a deferred field, the main thread calls builder.amend_scalar to allocate the 8-byte pointer slot in the parent's V-Table.
3. Queuing: A new IngestTask containing the MutableEntry handle and the string_view is pushed to the FIFO::Queue.
4. Worker Build: A thread pops the task, initializes a local simdjson parser, and builds the sub-tree in its own thread-local memory slice.
5. Atomic Patch: The worker executes parent_handle = built_offset, locking the branch into the main tree seamlessly.

## 5. Synergy with Compaction
This high-throughput ingestion model intentionally produces a Fragmented Stream (where child nodes are scattered across the memory buffer due to parallel allocation).
* Working Memory: The Flat mode handles this fragmentation easily via absolute offset jumps.
* Terminal Storage: The Compactor runs as a final pass, reading the fragmented stream and writing it out sequentially into the dense, packed bitset format for datalake export.