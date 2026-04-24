/**
 * @file FF_Memory.hpp
 * @brief Virtual Memory Arena (VMA) Handle/Body implementation for FastFHIR.
 */
#pragma once

#include <cstdint>
#include <atomic>
#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <filesystem>
#include <stdexcept>
#include <cstring>

namespace FastFHIR {

class FF_Memory_t;

/**
 * @class FF_Memory
 * @brief Lightweight handle providing shared ownership over the FastFHIR VMA Core.
 * * Acts as a copyable proxy to the underlying OS memory mapping. Multiple handles 
 * can point to the same physical memory arena safely.
 */
class Memory {
public:
    constexpr static uint64_t   STREAM_LOCK_BIT = 1ULL << 63;
    constexpr static uint64_t   OFFSET_MASK = ~STREAM_LOCK_BIT;
    constexpr static size_t     STREAM_HEADER_SIZE = 38;
    constexpr static size_t     STREAM_CURSOR_OFFSET = 8;
    constexpr static size_t     STREAM_PAYLOAD_OFFSET = 16;

    class View;
    class StreamHead;

    // --- Lifecycle ---
    
    /** @brief Constructs a null/empty memory handle. */
    Memory() = default;
    
    /** @brief Constructs a handle taking shared ownership of an existing core. */
    explicit Memory(std::shared_ptr<FF_Memory_t> core) : m_core(std::move(core)) {}

    /**
     * @brief Factory allocation for the Virtual Memory Arena.
     * @param shm_name Optional. If empty, creates an anonymous RAM mapping. If provided, creates a cross-process Shared Memory (SHM) segment.
     * @param capacity Defaults to a 4GB sparse allocation.
     * @return Initialized memory handle.
     */
    static Memory create(size_t capacity = 4ULL * 1024 * 1024 * 1024, std::string shm_name = "");
    
    /**
     * @brief Factory allocation for a file-backed Virtual Memory Arena.
     * @param filepath Path to the backing file on disk.
     * @param capacity Defaults to a 4GB sparse allocation.
     * @return Initialized memory handle.
     */
    static Memory createFromFile(const std::filesystem::path& filepath, size_t capacity = 4ULL * 1024 * 1024 * 1024);

    /** @brief Checks if this handle points to a valid, instantiated memory core. */
    explicit operator bool() const { return m_core != nullptr; }

    // --- Forwarding API ---

    /**
     * @brief Lock-Free Multiplexing for framed protocols.
     * Reserves an exclusive slice of the arena using a single atomic instruction.
     * @param bytes The exact number of bytes required.
     * @return The relative offset claimed for exclusive writing.
     * @throws std::runtime_error if the request exceeds VMA capacity.
     */
    uint64_t claim_space(size_t bytes) const;
    
    /**
     * @brief Attempts to acquire the exclusive network ingestion lock.
     * @return A StreamHead RAII guard if the lock is acquired, or std::nullopt if another socket is actively streaming.
     */
    std::optional<StreamHead> try_acquire_stream() const;

    /**
     * @brief Retrieves the mathematically strict base pointer of the Data Arena.
     * All internal offsets within FastFHIR structures are relative to this pointer.
     * @return Pointer to the payload arena (memory start + 8 bytes).
     */
    uint8_t* base() const;

    /**
     * @brief Returns the total requested capacity of the sparse mapping.
     */
    size_t capacity() const;

    /**
     * @brief Returns the SHM segment name, the file path, or an empty string if anonymous.
     */
    std::string name() const;

    /**
     * @brief Resets the committed stream boundary.
     * @param committed_size New committed size in bytes. Use 0 before streaming a raw
     * serialized FastFHIR archive into the arena so the first byte lands at offset 0.
     * @throws std::runtime_error if the requested size exceeds arena capacity.
     */
    void reset(size_t committed_size = 0) const;

    /**
     * @brief Returns the current boundary of globally visible, committed data.
     * Uses acquire semantics to ensure safe observation across threads.
     * @return The 64-bit size of the committed payload space.
     */
    uint64_t size() const;

    /**
     * @brief Returns a lifetime-safe, non-owning string_view wrapper of the committed arena.
     */
    View view() const { return View(m_core); }
    
