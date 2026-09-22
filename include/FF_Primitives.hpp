/**
 * @file FF_Primitives.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @version 0.1
 * @brief The FastFHIR wire blocks: layouts, witnesses and accessors.
 * @license This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 *
 * THE WIRE BLOCKS, and the only sanctioned way to read them.
 *
 * The top of a three-layer split. Each layer can be read on its own, and each
 * includes the one below it, so including this header still gives you all
 * three -- every existing `#include "FF_Primitives.hpp"` keeps working:
 *
 *     FF_Constants.hpp   the vocabulary: scalars, sentinels, enums, the slot
 *                        contract (FF_FieldKind / ff_slot_width), the packed
 *                        date/time codec. Values there are PERMANENT.
 *     FF_Values.hpp      what a CONSUMER holds: Result, Optional<T>, DateTime,
 *                        UcumUnit, FF_UUID, FF_Id. Touches no byte of any
 *                        stream. Include THIS one if you are writing a caller.
 *     FF_Primitives.hpp  you are here. The blocks themselves.
 *
 * WHAT IS HERE, in order:
 *   DATA_BLOCK .................. the base every block starts with, and the
 *                                 self-offset witness that makes a block vouch
 *                                 for itself
 *   FF_HEADER ................... always at arena offset 0; the 54-byte layout
 *   FF_CHECKSUM ................. the fixed-size footer
 *   FF_ARRAY .................... entries inline, offsets only for FF_STRING
 *   FF_STRING ................... and everything sharing its layout
 *   FF_URL_DIRECTORY ............ the stream-level URL intern table
 *   FF_MODULE_REGISTRY .......... URL index -> .wasm blob
 *   FF_RESOURCE / FF_CODED_VALUE  the polymorphic carriers
 *   FF_GET_* / FF_BLOCK_* ....... THE NAMED ACCESSORS. Invariant 9: every wire
 *                                 read goes through one of these. Hand-written
 *                                 `LOAD_U16(base + off + VTABLE::FIELD)` is the
 *                                 antipattern they replaced, and it caused
 *                                 three documented defects
 *   ENCODE_FF_* / STORE_FF_* .... the lock-free emitter signatures
 *   ChoiceEntry ................. a decoded choice variant, for diagnostics
 *
 * Every block carries its own validation and a recovery tag, because a block
 * that does not vouch for itself is not a block.
 */

// MARK: - FastFHIR Core Primitives
#pragma once

// Each entry below is used by THIS file's own code. <bit>, <cstring>, <limits>,
// <optional>, <stdexcept>, <unordered_map> and <vector> were carried over from
// the monolith and are gone: nothing here names them. The two FastFHIR headers
// are the layers underneath (see the file comment), and FF_String.hpp arrives
// through FF_Values.hpp, which is the layer that actually uses it.
#include <charconv>      // std::to_chars, in ChoiceEntry::to_string
#include <cstddef>       // std::size_t
#include <cstdint>       // the fixed-width types every layout is spelled in
#include <memory>        // std::unique_ptr, for ChoiceEntry's ChoiceBlock
#include <ostream>       // operator<<(std::ostream&, const ChoiceEntry&)
#include <string>        // the emitter signatures
#include <string_view>   // the emitter signatures
#include <type_traits>   // the RecoveryTraits dispatch
#include <variant>       // ChoiceEntry's alternatives

#include "FF_Constants.hpp"   // the vocabulary: scalars, sentinels, slot kinds
#include "FF_Values.hpp"      // the consumer value types
#include "FF_Export.h"        // FF_EXPORT
#include "FF_Version.hpp"     // FHIR_VERSION_*

// #####################################################################
// ## GLOBAL SCOPE  --  the wire format
// ##
// ## Everything from here to the value types near the end describes BYTES:
// ## the slot contract, the block layouts, and the named accessors that are
// ## the only sanctioned way to read them (CLAUDE.md invariant 9). Values in
// ## this region are PERMANENT -- never renumbered, never reordered.
// #####################################################################

// =====================================================================
// BASE DATA BLOCK
// =====================================================================
struct FF_EXPORT DATA_BLOCK
{
    enum vtable_sizes
    {
        VALIDATION_S = TYPE_SIZE_UINT64,
        RECOVERY_S = TYPE_SIZE_UINT16,
    };
    enum vtable_offsets
    {
        VALIDATION = 0,
        RECOVERY = VALIDATION + VALIDATION_S,
        HEADER_SIZE = RECOVERY + RECOVERY_S,
    };

#ifdef __EMSCRIPTEN__
    Offset __remote = FF_NULL_OFFSET;
    void *__response = nullptr;
#endif

    Offset __offset = FF_NULL_OFFSET;
    Size __size = 0;
    uint32_t __version = 0;        // FHIR revision (for generated FHIR resource blocks)
    uint32_t __engine_version = 0; // FastFHIR engine version (for primitive blocks)

    explicit DATA_BLOCK() = default;
    // constexpr so fits() can be evaluated at compile time and so the free
    // FF_BLOCK_IN_BOUNDS can delegate here without giving up constant folding.
    explicit constexpr DATA_BLOCK(Offset off, Size total_size, uint32_t fhir_rev,
                                  uint32_t engine_ver = 0)
        : __offset(off), __size(total_size), __version(fhir_rev), __engine_version(engine_ver) {}

    /// Whether `width` bytes actually exist at this block's offset.
    ///
    /// The bounds test lives on the block because every block has the same
    /// question and the same two facts to answer it with. This is the Iris
    /// contract (BlockHeader::fits): a block that does not fit is not a block.
    [[nodiscard]] constexpr bool fits(Size width = HEADER_SIZE) const noexcept
    {
        return __offset != FF_NULL_OFFSET &&
               static_cast<std::size_t>(__offset) + static_cast<std::size_t>(width) <=
                   static_cast<std::size_t>(__size);
    }

    /// A block is truthy only if its HEADER IS ACTUALLY THERE.
    ///
    /// This used to be `__offset != FF_NULL_OFFSET` -- present-vs-absent, and
    /// nothing about whether the bytes exist. Iris carries the same comparison
    /// and describes that exact form as the one it retired: "checked only that
    /// the offset is in range: requiring the whole header to fit makes a
    /// truncated file fail at construction rather than mid-read."
    ///
    /// It matters because `if (block)` is the guard every caller already
    /// writes. Making it mean what it looks like it means turns thousands of
    /// existing call sites into bounds checks for free, instead of asking each
    /// one to remember a separate test it will eventually forget.
    ///
    /// A block whose `__size` is 0 is now falsy, which is the intended
    /// consequence: it says "no extent was supplied", and a reader that cannot
    /// say how big the buffer is has no business dereferencing into it.
    operator bool() const { return fits(); }

    FF_Result validate_offset(const BYTE *const __base, const char *type_name, RECOVERY_TAG recovery_tag) const noexcept;

