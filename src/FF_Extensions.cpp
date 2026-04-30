// ============================================================
// FF_Extensions.cpp
// WAMR host implementation for FastFHIR WASM codec modules.
// ============================================================
#ifdef FASTFHIR_ENABLE_EXTENSIONS

#include "../include/FF_Extensions.hpp"
#include <cstring>
#include <mutex>
#include <cassert>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
// OpenSSL SHA-256 (available because the ingestor already links it)
#include <openssl/sha.h>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <arpa/inet.h>
#endif

namespace FastFHIR::Extensions {

// =====================================================================
// WAMR RUNTIME LIFECYCLE
// =====================================================================

static std::once_flag g_wamr_init_flag;

static void init_wamr_runtime() {
    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    // Use system allocator; WAMR manages its own heap.
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    bool ok = wasm_runtime_full_init(&init_args);
    (void)ok;
    assert(ok && "WAMR runtime initialisation failed");
}

// =====================================================================
// FF_WasmModuleInstance
// =====================================================================

FF_WasmModuleInstance::~FF_WasmModuleInstance() {
    if (instance) {
        wasm_runtime_deinstantiate(instance);
        instance = nullptr;
    }
    // module is owned by the host engine; do not destroy here.
}

// =====================================================================
// FF_WasmExtensionHost
// =====================================================================

FF_WasmExtensionHost::FF_WasmExtensionHost() {
    std::call_once(g_wamr_init_flag, init_wamr_runtime);
    m_wamr_initialised = true;
    m_aot_worker = std::thread([this](){ aot_worker_loop(); });
}

FF_WasmExtensionHost::~FF_WasmExtensionHost() {
    // Stop the AOT worker.
    {
        std::lock_guard lk(m_queue_mutex);
        m_aot_stop = true;
    }
    m_queue_cv.notify_all();
    if (m_aot_worker.joinable()) m_aot_worker.join();

    // Unload compiled modules.
    {
        std::unique_lock lock(m_mutex);
        for (auto& [url, cm] : m_modules) {
            if (cm.module)
                wasm_runtime_unload(cm.module);
        }
        m_modules.clear();
    }
    if (m_wamr_initialised)
        wasm_runtime_destroy();
}

FF_WasmExtensionHost& FF_WasmExtensionHost::get() {
    static FF_WasmExtensionHost s_instance;
    return s_instance;
}

bool FF_WasmExtensionHost::register_module(
    std::string_view extension_url,
    const uint8_t*   wasm_bytes,
    uint32_t         wasm_byte_len)
{
    char err[128] = {};
    wasm_module_t mod = wasm_runtime_load(
        const_cast<uint8_t*>(wasm_bytes),
        wasm_byte_len,
        err,
        sizeof(err));
    if (!mod) return false;

    std::unique_lock lock(m_mutex);
    auto& slot = m_modules[std::string(extension_url)];
    if (slot.module)
        wasm_runtime_unload(slot.module);
    slot.module = mod;
    return true;
}

bool FF_WasmExtensionHost::has_module(std::string_view extension_url) const {
    std::shared_lock lock(m_mutex);
    return m_modules.count(std::string(extension_url)) > 0;
}

std::unique_ptr<FF_WasmModuleInstance> FF_WasmExtensionHost::make_instance(
    const CompiledModule& compiled) const
{
    auto inst = std::make_unique<FF_WasmModuleInstance>();
    inst->module = compiled.module;

    char err[128] = {};
    // 64 KiB stack + 64 KiB heap inside the WAMR instance is sufficient for
    // a self-contained codec with no dynamic allocation.
    inst->instance = wasm_runtime_instantiate(
        compiled.module,
        FF_STAGING_BUFFER_SIZE,   // stack size
        FF_STAGING_BUFFER_SIZE,   // heap size
        err, sizeof(err));
    if (!inst->instance) return nullptr;

    // Allocate the per-thread staging buffer inside guest linear memory.
    uint32_t staging_addr = wasm_runtime_module_malloc(
        inst->instance, FF_STAGING_BUFFER_SIZE,
        reinterpret_cast<void**>(&inst->staging.ptr));
    if (staging_addr == 0 || !inst->staging.ptr) return nullptr;

    inst->staging.addr = staging_addr;
    inst->staging.size = FF_STAGING_BUFFER_SIZE;

    // Resolve the three mandatory guest exports.
    inst->fn_size   = wasm_runtime_lookup_function(inst->instance, "ext_size",   nullptr);
    inst->fn_encode = wasm_runtime_lookup_function(inst->instance, "ext_encode", nullptr);
    inst->fn_decode = wasm_runtime_lookup_function(inst->instance, "ext_decode", nullptr);

    if (!inst->fn_size || !inst->fn_encode || !inst->fn_decode) return nullptr;

    return inst;
}

FF_WasmModuleInstance* FF_WasmExtensionHost::acquire_instance(
    std::string_view extension_url)
{
    std::shared_lock lock(m_mutex);
    auto it = m_modules.find(std::string(extension_url));
    if (it == m_modules.end()) return nullptr;
    const CompiledModule& cm = it->second;
    lock.unlock();

    auto inst = make_instance(cm);
    if (!inst) return nullptr;
    return inst.release();
}

void FF_WasmExtensionHost::release_instance(
    std::string_view /*extension_url*/,
    FF_WasmModuleInstance* inst)
{
    delete inst;
}

// =====================================================================
// HOST CODEC WRAPPERS
// =====================================================================

Size FF_WasmExtensionSize(
    FF_WasmModuleInstance& inst,
    const uint8_t*         staging_src,
    uint32_t               staging_src_len,
    uint32_t               version)
{
    // Copy source payload into guest staging buffer.
    uint32_t copy_len = std::min(staging_src_len, inst.staging.size);
    memcpy(inst.staging.ptr, staging_src, copy_len);

    // Call ext_size(staging_ptr, version) -> uint32_t
    uint32_t args[2] = { inst.staging.addr, version };
    uint32_t result  = 0;
    if (!wasm_runtime_call_wasm(
            wasm_runtime_get_exec_env(inst.instance),
            inst.fn_size,
            2, args))
        return 0;

    result = args[0];  // WAMR stores the return value in args[0] on return
    return static_cast<Size>(result);
}

Offset FF_WasmExtensionStore(
    FF_WasmModuleInstance& inst,
    BYTE*                  arena_base,
    Offset                 hdr_off,
    const uint8_t*         staging_src,
    uint32_t               staging_src_len,
    uint32_t               version)
{
    // First determine encoded size.
    Size encoded_size = FF_WasmExtensionSize(inst, staging_src, staging_src_len, version);
    if (encoded_size == 0) return hdr_off;

    // Re-copy source payload (FF_WasmExtensionSize may have clobbered staging).
    uint32_t copy_len = std::min(staging_src_len, inst.staging.size);
    memcpy(inst.staging.ptr, staging_src, copy_len);

    // Call ext_encode(staging_ptr, version) -> uint32_t (bytes written)
    uint32_t args[2] = { inst.staging.addr, version };
    if (!wasm_runtime_call_wasm(
            wasm_runtime_get_exec_env(inst.instance),
            inst.fn_encode,
            2, args))
        return hdr_off;

    uint32_t bytes_written = args[0];
    if (bytes_written == 0 || bytes_written > inst.staging.size)
        return hdr_off;

    // Blit encoded vtable bytes from staging → FastFHIR arena.
    memcpy(arena_base + hdr_off, inst.staging.ptr, bytes_written);
    return hdr_off + static_cast<Offset>(bytes_written);
}

uint32_t FF_WasmExtensionDeserialize(
    FF_WasmModuleInstance& inst,
    const BYTE*            arena_base,
    Offset                 vtable_off,
    Size                   vtable_size,
    uint32_t               version,
    uint8_t*               out_buf,
    uint32_t               out_buf_len)
{
    // Copy encoded vtable bytes from arena into staging buffer.
    uint32_t copy_len = static_cast<uint32_t>(std::min<Size>(vtable_size, inst.staging.size));
    memcpy(inst.staging.ptr, arena_base + vtable_off, copy_len);

    // Call ext_decode(staging_ptr, version) -> uint32_t (bytes consumed/written)
    uint32_t args[2] = { inst.staging.addr, version };
    if (!wasm_runtime_call_wasm(
            wasm_runtime_get_exec_env(inst.instance),
            inst.fn_decode,
            2, args))
        return 0;

    uint32_t payload_len = args[0];
    if (payload_len == 0 || payload_len > inst.staging.size)
        return 0;

    // Copy decoded payload bytes out of staging → caller's output buffer.
    uint32_t out_len = std::min(payload_len, out_buf_len);
    memcpy(out_buf, inst.staging.ptr, out_len);
    return out_len;
}

// =====================================================================
// SHA-256 HELPERS
// =====================================================================

// Hash an arbitrary byte span → raw 32-byte digest (Role 2: binary version identity).
static std::array<uint8_t, 32> sha256_bytes(const void* data, size_t len) {
    std::array<uint8_t, 32> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(data), len, digest.data());
    return digest;
}