    /**
     * @class FF_Memory::View
     * @brief A lifetime-safe memory lens over the committed FastFHIR data arena.
     * * @details Unlike a standard `std::string_view` which only holds raw pointers and
     * can easily dangle if the source memory is unmapped, `FF_Memory::View` internally
     * holds a shared reference to the underlying VMA core. This guarantees that the
     * massive sparse mapping remains physically alive in RAM during asynchronous operations
     * (like non-blocking OS network egress or async database writes), strictly preventing
     * use-after-free errors when passing read-only views to downstream sinks.
     */
    class View {
    public:
        /**
         * @brief Implicit conversion to `std::string_view`.
         * * @details Allows drop-in compatibility with POSIX sockets, cryptographic hashers,
         * and external APIs expecting standard contiguous string views. The returned view
         * spans strictly from the arena base to the currently committed write head.
         * * @warning The resulting `std::string_view` drops the lifetime guarantee. Do not
         * outlive the parent `FF_Memory::View` object.
         * * @return A lightweight, non-owning view of the committed memory.
         */
        operator std::string_view() const noexcept;

        /**
         * @brief Retrieves a raw pointer to the start of the FastFHIR payload.
         * @return A read-only character pointer to the arena base.
         */
        const char* data() const noexcept;
        
        /**
         * @brief Retrieves the size of the globally visible, committed payload.
         * @return The size in bytes.
         */
        size_t size() const noexcept;
        
        /**
         * @brief Checks if the payload view contains any committed data.
         * @return `true` if the committed size is exactly 0, `false` otherwise.
         */
        bool empty() const noexcept;
    private:
        friend class Memory;
        const std::shared_ptr<FF_Memory_t> m_vma_ref;
        explicit View(std::shared_ptr<FF_Memory_t> vma_ref) : m_vma_ref(std::move(vma_ref)) {}
    };
    
    /**
     * @class FF_Memory::StreamHead
     * @brief Exclusive Network Proxy (RAII Lock) for raw socket ingestion.
     * * @details Provides an RAII-based exclusive lock on the VMA's write-head for raw,
     * unframed TCP streams. This ensures a single continuous stream can be DMA'd
     * directly from the NIC into the arena without thread interleaving or data corruption.
     * It is directly compatible with ASIO TCP networking buffers, allowing pure zero-copy
     * ingestion from the network stack into FastFHIR streams.
     */
    class StreamHead {
        FF_Memory_t* m_memory;
        mutable uint8_t m_staged_header[Memory::STREAM_HEADER_SIZE];
        size_t m_staging_offset = ~size_t{0};

        friend class FF_Memory_t;
        friend class Memory;
        
        /**
         * @brief Internal constructor utilized by `FF_Memory::try_acquire_stream()`.
         */
        explicit StreamHead(FF_Memory_t* memory);
        
        /**
         * @brief Unlocks the stream head, allowing other threads to acquire it.
         */
        void release();

    public:
        /** @brief Move constructor transfers the RAII lock ownership. */
        StreamHead(StreamHead&& other) noexcept;
        
        /** @brief Move assignment transfers the RAII lock ownership. */
        StreamHead& operator=(StreamHead&& other) noexcept;
        
        // Non-copyable to enforce strict exclusivity
        StreamHead(const StreamHead&) = delete;
        StreamHead& operator=(const StreamHead&) = delete;
        
        /** @brief Destructor automatically releases the exclusive stream lock. */
        ~StreamHead() { release(); }
        
        /**
         * @brief Gets the zero-copy destination pointer for socket reads.
         * @return A mutable pointer to the current active write edge of the arena.
         * @throws std::logic_error if accessed after the lock has been moved or released.
         */
        uint8_t* write_ptr() const;
        
        /**
         * @brief Alias for `write_ptr()`, satisfying standard C++ buffer concepts.
         * @return A void pointer to the write edge.
         */
        void* data() const { return write_ptr(); }
        
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
         * * @details Advances the VMA write-head post-read using release semantics. This
         * ensures that the newly DMA'd payload is immediately and safely visible across
         * all CPU cores and reader threads.
         *
         * The stream lock remains held after each call so a single acquired StreamHead
         * can commit multiple chunks contiguously. The lock is released only when the
         * StreamHead is destroyed or otherwise released.
         * * @param bytes_written The exact number of bytes successfully transferred from the NIC.
         * @throws std::runtime_error if the committed bytes exceed the arena's maximum capacity.
         * @throws std::logic_error if the lock is invalid.
         */
        void commit(size_t bytes_written);
    };

private:
    std::shared_ptr<FF_Memory_t> m_core;
};

// ============================================================================
// Internal Core (The "Body")
// ============================================================================

class FF_Memory_t : public std::enable_shared_from_this<FF_Memory_t> {
    friend class Memory;
    friend class Memory::StreamHead;
    friend class Memory::View;

public:
    ~FF_Memory_t();

private:
    FF_Memory_t() = delete;
    explicit FF_Memory_t(uint8_t* base, size_t capacity, void* fh, void* osh, int fd, const std::string& name);
    