    // The two fields every block carries are read with FF_GET_RECOVERY_TAG /
    // FF_GET_VALIDATION (declared below, after the block layouts). They are
    // free functions, not members, because the overwhelmingly common read is
    // "what is at THIS offset" with no block in hand -- a member would force
    // callers to conjure a DATA_BLOCK with a fabricated size and version just
    // to reach two bytes.

#ifdef __EMSCRIPTEN__
    void check_and_fetch_remote(const BYTE *const &__base);
#endif
};

// =====================================================================
// HEADER
// =====================================================================
// FF_HEADER is always written at arena offset 0. Layout as of the
// URGENT-TODO header redesign (54 bytes total):
//
//   Byte  0– 3 : MAGIC        — 4-byte format stamp (FF_MAGIC_BYTES)
//   Byte  4– 5 : RECOVERY     — recovery tag (RECOVER_FF_HEADER)
//   Byte  6– 7 : FHIR_REV     — FHIR schema revision (R4/R5)
//   Byte  8–15 : STREAM_SIZE  — total committed arena bytes
//   Byte 16–23 : ROOT_OFFSET  — arena offset of the root resource block
//   Byte 24–25 : ROOT_RECOVERY— recovery tag of the root resource type
//   Byte 26–33 : CHECKSUM_OFFSET — arena offset of FF_CHECKSUM block
//   Byte 34–41 : URL_DIR_OFFSET  — arena offset of FF_URL_DIRECTORY;
//                                   FF_NULL_OFFSET when no directory present.
//                                   Back-patched by FF_PredigestExtensionURLs
//                                   after writing the directory block.
//   Byte 42–49 : MODULE_REG_OFFSET — arena offset of FF_MODULE_REGISTRY;
//                                   FF_NULL_OFFSET until Phase 7 WASM work.
//   Byte 50–53 : VERSION      — encoded engine version + stream layout flag
//
struct FF_EXPORT FF_HEADER : DATA_BLOCK
{
    static constexpr char type[] = "FF_HEADER";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_HEADER;

    enum vtable_sizes
    {
        MAGIC_S = TYPE_SIZE_UINT32,             // 4
        RECOVERY_S = TYPE_SIZE_UINT16,          // 2
        FHIR_REV_S = TYPE_SIZE_UINT16,          // 2
        STREAM_SIZE_S = TYPE_SIZE_UINT64,       // 8
        ROOT_OFFSET_S = TYPE_SIZE_UINT64,       // 8
        ROOT_RECOVERY_S = TYPE_SIZE_UINT16,     // 2
        CHECKSUM_OFFSET_S = TYPE_SIZE_UINT64,   // 8
        URL_DIR_OFFSET_S = TYPE_SIZE_UINT64,    // 8
        MODULE_REG_OFFSET_S = TYPE_SIZE_UINT64, // 8
        VERSION_S = TYPE_SIZE_UINT32,           // 4
    };

    enum vtable_offsets
    {
        MAGIC = 0,                                             // 4 bytes (0-3)
        RECOVERY = MAGIC + MAGIC_S,                            // 2 bytes (4-5)
        FHIR_REV = RECOVERY + RECOVERY_S,                      // 2 bytes (6-7)
        STREAM_SIZE = FHIR_REV + FHIR_REV_S,                   // 8 bytes (8-15)  -> Hardware Aligned
        ROOT_OFFSET = STREAM_SIZE + STREAM_SIZE_S,             // 8 bytes (16-23) -> Hardware Aligned
        ROOT_RECOVERY = ROOT_OFFSET + ROOT_OFFSET_S,           // 2 bytes (24-25)
        CHECKSUM_OFFSET = ROOT_RECOVERY + ROOT_RECOVERY_S,     // 8 bytes (26-33)
        URL_DIR_OFFSET = CHECKSUM_OFFSET + CHECKSUM_OFFSET_S,  // 8 bytes (34-41)
        MODULE_REG_OFFSET = URL_DIR_OFFSET + URL_DIR_OFFSET_S, // 8 bytes (42-49)
        VERSION = MODULE_REG_OFFSET + MODULE_REG_OFFSET_S,     // 4 bytes (50-53)
        HEADER_SIZE = VERSION + VERSION_S                      // 54 bytes total
    };

    // Baseline header size for engine MAJOR 2026 (the first versioned engine).
    static constexpr Size HEADER_V2026_SIZE = HEADER_SIZE;
    // Returns the portion of this header that was valid at write time.  The
    // reader can use this to skip unknown trailing fields added by a newer engine
    // while still reaching the payload (root block, checksum, etc.).  All known
    // fields (MAGIC…VERSION) lie within HEADER_V2026_SIZE, so the bootstrapping
    // path always reads at least HEADER_SIZE bytes regardless.
    inline Size get_header_size() const noexcept
    {
        const uint16_t major = FF_ENGINE_MAJOR(__engine_version);
        if (major == 0 || major <= 2026)
            return HEADER_V2026_SIZE;
        return HEADER_SIZE;
    }

    explicit FF_HEADER(Size file_size) noexcept;

    FF_Result validate_full(const BYTE *const __base) const noexcept;
    uint32_t get_magic(const BYTE *const __base) const noexcept;
    uint32_t get_engine_version(const BYTE *const __base) const;
    FF_StreamCompaction get_stream_layout(const BYTE *const __base) const;
    uint16_t get_fhir_rev(const BYTE *const __base) const;
    FF_CHECKSUM get_checksum(const BYTE *const __base) const;
    Offset get_root(const BYTE *const __base) const;
    RECOVERY_TAG get_root_type(const BYTE *const __base) const;
    Offset get_url_dir_offset(const BYTE *const __base) const;
    Offset get_module_reg_offset(const BYTE *const __base) const;
};
void FF_EXPORT STORE_FF_HEADER(BYTE *const __base, uint16_t fhir_revision,
                               Size payload_size, Offset root_offset,
                               RECOVERY_TAG root_recovery, Offset checksum_offset,
                               Offset url_dir_offset = FF_NULL_OFFSET,
                               Offset module_reg_offset = FF_NULL_OFFSET,
                               FF_StreamCompaction stream_layout = FF_STREAM_COMPACTION_NONE);

// =====================================================================
// FIXED-SIZE CHECKSUM FOOTER
// =====================================================================
struct FF_EXPORT FF_CHECKSUM : DATA_BLOCK
{
    static constexpr char type[] = "FF_CHECKSUM";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_CHECKSUM;

    enum vtable_sizes
    {
        VALIDATION_S = TYPE_SIZE_UINT64, // 8
        RECOVERY_S = TYPE_SIZE_UINT16,   // 2
        ALGORITHM_S = TYPE_SIZE_UINT16,  // 2
        HASH_DATA_S = FF_MAX_HASH_BYTES, // 32
    };
    enum vtable_offsets
    {
        VALIDATION = 0,
        RECOVERY = VALIDATION + VALIDATION_S,  // 8
        ALGORITHM = RECOVERY + RECOVERY_S,     // 10
        HASH_DATA = ALGORITHM + ALGORITHM_S,   // 12
        HEADER_SIZE = HASH_DATA + HASH_DATA_S, // 44 bytes exactly
    };

