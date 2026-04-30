/**
 * @file FF_Memory.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @version 0.1
 * @date 2026-03-26
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file in the project root for terms.
 *
 * @brief Implementation of the FF_Memory class for FastFHIR's Virtual Memory Arena.
 *
 * This file contains the implementation of the FF_Memory class, which manages a large virtual memory
 * arena for concurrent data ingestion in FastFHIR. The memory manager provides methods for creating and
 * managing the memory mapping, as well as the StreamHead class for exclusive access to the write
 * head of the arena. The implementation includes OS-specific code for both Windows and POSIX systems
 * to handle memory mapping and synchronization.
 *
 */

#include "FF_Primitives.hpp"
#include "FF_Memory.hpp"

#include <stdexcept>
#include <system_error>
#include <algorithm>
#include <cstring>
#include <iostream>

// ============================================================================
// OS-Specific Includes & Macros
// ============================================================================
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace FastFHIR
{

    // Verify FF_Memory's layout constants stay in sync with FF_HEADER
    static_assert(Memory::STREAM_HEADER_SIZE == FF_HEADER::HEADER_SIZE,
                  "STREAM_HEADER_SIZE out of sync with FF_HEADER::HEADER_SIZE");
    static_assert(Memory::STREAM_CURSOR_OFFSET == FF_HEADER::STREAM_SIZE,
                  "STREAM_CURSOR_OFFSET out of sync with FF_HEADER::STREAM_SIZE");
    static_assert(Memory::STREAM_PAYLOAD_OFFSET == FF_HEADER::ROOT_OFFSET,
                  "STREAM_PAYLOAD_OFFSET out of sync with FF_HEADER::ROOT_OFFSET");

    namespace
    {

        bool looks_like_fastfhir_header(const uint8_t *base_ptr)
        {
            return std::memcmp(base_ptr, "FFHR", 4) == 0;
        }

        void warn_about_faulted_fastfhir_stream(const char *api_name)
        {
            std::cerr << "Warning: " << api_name
                      << " detected an invalid or incomplete FastFHIR stream.\n"
                      << "Initializing a new stream over the existing memory.\n";
        }

    }

    // ============================================================================
    // StreamHead Implementation
    // ============================================================================

    void Memory::StreamHead::commit(size_t bytes_written)
    {
        if (!m_memory)
            throw std::logic_error("Invalid StreamHead access");

        if (m_staging_offset < Memory::STREAM_HEADER_SIZE)
        {
            if (bytes_written > Memory::STREAM_HEADER_SIZE - m_staging_offset)
            {
                throw std::runtime_error("Staging commit overflow");
            }

            m_staging_offset += bytes_written;
            if (m_staging_offset == Memory::STREAM_HEADER_SIZE)
            {
                std::atomic_ref<uint64_t> head(*m_memory->m_head_ptr);
                head.store(Memory::STREAM_HEADER_SIZE | Memory::STREAM_LOCK_BIT,
                           std::memory_order_release);
            }
            return;
        }

        std::atomic_ref<uint64_t> head(*m_memory->m_head_ptr);
        uint64_t current = head.load(std::memory_order_relaxed);
        if ((current & Memory::STREAM_LOCK_BIT) == 0)
        {
            throw std::logic_error("Stream commit attempted without holding stream lock");
        }
        uint64_t actual_offset = current & OFFSET_MASK;

        if (actual_offset + bytes_written > m_memory->m_capacity)
        {
            throw std::runtime_error("Stream commit exceeds VMA capacity");
        }

        // Keep the stream lock bit set while streaming. This allows multiple
        // commit() calls on the same acquired StreamHead without reacquiring.
        uint64_t new_state = (actual_offset + bytes_written) | Memory::STREAM_LOCK_BIT;
        head.store(new_state, std::memory_order_release);
    }

    // ============================================================================
    // FF_Memory Lifecycle & OS Mapping
    // ============================================================================

    Memory Memory::create(size_t capacity, std::string shm_name)
    {
        uint8_t *base_ptr = nullptr;
        void *os_handle = nullptr;
        int os_fd = -1;
        bool is_new = true;
        const uint64_t total_size = static_cast<uint64_t>(capacity);

#ifdef _WIN32
        HANDLE hMapFile = NULL;
        if (shm_name.empty())
        {
            hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                          total_size >> 32, total_size & 0xFFFFFFFF, NULL);
        }
        else
        {
            hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                          total_size >> 32, total_size & 0xFFFFFFFF, shm_name.c_str());
            is_new = (GetLastError() != ERROR_ALREADY_EXISTS);
        }

        if (!hMapFile)
            throw std::system_error(GetLastError(), std::system_category(), "Win32 CreateFileMappingA failed");
        os_handle = static_cast<void *>(hMapFile);

        base_ptr = static_cast<uint8_t *>(MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, total_size));
        if (!base_ptr)
        {
            CloseHandle(hMapFile);
            throw std::system_error(GetLastError(), std::system_category(), "Win32 MapViewOfFile failed");
        }
