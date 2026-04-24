/**
 * @file FF_Builder.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief Concurrent lock-free FastFHIR stream builder.
 * @version 0.1
 * @date 2026-03-18
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file in the project root for terms.
 */
#pragma once

#include "FF_Parser.hpp"
#include "FF_Utilities.hpp"
#include <atomic>
#include <stdexcept>
#include <string_view>
#include <functional>

struct PyMutableEntry;

namespace FastFHIR {
class AdvancedBuilderAccess;
namespace Reflective {
class Node;
struct Entry;
class MutableEntry;
class ObjectHandle;
}

// =====================================================================
// BUILDER
// =====================================================================
/**
 * @brief Builder class for constructing FastFHIR binary streams in a concurrent, lock-free manner.
 * 
 * The Builder manages a large virtual memory arena and allows multiple threads to append data concurrently.
 * It provides thread-safe methods for appending data, setting the root resource, and finalizing the stream with a checksum.
 * The Builder also includes proxy classes for safely patching pointers and array entries in an active concurrent context.
 * 
 */
class Builder {
    friend class FastFHIR::AdvancedBuilderAccess;

    Memory              m_memory;
    BYTE* const         m_base;
    Offset              m_checksum_offset;
    Offset              m_root_offset;
    RECOVERY_TAG        m_root_recovery;
    FHIR_VERSION        m_fhir_rev;
    uint32_t            m_ff_version;
    std::atomic<bool>   m_finalizing;
    std::atomic<uint64_t> m_active_mutators;

    bool try_begin_mutation();
    void end_mutation();

public:
    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;
    Builder(Builder&&) = delete;
    Builder& operator=(Builder&&) = delete;
    const Memory& memory() const { return m_memory; }
    const FHIR_VERSION FhirVersion () const { return m_fhir_rev; }

    /**
     * @brief Constructs a builder bound to an existing Virtual Memory Arena.
     * 
     * @param memory Shared pointer to an initialized FF_Memory providing the arena for building or modifying the stream.
     * @param version FHIR version to target for schema-specific encoding rules (default: R5).
     */
    explicit Builder(const Memory& memory, FHIR_VERSION fhir_revision = FHIR_VERSION_R5);
    ~Builder();

    /**
     * @brief Generates a lightweight, snapshot of the current stream.
     * Creating this is nearly zero-cost as it only populates CPU registers.
     * 
     * @return A new Parser instance that can be used to read the current state of the stream without 
     * interfering with ongoing mutations.
     */
    Parser query() const {
        return Parser(m_memory);
    }

    // --- Lock-Free Concurrent Appending ---

    /**
     * @brief The core concurrent ingestion method. Cleaned up to use TypeTraits.
     */
    template<typename T_Data>
    Offset append(const T_Data& data) {
        if (!try_begin_mutation()) {
            throw std::runtime_error("FastFHIR: Builder is finalizing; append is no longer allowed.");
        }

        struct MutationGuard {
            Builder* self;
            ~MutationGuard() { self->end_mutation(); }
        } guard{this};

        // Automatically resolved via ffc.py generated traits
        Size data_size = TypeTraits<T_Data>::size(data, m_fhir_rev);
        
        // Thread-safe claim of space in the arena for the new data
        Offset offset = m_memory.claim_space(data_size);
        
        // Thread-safe write of the data into the claimed space
        TypeTraits<T_Data>::store(m_base, offset, data, m_fhir_rev);
        
        // Return the offset where the data was written for potential pointer patching
        return offset;
    }

    /**
     * @brief Appends data to the stream and returns a thread-safe handle for [] assignments.
     */
    template<typename T_Data>
    Reflective::ObjectHandle append_obj(const T_Data& data);

