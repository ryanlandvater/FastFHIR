/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * test_identity — the FF_FIELD_ID kind and the identity value types (§17).
 *
 * P3 adds the kind, its width, and the FF_UUID/FF_Id value types. NO field
 * uses the kind yet, so nothing here builds a stream: the units under test are
 * the slot codec (write_slot/read_slot) and the value type's own contract.
 *
 * WHAT THIS PINS, and why each matters:
 *   - ff_slot_width(FF_FIELD_ID) == 16 and the enum value, because the enum is
 *     readonly-ABI and the compactor sizes dense slots from the width.
 *   - The three ordered tests (§17.2) against HAND-BUILT slot bytes, not just
 *     bytes this type wrote. A codec tested only by round-tripping through
 *     itself proves self-consistency, which a wrong-but-symmetric layout also
 *     has; the hand-built slots prove the layout is the one §17.2 specifies.
 *   - The absence sentinel FIRST: an all-ones slot would otherwise read as a
 *     UUID with version nibble 15.
 *   - explicit operator Offset() returns FF_NULL_OFFSET for every non-GENERATED
 *     arm, INCLUDING RAW_STRING, whose payload is itself a plausible offset and
 *     is the one that would otherwise hand back a real-but-wrong address.
 *   - FF_IsFieldEmpty reads a 16-byte all-ones slot as empty.
 *
 * Run: ff_test_identity
 */

#include <FF_Primitives.hpp>
#include <FF_Utilities.hpp>

#include "FFHR_tests.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

// Independent little-endian encoders, so the hand-built slots below do not go
// through the code they are meant to exercise.
static void put_u32_le(BYTE* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<BYTE>((v >> (8 * i)) & 0xFF);
}
static void put_u7_le(BYTE* p, uint64_t v) {
    for (int i = 0; i < 7; ++i) p[i] = static_cast<BYTE>((v >> (8 * i)) & 0xFF);
}
static void fill(BYTE* p, BYTE v) { std::memset(p, v, FF_IdSlot::WIDTH); }

// ── The kind and its width ────────────────────────────────────────────────
static void test_kind_is_abi_pinned_and_sixteen_wide() {
    // FF_FIELD_DATETIME is 13 and FF_FIELD_URL is 14 (test_datetime pins the
    // former); a new kind appends, so its value is pinned here the same way.
    CHECK_EQ(static_cast<int>(FF_FIELD_ID), 15, "FF_FIELD_ID is the appended kind");
    CHECK_EQ(static_cast<int>(ff_slot_width(FF_FIELD_ID)), 16,
             "an identity slot is sixteen bytes");
    CHECK_EQ(static_cast<int>(FF_IdSlot::WIDTH), 16, "slot width constant agrees");
}

// ── FF_UUID: construction and equality ────────────────────────────────────
static void test_uuid_hex_construction() {
    const FF_UUID u("59bf0ef4-e89c-4628-9b51-12ae3fdbe22b");
    const BYTE expected[16] = {0x59, 0xbf, 0x0e, 0xf4, 0xe8, 0x9c, 0x46, 0x28,
                               0x9b, 0x51, 0x12, 0xae, 0x3f, 0xdb, 0xe2, 0x2b};
    CHECK(std::memcmp(u.bytes, expected, 16) == 0, "canonical hex decodes to the right bytes");
    CHECK(u == FF_UUID("59bf0ef4-e89c-4628-9b51-12ae3fdbe22b"), "equal spellings compare equal");
    CHECK(u != FF_UUID("00000000-0000-0000-0000-000000000000"), "a different UUID compares unequal");

    // The urn:uuid: prefix is accepted, and yields the same bytes.
    CHECK(FF_UUID("urn:uuid:59bf0ef4-e89c-4628-9b51-12ae3fdbe22b") == u,
          "the urn:uuid: prefix is accepted");

    // The default is all-ones -- the absence convention.
    const FF_UUID absent;
    for (BYTE b : absent.bytes) CHECK(b == 0xFF, "the default UUID is all-ones");
}

