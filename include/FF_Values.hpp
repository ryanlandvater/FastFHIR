/**
 * @file FF_Values.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * @brief The types a CONSUMER holds. Nothing here touches a byte of any stream.
 *
 * The middle layer, and the one most callers actually want:
 *
 *     FF_Constants.hpp   <- the vocabulary these are phrased in.
 *     FF_Values.hpp      <- you are here.
 *     FF_Primitives.hpp  <- the wire BLOCKS. Include that one if you are
 *                           reading or writing bytes.
 *
 * NAMING, which this file is the clearest example of. A type reached through
 * `FastFHIR::` carries no prefix; `FF_` marks a name that lives at GLOBAL
 * scope. So `FastFHIR::DateTime` is the value type and `FF_DateTime` is its
 * global alias, while `FF_UUID` and `FF_Id` are global outright.
 *
 * WHAT IS HERE:
 *   FastFHIR::Result ............ every write-path call's verdict, with a
 *                                 severity that makes warnings success-like
 *   FastFHIR::Optional<T> ....... how a generated POCO spells FHIR's `0..1`
 *   FastFHIR::UcumUnit .......... a UCUM code, wrapped so it cannot be confused
 *                                 with the FHIR codes it is bit-identical to
 *   FastFHIR::DateTime .......... a date/time field holding EITHER civil parts
 *                                 OR the original text, so an unpackable value
 *                                 still round-trips byte-exactly
 *   FF_UUID / FF_IdSlot / FF_Id . the identity slot's value types (§17)
 */
#pragma once

// Used by this file's own code, and nothing more.
#include <cstddef>       // std::size_t, std::nullptr_t
#include <cstdint>       // the fixed-width types the value types carry
#include <memory>        // std::unique_ptr, Optional<T>'s storage
#include <string>        // Result::message, DateTime::to_string
#include <string_view>   // FF_UUID's hex constructor, DateTime's text arm

#include "FF_Constants.hpp"
#include "FF_Export.h"
#include "FF_String.hpp"

// #####################################################################
// ## namespace FastFHIR  --  RESULT AND OPTIONAL
// ##
// ## The two types a consumer meets before any wire structure: Result, which
// ## every write-path call returns, and Optional<T>, which is how a generated
// ## POCO spells FHIR's 0..1. Their FF_ aliases follow the closing brace.
// #####################################################################

namespace FastFHIR
{

// =====================================================================
// RESULT TYPE
// =====================================================================

/// Severity bucket of a Result. Warnings are success-like: `operator bool`
/// is true so callers can ignore them, but they are explicitly visible via
/// `is_warning()`, `code`, and `message`.
enum class Result_Severity : uint8_t
{
    SUCCESS = 0,
    WARNING = 1,
    FAILURE = 2,
};

enum Result_Code : uint32_t
{
    // ── Success ────────────────────────────────────────────────────────────
    FF_SUCCESS = 0,

    // ── Warnings — operator bool is TRUE; inspect code/message to see them ──
    FF_WARNING_PARTIAL_INGEST = 1,  // some resources discarded (out-of-profile)
    FF_WARNING_LOGGER_OVERFLOW = 2, // warning buffer truncated
    FF_WARNING_EMPTY_RESULT   = 3,  // succeeded but produced no output

    // ── Failures — operator bool is FALSE ──────────────────────────────────
    FF_FAILURE            = 0x100, // generic engine failure
    FF_VALIDATION_FAILURE = 0x101, // stream/offset/type validation failed
    FF_INVALID_ARGUMENT   = 0x102, // null handle, mutually exclusive fields
    FF_CAPACITY_EXCEEDED  = 0x103, // arena or buffer capacity exceeded
    FF_IO_FAILURE         = 0x104, // file/network I/O failure
    FF_NOT_IMPLEMENTED    = 0x105, // HL7 v2/v3 ingestion, etc.
    FF_EXTENSION_FAILURE  = 0x106, // WASM extension codec failure
};

struct Result
{
    Result_Code code;
    std::string message;
    Result(Result_Code c, std::string msg) : code(c), message(tag(c, std::move(msg))) {}
    Result(Result_Code c) : code(c), message("") {}

