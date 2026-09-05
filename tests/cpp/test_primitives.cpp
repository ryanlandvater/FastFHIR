/**
 * @file test_primitives.cpp
 * @brief Unit tests for FF_Primitives.hpp — core types, constants, FF_HEADER,
 *        RECOVERY_TAG, FF_FieldKey, FF_ARRAY, FF_STRING, FF_CHECKSUM.
 *
 * Run: ff_test_primitives --filter <name>
 * Exit code: 0 = all pass, non-zero = failures.
 */

#include <FF_Primitives.hpp>
#include <FF_Ops.hpp>
#include <FF_Utilities.hpp>  // FF_IsResourceTag / FF_IsBackboneTag / FF_IsScalarBlockTag
#include "FF_ResourceTypes.hpp"

#include "FFHR_tests.hpp"
#include <cstring>

using namespace FastFHIR;
#include <cstdio>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef FF_TEST_ARTIFACT_DIR
#define FF_TEST_ARTIFACT_DIR "."
#endif

// ── Test framework (header-only, no deps) ──────────────────────────────────


// ── Helpers ────────────────────────────────────────────────────────────────

// Engine version encoded into every test header.  Defined as a macro so it
// mirrors the real compile-time FASTFHIR_VERSION_* macros from FF_Version.hpp.
// Version 2026.1 = (2026 << 16) | 1
#define TEST_ENGINE_VERSION ((2026 & 0x3FFF) << 16 | (1 & 0xFFFF))

/// Create a minimal buffer large enough for a FF_HEADER.
static std::vector<uint8_t> make_header_buffer(uint16_t fhir_rev = FHIR_VERSION_R5,
                                               Offset root_off = FF_NULL_OFFSET,
                                               RECOVERY_TAG root_rec = FF_RECOVER_UNDEFINED)
{
    std::vector<uint8_t> buf(FF_HEADER::HEADER_SIZE + 64, 0);
    STORE_FF_HEADER(buf.data(), fhir_rev, buf.size(), root_off, root_rec, FF_NULL_OFFSET);
    // Override the VERSION slot with the test's engine version so tests are
    // deterministic regardless of which compile-time FASTFHIR_VERSION_* is set.
    STORE_U32(buf.data() + FF_HEADER::VERSION,
              FF_ENCODE_HEADER_VERSION(TEST_ENGINE_VERSION, FF_STREAM_COMPACTION_NONE));
    return buf;
}

// =====================================================================
// FF_HEADER tests
// =====================================================================
static void test_header_validate()
{
    auto buf = make_header_buffer();
    printf("  buf size=%zu\n", buf.size());
    const FF_HEADER *hdr = reinterpret_cast<const FF_HEADER *>(buf.data());
    printf("  hdr=%p MAGIC=%08x\n", (void *)hdr, LOAD_U32(buf.data() + FF_HEADER::MAGIC));
    FF_Result r = hdr->validate_full(buf.data());
    printf("  validate_full returned code=%u\n", (unsigned)r.code);
    CHECK(r, "valid header should pass");
}

static void test_header_fhir_rev()
{
    {
        auto buf = make_header_buffer(FHIR_VERSION_R4);
        const FF_HEADER *hdr = reinterpret_cast<const FF_HEADER *>(buf.data());
        CHECK_EQ(hdr->get_fhir_rev(buf.data()), FHIR_VERSION_R4, "R4 revision");
    }
    {
        auto buf = make_header_buffer(FHIR_VERSION_R5);
        const FF_HEADER *hdr = reinterpret_cast<const FF_HEADER *>(buf.data());
        CHECK_EQ(hdr->get_fhir_rev(buf.data()), FHIR_VERSION_R5, "R5 revision");
    }
}

static void test_header_engine_version()
{
    auto buf = make_header_buffer();
    const FF_HEADER *hdr = reinterpret_cast<const FF_HEADER *>(buf.data());
    CHECK_EQ(hdr->get_engine_version(buf.data()), TEST_ENGINE_VERSION, "engine version");
}

static void test_header_root()
{
    Offset root_off = 128;
    auto buf = make_header_buffer(FHIR_VERSION_R5, root_off, RECOVER_FF_PATIENT);
    const FF_HEADER *hdr = reinterpret_cast<const FF_HEADER *>(buf.data());
    CHECK_EQ(hdr->get_root(buf.data()), root_off, "root offset");
    CHECK_EQ(hdr->get_root_type(buf.data()), RECOVER_FF_PATIENT, "root recovery");
}

