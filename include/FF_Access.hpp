/**
 * @file FF_Access.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * @version 0.1
 * 
 * @brief FastFHIR Public API — the arena and stream access.
 *
 * The document-access types, both directions, in one header:
 *
 *     read                                write
 *     Parser                              Builder
 *     Reflective::Node                    Reflective::ObjectHandle
 *     Reflective::Entry                   Reflective::MutableEntry
 *
 * Read and write are deliberately the same shape, so a developer who can read a
 * field can write it, and vice versa:
 *
 *     parser.root()      -> Node          builder.append_object(x) -> ObjectHandle
 *     node[key]          -> Entry         handle[key]              -> MutableEntry
 *     entry.as<T>()      -> value         entry = value
 *     node[i]            -> Node          handle[i]                -> MutableEntry
 *
 * Prefer including <FastFHIR.hpp>, which pulls this in.
 */

#pragma once

#include <concepts>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <iosfwd>
#include <span>
#include <type_traits>

#include "FF_Dictionary.hpp"
#include "FF_Primitives.hpp"
#include "FF_TypeTraits.hpp"   // TypeTraits<T> and TypeDescriptor
#include "FF_ResourceTypes.hpp"
#include "FF_Utilities.hpp"


// Defined by the pybind11 layer, which MutableEntry friends so the binding can
// reach its coordinates without a public accessor.
struct PyMutableEntry;
namespace FastFHIR {
class Builder_t;
struct ArchiveContext;
template<typename T> struct TypeTraits;
class Memory_t;   // the arena body; defined in FF_Memory.hpp

/**
 * @class Memory
 * @brief The arena handle and its nested proxy types.
 *
 * Memory derives from std::shared_ptr<Memory_t>, so it IS the pointer it
 * stands for: `mem->size()`, `if (mem)`, copy it freely, pass it by value,
 * assign nullptr. The arena's nested types and factories live HERE, on the
 * handle, because this class is defined before Memory_t and a nested type
 * must be complete before the body can name it:
 *
 *     Memory::View        a lifetime-safe window over the committed bytes
 *     Memory::StreamHead  the exclusive RAII lock for raw socket ingestion
 *     Memory::create / createFromFile / openReadOnly
 *
 * The body (Memory_t, below) aliases them as view_t / stream_head_t for its
 * own signatures, so there is still exactly one of each.
 */
class Memory : public std::shared_ptr<Memory_t>
{
    using Base = std::shared_ptr<Memory_t>;
public:
    // The handle's nested types, declared before the forwarders below name
    // them. Both are defined further down this class.
    class View;
    class StreamHead;

    using Base::Base;              // every shared_ptr constructor
    using Base::reset;
    Memory() noexcept = default;
    Memory(std::nullptr_t) noexcept {}

    /// Adopt an existing arena. Used by the factories and by Memory::View.
    Memory(std::shared_ptr<Memory_t> arena) noexcept : Base(std::move(arena)) {}

    // --- Lifecycle (the factories live on the handle, because they return one) ---

    /**
     * @brief Factory allocation for the Virtual Memory Arena.
     * @param shm_name Optional. If empty, creates an anonymous RAM mapping. If provided, creates a cross-process Shared Memory (SHM) segment.
     * @param capacity Defaults to a 4GB sparse allocation.
     * @return Initialized memory handle.
     */
    static Memory create(size_t capacity = 4ULL * 1024 * 1024 * 1024, std::string shm_name = "");

    /**
     * @brief A WRITABLE file-backed arena: what a Builder_t appends into.
     *
     * An existing finalized stream is mounted for appending (the Builder_t
     * rewinds to the checksum block and keeps writing); a missing file is
     * created. An existing file that is NOT a finalized stream is refused
     * and left byte-identical -- it may be a damaged archive Recovery can
     * still repair, and nothing here discards a stream someone sealed.
     * Starting over is the caller's own std::filesystem::remove().
     *
     * @param filepath Path to the backing file on disk.
     * @param capacity Sparse reservation; defaults to 4GB.
     * @throws std::system_error if the file cannot be opened or mapped.
     * @throws std::runtime_error if the file holds something that is not a
     *         finalized stream; the file is left untouched.
     */
    static Memory createFromFile(const std::filesystem::path &filepath,
                                 size_t capacity = 4ULL * 1024 * 1024 * 1024);

    /**
     * @brief A READ-ONLY file-backed arena: what a Parser reads.
     *
     * Maps exactly the bytes on disk. The file is never created, grown,
     * truncated or written, so a mistyped path leaves nothing behind and a
     * damaged submission keeps the bytes Recovery needs. Every write
     * through the arena throws.
     *
     * @throws std::system_error if the file is missing or cannot be mapped.
     * @throws std::runtime_error if it is too short to hold a header.
     */
    static Memory openReadOnly(const std::filesystem::path &filepath);

    // --- Queries -----------------------------------------------------------
    // These are the arena's own queries, reached from the handle. The same
    // values are available through `mem->`.
    Size size() const;
    Size capacity() const;
    BYTE *base() const;
    View view();

    /// Reset the committed stream boundary. Use 0 before streaming raw
    /// bytes in, so the first byte lands at offset 0.
    void reset(Size committed_size);

    /**
     * @class Memory::View
     * @brief A lifetime-safe memory lens over the committed FastFHIR data arena.
     * @details Unlike a standard `std::string_view` which only holds raw pointers and
     * can easily dangle if the source memory is unmapped, `Memory::View` internally
     * holds a shared reference to the underlying VMA core. This guarantees that the
     * massive sparse mapping remains physically alive in RAM during asynchronous operations
     * (like non-blocking OS network egress or async database writes), strictly preventing
     * use-after-free errors when passing read-only views to downstream sinks.
     */
    class View
    {
    public:
        /**
         * @brief Implicit conversion to `std::string_view`.
         * @details Allows drop-in compatibility with POSIX sockets, cryptographic hashers,
         * and external APIs expecting standard contiguous string views. The returned view
         * spans strictly from the arena base to the currently committed write head.
         * @warning The resulting `std::string_view` drops the lifetime guarantee. Do not
         * outlive the parent `Memory::View` object.
         * @return A lightweight, non-owning view of the committed memory.
         */
        operator std::string_view() const noexcept;

