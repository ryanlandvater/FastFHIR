/**
 * @file FF_Memory.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * @version 0.1
 *
 * @brief FastFHIR internal — the arena body and the raw-stream machinery.
 *
 * Include <FastFHIR.hpp> for the public `Memory` handle. Include this header as
 * well when you need the deeper surface it does not carry:
 *
 *     Memory_t::claim_space()          reserve arena space directly
 *     Memory_t::try_acquire_stream()   the exclusive raw-socket write lock
 *     Memory::StreamHead               that lock, once acquired
 *     Memory_t::truncate_file()        shrink a file-backed arena
 *     seal_stream() / StreamSealInfo   the shared sealing tail
 *
 * Everything here is reachable through the `->` operator on a public handle, so
 * a translation unit that includes this header can write `mem->claim_space()`
 * where a public-only one cannot.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "FF_Access.hpp"

namespace FastFHIR
{

class Memory_t;   // the arena body, defined below

    /**
     * @class Memory::StreamHead
     * @brief Exclusive Network Proxy (RAII Lock) for raw socket ingestion.
     * @details Provides an RAII-based exclusive lock on the VMA's write-head for raw,
     * unframed TCP streams. This ensures a single continuous stream can be DMA'd
     * directly from the NIC into the arena without thread interleaving or data corruption.
     * It is directly compatible with ASIO TCP networking buffers, allowing pure zero-copy
     * ingestion from the network stack into FastFHIR streams.
     */
class Memory::StreamHead
    {
        friend class Memory_t;
        Memory_t *m_memory;
        // FF_HEADER::HEADER_SIZE rather than Memory_t::STREAM_HEADER_SIZE: the
        // body is defined after this class, and the two are the same value --
        // Memory_t static_asserts STREAM_HEADER_SIZE == FF_HEADER::HEADER_SIZE.
        mutable uint8_t m_staged_header[FF_HEADER::HEADER_SIZE];
        size_t m_staging_offset = ~size_t{0};

        /**
         * @brief Internal constructor utilized by `try_acquire_stream()`.
         */
        explicit StreamHead(Memory_t *memory);

        /**
         * @brief Unlocks the stream head, allowing other threads to acquire it.
         */
        void release();

    public:
        /** @brief Move constructor transfers the RAII lock ownership. */
        StreamHead(StreamHead &&other) noexcept;

        /** @brief Move assignment transfers the RAII lock ownership. */
        StreamHead &operator=(StreamHead &&other) noexcept;

        // Non-copyable to enforce strict exclusivity
        StreamHead(const StreamHead &) = delete;
        StreamHead &operator=(const StreamHead &) = delete;

        /** @brief Destructor automatically releases the exclusive stream lock. */
        ~StreamHead() { release(); }

        /**
         * @brief Gets the zero-copy destination pointer for socket reads.
         * @return A mutable pointer to the current active write edge of the arena.
         * @throws std::logic_error if accessed after the lock has been moved or released.
         */
        uint8_t *write_ptr() const;

        /**
         * @brief Alias for `write_ptr()`, satisfying standard C++ buffer concepts.
         * @return A void pointer to the write edge.
         */
        void *data() const { return write_ptr(); }

        /**
         * @brief Calculates remaining contiguous physical memory in the arena.
         * @return Size in bytes available before the VMA capacity is exceeded.
         */
        size_t available_space() const;

        /**
         * @brief Alias for `available_space()`, satisfying standard C++ buffer concepts.
         * @return Size in bytes available.
         */
        size_t size() const { return available_space(); }

        /** @brief Explicitly releases the exclusive stream lock. Safe to call multiple times. */
        void close() { release(); }

        /**
         * @brief Publishes written data to the arena and advances the write head.
         * @details Advances the VMA write-head post-read using release semantics. This
         * ensures that the newly DMA'd payload is immediately and safely visible across
         * all CPU cores and reader threads.
         *
         * The stream lock remains held after each call so a single acquired StreamHead
         * can commit multiple chunks contiguously. The lock is released only when the
         * StreamHead is destroyed or otherwise released.
         * @param bytes_written The exact number of bytes successfully transferred from the NIC.
         * @throws std::runtime_error if the committed bytes exceed the arena's maximum capacity.
         * @throws std::logic_error if the lock is invalid.
         */
        void commit(size_t bytes_written);
    };

/**
 * @class Memory_t
 * @brief The FastFHIR Virtual Memory Arena body — the memory mapping, the
 *        write head and the stream lock. Held and named by the Memory handle.
 */
