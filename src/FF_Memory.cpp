/**
 * @file FF_Memory.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @version 0.1
 * @date 2026-03-26
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
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

        // Releases an OS handle on scope exit until ownership passes to the core,
        // so no early-exit path in createFromFile needs its own close call.
        template <typename Release>
        struct ScopeExit
        {
            Release release;
            bool armed = true;
            ~ScopeExit()
            {
                if (armed)
                    release();
            }
        };

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

#ifdef _WIN32
        const uint64_t total_size = static_cast<uint64_t>(capacity);
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

            // Size the segment ONLY when this call created it. A POSIX shared
            // segment can be sized once on Darwin, so ftruncate on an existing
            // one fails with EINVAL -- which made attaching to a live arena
            // (the cross-process case the SHM backing exists for) impossible.
            if (is_new)
            {
                if (ftruncate(os_fd, capacity) == -1)
                {
                    ::close(os_fd);
                    throw std::system_error(errno, std::system_category(),
                                            "FastFHIR: cannot size shared segment " + posix_name);
                }
            }
            else
            {
                // Adopt what the segment already is: the producer chose it, and
                // mapping a different length would hand out addresses the other
                // processes do not share.
                capacity = static_cast<size_t>(shm_stat.st_size);
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

        // ATTACHING MUST NOT DISTURB THE PRODUCER. This used to validate the
        // header and, on failure, zero FF_HEADER::HEADER_SIZE bytes -- which
        // spans the live write head at bytes 8-15. An in-progress arena has no
        // finalized header to validate, so a second process attaching to one
        // reset its cursor to zero and both then claimed the same space.
        //
        // A freshly created segment is already zero-filled by the OS, and an
        // anonymous mapping likewise, so there is nothing to initialize; an
        // existing segment belongs to whoever is writing it.

        // Create the FF_Memory handle with the initialized core.
        auto allocator = Memory(std::shared_ptr<FF_Memory_t>(new FF_Memory_t(base_ptr, capacity, nullptr, os_handle, os_fd, shm_name)));

        return allocator;
    }

    Memory Memory::createFromFile(const std::filesystem::path &filepath, size_t capacity)
    {
        return mapFile(filepath, capacity, FileAccess::Amend);
    }

    Memory Memory::openReadOnly(const std::filesystem::path &filepath)
    {
        // `capacity` is meaningless here: a read-only arena maps exactly the
        // file, so mapFile derives it from the file itself.
        return mapFile(filepath, 0, FileAccess::Read);
    }

    Memory Memory::mapFile(const std::filesystem::path &filepath, size_t capacity, FileAccess access)
    {
        // Preconditions: for FileAccess::Amend, `capacity` is the sparse
        // reservation and must be at least the existing file's size.
        const bool read_only = access == FileAccess::Read;

        const std::string path_str = filepath.string();
        uint8_t *base_ptr = nullptr;
        void *file_handle = nullptr;
        void *os_handle = nullptr;
        int os_fd = -1;
        size_t on_disk = 0;

        // --- Open, and learn what is already there -------------------------------
        // A writable arena opens or creates; a read-only one requires the file
        // and never creates it, so a mistyped path leaves nothing behind.
#ifdef _WIN32
        const DWORD disposition = read_only ? OPEN_EXISTING : OPEN_ALWAYS;
        HANDLE hFile = CreateFileA(path_str.c_str(),
                                   read_only ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE),
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   disposition, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            throw std::system_error(GetLastError(), std::system_category(), "FastFHIR: cannot open " + path_str);
        ScopeExit close_file{[&] { CloseHandle(hFile); }};
        LARGE_INTEGER file_size;
        if (!GetFileSizeEx(hFile, &file_size))
            throw std::system_error(GetLastError(), std::system_category(), "FastFHIR: cannot size " + path_str);
        on_disk = static_cast<size_t>(file_size.QuadPart);
#else
        const int flags = read_only ? O_RDONLY : (O_CREAT | O_RDWR);
        os_fd = open(path_str.c_str(), flags, 0666);
        if (os_fd == -1)
            throw std::system_error(errno, std::system_category(), "FastFHIR: cannot open " + path_str);
        ScopeExit close_file{[&] { ::close(os_fd); }};
        struct stat file_stat;
        if (fstat(os_fd, &file_stat) == -1)
            throw std::system_error(errno, std::system_category(), "FastFHIR: cannot size " + path_str);
        on_disk = static_cast<size_t>(file_stat.st_size);
#endif

        // Whether these bytes are a stream is not this layer's question; whether
        // there are any bytes to map is.
        const bool is_new = on_disk == 0;
        if (read_only)
        {
            if (is_new)
                throw std::runtime_error("FastFHIR: " + path_str + " is empty; there is nothing to map");
            capacity = on_disk;
        }
        else if (capacity < on_disk)
        {
            // Mapping less than the stream would hide its tail from the Builder.
            throw std::invalid_argument("FastFHIR: capacity " + std::to_string(capacity) +
                                        " is smaller than " + path_str + " (" +
                                        std::to_string(on_disk) + " bytes)");
        }

        // --- Map -----------------------------------------------------------------
        // A writable arena is a sparse reservation of `capacity` bytes; the file
        // grows to it and only touched pages commit. A read-only arena maps the
        // file as it is and grows nothing.
#ifdef _WIN32
        DWORD bytes_returned;
        if (!read_only && !DeviceIoControl(hFile, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &bytes_returned, NULL))
            throw std::system_error(GetLastError(), std::system_category(), "FastFHIR: cannot make " + path_str + " sparse");
        const uint64_t total_size = static_cast<uint64_t>(capacity);
        HANDLE hMapFile = CreateFileMappingA(hFile, NULL, read_only ? PAGE_READONLY : PAGE_READWRITE,
                                             static_cast<DWORD>(total_size >> 32),
                                             static_cast<DWORD>(total_size & 0xFFFFFFFF), NULL);
        if (!hMapFile)
            throw std::system_error(GetLastError(), std::system_category(), "FastFHIR: cannot map " + path_str);
        ScopeExit close_mapping{[&] { CloseHandle(hMapFile); }};
        base_ptr = static_cast<uint8_t *>(MapViewOfFile(hMapFile, read_only ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS,
                                                        0, 0, total_size));
        if (!base_ptr)
            throw std::system_error(GetLastError(), std::system_category(), "FastFHIR: cannot map " + path_str);
        close_mapping.armed = false;
        file_handle = static_cast<void *>(hFile);
        os_handle = static_cast<void *>(hMapFile);
#else
        if (!read_only && on_disk < capacity && ftruncate(os_fd, static_cast<off_t>(capacity)) == -1)
            throw std::system_error(errno, std::system_category(), "FastFHIR: cannot reserve " + path_str);
        base_ptr = static_cast<uint8_t *>(mmap(nullptr, capacity, read_only ? PROT_READ : (PROT_READ | PROT_WRITE),
                                               MAP_SHARED, os_fd, 0));
        if (base_ptr == MAP_FAILED)
            throw std::system_error(errno, std::system_category(), "FastFHIR: cannot map " + path_str);
#endif
        close_file.armed = false;

        // From here the core owns the mapping and the handles, so a refusal below
        // unmaps and closes through its destructor.
        Memory memory(std::shared_ptr<FF_Memory_t>(
            new FF_Memory_t(base_ptr, capacity, file_handle, os_handle, os_fd, path_str)));
        memory.m_core->m_read_only = read_only;
        memory.m_core->m_file_backed = true;
        // What the OS says the file is, for an existing one. `capacity` is the
        // sparse RESERVATION and says nothing about how many bytes exist, so it
        // cannot bound a reader on its own; a new file has no meaningful size.
        if (!is_new)
            memory.m_core->m_disk_size = on_disk;

        // Deliberately nothing else. This function maps bytes; it does not know
        // what a FastFHIR stream is, and it never writes one. It used to
        // validate the header here and ZERO it when validation failed, which is
        // how a receiver erased a damaged submission just by opening it. Whether
        // an arena holds something appendable is the Builder's question, and the
        // Builder asks it (src/FF_Builder.cpp). A freshly created file is already
        // zero-filled by the OS, so there is nothing to initialize either.
        return memory;
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
#ifdef _WIN32
    m_file_handle(fh),
    m_os_handle(osh),
#endif
    m_os_fd(fd) {
#ifndef _WIN32
        (void)fh;
        (void)osh;
#endif
    }

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

    void FF_Memory_t::require_writable(const char *operation) const
    {
        if (m_read_only)
            throw std::runtime_error(std::string("FastFHIR: ") + operation +
                                     " on a read-only arena (Memory::openReadOnly): " + m_name);
    }

    uint64_t FF_Memory_t::claim_space(size_t bytes)
    {
        require_writable("claim_space");
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
        require_writable("try_acquire_stream");
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
        require_writable("reset");
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
        require_writable("truncate_file");
        // Only a file has a tail to trim. An anonymous arena has no backing at
        // all, and a shared segment is sized once by the process that created
        // it -- ftruncate on one fails (EINVAL on Darwin), which is why this
        // check has to come before the error handling below and not after.
        if (!m_file_backed)
            return;
#ifdef _WIN32
        if (!m_file_handle)
            return;
        // Result deliberately unchecked here, unlike POSIX: Windows refuses to
        // shorten a file while a view of it is mapped (ERROR_USER_MAPPED_FILE),
        // so this fails on every live arena and the file keeps its sparse
        // reservation. Throwing would fail every finalize on Windows; the real
        // fix is to unmap first. Tracked in CAP_EXAMPLE_handoff.md T2.
        HANDLE hFile = static_cast<HANDLE>(m_file_handle);
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(size);
        SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
        SetEndOfFile(hFile);
#else
        if (m_os_fd == -1)
            return;
        if (ftruncate(m_os_fd, static_cast<off_t>(size)) == -1)
            throw std::system_error(errno, std::system_category(),
                                    "FastFHIR: cannot truncate " + m_name + " to " + std::to_string(size) + " bytes");
#endif
    }

} // namespace FastFHIR
