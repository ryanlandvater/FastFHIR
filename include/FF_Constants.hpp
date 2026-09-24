/**
 * @file FF_Constants.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * @brief The vocabulary the wire format is written in: scalars, sentinels,
 *        enums and the constexpr helpers over them.
 *
 * THE BOTTOM LAYER. Nothing here has a body that touches an arena -- these are
 * the names and numbers every other header is phrased in. Split out of
 * FF_Primitives.hpp so the two things above it can be read on their own:
 *
 *     FF_Constants.hpp   <- you are here. Vocabulary.
 *     FF_Values.hpp      <- what a CONSUMER holds (Result, DateTime, FF_Id).
 *     FF_Primitives.hpp  <- the wire BLOCKS and the accessors that read them.
 *
 * Every value in this file is PERMANENT (CLAUDE.md invariant 1): a RECOVERY_TAG
 * or a dictionary ID that has been assigned is never renumbered and never
 * reordered, because those numbers decode archives that already exist.
 *
 * WHAT IS HERE, in order:
 *   Offset / Size / BYTE ........ the three scalars everything is measured in
 *   FF_NULL_* ................... the all-ones absence sentinels. A sentinel is
 *                                 always tested in its OWN domain -- the comment
 *                                 on FF_NULL_F64 records what happens otherwise
 *   FF_DateTimeParts + codec .... PACK / UNPACK / IS_FALLBACK for the 8-byte
 *                                 civil date/time slot
 *   engine + stream layout ...... FF_HEADER::VERSION's two halves
 *   RECOVERY_TAG registry ....... via generated_src/FF_RecoveryTags.hpp
 *   TYPE_SIZE_* ................. one name per slot width
 *   checksum / source / filter .. the small wire enums
 *   FF_FieldKind + ff_slot_width  THE SLOT CONTRACT. Three switches with no
 *                                 `default`, so a new kind is a -Wswitch error
 *                                 rather than a silent wrong answer
 *   FF_FieldKey / FF_FieldInfo .. how generated code names a slot
 *   date/time free functions .... PARSE / FORMAT / SIZE / STORE / ENCODE
 */
#pragma once

// Used by this file's own code, and nothing more.
#include <bit>           // std::bit_cast, in the date/time codec
#include <cstddef>       // std::size_t
#include <cstdint>       // the fixed-width types every constant is spelled in
#include <optional>      // FF_PARSE_DATETIME's return
#include <span>          // FF_FieldInfo::variants
#include <string>        // FF_FORMAT_DATETIME's return
#include <string_view>   // the date/time and field-key signatures

#include "FF_Export.h"
#include "FF_Version.hpp"

// =====================================================================
// CORE TYPES & CONSTANTS
// =====================================================================
typedef uint8_t BYTE;
typedef uint64_t Offset;
typedef uint64_t Size;

// Standard Integer MAX Nulls
constexpr uint8_t FF_NULL_UINT8 = 0xFF;
constexpr uint16_t FF_NULL_UINT16 = 0xFFFF;
constexpr uint32_t FF_NULL_UINT32 = 0xFFFFFFFF;
constexpr uint64_t FF_NULL_UINT64 = 0xFFFFFFFFFFFFFFFF;

// Code Null (Safely traps 0xFFFFFFFF before custom string masking)
constexpr uint32_t FF_CODE_NULL = FF_NULL_UINT32;

// Decimal Null. BIT PATTERN, NOT VALUE -- the distinction is the whole point.
//
// One constant, not a float/double pair: `decimal` is the only FHIR type that
// reaches a floating-point slot and it is always a binary64, so the 32-bit
// sentinel had no user and is gone.
//
// The wire invariant every fixed-width slot obeys is "all-ones raw bytes mean
// absent", and FF_IsFieldEmpty implements exactly that for FF_FIELD_FLOAT64:
// LOAD_U64(slot) == FF_NULL_UINT64. A float sentinel therefore has to BE those
// bytes, which only bit_cast produces.
//
// These were previously written `= FF_NULL_UINT64`, a numeric conversion: it
// yields the double 1.8446744073709552e19, whose IEEE-754 encoding is
// 0x43F0000000000000. That never equals all-ones, so no decimal slot ever
// reported empty -- every absent Quantity.value, Timing.repeat.period and
// Attachment.duration exported as the literal 1.84467e+19 instead of being
// omitted. The sentinel and the test for it must be spelled in the same
// domain; here that domain is bits.
//
// The all-ones double is a quiet NaN, so it is unordered under == by design:
// nothing may test a decimal for absence by comparing against this constant.
// Read the slot's raw bytes (FF_IsFieldEmpty) -- the same rule the offset,
// code and datetime sentinels already follow.
constexpr double FF_NULL_F64 = std::bit_cast<double>(FF_NULL_UINT64);
constexpr Offset FF_NULL_OFFSET = FF_NULL_UINT64;
// ── Reserved In-Flight Sentinels ──────────────────────────────────────
// PERMANENT AND RESERVED. These differ from the FF_NULL_* family above in
// kind, not just in value: a null sentinel is a legal on-wire value meaning
// "this field is absent", whereas a PENDING sentinel must NEVER appear in a
// sealed stream. It marks a slot a writer has reserved but not yet resolved.
//
// Why they cannot simply reuse the null sentinels: a null reads as "absent",
// so a placeholder that survived to disk would present as a cleanly dropped
// field instead of an incomplete write. Distinct values let a writer verify,
// before sealing, that every deferred slot was actually resolved -- and turn
// what would be silent data loss into a loud failure.
//
// They are declared here rather than kept private to a writer so the values
// are RESERVED: no future dictionary ID, offset, or flag may be assigned
// these bit patterns. Reserving them is what keeps them off the wire.
//
//   FF_PENDING_OFFSET  one below FF_NULL_OFFSET. Unreachable as a real arena
//                      offset (it would require an 16 EiB arena) and never
//                      confusable with "absent".
//   FF_PENDING_CODE    dictionary ID 0, which is permanently reserved as the
//                      null slot and never assigned to any code (see
//                      dictionaries/README.md, "Reserved values"). Distinct
//                      from FF_CODE_NULL (0xFFFFFFFF, "no code present").
//                      A resolved code slot always has bit 31 set or is a
//                      dictionary ID >= 1, so 0 is unambiguous.
//
// Current consumer: the Compactor's deferred-write machinery
// (src/FF_Compactor.cpp), which pre-fills every deferred slot with the
// matching sentinel and refuses to seal while one survives.
constexpr Offset FF_PENDING_OFFSET = FF_NULL_OFFSET - 1;
constexpr uint32_t FF_PENDING_CODE = 0x00000000;

// ── Unified Dynamic Code Block Flag ───────────────────────────────────
// Bit 31 (MSB) distinguishes dictionary codes from dynamic fallback blocks.
//   MSB = 0  → 31-bit dictionary index (FF_ResolveCode)
//   MSB = 1  → 31-bit signed relative offset to dynamic fallback block
// FF_CODED_VALUE_FLAG (old Bit 30) is REMOVED — bit 30 is reclaimed
// for data, doubling dictionary capacity and signed offset range.
constexpr uint32_t FF_CODED_VALUE_FLAG    = 0x80000000;  // Bit 31
constexpr uint32_t FF_CODE_PAYLOAD_MASK     = 0x7FFFFFFF;  // Lower 31 bits