        /**
         * @brief Retrieves a raw pointer to the start of the FastFHIR payload.
         * @return A read-only character pointer to the arena base.
         */
        const char *data() const noexcept;

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

        /** @brief Null view; required for FF_* out-parameter patterns. */
        View() = default;

    private:
        friend class Memory_t;
        // Parser takes the strong reference out of the view, so a parser
        // built from one keeps its arena alive. Friendship rather than a
        // public accessor, so Memory_t never surfaces publicly.
        friend class Parser;
        friend class Parser_t;   // the body binds to a View in its constructor
        // Holds the shared_ptr, not the handle, so View does not depend on
        // the handle's own definition; a Memory converts to it freely.
        std::shared_ptr<Memory_t> m_vma_ref = nullptr;
        explicit View(std::shared_ptr<Memory_t> vma_ref) : m_vma_ref(std::move(vma_ref)) {}
    };

};

/// Concept: true for any T that has a FastFHIR::TypeTraits<T> specialization with a read() method.
template <typename T>
concept HasTypeTraits = requires(const BYTE* base, Offset off, Size sz, uint32_t ver) {
    { FastFHIR::TypeTraits<T>::read(base, off, sz, ver) } -> std::convertible_to<T>;
};

namespace Reflective {
struct ParserOps;
class Node;
struct Entry;
}

/**
 * @class Parser
 * @brief Entry point for reading a FastFHIR byte stream.
 *
 * At creation time `Parser` validates **the header and the root offset only**:
 * magic bytes, FHIR revision, the stream-layout flag, the structural integrity
 * of the checksum block when one is present, and that `ROOT_OFFSET` is either
 * null or addressable within the buffer.
 *
 * It does **not** walk the offset graph, and it does **not** verify the
 * checksum digest — `checksum()` exposes the stored digest for a caller that
 * wants to check it. Every other offset in the stream is therefore still
 * untrusted after construction. Do not treat a successfully constructed
 * `Parser` as proof that the bytes are well-formed; on untrusted input, call
 * `validate_FFHR_stream()` before reading through it.
 */
class Parser_t;   ///< the read BODY — defined in "FF_Parser.hpp"

/**
 * @class Parser
 * @brief The public READ handle for a FastFHIR byte stream.
 *
 * Derives from `std::shared_ptr<Parser_t>`, so it IS the pointer it stands for:
 * `parser.root()`, `if (parser)`, copy it freely, pass it by value. The `.`
 * methods below are the API a consumer uses; the deeper body (`Parser_t`) is
 * reached with `->` when you include "FF_Parser.hpp". This mirrors `Builder`
 * (its body is `Builder_t`, and `FF_Builder.hpp` completes it) — one shape for
 * read and write.
 *
 * At creation time a `Parser` validates **the header and the root offset
 * only**: magic bytes, FHIR revision, the stream-layout flag, the structural
 * integrity of the checksum block when present, and that `ROOT_OFFSET` is
 * either null or addressable. It does **not** walk the offset graph. Every
 * other offset is untrusted until `validate_FFHR_stream()` returns success;
 * call it on any stream you did not produce.
 */
class Parser : public std::shared_ptr<Parser_t>
{
    using Base = std::shared_ptr<Parser_t>;
public:
    using Base::Base;      // every shared_ptr constructor
    using Base::reset;     // keep reset() beside the body's reset forwarders

    /** @brief Null handle. */
    Parser() noexcept = default;
    Parser(std::nullptr_t) noexcept {}
    /** @brief Adopt an existing reader. Used by the factories below. */
    Parser(std::shared_ptr<Parser_t> body) noexcept : Base(std::move(body)) {}

    // The three constructors are kept as thin wrappers over
    // `make_shared<Parser_t>(...)`, DEFINED in FF_Parser.cpp where Parser_t is
    // complete — so `Parser(buffer, size)`, `Parser(memory)` and
    // `Parser(view)` read exactly as they always have, while the body is
    // heap-owned and shared. A constructor whose body "validates the header"
    // still throws std::runtime_error on a bad stream, unchanged.
    explicit Parser(const void* buffer, size_t size);
    explicit Parser(const Memory& memory);
    explicit Parser(const Memory::View& view);

    /** @brief Total bytes of the parsed buffer. */
    Size size() const noexcept;
    /** @brief Total bytes of the parsed buffer. */
    Size size_bytes() const noexcept;

    /** @brief The encoded FHIR/FastFHIR version from the file header. */
    uint32_t version() const noexcept;

    /** @brief Pointer to the underlying stream bytes. */
    const BYTE* data() const noexcept;

    /** @brief The stream layout mode detected from header metadata. */
    FF_StreamCompaction stream_layout() const noexcept;

    /** @brief The raw RECOVERY_TAG of the root resource. */
    uint16_t root_type() const noexcept;

    /** @brief The root resource as a typed enum, without exposing recovery tags. */
    RESOURCETYPE root_resource_type() const noexcept;

    /** @brief Typed root resource check. */
    template <RESOURCETYPE T>
    bool is_root() const noexcept { return _root_recovery() == ResourceTypeTraits<T>::recovery; }

    /** @brief Get the root resource node. */
    Reflective::Node root() const;

    /** @brief Stream the entire FastFHIR file back out as minified FHIR JSON. */
    void print_json(std::ostream& out) const;

    /**
     * @brief Walk the offset graph and verify it is structurally sound.
     *
     * **Call this on any stream you did not produce.** Construction validates
     * the header and the root offset only; every other offset is untrusted
     * until this returns success. Deliberately explicit rather than automatic,
     * so opening a trusted stream stays O(1) — the cost is O(blocks).
     */
    FF_Result validate_FFHR_stream() const;

