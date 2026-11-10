/**
 * @file FF_Parser.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * @version 0.1
 *
 * @brief FastFHIR read BODY — the internal tier behind `FastFHIR::Parser`.
 *
 * TIERS. FastFHIR's read surface is two types, deliberately:
 *
 *   - PUBLIC  — `FastFHIR::Parser` in "FF_Access.hpp" (pulled in by
 *               <FastFHIR.hpp>). A `std::shared_ptr<Parser_t>` handle; its `.`
 *               methods are the everyday read API. This is what a consumer
 *               includes and all most callers ever need.
 *   - BODY    — `FastFHIR::Parser_t` (here). The heap-owned reader that holds
 *               the arena and the parsed header fields, plus the deeper API:
 *               `validate_FFHR_stream_deep()`, `checksum()`, `url_directory()`,
 *               the module-registry accessors, and the raw members. Reach it
 *               with `parser->…` by including THIS header alongside FastFHIR.
 *
 * So the ordinary path is `#include <FastFHIR.hpp>`; the advanced one adds
 * `#include "FF_Parser.hpp"`. The split mirrors the write side (`Builder` /
 * `Builder_t`, completed by "FF_Builder.hpp") and the arena (`Memory` /
 * `Memory_t`).
 *
 * `Parser_t` derives from `std::enable_shared_from_this<Parser_t>`: it is only
 * ever owned through a `shared_ptr`, created by the `make_shared<Parser_t>`
 * calls in `Parser`'s constructors, so `shared_from_this()` is always valid
 * inside it (an internal helper that needs to keep the reader alive can take a
 * `Parser` from itself).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string_view>

#include "FF_Access.hpp"       // the Parser handle, Reflective fwd decls, Memory, value/type vocabulary
#include "FF_Dictionary.hpp"
#include "FF_Memory.hpp"
#include "FF_Primitives.hpp"
#include "FF_ResourceTypes.hpp"
#include "FF_Utilities.hpp"

namespace FastFHIR {

/**
 * @class Parser_t
 * @brief The read body behind @ref Parser. Internal tier.
 *
 * Not constructed directly by consumers: `Parser`'s constructors build one with
 * `make_shared<Parser_t>(...)` and hand back a handle. Reaching a member here
 * (via `parser->…`) is the advanced interface — it is why one includes this
 * header. The `.`-forwarding surface lives on @ref Parser.
 */
class Parser_t : public std::enable_shared_from_this<Parser_t>
{
    friend class Builder_t;
    friend class Parser;   // the handle's forwarders reach these members/methods

    // Non-const so the handle stays copy-assignable (FF_* out-parameter
    // pattern). The reader is never rebound after construction; the const-ness
    // was stylistic and blocked Parser& out params.
    Memory m_memory;
    const BYTE*     m_base = nullptr;
    Size            m_size = 0;
    uint32_t        m_version = 0;         // FHIR revision extracted from FF_HEADER::FHIR_REV
    uint32_t        m_engine_version = 0;  // engine version extracted from FF_HEADER::VERSION
    FF_StreamCompaction m_stream_layout = FF_STREAM_COMPACTION_NONE;
    const Reflective::ParserOps* m_ops = nullptr;
    Offset          m_root_offset = FF_NULL_OFFSET;
    RECOVERY_TAG    m_root_recovery = FF_RECOVER_UNDEFINED;
    Offset          m_url_dir_offset = FF_NULL_OFFSET;    // FF_NULL_OFFSET if absent
    Offset          m_module_reg_offset = FF_NULL_OFFSET; // FF_NULL_OFFSET if absent

public:
    /** @brief Null/invalid reader body. */
    Parser_t() = default;

    /**
    * @brief Bind to a raw in-memory byte buffer.
    * @throws std::runtime_error If the header is truncated or fails validation.
     */
    explicit Parser_t(const void* buffer, size_t size);

    /**
    * @brief Bind to an arena's memory mapping.
    * @throws std::runtime_error If the header is truncated or fails validation.
     */
    explicit Parser_t(const Memory& memory);

    /**
    * @brief Bind to a Memory::View, keeping its arena alive (no copy).
    *
    * A View already holds a strong reference to the arena it spans, so the
    * reader retains it: the bytes stay mapped for as long as the reader lives.
    * `Parser(view.data(), view.size())` names the same bytes but drops the
    * guarantee, because the raw-buffer constructor has nothing it can own.
    *
    * @throws std::runtime_error If the view is empty or the header fails.
    */
    explicit Parser_t(const Memory::View& view);