static void test_header_checksum()
{
    // Minimal checksum block
    std::vector<uint8_t> buf(static_cast<size_t>(FF_HEADER::HEADER_SIZE) + static_cast<size_t>(FF_CHECKSUM::HEADER_SIZE) + 64, 0);
    Offset cs_off = FF_HEADER::HEADER_SIZE;
    STORE_FF_HEADER(buf.data(), FHIR_VERSION_R5, buf.size(), FF_NULL_OFFSET,
                    FF_RECOVER_UNDEFINED, cs_off);
    // Write a minimal checksum footer
    STORE_U64(buf.data() + cs_off + FF_CHECKSUM::VALIDATION, cs_off);
    STORE_U16(buf.data() + cs_off + FF_CHECKSUM::RECOVERY, RECOVER_FF_CHECKSUM);
    STORE_U16(buf.data() + cs_off + FF_CHECKSUM::ALGORITHM, FF_CHECKSUM_SHA256);
    const char *kExpectedHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    std::memcpy(buf.data() + cs_off + FF_CHECKSUM::HASH_DATA, kExpectedHash, 32);

    const FF_HEADER *hdr = reinterpret_cast<const FF_HEADER *>(buf.data());
    auto cs = hdr->get_checksum(buf.data());
    CHECK_EQ(cs.get_algorithm(buf.data()), FF_CHECKSUM_SHA256, "checksum algorithm");
    auto hash = cs.get_hash_view(buf.data());
    CHECK_EQ(hash.size(), size_t(32), "checksum hash length");
    CHECK(std::memcmp(hash.data(), kExpectedHash, 32) == 0, "checksum hash data");
}

// =====================================================================
// RECOVERY_TAG tests
// =====================================================================
static void test_recovery_array_tag()
{
    CHECK(IsArrayTag(static_cast<RECOVERY_TAG>(0x8000)), "0x8000 is array");
    CHECK(IsArrayTag(static_cast<RECOVERY_TAG>(0x8301)), "0x8301 is array");
    CHECK(!IsArrayTag(static_cast<RECOVERY_TAG>(0x0301)), "0x0301 is not array");
    CHECK(!IsArrayTag(static_cast<RECOVERY_TAG>(0x0000)), "0x0000 is not array");
    CHECK(!IsArrayTag(RECOVER_FF_PATIENT), "Patient tag is not array");
}

static void test_recovery_type_mask()
{
    CHECK_EQ(GetTypeFromTag(static_cast<RECOVERY_TAG>(0x8301)),
             static_cast<RECOVERY_TAG>(0x0301), "mask removes array bit");
    CHECK_EQ(GetTypeFromTag(static_cast<RECOVERY_TAG>(0xFFFF)),
             static_cast<RECOVERY_TAG>(0x7FFF), "mask upper bound");
    CHECK_EQ(GetTypeFromTag(static_cast<RECOVERY_TAG>(0x0000)),
             static_cast<RECOVERY_TAG>(0x0000), "mask zero");
}

static void test_recovery_to_array()
{
    CHECK_EQ(ToArrayTag(RECOVER_FF_PATIENT),
             static_cast<RECOVERY_TAG>(RECOVER_FF_PATIENT | 0x8000),
             "patient to array");
    CHECK_EQ(ToArrayTag(RECOVER_FF_STRING),
             static_cast<RECOVERY_TAG>(RECOVER_FF_STRING | 0x8000),
             "string to array");
}