    /// Single source of truth for severity classification.
    /// Unknown codes classify as FAILURE so a typo'd or stale code fails safe.
    static constexpr Result_Severity severity_of(Result_Code c) noexcept
    {
        switch (c)
        {
        case FF_SUCCESS:
            return Result_Severity::SUCCESS;
        case FF_WARNING_PARTIAL_INGEST:
        case FF_WARNING_LOGGER_OVERFLOW:
        case FF_WARNING_EMPTY_RESULT:
            return Result_Severity::WARNING;
        default:
            return Result_Severity::FAILURE;
        }
    }

    /// Message tag for a code: "" / "[WARNING] " / "[FATAL] ".
    static const char* severity_tag(Result_Code c) noexcept
    {
        switch (severity_of(c))
        {
        case Result_Severity::SUCCESS: return "";
        case Result_Severity::WARNING: return "[WARNING] ";
        default:                          return "[FATAL] ";
        }
    }

    /// Appends one tagged entry; entries stack in append order, one per line.
    /// Appending a more severe code promotes `code` (failure > warning >
    /// success), so a warning is not lost to a later success, nor a failure to
    /// a later warning.
    Result& append(Result_Code c, std::string msg)
    {
        if (!message.empty()) message += '\n';
        message += severity_tag(c);
        message += std::move(msg);
        if (severity_of(c) > severity_of(code)) code = c;
        return *this;
    }

    /// Merges @p other: its (already tagged) entries stack after ours and the
    /// more severe code wins.
    Result& append(const Result& other)
    {
        if (!other.message.empty())
        {
            if (!message.empty()) message += '\n';
            message += other.message;
        }
        if (severity_of(other.code) > severity_of(code)) code = other.code;
        return *this;
    }

    /// True for success AND warnings — warnings are ignorable by design.
    inline bool succeeded() const noexcept { return severity_of(code) != Result_Severity::FAILURE; }
    /// True only for warnings; `message` carries the detail.
    inline bool is_warning() const noexcept { return severity_of(code) == Result_Severity::WARNING; }
    /// True only for failures — the go/no-go check for callers that must stop.
    inline bool failed() const noexcept { return severity_of(code) == Result_Severity::FAILURE; }

    inline operator bool() const { return succeeded(); }
    inline bool operator==(Result_Code c) const { return code == c; }
    inline bool operator!=(Result_Code c) const { return code != c; }

private:
    /// Tags the initial message entry with its severity tag; SUCCESS messages
    /// pass through untagged (a success should not carry a message).
    static std::string tag(Result_Code c, std::string msg)
    {
        if (msg.empty()) return msg;
        return std::string(severity_tag(c)) + std::move(msg);
    }
};

// =====================================================================
// OPTIONAL — a present-or-absent child block
// =====================================================================

/**
 * @brief A present-or-absent child block in a generated POCO: FHIR's `0..1`.
 *
 * What it replaces: the member used to be a bare `std::unique_ptr<T>`, so every
 * caller wrote `std::make_unique<CodeableConceptData>()` before it could say
 * anything about a field. That is an implementation detail of PRESENCE --
 * FHIR's optionality became C++ ownership -- and it showed up in every line of
 * consumer code. It is here so the natural form works:
 *
 *     observation.code = CodeableConceptData{ .coding = { ... } };
 *
 * Why the heap indirection stays: generated types refer to themselves
 * (Extension.extension), so an inline optional cannot be sized. This is
 * std::optional's shape with std::unique_ptr's storage.
 *
 * Value semantics, including COPY: a POCO is a value, and a brace list can only
 * copy its elements, so `.coding = { CodingData{...} }` needs copyable parts. A
 * copy costs what it copies; moves stay free.
 *
 * Not a wire type. This is the build-and-materialize side; no byte of any
 * stream changes because of it.
 */

template <typename T>
class Optional
{
public:
    using element_type = T;

    Optional() noexcept = default;
    ~Optional() = default;
    Optional(std::nullptr_t) noexcept {}

    /// By value, so both `= SomeData{...}` (moved) and `= existing` (copied) work.
    Optional(T value) : m_ptr(std::make_unique<T>(std::move(value))) {}
    /// The emitters build children with make_unique; that keeps working.
    Optional(std::unique_ptr<T> ptr) noexcept : m_ptr(std::move(ptr)) {}

