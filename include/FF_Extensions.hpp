/**
 * @file FF_Extensions.hpp
 * @brief WAMR host for FastFHIR WASM extension codec modules.
 *
 * A WASM extension module is a drop-in vtable codec for one specific FHIR
 * extension URL.  It exports three functions that mirror the generated C++
 * codec triple for native resource types:
 *
 *   ext_size  (uint32_t staging_ptr, uint32_t version) -> uint32_t
 *   ext_encode(uint32_t staging_ptr, uint32_t version) -> uint32_t
 *   ext_decode(uint32_t staging_ptr, uint32_t version) -> uint32_t
 *
 * The host serialises/deserialises through a 64 KiB per-thread staging buffer
 * that lives inside the guest's WAMR linear memory.  The FastFHIR arena is
 * never aliased into guest address space.  No host imports are required.
 *
 * This header is only compiled when FASTFHIR_ENABLE_EXTENSIONS is ON.
 */
#pragma once

#ifdef FASTFHIR_ENABLE_EXTENSIONS

#include "FF_Primitives.hpp"
#include "FF_Builder.hpp"
#include <cstdint>
#include <string_view>
#include <memory>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>
#include <utility>

// WAMR public API
#include <wasm_export.h>

namespace FastFHIR::Extensions {

// =====================================================================
// STAGING BUFFER SIZE
// =====================================================================
// One 64 KiB window per module instance lives inside the guest's linear
// memory.  Sufficient for any single extension vtable block.
static constexpr uint32_t FF_STAGING_BUFFER_SIZE = 64u * 1024u;

// =====================================================================
// EXTENSION DATA
// =====================================================================
// Opaque staging-buffer-relative payload shared between host and guest.
// The host lays out the source ExtensionData fields at staging_ptr before
// each ext_encode call; the guest reads them and writes the encoded vtable
// bytes back in-place.  For ext_decode the roles are reversed.
//
// The concrete layout is defined by the ffc.py --wasm emitter for each
// extension URL; this header only exposes the raw byte interface.
struct FF_ExtensionStagingLayout {
    uint8_t* ptr  = nullptr;  // host-side pointer into guest linear memory
    uint32_t addr = 0;        // guest-relative uint32_t address
    uint32_t size = FF_STAGING_BUFFER_SIZE;
};

// =====================================================================
// WASM EXTENSION MODULE INSTANCE
// =====================================================================
// One FF_WasmModuleInstance per (extension URL, thread).
// Lifetime is tied to the FF_WasmExtensionHost that owns it.
struct FF_WasmModuleInstance {
    wasm_module_t           module   = nullptr;
    wasm_module_inst_t      instance = nullptr;
    FF_ExtensionStagingLayout staging;

    // Resolved guest function handles (filled once at instantiation time).
    wasm_function_inst_t fn_size   = nullptr;
    wasm_function_inst_t fn_encode = nullptr;
    wasm_function_inst_t fn_decode = nullptr;

    ~FF_WasmModuleInstance();
};

// =====================================================================
// HOST CODEC WRAPPERS
// =====================================================================
// These are called from the same sites that today call the generated
// SIZE_FF_* / STORE_FF_* / FF_DESERIALIZE_* for native resource types.
//
// @param inst   Fully initialised module instance for this thread.
// @param base   FastFHIR arena base pointer.
// @param version FHIR version constant (FHIR_VERSION_R4 / R5).

/**
 * Mirror of SIZE_FF_<TYPE>: returns the byte size the encoded vtable will
 * occupy in the FastFHIR arena.  The host serialises @p staging_src into
 * the staging buffer, then calls the guest's ext_size export.
 */
Size FF_WasmExtensionSize(
    FF_WasmModuleInstance& inst,
    const uint8_t*         staging_src,
    uint32_t               staging_src_len,
    uint32_t               version);

/**
 * Mirror of STORE_FF_<TYPE>: encodes @p staging_src into the FastFHIR
 * arena at @p hdr_off.  Returns the next free Offset (hdr_off + encoded size).
 */
Offset FF_WasmExtensionStore(
    FF_WasmModuleInstance& inst,
    BYTE*                  arena_base,
    Offset                 hdr_off,
    const uint8_t*         staging_src,
    uint32_t               staging_src_len,
    uint32_t               version);

/**
 * Mirror of FF_DESERIALIZE_<TYPE>: decodes a vtable block from the FastFHIR
 * arena back into the staging buffer.  @p out_buf receives the decoded payload
 * bytes; its length must be at least FF_STAGING_BUFFER_SIZE bytes.
 * Returns the number of payload bytes written to @p out_buf.
 */
uint32_t FF_WasmExtensionDeserialize(
    FF_WasmModuleInstance& inst,
    const BYTE*            arena_base,
    Offset                 vtable_off,
    Size                   vtable_size,
    uint32_t               version,
    uint8_t*               out_buf,
    uint32_t               out_buf_len);

// =====================================================================
// EXTENSION HOST ENGINE
// =====================================================================
// Singleton-per-process.  Call FF_WasmExtensionHost::get() to acquire.
class FF_WasmExtensionHost {
public:
    /**
     * Returns the process-wide host, initialising WAMR exactly once.
     * Thread-safe; uses std::call_once.
     */
    static FF_WasmExtensionHost& get();