// FNV-1a 64-bit: compact, fast, non-crypto hash for URL → cache slot key (Role 1).
// 8 bytes (16 hex chars) is more than sufficient for a URL cache with at most
// thousands of distinct extension URLs.
static uint64_t fnv1a_u64(std::string_view sv) {
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME  = 1099511628211ULL;
    uint64_t h = FNV_OFFSET;
    for (unsigned char c : sv)
        h = (h ^ c) * FNV_PRIME;
    return h;
}

// Encode a 64-bit value as a 16-character lowercase hex string.
static std::string hex_u64(uint64_t v) {
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << v;
    return ss.str();
}

// Encode a raw 32-byte digest as lowercase hex (used for binary content hashes).
static std::string hex_encode(const std::array<uint8_t, 32>& digest) {
    std::ostringstream ss;
    for (auto b : digest)
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    return ss.str();
}

// =====================================================================
// DISK CACHE PATHS
// =====================================================================

// Base directory: ~/.cache/fastfhir/modules/
static std::filesystem::path modules_cache_dir() {
    std::filesystem::path base;
#if defined(_WIN32)
    const char* appdata = std::getenv("LOCALAPPDATA");
    base = appdata ? appdata : ".";
#else
    const char* home = std::getenv("HOME");
    base = home ? home : ".";
    base /= ".cache";
#endif
    base /= "fastfhir";
    base /= "modules";
    std::filesystem::create_directories(base);
    return base;
}

