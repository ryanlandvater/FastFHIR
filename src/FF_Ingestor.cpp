// ============================================================
// FF_Ingestor.cpp
// Main Thread Ingestion Routing & Bundle Parsing
// ============================================================
#include "FF_Ingestor.hpp"
#include "../include/FF_Queue.hpp"
#include "../include/FF_SIMD.hpp"
#include "../include/FF_Utilities.hpp"
#include "../generated_src/FF_Bundle.hpp"
#include "../generated_src/FF_IngestMappings.hpp"
#include "../generated_src/FF_KnownExtensions.hpp"
#ifdef FASTFHIR_ENABLE_EXTENSIONS
#include "FF_Extensions.hpp"
#endif
#include <cctype>
#include <thread>
#include <vector>
#include <algorithm>

namespace FastFHIR::Ingest {

// =====================================================================
// PREDIGESTION — pipelined Extension URL scan + concurrent trie builder
// =====================================================================
//
// Architecture overview
// ─────────────────────
//  Phase 1  Main thread: split the payload into one chunk per Bundle entry
//           (or the whole payload for a single resource).  Chunks are stored
//           as simdjson::padded_string in `chunk_storage`.  Their lifetime
//           spans the full predigestion, keeping every string_view in the
//           queue valid until the consumer thread has finished.
//
//  Phase 2  N producer threads (one per CPU core, capped at chunk count):
//           each thread scans its share of chunks with simdjson ondemand,
//           collects extension URLs as string_views (zero-copy into the
//           padded_string buffers), applies a 64-slot thread-local direct-
//           mapped dedup cache, and pushes UrlBatch structs to the shared
//           MPSC queue.  No arena writes and no heap allocation on the hot
//           path — producers own only stack-allocated state.
//
//  Phase 3  One consumer thread starts *before* the producers and drains the
//           queue concurrently.  For each URL it:
//             a) checks the global url_to_index map (dedup),
//             b) applies the FF_ExtensionFilterMode filter,
//             c) inserts the URL into a per-'/' radix trie whose nodes live
//                in a bump-allocated vector (scratch, not the FF arena),
//             d) claims arena space for each new segment via claim_space
//                (lock-free) and writes an FF_STRING block,
//             e) appends a TrieEntry{prior_idx, seg_off} to the entries vector.
//           All trie mutations happen on the single consumer thread — no
//           locking needed.
//
//  Barrier  Main thread joins all producers, sets `producers_done`, joins the
//           consumer.  At this point `entries` is complete.
//
//  Phase 4  Main thread writes the FF_URL_DIRECTORY block from `entries`
//           (identical to the old Phase 5), records the offset, and returns.
//
// Trie node layout
// ────────────────
//  Each TrieNode represents one '/'-delimited URL segment.  The virtual root
//  (index 0) is the common ancestor of all segments; it carries no URL of its
//  own.  Each real node has an entry_idx in the `entries` vector (the row in
//  FF_URL_DIRECTORY for that segment) and a seg_arena_off pointing to the
//  FF_STRING already written in the arena.
//
//  Children are stored inline (TRIE_FANOUT = 8 slots per node).  When a node
//  overflows, a chain of overflow TrieNodes is allocated — the last non-empty
//  slot of the original node is linked via `overflow_idx`.  Overflow nodes
//  carry no segment themselves; they just extend the child array.
//
//  SIMD-accelerated child lookup: ff_match_mask_u64x8 compares all 8 child
//  hashes in parallel (AVX2 / SSE4.1 / NEON / scalar) and returns a bitmask.
//  Each matching slot is then verified with memcmp to guard against the
//  (astronomically rare) 64-bit FNV-1a collision.
//
// Prior-chain reconstruction
// ──────────────────────────
//  FF_URL_DIRECTORY::get_url() collects segments from leaf to root, reverses,
//  and joins with '/'.  With the per-'/' trie:
//
//    "http://hl7.org/fhir/test" splits into segments:
//      ["http:", "", "hl7.org", "fhir", "test"]
//    Entries: 0→"http:", 1→"" (prior=0), 2→"hl7.org" (prior=1),
//             3→"fhir" (prior=2), 4→"test" (prior=3)
//    get_url(4) → "http:" + "/" + "" + "/" + "hl7.org" + "/" + "fhir"
//                        + "/" + "test" = "http://hl7.org/fhir/test" ✓
//
//  Interior nodes (shared URL prefixes not themselves complete URLs) are
//  included in `entries` so prior chains are always well-formed.  They are
//  *not* added to url_to_index.
//
// ─────────────────────────────────────────────────────────────────────────────

// ─── FNV-1a 64-bit hash (ingestor-local) ─────────────────────────────────────
// Used only for:
//  1) producer-side tiny dedup cache (hash bucket check), and
//  2) trie segment hashing in insert/find child operations.
// Full URL -> ext_ref routing remains keyed by full URL string in
// FF_UrlInternState::url_to_index (see consumer_process_batch and ingest
// mapping lookup path).
static constexpr uint64_t FNV1A_OFFSET = 14695981039346656037ULL;
static constexpr uint64_t FNV1A_PRIME  = 1099511628211ULL;

static inline uint64_t fnv1a(std::string_view s) noexcept {
    uint64_t h = FNV1A_OFFSET;
    for (const unsigned char c : s) { h ^= c; h *= FNV1A_PRIME; }
    return h;
}

// ─── Producer-side batch types ───────────────────────────────────────────────

static constexpr uint32_t URL_BATCH_SIZE = 64;   // UrlBatchEntries per push
static constexpr uint32_t BATCH_Q_CAP   = 256;   // queue node capacity (pow-2)
static constexpr uint32_t DEDUP_SLOTS   = 64;    // thread-local LRU direct-mapped cache

struct UrlBatchEntry {
    std::string_view url;       // points into a chunk_storage padded_string
};

struct UrlBatch {
    std::array<UrlBatchEntry, URL_BATCH_SIZE> entries;
    uint32_t                                  count = 0;
};

using UrlBatchQueue = FIFO::Queue<UrlBatch, BATCH_Q_CAP>;

// ─── ProducerCtx ─────────────────────────────────────────────────────────────
// Stack-allocated per producer thread.  Holds the current in-flight UrlBatch,
// a thread-local dedup cache, and the queue Injector.
struct ProducerCtx {
    UrlBatch                          batch;
    std::array<uint64_t, DEDUP_SLOTS> dedup_cache;
    UrlBatchQueue::Injector           injector;