    Optional(const Optional &other)
        : m_ptr(other.m_ptr ? std::make_unique<T>(*other.m_ptr) : nullptr) {}
    Optional(Optional &&) noexcept = default;

    Optional &operator=(const Optional &other)
    {
        m_ptr = other.m_ptr ? std::make_unique<T>(*other.m_ptr) : nullptr;
        return *this;
    }
    Optional &operator=(Optional &&) noexcept = default;
    Optional &operator=(T value)
    {
        m_ptr = std::make_unique<T>(std::move(value));
        return *this;
    }
    Optional &operator=(std::unique_ptr<T> ptr) noexcept
    {
        m_ptr = std::move(ptr);
        return *this;
    }
    Optional &operator=(std::nullptr_t) noexcept
    {
        m_ptr.reset();
        return *this;
    }

    T *operator->() noexcept { return m_ptr.get(); }
    const T *operator->() const noexcept { return m_ptr.get(); }
    T &operator*() noexcept { return *m_ptr; }
    const T &operator*() const noexcept { return *m_ptr; }
    T *get() noexcept { return m_ptr.get(); }
    const T *get() const noexcept { return m_ptr.get(); }

    explicit operator bool() const noexcept { return m_ptr != nullptr; }
    bool operator==(std::nullptr_t) const noexcept { return m_ptr == nullptr; }
    bool operator!=(std::nullptr_t) const noexcept { return m_ptr != nullptr; }

private:
    std::unique_ptr<T> m_ptr;
};

} // namespace FastFHIR



// The result types' global C-style aliases. Declared HERE rather than in
// FastFHIR.hpp's one alias block because this header is the lowest one every
// TU sees (the generated code, FF_Builder.hpp, FF_Parser.hpp all include it),
// and FF_Builder.hpp returns FF_Result without ever seeing FastFHIR.hpp.
using FF_Result          = FastFHIR::Result;
using FF_Result_Code     = FastFHIR::Result_Code;
using FF_Result_Severity = FastFHIR::Result_Severity;

/// The status-code enumerators (FF_SUCCESS, FF_FAILURE, ...) now live with
/// their enum, one scope in. This keeps them reachable unqualified, exactly as
/// they were when the enum was global (C++20 `using enum`).
using enum FastFHIR::Result_Code;



// #####################################################################
// ## namespace FastFHIR  --  THE CONSUMER VALUE TYPES
// ##
// ## Nothing below this line touches a byte of any stream. These are what a
// ## caller holds: UcumUnit, and DateTime with its civil/text arms. Each is
// ## aliased to an FF_ spelling at global scope after the closing brace.
// #####################################################################

// =====================================================================
// UCUM UNITS -- the one dictionary code a FHIR datatype fills three fields from
// =====================================================================
// A UCUM unit is a permanent dictionary code like any other (FF_CODE::UCUM::*),
// but it is the only kind a datatype spreads across THREE fields: Quantity's
// `code` is the UCUM expression, `unit` is the same expression in human form,
// and `system` is the CodeSystem URL. Writing those three by hand is three
// chances to disagree with each other, so `set_unit` takes this instead.
//
// Wrapping the constant in a type is the whole point: a UCUM unit is a
// dictionary code, and so is every FHIR code, so as bare uint32_t they are
// indistinguishable and `set_unit(some_fhir_code)` would compile. The
// constructor is explicit so a uint32_t does NOT become a unit on its own, and
// the conversion operator is implicit so a UCUM constant still works everywhere
// a uint32_t already does -- the version lookup tables alias these constants
// symbolically, and none of that should learn a new type.

namespace FastFHIR
{

struct UcumUnit
{
    /// The CodeSystem URI every UCUM unit shares. `unit` and `code` carry the
    /// UCUM expression itself ("mg/dL"); this is the `system` that names it.
    static constexpr std::string_view SYSTEM_URL = "http://unitsofmeasure.org";

    uint32_t id;

    constexpr explicit UcumUnit(uint32_t ucum_id) noexcept : id(ucum_id) {}

