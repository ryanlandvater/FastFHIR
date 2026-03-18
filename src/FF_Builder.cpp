/**
 * @file FF_Builder.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief Concurrent lock-free FastFHIR stream builder — implementation.
 * @version 0.1
 * @date 2026-03-18
 * * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 */

#include "FF_Builder.hpp"
#include "FF_Parser.hpp"
#include "../generated_src/FF_Reflection.hpp"
#include <atomic>
#include <stdexcept>
#include <thread>
#include <string>

// OS-Specific Virtual Memory Headers
#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/mman.h>
#endif

namespace FastFHIR {

// =====================================================================
// Constructor / Destructor
// =====================================================================

Builder::Builder(size_t virtual_capacity, uint32_t version)
    : m_base(nullptr),
      m_capacity(virtual_capacity),
      m_stream_head(0),
      m_root_offset(FF_NULL_OFFSET),
      m_root_recovery(FF_RECOVER_UNDEFINED),
      m_version(version),
      m_finalizing(false),
      m_active_mutators(0)
{
#ifdef _WIN32
    // WINDOWS: Reserve address space and commit pages on demand
    m_base = static_cast<BYTE*>(VirtualAlloc(nullptr, m_capacity,
                                             MEM_RESERVE | MEM_COMMIT,
                                             PAGE_READWRITE));
    if (m_base == nullptr) {
        throw std::runtime_error("FastFHIR: Failed to reserve Virtual Memory Arena via VirtualAlloc.");
    }
#else
    // UNIX/macOS: Anonymous private mapping with lazy physical RAM allocation
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    mmap_flags |= MAP_NORESERVE;
#endif
    m_base = static_cast<BYTE*>(mmap(nullptr, m_capacity,
                                     PROT_READ | PROT_WRITE,
                                     mmap_flags,
                                     -1, 0));
    if (m_base == MAP_FAILED) {
        throw std::runtime_error("FastFHIR: Failed to reserve Virtual Memory Arena via mmap.");
    }
#endif

    // Reserve space for the mandatory file header
    m_stream_head.store(FF_FILE_HEADER::HEADER_SIZE, std::memory_order_relaxed);
}

Builder::~Builder() {
#ifdef _WIN32
    if (m_base != nullptr) {
        VirtualFree(m_base, 0, MEM_RELEASE);
    }
#else
    if (m_base != nullptr && m_base != MAP_FAILED) {
        munmap(m_base, m_capacity);
    }
#endif
}

// =====================================================================
// Concurrency Guards
// =====================================================================

bool Builder::try_begin_mutation() {
    if (m_finalizing.load(std::memory_order_acquire)) {
        return false;
    }

    m_active_mutators.fetch_add(1, std::memory_order_acq_rel);

    // Close race where finalize starts after first check but before increment.
    if (m_finalizing.load(std::memory_order_acquire)) {
        m_active_mutators.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }

    return true;
}

void Builder::end_mutation() {
    m_active_mutators.fetch_sub(1, std::memory_order_acq_rel);
}

// =====================================================================
// View Node & Amend Pointer
// =====================================================================

Node Builder::view_node(Offset offset, uint16_t recovery, FF_FieldKind kind) const {
    if (offset == FF_NULL_OFFSET || offset >= total_written()) {
        return Node(); // Returns an empty/invalid node handle
    }

    // Directly constructs the non-owning value object using the stable m_base pointer
    return Node(m_base, total_written(), m_version, offset, recovery, kind);
}

void Builder::amend_pointer(Offset object_offset, size_t field_vtable_offset, Offset new_target_offset) {
    if (!try_begin_mutation()) {
        throw std::runtime_error("FastFHIR: Builder is finalizing; amend is no longer allowed.");
    }

    struct MutationGuard {
        Builder* self;
        ~MutationGuard() { self->end_mutation(); }
    } guard{this};

    if (object_offset > m_capacity || field_vtable_offset > (m_capacity - object_offset) ||
        sizeof(Offset) > (m_capacity - object_offset - field_vtable_offset)) {
        throw std::runtime_error("FastFHIR: Pointer amendment out of bounds.");
    }

    // Atomic store makes concurrent amendments to the same slot data-race free.
    Offset* ptr_location = reinterpret_cast<Offset*>(m_base + object_offset + field_vtable_offset);
    std::atomic_ref<Offset> ptr_atomic(*ptr_location);
    ptr_atomic.store(new_target_offset, std::memory_order_release);
}

// =====================================================================
// ObjectHandle String Lookup (Reflection)
// =====================================================================

PointerPatchProxy ObjectHandle::operator[](std::string_view key_name) const {
    if (m_offset == FF_NULL_OFFSET || m_recovery == FF_RECOVER_UNDEFINED) {
        throw std::runtime_error("FastFHIR: Invalid ObjectHandle; cannot resolve field assignment.");
    }

    // Use the generated reflection engine to find the field metadata
    const std::vector<FF_FieldInfo> fields = reflected_fields(m_recovery);
    
    for (const FF_FieldInfo& field : fields) {
        if (field.name && key_name == std::string_view(field.name)) {
            
            // FastFHIR only supports appending complex types/strings out-of-line
            if (field.kind != FF_FIELD_BLOCK && field.kind != FF_FIELD_ARRAY && field.kind != FF_FIELD_STRING) {
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

void Builder::set_root(Offset offset, uint16_t recovery_tag) {
    if (!try_begin_mutation()) {
        throw std::runtime_error("FastFHIR: Builder is finalizing; set_root is no longer allowed.");
    }

    struct MutationGuard {
        Builder* self;
        ~MutationGuard() { self->end_mutation(); }
    } guard{this};

    m_root_offset = offset;
    m_root_recovery = recovery_tag;
}

std::string_view Builder::finalize(FF_Checksum_Algorithm algo) {
    bool expected = false;
    if (!m_finalizing.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        throw std::runtime_error("FastFHIR: finalize() has already started or completed.");
    }

    // Wait for in-flight append/amend/set_root calls to finish.
    while (m_active_mutators.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }

    if (m_root_offset == FF_NULL_OFFSET) {
        throw std::runtime_error("FastFHIR: Cannot finalize without a root resource.");
    }

    // 1. Claim the final 48 bytes for the checksum footer
    // We use std::memory_order_relaxed here safely because all mutators have drained.
    Offset checksum_start = m_stream_head.fetch_add(FF_CHECKSUM::HEADER_SIZE, std::memory_order_relaxed);
    Size payload_size = checksum_start - FF_FILE_HEADER::HEADER_SIZE;

    // 2. Bake the main file header (with known checksum offset)
    STORE_FF_FILE_HEADER(m_base, m_version, m_root_offset, m_root_recovery, payload_size);

    // 3. Write checksum metadata (returns a pointer to the 32-byte hash block)
    BYTE* hash_dst = STORE_FF_CHECKSUM_METADATA(m_base, checksum_start, algo);
    (void)hash_dst;

    // Return a view from byte 0 to the start of the checksum block.
    // The calling application hashes this view, and writes the result into m_base + checksum_start + 16.
    return std::string_view(reinterpret_cast<const char*>(m_base), checksum_start);
}

} // namespace FastFHIR