    /**
     * @brief Overload for strongly-typed Offset Arrays.
     * Bypasses TypeTraits to dynamically inject the semantic array tag.
     */
    Offset append(const std::vector<Offset>& offsets, RECOVERY_TAG semantic_tag) {
        if (!try_begin_mutation()) {
            throw std::runtime_error("FastFHIR: Builder is finalizing; append is no longer allowed.");
        }

        struct MutationGuard {
            Builder* self;
            ~MutationGuard() { self->end_mutation(); }
        } guard{this};

        // 1. Calculate Size directly
        uint32_t count = static_cast<uint32_t>(offsets.size());
        Size data_size = FF_ARRAY::HEADER_SIZE + (count * 8); 
        
        // 2. Thread-safe claim of space
        Offset offset = m_memory.claim_space(data_size);
        
        // 3. Thread-safe write of data with the injected tag
        Offset write_head = offset;
        STORE_FF_ARRAY_HEADER(m_base, write_head, FF_ARRAY::OFFSET, 8, count, semantic_tag);
        for (Offset off : offsets) {
            STORE_U64(m_base + write_head, off);
            write_head += 8;
        }
        
        return offset;
    }

    /**
     * @brief Overload for appending offset arrays and returning a proxy handle.
     */
    Reflective::ObjectHandle append_obj(const std::vector<Offset>& offsets, RECOVERY_TAG semantic_tag);

    /**
     * @brief Instantiates a read-only Node directly from a known offset mid-stream.
     */
    Reflective::Node view_node(Offset offset, RECOVERY_TAG recovery, FF_FieldKind kind = FF_FIELD_BLOCK) const;

    /**
     * @brief Low-level mutable access to a V-Table pointer for amending data.
     */
    void amend_pointer(Offset object_offset, size_t field_vtable_offset, Offset new_target_offset);

    /**
     * @brief Low-level mutable access to a V-Table pointer for amending polymorphic data.
     */
    void amend_resource(Offset object_offset, size_t field_vtable_offset, Offset new_target_offset, RECOVERY_TAG new_tag);

    /**
     * @brief Low-level mutable access to a V-Table variant for amending polymorphic entry data (ie FHIR [x] entries).
     */
    void amend_variant(Offset object_offset, size_t field_vtable_offset, uint64_t raw_bits, RECOVERY_TAG new_tag);
    
    /**
     * @brief Low-level mutable access to a V-Table slot for amending fixed-schema primitives.
     */
    template <typename T>
    requires std::is_arithmetic_v<T>
    void amend_scalar(Offset object_offset, size_t field_vtable_offset, T val);
    
    // --- Finalization & Checksums ---

    /** 
     * @brief Sets the root resource pointer and recovery tag in the header. 
     * [NOTE] This function must be called before finalize().
     */
    void set_root(const Reflective::ObjectHandle& handle);

    /**
     * @brief Get the current mutable root handle, if one is set.
     * @return A valid ObjectHandle when root metadata is populated, otherwise a null handle.
     */
    Reflective::ObjectHandle root_handle() const;
    
    using HashCallback = std::function<std::vector<BYTE>(const unsigned char* byte_start, Size bytes_to_hash)>;
    /**
     * @brief Bakes the File Header, and executes the provided hashing callback to seal the file.
     * @param algo The cryptographic algorithm tag to write into the metadata.
     * @param hasher A callback that takes the payload and returns the raw hash bytes.
     * @return A view of the complete, sealed file (including the checksum footer) ready for network transmission.
     */
    Memory::View finalize(FF_Checksum_Algorithm algo = FF_CHECKSUM_NONE, const HashCallback& hasher = nullptr);

    protected:
    Offset allocate_raw(Size size);
    FF_Result write_offset_at(Offset target_addr, Offset child_offset);
};

/**
 * @brief AdvancedBuilderAccess provides low-level, unsafe access to the Builder's internal methods for expert use cases.
 * 
 * This class is intentionally not documented in detail, as its methods are unsafe and meant for advanced users who 
 * understand the internal workings of the Builder. Use with caution, as improper use can lead to data corruption 
 * or invalid FastFHIR streams.
 * 
 */
class AdvancedBuilderAccess {
    Builder*const m_builder;
public:
    AdvancedBuilderAccess(Builder& builder) : m_builder(&builder) {}
    AdvancedBuilderAccess() = delete;
    AdvancedBuilderAccess(const AdvancedBuilderAccess&) = delete;
    AdvancedBuilderAccess& operator=(const AdvancedBuilderAccess&) = delete;
    AdvancedBuilderAccess(AdvancedBuilderAccess&&) = delete;
    AdvancedBuilderAccess& operator=(AdvancedBuilderAccess&&) = delete;