    constexpr operator uint32_t() const noexcept { return id; }
};

// =====================================================================
// DateTime -- one field type for both directions of a date/time slot
// =====================================================================

/**
 * A date/time field, holding EITHER the civil components or the original text.
 *
 * The 8-byte wire slot has two arms and the conversion between them only runs
 * one way. Civil components always render to text (FF_FORMAT_DATETIME builds
 * it). Text only packs into components when it fits, and the text arm exists
 * precisely for the values that do not: a fractional second of four or more
 * digits, a year outside 0001..9999, an offset beyond +/-14:00, or text that
 * is not valid FHIR grammar at all and is kept verbatim so the document still
 * round-trips. ENCODE_FF_DATETIME is where that choice is made.
 *
 * So a reader typed to return FF_DateTimeParts alone has nothing correct to
 * hand back for the text arm -- a zeroed struct reads as 0001-01-01 for a
 * value that is really 12345-06-07 -- and this type is what closes that hole.
 *
 * ONE TYPE SERVES BOTH DIRECTIONS, which is why the text arm owns rather than
 * borrows. A producer's string may die before the append:
 *
 *     { std::string drawn = lis_collection_time(); obs.effective = drawn; }
 *     builder->append_obj(obs);      // drawn is gone; the copy is not
 *
 * FastFHIR::String does that owning half, and keeps a literal or an explicit
 * view borrowed, so the read path still materializes arena text with no
 * allocation. The spelling of the right-hand side picks the arm, exactly as it
 * does for a plain string field.
 *
 *     field = "2024-01-15";              // literal  -> borrowed text
 *     field = computed_std_string;       // string   -> owned text
 *     field = FF_DateTimeParts{...};     // components, no text at all
 *     field = runtime_char_pointer;      // COMPILE ERROR -- say which you meant
 *
 * THE TAG IS NOT ALWAYS KNOWN AT ASSIGNMENT. Which of date / dateTime / time /
 * instant a slot holds is the field's schema, not the value's, so a producer
 * writing text leaves `tag()` at FF_RECOVER_UNDEFINED and the encoder supplies
 * it from the field. A reader always fills it in, because rendering needs it.
 */
class DateTime
{
public:
    /// Which arm is live. ABSENT is a real state: FF_DATETIME_NULL means the
    /// field was never set, which is different from an empty string.
    enum class Form : uint8_t { ABSENT = 0, CIVIL = 1, TEXT = 2 };

    DateTime() noexcept : m_civil(), m_form(Form::ABSENT) {}

    DateTime(const FF_DateTimeParts &parts, RECOVERY_TAG tag = FF_RECOVER_UNDEFINED) noexcept
        : m_civil(parts), m_form(Form::CIVIL), m_tag(tag) {}

    /// consteval, so ONLY a string literal binds here -- the same guard
    /// FastFHIR::String uses, and for the same reason. A pointer that is not a
    /// compile-time constant cannot reach it, which rejects `p = s.c_str()`
    /// and a local char buffer alike instead of storing a dangling view.
    consteval DateTime(const char *literal) : m_text(literal), m_form(Form::TEXT) {}

    /// The caller asserting these bytes outlive the append: the arena on the
    /// read path, the ingestor's JSON buffer on the write path.
    DateTime(std::string_view text) noexcept : m_text(text), m_form(Form::TEXT) {}

    /// A std::string may be gone before the append, so its text is kept.
    DateTime(std::string text) noexcept : m_text(std::move(text)), m_form(Form::TEXT) {}

    DateTime(String text, RECOVERY_TAG tag = FF_RECOVER_UNDEFINED) noexcept
        : m_text(std::move(text)), m_form(Form::TEXT), m_tag(tag) {}

    ~DateTime() { release(); }

    DateTime(const DateTime &other) : m_form(other.m_form), m_tag(other.m_tag)
    {
        if (m_form == Form::TEXT) std::construct_at(&m_text, other.m_text);
        else                      std::construct_at(&m_civil, other.m_civil);
    }

    DateTime(DateTime &&other) noexcept : m_form(other.m_form), m_tag(other.m_tag)
    {
        if (m_form == Form::TEXT) std::construct_at(&m_text, std::move(other.m_text));
        else                      std::construct_at(&m_civil, other.m_civil);
    }