static void test_uuid_rejects_non_canonical() {
    const auto rejects = [](std::string_view s) {
        try { FF_UUID bad(s); return false; }
        catch (const std::invalid_argument&) { return true; }
    };
    CHECK(rejects("59BF0EF4-E89C-4628-9B51-12AE3FDBE22B"), "uppercase hex is rejected");
    CHECK(rejects("59bf0ef4e89c46289b5112ae3fdbe22b"), "missing hyphens are rejected");
    CHECK(rejects("59bf0ef4-e89c-4628-9b51-12ae3fdbe22"), "too short is rejected");
    CHECK(rejects("59bf0ef4-e89c-4628-9b51-12ae3fdbe22b0"), "too long is rejected");
    CHECK(rejects("59bf0ef4-e89c-4628-9b51-12ae3fdbe22g"), "a non-hex digit is rejected");
    CHECK(rejects("59bf0ef4+e89c-4628-9b51-12ae3fdbe22b"), "a misplaced separator is rejected");
}

// ── The three ordered tests, on hand-built slots ──────────────────────────
static void test_all_ones_is_absent_first() {
    BYTE slot[16];
    fill(slot, 0xFF);
    CHECK(FF_Id::read_slot(slot).absent(), "an all-ones slot reads ABSENT");

    // The trap the order exists for: all-ones is ALSO a well-formed 16-byte
    // value with version nibble 15. It must not read as a UUID.
    CHECK(FF_Id::read_slot(slot).form() == FF_Id::Form::ABSENT,
          "all-ones never reads as the UUID whose version nibble is 15");
}

static void test_uuid_arm_for_every_version_nibble() {
    // A conformant UUID sets byte 6's high nibble to the version (1..8) and
    // byte 8's top two bits to 10. None may be mistaken for a pointer form.
    for (int version = 1; version <= 8; ++version) {
        BYTE slot[16];
        std::memset(slot, 0x11, sizeof(slot));
        slot[6] = static_cast<BYTE>(version << 4);   // version nibble
        slot[8] = 0xA0;                              // variant 10, and != 0x01
        const FF_Id id = FF_Id::read_slot(slot);
        CHECK(id.form() == FF_Id::Form::UUID, "version " << version << " reads as a UUID");
        CHECK(std::memcmp(id.uuid().bytes, slot, 16) == 0,
              "version " << version << " preserves all sixteen bytes");
    }
}

static void test_pointer_arms_from_hand_built_slots() {
    // GENERATED: kind 1 in bytes 4..7, check byte 0x01, a 7-byte offset.
    {
        BYTE slot[16];
        fill(slot, 0x00);
        put_u32_le(slot + FF_IdSlot::KIND_WORD, FF_IdSlot::KIND_GENERATED);
        slot[FF_IdSlot::CHECK_BYTE] = FF_IdSlot::CHECK_VALUE;
        put_u7_le(slot + FF_IdSlot::OFFSET_BYTES, 0x1234567u);
        const FF_Id id = FF_Id::read_slot(slot);
        CHECK(id.is_generated(), "a GENERATED slot reads GENERATED");
        CHECK_EQ(static_cast<Offset>(id), Offset{0x1234567},
                 "the 7-byte offset round-trips");
    }
    // INTERNED: kind 2, a 4-byte index in bytes 0..3.
    {
        BYTE slot[16];
        fill(slot, 0x00);
        put_u32_le(slot, 0xDEADBEEFu);
        put_u32_le(slot + FF_IdSlot::KIND_WORD, FF_IdSlot::KIND_INTERNED);
        slot[FF_IdSlot::CHECK_BYTE] = FF_IdSlot::CHECK_VALUE;
        const FF_Id id = FF_Id::read_slot(slot);
        CHECK(id.form() == FF_Id::Form::INTERNED, "an INTERNED slot reads INTERNED");
        CHECK_EQ(id.index(), 0xDEADBEEFu, "the trie index round-trips");
    }
    // RAW_STRING: kind 3, a 7-byte offset.
    {
        BYTE slot[16];
        fill(slot, 0x00);
        put_u32_le(slot + FF_IdSlot::KIND_WORD, FF_IdSlot::KIND_RAW_STRING);
        slot[FF_IdSlot::CHECK_BYTE] = FF_IdSlot::CHECK_VALUE;
        put_u7_le(slot + FF_IdSlot::OFFSET_BYTES, 0x40u);
        const FF_Id id = FF_Id::read_slot(slot);
        CHECK(id.form() == FF_Id::Form::RAW_STRING, "a RAW_STRING slot reads RAW_STRING");
        CHECK_EQ(id.raw_string_offset(), Offset{0x40}, "the string offset round-trips");
    }
    // PENDING: kind 0 -- a placeholder that must never survive a seal.
    {
        BYTE slot[16];
        fill(slot, 0x00);
        slot[FF_IdSlot::CHECK_BYTE] = FF_IdSlot::CHECK_VALUE;   // kind word stays 0
        CHECK(FF_Id::read_slot(slot).form() == FF_Id::Form::PENDING,
              "kind 0 reads PENDING");
    }
}