    // Baseline header size for engine MAJOR 2026 (the first versioned engine).
    static constexpr Size HEADER_V2026_SIZE = HEADER_SIZE;
    inline Size get_header_size() const noexcept
    {
        const uint16_t major = FF_ENGINE_MAJOR(__engine_version);
        if (major == 0 || major <= 2026)
            return HEADER_V2026_SIZE;
        return HEADER_SIZE;
    }
    explicit FF_CHECKSUM(Offset off, Size size, uint32_t fhir_rev, uint32_t engine_ver = 0)
        : DATA_BLOCK(off, size, fhir_rev, engine_ver) {}

    FF_Result validate_full(const BYTE *const __base) const noexcept;
    FF_Checksum_Algorithm get_algorithm(const BYTE *const __base) const;
    std::string_view get_hash_view(const BYTE *const __base) const;
};

// Allocates the block, writes the metadata, and returns a pointer to the 32-byte hash buffer
FF_EXPORT BYTE *STORE_FF_CHECKSUM_METADATA(BYTE *const __base, Offset start_offset, FF_Checksum_Algorithm algo);

// =====================================================================
// ZERO-COPY ARRAY BLOCK
// =====================================================================
struct FF_EXPORT FF_ARRAY : DATA_BLOCK
{
    static constexpr char type[] = "FF_ARRAY";

    // Bitmasks for the packed 16-bit Kind & Step field at offset 10
    static constexpr uint16_t KIND_MASK = 0xC000; // Bits 15-14
    static constexpr uint16_t STEP_MASK = 0x3FFF; // Bits 13-0

    // High-bit flags identifying the physical layout of the elements
    enum EntryKind : uint16_t
    {
        SCALAR = 0x0000,      // 00... (e.g., bool, double, uint32)
        OFFSET = 0x4000,      // 01... (64-bit pointers to blocks)
        INLINE_BLOCK = 0x8000 // 10... (Contiguous structured blocks)
    };

    enum vtable_sizes
    {
        VALIDATION_S = TYPE_SIZE_UINT64,    // 8
        RECOVERY_S = TYPE_SIZE_UINT16,      // 2
        KIND_AND_STEP_S = TYPE_SIZE_UINT16, // 2 (Packed Kind & Step)
        ENTRY_COUNT_S = TYPE_SIZE_UINT32,   // 4
    };

    enum vtable_offsets
    {
        VALIDATION = 0,
        RECOVERY = VALIDATION + VALIDATION_S,          // 8
        KIND_AND_STEP = RECOVERY + RECOVERY_S,         // 10
        ENTRY_COUNT = KIND_AND_STEP + KIND_AND_STEP_S, // 12
        HEADER_SIZE = ENTRY_COUNT + ENTRY_COUNT_S,     // 16 bytes exactly
    };

    // Baseline header size for engine MAJOR 2026 (the first versioned engine).
    static constexpr Size HEADER_V2026_SIZE = HEADER_SIZE;
    inline Size get_header_size() const noexcept
    {
        const uint16_t major = FF_ENGINE_MAJOR(__engine_version);
        if (major == 0 || major <= 2026)
            return HEADER_V2026_SIZE;
        return HEADER_SIZE;
    }
    explicit FF_ARRAY(Offset off, Size size, uint32_t fhir_rev, uint32_t engine_ver = 0)
        : DATA_BLOCK(off, size, fhir_rev, engine_ver) {}

    FF_Result validate_full(const BYTE *const __base) const noexcept;
    uint16_t entry_step(const BYTE *const __base) const;
    EntryKind entry_kind(const BYTE *const __base) const;
    bool entries_are_pointers(const BYTE *const __base) const;
    uint32_t entry_count(const BYTE *const __base) const;
    const BYTE *entries(const BYTE *const __base) const;
};

void FF_EXPORT STORE_FF_ARRAY_HEADER(BYTE *const __base, Offset &write_head,
                                     FF_ARRAY::EntryKind kind,
                                     uint32_t entry_step, uint32_t entry_count,
                                     RECOVERY_TAG entry_recovery_tag);

// =====================================================================
// ZERO-COPY STRING BLOCK
// =====================================================================
struct FF_EXPORT FF_STRING : DATA_BLOCK
{
    static constexpr char type[] = "FF_STRING";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_STRING;
    enum vtable_sizes
    {
        VALIDATION_S = TYPE_SIZE_UINT64, // 8
        RECOVERY_S = TYPE_SIZE_UINT16,   // 2
        LENGTH_S = TYPE_SIZE_UINT32,     // 4
    };
    enum vtable_offsets
    {
        VALIDATION = 0,
        RECOVERY = VALIDATION + VALIDATION_S, // 8
        LENGTH = RECOVERY + RECOVERY_S,       // 10
        STRING_DATA = LENGTH + LENGTH_S,      // 14
        HEADER_SIZE = STRING_DATA,            // 14 bytes exactly
    };

    // Baseline header size for engine MAJOR 2026 (the first versioned engine).
    static constexpr Size HEADER_V2026_SIZE = HEADER_SIZE;
    inline Size get_header_size() const noexcept
    {
        const uint16_t major = FF_ENGINE_MAJOR(__engine_version);
        if (major == 0 || major <= 2026)
            return HEADER_V2026_SIZE;
        return HEADER_SIZE;
    }
    explicit FF_STRING(Offset off, Size size, uint32_t fhir_rev, uint32_t engine_ver = 0)
        : DATA_BLOCK(off, size, fhir_rev, engine_ver) {}

    FF_Result validate_full(const BYTE *const __base) const noexcept;

    // Length-prefixed payload byte count (the field the writer stamps at +10).
    // Same value as FF_GET_STRING_LENGTH(base, offset); this spelling is for
    // callers that already hold the block.
    inline uint32_t length(const BYTE *const __base) const noexcept;

    // Zero-Copy Mapped View
    std::string_view read_view(const BYTE *const __base) const;

    // Fallback std::string allocation for dictionary parsers
    std::string read(const BYTE *const __base) const;
};