class Memory_t : public std::enable_shared_from_this<Memory_t>
{
    friend class Memory;
    friend class Memory::View;
    friend class Memory::StreamHead;

public:
    constexpr static uint64_t STREAM_LOCK_BIT = 1ULL << 63;
    constexpr static uint64_t OFFSET_MASK = ~STREAM_LOCK_BIT;
    constexpr static size_t STREAM_HEADER_SIZE = FF_HEADER::HEADER_SIZE;
    constexpr static size_t STREAM_CURSOR_OFFSET = 8;
    constexpr static size_t STREAM_PAYLOAD_OFFSET = 16;

    /// The handle's nested types, under the body's `_t` spelling. One
    /// definition each, in Memory; these are the names the body uses.
    using view_t        = Memory::View;
    using stream_head_t = Memory::StreamHead;

    ~Memory_t();

    // --- Public API ---

    /** @brief True for an arena from openReadOnly(); every write through it throws. */
    bool read_only() const { return m_read_only; }

    /**
     * @brief Lock-Free Multiplexing for framed protocols.
     * Reserves an exclusive slice of the arena using a single atomic instruction.
     * @param bytes The exact number of bytes required.
     * @return The relative offset claimed for exclusive writing.
     * @throws std::runtime_error if the request exceeds VMA capacity.
     */
    uint64_t claim_space(size_t bytes);

    /**
     * @brief Attempts to acquire the exclusive network ingestion lock.
     * @return A StreamHead RAII guard if the lock is acquired, or std::nullopt if another socket is actively streaming.
     */
    std::optional<stream_head_t> try_acquire_stream();

    /**
     * @brief Retrieves the mathematically strict base pointer of the Data Arena.
     * All internal offsets within FastFHIR structures are relative to this pointer.
     * @return Pointer to the payload arena (memory start + 8 bytes).
     */
    uint8_t *base() const { return m_base; }
#ifndef NDEBUG
    /// Weak handle on this arena's liveness; a view attaches it to itself so it
    /// can tell whether the bytes it points at still exist.
    LivenessRef liveness() const noexcept { return m_liveness; }
#endif

    /** @brief Returns the total requested capacity of the sparse mapping. */
    size_t capacity() const { return m_capacity; }

    /**
     * @brief On-disk size of the backing file as the OS reports it, or 0 when
     * unknown (anonymous arena, or a file this call created).
     *
     * The authority for "how many bytes are really there". size() cannot serve
     * that role for untrusted input: the write head lives at STREAM_CURSOR_OFFSET,
     * which is the same 8 bytes as FF_HEADER::STREAM_SIZE, so on a damaged stream
     * size() returns a corrupted wire value. A reader that must not walk off the
     * end -- FastFHIR::Recovery above all -- bounds itself by this when it is
     * available, and by capacity() when it is not.
     */
    size_t disk_size() const { return m_disk_size; }

    /** @brief Returns the SHM segment name, the file path, or an empty string if anonymous. */
    std::string name() const { return m_name; }

    /**
     * @brief Resets the committed stream boundary.
     * @param committed_size New committed size in bytes. Use 0 before streaming a raw
     * serialized FastFHIR archive into the arena so the first byte lands at offset 0.
     * @throws std::runtime_error if the requested size exceeds arena capacity.
     */
    void reset(size_t committed_size = 0);

    /**
     * @brief Returns the current boundary of globally visible, committed data.
     * Uses acquire semantics to ensure safe observation across threads.
     * @return The 64-bit size of the committed payload space.
     */
    uint64_t size() const
    {
        return std::atomic_ref<uint64_t>(*m_head_ptr).load(std::memory_order_acquire) & OFFSET_MASK;
    }

    /**
     * @brief A `Memory::View` over this arena's committed bytes.
     * @details The View is an OWNER — it holds the arena alive — while the bytes
     * it exposes are a non-owning `std::string_view`. That split is the point: a
     * View can outlive every other handle to the arena, and the text it hands
     * out stays valid for as long as the View lives.
     */
    view_t view() { return view_t(shared_from_this()); }

    /**
     * @brief Truncates the backing file to @p size bytes.
     * @details No-op for anonymous and shared-memory arenas. After `finalize()` the
     * write head is parked at the sealed payload size; passing `size()` here
     * reclaims the unused disk space that was pre-allocated by `createFromFile`.
     * @param size Target file size in bytes. Must be ≤ capacity().
     */
    void truncate_file(size_t size);

    /**
     * @brief Eagerly releases OS handles (unmap + close file/mapping handles).
     * @details Idempotent. On Windows, file-backed arenas hold an exclusive lock on
     * the backing file for the lifetime of the mapping. Calling close() releases that
     * lock immediately regardless of how many Memory handle copies still exist,
     * allowing the file to be deleted or the containing directory to be removed.
     */
    void close() noexcept;

private:
    Memory_t() = delete;
    explicit Memory_t(uint8_t *base, size_t capacity, void *fh, void *osh, int fd, const std::string &name);

    void release_stream_lock() noexcept;
    void require_writable(const char *operation) const;

