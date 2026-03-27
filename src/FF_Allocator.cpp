/**
 * @file FF_Allocator.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @version 0.1
 * @date 2026-03-26
 * 
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file in the project root for terms.
 * 
 * @brief Implementation of the FF_Allocator class for FastFHIR's Virtual Memory Arena.
 * 
 * This file contains the implementation of the FF_Allocator class, which manages a large virtual memory
 * arena for concurrent data ingestion in FastFHIR. The allocator provides methods for creating and 
 * managing the memory mapping, as well as the StreamHead class for exclusive access to the write 
 * head of the arena. The implementation includes OS-specific code for both Windows and POSIX systems 
 * to handle memory mapping and synchronization.
 * 
 */

#include "FF_Allocator.hpp"

#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <iostream>

// ============================================================================
// OS-Specific Includes & Macros
// ============================================================================
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace FastFHIR {

// ============================================================================
// StreamHead Implementation
// ============================================================================

FF_Allocator::StreamHead::StreamHead(FF_Allocator* allocator) 
    : m_allocator(allocator) {}

FF_Allocator::StreamHead::StreamHead(StreamHead&& other) noexcept 
    : m_allocator(other.m_allocator) {
    other.m_allocator = nullptr;
}

FF_Allocator::StreamHead& FF_Allocator::StreamHead::operator=(StreamHead&& other) noexcept {
    if (this != &other) {
        release(); 
        m_allocator = other.m_allocator;
        other.m_allocator = nullptr;
    }
    return *this;
}

FF_Allocator::StreamHead::~StreamHead() {
    release();
}

uint8_t* FF_Allocator::StreamHead::write_ptr() const {
    if (!m_allocator) throw std::logic_error("Invalid StreamHead access");
    return m_allocator->base() + m_allocator->m_head->load(std::memory_order_relaxed);
}

size_t FF_Allocator::StreamHead::available_space() const {
    if (!m_allocator) return 0;
    return m_allocator->capacity() - m_allocator->m_head->load(std::memory_order_relaxed);
}

void FF_Allocator::StreamHead::commit(size_t bytes_written) {
    if (!m_allocator) throw std::logic_error("Invalid StreamHead access");
    
    // We hold the exclusive m_stream_lock, so read-add-store is safe here.
    // Memory order 'release' ensures NIC/Socket DMA writes are visible to other cores.
    uint64_t current = m_allocator->m_head->load(std::memory_order_relaxed);
    m_allocator->m_head->store(current + bytes_written, std::memory_order_release);
}

void FF_Allocator::StreamHead::release() {
    if (m_allocator) {
        m_allocator->release_stream_lock();
        m_allocator = nullptr;
    }
}

// ============================================================================
// FF_Allocator Lifecycle & OS Mapping
// ============================================================================

std::shared_ptr<FF_Allocator> FF_Allocator::Create(std::string name, size_t capacity) {
    // Private constructor requires this workaround for make_shared
    auto allocator = std::shared_ptr<FF_Allocator>(new FF_Allocator());
    allocator->initialize(std::move(name), capacity);
    return allocator;
}

void FF_Allocator::initialize(std::string name, size_t capacity) {
    m_name = std::move(name);
    m_capacity = capacity;
    m_total_size = m_capacity + 8; // +8 bytes for the atomic control header

#ifdef _WIN32
    HANDLE hMapFile = NULL;
    if (m_name.empty()) {
        // Anonymous Mapping
        hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 
                                      m_total_size >> 32, m_total_size & 0xFFFFFFFF, NULL);
    } else {
        // Named Shared Memory
        hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 
                                      m_total_size >> 32, m_total_size & 0xFFFFFFFF, m_name.c_str());
    }

    if (!hMapFile) throw std::runtime_error("Win32 CreateFileMappingA failed");
    m_os_handle = static_cast<void*>(hMapFile);

    m_shm_ptr = static_cast<uint8_t*>(MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, m_total_size));
    if (!m_shm_ptr) {
        CloseHandle(hMapFile);
        throw std::runtime_error("Win32 MapViewOfFile failed");
    }