// =====================================================================
// STREAM-LEVEL URL INTERN TABLE
// =====================================================================
// FF_URL_DIRECTORY deduplicates extension URLs across a stream using a
// chained-segment model: each entry stores only its leaf segment plus the
// index of its parent entry (or NO_PRIOR if it is a root segment).
//
// Binary layout:
//   HEADER (16): VALIDATION(8) | RECOVERY(2) | PAD(2) | ENTRY_COUNT(4)
//   ENTRY_TABLE:  ENTRY_COUNT × 16-byte URLEntry inline structs
//     URLEntry: PRIOR_IDX(4, FF_NULL_UINT32=root) | PAD(4) | SEG_OFFSET(8→FF_STRING)
//
// Example for "hl7.org/fhir/test" and "hl7.org/fhir/example":
//   Entry 0: prior=NONE, seg="hl7.org/fhir"
//   Entry 1: prior=0,    seg="test"   → full URL: "hl7.org/fhir/test"
//   Entry 2: prior=0,    seg="example"→ full URL: "hl7.org/fhir/example"
//
// get_url(idx): walks the prior chain, collects segments, joins with "/".
struct FF_EXPORT FF_URL_DIRECTORY : DATA_BLOCK
{
    static constexpr char type[] = "FF_URL_DIRECTORY";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_URL_DIRECTORY;
    static constexpr uint32_t NO_PRIOR = FF_NULL_UINT32;

    enum vtable_sizes
    {
        VALIDATION_S = TYPE_SIZE_UINT64,
        RECOVERY_S = TYPE_SIZE_UINT16,
        PAD_S = TYPE_SIZE_UINT16,
        ENTRY_COUNT_S = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets
    {
        VALIDATION = 0,
        RECOVERY = VALIDATION + VALIDATION_S,      // 8
        PAD = RECOVERY + RECOVERY_S,               // 10
        ENTRY_COUNT = PAD + PAD_S,                 // 12
        HEADER_SIZE = ENTRY_COUNT + ENTRY_COUNT_S, // 16
    };

    // Each URLEntry in the ENTRY_TABLE is 16 bytes:
    //   prior_idx (uint32_t, 4) | pad (uint32_t, 4) | seg_offset (Offset, 8)
    static constexpr Size URL_ENTRY_SIZE = 16;
    static constexpr Size URL_ENTRY_PRIOR_IDX = 0;  // byte offset of prior_idx within entry
    static constexpr Size URL_ENTRY_PAD = 4;        // byte offset of pad
    static constexpr Size URL_ENTRY_SEG_OFFSET = 8; // byte offset of seg_offset within entry

    // Baseline header size for engine MAJOR 2026 (the first versioned engine).
    static constexpr Size HEADER_V2026_SIZE = HEADER_SIZE;
    inline Size get_header_size() const noexcept
    {
        const uint16_t major = FF_ENGINE_MAJOR(__engine_version);
        if (major == 0 || major <= 2026)
            return HEADER_V2026_SIZE;
        return HEADER_SIZE;
    }
    explicit FF_URL_DIRECTORY(Offset off, Size size, uint32_t fhir_rev, uint32_t engine_ver = 0)
        : DATA_BLOCK(off, size, fhir_rev, engine_ver) {}

    uint32_t entry_count(const BYTE *base) const;
    uint32_t prior_idx(const BYTE *base, uint32_t entry_idx) const;
    Offset seg_offset(const BYTE *base, uint32_t entry_idx) const;
    std::string_view seg_string(const BYTE *base, uint32_t entry_idx) const;
    // Reconstructed full URL by walking the prior chain
    std::string get_url(const BYTE *base, uint32_t entry_idx) const;
};

// =====================================================================
// URL INTERNAL STATE
// =====================================================================
// Produced by FF_PredigestExtensionURLs(); consumed read-only by ingest
// workers.  FF_NULL_UINT32 as value means "filtered/known — skip block".
//
// =====================================================================
// EXT_REF (FF_EXTENSION discriminated-union routing word)
// =====================================================================
// 4-byte field stored at FF_EXTENSION::EXT_REF.  Bit 31 = routing flag;
// lower 31 bits = index payload.
//   MSB=0 → URL_IDX  → FF_URL_DIRECTORY
//   MSB=1 → MODULE_IDX → FF_MODULE_REGISTRY
constexpr uint32_t FF_EXT_REF_MSB = 0x80000000u;
constexpr uint32_t FF_EXT_REF_INDEX_MASK = 0x7FFFFFFFu;
constexpr uint32_t FF_EXT_REF_NULL = FF_NULL_UINT32;

inline bool ff_ext_ref_is_module(uint32_t r) noexcept { return r != FF_EXT_REF_NULL && (r & FF_EXT_REF_MSB) != 0; }
inline bool ff_ext_ref_is_url(uint32_t r) noexcept { return r != FF_EXT_REF_NULL && (r & FF_EXT_REF_MSB) == 0; }
inline uint32_t ff_ext_ref_index(uint32_t r) noexcept { return r & FF_EXT_REF_INDEX_MASK; }
inline uint32_t ff_make_module_ref(uint32_t i) noexcept { return (i & FF_EXT_REF_INDEX_MASK) | FF_EXT_REF_MSB; }
inline uint32_t ff_make_url_ref(uint32_t i) noexcept { return i & FF_EXT_REF_INDEX_MASK; }

// =====================================================================
// STREAM-LEVEL MODULE REGISTRY
// =====================================================================
// FF_MODULE_REGISTRY maps extension URL_IDX values to .wasm blob offsets
// in the arena so a parser can locate and load WASM codecs for all
// extension URLs present in a given stream.
//
// Binary layout:
//   HEADER (16): VALIDATION(8) | RECOVERY(2) | PAD(2) | ENTRY_COUNT(4)
//   ENTRY_TABLE:  ENTRY_COUNT × 88-byte RegistryEntry inline structs
//     RegistryEntry: URL_IDX(4) | KIND(2) | KIND_PAD(2) | WASM_BLOB_OFFSET(8) |
//                    WASM_BLOB_SIZE(4) | PAD2(4) | MODULE_HASH(32) | SCHEMA_HASH(32)
//
// KIND discriminates the codec path:
//   KIND=0 (DYNAMIC) — WASM codec; WASM_BLOB_OFFSET and WASM_BLOB_SIZE are valid.
//   KIND=1 (STATIC)  — Compiled C++ extension; WASM_BLOB_OFFSET is FF_NULL_OFFSET.
//   KIND≥2           — Reserved for future extension mechanisms.
//
// WASM_BLOB_OFFSET points to the raw .wasm bytes stored in the arena as
// a flat byte sequence (no FF_STRING header).  WASM_BLOB_SIZE is the byte
// count of that sequence.  Entries are sorted by URL_IDX for O(log n) lookup.
//
// MODULE_HASH is the SHA-256 digest of the raw .wasm binary bytes at the
// time of ingest.  It is the canonical version identity of the module:
// a content-addressed key that changes exactly when the binary changes.
//
// SCHEMA_HASH is the SHA-256 digest of the canonical descriptor blob (.ffd).
// It identifies the wire schema independently of the binary so a reader can
// verify schema compatibility without invoking WAMR.
//
// Backward compatibility: Streams written by engine < 2026 use 56-byte entries
// (no KIND / SCHEMA_HASH fields); get_entry_size() returns the appropriate size.

/// Discriminates the codec path stored in a FF_MODULE_REGISTRY entry.
enum FF_ModuleKind : uint16_t {
    FF_MODULE_KIND_DYNAMIC = 0, ///< WASM codec path; WASM_BLOB_OFFSET/SIZE are valid pointers.
    FF_MODULE_KIND_STATIC  = 1, ///< Compiled C++ extension; WASM_BLOB_OFFSET is FF_NULL_OFFSET.
};

struct FF_EXPORT FF_MODULE_REGISTRY : DATA_BLOCK
{
    static constexpr char type[] = "FF_MODULE_REGISTRY";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_MODULE_REGISTRY;