    /// Which of the two factories above is running. One mapping routine
    /// serves both; the difference is what it may do to the file.
    enum class FileAccess
    {
        Amend, ///< createFromFile: read-write, reserves `capacity`
        Read,  ///< openReadOnly: read-only, maps exactly what is there
    };
    static Memory mapFile(const std::filesystem::path &filepath, size_t capacity, FileAccess access);

    std::string m_name;
    size_t m_capacity = 0;
    // Size the OS reports for the backing file, when this arena was mapped
    // from an EXISTING one; 0 for an anonymous arena or a freshly created
    // file. It is the only extent that does not come from the stream's own
    // bytes -- see disk_size().
    size_t m_disk_size = 0;
    // Backed by a file on disk (createFromFile / openReadOnly), as opposed to
    // an anonymous or shared-memory arena. Only a file has a length that
    // truncate_file can trim; a shared segment is sized once by whoever
    // created it.
    bool m_file_backed = false;
    // Mapped by openReadOnly(). The pages are PROT_READ, so a write would
    // fault; every writing entry point checks this first and throws a
    // named error instead.
    bool m_read_only = false;

    uint8_t *m_base = nullptr;
    uint64_t *m_head_ptr = nullptr;

#ifndef NDEBUG
    /// This arena's liveness token. Views built over this arena hold a weak
    /// reference to it, so a read after the arena is released throws instead of
    /// touching unmapped memory. Debug builds only — see VIEW LIVENESS in
    /// FF_Access.hpp.
    ///
    /// The token belongs to the ARENA, not to the Parser or Builder that read
    /// from it. That is the condition that actually invalidates a view: the
    /// bytes go away when the last `Memory` reference goes away, whatever
    /// parser happened to hand the view out. A temporary Parser over a live
    /// arena therefore does not make its nodes stale.
    std::shared_ptr<const Detail::LivenessToken> m_liveness = Detail::make_liveness_token();
#endif

#ifdef _WIN32
    void *m_file_handle = nullptr;
    void *m_os_handle = nullptr;
#endif
    int m_os_fd = -1;
};

inline Memory::StreamHead::StreamHead(Memory_t *memory)
    : m_memory(memory),
      m_staging_offset((std::atomic_ref<uint64_t>(*memory->m_head_ptr).load(std::memory_order_relaxed) & Memory_t::OFFSET_MASK) == 0
                           ? 0
                           : ~size_t{0})
{
    std::memset(m_staged_header, 0, Memory_t::STREAM_HEADER_SIZE);
}
inline Memory::StreamHead::StreamHead(StreamHead &&other) noexcept
    : m_memory(other.m_memory),
      m_staging_offset(other.m_staging_offset)
{
    std::memcpy(m_staged_header, other.m_staged_header, Memory_t::STREAM_HEADER_SIZE);
    other.m_memory = nullptr;
    other.m_staging_offset = ~size_t{0};
}
inline Memory::StreamHead &Memory::StreamHead::operator=(StreamHead &&other) noexcept
{
    if (this != &other)
    {
        release();
        m_memory = other.m_memory;
        m_staging_offset = other.m_staging_offset;
        std::memcpy(m_staged_header, other.m_staged_header, Memory_t::STREAM_HEADER_SIZE);
        other.m_memory = nullptr;
        other.m_staging_offset = ~size_t{0};
    }
    return *this;
}
inline uint8_t *Memory::StreamHead::write_ptr() const
{
    if (!m_memory)
        throw std::logic_error("Invalid StreamHead access");
    if (m_staging_offset < Memory_t::STREAM_HEADER_SIZE)
    {
        return m_staged_header + m_staging_offset;
    }
    return m_memory->m_base + (std::atomic_ref<uint64_t>(*m_memory->m_head_ptr).load(std::memory_order_relaxed) & Memory_t::OFFSET_MASK);
}
inline size_t Memory::StreamHead::available_space() const
{
    if (!m_memory)
        return 0;
    if (m_staging_offset < Memory_t::STREAM_HEADER_SIZE)
    {
        return Memory_t::STREAM_HEADER_SIZE - m_staging_offset;
    }
    return m_memory->m_capacity - (std::atomic_ref<uint64_t>(*m_memory->m_head_ptr).load(std::memory_order_relaxed) & Memory_t::OFFSET_MASK);
}
inline void Memory::StreamHead::release()
{
    if (m_memory)
    {
        if (m_staging_offset == Memory_t::STREAM_HEADER_SIZE)
        {
            std::memcpy(m_memory->m_base, m_staged_header, Memory_t::STREAM_CURSOR_OFFSET);
            std::memcpy(m_memory->m_base + Memory_t::STREAM_PAYLOAD_OFFSET,
                        m_staged_header + Memory_t::STREAM_PAYLOAD_OFFSET,
                        Memory_t::STREAM_HEADER_SIZE - Memory_t::STREAM_PAYLOAD_OFFSET);
        }
        m_memory->release_stream_lock();
        m_memory = nullptr;
    }
}


