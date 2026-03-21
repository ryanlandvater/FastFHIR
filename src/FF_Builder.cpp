/**
 * @file FF_Builder.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief Concurrent lock-free FastFHIR stream builder — implementation.
 * @version 0.1
 * @date 2026-03-18
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file in the project root for terms.
 */

#include "FF_Builder.hpp"
#include "FF_Parser.hpp"
#include "../generated_src/FF_Reflection.hpp"
#include <atomic>
#include <stdexcept>
#include <thread>
#include <string>
#include <iostream>

// OS-Specific Virtual Memory Headers
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace FastFHIR
{

    // =====================================================================
    // Constructor / Destructor
    // =====================================================================

    Builder::Builder(size_t size_hint, size_t virtual_capacity, uint32_t version)
        : m_base(nullptr),
          // If the user provides a size hint larger than the default virtual capacity,
          // use it (doubled for safety). Otherwise, use the default virtual capacity.
          m_capacity(size_hint > virtual_capacity ? size_hint * 2 : virtual_capacity),
          m_stream_head(0),
          m_checksum_offset(FF_NULL_OFFSET),
          m_root_offset(FF_NULL_OFFSET),
          m_root_recovery(FF_RECOVER_UNDEFINED),
          m_version(version),
          m_finalizing(false),
          m_active_mutators(0)
    {
#ifdef _WIN32
        // WINDOWS: Reserve address space and commit pages on demand
        m_base = static_cast<BYTE *>(VirtualAlloc(nullptr, m_capacity,
                                                  MEM_RESERVE | MEM_COMMIT,
                                                  PAGE_READWRITE));
        if (m_base == nullptr)
        {
            throw std::runtime_error("FastFHIR: Failed to reserve Virtual Memory Arena via VirtualAlloc.");
        }
#else
        // UNIX/macOS: Anonymous private mapping with lazy physical RAM allocation
        int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
        mmap_flags |= MAP_NORESERVE;
#endif
        m_base = static_cast<BYTE *>(mmap(nullptr, m_capacity,
                                          PROT_READ | PROT_WRITE,
                                          mmap_flags,
                                          -1, 0));
        if (m_base == MAP_FAILED)
        {
            throw std::runtime_error("FastFHIR: Failed to reserve Virtual Memory Arena via mmap.");
        }
#endif

        // Reserve space for the mandatory stream header
        m_stream_head.store(FF_HEADER::HEADER_SIZE, std::memory_order_relaxed);
    }

    Builder::~Builder()
    {
#ifdef _WIN32
        if (m_base != nullptr)
        {
            VirtualFree(m_base, 0, MEM_RELEASE);
        }
#else
        if (m_base != nullptr && m_base != MAP_FAILED)
        {
            munmap(m_base, m_capacity);
        }
#endif
    }

    // =====================================================================
    // Concurrency Guards
    // =====================================================================

    bool Builder::try_begin_mutation()
    {
        if (m_finalizing.load(std::memory_order_acquire))
        {
            return false;
        }

        m_active_mutators.fetch_add(1, std::memory_order_acq_rel);

        // Close race where finalize starts after first check but before increment.
        if (m_finalizing.load(std::memory_order_acquire))
        {
            m_active_mutators.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }

        return true;
    }

    void Builder::end_mutation()
    {
        m_active_mutators.fetch_sub(1, std::memory_order_acq_rel);
    }

    // =====================================================================
    // View Node & Amend Pointer
    // =====================================================================

    Node Builder::view_node(Offset offset, RECOVERY_TAG recovery, FF_FieldKind kind) const
    {
        // 1. Snapshot the atomic boundary once
        Size current_total = total_written();

        if (offset == FF_NULL_OFFSET || offset >= current_total)
        {
            return Node();
        }

        // 2. Use the exact same boundary for the Node's validation
        return Node(m_base, current_total, m_version, offset, recovery, kind);
    }

    void Builder::amend_pointer(Offset object_offset, size_t field_vtable_offset, Offset new_target_offset)
    {
        if (!try_begin_mutation())
        {
            throw std::runtime_error("FastFHIR: Builder is finalizing; amend is no longer allowed.");
        }

        struct MutationGuard
        {
            Builder *self;
            ~MutationGuard() { self->end_mutation(); }
        } guard{this};

        if (object_offset > m_capacity || field_vtable_offset > (m_capacity - object_offset) ||
            sizeof(Offset) > (m_capacity - object_offset - field_vtable_offset))
        {
            throw std::runtime_error("FastFHIR: Pointer amendment out of bounds.");
        }

        // Atomic store makes concurrent amendments to the same slot data-race free.
        Offset *ptr_location = reinterpret_cast<Offset *>(m_base + object_offset + field_vtable_offset);
        std::atomic_ref<Offset> ptr_atomic(*ptr_location);
        Offset expected = FF_NULL_OFFSET;
        if (!ptr_atomic.compare_exchange_strong(expected, new_target_offset,
                                                std::memory_order_release, std::memory_order_relaxed))
        {
            std::string msg = "FastFHIR: Pointer amendment failed — attempted to insert offset " + std::to_string(new_target_offset) + " into object at offset " + std::to_string(object_offset) + ", field vtable offset " + std::to_string(field_vtable_offset) + ". This field was already assigned. Attempting to patch an already-assigned pointer risks orphaning elements of the stream. " + "If you are inserting a new object, ensure you are not overwriting an existing reference. " + "(Concurrent modification detected.)";
            throw std::runtime_error(msg);
        }
    }

    // =====================================================================
    // ObjectHandle String Lookup (Reflection)
    // =====================================================================

    PointerPatchProxy ObjectHandle::operator[](std::string_view key_name) const
    {
        if (m_offset == FF_NULL_OFFSET || m_recovery == FF_RECOVER_UNDEFINED)
        {
            throw std::runtime_error("FastFHIR: Invalid ObjectHandle; cannot resolve field assignment.");
        }

        // Use the generated reflection engine to find the field metadata
        const std::vector<FF_FieldInfo> fields = reflected_fields(m_recovery);

        for (const FF_FieldInfo &field : fields)
        {
            if (field.name && key_name == std::string_view(field.name))
            {

                // FastFHIR only supports appending complex types/strings out-of-line
                if (field.kind != FF_FIELD_BLOCK && field.kind != FF_FIELD_ARRAY && field.kind != FF_FIELD_STRING)
                {
                    throw std::runtime_error("FastFHIR: Assignment supports only pointer-backed fields (BLOCK/ARRAY/STRING).");
                }

                return PointerPatchProxy(m_builder, PointerRef{m_offset, field.field_offset});
            }
        }

        throw std::runtime_error("FastFHIR: Field key '" + std::string(key_name) + "' not found on current object.");
    }

    // =====================================================================
    // Finalization & Checksums
    // =====================================================================

    void Builder::set_root(const ObjectHandle &handle)
    {
        if (!try_begin_mutation())
        {
            throw std::runtime_error("FastFHIR: Builder is finalizing; set_root is no longer allowed.");
        }

        struct MutationGuard
        {
            Builder *self;
            ~MutationGuard() { self->end_mutation(); }
        } guard{this};

        m_root_offset = handle.offset();
        m_root_recovery = handle.recovery();
    }

    std::string_view Builder::finalize(FF_Checksum_Algorithm algo, const HashCallback &hasher)
    {
        bool expected = false;
        if (!m_finalizing.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            throw std::runtime_error("FastFHIR: finalize() has already started or completed.");
        }

        // Wait for in-flight append/amend/set_root calls to finish.
        while (m_active_mutators.load(std::memory_order_acquire) != 0)
        {
            std::this_thread::yield();
        }

        // Finalization sanity check: Ensure a root resource was set and is within bounds
        if (m_root_offset >= m_stream_head.load(std::memory_order_relaxed) || m_root_offset >= m_capacity)
        {
            throw std::runtime_error("FastFHIR: Cannot finalize without a valid in-range root resource.");
        }

        // m_checksum_offset is the exact byte where the 48-byte footer begins
        m_checksum_offset = m_stream_head.fetch_add(FF_CHECKSUM::HEADER_SIZE, std::memory_order_relaxed);
        Size final_file_size = m_stream_head.load(std::memory_order_relaxed);

        if (hasher == nullptr || algo == FF_CHECKSUM_NONE)
        {
            std::cerr << "[FastFHIR] Warning: No hash function provided; file will be emitted with zeroed checksum.\n";
            algo = FF_CHECKSUM_NONE;
        }

        STORE_FF_HEADER(m_base, m_version, m_checksum_offset, m_root_offset, m_root_recovery, final_file_size);

        // Writes the 16 bytes of metadata, returns a pointer to byte 16 (the 32-byte slot)
        BYTE *hash_dst = STORE_FF_CHECKSUM_METADATA(m_base, m_checksum_offset, algo);

        if (hasher != nullptr && algo != FF_CHECKSUM_NONE)
        {

            // Hash the payload + the 16 bytes of metadata, stopping exactly where the hash slot begins.
            Size bytes_to_hash = m_checksum_offset + FF_CHECKSUM::HASH_DATA;
            std::vector<BYTE> hash_value = hasher(m_base, bytes_to_hash);

            size_t copy_len = std::min(hash_value.size(), static_cast<size_t>(FF_MAX_HASH_BYTES));
            std::memcpy(hash_dst, hash_value.data(), copy_len);
        }

        return std::string_view(reinterpret_cast<const char *>(m_base), final_file_size);
    }
} // namespace FastFHIR