// Content-addressed binary: <cache_dir>/<sha256_of_binary>.wasm
// Immutable once written — safe to cache indefinitely by content hash.
static std::filesystem::path wasm_path_for_content_hash(
    const std::array<uint8_t, 32>& content_hash)
{
    return modules_cache_dir() / (hex_encode(content_hash) + ".wasm");
}

// Sidecar metadata: <cache_dir>/meta/<fnv1a64_of_url>.meta
// Records: content_hash_hex\nfetched_epoch_seconds\n
// FNV-1a 64-bit is sufficient for this use case: it is only a filesystem slot
// key for a small URL cache, not a security-sensitive identifier.
static std::filesystem::path meta_path_for_url(std::string_view url) {
    auto dir = modules_cache_dir() / "meta";
    std::filesystem::create_directories(dir);
    return dir / (hex_u64(fnv1a_u64(url)) + ".meta");
}

struct ModuleMeta {
    std::array<uint8_t, 32> content_hash = {};
    bool                    valid        = false;
    int64_t                 fetched_at   = 0;  // epoch-seconds
};

static ModuleMeta read_module_meta(std::string_view url) {
    ModuleMeta m;
    auto path = meta_path_for_url(url);
    if (!std::filesystem::exists(path)) return m;
    std::ifstream f(path);
    std::string hash_hex, epoch_str;
    if (!std::getline(f, hash_hex) || hash_hex.size() != 64) return m;
    if (!std::getline(f, epoch_str)) return m;
    // Decode hex → bytes
    for (int i = 0; i < 32; ++i) {
        auto byte_str = hash_hex.substr(i * 2, 2);
        m.content_hash[i] = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
    }
    m.fetched_at = std::stoll(epoch_str);
    m.valid = true;
    return m;
}