// ============================================================================
// Shared stream sealing
// ============================================================================

/**
 * @brief Everything seal_stream() needs, as one bundle.
 *
 * Same shape as the FF_*Info structs on the external surface: a single
 * `const StreamSealInfo&` argument instead of a nine-parameter call, so the
 * sealing tail can grow a field without touching either caller's argument
 * list.
 *
 * REQUIRED: `memory`, `fhir_revision`, `root_offset` and `root_recovery`.
 * Each of those must be set explicitly — leaving one at its default sentinel
 * is REFUSED by seal_stream() at runtime, because a stream sealed with an
 * unset root is a stream no reader can enter.
 */
struct StreamSealInfo {
    Memory                        memory;                                   ///< The arena holding the payload; the checksum slot is claimed at the current write head, so call after the payload is fully written.
    uint16_t                      fhir_revision        = 0;                 ///< FHIR revision stamped into the header; 0 is unset.
    Offset                        root_offset          = FF_NULL_OFFSET;    ///< Offset of the root block.
    RECOVERY_TAG                  root_recovery        = FF_RECOVER_UNDEFINED; ///< Recovery tag of the root block.
    FF_Checksum_Algorithm         algorithm            = FF_CHECKSUM_NONE;  ///< Checksum algorithm; NONE emits a zeroed checksum.
    std::function<std::vector<BYTE>(const unsigned char*, Size)> hasher = nullptr; ///< Required when algorithm != NONE; null otherwise.
    FF_StreamCompaction           stream_layout        = FF_STREAM_COMPACTION_NONE; ///< Standard or compact.
    Offset                        url_dir_offset       = FF_NULL_OFFSET;    ///< Stream-level FF_URL_DIRECTORY offset, or FF_NULL_OFFSET.
    Offset                        module_reg_offset    = FF_NULL_OFFSET;    ///< FF_MODULE_REGISTRY offset, or FF_NULL_OFFSET.
};

/**
 * @brief Seals a stream: claims the checksum slot, stamps the FF_HEADER, and
 *        hashes the payload up to the hash slot.
 * @details The single implementation of the sealing tail shared by the only two
 * producers of sealed FastFHIR streams -- Builder_t::finalize() (standard layout,
 * with URL/module directory offsets) and Compactor::archive() (compact layout,
 * no directory offsets). Producer-specific concerns stay with the callers:
 * checksum-algorithm defaulting/warnings and backing-file truncation.
 * @param info The arena and every header field; see StreamSealInfo.
 * @return A lifetime-safe view of the sealed stream.
 */
inline Memory::View seal_stream(const StreamSealInfo& info)
{
    // Preconditions: an arena, a revision, and a root that exists. Checked
    // before anything is claimed, so a refusal leaves the arena untouched.
    if (!info.memory)
        throw std::runtime_error("FastFHIR: seal_stream requires an arena (StreamSealInfo::memory)");
    if (info.fhir_revision == 0)
        throw std::runtime_error("FastFHIR: seal_stream requires a FHIR revision (StreamSealInfo::fhir_revision)");
    if (info.root_offset == FF_NULL_OFFSET || info.root_recovery == FF_RECOVER_UNDEFINED)
        throw std::runtime_error("FastFHIR: seal_stream requires a root block "
                                 "(StreamSealInfo::root_offset / root_recovery); "
                                 "sealing without one writes a stream no reader can enter");

    const Offset checksum_off = info.memory->claim_space(FF_CHECKSUM::HEADER_SIZE);
    STORE_FF_HEADER(info.memory->base(), info.fhir_revision, info.memory->size(),
                    info.root_offset, info.root_recovery, checksum_off,
                    info.url_dir_offset, info.module_reg_offset, info.stream_layout);
    // Writes the 12 bytes of metadata, returns a pointer to byte 12 (the 32-byte slot)
    BYTE* hash_dst = STORE_FF_CHECKSUM_METADATA(info.memory->base(), checksum_off, info.algorithm);
    if (info.hasher != nullptr && info.algorithm != FF_CHECKSUM_NONE) {
        // Hash the payload + the 12 bytes of metadata, stopping exactly where the hash slot begins.
        const Size bytes_to_hash = checksum_off + FF_CHECKSUM::HASH_DATA;
        std::vector<BYTE> hash_value = info.hasher(info.memory->base(), bytes_to_hash);
        const size_t copy_len = std::min(hash_value.size(), static_cast<size_t>(FF_MAX_HASH_BYTES));
        std::memcpy(hash_dst, hash_value.data(), copy_len);
    }
    return info.memory->view();
}


} // namespace FastFHIR