// Hard cap on permanent dictionary size to prevent overflow into Bit 31.
// Enforced at generation time by generator/emit/dictionary.py.
constexpr uint32_t FF_CODE_DICTIONARY_MAX   = 0x7FFFFFFF;

// ── Packed Date/Time Slot ─────────────────────────────────────────────
// THE SAME SLOT CONTRACT AS FF_CODE ABOVE, WIDENED TO 8 BYTES. Read that
// block first; everything here is its counterpart, deliberately so:
//
//                    FF_CODE (4 bytes)              FF_DATETIME (8 bytes)
//   Discriminator    bit 31 (CODED_VALUE_FLAG)      bit 63 (FALLBACK_FLAG)
//   Flag clear       31-bit dictionary ID           63-bit packed civil value
//   Flag set         31-bit signed relative offset  63-bit signed relative offset
//                    to an FF_CODED_VALUE      to an FF_STRING
//   Offset is rel to the containing block           the containing block (same rule)
//   Sign-extension   FF_ResolveCodeableConceptOffset FF_ResolveDateTimeOffset
//   Null sentinel    FF_CODE_NULL (all ones)        FF_DATETIME_NULL (all ones)
//
// Two properties that make the parity exact rather than approximate:
//
//   1. The fallback offset is SIGNED and RELATIVE to the containing block, not
//      absolute. Absolute would have worked and would have been wrong: a second
//      convention for the same job is a thing to memorise instead of transfer.
//   2. The null sentinel lives inside the flag-set space and is reserved out of
//      it. All-ones has the flag set, so it would otherwise read as a relative
//      offset of 0x7FFF'FFFF'FFFF'FFFF -- an impossible one, which is what makes
//      it free to mean "absent". Test the null BEFORE the flag, exactly as the
//      code path does.
//
// AND IT IS AN EDGE, NOT INLINE DATA, WHENEVER THE FLAG IS SET. A packed value
// is structurally inert and validate_FFHR_stream() may skip it; a flagged one
// points somewhere and MUST be walked like any other offset (TASKS.md DT-1.5).
// That is the rule the code slot already follows (src/FF_Parser.cpp:573-590),
// not an exception to it.
constexpr uint64_t FF_DATETIME_FALLBACK_FLAG = 0x8000000000000000;  // Bit 63
constexpr uint64_t FF_DATETIME_PAYLOAD_MASK  = 0x7FFFFFFFFFFFFFFF;  // Lower 63
constexpr uint64_t FF_DATETIME_NULL          = FF_NULL_UINT64;

// Bit assignments as symbolic sums, low field first -- the same idiom the
// V-Table blocks below use (VALIDATION_S / VALIDATION). Never write a literal
// shift: the sums are the single definition, and a new field is inserted by
// adding a width, not by re-adding a column of numbers.
//
// Days sit at the TOP of the payload so that two values sharing a UTC offset
// compare chronologically as plain integers. That does NOT generalise to values
// with different offsets -- the offset field is below the time fields, so an
// integer compare orders by local wall time, not by instant. Equality is exact
// for every case; ordering is only meaningful within one offset.
enum FF_DateTimeBits : uint8_t
{
    FF_DT_PRECISION_S = 3,   // 7 values, see FF_DateTimePrecision
    FF_DT_OFFSET_S    = 11,  // signed minutes, -840..+840 (1,681 of 2,048 used)
    FF_DT_MILLI_S     = 10,  // 0..999
    FF_DT_SECOND_S    = 6,   // 0..60 -- 60 is representable, leap seconds survive
    FF_DT_MINUTE_S    = 6,   // 0..59
    FF_DT_HOUR_S      = 5,   // 0..23
    FF_DT_DAYS_S      = 22,  // civil days from 0001-01-01, UNSIGNED

    FF_DT_PRECISION = 0,
    FF_DT_OFFSET    = FF_DT_PRECISION + FF_DT_PRECISION_S,
    FF_DT_MILLI     = FF_DT_OFFSET    + FF_DT_OFFSET_S,
    FF_DT_SECOND    = FF_DT_MILLI     + FF_DT_MILLI_S,
    FF_DT_MINUTE    = FF_DT_SECOND    + FF_DT_SECOND_S,
    FF_DT_HOUR      = FF_DT_MINUTE    + FF_DT_MINUTE_S,
    FF_DT_DAYS      = FF_DT_HOUR      + FF_DT_HOUR_S,
    FF_DT_FLAG      = FF_DT_DAYS      + FF_DT_DAYS_S,
};
static_assert(FF_DT_FLAG == 63, "packed date/time payload must be exactly 63 bits");

// Days from 0001-01-01 to 1970-01-01. The epoch is 0001-01-01 and the field is
// UNSIGNED, and neither is a free choice: a signed count from the usual 1970
// epoch does not fit. 1970->9999 is 2,932,896 days and signed 22 bits reach only
// 2,097,151, which would cap the format at year 7711 while FHIR permits 9999.
// Measured from 0001-01-01 the whole legal span is 3,652,058 days against an
// unsigned 22-bit capacity of 4,194,303.
constexpr int64_t  FF_DATETIME_CIVIL_EPOCH = 719162;
constexpr uint32_t FF_DATETIME_MAX_DAYS    = 3652058;   // 9999-12-31

// How much of the value is populated. This expresses WITHIN-TYPE variation
// only: which of date/dateTime/time/instant a slot holds is the RECOVERY_TAG's
// job, not this field's, and keeping that split is what holds the enum to 3
// bits. FHIR's own grammar makes seconds mandatory once 'T' is present, so
// there is no MINUTE precision to represent.
//
// FRAC1..3 exist because ".5", ".50" and ".500" are one instant and three
// texts, and byte-exact round-trip means reproducing the text that arrived.
enum class FF_DateTimePrecision : uint8_t
{
    YEAR       = 0,   // "2024"
    YEAR_MONTH = 1,   // "2024-01"
    DATE       = 2,   // "2024-01-15"
    SECOND     = 3,   // "2024-01-15T13:45:30Z"      (no fractional part)
    FRAC1      = 4,   // "2024-01-15T13:45:30.5Z"
    FRAC2      = 5,   // "2024-01-15T13:45:30.50Z"
    FRAC3      = 6,   // "2024-01-15T13:45:30.500Z"
};

// 'Z' and "+00:00" are the same instant and different text, so the offset field
// distinguishes them rather than costing a bit elsewhere: 1,681 of its 2,048
// codes are legal offsets, leaving 367 spare, and one spare code carries the
// spelling. Numeric 0 means the text said "+00:00"; this sentinel means it said
// "Z". Chosen as the most negative 11-bit two's-complement value, which is
// unreachable as a real offset (legal range is -840..+840).
constexpr int16_t FF_DATETIME_OFFSET_Z   = -1024;
constexpr int16_t FF_DATETIME_OFFSET_MIN = -840;
constexpr int16_t FF_DATETIME_OFFSET_MAX = 840;