    /**
     * @brief Unsafely allocates raw memory within the builder's arena.
     * 
     * @param size The size of the memory to allocate.
     * @return Offset The offset of the allocated memory within the builder's arena.
     */
    Offset UNSAFE_allocate_raw(Size size) {
        return m_builder ? m_builder->allocate_raw(size) : FF_NULL_OFFSET;
    }

    /**
    * @brief Unsafely writes an offset directly (non-atomically) within the builder's arena.
    * 
    * @param target_addr The address of the target offset to write.
    * @param child_offset The new offset value to write.
    */
    FF_Result UNSAFE_write_offset_at(Offset target_addr, Offset child_offset) {
        return m_builder ? m_builder->write_offset_at(target_addr, child_offset) : 
        FF_Result{FF_FAILURE,"AdvancedBuilderAccess: Builder instance is null."};
    }
};

// =====================================================================
// PROXIES & HANDLES
// =====================================================================
namespace Reflective {
/**
 * @brief Ephemeral proxy object returned by ObjectHandle::operator[].
 * Allows assigning either an existing Offset or dynamically appending new data.
 * Thin coordinate handle: stores builder context + Entry coordinates without inheritance.
 */
class MutableEntry {
    Builder* m_builder = nullptr;
    const BYTE* m_base = nullptr;
    Offset m_parent_offset = FF_NULL_OFFSET;
    uint32_t m_vtable_offset = 0;
    RECOVERY_TAG m_recovery = FF_RECOVER_UNDEFINED;
    FF_FieldKind m_kind = FF_FIELD_UNKNOWN;
    
public:
    MutableEntry() = default;
    MutableEntry(Builder* b, Offset p, uint32_t v, RECOVERY_TAG r, FF_FieldKind k)
    : m_builder(b), m_base(b ? b->memory().base() : nullptr), 
      m_parent_offset(p), m_vtable_offset(v), m_recovery(r), m_kind(k) {}

    // Materialize Entry view from stored coordinates
    Entry as_entry() const;
    
    ObjectHandle as_handle() const;

    MutableEntry& operator=(const ObjectHandle& child);

    template <typename T_Data>
    requires (!std::is_arithmetic_v<T_Data>)
    Offset operator=(const T_Data& data);

    template <typename T>
    requires std::is_arithmetic_v<T>
    MutableEntry& operator=(T val);

    MutableEntry& operator=(const std::vector<Offset>& offsets);

    // Direct access for Entry-like queries
    explicit operator bool() const {
        return m_base != nullptr && m_parent_offset != FF_NULL_OFFSET;
    }

    // Builder and coordinate accessors
    Builder* get_builder() const { return m_builder; }
    Offset offset() const {
        return (m_parent_offset == FF_NULL_OFFSET) ? FF_NULL_OFFSET : (m_parent_offset + static_cast<Offset>(m_vtable_offset));
    }

    // Lazy materialization forwarding (delegates to as_handle() then as_node())
    operator Reflective::Node() const;
    operator ObjectHandle() const;
    Node as_node() const;
    
    // Query methods (implemented inline after ObjectHandle definition)
    bool is_array() const;
    bool is_object() const;
    bool is_string() const;
    bool is_scalar() const;
    size_t size() const;
    std::vector<FF_FieldInfo> fields() const;
    std::vector<std::string_view> keys() const;
    
    // Field/index access
    MutableEntry operator[](FF_FieldKey key) const;
    MutableEntry operator[](size_t index) const;

private:
    friend struct ::PyMutableEntry;
    void validate_assignment(RECOVERY_TAG child_tag) const;
};

// =====================================================================
// OBJECT HANDLE (Thread-Local Data Blocks Abstraction)
// =====================================================================
/**
 * @brief Thread-local handle representing a specific parent object being built.
 * Replaces the unsafe global operator[] on the Builder itself.
 */

class ObjectHandle {
    Builder* m_builder = nullptr;
    Offset m_offset = FF_NULL_OFFSET;
    RECOVERY_TAG m_recovery = FF_RECOVER_UNDEFINED;
    
public:
    ObjectHandle() = default;  // Default-constructible to null handle
    