    enum vtable_sizes
    {
        VALIDATION_S = TYPE_SIZE_UINT64,
        RECOVERY_S = TYPE_SIZE_UINT16,
        PAD_S = TYPE_SIZE_UINT16,
        ENTRY_COUNT_S = TYPE_SIZE_UINT32,
    };
    enum vtable_offsets
    {
        VALIDATION = 0,
        RECOVERY = VALIDATION + VALIDATION_S,      // 8
        PAD = RECOVERY + RECOVERY_S,               // 10
        ENTRY_COUNT = PAD + PAD_S,                 // 12
        HEADER_SIZE = ENTRY_COUNT + ENTRY_COUNT_S, // 16
    };

    // Each RegistryEntry in the ENTRY_TABLE is 88 bytes:
    //   url_idx          (uint32_t,    4) | kind         (uint16_t,    2) |
    //   kind_pad         (uint16_t,    2) | wasm_blob_offset(Offset,   8) |
    //   wasm_blob_size   (uint32_t,    4) | pad2         (uint32_t,    4) |
    //   module_hash      (uint8_t[32])    | schema_hash  (uint8_t[32])
    // Legacy: engines < 2026 wrote 56-byte entries (no KIND/SCHEMA_HASH).
    static constexpr Size REG_ENTRY_SIZE_LEGACY = 56; // engines < 2026, no KIND/SCHEMA_HASH
    static constexpr Size REG_ENTRY_SIZE = 88;        // current layout
    static constexpr Size REG_ENTRY_URL_IDX = 0;
    static constexpr Size REG_ENTRY_KIND = 4;         // uint16_t; FF_ModuleKind
    static constexpr Size REG_ENTRY_KIND_PAD = 6;     // uint16_t padding
    static constexpr Size REG_ENTRY_WASM_BLOB_OFFSET = 8;
    static constexpr Size REG_ENTRY_WASM_BLOB_SIZE = 16;
    static constexpr Size REG_ENTRY_PAD2 = 20;
    static constexpr Size REG_ENTRY_MODULE_HASH = 24; // 32-byte SHA-256 of .wasm binary
    static constexpr Size REG_ENTRY_HASH_SIZE = 32;
    static constexpr Size REG_ENTRY_SCHEMA_HASH = 56; // 32-byte SHA-256 of .ffd descriptor

    /// Version-aware entry size.  Streams written by engines < 2026 (pre-KIND)
    /// use 56-byte entries; all current and future streams use 88-byte entries.
    inline Size get_entry_size() const noexcept {
        const uint16_t major = FF_ENGINE_MAJOR(__engine_version);
        return (major > 0 && major < 2026) ? REG_ENTRY_SIZE_LEGACY : REG_ENTRY_SIZE;
    }

    // Baseline header size for engine MAJOR 2026 (the first versioned engine).
    static constexpr Size HEADER_V2026_SIZE = HEADER_SIZE;
    inline Size get_header_size() const noexcept
    {
        const uint16_t major = FF_ENGINE_MAJOR(__engine_version);
        if (major == 0 || major <= 2026)
            return HEADER_V2026_SIZE;
        return HEADER_SIZE;
    }
    explicit FF_MODULE_REGISTRY(Offset off, Size size, uint32_t fhir_rev, uint32_t engine_ver = 0)
        : DATA_BLOCK(off, size, fhir_rev, engine_ver) {}

    uint32_t entry_count(const BYTE *base) const;
    uint32_t url_idx(const BYTE *base, uint32_t entry_idx) const;
    /// Returns the codec kind (DYNAMIC or STATIC) for this entry.
    FF_ModuleKind kind(const BYTE *base, uint32_t entry_idx) const;
    Offset wasm_blob_offset(const BYTE *base, uint32_t entry_idx) const;
    uint32_t wasm_blob_size(const BYTE *base, uint32_t entry_idx) const;
    /// Returns a view of the 32-byte SHA-256 content hash (MODULE_HASH) for this entry.
    std::string_view module_hash(const BYTE *base, uint32_t entry_idx) const;
    /// Returns a view of the 32-byte SHA-256 schema hash (SCHEMA_HASH) for this entry.
    /// Only valid for entries written with REG_ENTRY_SIZE=88 (engine >= 2026).
    std::string_view schema_hash(const BYTE *base, uint32_t entry_idx) const;

    /// Binary-search for entry with @p url_idx.  Returns FF_NULL_UINT32 if absent.
    uint32_t find_entry(const BYTE *base, uint32_t url_idx) const;
};

// =====================================================================
// GENERIC RESOURCE WRAPPER
// =====================================================================
// A passive coordinate for polymorphic resources (ie Bundle.Entry.Resource)
struct ResourceReference
{
    Offset offset = FF_NULL_OFFSET;
    RECOVERY_TAG recovery = FF_RECOVER_UNDEFINED;

    ResourceReference() = default;
    ResourceReference(Offset off, RECOVERY_TAG rec) : offset(off), recovery(rec) {}
};

// Slim staging structure for polymorphic FHIR choice [x] fields
// =====================================================================
// UNIFIED CODABLE CONCEPT FALLBACK BLOCK
// =====================================================================
// Variable-length block for ALL non-dictionary code values.  Bit 31 of the
// vtable slot (FF_CODED_VALUE_FLAG) signals this path; the remaining 31 bits
// are a signed relative offset to this block.
//
// Layout (no fixed padding — FF_Ops.hpp handles unaligned ARM access):
//   Offset  0– 7 : VALIDATION  (uint64_t) — standard DATA_BLOCK
//   Offset  8– 9 : RECOVERY    (uint16_t) — RECOVER_FF_CODED_VALUE
//   Offset 10    : SYSTEM      (uint8_t)  — FF_CodeableConceptSystem discriminator
//   Offset 11    : LENGTH      (uint8_t)  — payload byte count
//   Offset 12+   : PAYLOAD     (variable) — LENGTH bytes
//
// System-specific payload interpretation:
//   UNKNOWN (0x00): 2-byte URL index (uint16_t BE) + raw code string
//   UCUM    (0x01): raw ASCII UCUM expression
//   SNOMED  (0x02): 8-byte big-endian concept ID (uint64_t)
//   DICOM   (0x05): 4-byte big-endian tag (uint32_t)
struct FF_EXPORT FF_CODED_VALUE : DATA_BLOCK {
    static constexpr char type[] = "FF_CODED_VALUE";
    static constexpr enum RECOVERY_TAG recovery = RECOVER_FF_CODED_VALUE;