/// Unpacked form of the 8-byte slot.
///
/// PRECONDITION FOR BYTE-EXACT EQUALITY: every field the owning RECOVERY_TAG
/// does not make meaningful MUST be left at zero, so that two equal values are
/// one integer compare. The tag decides: RECOVER_FF_DATE has no time and no
/// offset, RECOVER_FF_TIME has no days and no offset, RECOVER_FF_DATETIME
/// carries an offset only at SECOND precision or finer (FHIR requires a
/// timezone once 'T' is present), RECOVER_FF_INSTANT always carries one.
/// Defaulting every member to zero is what makes that the natural outcome
/// rather than a rule to remember.
struct FF_DateTimeParts
{
    uint32_t days        = 0;   // from 0001-01-01; 0 when the tag carries no date
    uint8_t  hour        = 0;
    uint8_t  minute      = 0;
    uint8_t  second      = 0;   // 60 is legal
    uint16_t millisecond = 0;
    int16_t  utc_offset  = 0;   // minutes, or FF_DATETIME_OFFSET_Z for a literal 'Z'
    FF_DateTimePrecision precision = FF_DateTimePrecision::YEAR;
};

struct FF_CivilDate
{
    int32_t year  = 0;
    uint8_t month = 0;
    uint8_t day   = 0;
};

/// Days from 1970-01-01 for a civil date (Howard Hinnant's days_from_civil).
/// Kept in its published form rather than trimmed to this domain: the `era`
/// ternary is dead for year >= 1, and rewriting a proven algorithm to save a
/// conditional move is how such algorithms acquire bugs.
/// PRECONDITION: a real civil date; month 1-12, day valid for the month.
constexpr int64_t ff_days_from_civil(int32_t y, uint32_t m, uint32_t d) noexcept
{
    y -= (m <= 2);
    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);              // [0, 399]
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;    // [0, 365]
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             // [0, 146096]
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

/// Inverse of ff_days_from_civil (Hinnant's civil_from_days). Its `era` ternary
/// is likewise dead here -- the +719468 lands z above zero for every year in
/// 0001..9999, confirmed by mutation -- and is likewise kept. The signedness
/// that DOES matter is the epoch shift in ff_datetime_civil_from_days below:
/// computing that in unsigned arithmetic wraps every pre-1970 date.
constexpr FF_CivilDate ff_civil_from_days(int64_t z) noexcept
{
    z += 719468;
    const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = static_cast<uint32_t>(z - era * 146097);           // [0, 146096]
    const uint32_t yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;              // [0, 399]
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           // [0, 365]
    const uint32_t mp  = (5 * doy + 2) / 153;                               // [0, 11]
    const uint32_t d   = doy - (153 * mp + 2) / 5 + 1;                      // [1, 31]
    const uint32_t m   = mp + (mp < 10 ? 3 : -9);                           // [1, 12]
    return {static_cast<int32_t>(static_cast<int64_t>(yoe) + era * 400 + (m <= 2)),
            static_cast<uint8_t>(m), static_cast<uint8_t>(d)};
}

// Stored days are relative to 0001-01-01; the civil helpers above are relative
// to 1970-01-01. These two are the ONLY place that shift happens -- a second
// spelling of the epoch is a second chance to get it wrong.
constexpr uint32_t ff_datetime_days_from_civil(int32_t y, uint32_t m, uint32_t d) noexcept
{
    return static_cast<uint32_t>(ff_days_from_civil(y, m, d) + FF_DATETIME_CIVIL_EPOCH);
}
constexpr FF_CivilDate ff_datetime_civil_from_days(uint32_t days) noexcept
{
    return ff_civil_from_days(static_cast<int64_t>(days) - FF_DATETIME_CIVIL_EPOCH);
}
static_assert(ff_datetime_days_from_civil(1, 1, 1) == 0, "civil epoch is 0001-01-01");
static_assert(ff_datetime_days_from_civil(9999, 12, 31) == FF_DATETIME_MAX_DAYS,
              "9999-12-31 must be the last representable day");
static_assert(FF_DATETIME_MAX_DAYS < (1u << FF_DT_DAYS_S),
              "the legal FHIR year range must fit the days field");

constexpr uint64_t ff_dt_mask(uint8_t width) noexcept { return (1ull << width) - 1ull; }

/// Assemble the 8-byte slot. Bit 63 is always clear: a packed value is by
/// definition not a fallback offset. Every field is masked to its own width, so
/// an out-of-range component can corrupt only itself and never its neighbours --
/// range VALIDATION belongs to the caller (ff_datetime_fits), which decides
/// between packing and the FF_STRING fallback.
constexpr uint64_t FF_PACK_DATETIME(const FF_DateTimeParts& p) noexcept
{
    return (static_cast<uint64_t>(p.days)   & ff_dt_mask(FF_DT_DAYS_S))   << FF_DT_DAYS
         | (static_cast<uint64_t>(p.hour)   & ff_dt_mask(FF_DT_HOUR_S))   << FF_DT_HOUR
         | (static_cast<uint64_t>(p.minute) & ff_dt_mask(FF_DT_MINUTE_S)) << FF_DT_MINUTE
         | (static_cast<uint64_t>(p.second) & ff_dt_mask(FF_DT_SECOND_S)) << FF_DT_SECOND
         | (static_cast<uint64_t>(p.millisecond) & ff_dt_mask(FF_DT_MILLI_S)) << FF_DT_MILLI
         | (static_cast<uint64_t>(p.utc_offset)  & ff_dt_mask(FF_DT_OFFSET_S)) << FF_DT_OFFSET
         | (static_cast<uint64_t>(p.precision)   & ff_dt_mask(FF_DT_PRECISION_S));
}

/// Split the 8-byte slot. PRECONDITION: the slot is a packed value, i.e. it is
/// neither FF_DATETIME_NULL nor flagged -- test both before calling.
///
/// The offset is sign-extended by shifting its top bit up to bit 31 and back
/// down arithmetically, the same idiom FF_ResolveCodeableConceptOffset uses for
/// the 31-bit case.
constexpr FF_DateTimeParts FF_UNPACK_DATETIME(uint64_t slot) noexcept
{
    constexpr uint8_t OFF_PAD = 32 - FF_DT_OFFSET_S;
    const auto raw_off = static_cast<uint32_t>((slot >> FF_DT_OFFSET) & ff_dt_mask(FF_DT_OFFSET_S));
    return {
        static_cast<uint32_t>((slot >> FF_DT_DAYS)   & ff_dt_mask(FF_DT_DAYS_S)),
        static_cast<uint8_t> ((slot >> FF_DT_HOUR)   & ff_dt_mask(FF_DT_HOUR_S)),
        static_cast<uint8_t> ((slot >> FF_DT_MINUTE) & ff_dt_mask(FF_DT_MINUTE_S)),
        static_cast<uint8_t> ((slot >> FF_DT_SECOND) & ff_dt_mask(FF_DT_SECOND_S)),
        static_cast<uint16_t>((slot >> FF_DT_MILLI)  & ff_dt_mask(FF_DT_MILLI_S)),
        static_cast<int16_t> (static_cast<int32_t>(raw_off << OFF_PAD) >> OFF_PAD),
        static_cast<FF_DateTimePrecision>(slot & ff_dt_mask(FF_DT_PRECISION_S)),
    };
}