    // The DEEPER API lives on Parser_t (reached with `->` by including
    // "FF_Parser.hpp"): validate_FFHR_stream_deep(), checksum(), url_directory(),
    // the module-registry accessors, and every parsed field. This handle
    // carries only the everyday read surface.

private:
    /// The root's RECOVERY_TAG, for the is_root<T> template above (whose body
    /// cannot name the incomplete Parser_t). Defined in FF_Parser.cpp.
    uint16_t _root_recovery() const noexcept;
};

// =====================================================================
// VIEW LIVENESS — debug builds only
// =====================================================================
// `Node` and `Entry` are non-owning viewers (think std::string_view): each holds
// a raw pointer into an arena that its owner keeps mapped. A viewer that
// outlives its owner reads released memory.
//
// Making viewers own their arena is not the answer — a reference count on every
// field access measured ~10x single-threaded and ~218x under 8-way contention,
// and `Node` is on the hot read path. So the guarantee is a CHECK, not a
// permanent reference count.
//
// Each ARENA owns a liveness token; every viewer over it keeps a WEAK reference,
// and a read through a viewer whose arena is gone throws instead of faulting.
// Descendants inherit the token, so `parser.root()[key]` stays checked however
// deep you walk.
//
// The token belongs to the arena rather than to a `Parser_t`/`Builder_t` on
// purpose: the bytes go away when the last `Memory` reference goes away, not
// when some particular reader does. A temporary Parser over a live arena would
// otherwise mark every node it handed out as stale.
//
// **This is compiled out under `NDEBUG`.** A release build does not catch it,
// and the read is undefined behaviour exactly as it was before. That is the
// intended trade: the check costs a debug build one weak_ptr and one branch per
// read, and a release build nothing at all. To stay safe rather than lucky,
// keep the owning handle alive for as long as you keep a view — `Parser`,
// `Builder` and `Memory::View` are all `shared_ptr`-backed, so holding one is
// enough.
#ifndef NDEBUG
namespace Detail {
/// The sentinel a view weakly references to ask whether its owner still lives.
struct LivenessToken {};

/// A fresh token for a body to own.
inline std::shared_ptr<const LivenessToken> make_liveness_token()
{
    return std::make_shared<LivenessToken>();
}

/// The token given to a viewer with no owning arena — one built directly from
/// raw bytes. It never dies, so such a viewer is never reported stale: it has
/// nothing to be stale against.
inline std::shared_ptr<const LivenessToken> liveness_untracked()
{













    static const std::shared_ptr<const LivenessToken> token = make_liveness_token();
    return token;
}
} // namespace Detail

/// A view's weak handle on its owner's liveness token.
using LivenessRef = std::weak_ptr<const Detail::LivenessToken>;

/**
 * @brief Throw if @p ref names an owner that has already been destroyed.
 *
 * A view handed out with no owner carries the never-dying sentinel instead, so
 * this is silent for it — an unowned view cannot be stale.
 */
inline void require_alive(const LivenessRef &ref)
{
    if (ref.expired())
        throw std::runtime_error(
            "FastFHIR view read after its arena was released: this Node/Entry "
            "points into memory that has been unmapped. Keep an owning handle "
            "(Parser, Builder, or Memory::View) alive for as long as you keep "
            "the view. This check runs in debug builds only; a release build "
            "would have read released memory instead.");
}
#endif  // !NDEBUG

namespace Reflective {
// Vtable slot coordinate — represents one field slot within a parent block.
struct Entry {
    const BYTE*  base            = nullptr;
    Offset       parent_offset   = FF_NULL_OFFSET;
    uint32_t     vtable_offset   = 0;
    RECOVERY_TAG target_recovery = FF_RECOVER_UNDEFINED;
    FF_FieldKind kind            = FF_FIELD_UNKNOWN;
    Size         m_size          = 0;
    uint32_t     m_version       = 0;         // FHIR revision
    uint32_t     m_engine_version = 0;        // FastFHIR engine version
    const ParserOps* m_ops       = nullptr;

#ifndef NDEBUG
    /// Weak handle on the owning body's liveness token. Defaults to the
    /// never-dying sentinel, so a view built without an owner is never falsely
    /// reported stale. Absent entirely under NDEBUG — see VIEW LIVENESS.
    LivenessRef m_liveness = Detail::liveness_untracked();
    /// Throw if the backing body is gone; no-op in a release build.
    void _check_alive() const { require_alive(m_liveness); }
    /// Adopt the token of the view this one was derived from.
    void _attach(const LivenessRef &ref) noexcept { m_liveness = ref; }
#else
    void _check_alive() const noexcept {}
#endif

    Entry() = default;
    // 5-arg constructor — backward-compatible; size/version remain zero.
    Entry(const BYTE* b, Offset parent, uint32_t vtable, RECOVERY_TAG recovery, FF_FieldKind field_kind)
        : base(b), parent_offset(parent), vtable_offset(vtable), target_recovery(recovery), kind(field_kind) {}
    // 7-arg constructor — bakes in size/version for self-contained chaining.
    Entry(const BYTE* b, Offset parent, uint32_t vtable, RECOVERY_TAG recovery, FF_FieldKind field_kind,
            Size size, uint32_t version, const ParserOps* ops = nullptr, uint32_t engine_ver = 0)
        : base(b), parent_offset(parent), vtable_offset(vtable), target_recovery(recovery), kind(field_kind),
            m_size(size), m_version(version), m_engine_version(engine_ver), m_ops(ops) {}

    Offset absolute_offset() const {
        if (parent_offset == FF_NULL_OFFSET) return FF_NULL_OFFSET;
        return parent_offset + static_cast<Offset>(vtable_offset);
    }

    // Validity check — true when this vtable slot is non-null.
    explicit operator bool() const {
        return base != nullptr && parent_offset != FF_NULL_OFFSET;
    }

    // Read a fixed-width scalar value at this vtable slot.
    // Defined in FF_Parser.cpp, explicitly instantiated for the six FHIR scalar
    // wire types.
    template <typename T>
    requires std::is_arithmetic_v<T>
    T as_scalar(RECOVERY_TAG expected_tag) const;

    // Serialize an inline scalar (bool, int, uint, float, code) directly to JSON.
    void print_scalar_json(std::ostream& out, uint32_t version) const;