    explicit ProducerCtx(UrlBatchQueue& q) : injector(q.get_injector()) {
        dedup_cache.fill(0);
    }

    // Add a URL to the current batch, flushing when full.
    // Zero heap allocation on the hot path.
    void push_url(std::string_view url) noexcept {
        if (url.empty()) return;
        const uint64_t h    = fnv1a(url);
        const uint64_t slot = h & (DEDUP_SLOTS - 1u);
        if (dedup_cache[slot] == h) return; // thread-local cache hit — skip
        dedup_cache[slot] = h;
        batch.entries[batch.count++] = {url};
        if (batch.count == URL_BATCH_SIZE) {
            injector.push(batch);
            batch.count = 0;
        }
    }

    // Flush any partial batch remaining after chunk scanning.
    void flush() {
        if (batch.count > 0) {
            injector.push(batch);
            batch.count = 0;
        }
    }
};

// ─── URL collection (producer, recursive) ────────────────────────────────────
// Mirrors collect_extension_urls() but pushes to ProducerCtx instead of a
// vector — no std::string copies, no arena writes on the producer side.
static void collect_extension_urls_pipeline(simdjson::ondemand::object obj,
                                             ProducerCtx& ctx)
{
    for (auto field : obj) {
        std::string_view key = field.unescaped_key().value_unsafe();
        if (key == "extension" || key == "modifierExtension") {
            simdjson::ondemand::array arr;
            if (field.value().get_array().get(arr) != simdjson::SUCCESS) continue;
            for (auto item : arr) {
                simdjson::ondemand::object ext_obj;
                if (item.get_object().get(ext_obj) != simdjson::SUCCESS) continue;
                std::string_view found_url;
                for (auto ext_field : ext_obj) {
                    std::string_view ext_key = ext_field.unescaped_key().value_unsafe();
                    if (ext_key == "url") {
                        std::string_view url_sv;
                        if (ext_field.value().get_string().get(url_sv) == simdjson::SUCCESS)
                            found_url = url_sv;
                    } else {
                        simdjson::ondemand::object nested;
                        if (ext_field.value().get_object().get(nested) == simdjson::SUCCESS)
                            collect_extension_urls_pipeline(nested, ctx);
                    }
                }
                if (!found_url.empty())
                    ctx.push_url(found_url);
            }
        } else {
            simdjson::ondemand::object child_obj;
            if (field.value().get_object().get(child_obj) == simdjson::SUCCESS) {
                collect_extension_urls_pipeline(child_obj, ctx);
            } else {
                simdjson::ondemand::array child_arr;
                if (field.value().get_array().get(child_arr) == simdjson::SUCCESS) {
                    for (auto item : child_arr) {
                        simdjson::ondemand::object arr_obj;
                        if (item.get_object().get(arr_obj) == simdjson::SUCCESS)
                            collect_extension_urls_pipeline(arr_obj, ctx);
                    }
                }
            }
        }
    }
}

// Scan one padded_string chunk for extension URLs and push them to the queue.
static void scan_chunk_producer(const simdjson::padded_string& chunk,
                                 ProducerCtx& ctx)
{
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc = parser.iterate(chunk);
    simdjson::ondemand::object obj;
    if (doc.get_object().get(obj) != simdjson::SUCCESS) return;
    collect_extension_urls_pipeline(obj, ctx);
}

// ─── Consumer-side trie types ─────────────────────────────────────────────────

static constexpr uint32_t TRIE_FANOUT = 8;
static constexpr uint32_t TRIE_NULL   = 0xFFFFFFFFu;

// One node in the arena-backed radix trie.  FANOUT=8 inline child slots;
// overflow nodes (overflow_idx != TRIE_NULL) extend the child array.
struct TrieNode {
    uint32_t entry_idx     = TRIE_NULL;       // row in `entries`; TRIE_NULL = interior
    Offset   seg_arena_off = FF_NULL_OFFSET;  // arena offset of FF_STRING for this segment
    uint32_t overflow_idx  = TRIE_NULL;       // index of overflow TrieNode; TRIE_NULL = none
    uint16_t child_count   = 0;               // populated children in THIS node (≤ FANOUT)
    uint64_t child_hashes[TRIE_FANOUT]  = {}; // FNV-1a hash of each child's segment
    uint32_t child_idx    [TRIE_FANOUT] = {}; // index in trie_nodes vector; TRIE_NULL = unused
};

// Flat entry appended for every trie node (interior or leaf) — provides the
// prior-chain data written into FF_URL_DIRECTORY.
struct TrieEntry {
    uint32_t prior;   // parent's entry_idx, or FF_URL_DIRECTORY::NO_PRIOR for roots
    Offset   seg_off; // arena offset of this segment's FF_STRING (FF_NULL_OFFSET for "")
};

// ─── Trie helper: find child ─────────────────────────────────────────────────
// Searches node `start_idx` and its overflow chain for a child whose hash
// matches `seg_hash` AND whose stored segment bytes match `seg`.
// Returns the child node index, or TRIE_NULL if not found.
static uint32_t trie_find_child(
    const std::vector<TrieNode>& nodes,
    uint32_t                     start_idx,
    uint64_t                     seg_hash,
    std::string_view             seg,
    const uint8_t*               base) noexcept
{
    uint32_t nidx = start_idx;
    while (nidx != TRIE_NULL) {
        const TrieNode& n = nodes[nidx];

        // SIMD match mask restricted to valid children.
        // All 8 lanes are always compared regardless of child_count: the SIMD
        // path has fixed latency (no branch, no loop) which is faster than
        // branching on child_count in the common case where FANOUT >= child_count.
        // valid_mask zeroes out uninitialised slots so spurious matches never
        // propagate to the memcmp verification.
        const uint8_t valid_mask = (n.child_count >= TRIE_FANOUT)
                                 ? static_cast<uint8_t>(0xFFu)
                                 : static_cast<uint8_t>((1u << n.child_count) - 1u);
        uint8_t mask = ff_match_mask_u64x8(n.child_hashes, seg_hash) & valid_mask;

        while (mask != 0) {
            const uint32_t i   = static_cast<uint32_t>(__builtin_ctz(mask));
            mask &= mask - 1u;
            const uint32_t cid = n.child_idx[i];
            const TrieNode& child = nodes[cid];

            // Verify: handle empty segment (stored as FF_NULL_OFFSET)
            if (child.seg_arena_off == FF_NULL_OFFSET) {
                if (seg.empty()) return cid;
                continue;
            }
            const uint32_t slen = LOAD_U32(base + child.seg_arena_off + FF_STRING::LENGTH);
            if (slen == static_cast<uint32_t>(seg.size())) {
                const char* sdata = reinterpret_cast<const char*>(
                    base + child.seg_arena_off + FF_STRING::STRING_DATA);
                if (std::memcmp(sdata, seg.data(), slen) == 0) return cid;
            }
        }
        nidx = n.overflow_idx;
    }
    return TRIE_NULL;
}

// ─── Trie helper: add child ───────────────────────────────────────────────────
// Appends (seg_hash, child_node_idx) to the node's child list, creating
// overflow nodes as needed.  Safe to call after nodes.push_back() since
// all access is by index (not pointer/reference).
static void trie_add_child(std::vector<TrieNode>& nodes,
                            uint32_t               node_idx,
                            uint64_t               seg_hash,
                            uint32_t               child_node_idx)
{
    while (true) {
        if (nodes[node_idx].child_count < TRIE_FANOUT) {
            const uint16_t cnt = nodes[node_idx].child_count;
            nodes[node_idx].child_hashes[cnt] = seg_hash;
            nodes[node_idx].child_idx   [cnt] = child_node_idx;
            nodes[node_idx].child_count++;
            return;
        }
        if (nodes[node_idx].overflow_idx == TRIE_NULL) {
            const uint32_t ov_idx = static_cast<uint32_t>(nodes.size());
            nodes.push_back({}); // new overflow node (no segment, no entry)
            nodes[node_idx].overflow_idx = ov_idx; // re-index after push_back is safe
        }
        node_idx = nodes[node_idx].overflow_idx;
    }
}

// ─── Trie helper: get or create child ────────────────────────────────────────
// Finds an existing child for `seg` under `parent_node_idx`, or creates one.
// `parent_entry_idx` is the prior_idx to record for any newly-created entry.
// Returns the (possibly new) child node index.
static uint32_t trie_get_or_create_child(
    std::vector<TrieNode>&  nodes,
    std::vector<TrieEntry>& entries,
    const Memory&           mem,
    uint8_t*                base,
    uint32_t                parent_node_idx,
    uint32_t                parent_entry_idx, // TRIE_NULL == NO_PRIOR for root children
    std::string_view        seg,
    uint64_t                seg_hash)
{
    // Fast path: existing child.
    const uint32_t found = trie_find_child(nodes, parent_node_idx, seg_hash, seg, base);
    if (found != TRIE_NULL) return found;

    // Allocate FF_STRING for this segment.  Empty segments (from "http://…")
    // use FF_NULL_OFFSET so no bytes are written — seg_string() returns "" for them.
    Offset seg_off = FF_NULL_OFFSET;
    if (!seg.empty()) {
        seg_off = mem.claim_space(SIZE_FF_STRING(seg));
        STORE_FF_STRING(base, seg_off, seg);
    }

    // Allocate an entry (prior chain).
    const uint32_t new_entry_idx = static_cast<uint32_t>(entries.size());
    entries.push_back({parent_entry_idx, seg_off});

    // Create and register the trie node.
    TrieNode new_node;
    new_node.entry_idx    = new_entry_idx;
    new_node.seg_arena_off = seg_off;
    const uint32_t new_node_idx = static_cast<uint32_t>(nodes.size());
    nodes.push_back(new_node); // may reallocate; index-based access remains valid

    trie_add_child(nodes, parent_node_idx, seg_hash, new_node_idx);
    return new_node_idx;
}

// ─── Trie helper: insert URL ─────────────────────────────────────────────────
// Traverse/insert all '/' segments of `url` into the trie.
// Returns the entry_idx of the leaf node corresponding to the full URL.
static uint32_t insert_url_to_trie(
    std::vector<TrieNode>&  nodes,
    std::vector<TrieEntry>& entries,
    const Memory&           mem,
    uint8_t*                base,
    std::string_view        url)
{
    uint32_t node_idx  = 0;       // virtual root (no segment, no entry)
    uint32_t entry_idx = TRIE_NULL; // NO_PRIOR for root-level children

    size_t pos = 0;
    while (true) {
        const size_t next = url.find('/', pos);
        const bool   last = (next == std::string_view::npos);
        std::string_view seg = url.substr(pos, last ? std::string_view::npos : next - pos);

        node_idx  = trie_get_or_create_child(nodes, entries, mem, base,
                                              node_idx, entry_idx, seg, fnv1a(seg));
        entry_idx = nodes[node_idx].entry_idx;

        if (last) break;
        pos = next + 1;
    }
    return nodes[node_idx].entry_idx;
}

// ─── Consumer state ───────────────────────────────────────────────────────────
struct ConsumerState {
    std::vector<TrieNode>  trie_nodes; // scratch trie (not FF arena)
    std::vector<TrieEntry> entries;    // parallel to FF_URL_DIRECTORY ENTRY_TABLE
    FF_UrlInternState&     state;
    const Memory&          mem;
    uint8_t*               base;
    FF_ExtensionFilterMode mode;