static void test_pointer_test_needs_both_bytes() {
    // Test 2 is an AND: the check byte alone, or a zero version byte alone, is
    // not enough -- otherwise the discrimination would misfire on ordinary bytes.
    {
        BYTE slot[16];
        std::memset(slot, 0x11, sizeof(slot));
        slot[FF_IdSlot::CHECK_BYTE] = FF_IdSlot::CHECK_VALUE;   // check byte set
        slot[6] = 0x30;                                         // but a real version
        CHECK(FF_Id::read_slot(slot).form() == FF_Id::Form::UUID,
              "check byte without a zero version byte is still a UUID");
    }
    {
        BYTE slot[16];
        std::memset(slot, 0x11, sizeof(slot));
        slot[8] = 0x80;                                         // UUID variant
        slot[6] = 0x00;                                         // zero version byte
        CHECK(FF_Id::read_slot(slot).form() == FF_Id::Form::UUID,
              "zero version byte without the check byte is still a UUID");
    }
}

// ── Round-trip through this type's own writer ─────────────────────────────
static void test_roundtrip_all_wire_arms() {
    const auto roundtrip = [](const FF_Id& id) {
        BYTE slot[16];
        id.write_slot(slot);
        return FF_Id::read_slot(slot);
    };

    const FF_Id uuid_id(FF_UUID("59bf0ef4-e89c-4628-9b51-12ae3fdbe22b"));
    CHECK(roundtrip(uuid_id).uuid() == uuid_id.uuid(), "UUID arm round-trips");

    const FF_Id gen = FF_Id::generated(0x00ABCDEF);
    CHECK(roundtrip(gen).is_generated() &&
          static_cast<Offset>(roundtrip(gen)) == 0x00ABCDEF, "GENERATED arm round-trips");

    const FF_Id in = FF_Id::interned(4242);
    CHECK(roundtrip(in).form() == FF_Id::Form::INTERNED &&
          roundtrip(in).index() == 4242, "INTERNED arm round-trips");

    const FF_Id raw = FF_Id::raw_string(0x1000);
    CHECK(roundtrip(raw).form() == FF_Id::Form::RAW_STRING &&
          roundtrip(raw).raw_string_offset() == 0x1000, "RAW_STRING arm round-trips");

    CHECK(roundtrip(FF_Id{}).absent(), "ABSENT arm round-trips");
    CHECK(roundtrip(FF_Id::pending()).form() == FF_Id::Form::PENDING,
          "PENDING arm round-trips");
    CHECK_EQ(static_cast<int>(roundtrip(FF_Id{}).form()), static_cast<int>(FF_Id::Form::ABSENT),
             "an absent slot is all-ones, not a UUID");
}

// ── Conversions ───────────────────────────────────────────────────────────
static void test_offset_conversion_is_partial() {
    const FF_Id gen = FF_Id::generated(0x00AABBCC);
    CHECK(gen.is_generated(), "generated() reports GENERATED");
    CHECK_EQ(static_cast<Offset>(gen), Offset{0x00AABBCC},
             "Offset(id) returns the block offset for a GENERATED id");

    // Every other arm yields FF_NULL_OFFSET. RAW_STRING is the sharp case: its
    // payload IS an offset (of the FF_STRING), and returning it would be a
    // real, in-bounds, self-validating wrong address.
    CHECK_EQ(static_cast<Offset>(FF_Id(FF_UUID("59bf0ef4-e89c-4628-9b51-12ae3fdbe22b"))),
             FF_NULL_OFFSET, "a UUID has no block offset");
    CHECK_EQ(static_cast<Offset>(FF_Id::interned(7)), FF_NULL_OFFSET,
             "an INTERNED index is not an address");
    CHECK_EQ(static_cast<Offset>(FF_Id::raw_string(0x1000)), FF_NULL_OFFSET,
             "RAW_STRING's payload is a STRING offset, not the resource's");
    CHECK_EQ(static_cast<Offset>(FF_Id::pending()), FF_NULL_OFFSET,
             "a PENDING id has no address");
    CHECK_EQ(static_cast<Offset>(FF_Id{}), FF_NULL_OFFSET, "an absent id has no address");

    // FF_NULL_OFFSET can never name a real arena block, so a caller that forgot
    // to test is handed a value that fails the block checks rather than a
    // plausible wrong address.
    CHECK_EQ(FF_NULL_OFFSET, FF_NULL_UINT64, "the sentinel is all-ones");
}