    /**
     * @brief The date/time this slot holds, in whichever arm it holds it.
     *
     * Available on Entry rather than Node because the fallback arm stores a
     * signed offset measured from the CONTAINING BLOCK, so resolving it takes
     * both the slot and its parent — and a Node carries only its own offset.
     *
     * Degrades rather than throws: a slot that is not a date/time, a choice
     * whose live variant is some other type, a null slot, or a fallback offset
     * that does not land on a block vouching for itself all come back as an
     * absent DateTime, which is falsy.
     */
    DateTime datetime_value() const;

    // Produce a Node over a DATA_BLOCK field (block, array, string, choice, resource).
    // 4-arg form used by generated code and Python bindings.
    Node as_node(Size size, uint32_t version, RECOVERY_TAG tag, FF_FieldKind field_kind,
                 const ParserOps* ops = nullptr) const;
    // 0-arg form — uses stored m_size/m_version; requires the 7-arg constructor.
    Node as_node() const;

    // ── Implicit conversions — bodies defined after Node ──────────────────────
    // String and code reads require a Node expansion (as_node() pointer-hop).
    operator std::string_view() const;
    // TypeTraits-backed struct reads (PatientData, BundleData, …).
    template <HasTypeTraits T>
    operator T() const;
    // Pure in-place scalar reads — no Node expansion needed.
    operator int32_t()  const { return as_scalar<int32_t> (RECOVER_FF_INT32);   }
    operator uint32_t() const { return as_scalar<uint32_t>(RECOVER_FF_UINT32);  }
    operator int64_t()  const { return as_scalar<int64_t> (RECOVER_FF_INT64);   }
    operator uint64_t() const { return as_scalar<uint64_t>(RECOVER_FF_UINT64);  }
    operator double()   const { return as_scalar<double>  (RECOVER_FF_FLOAT64); }
    // Explicit escape hatch — handles bool and auto/template contexts.
    // Body defined after Node.
    template <typename T>
    T as() const;

    // ── Chained traversal — bodies defined after Node ─────────────────────────
    Entry             operator[](FF_FieldKey key) const;
    Node              operator[](size_t index)    const;
    std::vector<Node> entries()                   const;
    bool   is_array()  const;
    bool   is_object() const;
    bool   is_string() const;
    bool   is_scalar() const;
    size_t size()      const;


    /**
     * @brief The concrete RECOVERY_TAG recorded BESIDE the offset in a 10-byte
     *        {offset, tag} tuple (FF_FIELD_RESOURCE / FF_FIELD_CHOICE).
     *
     * Reads the SLOT only — it does not follow the offset, so it answers
     * "what is this?" without touching the target block. `target_recovery`
     * cannot: for a resource slot it carries the static `child_recovery`
     * (RECOVER_FF_RESOURCE, the polymorphic base), and only a choice slot
     * resolves the runtime tag during lookup.
     *
     * The type is on the wire twice — here and in the target's header — which
     * is the redundancy the recovery adjudicator settles when they disagree.
     * A consumer needing certainty still calls as_node(); a consumer filtering
     * a scan by type does not, and should not pay a dereference per record for
     * a value already sitting in the slot it just read.
     *
     * @return the tag, or FF_RECOVER_UNDEFINED for slot kinds that are not
     *         tuples or when the slot is out of bounds.
     */
    RECOVERY_TAG concrete_recovery() const;
};
/**
 * @class Node
 * @brief Lightweight non-owning viewer over a FastFHIR value.
 * 
 * The Node class represents a node in the parsed FastFHIR data structure. It can represent
 * various types of data, including objects (blocks), arrays, strings, and scalar values.
 * 
 * Nodes are lightweight, non-owning viewers (think `std::string_view`) over the
 * parsed stream. The class provides methods for
 * accessing child nodes by key or index,
 * as well as methods for retrieving the node's value in the appropriate type (e.g., string, bool, uint32_t, double).
 *
 * **A Node does not own the bytes it views.** It stays valid only while the
 * arena behind it is mapped, which the `shared_ptr`-backed handles (`Parser`,
 * `Builder`, `Memory`, `Memory::View`) keep alive — so simply keeping one in
 * scope is enough. Reading through a Node whose arena is gone is a bug, and in
 * debug builds it throws with that message rather than reading released memory —
 * see VIEW LIVENESS above. The check is compiled out of release builds.
 */
class Node {
protected:
    friend struct ParserOps;
    friend struct Entry;   // copies m_liveness when it hands out a Node
    // Qualified: an unqualified name here would bind to Reflective::Parser_t,
    // which does not exist.
    friend class FastFHIR::Parser_t;   // installs the token on the root it returns
    friend class FastFHIR::Builder_t;  // installs the token on a node viewed mid-build
    friend struct FastFHIR::ArchiveContext;
    const BYTE* m_base = nullptr;
    Offset m_node_offset = FF_NULL_OFFSET;
    Size m_size = 0;
    uint32_t m_version = 0;         // FHIR revision
    uint32_t m_engine_version = 0;  // FastFHIR engine version
    RECOVERY_TAG m_recovery = FF_RECOVER_UNDEFINED;
    RECOVERY_TAG m_child_recovery = FF_RECOVER_UNDEFINED;
    FF_FieldKind m_kind = FF_FIELD_UNKNOWN;
    bool m_array_entries_are_offsets = false;
    const ParserOps* m_ops = nullptr;

#ifndef NDEBUG
    /// Weak handle on the owning body's liveness token. Defaults to the
    /// never-dying sentinel, so a view built without an owner is never falsely
    /// reported stale. Absent entirely under NDEBUG — see VIEW LIVENESS.
    LivenessRef m_liveness = Detail::liveness_untracked();
    /// Throw if the backing body is gone; no-op in a release build.
    void _check_alive() const { require_alive(m_liveness); }
    /// Adopt the token of the view this one was derived from.
    void _attach(const LivenessRef &ref) noexcept { m_liveness = ref; }
#else
    void _check_alive() const noexcept {}
#endif