static void write_module_meta(std::string_view url,
                              const std::array<uint8_t, 32>& content_hash,
                              int64_t fetched_at) {
    auto path = meta_path_for_url(url);
    std::ofstream f(path);
    f << hex_encode(content_hash) << '\n' << fetched_at << '\n';
}

// TTL for cached module manifests: 24 hours.
static constexpr int64_t FF_MODULE_CACHE_TTL_SECONDS = 24 * 60 * 60;

static int64_t now_epoch() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// =====================================================================
// MINIMAL HTTP/1.1 GET (no dependency on libcurl)
// Retrieves https://registry.fastfhir.org/v1/modules/<content_hex>.wasm
// Only used when cache miss occurs; runs synchronously on the calling thread.
// NOTE: Uses plain TCP on port 80 to a redirecting CDN; a proper
//       implementation should use TLS (mbedTLS / openssl BIO).
//       This is a best-effort offline-fallback path only.
// =====================================================================

static std::vector<uint8_t> http_get_wasm(const std::string& content_hex) {
    // registry.fastfhir.org is not yet live; return empty to trigger offline path.
    (void)content_hex;
    return {};
}

// Fetch the latest manifest for @p url_hex from the registry.
// Returns the reported content hash, or an empty string on failure.
// Format: https://registry.fastfhir.org/v1/modules/<url_hex>/latest
// Expected response body: "<content_sha256_hex>\n"
static std::string http_get_manifest(const std::string& /*url_hex*/) {
    // registry.fastfhir.org is not yet live.
    return {};
}

// =====================================================================
// register_module (internal, with content hash)
// =====================================================================

bool FF_WasmExtensionHost::register_module(
    std::string_view extension_url,
    const uint8_t*   wasm_bytes,
    uint32_t         wasm_byte_len)
{
    auto content_hash = sha256_bytes(wasm_bytes, wasm_byte_len);
    const int64_t now = now_epoch();

    char err[128] = {};
    wasm_module_t mod = wasm_runtime_load(
        const_cast<uint8_t*>(wasm_bytes),
        wasm_byte_len,
        err,
        sizeof(err));
    if (!mod) return false;

    std::unique_lock lock(m_mutex);
    auto& slot = m_modules[std::string(extension_url)];
    if (slot.module)
        wasm_runtime_unload(slot.module);
    slot.module      = mod;
    slot.content_hash = content_hash;
    slot.hash_known  = true;
    slot.fetched_at  = now;
    return true;
}

// =====================================================================
// resolve_or_fetch_module
// =====================================================================