    ObjectHandle(Builder* builder, Offset offset, RECOVERY_TAG recovery = FF_RECOVER_UNDEFINED)
    : m_builder(builder), m_offset(offset), m_recovery(recovery) {
        if (offset != FF_NULL_OFFSET && recovery == FF_RECOVER_UNDEFINED)
            throw std::invalid_argument("FastFHIR: Cannot instantiate an ObjectHandle with valid offset but UNDEFINED recovery tag.");
    }
    
    Builder* get_builder() const { return m_builder; }
    Offset offset() const { return m_offset; }
    RECOVERY_TAG recovery() const { return m_recovery; }
    explicit operator bool() const { return m_builder != nullptr && m_offset != FF_NULL_OFFSET; }
    
    // Spawns a fresh, safely bounds-checked snapshot using the latest builder arena size
    Node as_node() const {
        if (m_builder == nullptr || m_offset == FF_NULL_OFFSET) return {};
        FF_FieldKind node_kind = IsArrayTag(m_recovery) ? FF_FIELD_ARRAY : Recovery_to_Kind(m_recovery);
        return m_builder->view_node(m_offset, m_recovery, node_kind);
    }

    bool is_array() const { return as_node().is_array(); }
    bool is_object() const { return as_node().is_object(); }
    bool is_string() const { return as_node().is_string(); }
    bool is_scalar() const { return as_node().is_scalar(); }
    size_t size() const { return as_node().size(); }
    std::vector<FF_FieldInfo> fields() const { return as_node().fields(); }
    std::vector<std::string_view> keys() const { return as_node().keys(); }
    
    MutableEntry operator[](FF_FieldKey key) const;
    MutableEntry operator[](size_t index) const;