static void test_recovery_known_tags()
{
    CHECK_EQ(RECOVER_FF_HEADER, 0x0001, "FF_HEADER");
    CHECK_EQ(RECOVER_FF_STRING, 0x0002, "FF_STRING");
    // FF_CODE moved 0x0003 -> 0x010B on 2026-08-19: it is an inline scalar, and
    // the primitive band put it outside the band test that routes inline
    // scalars, leaving two branches written for it unreachable. Pre-release,
    // deliberate — see _retired in dictionaries/master_tags.json. 0x0003 is
    // burned and must never be reused.
    CHECK_EQ(RECOVER_FF_CODE, 0x010B, "FF_CODE");
    CHECK_EQ(FF_IsScalarBlockTag(RECOVER_FF_CODE), true, "FF_CODE is a scalar");
    CHECK_EQ(RECOVER_FF_RESOURCE, 0x0003, "FF_RESOURCE");
    CHECK_EQ(RECOVER_FF_CHECKSUM, 0x0004, "FF_CHECKSUM");
    CHECK_EQ(RECOVER_FF_BOOL, 0x0101, "BOOL");
    CHECK_EQ(RECOVER_FF_INT32, 0x0102, "INT32");
    CHECK_EQ(RECOVER_FF_UINT32, 0x0103, "UINT32");
    CHECK_EQ(RECOVER_FF_FLOAT64, 0x0106, "FLOAT64");
    CHECK_EQ(RECOVER_FF_EXTENSION, 0x0201, "EXTENSION");
    // Resources moved 0x0300 -> 0x1000 in the 2026-08-14 band re-cut: 256 slots
    // could not hold FHIR's 178 resource types, and sub-elements needed 711
    // against the same 256. Pre-release, one-time, deliberate — see the BAND MAP
    // in FF_RecoveryTags.hpp. These values are permanent from here.
    CHECK_EQ(RECOVER_FF_PATIENT, 0x1014, "PATIENT");
    CHECK_EQ(RECOVER_FF_OBSERVATION, 0x1012, "OBSERVATION");
    CHECK_EQ(RECOVER_FF_BUNDLE, 0x1002, "BUNDLE");
    CHECK_EQ(RECOVER_FF_BUNDLE_ENTRY, 0x2005, "BUNDLE_ENTRY");
}

// Band classification must survive the re-cut: FF_IsResourceTag was a high-byte
// test (`& 0xFF00 == 0x0300`) and would now return false for every resource,
// since the band spans 0x1000-0x1FFF across 16 high-byte values.
static void test_recovery_band_classification()
{
    CHECK_EQ(FF_IsResourceTag(RECOVER_FF_PATIENT), true, "PATIENT is a resource");
    CHECK_EQ(FF_IsResourceTag(RECOVER_FF_BUNDLE), true, "BUNDLE is a resource");
    CHECK_EQ(FF_IsResourceTag(ToArrayTag(RECOVER_FF_PATIENT)), true, "array-of-PATIENT");
    CHECK_EQ(FF_IsResourceTag(RECOVER_FF_PERIOD), false, "PERIOD is a datatype");
    CHECK_EQ(FF_IsResourceTag(RECOVER_FF_BUNDLE_ENTRY), false, "BUNDLE_ENTRY is backbone");
    CHECK_EQ(FF_IsBackboneTag(RECOVER_FF_BUNDLE_ENTRY), true, "BUNDLE_ENTRY is backbone");
    CHECK_EQ(FF_IsBackboneTag(RECOVER_FF_PATIENT), false, "PATIENT is not backbone");
    CHECK_EQ(FF_IsScalarBlockTag(RECOVER_FF_BOOL), true, "BOOL is a scalar");
    CHECK_EQ(FF_IsScalarBlockTag(RECOVER_FF_PATIENT), false, "PATIENT is not a scalar");
    // Band edges: the first and last representable slot of the resource band.
    CHECK_EQ(FF_IsResourceTag(static_cast<RECOVERY_TAG>(RECOVER_BAND_RESOURCE_FIRST)), true, "band first");
    CHECK_EQ(FF_IsResourceTag(static_cast<RECOVERY_TAG>(RECOVER_BAND_RESOURCE_LAST)), true, "band last");
    CHECK_EQ(FF_IsResourceTag(static_cast<RECOVERY_TAG>(RECOVER_BAND_RESOURCE_LAST + 1)), false, "past band end");
    CHECK_EQ(FF_IsResourceTag(static_cast<RECOVERY_TAG>(RECOVER_BAND_RESOURCE_FIRST - 1)), false, "before band start");
}

// =====================================================================
// FF_FieldKey tests
// =====================================================================
static void test_field_key_construction()
{
    FF_FieldKey key(RECOVER_FF_PATIENT, FF_FIELD_BLOCK, 10,
                    RECOVER_FF_STRING, false, "id");
    CHECK_EQ(key.owner_recovery, RECOVER_FF_PATIENT, "owner");
    CHECK_EQ(key.kind, FF_FIELD_BLOCK, "kind");
    CHECK_EQ(key.field_offset, uint32_t(10), "field offset");
    CHECK_EQ(key.child_recovery, RECOVER_FF_STRING, "child");
}