    /// Offset of the block backing this node; `FF_NULL_OFFSET` when empty.
    /// Not for arithmetic — it is a coordinate within one parse, not a wire
    /// coordinate, and the offsets a stream stores are relative to other blocks.
    Offset offset() const { return m_node_offset; }
public:
    /** @brief Construct an empty/invalid node handle. */
    Node() = default;
    /**
    * @brief Construct a node backed by a structured (offset-based) value.
    */
    Node(const BYTE* base, Size size, uint32_t version, Offset offset,
         RECOVERY_TAG recovery, FF_FieldKind kind,
            RECOVERY_TAG child_recovery = FF_RECOVER_UNDEFINED, bool array_entries_are_offsets = false,
            const ParserOps* ops = nullptr, uint32_t engine_ver = 0);

    /** @brief Check whether this node contains a value */
    bool is_empty() const;

    /**
     * @brief Resolve a choice node to its concrete type based on the schema kind.
     *
     * @param base Pointer to the base of the FastFHIR data.
     * @param size Size of the FastFHIR data.
     * @param version Version of the FastFHIR data.
     * @param parent_offset Offset of the **containing block** — NOT the slot.
     *        Load-bearing: a flagged `RECOVER_FF_CODE` variant stores a signed
     *        offset relative to the block, and this is the only operand that
     *        can resolve it (a `Node` keeps no parent). Both call sites once
     *        passed `value_offset` here, which read one V-Table width past the
     *        real block and returned an empty code.
     * @param value_offset Offset of the value within the parent node.
     * @param schema_kind The schema kind to resolve the choice against.
     * @return Node representing the resolved choice. For a flagged code variant
     *         this already points at the `FF_CODED_VALUE` block.
     */
    static Node resolve_choice(const BYTE* base, Size size, uint32_t version, 
                       Offset parent_offset, Offset value_offset, FF_FieldKind schema_kind,
                       const ParserOps* ops = nullptr);

    /**
     * @brief Check whether this node references a valid underlying value.
     * @return `true` if this node is bound to valid data; otherwise `false`.
     */
    explicit operator bool() const;

    /** @brief Check whether this node is an array value. */
    bool is_array() const;
    /** @brief Check whether this node is an object/block value. */
    bool is_object() const;
    /** @brief Check whether this node is a string value. */
    bool is_string() const;
    /** @brief Check whether this node is an inline scalar value. */
    bool is_scalar() const;

    /**
     * @brief Get the node field kind.
     * @return Encoded FastFHIR field kind for this node.
     */
    FF_FieldKind kind() const;

    /**
     * @brief Get the node recovery/type tag.
     * @return Recovery/type tag associated with this node.
     */
    RECOVERY_TAG recovery() const;

    /**
     * @brief Get this node's concrete resource type as a typed enum.
     */
    RESOURCETYPE resource_type() const { return resource_type_from_recovery(m_recovery); }

    /**
     * @brief Typed check for concrete resource type.
     */
    template <RESOURCETYPE T>
    bool is() const { return m_recovery == ResourceTypeTraits<T>::recovery; }

    /**
     * @brief Zero-copy view of reflected field metadata for object nodes.
     * @return Span over the block's static field table; empty for non-object
     *         nodes. No allocation: the table is a static array in the
     *         generated block (see reflected_fields_view()).
     */
    std::span<const FF_FieldInfo> fields() const;

    /**
     * @brief Enumerate reflected key names for object nodes.
     * @return Key list for object nodes; empty for non-object nodes.
     */
    std::vector<std::string_view> keys() const;

    /**
     * @brief Get the number of direct children.
     * @return Entry count for arrays, key count for objects, otherwise `0`.
     */
    size_t size() const;

    /**
     * @brief Materialize all direct array entries.
     * @return Child nodes for arrays; empty for non-array nodes.
     */
    std::vector<Node> entries() const;

    /**
     * @brief Lookup a child field by precomputed key descriptor.
     * @return An Entry containing vtable slot coordinates; implicitly converts to scalars or structs.
     */
    Entry operator[](FF_FieldKey key) const;

    /**
     * @brief Lookup the child field described by one row of fields().
     * @return The same Entry operator[](FF_FieldKey) returns; falsy when the slot is absent.
     */
    Entry operator[](const FF_FieldInfo& field) const { return (*this)[FF_FieldKey(m_recovery, field)]; }

    /**
     * @brief Lookup an array element by index.
     * @return The resolved child Node at the given index.
     */
    Node operator[](size_t index) const;

    /**
     * @brief Materialize this block node into a typed C++ struct.
     * @tparam T A generated FastFHIR data struct with a TypeTraits specialization.
     */
    template <HasTypeTraits T>
    operator T() const { return as<T>(); }