#else
        if (shm_name.empty())
        {
            base_ptr = static_cast<uint8_t *>(mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
                                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
            if (base_ptr == MAP_FAILED)
                throw std::system_error(errno, std::system_category(), "POSIX anonymous mmap failed");
        }
        else
        {
            std::string posix_name = (shm_name.front() != '/') ? "/" + shm_name : shm_name;
            os_fd = shm_open(posix_name.c_str(), O_CREAT | O_RDWR, 0666);
            if (os_fd == -1)
                throw std::system_error(errno, std::system_category(), "POSIX shm_open failed");

            struct stat shm_stat;
            fstat(os_fd, &shm_stat);
            is_new = (shm_stat.st_size == 0);

            if (ftruncate(os_fd, capacity) == -1)
            {
                ::close(os_fd);
                throw std::system_error(errno, std::system_category(), "POSIX ftruncate failed");
            }

            base_ptr = static_cast<uint8_t *>(mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
                                                   MAP_SHARED, os_fd, 0));
            if (base_ptr == MAP_FAILED)
            {
                ::close(os_fd);
                throw std::system_error(errno, std::system_category(), "POSIX shared mmap failed");
            }
        }
#endif

        // --- Parser Validation & Fault Recovery ---
        FF_HEADER header(capacity);
        if (is_new || header.validate_full(base_ptr) != FF_SUCCESS)
        {
            // Faulted or brand new: initialize a provisional header region, but do not
            // emit a valid finalized FastFHIR header. This lets Builder distinguish
            // fresh writable memory from a completed archive.
            if (!is_new && looks_like_fastfhir_header(base_ptr))
            {
                // TODO: If header magic/version/offsets are plausible, attempt bounded
                // recovery before zeroing the provisional header region.
                warn_about_faulted_fastfhir_stream("FF_Memory::create");
            }

            std::memset(base_ptr, 0, FF_HEADER::HEADER_SIZE);
        }

        // Create the FF_Memory handle with the initialized core.
        auto allocator = Memory(std::shared_ptr<FF_Memory_t>(new FF_Memory_t(base_ptr, capacity, nullptr, os_handle, os_fd, shm_name)));

        return allocator;
    }

    Memory Memory::createFromFile(const std::filesystem::path &filepath, size_t capacity)
    {
        bool is_new = false;
        uint8_t *base_ptr = nullptr;
        void *file_handle = nullptr;
        void *os_handle = nullptr;
        int os_fd = -1;
        const uint64_t total_size = static_cast<uint64_t>(capacity);

        // Convert path to string for native OS APIs
        std::string path_str = filepath.string();

#ifdef _WIN32
        HANDLE hFile = CreateFileA(path_str.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            throw std::system_error(GetLastError(), std::system_category(), "Win32 CreateFileA failed");
        }
        is_new = (GetLastError() != ERROR_ALREADY_EXISTS);
        file_handle = static_cast<void *>(hFile);

        DWORD bytesReturned;
        if (!DeviceIoControl(hFile, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &bytesReturned, NULL))
        {
            CloseHandle(hFile);
            throw std::system_error(GetLastError(), std::system_category(), "Win32 Set Sparse failed");
        }

        HANDLE hMapFile = CreateFileMappingA(hFile, NULL, PAGE_READWRITE,
                                             total_size >> 32, total_size & 0xFFFFFFFF, NULL);
        if (!hMapFile)
        {
            CloseHandle(hFile);
            throw std::system_error(GetLastError(), std::system_category(), "Win32 CreateFileMappingA failed");
        }
        os_handle = static_cast<void *>(hMapFile);

        base_ptr = static_cast<uint8_t *>(MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, total_size));
        if (!base_ptr)
        {
            CloseHandle(hMapFile);
            CloseHandle(hFile);
            throw std::system_error(GetLastError(), std::system_category(), "Win32 MapViewOfFile failed");
        }