/// Does this value fit the packed form? Everything that does not -- a year
/// outside 0001..9999, more than 3 fractional digits (the caller reports that
/// as a precision it cannot express), an offset beyond +/-14:00 -- takes the
/// FF_STRING fallback instead, exactly as a non-dictionary code does.
constexpr bool ff_datetime_fits(const FF_DateTimeParts& p) noexcept
{
    return p.days <= FF_DATETIME_MAX_DAYS
        && p.hour <= 23 && p.minute <= 59 && p.second <= 60
        && p.millisecond <= 999
        && (p.utc_offset == FF_DATETIME_OFFSET_Z
            || (p.utc_offset >= FF_DATETIME_OFFSET_MIN && p.utc_offset <= FF_DATETIME_OFFSET_MAX));
}

/// Is this slot a relative offset to an FF_STRING rather than a packed value?
/// PRECONDITION: the caller has already excluded FF_DATETIME_NULL, which has
/// bit 63 set and would otherwise answer true. Same ordering as the code slot.
constexpr bool FF_DATETIME_IS_FALLBACK(uint64_t slot) noexcept
{
    return (slot & FF_DATETIME_FALLBACK_FLAG) != 0;
}

// ── CodeableConcept System Discriminator ───────────────────────────
// Byte at offset 10 of the CodeableConcept block.
// Values from FHIR terminology systems (§4.3.0.1):
//   https://build.fhir.org/terminologies-systems.html
enum class FF_CodeableConceptSystem : uint8_t {
    UNKNOWN           = 0x00,  // uint16_t URL index + raw code string

    // http://unitsofmeasure.org
    // raw ASCII UCUM expression (variable)
    UCUM              = 0x01,

    // http://snomed.info/sct
    // 8-byte native-endian concept ID (uint64_t)
    SNOMED_CT         = 0x02,

    // http://www.nlm.nih.gov/research/umls/rxnorm
    // 4-byte native-endian numeric code (uint32_t)
    RXNORM            = 0x03,

    // http://loinc.org
    // raw ASCII LOINC code (variable, alphanumeric with check digit)
    LOINC             = 0x04,

    // http://dicom.nema.org/resources/ontology/DCM
    // 4-byte native-endian tag (uint32_t)
    DICOM             = 0x05,

    // http://www.ama-assn.org/go/cpt
    // 2-byte native-endian numeric code (uint16_t)
    CPT               = 0x06,

    // http://hl7.org/fhir/sid/cvx
    // 1-byte native-endian vaccine code (uint8_t)
    CVX               = 0x07,

    // http://hl7.org/fhir/sid/ndc
    // raw ASCII NDC drug code (variable, contains dashes)
    NDC               = 0x08,

    // http://hl7.org/fhir/sid/icd-9-cm
    // raw ASCII ICD-9-CM code (variable, contains dots)
    ICD_9_CM          = 0x09,

    // http://hl7.org/fhir/sid/icd-10
    // raw ASCII ICD-10 code (variable, alphanumeric)
    ICD_10            = 0x0A,

    // urn:iso:std:iso:3166
    // raw ASCII ISO-3166 country code (variable, 2-letter)
    ISO_3166          = 0x0B,

    // urn:iso:std:iso:11073:10101
    // 4-byte native-endian numeric code (uint32_t)
    MDC               = 0x0C,

    // http://fdasis.nlm.nih.gov
    // raw ASCII UNII ingredient code (variable)
    UNII              = 0x0D,

    // http://va.gov/terminology/medrt
    // 8-byte native-endian numeric code (uint64_t)
    MED_RT            = 0x0E,

    // https://fhir.infoway-inforoute.ca/CodeSystem/pCLOCD
    // raw ASCII pCLOCD pan-Canadian code (variable)
    PCLOCD            = 0x0F,

    // http://hl7.org/fhir/ (IDMP medicinal product codes)
    // 8-byte native-endian concept ID (uint64_t)
    IDMP              = 0x10,

    // 0x11–0xFE  reserved for future external systems
    FHIR_DICTIONARY   = 0xFF,  // dictionary-resolved code (not a CodeableConcept block)
};


// FastFHIR magic bytes: "FFHR" in little-endian
constexpr uint32_t FF_MAGIC_BYTES = 0x52484646;

enum FHIR_VERSION : uint16_t
{
    FHIR_VERSION_R4 = 0x0400,
    FHIR_VERSION_R5 = 0x0500,
};

// =====================================================================
// STREAM LAYOUT MODE (encoded in FF_HEADER::VERSION high bits)
// =====================================================================
enum FF_StreamCompaction : uint8_t
{
    FF_STREAM_COMPACTION_NONE = 0,
    FF_STREAM_COMPACTED = 1,
};

constexpr uint32_t FF_STREAM_LAYOUT_BITS = 2;
constexpr uint32_t FF_STREAM_LAYOUT_SHIFT = 32 - FF_STREAM_LAYOUT_BITS;
constexpr uint32_t FF_STREAM_LAYOUT_MASK = (0x3u << FF_STREAM_LAYOUT_SHIFT);
constexpr uint32_t FF_ENGINE_VERSION_MASK = ~FF_STREAM_LAYOUT_MASK;

inline constexpr uint32_t FF_ENCODE_HEADER_VERSION(uint32_t engine_version, FF_StreamCompaction layout)
{
    return (engine_version & FF_ENGINE_VERSION_MASK) |
           ((static_cast<uint32_t>(layout) << FF_STREAM_LAYOUT_SHIFT) & FF_STREAM_LAYOUT_MASK);
}

inline constexpr uint32_t FF_HEADER_ENGINE_VERSION(uint32_t encoded_version)
{
    return encoded_version & FF_ENGINE_VERSION_MASK;
}

/// Extract the MAJOR component from a 30-bit engine version word produced by
/// FF_HEADER_ENGINE_VERSION().  MAJOR occupies bits 29–16 (14 usable bits).
inline constexpr uint16_t FF_ENGINE_MAJOR(uint32_t engine_version) noexcept
{
    return static_cast<uint16_t>((engine_version >> 16) & 0x3FFFu);
}

/// Extract the MINOR component (bits 15–0) from an engine version word.
inline constexpr uint16_t FF_ENGINE_MINOR(uint32_t engine_version) noexcept
{
    return static_cast<uint16_t>(engine_version & 0xFFFFu);
}

inline constexpr FF_StreamCompaction FF_HEADER_STREAM_LAYOUT(uint32_t encoded_version)
{
    return static_cast<FF_StreamCompaction>((encoded_version & FF_STREAM_LAYOUT_MASK) >> FF_STREAM_LAYOUT_SHIFT);
}


// =====================================================================
// RECOVERY TAG REGISTRY (auto-generated from FHIR StructureDefinitions)
// =====================================================================
#include "FF_RecoveryTags.hpp"