static void test_field_key_scalar()
{
    FF_FieldKey key(RECOVER_FF_PATIENT, FF_FIELD_BOOL, 14,
                    RECOVER_FF_BOOL, false, "active");
    CHECK_EQ(key.kind, FF_FIELD_BOOL, "scalar kind");
    CHECK_EQ(key.child_recovery, RECOVER_FF_BOOL, "scalar child");
}

static void test_field_key_array_offsets()
{
    FF_FieldKey key(RECOVER_FF_PATIENT, FF_FIELD_BLOCK, 30,
                    RECOVER_FF_STRING, true, "name");
    CHECK(key.array_entries_are_offsets, "array entries are offsets");
}

// =====================================================================
// FF_ARRAY tests
// =====================================================================
static void test_array_header_rw()
{
    // 3 entries of stride 8 need 24 payload bytes; this allocated 16 and
    // stamped a count of 3 into it. Harmless only while nothing compared the
    // count against the space -- entry_count() now does, and clamped to 2.
    std::vector<uint8_t> buf(FF_ARRAY::HEADER_SIZE + 3 * 8, 0);
    Offset write_off = 0;
    STORE_FF_ARRAY_HEADER(buf.data(), write_off, FF_ARRAY::OFFSET, 8, 3,
                          ToArrayTag(RECOVER_FF_STRING));
    CHECK_EQ(write_off, FF_ARRAY::HEADER_SIZE, "write head advanced");
    // Read back via instance
    // Buffer extent, not header size -- same mislabel as the string test below.
    FF_ARRAY arr(0, static_cast<Size>(buf.size()), FHIR_VERSION_R5);
    CHECK_EQ(arr.entry_count(buf.data()), uint32_t(3), "element count");
    CHECK_EQ(arr.entry_step(buf.data()), uint16_t(8), "stride");
    auto arr_tag = static_cast<RECOVERY_TAG>(LOAD_U16(buf.data() + FF_ARRAY::RECOVERY));
    CHECK_EQ(GetTypeFromTag(arr_tag), RECOVER_FF_STRING, "entry recovery tag");
}

// =====================================================================
// FF_STRING tests
// =====================================================================
static void test_string_block_rw()
{
    std::vector<uint8_t> buf(256, 0);
    const char *kTestStr = "Hello, FastFHIR!";
    Offset write_off = 0;
    STORE_FF_STRING(buf.data(), write_off, kTestStr);
    // Read back via instance
    // The second argument is the BUFFER extent, not the block's header size.
    // This passed HEADER_SIZE, which reads as "the arena is 14 bytes long" and
    // is only harmless while nothing bounds-checks against it. It does now, so
    // the payload was clamped to zero and the mistake surfaced.
    FF_STRING str(0, static_cast<Size>(buf.size()), FHIR_VERSION_R5);
    std::string_view read = str.read_view(buf.data());
    CHECK_EQ(read, std::string_view(kTestStr), "string content");
}

static void test_string_block_empty()
{
    std::vector<uint8_t> buf(64, 0);
    Offset write_off = 0;
    STORE_FF_STRING(buf.data(), write_off, "");
    FF_STRING str(0, FF_STRING::HEADER_SIZE, FHIR_VERSION_R5);
    std::string_view read = str.read_view(buf.data());
    CHECK(read.empty(), "empty string read back");
}

// =====================================================================
// FF_CHECKSUM tests
// =====================================================================
static void test_checksum_null_sentinel()
{
    CHECK_EQ(FF_CODE_NULL, 0xFFFFFFFFu, "FF_CODE_NULL is 0xFFFFFFFF");
    CHECK_EQ(FF_NULL_UINT32, 0xFFFFFFFFu, "FF_NULL_UINT32 is 0xFFFFFFFF");
}

// =====================================================================
// ResourceType tests
// =====================================================================
static void test_resource_type_from_recovery()
{
    CHECK(resource_type_from_recovery(RECOVER_FF_PATIENT) == RESOURCETYPE::PATIENT, "patient");
    CHECK(resource_type_from_recovery(RECOVER_FF_BUNDLE) == RESOURCETYPE::BUNDLE, "bundle");
    CHECK(resource_type_from_recovery(RECOVER_FF_OBSERVATION) == RESOURCETYPE::OBSERVATION, "observation");
    CHECK(resource_type_from_recovery(FF_RECOVER_UNDEFINED) == RESOURCETYPE::UNKNOWN, "undefined");
}