    enum vtable_sizes {
        VALIDATION_S = TYPE_SIZE_UINT64,   // 8
        RECOVERY_S   = TYPE_SIZE_UINT16,   // 2
        SYSTEM_S     = TYPE_SIZE_UINT8,    // 1
        LENGTH_S     = TYPE_SIZE_UINT8,    // 1
        HEADER_SIZE  = VALIDATION_S + RECOVERY_S + SYSTEM_S + LENGTH_S,  // 12
    };
    enum vtable_offsets {
        VALIDATION = 0,
        RECOVERY   = 8,
        SYSTEM     = 10,
        LENGTH     = 11,
        PAYLOAD    = 12,
    };

    explicit FF_CODED_VALUE(Offset off, Size total_size, uint32_t ver)
        : DATA_BLOCK(off, total_size, ver) {}

    FF_CodeableConceptSystem system(const BYTE* base) const noexcept {
        return static_cast<FF_CodeableConceptSystem>(base[__offset + SYSTEM]);
    }
    uint8_t length(const BYTE* base) const noexcept {
        return base[__offset + LENGTH];
    }
    const BYTE* payload(const BYTE* base) const noexcept {
        return base + __offset + PAYLOAD;
    }
};

// =====================================================================
// WIRE FIELD ACCESSORS
// =====================================================================
// The one way to read a block header field. These replace hand-written
// `LOAD_U16(base + off + DATA_BLOCK::RECOVERY)`: a caller names the field it
// wants and the block it wants it from, and never writes the offset
// arithmetic that the vtable constants exist to describe. That arithmetic is
// where the mistakes live, and consumers of the public API have no business
// doing it.
//
// They are FREE FUNCTIONS on purpose. Almost every read in the library is
// "what is at THIS offset" with no block object in hand, and a member forces
// the caller to conjure one -- DATA_BLOCK(off, 0, 0).recovery_tag(base) -- in
// which the size and version arguments are fabricated. Those two zeros are
// not harmless: FF_STRING::validate_full and FF_ARRAY::validate_full bound
// their checks against __size, so a block built to peek at two bytes is a
// block that silently passes its own validation.
//
// The bodies assemble bytes rather than calling LOAD_U16 / LOAD_U64 for two
// reasons. FF_Ops.hpp includes this header, so it cannot be included from it,
// and pushing these definitions into FF_Primitives.cpp to reach the macros
// would put an un-inlinable call on the hot read path (no LTO in this build).
// And the wire is strictly little-endian by definition -- FF_Ops.hpp says so
// at the top -- so the byte order below IS the format, not a portability
// hedge. Compilers fold each of these back into a single unaligned load.
// FF_CODED_VALUE::system() and FF_IsFieldEmpty() already read bytes this
// way; these follow them.

/// THE bounds test for a wire offset. Every block read goes through this one.
///
/// They are all blocks: a string, an array, a codeable concept and a resource
/// differ only in what follows the header, so "is there a block here" has
/// exactly one answer and belongs in exactly one function. Writing it per call
/// site is how a reader ends up with nine guarded hops and a tenth that
/// segfaults.
///
/// `stream_size` is the extent of the STREAM -- the bytes actually written and
/// present -- not the arena's capacity. Those differ by orders of magnitude:
/// Memory::create() reserves 4 GiB of sparse address space by default, of which
/// a 1 MB document occupies 1 MB. Bounding against the reservation permits a
/// read anywhere in 4 GiB of unmapped pages, which is not a bounds check, it is
/// the same segfault with extra arithmetic.
///
/// `width` is how many bytes the caller is about to touch: the header for a
/// hop, header + payload for a string.
///
/// No `off >= 0` test: Offset is uint64_t, so it cannot be negative and the
/// comparison is always true. A wrapped-around offset presents as an enormous
/// positive value and is caught by the upper bound, which is the only test that
/// was ever doing work.
inline constexpr bool FF_BLOCK_IN_BOUNDS(Offset off, Size stream_size,
                                         Size width = DATA_BLOCK::HEADER_SIZE) noexcept
{
    // Delegates to DATA_BLOCK::fits so there is one implementation, not two.
    // This spelling exists for the callers that hold a loose (offset, size)
    // pair and no block -- the parser's hops, which are deciding whether to
    // build one at all.
    return DATA_BLOCK(off, stream_size, 0).fits(width);
}

/// Semantic identity (RECOVERY_TAG) of the block at `block_offset`.
inline constexpr RECOVERY_TAG FF_GET_RECOVERY_TAG(const BYTE *base, Offset block_offset) noexcept
{
    const BYTE *const p = base + block_offset + DATA_BLOCK::RECOVERY;
    return static_cast<RECOVERY_TAG>(static_cast<uint16_t>(p[0]) |
                                     static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8));
}

/// Self-offset word of the block at `block_offset`. A block is well-formed
/// only when this equals `block_offset` -- that identity is what makes the
/// arena scannable, and it is the check the recovery scan is built on.
inline constexpr uint64_t FF_GET_VALIDATION(const BYTE *base, Offset block_offset) noexcept
{
    const BYTE *const p = base + block_offset + DATA_BLOCK::VALIDATION;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | static_cast<uint64_t>(p[i]);
    return v;
}

/// Whether a block SELF-VALIDATES: it fits, and its VALIDATION word holds
/// its own offset.
///
/// Every block is written with that self-offset witness, and both the explicit
/// validator and the recovery engine test it -- the NAVIGATION path never did.
/// FF_BLOCK_IN_BOUNDS asks only whether the bytes exist, so a corrupted slot
/// aiming at arbitrary in-range bytes was followed without question and
/// whatever those bytes claimed (tag, entry count, length) was believed.
///
/// Measured on a 1 MB Synthea bundle at 512 flipped bits: the recovery report
/// said ZERO invented references -- correctly, it refused those very targets --
/// while the exported document gained 20,057 fabricated leaf values the reader
/// had walked to anyway. The engine was right and nobody asked it. Checking the
/// witness costs one 8-byte load on the cache line the header read is about to
/// touch, and drops those 20,057 to 18.
[[nodiscard]] inline bool FF_BLOCK_SELF_VALIDATES(const BYTE* base, Offset off, Size stream_size,
                                                  Size width = DATA_BLOCK::HEADER_SIZE) noexcept
{
    return FF_BLOCK_IN_BOUNDS(off, stream_size, width) &&
           FF_GET_VALIDATION(base, off) == static_cast<uint64_t>(off);
}