// =====================================================================
// TYPE SIZE CONSTANTS
// =====================================================================
enum TYPE_SIZE : uint8_t
{
    TYPE_SIZE_UINT8 = 1,
    TYPE_SIZE_UINT16 = 2,
    TYPE_SIZE_UINT24 = 3,
    TYPE_SIZE_UINT32 = 4,
    TYPE_SIZE_INT32 = 4,
    TYPE_SIZE_UINT64 = 8,
    TYPE_SIZE_OFFSET = 8,
    TYPE_SIZE_DECIMAL = 9,
    TYPE_SIZE_RESOURCE = 10,
    TYPE_SIZE_CHOICE = 10,
    TYPE_SIZE_IDENTITY = 16,
};

// =====================================================================
// SUPPORTED CHECKSUM ALGORITHMS
// =====================================================================
enum FF_Checksum_Algorithm : uint16_t
{
    FF_CHECKSUM_NONE = 0,
    FF_CHECKSUM_CRC32 = 1,  // 4 bytes  (32 bits)
    FF_CHECKSUM_MD5 = 2,    // 16 bytes (128 bits)
    FF_CHECKSUM_SHA256 = 3, // 32 bytes (256 bits)
};

// Define the maximum inline hash size (256 bits = 32 bytes)
constexpr uint32_t FF_MAX_HASH_BYTES = 32;

// =====================================================================
// SOURCE FORMAT (ingestion)
// =====================================================================
enum FF_SourceType : uint8_t
{
    FF_SOURCE_FHIR_JSON = 0, // Standard FHIR JSON (R4/R5)
    FF_SOURCE_HL7_V2    = 1, // Pipe-delimited (ER7)
    FF_SOURCE_HL7_V3    = 2, // XML-based (CDA)
};

// =====================================================================
// EXTENSION URL FILTER (predigestion)
// =====================================================================
enum class FF_ExtensionFilterMode : uint8_t
{
    FILTER_ALL_KNOWN = 0,   // Suppress profile-native + HL7-known-safe (default)
    FILTER_NATIVE_ONLY = 1, // Suppress only profile-native extensions
    FILTER_NONE = 2,        // Store all extension URLs in the FF_URL_DIRECTORY
};

// =====================================================================
// FORWARD DECLARATIONS FOR DATA TYPES
// =====================================================================
enum FF_FieldKind : uint16_t
{
    FF_FIELD_UNKNOWN = 0,
    FF_FIELD_STRING,
    FF_FIELD_ARRAY,
    FF_FIELD_BLOCK,
    FF_FIELD_CODE,
    FF_FIELD_BOOL,
    FF_FIELD_INT32,
    FF_FIELD_UINT32,
    FF_FIELD_INT64,
    FF_FIELD_UINT64,
    FF_FIELD_FLOAT64,
    FF_FIELD_RESOURCE,
    FF_FIELD_CHOICE,
    // ONE kind for all four date/time tags. Which of date/dateTime/time/instant
    // a slot holds is the RECOVERY_TAG's job;
    FF_FIELD_DATETIME,
    // A 4-byte URL-directory ref: the FF_URL_DIRECTORY entry index for a URL
    // interned during predigestion (Extension.url, Bundle.entry.fullUrl). The
    // reader resolves the ref back to the full URL string via get_url();
    // FF_EXT_REF_NULL (all ones) is the absent marker.
    FF_FIELD_URL,
    // A 16-byte identity slot (§17): wide enough to hold a UUID INLINE, so a
    // reference compare or a resolve against a fullUrl is a plain 16-byte
    // compare with no directory hop and no text reconstruction. Four arms, all
    // discriminated by the slot's own bytes -- an inline UUID, a block offset
    // we minted (GENERATED), a URL-directory index (INTERNED), or an FF_STRING
    // offset holding the text verbatim (RAW_STRING). FF_Id is the value type;
    // FF_Id::read_slot is the ONE place the discrimination is written.
    FF_FIELD_ID,
};

// =====================================================================
// SLOT WIDTH — the one definition
// =====================================================================
// How many bytes a field of this kind occupies in its parent block. This is
// the SINGLE source of truth; every other spelling of these numbers must be a
// projection of this function, never an independent derivation:
//
//   * the compactor sizes its dense slots with it (src/FF_Compactor.cpp)
//   * the generated COMPACT_SLOT_SIZES tables are emitted as calls to it, so
//     the compact reader and writer consume the same constants
//   * generated blocks static_assert their V-Table widths against it
//
// A previous version had the generator re-derive these from FHIR type names
// while C++ switched on the kind. They disagreed on 1,393 of 1,611 slots and
// the compact reader read every field after the first string from the wrong
// address. Do not reintroduce a second derivation -- add a case here. No
// default on purpose: a new FF_FieldKind without a width case is a compile
// error (-Wswitch), never a silent TYPE_SIZE_OFFSET guess.
constexpr uint8_t ff_slot_width(const FF_FieldKind kind)
{
    switch (kind)
    {
    case FF_FIELD_BOOL:     return TYPE_SIZE_UINT8;
    case FF_FIELD_INT32:    return TYPE_SIZE_INT32;
    case FF_FIELD_UINT32:   return TYPE_SIZE_UINT32;
    case FF_FIELD_INT64:    return TYPE_SIZE_UINT64;
    case FF_FIELD_UINT64:   return TYPE_SIZE_UINT64;
    case FF_FIELD_FLOAT64:  return TYPE_SIZE_DECIMAL;
    case FF_FIELD_CODE:     return TYPE_SIZE_UINT32;
    case FF_FIELD_URL:      return TYPE_SIZE_UINT32;
    case FF_FIELD_DATETIME: return TYPE_SIZE_UINT64;
    case FF_FIELD_ID:       return TYPE_SIZE_IDENTITY;
    case FF_FIELD_RESOURCE: return TYPE_SIZE_RESOURCE;
    case FF_FIELD_CHOICE:   return TYPE_SIZE_CHOICE;
    // STRING, ARRAY, BLOCK and UNKNOWN hold an arena offset.
    case FF_FIELD_STRING:
    case FF_FIELD_ARRAY:
    case FF_FIELD_BLOCK:
    case FF_FIELD_UNKNOWN:
        return TYPE_SIZE_OFFSET;
    }
}