#else
    if (m_name.empty()) {
        // Anonymous Mapping (Private to this process/children)
        m_shm_ptr = static_cast<uint8_t*>(mmap(nullptr, m_total_size, 
                                               PROT_READ | PROT_WRITE, 
                                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (m_shm_ptr == MAP_FAILED) throw std::runtime_error("POSIX anonymous mmap failed");
    } else {
        // Named Shared Memory (Cross-process)
        // Ensure name starts with a slash for POSIX compliance
        std::string posix_name = (m_name.front() != '/') ? "/" + m_name : m_name;
        
        m_os_fd = shm_open(posix_name.c_str(), O_CREAT | O_RDWR, 0666);
        if (m_os_fd == -1) throw std::runtime_error("POSIX shm_open failed");

        // Truncate to reserve the sparse virtual memory space
        if (ftruncate(m_os_fd, m_total_size) == -1) {
            close(m_os_fd);
            throw std::runtime_error("POSIX ftruncate failed");
        }

        m_shm_ptr = static_cast<uint8_t*>(mmap(nullptr, m_total_size, 
                                               PROT_READ | PROT_WRITE, 
                                               MAP_SHARED, m_os_fd, 0));
        if (m_shm_ptr == MAP_FAILED) {
            close(m_os_fd);
            throw std::runtime_error("POSIX shared mmap failed");
        }
    }
#endif

    // Establish the mathematically strict memory layout
    m_head = reinterpret_cast<std::atomic<uint64_t>*>(m_shm_ptr);
    m_base = m_shm_ptr + 8;
}

FF_Allocator::~FF_Allocator() {
#ifdef _WIN32
    if (m_shm_ptr) UnmapViewOfFile(m_shm_ptr);
    if (m_os_handle) CloseHandle(static_cast<HANDLE>(m_os_handle));
#else
    if (m_shm_ptr && m_shm_ptr != MAP_FAILED) munmap(m_shm_ptr, m_total_size);
    if (m_os_fd != -1) close(m_os_fd);
    
    // shm_unlink is deliberately omitted here. 
    // If a service restarts, we want the Named SHM to persist in /dev/shm 
    // so we can re-attach and recover the clinical stream.
#endif
}

// ============================================================================
// Ingestion & Lock Management
// ============================================================================

uint64_t FF_Allocator::claim_space(size_t bytes) {
    // Relaxed memory order is sufficient here; we are only claiming an offset.
    // The data visibility is handled later by the Builder's release fence.
    uint64_t offset = m_head->fetch_add(bytes, std::memory_order_relaxed);
    
    if (offset + bytes > m_capacity) {
        throw std::runtime_error("FastFHIR VMA Capacity Exceeded");
    }
    return offset;
}

std::optional<FF_Allocator::StreamHead> FF_Allocator::try_acquire_stream() {
    if (m_stream_lock.test_and_set(std::memory_order_acquire)) {
        return std::nullopt; // Lock is held by another socket thread
    }
    return StreamHead(this);
}

void FF_Allocator::release_stream_lock() {
    m_stream_lock.clear(std::memory_order_release);
}

// ============================================================================
// Transactional Integrity & Views
// ============================================================================

void FF_Allocator::set_root_offset(uint64_t offset) {
    // According to the FastFHIR schema, the FF_HEADER lives at the very start of m_base.
    // Assuming the root_offset is located at byte 8 of the payload arena:
    // [Magic: 4 bytes] [Version: 4 bytes] [Root Offset: 8 bytes]
    
    // Note: In production, include your generated FF_HEADER struct and cast it properly.
    auto* root_ptr = reinterpret_cast<std::atomic<uint64_t>*>(m_base + 8);
    root_ptr->store(offset, std::memory_order_release);
}

uint64_t FF_Allocator::get_root_offset() const {
    auto* root_ptr = reinterpret_cast<std::atomic<uint64_t>*>(m_base + 8);
    return root_ptr->load(std::memory_order_acquire);
}

uint64_t FF_Allocator::current_write_head() const {
    return m_head->load(std::memory_order_acquire);
}

StreamView FF_Allocator::view() {
    return StreamView(shared_from_this());
}

} // namespace FastFHIR