static void test_resource_type_traits()
{
    CHECK_EQ(ResourceTypeTraits<RESOURCETYPE::PATIENT>::recovery, RECOVER_FF_PATIENT, "patient trait");
    CHECK_EQ(ResourceTypeTraits<RESOURCETYPE::BUNDLE>::recovery, RECOVER_FF_BUNDLE, "bundle trait");
    CHECK_EQ(ResourceTypeTraits<RESOURCETYPE::UNKNOWN>::recovery, FF_RECOVER_UNDEFINED, "unknown trait");
}

// =====================================================================
// Byte-ordering helpers
// =====================================================================
static void test_load_store()
{
    uint8_t buf[8] = {};
    STORE_U16(buf, 0xAABB);
    CHECK_EQ(LOAD_U16(buf), uint16_t(0xAABB), "U16");

    STORE_U32(buf, 0x12345678);
    CHECK_EQ(LOAD_U32(buf), 0x12345678u, "U32");

    STORE_U64(buf, 0xDEADBEEFCAFEBABEull);
    CHECK_EQ(LOAD_U64(buf), 0xDEADBEEFCAFEBABEull, "U64");
}

// =====================================================================
// DATA_BLOCK validate_offset
// =====================================================================
static void test_validate_offset()
{
    // FF_HEADER uses MAGIC bytes at offset 0, not a self-pointer,
    // so validate_offset is expected to fail here.  This test confirms
    // it returns a validation failure rather than crashing.
    auto buf = make_header_buffer();
    DATA_BLOCK blk(0, FF_HEADER::HEADER_SIZE, FHIR_VERSION_R5);
    FF_Result r = blk.validate_offset(buf.data(), "test", RECOVER_FF_HEADER);
    // FF_HEADER stores MAGIC (0x52484646) at offset 0, not __offset,
    // so this should return a validation failure.
    CHECK(r.code == FF_VALIDATION_FAILURE, "validate_offset fails on FF_HEADER (expected)");
}

// =====================================================================
// Main dispatcher
// =====================================================================
int main(int argc, char **argv)
{
    ff_test::set_filter(argc, argv);


    printf("DEBUG: main starting\n");
    TEST_GROUP("FF_HEADER");
    printf("DEBUG: calling validate\n");
    ff_test::run("test_header_validate", test_header_validate);
    printf("DEBUG: calling fhir_rev\n");
    ff_test::run("test_header_fhir_rev", test_header_fhir_rev);
    printf("DEBUG: calling engine_version\n");
    ff_test::run("test_header_engine_version", test_header_engine_version);
    printf("DEBUG: calling root\n");
    ff_test::run("test_header_root", test_header_root);
    printf("DEBUG: calling checksum\n");
    ff_test::run("test_header_checksum", test_header_checksum);

    TEST_GROUP("RECOVERY_TAG");
    ff_test::run("test_recovery_array_tag", test_recovery_array_tag);
    ff_test::run("test_recovery_type_mask", test_recovery_type_mask);
    ff_test::run("test_recovery_to_array", test_recovery_to_array);
    ff_test::run("test_recovery_known_tags", test_recovery_known_tags);
    ff_test::run("test_recovery_band_classification", test_recovery_band_classification);

    TEST_GROUP("FF_FieldKey");
    ff_test::run("test_field_key_construction", test_field_key_construction);
    ff_test::run("test_field_key_scalar", test_field_key_scalar);
    ff_test::run("test_field_key_array_offsets", test_field_key_array_offsets);

    TEST_GROUP("FF_ARRAY");
    ff_test::run("test_array_header_rw", test_array_header_rw);

    TEST_GROUP("FF_STRING");
    ff_test::run("test_string_block_rw", test_string_block_rw);
    ff_test::run("test_string_block_empty", test_string_block_empty);

    TEST_GROUP("FF_CHECKSUM");
    ff_test::run("test_checksum_null_sentinel", test_checksum_null_sentinel);

    TEST_GROUP("ResourceType");
    ff_test::run("test_resource_type_from_recovery", test_resource_type_from_recovery);
    ff_test::run("test_resource_type_traits", test_resource_type_traits);

    TEST_GROUP("ByteOps");
    ff_test::run("test_load_store", test_load_store);

    TEST_GROUP("DATA_BLOCK");
    ff_test::run("test_validate_offset", test_validate_offset);

    std::cout << "\n────────────────────────────────────────────────\n"
              << ::ff_test::g_checks << " tests, " << ::ff_test::g_failures << " failures\n";
    return ::ff_test::g_failures > 0 ? 1 : 0;
}