#else
        os_fd = open(path_str.c_str(), O_CREAT | O_RDWR, 0666);
        if (os_fd == -1)
            throw std::system_error(errno, std::system_category(), "POSIX open failed");

        struct stat file_stat;
        if (fstat(os_fd, &file_stat) == -1)
        {
            ::close(os_fd);
            throw std::system_error(errno, std::system_category(), "POSIX fstat failed");
        }

        // Treat as new if it lacks the minimum structural space for the 38-byte header
        is_new = (file_stat.st_size < FF_HEADER::HEADER_SIZE);

        if (static_cast<size_t>(file_stat.st_size) < capacity)
        {
            if (ftruncate(os_fd, capacity) == -1)
            {
                ::close(os_fd);
                throw std::system_error(errno, std::system_category(), "POSIX ftruncate failed");
            }
        }

        base_ptr = static_cast<uint8_t *>(mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
                                               MAP_SHARED, os_fd, 0));
        if (base_ptr == MAP_FAILED)
        {
            ::close(os_fd);
            throw std::system_error(errno, std::system_category(), "POSIX file mmap failed");
        }
#endif

        // --- Parser Validation & Fault Recovery ---
        FF_HEADER header(capacity);
        if (is_new || header.validate_full(base_ptr) != FF_SUCCESS)
        {
            if (!is_new && looks_like_fastfhir_header(base_ptr))
            {
                // TODO: If header magic/version/offsets are plausible, attempt bounded
                // recovery before zeroing the provisional header region.
                warn_about_faulted_fastfhir_stream("FF_Memory::createFromFile");
            }

            std::memset(base_ptr, 0, FF_HEADER::HEADER_SIZE);
        }

        // Create the FF_Memory handle with the initialized core.
        auto allocator = Memory(std::shared_ptr<FF_Memory_t>(
            new FF_Memory_t(base_ptr, capacity, file_handle, os_handle, os_fd, path_str)));

        return allocator;
    }

    // ============================================================================
    // Internal Core Methods (FF_Memory_t)
    // ============================================================================

    // Strict initialization order to match header declaration and prevent -Wreorder warnings
    FF_Memory_t::FF_Memory_t(uint8_t *base, size_t capacity, void *fh, void *osh, int fd, const std::string &name) : 
    m_name(name),
    m_capacity(capacity),
    m_base(base),
    m_head_ptr(reinterpret_cast<uint64_t *>(base + FF_HEADER::STREAM_SIZE)),
    m_file_handle(fh),
    m_os_handle(osh),
    m_os_fd(fd) {}

    void FF_Memory_t::close() noexcept
    {
#ifdef _WIN32
        if (m_base)
        {
            UnmapViewOfFile(m_base);
            m_base = nullptr;
        }
        if (m_os_handle)
        {
            CloseHandle(static_cast<HANDLE>(m_os_handle));
            m_os_handle = nullptr;
        }
        if (m_file_handle)
        {
            CloseHandle(static_cast<HANDLE>(m_file_handle));
            m_file_handle = nullptr;
        }
#else
        if (m_base && m_base != MAP_FAILED)
        {
            munmap(m_base, m_capacity);
            m_base = nullptr;
        }
        if (m_os_fd != -1)
        {
            ::close(m_os_fd);
            m_os_fd = -1;
        }
        // shm_unlink deliberately omitted — Named SHM persists in /dev/shm for reconnect after restart.
#endif
    }

    FF_Memory_t::~FF_Memory_t()
    {
        close(); // idempotent — nulls out handles, so double-close is safe
    }

    // ============================================================================
    // Ingestion & Lock Management
    // ============================================================================

    uint64_t FF_Memory_t::claim_space(size_t bytes)
    {
        std::atomic_ref<uint64_t> head(*m_head_ptr);
        uint64_t current = head.load(std::memory_order_acquire);

        while (true)
        {
            // 1. If locked, park the thread at the OS level
            if (current & Memory::STREAM_LOCK_BIT)
            {
                head.wait(current, std::memory_order_acquire);
                current = head.load(std::memory_order_acquire);
                continue;
            }

            // 2. Check capacity bounds safely without the lock bit
            if ((current & Memory::OFFSET_MASK) + bytes > m_capacity)
            {
                throw std::runtime_error("FastFHIR VMA Capacity Exceeded");
            }

            // 3. Attempt strong swap to claim space
            if (head.compare_exchange_strong(current, current + bytes,
                                             std::memory_order_relaxed,
                                             std::memory_order_acquire))
            {
                // Success! Return the base offset where writing should start.
                return current & Memory::OFFSET_MASK;
            }
        }
    }

    std::optional<Memory::StreamHead> FF_Memory_t::try_acquire_stream()
    {
        std::atomic_ref<uint64_t> head(*m_head_ptr);
        uint64_t current = head.load(std::memory_order_relaxed);

        while (true)
        {
            // If the lock bit is already 1, another socket is streaming
            if (current & Memory::STREAM_LOCK_BIT)
                return std::nullopt;

            // Try to flip the 63rd bit to 1
            if (head.compare_exchange_weak(current, current | Memory::STREAM_LOCK_BIT,
                                           std::memory_order_acquire))
            {
                return Memory::StreamHead(this);
            }
        }
    }

    void FF_Memory_t::reset(size_t committed_size)
    {
        if (committed_size > m_capacity)
        {
            throw std::runtime_error("FastFHIR: reset size exceeds VMA capacity");
        }

        std::atomic_ref<uint64_t> head(*m_head_ptr);
        uint64_t current = head.load(std::memory_order_acquire);
        if (current & Memory::STREAM_LOCK_BIT)
        {
            throw std::logic_error("FastFHIR: cannot reset while a StreamHead is active");
        }
        head.store(committed_size, std::memory_order_release);
        head.notify_all();
    }

    void FF_Memory_t::release_stream_lock() noexcept
    {
        // Called if StreamHead is destroyed without calling commit() (e.g., socket closed prematurely)
        // Strip the lock bit atomically using fetch_and, then wake waiting threads
        std::atomic_ref<uint64_t> head(*m_head_ptr);
        head.fetch_and(Memory::OFFSET_MASK, std::memory_order_release);
        head.notify_all();
    }

    void FF_Memory_t::truncate_file(size_t size)
    {
#ifdef _WIN32
        if (!m_file_handle)
            return;
        HANDLE hFile = static_cast<HANDLE>(m_file_handle);
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(size);
        SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
        SetEndOfFile(hFile);
#else
        if (m_os_fd == -1)
            return;
        ftruncate(m_os_fd, static_cast<off_t>(size));
#endif
    }

} // namespace FastFHIR