    /**
     * @brief Explicit typed read — escape hatch for auto/template/bool contexts.
     * @tparam T Primitives, std::string_view, or a HasTypeTraits struct.
     * @throws std::runtime_error on recovery tag mismatch.
     */
    template <typename T>
    T as() const {
        _check_alive();
        if constexpr (std::is_same_v<T, std::string_view>) {
            if (m_recovery == RECOVER_FF_CODE) {
                uint32_t raw_code = code_word();
                if (raw_code == FF_CODE_NULL) return "";

                if (const char* label = FF_ResolveCode(raw_code, m_version)) {
                    return label;
                }

                if (raw_code & FF_CODED_VALUE_FLAG) {
                    // Unreachable: every producer of a code Node resolves the
                    // flagged case eagerly (ParserOps::code_node), because the
                    // offset is relative to the CONTAINING BLOCK and a Node no
                    // longer knows it. This used to resolve against
                    // m_node_offset -- the slot for a choice variant -- and so
                    // decoded a V-Table width away from the real block. Loud,
                    // not silent: a wrong address here reads plausible garbage.
                    throw std::runtime_error(
                        "FastFHIR: code Node carries an unresolved CodeableConcept "
                        "offset; the containing block is no longer known. Build it "
                        "through ParserOps::code_node.");
                }

                return "";
            }

            // A flagged code, already resolved to its block by code_node(). No
            // arithmetic left to do -- that is the entire point of resolving at
            // construction.
            if (m_recovery == RECOVER_FF_CODED_VALUE) {
                return FF_DECODE_CODED_VALUE(m_base, m_node_offset, m_version, m_size).label;
            }

            // Opaque JSON shares the FF_STRING layout exactly, so the DECODE is
            // identical; only the render sites care that the bytes are already
            // JSON. See FF_IsStringLayoutTag.
            if (!FF_IsStringLayoutTag(m_recovery)) {
                throw std::runtime_error("FastFHIR: Node is not a string or code.");
            }
            return FF_STRING(m_node_offset, m_size, m_version, m_engine_version).read_view(m_base);
        } else if constexpr (
            std::is_same_v<T, bool> ||
            std::is_same_v<T, int32_t> ||
            std::is_same_v<T, uint32_t> ||
            std::is_same_v<T, int64_t> ||
            std::is_same_v<T, uint64_t> ||
            std::is_same_v<T, double>
        ) {
            RECOVERY_TAG expected = FF_RECOVER_UNDEFINED;
            if constexpr (std::is_same_v<T, bool>) expected = RECOVER_FF_BOOL;
            else if constexpr (std::is_same_v<T, int32_t>) expected = RECOVER_FF_INT32;
            else if constexpr (std::is_same_v<T, uint32_t>) expected = RECOVER_FF_UINT32;
            else if constexpr (std::is_same_v<T, int64_t>) expected = RECOVER_FF_INT64;
            else if constexpr (std::is_same_v<T, uint64_t>) expected = RECOVER_FF_UINT64;
            else if constexpr (std::is_same_v<T, double>) expected = RECOVER_FF_FLOAT64;

            if (m_recovery != expected) {
                throw std::runtime_error("Node::as() recovery tag mismatch");
            }
            return read_scalar<T>(expected);
        } else {
            if (m_recovery != TypeTraits<T>::recovery) {
                throw std::runtime_error("Node::as() recovery tag mismatch");
            }
            return TypeTraits<T>::read(m_base, m_node_offset, m_size, m_version);
        }
    }

public:
    // Packed code word at this node's slot — out-of-line so the load stays in
    // FF_Parser.cpp (FF_Ops.hpp is an implementation detail there).
    uint32_t code_word() const;
    // Six-wire-scalar typed read at this node's slot — declared here, defined
    // in FF_Parser.cpp via Decode::scalar, explicitly instantiated.
    template <typename T>
    requires std::is_arithmetic_v<T>
    T read_scalar(RECOVERY_TAG expected_tag) const;

    /**
     * @brief Recursively print this node and all children as minified FHIR JSON.
     * @param out The output stream.
     */
    void print_json(std::ostream& out) const;

#ifndef NDEBUG
    /**
     * @brief `print_json` with the wire metadata each value was decoded from.
     *
     * Debug builds only. Every value becomes an object carrying what the reader
     * believed about it, so a wrong belief is visible instead of merely wrong:
     *
     * ```json
     * "effectiveDateTime": {"_off":40184,"_tag":"RECOVER_FF_STRING",
     *                       "_kind":"FF_FIELD_STRING","_v":"2019-04-01"}
     * ```
     *
     * That line is the motivating case: the value round-trips correctly and the
     * tag is still wrong, which no amount of JSON diffing can surface. Auditing
     * a corpus for it is a grep:
     *
     *     ff_roundtrip fixture.json --debug | grep -o '"[A-Za-z]*":{[^}]*RECOVER_FF_STRING' \
     *       | grep -i 'date\\|time\\|instant\\|period'
     *
     * Emitted per value: `_off` (absolute byte offset of the block or slot the
     * value was read from -- paste it straight into a hex dump), `_tag`
     * (RECOVERY_TAG spelling via FF_RecoveryName), `_kind` (the FF_FieldKind the
     * reader dispatched on), `_v` (the value print_json would have emitted).
     * Arrays add `_entry_kind` / `_stride` / `_count` / `_elem`; code slots add
     * `_code` and whether the CodeableConcept fallback flag is set; date/time
     * slots add whether the slot is packed or a fallback offset.
     *
     * NOT valid FHIR and never round-trip input -- it is a lens on the bytes.
     *
     * @param out    The output stream.
     * @param indent Spaces per level; 0 (default) minifies.
     */
    void to_debug_json(std::ostream& out, int indent = 0) const;
#endif // NDEBUG

};

// ── Entry methods requiring the complete Node definition ─────────────────────
inline Node Entry::as_node() const {
    _check_alive();
    Node out = as_node(m_size, m_version, target_recovery, kind, m_ops);
#ifndef NDEBUG
    out._attach(m_liveness);   // the child stays as checked as its parent
#endif
    return out;
}

// Entry::operator std::string_view() is defined in FF_Parser.cpp.

template <HasTypeTraits T>
inline Entry::operator T() const { return as_node().as<T>(); }

template <typename T>
inline T Entry::as() const {
    _check_alive();
    if constexpr (std::is_same_v<T, bool>)               return as_scalar<bool>(RECOVER_FF_BOOL);
    else if constexpr (std::is_same_v<T, DateTime>)      return datetime_value();
    else if constexpr (std::is_same_v<T, std::string_view>) return operator std::string_view();
    else if constexpr (std::is_same_v<T, int32_t>)       return operator int32_t();
    else if constexpr (std::is_same_v<T, uint32_t>)      return operator uint32_t();
    else if constexpr (std::is_same_v<T, int64_t>)       return operator int64_t();
    else if constexpr (std::is_same_v<T, uint64_t>)      return operator uint64_t();
    else if constexpr (std::is_same_v<T, double>)        return operator double();
    else                                                  return operator T();
}

inline Entry             Entry::operator[](FF_FieldKey key) const { return as_node()[key]; }
inline Node              Entry::operator[](size_t index)    const { return as_node()[index]; }
inline std::vector<Node> Entry::entries()                   const { return as_node().entries(); }
inline bool   Entry::is_array()  const { return as_node().is_array(); }
inline bool   Entry::is_object() const { return as_node().is_object(); }
inline bool   Entry::is_string() const { return as_node().is_string(); }
inline bool   Entry::is_scalar() const { return as_node().is_scalar(); }
inline size_t Entry::size()      const { return as_node().size(); }

} // namespace Reflective

// =====================================================================
// WRITE ACCESS — ObjectHandle, MutableEntry, Builder
// =====================================================================
// The write half of the pairs above. Same verbs as the read half:
// builder.append_object(x) -> ObjectHandle, and handle[key] -> MutableEntry.
namespace Reflective
{

// Named here because the erased entry points below take them by reference; both
// classes are defined later in this namespace, ObjectHandle first — MutableEntry's
// forwarders materialize through it, so it must be complete by then.
class MutableEntry;
class ObjectHandle;

// =====================================================================
// MUTABLE ENTRY — the write half of Entry
// =====================================================================
/**
 * @brief Ephemeral proxy returned by `ObjectHandle::operator[]`.
 * @details Assign either an existing Offset or new data: `handle[key] = value`.
 *
 * Assignment fills a slot that is currently UNSET. FastFHIR is append-only, so
 * assigning to a slot that already holds a value is refused at runtime rather
 * than overwriting it — see `assign_erased`.
 */
class MutableEntry
{
    Builder_t *m_builder = nullptr;
    const BYTE *m_base = nullptr;
    Offset m_parent_offset = FF_NULL_OFFSET;
    uint32_t m_vtable_offset = 0;
    RECOVERY_TAG m_recovery = FF_RECOVER_UNDEFINED;
    FF_FieldKind m_kind = FF_FIELD_UNKNOWN;

public:
    MutableEntry() = default;