    /// Same-arm assignment stays in the arm and never re-activates the union,
    /// so the common case is String's own operator=. Crossing arms builds the
    /// replacement BEFORE releasing, so a throwing copy leaves this object
    /// intact and self-assignment cannot free what it is about to read. Same
    /// shape as FastFHIR::String::operator=, deliberately.
    DateTime &operator=(const DateTime &other)
    {
        if (this == &other) return *this;
        if (m_form == Form::TEXT && other.m_form == Form::TEXT)
        {
            m_text = other.m_text;
            m_tag  = other.m_tag;
            return *this;
        }
        DateTime replacement(other);
        release();
        adopt(std::move(replacement));
        return *this;
    }

    DateTime &operator=(DateTime &&other) noexcept
    {
        if (this == &other) return *this;
        if (m_form == Form::TEXT && other.m_form == Form::TEXT)
        {
            m_text = std::move(other.m_text);
            m_tag  = other.m_tag;
            return *this;
        }
        release();
        adopt(std::move(other));
        return *this;
    }

    [[nodiscard]] Form form() const noexcept { return m_form; }
    [[nodiscard]] bool is_civil() const noexcept { return m_form == Form::CIVIL; }
    [[nodiscard]] bool is_text() const noexcept { return m_form == Form::TEXT; }
    [[nodiscard]] bool absent() const noexcept { return m_form == Form::ABSENT; }

    /// Present, in the sense the read path means it: a slot that was set.
    explicit operator bool() const noexcept { return m_form != Form::ABSENT; }

    /// Which FHIR type this is -- date, dateTime, time or instant. Rendering
    /// needs it, because one layout serves all four and the tag is the only
    /// thing that says a `date` must not grow a timezone.
    [[nodiscard]] RECOVERY_TAG tag() const noexcept { return m_tag; }
    void set_tag(RECOVERY_TAG tag) noexcept { m_tag = tag; }

    /// PRECONDITION: is_civil(). Checked, because reading the wrong arm of a
    /// union is not a wrong answer but undefined behaviour.
    [[nodiscard]] const FF_DateTimeParts &parts() const
    {
        if (m_form != Form::CIVIL)
            throw std::runtime_error("FastFHIR: DateTime::parts() on a value that holds "
                                     "text or nothing; test is_civil() first");
        return m_civil;
    }

    /// PRECONDITION: is_text().
    [[nodiscard]] const String &text() const
    {
        if (m_form != Form::TEXT)
            throw std::runtime_error("FastFHIR: DateTime::text() on a value that holds "
                                     "components or nothing; test is_text() first");
        return m_text;
    }

    /// The text of this value WHICHEVER arm it holds, which is the call a
    /// consumer that only wants to print it should make. Empty when absent.
    /// Civil components are rendered on demand, because their text does not
    /// exist on the wire. Defined out of class below, where
    /// FF_FORMAT_DATETIME is visible.
    [[nodiscard]] std::string to_string() const;

private:
    void adopt(DateTime &&other) noexcept
    {
        m_form = other.m_form;
        m_tag  = other.m_tag;
        if (m_form == Form::TEXT) std::construct_at(&m_text, std::move(other.m_text));
        else                      std::construct_at(&m_civil, other.m_civil);
    }

    /// The single point where the text arm is torn down.
    void release() noexcept
    {
        if (m_form == Form::TEXT) std::destroy_at(&m_text);
    }

    union {
        FF_DateTimeParts m_civil;
        String           m_text;
    };
    Form         m_form;
    RECOVERY_TAG m_tag = FF_RECOVER_UNDEFINED;
};

// DateTime::to_string is out of class for the same reason as
// ChoiceEntry::to_string below: FF_FORMAT_DATETIME is only declared further up
// this header, and the body is inline so every TU sees one definition.
inline std::string DateTime::to_string() const
{
    switch (m_form)
    {
    case Form::TEXT:
        // Already text, on the wire or supplied by the producer. Returned
        // verbatim, which is what keeps an unpackable value byte-exact.
        return std::string(m_text);
    case Form::CIVIL:
        // Synthesized: a packed value has no text on the wire, so this is
        // where the allocation belongs rather than at decode.
        return FF_FORMAT_DATETIME(m_civil, m_tag);
    case Form::ABSENT:
    default:
        return {};
    }
}
}  // namespace FastFHIR