bool FF_WasmExtensionHost::resolve_or_fetch_module(std::string_view extension_url) {
    // Fast-path: already in memory and not stale.
    {
        std::shared_lock lock(m_mutex);
        auto it = m_modules.find(std::string(extension_url));
        if (it != m_modules.end()) {
            const int64_t age = now_epoch() - it->second.fetched_at;
            if (age < FF_MODULE_CACHE_TTL_SECONDS) return true;
            // Module is stale — fall through to re-check manifest, but do not
            // unload yet; we continue serving the old binary until a new one
            // is confirmed.
        }
    }

    const std::string url_hex = hex_u64(fnv1a_u64(extension_url));

    // 1. Read sidecar metadata to get the last-known content hash + fetch time.
    ModuleMeta meta = read_module_meta(extension_url);
    const int64_t now = now_epoch();

    if (meta.valid) {
        const int64_t age = now - meta.fetched_at;
        if (age < FF_MODULE_CACHE_TTL_SECONDS) {
            // Metadata is fresh — load from content-addressed disk cache.
            auto wasm_path = wasm_path_for_content_hash(meta.content_hash);
            if (std::filesystem::exists(wasm_path)) {
                std::ifstream f(wasm_path, std::ios::binary);
                std::vector<uint8_t> bytes(
                    (std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
                if (!bytes.empty()) {
                    // Verify content integrity before loading.
                    if (sha256_bytes(bytes.data(), bytes.size()) == meta.content_hash)
                        return register_module(extension_url,
                                               bytes.data(),
                                               static_cast<uint32_t>(bytes.size()));
                    // Hash mismatch: corrupt cache file — fall through to re-fetch.
                }
            }
        }
        // Metadata stale or binary missing — check registry for a newer version.
    }

    // 2. Query manifest endpoint for the current content hash.
    std::string latest_hex = http_get_manifest(url_hex);
    std::array<uint8_t, 32> expected_hash = {};
    bool have_expected = (latest_hex.size() == 64);
    if (have_expected) {
        for (int i = 0; i < 32; ++i) {
            auto b = latest_hex.substr(i * 2, 2);
            expected_hash[i] = static_cast<uint8_t>(std::stoul(b, nullptr, 16));
        }
        // If we already have this version cached, just refresh the metadata TTL.
        auto wasm_path = wasm_path_for_content_hash(expected_hash);
        if (std::filesystem::exists(wasm_path)) {
            write_module_meta(extension_url, expected_hash, now);
            std::ifstream f(wasm_path, std::ios::binary);
            std::vector<uint8_t> bytes(
                (std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());
            if (!bytes.empty() &&
                sha256_bytes(bytes.data(), bytes.size()) == expected_hash)
                return register_module(extension_url,
                                       bytes.data(),
                                       static_cast<uint32_t>(bytes.size()));
        }
    }

    // 3. Download binary from registry by content hash.
    auto bytes = http_get_wasm(have_expected ? latest_hex : url_hex);
    if (bytes.empty()) {
        // Offline — fall back to any stale disk-cached version if we have one.
        if (meta.valid) {
            auto wasm_path = wasm_path_for_content_hash(meta.content_hash);
            if (std::filesystem::exists(wasm_path)) {
                std::ifstream f(wasm_path, std::ios::binary);
                std::vector<uint8_t> stale(
                    (std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
                if (!stale.empty() &&
                    sha256_bytes(stale.data(), stale.size()) == meta.content_hash)
                    return register_module(extension_url,
                                           stale.data(),
                                           static_cast<uint32_t>(stale.size()));
            }
        }
        return false;
    }

    // Verify the downloaded binary matches what the manifest promised.
    auto actual_hash = sha256_bytes(bytes.data(), bytes.size());
    if (have_expected && actual_hash != expected_hash) return false;

    // Persist to content-addressed disk cache + update sidecar.
    auto wasm_path = wasm_path_for_content_hash(actual_hash);
    {
        std::ofstream f(wasm_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    write_module_meta(extension_url, actual_hash, now);

    return register_module(extension_url, bytes.data(),
                           static_cast<uint32_t>(bytes.size()));
}

// =====================================================================
// write_module_registry
// =====================================================================

void FF_WasmExtensionHost::write_module_registry(
    Builder& builder,
    const std::vector<std::pair<uint32_t, std::string>>& ordered_entries)
{
    using namespace FastFHIR;
    if (ordered_entries.empty()) return;
    Memory&  mem  = builder.memory();
    uint8_t* base = mem.base();

    // Vector position == MODULE_IDX.  Each entry's url_idx is masked to its
    // lower 31 bits so that registry rows store pure URL_IDX back-pointers
    // (the EXT_REF MSB lives in the FF_EXTENSION block, not here).
    uint32_t n = static_cast<uint32_t>(ordered_entries.size());
    Size reg_total = FF_MODULE_REGISTRY::HEADER_SIZE +
                     static_cast<Size>(n) * FF_MODULE_REGISTRY::REG_ENTRY_SIZE;
    Offset reg_off = mem.claim_space(reg_total);
    uint8_t* rp    = base + reg_off;

    STORE_U64(rp + FF_MODULE_REGISTRY::VALIDATION,  reg_off);
    STORE_U16(rp + FF_MODULE_REGISTRY::RECOVERY,    RECOVER_FF_MODULE_REGISTRY);
    STORE_U16(rp + FF_MODULE_REGISTRY::PAD,         0);
    STORE_U32(rp + FF_MODULE_REGISTRY::ENTRY_COUNT, n);

    uint8_t* et = rp + FF_MODULE_REGISTRY::HEADER_SIZE;
    for (uint32_t i = 0; i < n; ++i) {
        const auto& [url_idx, url] = ordered_entries[i];

        // Look up the content hash for this module from the in-memory registry.
        std::array<uint8_t, 32> content_hash{};
        {
            std::shared_lock lock(m_mutex);
            auto it = m_modules.find(url);
            if (it != m_modules.end() && it->second.hash_known)
                content_hash = it->second.content_hash;
        }

        Offset   blob_off  = FF_NULL_OFFSET;
        uint32_t blob_size = 0;

        uint8_t* ep = et + static_cast<Size>(i) * FF_MODULE_REGISTRY::REG_ENTRY_SIZE;
        STORE_U32(ep + FF_MODULE_REGISTRY::REG_ENTRY_URL_IDX,
                  url_idx & FF_EXT_REF_INDEX_MASK);
        // KIND = DYNAMIC: WASM blob codec path (static C++ extensions use KIND_STATIC).
        STORE_U16(ep + FF_MODULE_REGISTRY::REG_ENTRY_KIND,             FF_MODULE_KIND_DYNAMIC);
        STORE_U16(ep + FF_MODULE_REGISTRY::REG_ENTRY_KIND_PAD,         0);
        STORE_U64(ep + FF_MODULE_REGISTRY::REG_ENTRY_WASM_BLOB_OFFSET, blob_off);
        STORE_U32(ep + FF_MODULE_REGISTRY::REG_ENTRY_WASM_BLOB_SIZE,   blob_size);
        STORE_U32(ep + FF_MODULE_REGISTRY::REG_ENTRY_PAD2,             0);
        // Write the 32-byte SHA-256 content hash — version identity for this module.
        std::memcpy(ep + FF_MODULE_REGISTRY::REG_ENTRY_MODULE_HASH,
                    content_hash.data(), FF_MODULE_REGISTRY::REG_ENTRY_HASH_SIZE);
        // Schema hash: populated when the .ffd descriptor is available; zero-filled until then.
        std::memset(ep + FF_MODULE_REGISTRY::REG_ENTRY_SCHEMA_HASH,    0,
                    FF_MODULE_REGISTRY::REG_ENTRY_HASH_SIZE);
    }

    STORE_U64(base + FF_HEADER::MODULE_REG_OFFSET, reg_off);
    builder.set_module_reg_offset(reg_off);
}

// =====================================================================
// Async AOT worker
// =====================================================================

void FF_WasmExtensionHost::enqueue_resolve(std::string extension_url) {
    if (extension_url.empty()) return;
    {
        std::lock_guard lk(m_queue_mutex);
        if (m_aot_stop) return;
        if (!m_in_flight.insert(extension_url).second) return; // dup
        m_resolve_queue.push_back(std::move(extension_url));
    }
    m_queue_cv.notify_one();
}

void FF_WasmExtensionHost::aot_worker_loop() {
    for (;;) {
        std::string url;
        {
            std::unique_lock lk(m_queue_mutex);
            m_queue_cv.wait(lk, [this]{ return m_aot_stop || !m_resolve_queue.empty(); });
            if (m_aot_stop && m_resolve_queue.empty()) return;
            url = std::move(m_resolve_queue.front());
            m_resolve_queue.pop_front();
        }
        // Best-effort fetch; failures are logged inside resolve_or_fetch_module.
        (void)resolve_or_fetch_module(url);
        {
            std::lock_guard lk(m_queue_mutex);
            m_in_flight.erase(url);
        }
    }
}

} // namespace FastFHIR::Extensions

#endif // FASTFHIR_ENABLE_EXTENSIONS