    MutableEntry(Builder_t *builder, Offset parent_offset, uint32_t vtable_offset,
                 RECOVERY_TAG recovery, FF_FieldKind kind);

    // Materialize the READ half of this coordinate: the same field as a Node
    // (or the direct Entry form). The read API then applies.
    Entry as_entry() const;

    ObjectHandle as_handle() const;

    MutableEntry &operator=(const ObjectHandle &child);

    /**
     * @brief Append @p data into the stream and point this slot at it.
     *
     * The field must be unset. A `std::string_view` assigned to a date/time
     * field is encoded into the slot rather than stored as a child string.
     */
    template <typename T_Data>
        requires(!std::is_arithmetic_v<T_Data>)
    Offset operator=(const T_Data &data)
    {
        if constexpr (std::is_same_v<T_Data, std::string_view>)
        {
            if (m_kind == FF_FIELD_DATETIME)
            {
                assign_datetime_erased(*this, data);
                return offset();
            }
        }
        return assign_erased(*this, TypeTraits<T_Data>::descriptor(), &data);
    }

    /// Write a scalar in place. A `choice[x]` slot takes the packed variant form.
    template <typename T>
        requires std::is_arithmetic_v<T>
    MutableEntry &operator=(T val)
    {
        if (m_kind == FF_FIELD_CHOICE)
        {
            RECOVERY_TAG tag = FF_RECOVER_UNDEFINED;
            if constexpr (std::is_same_v<T, bool>)
                tag = RECOVER_FF_BOOL;
            else if constexpr (std::is_floating_point_v<T>)
                tag = RECOVER_FF_FLOAT64;
            else if constexpr (sizeof(T) == 4)
                tag = std::is_signed_v<T> ? RECOVER_FF_INT32 : RECOVER_FF_UINT32;
            else if constexpr (sizeof(T) == 8)
                tag = std::is_signed_v<T> ? RECOVER_FF_INT64 : RECOVER_FF_UINT64;

            amend_variant_erased(*this, static_cast<uint64_t>(val), tag);
        }
        else
        {
            amend_scalar_erased(*this, val);
        }
        return *this;
    }

    MutableEntry &operator=(const std::vector<Offset> &offsets);

    // Direct access for Entry-like queries
    explicit operator bool() const
    {
        return m_base != nullptr && m_parent_offset != FF_NULL_OFFSET;
    }

    // Builder and coordinate accessors
    Builder_t *get_builder() const { return m_builder; }
    Offset offset() const
    {
        return (m_parent_offset == FF_NULL_OFFSET) ? FF_NULL_OFFSET : (m_parent_offset + static_cast<Offset>(m_vtable_offset));
    }

    // Read this field through the read API. `as_node()` gives a bounds-checked
    // snapshot; the query and index methods below forward to it.
    operator Reflective::Node() const;
    operator ObjectHandle() const;
    Node as_node() const;

    bool is_array() const;
    bool is_object() const;
    bool is_string() const;
    bool is_scalar() const;
    size_t size() const;
    std::span<const FF_FieldInfo> fields() const;
    std::vector<std::string_view> keys() const;

    MutableEntry operator[](FF_FieldKey key) const;
    MutableEntry operator[](size_t index) const;

private:
    friend struct ::PyMutableEntry;

    // =================================================================
    // ERASED MUTATION CORES
    // =================================================================
    // Implementations of the assignment templates above. They are defined where
    // the stream writer is complete, so the templates can stay in this header.
    // Private: a caller writes `entry[key] = value` and never these.

    /**
     * @brief Append @p data and patch this slot to point at it.
     *
     * Validates the recovery tag, claims arena space, stores through the
     * descriptor's thunk, and back-patches the parent V-Table.
     *
     * @throws std::invalid_argument If the assigned type does not match the
     *         slot's own (or is not a resource where one is required).
     * @throws std::runtime_error If the slot already holds a value. FastFHIR is
     *         append-only; assigning over an existing value is refused rather
     *         than orphaning the bytes already written for it.
     */
    static Offset assign_erased(MutableEntry &entry, const TypeDescriptor &desc, const void *data);

    /// Encode date/time text into the slot. Not an append: the text is packed
    /// into 8 bytes, or flagged to a string block when it does not fit.
    static void assign_datetime_erased(MutableEntry &entry, std::string_view text);

    /// Write the packed form of a `choice[x]` value (a FHIR `[x]` field).
    static void amend_variant_erased(MutableEntry &entry, uint64_t raw_bits, RECOVERY_TAG tag);

