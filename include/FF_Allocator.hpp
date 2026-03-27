#pragma once

#include <cstdint>
#include <atomic>
#include <string>
#include <string_view>
#include <memory>
#include <optional>

namespace FastFHIR {

class FF_Allocator;

/**
 * @class StreamView
 * @brief A lifetime-safe memory lens over the committed data arena.
 * * Binds a standard std::string_view to the shared ownership of the FF_Allocator. 
 * This ensures the massive sparse mapping remains alive in physical RAM during 
 * asynchronous OS network egress or database writes, preventing use-after-free 
 * errors when passing views to downstream sinks.
 */
class StreamView {
public:
    /**
     * @brief Constructs a lifetime-safe view of the VMA.
     * @param allocator Shared pointer to the allocator ensuring memory lifespan.
     */
    explicit StreamView(std::shared_ptr<FF_Allocator> allocator);

    /**
     * @brief Implicit conversion to std::string_view.
     * Allows drop-in compatibility with sockets, cryptographic hashers, and APIs 
     * expecting standard string views.
     * @return A lightweight, non-owning view of the committed memory.
     */
    operator std::string_view() const noexcept;

    /** @brief Returns a pointer to the start of the payload. */
    const char* data() const noexcept;
    
    /** @brief Returns the size of the committed payload in bytes. */
    size_t size() const noexcept;
    
    /** @brief Checks if the payload view is empty. */
    bool empty() const noexcept;

private:
    std::shared_ptr<FF_Allocator> m_vma_ref;
    std::string_view m_view;
};

/**
 * @class FF_Allocator
 * @brief Virtual Memory Arena (VMA) Manager for zero-copy FHIR serialization.
 * * Manages OS-level sparse mapping (typically 10GB to 1TB) to allow theoretically 
 * infinite clinical data streams within a single contiguous address space without 
 * reallocation. The memory layout is mathematically strict:
 * the first 8 bytes house the atomic control header, followed immediately by the 
 * FastFHIR payload arena.
 */
class FF_Allocator : public std::enable_shared_from_this<FF_Allocator> {
public:
    /**
     * @class StreamHead
     * @brief Exclusive Network Proxy (RAII Lock) for raw socket ingestion.
     * * Provides an RAII-based exclusive lock on the VMA's write-head for raw, 
     * unframed TCP streams. This ensures a single continuous 
     * stream can be DMA'd directly from the NIC into the arena without interleaving 
     * or corruption.
     * 
     * NOTE: This class is directly compatible with ASIO TCP networking buffers, 
     * allowing zero-copy ingestion from the network stack into FastFHIR streams.
     */
    class StreamHead {
    public:
        StreamHead(StreamHead&& other) noexcept;
        StreamHead& operator=(StreamHead&& other) noexcept;
        ~StreamHead();

        StreamHead(const StreamHead&) = delete;
        StreamHead& operator=(const StreamHead&) = delete;

        /**
         * @brief Gets the zero-copy destination pointer for socket reads.
         * @return Pointer to the current active write edge of the arena.
         */
        uint8_t* write_ptr() const;
        void* data() const { return write_ptr(); }

        /**
         * @brief Calculates remaining bytes in the arena.
         * @return Size in bytes available before capacity is exceeded.
         */
        size_t available_space() const;
        size_t size() const { return available_space(); }


        /**
         * @brief Publishes written data to the arena.
         * Advances the VMA write-head post-read using release semantics to ensure
         * payload visibility across all CPU cores.
         * @param bytes_written The exact number of bytes transferred from the NIC.
         */
        void commit(size_t bytes_written);

    private:
        friend class FF_Allocator; 
        explicit StreamHead(FF_Allocator* allocator);
        void release();

        FF_Allocator* m_allocator;
    };

    // ------------------------------------------------------------------------
    // Factory & Lifecycle
    // ------------------------------------------------------------------------
    
    /**
     * @brief Factory allocation for the Virtual Memory Arena.
     * @param name Optional. If empty, creates an anonymous RAM mapping. If provided, 
     * creates a cross-process Shared Memory (SHM) segment.
     * @param capacity Defaults to a 4GB sparse allocation.
     * @return Shared pointer to the initialized allocator.
     */
    static std::shared_ptr<FF_Allocator> Create(std::string name = "", 
                                                size_t capacity = 4ULL * 1024 * 1024 * 1024);
    
    /**
     * @brief Destroys the allocator and unmaps the OS memory pages.
     */
    ~FF_Allocator(); 

    // ------------------------------------------------------------------------
    // Ingestion API
    // ------------------------------------------------------------------------

    /**
     * @brief Lock-Free Multiplexing for framed protocols.
     * Used when payload size is known upfront (e.g., HTTP/FastFHIR). Allows dozens 
     * of threads to concurrently reserve exclusive slices using a single atomic 
     * instruction.
     * @param bytes The exact number of bytes required for the serialized object.
     * @return The relative offset claimed for exclusive writing.
     * @throws std::runtime_error if the request exceeds VMA capacity.
     */
    uint64_t claim_space(size_t bytes);

    /**
     * @brief Attempts to acquire the exclusive network ingestion lock.
     * @return A StreamHead RAII guard if the lock is acquired, or std::nullopt 
     * if another socket is actively streaming into this VMA.
     */
    std::optional<StreamHead> try_acquire_stream();