    uint64_t claim_space(size_t bytes);
    std::optional<Memory::StreamHead> try_acquire_stream();
    void reset(size_t committed_size);
    void release_stream_lock() noexcept;

    std::string m_name;
    size_t m_capacity = 0;

    uint8_t* m_base = nullptr;
    uint64_t* m_head_ptr = nullptr;

    void* m_file_handle = nullptr;
    void* m_os_handle = nullptr;
    int   m_os_fd = -1;
};

// ============================================================================
// Inline Implementations
// ============================================================================

inline uint64_t Memory::claim_space(size_t bytes) const { return m_core->claim_space(bytes); }
inline std::optional<Memory::StreamHead> Memory::try_acquire_stream() const { return m_core->try_acquire_stream(); }
inline uint8_t* Memory::base() const { return m_core->m_base; }
inline size_t Memory::capacity() const { return m_core->m_capacity; }
inline std::string Memory::name() const { return m_core->m_name; }
inline void Memory::reset(size_t committed_size) const { m_core->reset(committed_size); }
inline uint64_t Memory::size() const {
    return std::atomic_ref<uint64_t>(*m_core->m_head_ptr).load(std::memory_order_acquire) & OFFSET_MASK;
}
inline const char* Memory::View::data() const noexcept {
    return reinterpret_cast<const char*>(m_vma_ref->m_base);
}
inline size_t Memory::View::size() const noexcept {
    return std::atomic_ref<uint64_t>(*m_vma_ref->m_head_ptr).load(std::memory_order_acquire) & OFFSET_MASK;
}
inline Memory::View::operator std::string_view() const noexcept {
    return std::string_view(reinterpret_cast<const char*>(m_vma_ref->m_base), size());
}
inline bool Memory::View::empty() const noexcept {
    return size() == 0;
}

inline Memory::StreamHead::StreamHead(FF_Memory_t* memory)
    : m_memory(memory),
      m_staging_offset((std::atomic_ref<uint64_t>(*memory->m_head_ptr).load(std::memory_order_relaxed) & OFFSET_MASK) == 0
                           ? 0
                           : ~size_t{0}) {
    std::memset(m_staged_header, 0, STREAM_HEADER_SIZE);
}
inline Memory::StreamHead::StreamHead(StreamHead&& other) noexcept
    : m_memory(other.m_memory),
      m_staging_offset(other.m_staging_offset) {
    std::memcpy(m_staged_header, other.m_staged_header, STREAM_HEADER_SIZE);
    other.m_memory = nullptr;
    other.m_staging_offset = ~size_t{0};
}
inline Memory::StreamHead& Memory::StreamHead::operator=(StreamHead&& other) noexcept {
    if (this != &other) {
        release();
        m_memory = other.m_memory;
        m_staging_offset = other.m_staging_offset;
        std::memcpy(m_staged_header, other.m_staged_header, STREAM_HEADER_SIZE);
        other.m_memory = nullptr;
        other.m_staging_offset = ~size_t{0};
    }
    return *this;
}
inline uint8_t* Memory::StreamHead::write_ptr() const {
    if (!m_memory) throw std::logic_error("Invalid StreamHead access");
    if (m_staging_offset < STREAM_HEADER_SIZE) {
        return m_staged_header + m_staging_offset;
    }
    return m_memory->m_base + (std::atomic_ref<uint64_t>(*m_memory->m_head_ptr).load(std::memory_order_relaxed) & OFFSET_MASK);
}
inline size_t Memory::StreamHead::available_space() const {
    if (!m_memory) return 0;
    if (m_staging_offset < STREAM_HEADER_SIZE) {
        return STREAM_HEADER_SIZE - m_staging_offset;
    }
    return m_memory->m_capacity - (std::atomic_ref<uint64_t>(*m_memory->m_head_ptr).load(std::memory_order_relaxed) & OFFSET_MASK);
}
inline void Memory::StreamHead::release() {
    if (m_memory) {
        if (m_staging_offset == STREAM_HEADER_SIZE) {
            std::memcpy(m_memory->m_base, m_staged_header, STREAM_CURSOR_OFFSET);
            std::memcpy(m_memory->m_base + STREAM_PAYLOAD_OFFSET,
                        m_staged_header + STREAM_PAYLOAD_OFFSET,
                        STREAM_HEADER_SIZE - STREAM_PAYLOAD_OFFSET);
        }
        m_memory->release_stream_lock();
        m_memory = nullptr;
    }
}

} // namespace FastFHIR