// #####################################################################
// ## GLOBAL SCOPE  --  the identity slot (§17)
// ##
// ## FF_UUID, FF_IdSlot and FF_Id are wire carriers, so they are global and
// ## keep the FF_ prefix, beside FF_DateTimeParts and FF_HEADER. The value
// ## types they are read into live in the namespace above.
// #####################################################################

// =====================================================================
// FF_UUID — sixteen raw identity bytes (§17)
// =====================================================================
// These live at global scope, which is what the FF_ prefix means throughout
// this tree. The wire-level carriers in this header — FF_DateTimeParts,
// FF_HEADER, FF_URL_DIRECTORY — are all spelled that way and all sit outside
// the namespace. The unprefixed value types (DateTime, UcumUnit) live inside it
// and are reached through an FF_ alias. FF_UUID is a carrier of the first kind,
// and §17.10 defines it as `struct FF_UUID`, so it belongs here.

/// BYTE, never char: char's signedness is implementation-defined, and BYTE is
/// the codebase's uint8_t everywhere else. The default value is all-ones, the
/// absence convention shared with every other sentinel (FF_NULL_*); a UUID is
/// never itself absent, but an FF_Id slot that was never set is.
///
/// The hex constructor accepts the canonical 8-4-4-4-12 spelling, with or
/// without a `urn:uuid:` prefix, and throws on anything else. That strictness
/// is §17.4's governing rule made structural: only text that renders back
/// byte-identically may be packed into the inline arm, so a spelling this type
/// cannot reproduce (an uppercase hex digit, a missing hyphen) is rejected
/// here rather than silently normalised -- it routes to INTERNED or RAW_STRING
/// upstream, where it is held verbatim.
struct FF_EXPORT FF_UUID {
    BYTE bytes[16];

    FF_UUID() noexcept;                        // all-ones: the absent sentinel
    explicit FF_UUID(std::string_view hex);    // "59bf0ef4-e89c-4628-9b51-12ae3fdbe22b"

    bool operator==(const FF_UUID& other) const noexcept;
    bool operator!=(const FF_UUID& other) const noexcept { return !(*this == other); }
};

// =====================================================================
// FF_Id — the value an identity slot holds (§17.10)
// =====================================================================
// A form tag over the four wire arms (§17.2), plus two build-time-only forms
// that can never reach the wire. Follows FastFHIR::DateTime (a form tag over a
// union) and FastFHIR::String: the tag is almost the whole behaviour, because
// three of the four wire arms carry a different payload and resolve to text a
// different way.
//
// WHY A TAGGED TYPE AND NOT A BARE FF_UUID: all three fields that will carry
// an id (§17.10) legitimately hold text that is NOT a UUID -- contained-
// resource ids like `referral`, absolute `fullUrl`s in a searchset bundle, and
// `Patient/123` or `#fragment` references -- so the value must hold text as
// well as a UUID.
//
// THE SLOT LAYOUT the codec reads and writes is §17.2's, repeated here because
// it is the one place the discrimination is defined:
//   byte 0-3   idx      uint32, little-endian   (INTERNED arm only)
//   byte 4-7   kind     uint32, little-endian, small
//   byte 8     check    0x01
//   byte 9-15  offset   7 bytes, block or FF_STRING offset
// and an inline UUID (the third ordered test) is simply the sixteen bytes.
namespace FF_IdSlot {
constexpr Size WIDTH        = 16;
constexpr Size KIND_WORD    = 4;    // bytes 4..7 hold the kind word
constexpr Size CHECK_BYTE   = 8;    // byte 8 holds the check byte
constexpr Size OFFSET_BYTES = 9;    // POSITION: the offset starts at byte 9
constexpr Size OFFSET_WIDTH = 7;    // EXTENT:   and runs to byte 15
constexpr BYTE CHECK_VALUE  = 0x01;

/// The widest offset the slot can carry. Seven bytes is a deliberate part of
/// the layout (the four bytes before it name the arm), so an offset above this
/// is not representable and write_slot REFUSES it rather than truncating --
/// FF_NULL_OFFSET would otherwise land as 0x00FFFFFFFFFFFFFF and a wider one
/// would alias a real, in-bounds, self-validating block.
constexpr Offset MAX_OFFSET = (Offset{1} << (8 * OFFSET_WIDTH)) - 1;

constexpr uint32_t KIND_PENDING    = 0;  // a deferred slot; must never reach the wire
constexpr uint32_t KIND_GENERATED  = 1;  // the block offset we minted
constexpr uint32_t KIND_INTERNED   = 2;  // a URL-directory (trie) index
constexpr uint32_t KIND_RAW_STRING = 3;  // an FF_STRING offset
}  // namespace FF_IdSlot