    /**
     * Register a .wasm module binary for @p extension_url.
     * The bytes are AOT-compiled and stored; the original buffer need not
     * outlive this call.  Calling twice for the same URL replaces the
     * previous registration.
     *
     * Thread-safe: protected by m_mutex.
     */
    bool register_module(std::string_view extension_url,
                         const uint8_t*   wasm_bytes,
                         uint32_t         wasm_byte_len);

    /**
     * Returns true if @p extension_url has a registered module.
     */
    bool has_module(std::string_view extension_url) const;

    /**
     * Acquire a per-thread module instance for @p extension_url.
     * Returns nullptr if the URL is not registered.
     *
     * The returned pointer is valid until the next call to
     * release_instance() on this thread or until the host is destroyed.
     */
    FF_WasmModuleInstance* acquire_instance(std::string_view extension_url);

    /**
     * Return a module instance to the per-thread pool after use.
     */
    void release_instance(std::string_view extension_url,
                          FF_WasmModuleInstance* inst);

    /**
     * Attempt to resolve and load a WASM codec for @p extension_url from:
     *   1. In-memory cache (already registered via register_module).
     *   2. Disk cache (~/.cache/fastfhir/modules/<hex_sha256>.wasm).
     *   3. Remote registry (https://registry.fastfhir.org/v1/modules/<hex_sha256>.wasm).
     *
     * On success, the module is registered and true is returned.
     * On failure (offline / invalid), logs a warning and returns false.
     * The caller should fall back to storing the raw FF_EXTENSION block.
     *
     * Thread-safe.
     */
    bool resolve_or_fetch_module(std::string_view extension_url);

    /**
     * Write an FF_MODULE_REGISTRY block for the active modules referenced by
     * @p ordered_entries.  The vector position is the MODULE_IDX written into
     * each EXT_REF (MSB=1).  Each pair is (url_idx, url) — url_idx is the
     * back-pointer to FF_URL_DIRECTORY for round-trip export.
     * Back-patches FF_HEADER::MODULE_REG_OFFSET.
     */
    void write_module_registry(
        Builder& builder,
        const std::vector<std::pair<uint32_t, std::string>>& ordered_entries);

    /**
     * Returns true if @p extension_url currently has a compiled module in
     * the in-memory cache.  Non-blocking; equivalent to has_module().
     */
    bool has_module_cached(std::string_view extension_url) const { return has_module(extension_url); }

    /**
     * Enqueue an asynchronous resolve+AOT-compile job for @p extension_url.
     * Non-blocking.  The background worker will populate the in-memory and
     * on-disk caches; subsequent ingestions of the same URL will take Path A.
     * Idempotent: enqueueing the same URL twice is a no-op.
     */
    void enqueue_resolve(std::string extension_url);

    /**
     * Provides a single-process singleton accessor (alias for get()).
     */
    static FF_WasmExtensionHost& instance() { return get(); }

    ~FF_WasmExtensionHost();

    // Non-copyable, non-movable singleton.
    FF_WasmExtensionHost(const FF_WasmExtensionHost&) = delete;
    FF_WasmExtensionHost& operator=(const FF_WasmExtensionHost&) = delete;

private:
    FF_WasmExtensionHost();

    struct CompiledModule {
        wasm_module_t           module      = nullptr;
        // SHA-256 of the raw .wasm bytes — canonical version identity.
        // Content-addressed: changes when and only when the binary changes.
        // Stored in FF_MODULE_REGISTRY so binary files are self-describing.
        std::array<uint8_t, 32> content_hash = {};
        bool                    hash_known  = false;
        // Epoch-seconds when this module was last fetched from the registry.
        // Used by the AOT worker to enforce the on-disk TTL.
        int64_t                 fetched_at  = 0;
    };

    mutable std::shared_mutex                              m_mutex;
    std::unordered_map<std::string, CompiledModule>        m_modules;
    bool                                                   m_wamr_initialised = false;

    // ── Async AOT worker ──────────────────────────────────────────────
    // A single background thread drains m_resolve_queue, fetching and
    // AOT-compiling modules so that subsequent stream ingestions can take
    // the active (Path A) WASM dispatch path.
    std::thread                          m_aot_worker;
    std::mutex                           m_queue_mutex;
    std::condition_variable              m_queue_cv;
    std::deque<std::string>              m_resolve_queue;
    std::unordered_set<std::string>      m_in_flight;     // de-duplication
    bool                                 m_aot_stop = false;
    void aot_worker_loop();

    // Creates and initialises a fresh module instance + staging buffer.
    std::unique_ptr<FF_WasmModuleInstance> make_instance(
        const CompiledModule& compiled) const;
};

} // namespace FastFHIR::Extensions

#endif // FASTFHIR_ENABLE_EXTENSIONS