// =====================================================================
// DECIMAL SLOT — [ double (8) | sigfigs (1) ]
// =====================================================================
// The kind is FF_FIELD_FLOAT64 and stays that way on purpose. FHIR `decimal`
// is its only occupant, but the slot's PRIMARY view -- the one most consumers
// want -- is a plain IEEE-754 double, and the kind is named for the view, not
// for the FHIR type. Reading the slot as a float64 and ignoring the 9th byte
// is a supported, complete way to consume it: a column store, an index, or any
// arithmetic that does not care how the number was typed gets exactly what it
// needs from the first 8 bytes with no decode. Sigfigs is an addendum for the
// one consumer that needs lexical fidelity -- the JSON exporter.
//
// Two values that are numerically equal but were written differently (`100.0`
// and `100`) therefore have IDENTICAL first 8 bytes and differ only at +8.
// That is the intended semantics: equal as numbers, distinguishable as text.
//
// The slot carries two things that cannot be derived from one another:
//
//   +0  IEEE-754 binary64, the VALUE. Numerically exact and readable with a
//       plain LOAD_F64 -- a downstream consumer (a column store, an index)
//       maps these 8 bytes as a DOUBLE and sorts and filters on them natively,
//       with no decode step. Nothing may be smuggled into this field.
//   +8  sigfigs: how many digits followed the '.' in the SOURCE document --
//       the decimal SCALE, which is what `%.*f` consumes on the way out.
//       Not the count of significant figures: `62.00` stores 2 here and has
//       four sig figs, `0.0895` stores 4 and has three. Scale is the quantity
//       that reproduces the source text; sig figs is not (a %g render at four
//       sig figs gives `62`, dropping the very zeros this exists to keep).
//
// Why it cannot live in the double: FHIR treats trailing zeros as significant
// (`62.00` asserts hundredths, `62` does not), and every bit pattern of a
// binary64 is already a legal value, so there is nowhere to put the
// information. It is metadata about the source text, not about the number --
// the same split DT-2 made for date/time, where a 3-bit precision field is
// what keeps "2024" from round-tripping as "2024-01-01T00:00:00Z".
//
// FF_DECIMAL_SIGFIGS_UNSPECIFIED means "nothing recorded" -- exponent notation
// in the source, or more fractional digits than a double can distinguish. The
// exporter then falls back to the shortest representation that round-trips to
// the stored bits, which is the faithful rendering when the source form is
// unknown. Absence of the whole field is still the all-ones double at +0
// (FF_IsFieldEmpty); this byte says nothing about presence.
constexpr uint8_t FF_DECIMAL_SIGFIGS_UNSPECIFIED = FF_NULL_UINT8;

// Largest sigfigs a binary64 can actually distinguish. Beyond this the digits are
// noise, and recording them would make the exporter print noise back.
constexpr uint8_t FF_DECIMAL_SIGFIGS_MAX = 17;

// Sigfigs of a JSON number token, as written. Exponent forms and over-long
// fractions record no sigfigs rather than a wrong one -- a guess here prints
// digits the source never had.
constexpr uint8_t FF_DecimalSigfigsFromToken(std::string_view token)
{
    if (token.find('e') != std::string_view::npos ||
        token.find('E') != std::string_view::npos)
        return FF_DECIMAL_SIGFIGS_UNSPECIFIED;

    const size_t dot = token.find('.');
    if (dot == std::string_view::npos) return 0;  // integer literal: "100" -> "100"

    // raw_json_token() may carry trailing whitespace up to the next structural
    // character; count only the digits.
    size_t digits = 0;
    for (size_t i = dot + 1; i < token.size(); ++i)
    {
        if (token[i] < '0' || token[i] > '9') break;
        ++digits;
    }
    return digits > FF_DECIMAL_SIGFIGS_MAX ? FF_DECIMAL_SIGFIGS_UNSPECIFIED
                                         : static_cast<uint8_t>(digits);
}

// =====================================================================
// INLINE SCALAR vs DATA_BLOCK — the other half of the slot contract
// =====================================================================
// True when a slot of this kind holds its value IN the slot, false when the
// slot points at a DATA_BLOCK. Serialization turns on the distinction: an
// inline scalar always renders exactly one JSON token, whereas a block child
// can be vacuous -- every field absent -- and render nothing at all.
//
// Single source of truth, for the same reason ff_slot_width above is one:
// Reflective::Node::is_scalar() and the JSON field dispatch in
// Node::print_json used to carry independent copies of this list, and a kind
// missing from either one silently changed how a field printed. No default for
// the same reason ff_slot_width has none: a kind added to the enum without
// triage here is a compile error, not a silent misclassification.
constexpr bool ff_kind_is_inline_scalar(const FF_FieldKind kind)
{
    switch (kind)
    {
    case FF_FIELD_BOOL:
    case FF_FIELD_INT32:
    case FF_FIELD_UINT32:
    case FF_FIELD_INT64:
    case FF_FIELD_UINT64:
    case FF_FIELD_FLOAT64:
    case FF_FIELD_CODE:
    case FF_FIELD_DATETIME:
    case FF_FIELD_URL:
    case FF_FIELD_ID:
        return true;

    // STRING, ARRAY, BLOCK, RESOURCE, CHOICE and UNKNOWN all resolve through
    // an offset to a block.
    case FF_FIELD_STRING:
    case FF_FIELD_ARRAY:
    case FF_FIELD_BLOCK:
    case FF_FIELD_RESOURCE:
    case FF_FIELD_CHOICE:
    case FF_FIELD_UNKNOWN:
        return false;
    }
}

inline RECOVERY_TAG Kind_to_Recovery(const FF_FieldKind kind)
{
    switch (kind)
    {
    case FF_FIELD_STRING:
        return RECOVER_FF_STRING;
    case FF_FIELD_BOOL:
        return RECOVER_FF_BOOL;
    case FF_FIELD_INT32:
        return RECOVER_FF_INT32;
    case FF_FIELD_UINT32:
        return RECOVER_FF_UINT32;
    case FF_FIELD_INT64:
        return RECOVER_FF_INT64;
    case FF_FIELD_UINT64:
        return RECOVER_FF_UINT64;
    case FF_FIELD_FLOAT64:
        return RECOVER_FF_FLOAT64;
    case FF_FIELD_CODE:
        return RECOVER_FF_CODE;
    case FF_FIELD_URL:
        // A URL-directory ref reads as its URL string.
        return RECOVER_FF_STRING;
    // FF_FIELD_DATETIME is deliberately ABSENT. This mapping is a function only
    // where a kind names exactly one tag, and the date/time kind names four
    // (DATE, DATETIME, TIME, INSTANT). Guessing DATETIME would be wrong three
    // times in four and would surface as a `date` exported as `valueDateTime` --
    // the same class of defect the DT work order exists to remove. Every caller
    // uses this only as a fallback when FF_FieldKey::child_recovery is
    // UNDEFINED, and a generated date/time key always carries its specific tag,
    // so the fallback must not fire; returning UNDEFINED keeps "I do not know"
    // honest instead of laundering it into a plausible wrong answer.
    default:
        return FF_RECOVER_UNDEFINED;
    }
}
// Compile-time trait dispatch for C++ API
template <RECOVERY_TAG T_Tag>
struct RecoveryTraits
{
    static constexpr FF_FieldKind kind = (T_Tag >= 0x0200) ? FF_FIELD_BLOCK : FF_FIELD_UNKNOWN;
};
template <>
struct RecoveryTraits<RECOVER_FF_BOOL>
{
    static constexpr FF_FieldKind kind = FF_FIELD_BOOL;
};
template <>
struct RecoveryTraits<RECOVER_FF_INT32>
{
    static constexpr FF_FieldKind kind = FF_FIELD_INT32;
};
template <>
struct RecoveryTraits<RECOVER_FF_UINT32>
{
    static constexpr FF_FieldKind kind = FF_FIELD_UINT32;
};
template <>
struct RecoveryTraits<RECOVER_FF_INT64>
{
    static constexpr FF_FieldKind kind = FF_FIELD_INT64;
};
template <>
struct RecoveryTraits<RECOVER_FF_UINT64>
{
    static constexpr FF_FieldKind kind = FF_FIELD_UINT64;
};
template <>
struct RecoveryTraits<RECOVER_FF_FLOAT64>
{
    static constexpr FF_FieldKind kind = FF_FIELD_FLOAT64;
};
template <>
struct RecoveryTraits<RECOVER_FF_CODE>
{
    static constexpr FF_FieldKind kind = FF_FIELD_CODE;
};
// The compile-time half of Recovery_to_Kind, and it must agree with it: two
// mappings from the same tags to the same kinds is two chances to disagree, so
// the static_asserts after this block pin them together.
template <>
struct RecoveryTraits<RECOVER_FF_DATE>
{
    static constexpr FF_FieldKind kind = FF_FIELD_DATETIME;
};
template <>
struct RecoveryTraits<RECOVER_FF_DATETIME>
{
    static constexpr FF_FieldKind kind = FF_FIELD_DATETIME;
};
template <>
struct RecoveryTraits<RECOVER_FF_TIME>
{
    static constexpr FF_FieldKind kind = FF_FIELD_DATETIME;
};
template <>
struct RecoveryTraits<RECOVER_FF_INSTANT>
{
    static constexpr FF_FieldKind kind = FF_FIELD_DATETIME;
};
template <>
struct RecoveryTraits<RECOVER_FF_STRING>
{
    static constexpr FF_FieldKind kind = FF_FIELD_STRING;
};
template <>
struct RecoveryTraits<RECOVER_FF_RESOURCE>
{
    static constexpr FF_FieldKind kind = FF_FIELD_RESOURCE;
};
template <>
struct RecoveryTraits<RECOVER_FF_OPAQUE_JSON>
{
    static constexpr FF_FieldKind kind = FF_FIELD_STRING;
};