class FF_EXPORT FF_Id {
public:
    enum class Form : uint8_t {
        ABSENT = 0,      // never set; the all-ones slot
        UUID = 1,        // an FF_UUID, inline in the slot
        GENERATED = 2,   // the block offset we minted for this stream
        INTERNED = 3,    // a URL-directory (trie) index
        RAW_STRING = 4,  // an FF_STRING offset holding the text verbatim
        POINTER = 5,     // build-time only: a POCO pointer awaiting append (§17.11)
        PENDING = 6,     // build-time only: a deferred slot; a seal with one left is a throw
    };

    FF_Id() noexcept = default;                        // ABSENT (all-ones slot)
    FF_Id(const FF_UUID& uuid) noexcept : m_uuid(uuid), m_form(Form::UUID) {}

    /// TOTAL: every offset names a real block -- the one the id identifies.
    static FF_Id generated(Offset block_offset) noexcept;
    static FF_Id interned(uint32_t trie_index) noexcept;
    static FF_Id raw_string(Offset string_offset) noexcept;
    static FF_Id pending() noexcept;

    [[nodiscard]] Form form() const noexcept { return m_form; }
    [[nodiscard]] bool is_generated() const noexcept { return m_form == Form::GENERATED; }
    [[nodiscard]] bool absent() const noexcept { return m_form == Form::ABSENT; }
    explicit operator bool() const noexcept { return m_form != Form::ABSENT; }

    /// PARTIAL, and deliberately so. Only GENERATED is a block offset; every
    /// other form yields FF_NULL_OFFSET, which is all-ones and can never be a
    /// real arena offset, so a caller who forgets to test degrades to a falsy
    /// node rather than chasing a wrong address.
    ///
    /// RAW_STRING is the row that makes this explicit rather than a bare
    /// reinterpretation: its payload IS an offset -- of the FF_STRING holding
    /// the text, not of the resource -- so returning it would be a plausible,
    /// in-bounds, self-validating WRONG answer (CLAUDE.md, wrong-answer class).
    [[nodiscard]] explicit operator Offset() const noexcept {
        return m_form == Form::GENERATED ? m_offset : FF_NULL_OFFSET;
    }

    /// PRECONDITION: the matching form. Checked and throwing, because reading
    /// the wrong arm is not a wrong answer but undefined behaviour -- the same
    /// contract DateTime::parts()/text() state one type up.
    [[nodiscard]] const FF_UUID& uuid() const;
    [[nodiscard]] uint32_t       index() const;
    [[nodiscard]] Offset         raw_string_offset() const;

    /// The three ordered tests (§17.2), the ONE place the slot's discrimination
    /// is written. Never throws: the read path degrades (invariant 10).
    [[nodiscard]] static FF_Id read_slot(const BYTE* slot) noexcept;

    /// Encode into the slot's 16 wire bytes. Throws on POINTER, which is build-
    /// time only and must be resolved at append (§17.11) before anything is
    /// written.
    void write_slot(BYTE* slot) const;

private:
    FF_UUID  m_uuid{};                    // UUID arm
    Offset   m_offset = FF_NULL_OFFSET;   // GENERATED, RAW_STRING
    uint32_t m_index  = FF_NULL_UINT32;   // INTERNED
    Form     m_form   = Form::ABSENT;
};