static void test_no_implicit_integer_conversion() {
    // The whole reason the conversion is explicit: Offset is a bare integer, so
    // an implicit FF_Id -> integer would put the id into every overload and
    // arithmetic expression in the tree (the T6 MutableEntry::operator= class).
    static_assert(!std::is_convertible_v<FF_Id, Offset>,      "FF_Id must not implicitly become Offset");
    static_assert(!std::is_convertible_v<FF_Id, uint64_t>,    "FF_Id must not implicitly become uint64_t");
    static_assert(!std::is_convertible_v<FF_Id, int>,         "FF_Id must not implicitly become int");
    static_assert(std::is_constructible_v<Offset, FF_Id>,     "the explicit cast must still work");
    CHECK(true, "FF_Id does not implicitly convert to an integer");
}

// ── Accessors and the encoder refuse the wrong arm ────────────────────────
static void test_wrong_arm_access_throws() {
    const auto throws = [](auto&& fn) {
        try { fn(); return false; }
        catch (const std::runtime_error&) { return true; }
    };
    CHECK(throws([] { (void)FF_Id::generated(1).uuid(); }), "uuid() on a GENERATED id throws");
    CHECK(throws([] { (void)FF_Id::generated(1).index(); }), "index() on a GENERATED id throws");
    CHECK(throws([] { (void)FF_Id::generated(1).raw_string_offset(); }),
          "raw_string_offset() on a GENERATED id throws");
    CHECK(throws([] { (void)FF_Id{}.uuid(); }), "uuid() on an absent id throws");
}

// ── Absence at the field level ────────────────────────────────────────────
static void test_field_empty_reads_sixteen_bytes() {
    BYTE slot[16];
    fill(slot, 0xFF);
    CHECK(FF_IsFieldEmpty(slot, 0, FF_FIELD_ID), "an all-ones identity slot is empty");

    slot[15] = 0x00;
    CHECK(!FF_IsFieldEmpty(slot, 0, FF_FIELD_ID),
          "one non-ones byte makes the slot non-empty");
}

int main() {
    TEST_GROUP("IdentitySlot");
    ff_test::run("test_kind_is_abi_pinned_and_sixteen_wide", test_kind_is_abi_pinned_and_sixteen_wide);
    ff_test::run("test_uuid_hex_construction", test_uuid_hex_construction);
    ff_test::run("test_uuid_rejects_non_canonical", test_uuid_rejects_non_canonical);
    ff_test::run("test_all_ones_is_absent_first", test_all_ones_is_absent_first);
    ff_test::run("test_uuid_arm_for_every_version_nibble", test_uuid_arm_for_every_version_nibble);
    ff_test::run("test_pointer_arms_from_hand_built_slots", test_pointer_arms_from_hand_built_slots);
    ff_test::run("test_pointer_test_needs_both_bytes", test_pointer_test_needs_both_bytes);
    ff_test::run("test_roundtrip_all_wire_arms", test_roundtrip_all_wire_arms);
    ff_test::run("test_offset_conversion_is_partial", test_offset_conversion_is_partial);
    ff_test::run("test_no_implicit_integer_conversion", test_no_implicit_integer_conversion);
    ff_test::run("test_wrong_arm_access_throws", test_wrong_arm_access_throws);
    ff_test::run("test_field_empty_reads_sixteen_bytes", test_field_empty_reads_sixteen_bytes);

    return ff_test::report("all identity-slot checks pass");
}