// Does this tag name a block laid out exactly like an FF_STRING?
//
// The dual type system separates PHYSICAL layout (the kind) from SEMANTIC
// identity (the tag), and this is the one place two tags share a layout:
// RECOVER_FF_OPAQUE_JSON is byte-for-byte an FF_STRING -- 14-byte header, then
// `length` payload bytes -- but the payload is already-serialized JSON rather
// than a JSON string value. So every reader that walks, bounds-checks or
// decodes the BYTES asks this question, and only the two places that decide how
// to RENDER them (Node::print_json, Node::to_debug_json) test the tag itself.
//
// Kept as one predicate rather than two `==` comparisons at each site for the
// reason ff_kind_is_inline_scalar is: the last four defects in this file's
// history were a list of tags copied to a second site and then extended at only
// one of them.
constexpr bool FF_IsStringLayoutTag(RECOVERY_TAG tag)
{
    return tag == RECOVER_FF_STRING || tag == RECOVER_FF_OPAQUE_JSON;
}

// Exhaustive runtime mapping for dynamic bindings (Python/JSON)
inline constexpr FF_FieldKind Recovery_to_Kind(RECOVERY_TAG tag)
{
    RECOVERY_TAG base = GetTypeFromTag(tag);
    // The scalar band (0x01xx) is the membership test for "inline scalar".
    // Every inline value type belongs in it, RECOVER_FF_CODE included: it moved
    // out of the primitive band to 0x010B on 2026-08-19 precisely so this branch
    // can reach it. While it sat at 0x0003, 0x0003 & 0xFF00 == 0x0000, so the
    // case below was unreachable and the live handling was a duplicate in the
    // second switch -- and the same misbanding silently disabled the code path
    // in the compactor's write_choice_slot. The primitive band was compacted
    // afterwards, so 0x0003 now belongs to RECOVER_FF_RESOURCE.
    if ((base & 0xFF00) == RECOVER_FF_SCALAR_BLOCK)
    {
        switch (base)
        {
        case RECOVER_FF_BOOL:
            return FF_FIELD_BOOL;
        case RECOVER_FF_INT32:
            return FF_FIELD_INT32;
        case RECOVER_FF_UINT32:
            return FF_FIELD_UINT32;
        case RECOVER_FF_INT64:
            return FF_FIELD_INT64;
        case RECOVER_FF_UINT64:
            return FF_FIELD_UINT64;
        case RECOVER_FF_FLOAT64:
            return FF_FIELD_FLOAT64;
        case RECOVER_FF_CODE:
            return FF_FIELD_CODE;
        case RECOVER_FF_DATE:
        case RECOVER_FF_DATETIME:
        case RECOVER_FF_TIME:
        case RECOVER_FF_INSTANT:
            return FF_FIELD_DATETIME;
        default:
            return FF_FIELD_UNKNOWN;
        }
    }
    switch (base)
    {
    // Both string-layout tags land on the string KIND -- the kind names the
    // bytes, and these bytes are identical. What separates them is what the
    // payload MEANS, which only the render sites consult. See
    // FF_IsStringLayoutTag.
    case RECOVER_FF_STRING:
    case RECOVER_FF_OPAQUE_JSON:
        return FF_FIELD_STRING;
    case RECOVER_FF_RESOURCE:
        return FF_FIELD_RESOURCE;
    default:
        return (base >= RECOVER_FF_DATA_TYPE_BLOCK) ? FF_FIELD_BLOCK : FF_FIELD_UNKNOWN;
    }
}

// The two tag->kind mappings above (compile-time RecoveryTraits, runtime
// Recovery_to_Kind) are independent code paths over the same facts, which is
// exactly the shape that drifts. Pin them. The width assert is here too because
// the packed date/time is only "free" if it did not widen its slot: 8 bytes is
// what FF_FIELD_STRING already occupied, so routing these types off STRING_TYPES
// in DT-2 moves no V-Table offset.
static_assert(Recovery_to_Kind(RECOVER_FF_DATE) == RecoveryTraits<RECOVER_FF_DATE>::kind &&
              Recovery_to_Kind(RECOVER_FF_DATETIME) == RecoveryTraits<RECOVER_FF_DATETIME>::kind &&
              Recovery_to_Kind(RECOVER_FF_TIME) == RecoveryTraits<RECOVER_FF_TIME>::kind &&
              Recovery_to_Kind(RECOVER_FF_INSTANT) == RecoveryTraits<RECOVER_FF_INSTANT>::kind,
              "the compile-time and runtime tag->kind mappings disagree for date/time");
static_assert(Recovery_to_Kind(RECOVER_FF_DATE) == FF_FIELD_DATETIME,
              "all four date/time tags must collapse to the single FF_FIELD_DATETIME kind");
static_assert(ff_slot_width(FF_FIELD_DATETIME) == TYPE_SIZE_UINT64,
              "a packed date/time must occupy the same 8 bytes the string offset did");
// Same pinning for the two string-layout tags, for the same reason: the
// predicate and the two mappings are three independent statements of one fact.
static_assert(Recovery_to_Kind(RECOVER_FF_OPAQUE_JSON) ==
                      RecoveryTraits<RECOVER_FF_OPAQUE_JSON>::kind &&
                  Recovery_to_Kind(RECOVER_FF_STRING) == RecoveryTraits<RECOVER_FF_STRING>::kind,
              "the compile-time and runtime tag->kind mappings disagree for the string layout");
static_assert(FF_IsStringLayoutTag(RECOVER_FF_STRING) &&
                  FF_IsStringLayoutTag(RECOVER_FF_OPAQUE_JSON) &&
                  Recovery_to_Kind(RECOVER_FF_OPAQUE_JSON) == FF_FIELD_STRING,
              "every string-layout tag must map to FF_FIELD_STRING");