    operator ResourceReference() const { return {m_offset, m_recovery}; }
};

// =====================================================================
// INLINE TEMPLATE IMPLEMENTATIONS
// =====================================================================
// 1. Implicit Conversion & Query Method Forwarding
inline MutableEntry::operator Reflective::Node() const { return as_node(); }
inline MutableEntry::operator ObjectHandle() const { return as_handle(); }
inline Node MutableEntry::as_node() const { return as_handle().as_node(); }

// 2. Query Methods (delegates to as_handle() for lazy materialization through Node)
inline bool MutableEntry::is_array() const { return as_handle().is_array(); }
inline bool MutableEntry::is_object() const { return as_handle().is_object(); }
inline bool MutableEntry::is_string() const { return as_handle().is_string(); }
inline bool MutableEntry::is_scalar() const { return as_handle().is_scalar(); }
inline size_t MutableEntry::size() const { return as_handle().size(); }
inline std::vector<FF_FieldInfo> MutableEntry::fields() const { return as_handle().fields(); }
inline std::vector<std::string_view> MutableEntry::keys() const { return as_handle().keys(); }

// 3. Field/Index Access
inline MutableEntry MutableEntry::operator[](FF_FieldKey key) const { return as_handle()[key]; }
inline MutableEntry MutableEntry::operator[](size_t index) const { return as_handle()[index]; }

template <typename T_Data>
requires (!std::is_arithmetic_v<T_Data>)
Offset MutableEntry::operator=(const T_Data& data) {
    // 1. Offload strict schema validation to the translation unit
    validate_assignment(TypeTraits<T_Data>::recovery);
    
    // 2. Thread-safe append of the child data
    Offset child_offset = m_builder->append(data);
    
    // 3. Thread-safe pointer patch on the parent V-Table using direct offsets
    m_builder->amend_pointer(m_parent_offset, m_vtable_offset, child_offset);
    
    return child_offset;
}

template <typename T>
requires std::is_arithmetic_v<T>
MutableEntry& MutableEntry::operator=(T val) {
    if (m_kind == FF_FIELD_CHOICE) {
        // Choice types always use the 10-byte polymorphic routine
        RECOVERY_TAG tag = FF_RECOVER_UNDEFINED;
        if constexpr (std::is_same_v<T, bool>) tag = RECOVER_FF_BOOL;
        else if constexpr (std::is_floating_point_v<T>) tag = RECOVER_FF_FLOAT64;
        else if constexpr (sizeof(T) == 4) tag = std::is_signed_v<T> ? RECOVER_FF_INT32 : RECOVER_FF_UINT32;
        else if constexpr (sizeof(T) == 8) tag = std::is_signed_v<T> ? RECOVER_FF_INT64 : RECOVER_FF_UINT64;
        
        m_builder->amend_variant(m_parent_offset, m_vtable_offset, static_cast<uint64_t>(val), tag);
    } else {
        // Fixed types use the size-aware scalar routine
        m_builder->amend_scalar(m_parent_offset, m_vtable_offset, val);
    }
    return *this;
}

inline MutableEntry& MutableEntry::operator=(const std::vector<Offset>& offsets) {
        if (m_parent_offset == FF_NULL_OFFSET)
            throw std::runtime_error("FastFHIR: Cannot assign to a NULL entry.");
            
        // 1. Thread-safe append of the array
        Offset child_offset = m_builder->append(offsets, m_recovery);
        
        // 2. Thread-safe pointer patch
        m_builder->amend_pointer(m_parent_offset, m_vtable_offset, child_offset);
        
        return *this;
    }

inline MutableEntry ObjectHandle::operator[](FF_FieldKey key) const {
    if (m_offset == FF_NULL_OFFSET)
        throw std::runtime_error("FastFHIR: Invalid ObjectHandle...");
    
    // Entry::kind already encodes whether this is an array; target_recovery holds the
    // clean element type directly — no ToArrayTag encoding needed or wanted here.
    RECOVERY_TAG target_tag = key.child_recovery;
    
    if (target_tag == FF_RECOVER_UNDEFINED && key.kind != FF_FIELD_UNKNOWN) {
        target_tag = Kind_to_Recovery(key.kind);
    }
    
    return MutableEntry(m_builder, m_offset, key.field_offset, target_tag, key.kind);
}

} // namespace Reflective

template <typename T_Data>
Reflective::ObjectHandle Builder::append_obj(const T_Data& data) {
    return Reflective::ObjectHandle(this, append(data), TypeTraits<T_Data>::recovery);
}

inline Reflective::ObjectHandle Builder::append_obj(const std::vector<Offset>& offsets, RECOVERY_TAG semantic_tag) {
    return Reflective::ObjectHandle(this, append(offsets, semantic_tag), semantic_tag);
}

inline Reflective::ObjectHandle Builder::root_handle() const {
    if (m_root_offset != FF_NULL_OFFSET && m_root_recovery != FF_RECOVER_UNDEFINED) {
        return Reflective::ObjectHandle(const_cast<Builder*>(this), m_root_offset, m_root_recovery);
    }

    throw std::runtime_error(
        "FastFHIR: Root is not set on this Builder. Calling application must set root explicitly "
        "before reading or finalizing the stream.");
}

template <typename T>
requires std::is_arithmetic_v<T>
void Builder::amend_scalar(Offset object_offset, size_t field_vtable_offset, T val) {
    if (!try_begin_mutation()) {
        throw std::runtime_error("FastFHIR: Builder is finalizing; amend is no longer allowed.");
    }

    struct MutationGuard {
        Builder* self;
        ~MutationGuard() { self->end_mutation(); }
    } guard{this};

    if (object_offset + field_vtable_offset + sizeof(T) > m_memory.capacity()) {
        throw std::runtime_error("FastFHIR: Scalar amendment out of bounds.");
    }

    BYTE* ptr = const_cast<BYTE*>(m_base) + object_offset + field_vtable_offset;

    if constexpr (sizeof(T) == 1) {
        STORE_U8(ptr, static_cast<uint8_t>(val));
    }
    else if constexpr (sizeof(T) == 4) {
        STORE_U32(ptr, static_cast<uint32_t>(val));
    }
    else if constexpr (sizeof(T) == 8) {
        if constexpr (std::is_floating_point_v<T>) {
            STORE_F64(ptr, static_cast<double>(val));
        } else {
            STORE_U64(ptr, static_cast<uint64_t>(val));
        }
    }
}
} // namespace FastFHIR