    // ------------------------------------------------------------------------
    // Transactional Integrity API
    // ------------------------------------------------------------------------

    /**
     * @brief Late Anchoring: Patches the root offset pointing to the final Bundle.
     * Ensures that Parsers never see a partially constructed or inconsistent Bundle 
     * while concurrent threads are writing resources.
     * @param offset The absolute offset from the base of the arena to the root object.
     */
    void set_root_offset(uint64_t offset);

    /**
     * @brief Retrieves the currently anchored root offset.
     * @return The offset pointing to the primary bundle/resource.
     */
    uint64_t get_root_offset() const;

    // ------------------------------------------------------------------------
    // Accessors & Views
    // ------------------------------------------------------------------------

    /** * @brief Retrieves the mathematically strict base pointer of the Data Arena.
     * All internal offsets within FastFHIR structures are relative to this pointer. 
     * @return Pointer to the payload arena (memory start + 8 bytes).
     */
    uint8_t* base() const { return m_base; }
    
    /** @brief Returns the total requested capacity of the sparse mapping. */
    size_t capacity() const { return m_capacity; }
    
    /** @brief Returns the SHM segment name, or an empty string if anonymous. */
    const std::string& name() const { return m_name; }
    
    /**
     * @brief Returns the current boundary of globally visible, committed data.
     * Uses acquire semantics to ensure safe observation.
     * @return The 64-bit size of the committed payload space.
     */
    uint64_t current_write_head() const { return m_head->load(std::memory_order_acquire); }

    /**
     * @brief Returns a lifetime-safe wrapper of the committed arena.
     * @return A StreamView encompassing all data from base() to current_write_head().
     */
    StreamView view() { return StreamView(shared_from_this()); }

private:
    FF_Allocator() = default;
    void initialize(std::string name, size_t capacity);

    void release_stream_lock();

    std::string m_name;
    size_t m_capacity = 0;
    size_t m_total_size = 0;

    // --- OS Agnostic Memory Pointers ---
    uint8_t* m_shm_ptr = nullptr;
    std::atomic<uint64_t>* m_head = nullptr; 
    uint8_t* m_base = nullptr;      

    // In-process spinlock for the StreamHead exclusive proxy.
    std::atomic_flag m_stream_lock = ATOMIC_FLAG_INIT;

    // --- Opaque OS Handles (Hidden from Header) ---
    void* m_os_handle = nullptr; 
    int   m_os_fd = -1;          
};

// ============================================================================
// StreamView Inline Implementations
// ============================================================================
// NOTE: Placed below FF_Allocator so the compiler knows the full layout and methods.
inline StreamView::StreamView(std::shared_ptr<FF_Allocator> vma)
    : m_vma_ref(std::move(vma)) {}

inline StreamView::operator std::string_view() const noexcept {
    return std::string_view(
        reinterpret_cast<const char*>(m_vma_ref->base()), 
        m_vma_ref->current_write_head()
    );
}

inline const char* StreamView::data() const noexcept {
    return reinterpret_cast<const char*>(m_vma_ref->base());
}

inline size_t StreamView::size() const noexcept {
    return m_vma_ref->current_write_head();
}

inline bool StreamView::empty() const noexcept {
    return size() == 0;
}

inline FF_Allocator::StreamHead::StreamHead(FF_Allocator* allocator) 
    : m_allocator(allocator) {}

inline FF_Allocator::StreamHead::StreamHead(StreamHead&& other) noexcept 
    : m_allocator(other.m_allocator) {
    other.m_allocator = nullptr;
}

inline FF_Allocator::StreamHead& FF_Allocator::StreamHead::operator=(StreamHead&& other) noexcept {
    if (this != &other) {
        release(); 
        m_allocator = other.m_allocator;
        other.m_allocator = nullptr;
    }
    return *this;
}

inline FF_Allocator::StreamHead::~StreamHead() {
    release();
}

inline uint8_t* FF_Allocator::StreamHead::write_ptr() const {
    if (!m_allocator) throw std::logic_error("Invalid StreamHead access");
    return m_allocator->m_base + m_allocator->m_head->load(std::memory_order_relaxed);
}

inline size_t FF_Allocator::StreamHead::available_space() const {
    if (!m_allocator) return 0;
    return m_allocator->m_capacity - m_allocator->m_head->load(std::memory_order_relaxed);
}

inline void FF_Allocator::StreamHead::commit(size_t bytes_written) {
    if (!m_allocator) throw std::logic_error("Invalid StreamHead access");
    
    uint64_t current = m_allocator->m_head->load(std::memory_order_relaxed);
    if (current + bytes_written > m_allocator->m_capacity) {
        throw std::runtime_error("Stream commit exceeds VMA capacity");
    }
    
    // Release semantics push the NIC/Socket DMA writes to global visibility
    m_allocator->m_head->store(current + bytes_written, std::memory_order_release);
}

inline void FF_Allocator::StreamHead::release() {
    if (m_allocator) {
        m_allocator->release_stream_lock();
        m_allocator = nullptr;
    }
}

} // namespace FastFHIR