    /** @brief Total bytes of the parsed buffer. */
    Size size() const noexcept { return m_size; }

    /** @brief Total stream size in bytes. */
    Size size_bytes() const noexcept { return m_size; }

    /** @brief Check whether this reader references a parsed buffer. */
    explicit operator bool() const { return m_base != nullptr; }

    /** @brief The encoded FastFHIR/FHIR version from the file header. */
    uint32_t version() const;

    /** @brief Pointer to the underlying stream bytes. */
    const BYTE* data() const noexcept { return m_base; }

    /** @brief The stream layout mode detected from header metadata. */
    FF_StreamCompaction stream_layout() const noexcept { return m_stream_layout; }

    /** @brief True if the stream has a URL directory. */
    bool has_url_directory() const noexcept { return m_url_dir_offset != FF_NULL_OFFSET; }

    /** @brief True if the stream has a WASM module registry binding. */
    bool has_module_registry() const noexcept { return m_module_reg_offset != FF_NULL_OFFSET; }

    /** @brief Raw offset to the FF_MODULE_REGISTRY block (FF_NULL_OFFSET if none). */
    Offset module_registry_offset() const noexcept { return m_module_reg_offset; }

    /** @brief A zero-copy view of the stream-level FF_URL_DIRECTORY.
     *  @throws std::runtime_error if the stream has no URL directory. */
    FF_URL_DIRECTORY url_directory() const;

    /** @brief The root resource recovery/type tag from the header. */
    uint16_t root_type() const;

    /** @brief The root resource as a typed enum, without exposing recovery tags. */
    RESOURCETYPE root_resource_type() const noexcept { return resource_type_from_recovery(m_root_recovery); }

    /** @brief Typed root resource check. */
    template <RESOURCETYPE T>
    bool is_root() const noexcept { return m_root_recovery == ResourceTypeTraits<T>::recovery; }

    /** @brief The root resource node. */
    Reflective::Node root() const;

    /** @brief Stream the entire file back out as minified FHIR JSON. */
    void print_json(std::ostream& out) const;

    /**
     * @brief Walk the offset graph and verify it is structurally sound.
     *
     * **Call this on any stream you did not produce.** Construction validates
     * the header and the root offset only; every other offset is untrusted
     * until this returns success. O(blocks), explicit, never automatic.
     *
     * @return `FF_SUCCESS`, or a failure whose message names the offending
     *         offset and the field it was reached through.
     */
    FF_Result validate_FFHR_stream() const;

    /**
     * @brief `validate_FFHR_stream()` plus inspection of inline scalar slots.
     *
     * The structural pass deliberately skips scalar slots: they are data inside
     * a V-Table it has already bounds-checked, so they cannot aim the reader at
     * memory it does not own. This variant visits them too, at proportionally
     * higher cost, for callers who want the values inspected as well as the
     * shape.
     */
    FF_Result validate_FFHR_stream_deep() const;

    /**
    * @struct ChecksumValidation
    * @brief Checksum metadata extracted from the file.
     *
     * Checksum validation is performed outside the reader so the caller
     * controls how and when it runs, and so the reader is not coupled to any
     * particular checksum implementation.
     */
    struct ChecksumValidation {
        /// Pointer to first byte included in the checksum span.
        const void*           first_byte;
        /// Number of bytes to include starting at @ref first_byte.
        size_t                total_bytes;
        /// Hash/checksum algorithm declared by the file.
        FF_Checksum_Algorithm algorithm;
        /// Expected digest/hash value stored in the file footer.
        std::string_view      expected_checksum;
        /// Convenience validity check; true when an expected checksum is present.
        explicit operator bool() const { return !expected_checksum.empty(); }
    };

    /**
     * @brief Get checksum metadata for external checksum verification.
     * @return A populated @ref ChecksumValidation when present, otherwise an
     *         empty one (`first_byte == nullptr`, `total_bytes == 0`, empty text).
     */
    ChecksumValidation checksum() const;

private:
    /**
     * @brief The one initialization path every constructor converges on.
     *
     * Validates the header and extracts the parsed fields, so the pipeline
     * exists once rather than once per entry point.
     *
     * Preconditions: m_base / m_size / m_memory are set.
     * @throws std::runtime_error On a null base or an invalid header.
     */
    void _bind();
};

} // namespace FastFHIR