/// Payload byte count of the FF_STRING (or string-layout block) at `string_offset`.
/// Valid for every tag FF_IsStringLayoutTag() accepts, opaque JSON included.
inline constexpr uint32_t FF_GET_STRING_LENGTH(const BYTE *base, Offset string_offset) noexcept
{
    const BYTE *const p = base + string_offset + FF_STRING::LENGTH;
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/// Zero-copy view of the payload of the string-layout block at `string_offset`.
inline std::string_view FF_GET_STRING_VIEW(const BYTE *base, Offset string_offset) noexcept
{
    return std::string_view(reinterpret_cast<const char *>(base + string_offset + FF_STRING::STRING_DATA),
                            FF_GET_STRING_LENGTH(base, string_offset));
}

/// Payload byte count of the FF_CODED_VALUE at `concept_offset` (1 byte, at +11).
inline constexpr uint8_t FF_GET_CONCEPT_LENGTH(const BYTE *base, Offset concept_offset) noexcept
{
    return base[concept_offset + FF_CODED_VALUE::LENGTH];
}

inline uint32_t FF_STRING::length(const BYTE *const __base) const noexcept
{
    return FF_GET_STRING_LENGTH(__base, __offset);
}

// ── CodeableConcept decode result ──────────────────────────────
struct FF_CodedValueResult {
    FF_CodeableConceptSystem system;   // discriminator byte
    uint64_t                 raw_code; // integer value (0 for string systems)
    std::string_view         label;    // human-readable string
};

// Decode a CodeableConcept block.  Returns structured result with
// system discriminator, raw integer (for fixed-width systems), and
// human-readable label.  Thread-local buffer for label string.
/// `stream_size` is the written extent, so this can refuse an offset outside it
/// -- the same route every other block read takes (FF_BLOCK_IN_BOUNDS).
/// It is a required parameter, not a defaulted one: a caller that cannot say
/// how big the buffer is has no business dereferencing into it, and a default
/// would just reintroduce the unchecked path this exists to remove.
FF_CodedValueResult FF_DECODE_CODED_VALUE(
    const BYTE* base, Offset offset, uint32_t version, Size stream_size);

// Write an unknown-system dynamic block (SYSTEM=0x00) with a 2-byte URL index
// followed by the raw code string.  Returns packed uint32_t with
// FF_CODED_VALUE_FLAG set.
uint32_t ENCODE_FF_CODED_VALUE_UNKNOWN(BYTE* __base, Offset block_offset,
                                          Offset& child_off,
                                          const std::string& code_str,
                                          uint16_t url_index,
                                          uint32_t version);

// Write a UCUM dynamic block (SYSTEM=0x01) with raw ASCII expression.
uint32_t ENCODE_FF_CODED_VALUE_UCUM(BYTE* __base, Offset block_offset,
                                       Offset& child_off,
                                       const std::string& ucum_expr,
                                       uint32_t version);

// Defined in generated FF_ChoiceBlock.hpp -- a variant over the datatypes this
// profile generates. Forward-declared here because this header is BELOW the
// generated code: ChoiceEntry holds it by unique_ptr, which needs only an
// incomplete type so long as the special members below stay out of line.
struct ChoiceBlock;

struct ChoiceEntry
{
    RECOVERY_TAG tag = FF_RECOVER_UNDEFINED;
    std::variant<
        std::monostate,
        bool,
        int32_t,
        uint32_t,
        int64_t,
        uint64_t,
        double,
        std::string_view>
        value;

    /// The DECODED value when `tag` names a block type.
    ///
    /// This used to be a raw arena OFFSET in the `uint64_t` arm above -- a
    /// structural address handed out through the value API. Nothing in the
    /// library ever read it back, so inside one arena it was invisible; but
    /// STORE wrote it out verbatim, so a POCO re-serialized into a DIFFERENT
    /// arena produced a slot naming an address that meant nothing there. The
    /// offset chain broke at write time, silently, and surfaced only in
    /// whatever later walked the whole graph.
    ///
    /// Offsets are structure. A caller gets values, by every access path.
    ///
    /// unique_ptr rather than an inline variant: std::variant is sized by its
    /// largest alternative, so inlining would take ChoiceEntry from 32 bytes to
    /// ~184 (AddressData alone is 176) and grow every struct holding one --
    /// and they nest, and live in vectors.
    std::unique_ptr<ChoiceBlock> block;

    /// A VALUE, copy included. It was move-only while the nested blocks were
    /// held by std::unique_ptr, which had no copy to give; with Optional they
    /// are values too, so a ChoiceEntry deep-copies its block like any other
    /// member. That is what lets a POCO be brace-initialized, since an
    /// initializer_list can only copy its elements.
    ///
    /// Out of line because the header only forward-declares ChoiceBlock; the
    /// generated TU defines these where it is complete.
    ChoiceEntry();
    ~ChoiceEntry();
    ChoiceEntry(const ChoiceEntry &);
    ChoiceEntry &operator=(const ChoiceEntry &);
    ChoiceEntry(ChoiceEntry &&) noexcept;
    ChoiceEntry &operator=(ChoiceEntry &&) noexcept;

    /// Assign a FHIR datatype straight into the slot: the variant tag comes
    /// from TypeTraits<T>, so the type is named once instead of three times.
    ///
    ///     observation.value = QuantityData{ .value = 94.0, .unit = "mg/dL" };
    ///
    /// replaces `tag = RECOVER_FF_QUANTITY;` + `block = FF_MakeChoiceBlock(tag);`
    /// + `block->value = std::move(quantity);`, where a mismatch between the
    /// first two spellings was a silent wrong-variant write.
    ///
    /// Constrained away from ChoiceEntry itself: for a non-const lvalue this
    /// template would otherwise be a better match than the copy assignment and
    /// quietly hijack `a = b`. Defined in the generated header, where
    /// ChoiceBlock is complete; a type that is not one of its alternatives is a
    /// compile error there, naming the type.
    template <typename T>
        requires(!std::is_same_v<std::decay_t<T>, ChoiceEntry>)
    ChoiceEntry &operator=(T &&value);

    bool is_empty() const { return tag == FF_RECOVER_UNDEFINED; }

    /// The text form of this choice's value -- for ANY tag and ANY arm:
    /// strings, code labels and fallback date/time text come back as-is;
    /// booleans, integers and decimals are formatted; a PACKED date/time is
    /// synthesized on demand. Empty for an absent entry or a block
    /// alternative (`.block` is a structured object with no scalar text).
    ///
    /// WHY on demand, not at decode: a packed date/time has no text on the
    /// wire -- the 8-byte slot is civil parts -- so its text must be
    /// SYNTHESIZED, and this struct deliberately owns no buffer. Formatting
    /// here, when a caller asks for text, is when the allocation belongs. The
    /// fallback form (legal text that could not pack) is preserved verbatim in
    /// the arena and arrives in `value` as a string_view; it is returned as-is
    /// without allocating. FF_FORMAT_DATETIME is the seam where future
    /// formatting options (zone, precision) would attach.
    std::string to_string() const;

    /// Stream output
    // NOT const-qualified: this is a friend FREE function, and only member
    // functions take a cv-qualifier. The qualifier made the whole header fail
    // to parse ("non-member function cannot have 'const' qualifier"), taking
    // every translation unit with it.
    friend std::ostream &operator<<(std::ostream &os, const ChoiceEntry &entry);
};

// =====================================================================
// LOCK-FREE EMITTER SIGNATURES
// =====================================================================
Size SIZE_FF_STRING(std::string_view str);
Size SIZE_FF_CODE(std::string_view code_str, uint32_t version);
// `tag` is the ONE thing that varies between the two string-layout blocks
// (FF_IsStringLayoutTag): RECOVER_FF_OPAQUE_JSON writes the identical header and
// payload but marks the bytes as already-serialized JSON. One emitter with an
// enum argument, not two near-identical emitters to drift apart.
// THE STORE RETURN CONTRACT -- the return TYPE is the whole signal.
//
//   Size   STORE_*  -> BYTES WRITTEN. 0 means "nothing was written" (a
//                     dictionary code and a packed date/time both need no child
//                     block), so it pairs with `child_off += ...` and with the
//                     SIZE/STORE contract check, which can only be expressed as
//                     a count: `written != claimed` is the guard that catches a
//                     store overrunning its claim.
//   Offset STORE_*  -> THE NEXT FREE OFFSET. A block writes its header at one
//                     address and its children at another, so "bytes written"
//                     is not even well defined for it; the cursor is. Pairs
//                     with `child_off = ...`.
//
// Both are needed and neither is arbitrary. What is NOT allowed is a function
// whose declared type disagrees with what it returns: STORE_FF_CODE and
// STORE_FF_DATETIME were declared Offset while returning a count, which made
// the type unreliable as a signal and the convention unlearnable. That
// mislabelling is what produced `child_off += <an absolute address>` in the
// generated choice store -- STORE consumed 13,917 bytes against a 4,847-byte
// claim, and only the contract check above caught it.
Size STORE_FF_STRING(BYTE *const __base, Offset start_offset, std::string_view str,
                     RECOVERY_TAG tag = RECOVER_FF_STRING);
Size STORE_FF_CODE(BYTE *const __base, Offset start_offset, std::string_view code_str, uint32_t version);
// Pack a code value into a 32-bit vtable slot.  Returns dictionary index
// (MSB=0) when the code is in the permanent dictionary, or a packed relative
// offset with FF_CODED_VALUE_FLAG set (MSB=1) when the code requires a
// dynamic fallback block.
uint32_t ENCODE_FF_CODE(BYTE *const __base, Offset block_offset, Offset &child_off,
                         const std::string &code_str, uint32_t version = FHIR_VERSION_R5,
                         FF_CodeableConceptSystem system = FF_CodeableConceptSystem::UNKNOWN);



// Defined out of class so FF_FORMAT_DATETIME (declared above) is visible; the
// body is inline so every TU sees the same definition.
//
// One function owns the text of EVERY alternative: string/code/fallback text
// is returned as-is, numeric arms are formatted, and a packed date/time -- the
// one alternative whose text does not exist on the wire -- is synthesized here
// because this struct owns no buffer. Date/time is dispatched on the TAG
// first: a packed date/time lives in the uint64_t arm and would otherwise
// format as a plain integer.
inline std::string ChoiceEntry::to_string() const
{
    // Date/time alternatives first: their text does not exist on the wire --
    // the slot holds civil parts (packed) or a relative offset to the original
    // text (fallback, materialized into the string_view arm at decode).
    if (FF_IsDateTimeTag(tag))
    {
        if (const auto* text = std::get_if<std::string_view>(&value)) return std::string(*text);
        if (const auto* raw = std::get_if<uint64_t>(&value))
        {
            // A FLAGGED value never reaches this arm: the decoder materializes
            // the fallback's text into the string_view arm, because a relative
            // offset cannot be resolved without the containing block's arena
            // base. What remains here is the packed civil value -- format it.
            if (FF_DATETIME_IS_FALLBACK(*raw)) return {};
            return FF_FORMAT_DATETIME(FF_UNPACK_DATETIME(*raw), tag);
        }
        return {};
    }

    if (const auto* text = std::get_if<std::string_view>(&value)) return std::string(*text);
    if (const auto* flag = std::get_if<bool>(&value)) return *flag ? "true" : "false";
    if (const auto* v = std::get_if<int32_t>(&value)) return std::to_string(*v);
    if (const auto* v = std::get_if<uint32_t>(&value)) return std::to_string(*v);
    if (const auto* v = std::get_if<int64_t>(&value)) return std::to_string(*v);
    if (const auto* v = std::get_if<uint64_t>(&value)) return std::to_string(*v);
    if (const auto* v = std::get_if<double>(&value))
    {
        // Shortest-round-trip -- the same answer print_decimal_json gives a
        // decimal choice variant. ostream's default six digits truncates
        // 42.142567166419695 to 42.1426.
        char buf[64];
        const auto res = std::to_chars(buf, buf + sizeof buf, *v);
        return res.ec == std::errc{} ? std::string(buf, res.ptr - buf) : std::string{};
    }
    // monostate, or a block alternative whose value lives in `.block` -- a
    // structured object has no scalar text form.
    return {};
}

// The stream twin of to_string(), defined beside it for the same reasons: one
// inline definition every TU sees, placed where FF_RecoveryName is declared.
// Value text goes through to_string(), so the two can never disagree about what
// text a value has. A nested block has no scalar text -- its TYPE is the tag's
// name (RECOVER_FF_QUANTITY and friends), and the decoded payload is a struct
// ChoiceBlock only forward-declares at this layer -- so the structure is marked
// instead: `RECOVER_FF_QUANTITY: <block>`.
inline std::ostream &operator<<(std::ostream &os, const ChoiceEntry &entry)
{
    // FF_RecoveryName is a DEBUG-ONLY dump helper: generated inside
    // `#ifndef NDEBUG` in FF_RecoveryTags.hpp, so it does not exist in a
    // release build. Calling it unconditionally compiled fine under the
    // default (debug) presets and broke EVERY -DNDEBUG build -- which is how
    // it reached the benchmark, whose Bazel arms are the only release
    // compilation in the workspace.
    //
    // The tag is still reported in release, as its numeric value: a diagnostic
    // that vanishes is worse than one that is terse.
#ifndef NDEBUG
    os << FF_RecoveryName(entry.tag) << ": ";
#else
    os << "tag:" << static_cast<int>(entry.tag) << ": ";
#endif
    if (entry.block) os << "<block>";
    else os << entry.to_string();
    return os;
}