    // One overload per FHIR scalar wire type. Overload resolution picks by the
    // argument's own type, so no value is widened in transit (a `double` would
    // not survive a round trip through uint64_t). Any other type is a compile
    // error here.
    static void amend_scalar_erased(MutableEntry &entry, bool val);
    static void amend_scalar_erased(MutableEntry &entry, uint8_t val);
    static void amend_scalar_erased(MutableEntry &entry, int32_t val);
    static void amend_scalar_erased(MutableEntry &entry, uint32_t val);
    static void amend_scalar_erased(MutableEntry &entry, int64_t val);
    static void amend_scalar_erased(MutableEntry &entry, uint64_t val);
    static void amend_scalar_erased(MutableEntry &entry, double val);

    void validate_assignment(RECOVERY_TAG child_tag) const;
};

// =====================================================================
// OBJECT HANDLE — the write half of Node
// =====================================================================
/**
 * @brief Thread-local handle representing a specific parent object being built.
 * @details The return of `Builder::append_object`. Field access is `handle[key]`,
 * which mirrors `node[key]` on the read side.
 */
class ObjectHandle
{
    Builder_t *m_builder = nullptr;
    Offset m_offset = FF_NULL_OFFSET;
    RECOVERY_TAG m_recovery = FF_RECOVER_UNDEFINED;

public:
    ObjectHandle() = default; // Default-constructible to null handle

    ObjectHandle(Builder_t *builder, Offset offset, RECOVERY_TAG recovery = FF_RECOVER_UNDEFINED)
        : m_builder(builder), m_offset(offset), m_recovery(recovery)
    {
        if (offset != FF_NULL_OFFSET && recovery == FF_RECOVER_UNDEFINED)
            throw std::invalid_argument("FastFHIR: Cannot instantiate an ObjectHandle with valid offset but UNDEFINED recovery tag.");
    }

    Builder_t *get_builder() const { return m_builder; }
    Offset offset() const { return m_offset; }
    RECOVERY_TAG recovery() const { return m_recovery; }
    explicit operator bool() const { return m_builder != nullptr && m_offset != FF_NULL_OFFSET; }

    /// Bounds-checked snapshot via the builder's arena. Out of line.
    Node as_node() const;

    bool is_array() const { return as_node().is_array(); }
    bool is_object() const { return as_node().is_object(); }
    bool is_string() const { return as_node().is_string(); }
    bool is_scalar() const { return as_node().is_scalar(); }
    size_t size() const { return as_node().size(); }
    std::span<const FF_FieldInfo> fields() const { return as_node().fields(); }
    std::vector<std::string_view> keys() const { return as_node().keys(); }

    MutableEntry operator[](FF_FieldKey key) const;
    MutableEntry operator[](size_t index) const;

    operator ResourceReference() const { return {m_offset, m_recovery}; }
};

// =====================================================================
// MUTABLE ENTRY — read-side forwards
// =====================================================================
// as_handle()/as_node() materialize the read half of this coordinate; the rest
// are one-line forwards through them.
inline MutableEntry::operator Reflective::Node() const { return as_node(); }
inline MutableEntry::operator ObjectHandle() const { return as_handle(); }
inline Node MutableEntry::as_node() const { return as_handle().as_node(); }

inline bool MutableEntry::is_array() const { return as_handle().is_array(); }
inline bool MutableEntry::is_object() const { return as_handle().is_object(); }
inline bool MutableEntry::is_string() const { return as_handle().is_string(); }
inline bool MutableEntry::is_scalar() const { return as_handle().is_scalar(); }
inline size_t MutableEntry::size() const { return as_handle().size(); }
inline std::span<const FF_FieldInfo> MutableEntry::fields() const { return as_handle().fields(); }
inline std::vector<std::string_view> MutableEntry::keys() const { return as_handle().keys(); }

inline MutableEntry MutableEntry::operator[](FF_FieldKey key) const { return as_handle()[key]; }
inline MutableEntry MutableEntry::operator[](size_t index) const { return as_handle()[index]; }

} // namespace Reflective

// =====================================================================
// BUILDER — the public write handle
// =====================================================================
/**
 * @brief The public write handle. Derives from `std::shared_ptr<Builder_t>`.
 * @details The `.` methods are the API. The handle keeps its arena alive, so
 * the stream stays writable for as long as the handle exists.
 *
 * Mirrors `Parser`: `builder.append_object(x)` returns a handle whose `[key]`
 * access matches `parser.root()[key]` on the read side.
 */
class Builder : public std::shared_ptr<Builder_t>
{
    using Base = std::shared_ptr<Builder_t>;
public:
    using Base::Base;
    using Base::reset;   // keeps both reset() forms; see FF_Memory.hpp

    /// Construct a null handle.
    Builder() noexcept = default;
    Builder(std::nullptr_t) noexcept {}

    /// Adopt an existing writer. Used by FF_CreateBuilder.
    Builder(std::shared_ptr<Builder_t> body) noexcept : Base(std::move(body)) {}

    /**
     * @brief Append a resource/backbone value and return a handle to fill it.
     *
     *     auto obs = builder.append_object(observation);
     *     obs[Fields::OBSERVATION::STATUS] = std::string_view("final");
     *
     * @return A valid ObjectHandle, or a null handle on failure. Never throws.
     */
    template <typename T_Data>
    Reflective::ObjectHandle append_object(const T_Data &data) const
    {
        return append_erased(*this, TypeTraits<T_Data>::descriptor(), &data);
    }

    /// The stream's current root handle, or a null handle if none is set yet.
    Reflective::ObjectHandle root() const;

    /// The arena this builder writes into.
    Memory memory() const;

    /// The FHIR revision this stream is being written for.
    FHIR_VERSION fhir_version() const;

private:
    // Implementation of append_object above. Returns a null handle on failure
    // rather than throwing, so no exception crosses this boundary.
    static Reflective::ObjectHandle append_erased(const Builder &builder,
                                                  const TypeDescriptor &desc,
                                                  const void *data);
};
} // namespace FastFHIR