    ConsumerState(FF_UrlInternState& s, const Memory& m, FF_ExtensionFilterMode md)
        : state(s), mem(m), base(m.base()), mode(md)
    {
        trie_nodes.reserve(256);
        trie_nodes.push_back({}); // virtual root at index 0
        entries.reserve(64);
    }
};

// Process one batch of URL entries on the consumer thread.
static void consumer_process_batch(ConsumerState& cs, const UrlBatch& batch) {
    for (uint32_t i = 0; i < batch.count; ++i) {
        const UrlBatchEntry& e = batch.entries[i];
        if (e.url.empty()) continue;

        // Dedup: skip URLs already interned (duplicate from another thread/chunk).
        auto it = cs.state.url_to_index.find(e.url);
        if (it != cs.state.url_to_index.end()) continue;

        // Apply extension filter.
        bool suppress = false;
        switch (cs.mode) {
            case FF_ExtensionFilterMode::FILTER_ALL_KNOWN:
                suppress = FF_IsKnownExtension(e.url); break;
            case FF_ExtensionFilterMode::FILTER_NATIVE_ONLY:
                suppress = FF_IsNativeExtension(e.url); break;
            case FF_ExtensionFilterMode::FILTER_NONE:
                break;
        }

        if (suppress) {
            cs.state.url_to_index.emplace(e.url, FF_NULL_UINT32);
            continue;
        }

        // Insert into trie and record in url_to_index.
        const uint32_t idx = insert_url_to_trie(cs.trie_nodes, cs.entries,
                                                  cs.mem, cs.base, e.url);
        cs.state.url_to_index.emplace(e.url, idx);
        // passive_payload: not captured in pipeline mode
        // (WASM Path B feature is not yet fully implemented upstream)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FF_PredigestExtensionURLs  —  public entry point
// ─────────────────────────────────────────────────────────────────────────────

static void build_bundle_entry_chunks(
    std::string_view json_payload,
    std::vector<simdjson::padded_string>& chunk_storage)
{
    simdjson::ondemand::parser splitter;
    simdjson::padded_string padded(json_payload.data(), json_payload.size());
    simdjson::ondemand::document doc = splitter.iterate(padded);
    std::string_view rtype;
    if (doc["resourceType"].get_string().get(rtype) == simdjson::SUCCESS &&
        rtype == "Bundle") {
        simdjson::ondemand::array entry_arr;
        if (doc["entry"].get_array().get(entry_arr) == simdjson::SUCCESS) {
            for (auto item : entry_arr) {
                std::string_view raw;
                if (item.raw_json().get(raw) == simdjson::SUCCESS)
                    chunk_storage.emplace_back(raw.data(), raw.size());
            }
        }
    } else {
        // Non-Bundle ingestion still uses prechunked mode: one chunk = full payload.
        chunk_storage.emplace_back(json_payload.data(), json_payload.size());
    }
}

FF_UrlInternState FF_PredigestExtensionURLs(
    const std::vector<simdjson::padded_string>& prechunked_entries,
    Builder&               builder,
    FF_ExtensionFilterMode mode)
{
    FF_UrlInternState state;
    const Memory& mem = builder.memory();
    uint8_t*      base = mem.base();

    // Chunks are required and owned by the caller for the full predigest call.
    if (prechunked_entries.empty()) return state;
    const size_t num_chunks = prechunked_entries.size();

    // ── Phase 2 + 3: Pipeline — producers scan, consumer builds trie ─────────
    // The consumer thread starts *before* the producers to begin draining as
    // soon as the first batch arrives.  Producers push UrlBatch items to the
    // shared MPSC queue; the consumer pops and inserts into the radix trie.

    UrlBatchQueue       batch_queue;
    std::atomic<bool>   producers_done{false};
    ConsumerState       cs(state, mem, mode);

    // Acquire Consumer before any Injectors (gets head node reference).
    auto consumer_handle = batch_queue.get_consumer();

    std::thread consumer_thread(
        [c = std::move(consumer_handle), &producers_done, &cs]() mutable {
            UrlBatch batch;
            while (true) {
                if (c.pop(batch)) {
                    consumer_process_batch(cs, batch);
                    continue;
                }
                if (producers_done.load(std::memory_order_acquire)) {
                    // Producers done: drain any remaining in-flight batches.
                    while (c.pop(batch)) consumer_process_batch(cs, batch);
                    break;
                }
                std::this_thread::yield();
            }
        });

    // Producer threads: each scans its assigned chunks.
    const unsigned nthreads = std::min(
        static_cast<unsigned>(num_chunks),
        std::max(1u, std::thread::hardware_concurrency()));

    {
        std::vector<std::thread> workers;
        workers.reserve(nthreads);
        for (unsigned t = 0; t < nthreads; ++t) {
            workers.emplace_back([&, t]() {
                ProducerCtx ctx(batch_queue);
                for (size_t i = t; i < num_chunks; i += nthreads)
                    scan_chunk_producer(prechunked_entries[i], ctx);
                ctx.flush(); // push partial tail batch
            });
        }
        for (auto& w : workers) w.join();
    }

    // Signal consumer that no more batches will arrive.
    producers_done.store(true, std::memory_order_release);
    consumer_thread.join();

    // ── Phase 4: Early-exit if no URLs were interned ──────────────────────────
    if (cs.entries.empty()) return state;

    // ── Phase 5: Write FF_URL_DIRECTORY block ─────────────────────────────────
    const uint32_t n_entries = static_cast<uint32_t>(cs.entries.size());
    const Size dir_total = FF_URL_DIRECTORY::HEADER_SIZE +
                           static_cast<Size>(n_entries) * FF_URL_DIRECTORY::URL_ENTRY_SIZE;
    const Offset dir_off = mem.claim_space(dir_total);
    BYTE* dir_ptr = base + dir_off;

    STORE_U64(dir_ptr + FF_URL_DIRECTORY::VALIDATION,  dir_off);
    STORE_U16(dir_ptr + FF_URL_DIRECTORY::RECOVERY,    RECOVER_FF_URL_DIRECTORY);
    STORE_U16(dir_ptr + FF_URL_DIRECTORY::PAD,         0);
    STORE_U32(dir_ptr + FF_URL_DIRECTORY::ENTRY_COUNT, n_entries);

    BYTE* et = dir_ptr + FF_URL_DIRECTORY::HEADER_SIZE;
    for (uint32_t i = 0; i < n_entries; ++i) {
        BYTE* ep = et + static_cast<Size>(i) * FF_URL_DIRECTORY::URL_ENTRY_SIZE;
        STORE_U32(ep + FF_URL_DIRECTORY::URL_ENTRY_PRIOR_IDX,  cs.entries[i].prior);
        STORE_U32(ep + FF_URL_DIRECTORY::URL_ENTRY_PAD,        0);
        STORE_U64(ep + FF_URL_DIRECTORY::URL_ENTRY_SEG_OFFSET, cs.entries[i].seg_off);
    }

    // ── Phase 6: Record URL directory offset in builder ───────────────────────
    STORE_U64(base + FF_HEADER::URL_DIR_OFFSET, dir_off);
    builder.set_url_dir_offset(dir_off);

    // ── Phase 7: Promote cached URLs to MODULE_IDX (EXT_REF MSB=1) ───────────
#ifdef FASTFHIR_ENABLE_EXTENSIONS
    {
        auto& host = FF_WasmExtensionHost::instance();
        struct Pending { uint32_t url_idx; std::string_view url; };
        std::vector<Pending> active;
        std::vector<Pending> pending;

        for (auto& [url, ref] : state.url_to_index) {
            if (ref == FF_NULL_UINT32) continue;
            const uint32_t url_idx = ff_ext_ref_index(ref);
            if (host.has_module_cached(url)) {
                active.push_back({url_idx, url});
            } else {
                pending.push_back({url_idx, url});
            }
        }

        if (!active.empty()) {
            std::sort(active.begin(), active.end(),
                      [](const Pending& a, const Pending& b){ return a.url_idx < b.url_idx; });
            std::vector<std::pair<uint32_t, std::string>> reg_entries;
            reg_entries.reserve(active.size());
            for (uint32_t module_idx = 0; module_idx < active.size(); ++module_idx) {
                const auto& a = active[module_idx];
                state.url_to_index[a.url] = ff_make_module_ref(module_idx);
                reg_entries.emplace_back(a.url_idx, std::string(a.url));
                state.passive_payload.erase(a.url);
            }
            host.write_module_registry(builder, reg_entries);
        }

        for (auto& p : pending) host.enqueue_resolve(std::string(p.url));
    }
#endif

    return state;
}


// =====================================================================
// INGEST WORK QUEUE
// Complex field values (blocks and arrays) are enqueued rather than
// dispatched inline, so the calling frame stays shallow and the queue
// is drained iteratively after the initial parse.
// =====================================================================

enum class IngestPendingKind {
    BlockField,   // FF_FIELD_BLOCK  → dispatch_block → parent[key] = child
    ArrayField,   // FF_FIELD_ARRAY  → dispatch_block(synthetic owner) → extract array → parent[key] = array
};

struct IngestPending {
    IngestPendingKind          kind     = IngestPendingKind::BlockField;
    Reflective::ObjectHandle   parent;                            // target parent object
    FF_FieldKey                key;                               // field within parent to write
    std::string                payload;                           // owned JSON for this value
    RECOVERY_TAG               recovery = FF_RECOVER_UNDEFINED;  // dispatch recovery tag
};

using IngestQueue = FIFO::Queue<IngestPending, 256>;

// =====================================================================
// BUNDLE WORK QUEUE
// Each bundle entry is represented as a lightweight view-plus-index task.
// The queue is fully populated before any worker starts (all slots are
// PENDING), so workers never spin waiting for items to be written.
// Workers each hold an independent Consumer; pop() uses a CAS on every
// slot so exactly one thread claims each task — no coordinator needed.
// =====================================================================
struct BundleTask {
    size_t index; // position within the preallocated inline entry array
};

// CAPACITY=64 → up to 64 concurrent live nodes × 2000 entries/node = 128 000 entries.
using BundleQueue = FIFO::Queue<BundleTask, 64>;

struct IngestContext {
    Builder&                      builder;
    simdjson::ondemand::parser&   parser;
    IngestQueue                   queue;
    IngestQueue::Injector         injector;
    IngestQueue::Consumer         consumer;
    ConcurrentLogger*             logger;

    IngestContext(Builder& b, simdjson::ondemand::parser& p, ConcurrentLogger* lg)
        : builder(b), parser(p), queue(),
          injector(queue.get_injector()), consumer(queue.get_consumer()), logger(lg) {}
};

static void enqueue_ingest_pending(IngestContext& ctx, IngestPending item) {
    ctx.injector.push(item);
}

static FF_Result process_ingest_pending(IngestPending& pending, IngestContext& ctx) {
    simdjson::padded_string safe_json(pending.payload);
    try {
        if (pending.kind == IngestPendingKind::BlockField) {
            simdjson::ondemand::document doc = ctx.parser.iterate(safe_json);
            simdjson::ondemand::value    val = doc.get_value();
            Reflective::ObjectHandle child =
                dispatch_block(pending.recovery, val, ctx.builder, ctx.logger);
            if (child.offset() == FF_NULL_OFFSET)
                return FF_Result{FF_FAILURE,
                    std::string("IngestPending: failed to build block '") + pending.key.name + "'."};
            pending.parent[pending.key] = child;

        } else /* ArrayField */ {
            // INLINE OBJECT ARRAYS ONLY.
            // FastFHIR never uses offset-arrays for object fields. Every element is
            // written contiguously into the arena as an inline block at the time the
            // generated *_from_json() function runs. The resulting layout is:
            //
            //   [ FF_ARRAY header | elem[0] block | elem[1] block | ... ]
            //
            // We call dispatch_block() with the owner_recovery tag, which resolves to
            // the generated function for the *parent* type (e.g. Patient for Patient.telecom).
            // That function writes the full owner object including the packed array block
            // into the arena in one call. We then read back the array offset from the
            // owner's vtable slot and hand a typed ObjectHandle to the MutableEntry
            // assignment, which patches the pointer in the real parent.
            simdjson::ondemand::document doc  = ctx.parser.iterate(safe_json);
            simdjson::ondemand::value    val  = doc.get_value();
            Reflective::ObjectHandle owner =
                dispatch_block(pending.recovery, val, ctx.builder, ctx.logger);
            if (owner.offset() == FF_NULL_OFFSET)
                return FF_Result{FF_FAILURE,
                    std::string("IngestPending: failed to build array owner for field '") + pending.key.name + "'."};

            // Read the array block offset from the owner's vtable slot for this field.
            // key.field_offset is the byte distance from the owner block base to the
            // 8-byte pointer slot that holds the FF_ARRAY block offset.
            const BYTE* base      = ctx.builder.memory().base();
            Offset      slot_addr = owner.offset() + pending.key.field_offset;
            Offset      array_off = LOAD_U64(base + slot_addr);
            if (array_off == FF_NULL_OFFSET)
                return FF_Result{FF_FAILURE,
                    std::string("IngestPending: array payload parsed to null for field '") + pending.key.name + "'."};

            // Confirm the block at array_off is a genuine FF_ARRAY (RECOVER_ARRAY_BIT set).
            // This catches any mismatch between owner_recovery and the actual field.
            RECOVERY_TAG stored_tag = static_cast<RECOVERY_TAG>(
                LOAD_U16(base + array_off + DATA_BLOCK::RECOVERY));
            if ((stored_tag & RECOVER_ARRAY_BIT) == 0)
                return FF_Result{FF_FAILURE,
                    std::string("IngestPending: payload is not an FF_ARRAY block for field '") + pending.key.name + "'."};

            // Wrap the existing inline array block in an ObjectHandle using the
            // semantic element recovery tag (child_recovery), then assign it to
            // the parent's field slot via MutableEntry::operator=(ObjectHandle).
            Reflective::ObjectHandle array_handle(&ctx.builder, array_off, pending.key.child_recovery);
            pending.parent[pending.key] = array_handle;
        }
    } catch (const simdjson::simdjson_error& e) {
        return FF_Result{FF_FAILURE,
            std::string("IngestPending simdjson error for field '") + pending.key.name + "': " + e.what()};
    } catch (const std::exception& e) {
        return FF_Result{FF_FAILURE,
            std::string("IngestPending error for field '") + pending.key.name + "': " + e.what()};
    }
    return FF_SUCCESS;
}

FF_Result Ingestor::ingest(const IngestRequest& request, Reflective::ObjectHandle& out_root, size_t& out_parsed_count)
{
    switch (request.source_type) {
        case SourceType::FHIR_JSON:
            return ingest_fhir_json(request, out_root, out_parsed_count);
        case SourceType::HL7_V2:
            return FF_Result{FF_FAILURE, "HL7 v2 ingestion not implemented."};
        case SourceType::HL7_V3:
            return FF_Result{FF_FAILURE, "HL7 v3 ingestion not implemented."};
        default:
            return FF_Result{FF_FAILURE, "Unknown source type."};
    }
}

FF_Result Ingestor::insert_at_field(Reflective::ObjectHandle& parent_object, const FF_FieldKey& key, std::string_view payload, SourceType fmt)
{
    switch (fmt) {
        case SourceType::FHIR_JSON:
            return insert_at_field_json(parent_object, key, payload);
        case SourceType::HL7_V2:
            return FF_Result{FF_FAILURE, "HL7 v2 field ingestion not implemented."};
        case SourceType::HL7_V3:
            return FF_Result{FF_FAILURE, "HL7 v3 field ingestion not implemented."};
        default:
            return FF_Result{FF_FAILURE, "Unknown source type."};
    }
} 

FF_Result Ingestor::ingest_fhir_json(const IngestRequest& request, Reflective::ObjectHandle& out_root, size_t& out_parsed_count) {
    if (is_faulted()) return FF_Result{FF_FAILURE, "Engine is faulted. Reset() before ingesting new data."};

    try {
        auto& m_builder = request.builder;
        m_logger.log("[Info] FastFHIR: Allocating simdjson tape...");

        // Create a local parser instance for the main thread to handle the initial routing and metadata extraction
        auto& parser = m_parser_pool[0];

        // Parse the root JSON object to determine if it's a Bundle or a single resource
        simdjson::ondemand::document doc = parser.iterate(
            request.json_string.data(), 
            request.json_string.size(), 
            request.json_string.size() + simdjson::SIMDJSON_PADDING
        );
        simdjson::ondemand::object root_obj = doc.get_object();
        std::string_view root_type;
        if (root_obj["resourceType"].get_string().get(root_type) != simdjson::SUCCESS) {
            return FF_Result{FF_FAILURE, "Invalid FHIR JSON: Missing resourceType at root."};
        }

        std::vector<simdjson::padded_string> entry_chunks;
        build_bundle_entry_chunks(
            std::string_view(request.json_string.data(), request.json_string.size()),
            entry_chunks);

        // =====================================================================
        // PREDIGESTION: scan extension URLs and build FF_URL_DIRECTORY
        // Must run before any resource data is written to the arena.
        // Reuse prebuilt bundle entry chunks when available.
        // =====================================================================
        FF_UrlInternState intern_state = FF_PredigestExtensionURLs(
            entry_chunks,
            m_builder,
            FF_ExtensionFilterMode::FILTER_ALL_KNOWN);

        // Make intern_state available to all ingest workers via the Builder.
        m_builder.set_intern_state(&intern_state);

        // =====================================================================
        // FAST PATH: SINGLE RESOURCE (Non-Bundle)
        // =====================================================================
        if (root_type != "Bundle") {
            m_logger.log(std::string("[Info] FastFHIR: Ingesting single ") + std::string(root_type) + " resource...");
            
            out_root = dispatch_resource(root_type, root_obj, m_builder, &m_logger);
            m_builder.set_intern_state(nullptr);
            if (out_root.offset() == FF_NULL_OFFSET) 
                return FF_Result{FF_FAILURE, "Failed to parse root resource."};
            out_parsed_count = 1;
            
            return FF_SUCCESS;
        }

        // =====================================================================
        // CONCURRENT PATH: BUNDLE (Top-Down)
        // =====================================================================
        m_logger.log("[Info] FastFHIR: Allocating Top-Down Bundle structure...");
        root_obj.reset(); 
        std::vector<std::string_view> task_payloads;

        // =====================================================================
        // 1. EXTRACT DATA & QUEUE TASKS
        // =====================================================================
        // This single line parses all metadata AND slices the entry array into our task vector.
        BundleData pre_bundle = Bundle_from_json(root_obj, &m_logger, &task_payloads);
        
        // =====================================================================
        // 2. PREPARE THE PREALLOCATED ARRAY
        // =====================================================================
        // INLINE OBJECTS: Bundle.entry elements are stored inline — not as an
        // array of offsets pointing to heap-allocated objects. Each BundleentryData
        // is a fixed-size struct; by pre-populating N default-constructed entries
        // here, the Builder's append_obj() call below writes a single contiguous
        // arena region:
        //
        //   [ FF_ARRAY header | BundleentryData[0] | BundleentryData[1] | ... ]
        //
        // Worker threads then patch fields *within* each already-allocated slot
        // using MutableEntry assignments — no secondary allocation or pointer
        // chasing for the array elements themselves.
        size_t count = task_payloads.size();
        if (entry_chunks.size() != count) {
            return FF_Result{FF_FAILURE, "Bundle chunk/task count mismatch."};
        }
        pre_bundle.entry = std::vector<BundleentryData>(count);

        // =====================================================================
        // 3. WRITE THE PREALLOCATED BUNDLE TOP-DOWN
        // =====================================================================
        // Writes strings, the Bundle header block, and the fully-packed inline
        // entry array block to the arena in one append_obj() call.
        Reflective::ObjectHandle root_handle = m_builder.append_obj(pre_bundle);
        Offset bundle_offset = root_handle.offset();
        (void)bundle_offset;

        // =====================================================================
        // 4. LOCATE THE PREALLOCATED ARRAY BLOCK FOR PATCHING
        // =====================================================================
        // entry_array is an ObjectHandle pointing at the inline FF_ARRAY block.
        // Indexing it with operator[](size_t) returns a MutableEntry whose
        // parent_offset is the array block itself and whose vtable_offset is the
        // byte position of that element within the contiguous block.
        Reflective::ObjectHandle entry_array = root_handle[Fields::BUNDLE::ENTRY];

        // =====================================================================
        // 5. CONCURRENT ARRAY PATCHING — SHARED QUEUE / DYNAMIC LOAD BALANCING
        // =====================================================================
        // All N entry tasks are pushed into a single BundleQueue before any
        // worker is spawned. Workers race to pop() tasks via an atomic CAS on
        // every queue slot: the first thread to successfully CAS a slot from
        // PENDING → READING owns that entry; all other threads skip it and
        // advance to the next slot. This gives perfect dynamic load balancing
        // — faster threads naturally pull more work with zero coordination
        // overhead, and no thread ever idles while tasks remain in the queue.
        out_parsed_count = count;

        // Push all tasks before spawning workers so every slot is PENDING
        // (not WRITING) when consumers start, eliminating any spin-wait.
        BundleQueue bundle_queue;
        {
            auto injector = bundle_queue.get_injector();
            for (size_t i = 0; i < count; ++i) {
                injector.push(BundleTask{i});
            }
            // Injector destructs here; all slots are now PENDING.
        }

        std::vector<std::thread> workers;
        auto* shared_chunks = &entry_chunks;
        for (unsigned int i = 0; i < m_num_threads; ++i) {
            workers.emplace_back([this, i, &bundle_queue, shared_chunks, entry_array, &m_builder]() mutable {
                auto& local_parser = m_parser_pool[i];

                // Each worker gets its own Consumer starting at the queue head.
                // pop() uses CAS so no two workers ever claim the same task.
                // Faster workers simply pop more entries — no static partition.
                auto consumer = bundle_queue.get_consumer();
                BundleTask task;

                while (!m_engine_faulted.load(std::memory_order_relaxed)) {
                    if (consumer.pop(task)) {
                        try {
                            // Locate the pre-allocated inline arena slot for this entry.
                            Reflective::MutableEntry entry_wrapper = entry_array[task.index];

                            // All tasks map 1:1 to prechunked payloads.
                            simdjson::ondemand::document local_doc = local_parser.iterate((*shared_chunks)[task.index]);
                            simdjson::ondemand::object local_obj = local_doc.get_object();
                            Ingest::patch_Bundle_entry_from_json(local_obj, entry_wrapper, m_builder, &m_logger);

                        } catch (const simdjson::simdjson_error& e) {
                            m_engine_faulted.store(true, std::memory_order_release);
                            m_logger.log(std::string("[Fatal] Worker thread crashed on bundle entry ") +
                                         std::to_string(task.index) + " (simdjson): " + e.what());
                            break;
                        } catch (const std::exception& e) {
                            m_engine_faulted.store(true, std::memory_order_release);
                            m_logger.log(std::string("[Fatal] Worker thread crashed on bundle entry ") +
                                         std::to_string(task.index) + " (std::exception): " + e.what());
                            break;
                        } catch (...) {
                            m_engine_faulted.store(true, std::memory_order_release);
                            m_logger.log(std::string("[Fatal] Worker thread crashed on bundle entry ") +
                                         std::to_string(task.index) + " with unknown exception.");
                            break;
                        }
                        continue;
                    }
                    // Queue exhausted — all entries claimed by this or other workers.
                    if (consumer.at_end()) break;
                    // pop() returned false but queue not yet exhausted: another
                    // thread holds the next slot mid-claim. Spin briefly.
                }
            });
        }

        for (auto& worker : workers) worker.join();

        // Check if the engine faulted during the worker runs
        if (m_engine_faulted.load(std::memory_order_acquire)) {
            return FF_Result{FF_FAILURE, "Ingestion aborted due to worker thread crash. Check ingestor engine logs for error details."};
        }

        // =====================================================================
        // 6. Return Root
        // =====================================================================
        out_root = root_handle;
        m_builder.set_intern_state(nullptr);
        return FF_SUCCESS;

    } catch (const simdjson::simdjson_error& e) {
        request.builder.set_intern_state(nullptr);
        return FF_Result{FF_FAILURE, std::string("simdjson Exception: ") + e.what()};
    } catch (const std::exception& e) {
        request.builder.set_intern_state(nullptr);
        return FF_Result{FF_FAILURE, std::string("Standard Exception: ") + e.what()};
    }
}


// =====================================================================
// FIELD-LEVEL JSON ENGINE
// =====================================================================
FF_Result Ingestor::insert_at_field_json(Reflective::ObjectHandle& parent_object, const FF_FieldKey& key, std::string_view payload) {
    if (is_faulted()) return FF_Result{FF_FAILURE, "Engine is faulted. Reset() before ingesting new data."};

    // 1. Prevent unsupported writes up front.
    if (key.kind == FF_FIELD_ARRAY) {
        if (key.owner_recovery == FF_RECOVER_UNDEFINED) {
            return FF_Result{FF_FAILURE,
                std::string("insert_at_field array target '") + key.name +
                "' requires a typed FF_FieldKey (owner recovery is undefined)."};
        }
        if (key.array_entries_are_offsets != 0) {
            return FF_Result{FF_FAILURE,
                std::string("insert_at_field does not support offset-array field '") + key.name +
                "' yet. Only inline-block arrays are supported."};
        }
    }
    if (key.kind != FF_FIELD_BLOCK) {
        if (key.kind == FF_FIELD_ARRAY) {
            // Handled below.
        } else {
        return FF_Result{FF_FAILURE,
            "insert_at_field currently supports only object/block fields. Use direct assignment for scalars like " +
            std::string(key.name)};
        }
    }

    // 2. Build an IngestContext and enqueue the work item rather than dispatching inline.
    //    The parent slot is recorded, and the actual JSON parse + builder write happens 
    //    during the queue drain below.
    IngestContext ctx(*parent_object.get_builder(), m_parser_pool[0], &m_logger);

    if (key.kind == FF_FIELD_ARRAY) {
        // Build the synthetic owner JSON that wraps the array payload.
        bool is_array_payload = false;
        for (char ch : payload) {
            if (!std::isspace(static_cast<unsigned char>(ch))) {
                is_array_payload = (ch == '[');
                break;
            }
        }

        std::string owner_json;
        owner_json.reserve(key.name_len + payload.size() + 16);
        owner_json += "{\"";
        owner_json.append(key.name, key.name_len);
        owner_json += "\":";
        if (is_array_payload) {
            owner_json.append(payload.data(), payload.size());
        } else {
            owner_json += "[";
            owner_json.append(payload.data(), payload.size());
            owner_json += "]";
        }
        owner_json += "}";

        enqueue_ingest_pending(ctx, IngestPending{
            IngestPendingKind::ArrayField,
            parent_object,
            key,
            std::move(owner_json),
            key.owner_recovery,
        });
    } else {
        // Block field path.
        enqueue_ingest_pending(ctx, IngestPending{
            IngestPendingKind::BlockField,
            parent_object,
            key,
            std::string(payload),
            key.child_recovery,
        });
    }

    // 3. Drain the queue
    //    Each item records the MutableEntry slot (parent + key) and is updated
    //    once its JSON is parsed and the child block written to the arena.
    IngestPending pending;
    while (true) {
        if (ctx.consumer.pop(pending)) {
            FF_Result result = process_ingest_pending(pending, ctx);
            if (!result) return result;
            continue;
        }
        if (ctx.consumer.at_end()) break;
    }

    return FF_SUCCESS;
}
} // namespace FastFHIR::Ingest