static_assert(!FF_IsStringLayoutTag(RECOVER_FF_CODED_VALUE) &&
                  !FF_IsStringLayoutTag(RECOVER_FF_RESOURCE),
              "FF_IsStringLayoutTag must not claim blocks that merely contain text");

struct FF_FieldInfo
{
    const char *name = nullptr;
    FF_FieldKind kind = FF_FIELD_UNKNOWN;
    uint16_t field_offset = 0;
    RECOVERY_TAG child_recovery = FF_RECOVER_UNDEFINED;
    uint8_t array_entries_are_offsets = 0;
    /// FF_FIELD_CHOICE only: every tag the field's StructureDefinitions allow
    /// its tuple to carry, over every revision this build reads. Empty for any
    /// other kind. Recovery refuses a choice tag outside this set, since the
    /// writer can never have stored it there.
    std::span<const RECOVERY_TAG> variants = {};
};
struct FF_FieldKey
{
    RECOVERY_TAG owner_recovery = FF_RECOVER_UNDEFINED;
    FF_FieldKind kind = FF_FIELD_UNKNOWN;
    uint16_t field_offset = 0;
    RECOVERY_TAG child_recovery = FF_RECOVER_UNDEFINED;
    uint8_t array_entries_are_offsets = 0;
    const char *name = nullptr;
    std::size_t name_len = 0;

    constexpr FF_FieldKey() = default;

    template <std::size_t N>
    constexpr FF_FieldKey(const char (&literal)[N]) noexcept : name(literal), name_len(N - 1) {}

    template <std::size_t N>
    constexpr FF_FieldKey(RECOVERY_TAG owner,
                          FF_FieldKind field_kind,
                          uint16_t offset,
                          RECOVERY_TAG child,
                          uint8_t array_offsets,
                          const char (&field_name)[N]) noexcept
        : owner_recovery(owner),
          kind(field_kind),
          field_offset(offset),
          child_recovery(child),
          array_entries_are_offsets(array_offsets),
          name(field_name),
          name_len(N - 1) {}

    constexpr FF_FieldKey(RECOVERY_TAG owner,
                          FF_FieldKind field_kind,
                          uint16_t offset,
                          RECOVERY_TAG child,
                          uint8_t array_offsets,
                          const char *field_name,
                          std::size_t field_name_len) noexcept
        : owner_recovery(owner),
          kind(field_kind),
          field_offset(offset),
          child_recovery(child),
          array_entries_are_offsets(array_offsets),
          name(field_name),
          name_len(field_name_len) {}

    constexpr FF_FieldKey(const char *key_name, std::size_t key_name_len) noexcept
        : name(key_name),
          name_len(key_name_len) {}

    static FF_FieldKey from_cstr(const char *key_name) noexcept
    {
        return FF_FieldKey(key_name, key_name ? std::char_traits<char>::length(key_name) : 0);
    }

    static FF_FieldKey from_cstr(RECOVERY_TAG owner,
                                 FF_FieldKind field_kind,
                                 uint16_t offset,
                                 RECOVERY_TAG child,
                                 uint8_t array_offsets,
                                 const char *field_name) noexcept
    {
        return FF_FieldKey(owner,
                           field_kind,
                           offset,
                           child,
                           array_offsets,
                           field_name,
                           field_name ? std::char_traits<char>::length(field_name) : 0);
    }

    constexpr std::string_view view() const noexcept
    {
        return (name && name_len > 0) ? std::string_view{name, name_len} : std::string_view{};
    }

    constexpr operator std::string_view() const noexcept { return view(); }
};

struct DATA_BLOCK;  // Base structure for all data blocks
struct FF_HEADER;   // Stream header block
struct FF_CHECKSUM; // Checksum block
struct FF_ARRAY;    // Array template block
struct FF_STRING;   // String block


// ── Packed date/time: the same three functions, one width up ──────────
// SIZE/STORE/ENCODE split exactly as the code emitters above, and for the same
// reason: SIZE answers "how many bytes of child space does this need" before
// the arena is claimed, STORE writes the fallback block if there is one, and
// ENCODE returns the V-Table slot. `tag` selects the per-type FHIR rules the
// way `system` does for codes -- RECOVER_FF_DATE rejects a timezone,
// RECOVER_FF_TIME rejects a date, RECOVER_FF_INSTANT demands both -- which is
// how four tags share one encoder.
//
// Text that parses and fits returns a packed value (MSB=0). Text that is legal
// FHIR but does not fit the 63 bits (a year outside 0001..9999, more than three
// fractional digits) returns a relative offset with FF_DATETIME_FALLBACK_FLAG
// set (MSB=1) to an FF_STRING holding the ORIGINAL text, so the round trip is
// byte-exact either way. Empty text returns FF_DATETIME_NULL.
std::optional<FF_DateTimeParts> FF_PARSE_DATETIME(std::string_view text, RECOVERY_TAG tag);
std::string FF_FORMAT_DATETIME(const FF_DateTimeParts &parts, RECOVERY_TAG tag);
Size SIZE_FF_DATETIME(std::string_view text, RECOVERY_TAG tag);
Size STORE_FF_DATETIME(BYTE *const __base, Offset start_offset,
                          std::string_view text, RECOVERY_TAG tag);
uint64_t ENCODE_FF_DATETIME(BYTE *const __base, Offset block_offset, Offset &child_off,
                             std::string_view text, RECOVERY_TAG tag);

// Is this tag one of the four date/time types?
//
// A choice ([x]) slot needs this at runtime and cannot get it from the field
// kind: the POD holds the text as a std::string_view exactly like `string`
// does, so the C++ type the store dispatches on is identical for both, and the
// TAG is the only thing that separates valueDateTime from valueString. Writing
// a date/time variant as a plain FF_STRING offset is not merely a wrong label
// -- the reader unpacks those 8 bytes as a packed civil value and produces a
// garbage date.
//
// The four are contiguous by construction; the static_asserts hold the range
// to the enum so a fifth date/time tag cannot be appended past the end and
// silently fall outside.
constexpr bool FF_IsDateTimeTag(RECOVERY_TAG tag)
{
    return tag >= RECOVER_FF_DATE && tag <= RECOVER_FF_INSTANT;
}
static_assert(RECOVER_FF_DATE < RECOVER_FF_DATETIME &&
                  RECOVER_FF_DATETIME < RECOVER_FF_TIME &&
                  RECOVER_FF_TIME < RECOVER_FF_INSTANT,
              "FF_IsDateTimeTag assumes the four date/time tags are contiguous and ordered");
static_assert(FF_IsDateTimeTag(RECOVER_FF_DATE) && FF_IsDateTimeTag(RECOVER_FF_DATETIME) &&
                  FF_IsDateTimeTag(RECOVER_FF_TIME) && FF_IsDateTimeTag(RECOVER_FF_INSTANT),
              "every date/time tag must satisfy FF_IsDateTimeTag");
static_assert(!FF_IsDateTimeTag(RECOVER_FF_STRING) && !FF_IsDateTimeTag(RECOVER_FF_CODE),
              "FF_IsDateTimeTag must not claim the string or code tags");
