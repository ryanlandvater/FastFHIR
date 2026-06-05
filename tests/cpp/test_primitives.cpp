/**
 * @file test_primitives.cpp
 * @brief Unit tests for FF_Primitives.hpp — core types, constants, FF_HEADER,
 *        RECOVERY_TAG, FF_FieldKey, FF_ARRAY, FF_STRING, FF_CHECKSUM.
 *
 * Run: ff_test_primitives --filter <name>
 * Exit code: 0 = all pass, non-zero = failures.
 */

#include <FF_Primitives.hpp>
#include <FF_Recovery.hpp>
#include <FF_Ops.hpp>
#include "../generated_src/FF_ResourceTypes.hpp"
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

static int g_failures = 0;
static int g_tests = 0;
static const char *g_current_group = "";

#define TEST_GROUP(name)                     \
    do                                       \
    {                                        \
        g_current_group = name;              \
        std::cout << "\n[" << name << "]\n"; \
    } while (0)
#define TEST(name) \
    do             \
    {              \
        ++g_tests; \
    } while (0)
#define CHECK(cond, msg)                                                                                                  \
    do                                                                                                                    \
    {                                                                                                                     \
        if (!(cond))                                                                                                      \
        {                                                                                                                 \
            ++g_failures;                                                                                                 \
            std::cerr << "  FAIL " << g_current_group << "::" << __func__ << " line " << __LINE__ << ": " << msg << "\n"; \
        }                                                                                                                 \
    } while (0)
#define CHECK_EQ(a, b, msg) CHECK((a) == (b), msg << " expected " << (b) << " got " << (a))
#define CHECK_NE(a, b, msg) CHECK((a) != (b), msg)

// ── Helpers ────────────────────────────────────────────────────────────────

// Engine version encoded into every test header.  Defined as a macro so it
// mirrors the real compile-time FASTFHIR_VERSION_* macros from FF_Version.hpp.
// Version 2026.1 = (2026 << 16) | 1
#define TEST_ENGINE_VERSION  ((2026 & 0x3FFF) << 16 | (1 & 0xFFFF))

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
              FF_ENCODE_HEADER_VERSION(TEST_ENGINE_VERSION, FF_STREAM_LAYOUT_STANDARD));
    return buf;
}

// =====================================================================
// FF_HEADER tests
// =====================================================================
static void test_header_validate()
{
    TEST("validate_full on a valid header");
    auto buf = make_header_buffer();
    printf("  buf size=%zu\n", buf.size());
    const FF_HEADER *hdr = reinterpret_cast<const FF_HEADER *>(buf.data());
    printf("  hdr=%p MAGIC=%08x\n", (void*)hdr, LOAD_U32(buf.data() + FF_HEADER::MAGIC));
    FF_Result r = hdr->validate_full(buf.data());
    printf("  validate_full returned code=%u\n", (unsigned)r.code);
    CHECK(r, "valid header should pass");
}

static void test_header_fhir_rev()
{
    TEST("FHIR_REV round-trip");
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
    TEST("engine version encode/decode");
    auto buf = make_header_buffer();
    const FF_HEADER *hdr = reinterpret_cast<const FF_HEADER *>(buf.data());
    CHECK_EQ(hdr->get_engine_version(buf.data()), TEST_ENGINE_VERSION, "engine version");
}

static void test_header_root()
{
    TEST("root offset and recovery round-trip");
    Offset root_off = 128;
    auto buf = make_header_buffer(FHIR_VERSION_R5, root_off, RECOVER_FF_PATIENT);
    const FF_HEADER *hdr = reinterpret_cast<const FF_HEADER *>(buf.data());
    CHECK_EQ(hdr->get_root(buf.data()), root_off, "root offset");
    CHECK_EQ(hdr->get_root_type(buf.data()), RECOVER_FF_PATIENT, "root recovery");
}

static void test_header_checksum()
{
    TEST("checksum footer round-trip");
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
    TEST("IsArrayTag");
    CHECK(IsArrayTag(static_cast<RECOVERY_TAG>(0x8000)), "0x8000 is array");
    CHECK(IsArrayTag(static_cast<RECOVERY_TAG>(0x8301)), "0x8301 is array");
    CHECK(!IsArrayTag(static_cast<RECOVERY_TAG>(0x0301)), "0x0301 is not array");
    CHECK(!IsArrayTag(static_cast<RECOVERY_TAG>(0x0000)), "0x0000 is not array");
    CHECK(!IsArrayTag(RECOVER_FF_PATIENT), "Patient tag is not array");
}

static void test_recovery_type_mask()
{
    TEST("GetTypeFromTag");
    CHECK_EQ(GetTypeFromTag(static_cast<RECOVERY_TAG>(0x8301)),
             static_cast<RECOVERY_TAG>(0x0301), "mask removes array bit");
    CHECK_EQ(GetTypeFromTag(static_cast<RECOVERY_TAG>(0xFFFF)),
             static_cast<RECOVERY_TAG>(0x7FFF), "mask upper bound");
    CHECK_EQ(GetTypeFromTag(static_cast<RECOVERY_TAG>(0x0000)),
             static_cast<RECOVERY_TAG>(0x0000), "mask zero");
}

static void test_recovery_to_array()
{
    TEST("ToArrayTag");
    CHECK_EQ(ToArrayTag(RECOVER_FF_PATIENT),
             static_cast<RECOVERY_TAG>(RECOVER_FF_PATIENT | 0x8000),
             "patient to array");
    CHECK_EQ(ToArrayTag(RECOVER_FF_STRING),
             static_cast<RECOVERY_TAG>(RECOVER_FF_STRING | 0x8000),
             "string to array");
}

static void test_recovery_known_tags()
{
    TEST("known RECOVERY_TAG values are correct");
    CHECK_EQ(RECOVER_FF_HEADER, 0x0001, "FF_HEADER");
    CHECK_EQ(RECOVER_FF_STRING, 0x0002, "FF_STRING");
    CHECK_EQ(RECOVER_FF_CODE, 0x0003, "FF_CODE");
    CHECK_EQ(RECOVER_FF_RESOURCE, 0x0004, "FF_RESOURCE");
    CHECK_EQ(RECOVER_FF_CHECKSUM, 0x0005, "FF_CHECKSUM");
    CHECK_EQ(RECOVER_FF_BOOL, 0x0101, "BOOL");
    CHECK_EQ(RECOVER_FF_INT32, 0x0102, "INT32");
    CHECK_EQ(RECOVER_FF_UINT32, 0x0103, "UINT32");
    CHECK_EQ(RECOVER_FF_FLOAT64, 0x0106, "FLOAT64");
    CHECK_EQ(RECOVER_FF_EXTENSION, 0x0201, "EXTENSION");
    CHECK_EQ(RECOVER_FF_PATIENT, 0x0314, "PATIENT");
    CHECK_EQ(RECOVER_FF_OBSERVATION, 0x0312, "OBSERVATION");
    CHECK_EQ(RECOVER_FF_BUNDLE, 0x0302, "BUNDLE");
}

// =====================================================================
// FF_FieldKey tests
// =====================================================================
static void test_field_key_construction()
{
    TEST("FF_FieldKey construction");
    FF_FieldKey key(RECOVER_FF_PATIENT, FF_FIELD_BLOCK, 10,
                    RECOVER_FF_STRING, false, "id");
    CHECK_EQ(key.owner_recovery, RECOVER_FF_PATIENT, "owner");
    CHECK_EQ(key.kind, FF_FIELD_BLOCK, "kind");
    CHECK_EQ(key.field_offset, uint32_t(10), "field offset");
    CHECK_EQ(key.child_recovery, RECOVER_FF_STRING, "child");
}

static void test_field_key_scalar()
{
    TEST("FF_FieldKey scalar field");
    FF_FieldKey key(RECOVER_FF_PATIENT, FF_FIELD_BOOL, 14,
                    RECOVER_FF_BOOL, false, "active");
    CHECK_EQ(key.kind, FF_FIELD_BOOL, "scalar kind");
    CHECK_EQ(key.child_recovery, RECOVER_FF_BOOL, "scalar child");
}

static void test_field_key_array_offsets()
{
    TEST("FF_FieldKey array-of-offsets flag");
    FF_FieldKey key(RECOVER_FF_PATIENT, FF_FIELD_BLOCK, 30,
                    RECOVER_FF_STRING, true, "name");
    CHECK(key.array_entries_are_offsets, "array entries are offsets");
}

// =====================================================================
// FF_ARRAY tests
// =====================================================================
static void test_array_header_rw()
{
    TEST("FF_ARRAY header read/write");
    std::vector<uint8_t> buf(FF_ARRAY::HEADER_SIZE + 16, 0);
    Offset write_off = 0;
    STORE_FF_ARRAY_HEADER(buf.data(), write_off, FF_ARRAY::OFFSET, 8, 3,
                          ToArrayTag(RECOVER_FF_STRING));
    CHECK_EQ(write_off, FF_ARRAY::HEADER_SIZE, "write head advanced");
    // Read back via instance
    FF_ARRAY arr(0, FF_ARRAY::HEADER_SIZE, FHIR_VERSION_R5);
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
    TEST("FF_STRING block read/write");
    std::vector<uint8_t> buf(256, 0);
    const char *kTestStr = "Hello, FastFHIR!";
    Offset write_off = 0;
    STORE_FF_STRING(buf.data(), write_off, kTestStr);
    // Read back via instance
    FF_STRING str(0, FF_STRING::HEADER_SIZE, FHIR_VERSION_R5);
    std::string_view read = str.read_view(buf.data());
    CHECK_EQ(read, std::string_view(kTestStr), "string content");
}

static void test_string_block_empty()
{
    TEST("FF_STRING block empty string");
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
    TEST("FF_CODE_NULL sentinel");
    CHECK_EQ(FF_CODE_NULL, 0xFFFFFFFFu, "FF_CODE_NULL is 0xFFFFFFFF");
    CHECK_EQ(FF_NULL_UINT32, 0xFFFFFFFFu, "FF_NULL_UINT32 is 0xFFFFFFFF");
}

// =====================================================================
// ResourceType tests
// =====================================================================
static void test_resource_type_from_recovery()
{
    TEST("resource_type_from_recovery maps correctly");
    CHECK(resource_type_from_recovery(RECOVER_FF_PATIENT) == RESOURCETYPE::PATIENT, "patient");
    CHECK(resource_type_from_recovery(RECOVER_FF_BUNDLE) == RESOURCETYPE::BUNDLE, "bundle");
    CHECK(resource_type_from_recovery(RECOVER_FF_OBSERVATION) == RESOURCETYPE::OBSERVATION, "observation");
    CHECK(resource_type_from_recovery(FF_RECOVER_UNDEFINED) == RESOURCETYPE::UNKNOWN, "undefined");
}

static void test_resource_type_traits()
{
    TEST("ResourceTypeTraits::recovery matches");
    CHECK_EQ(ResourceTypeTraits<RESOURCETYPE::PATIENT>::recovery, RECOVER_FF_PATIENT, "patient trait");
    CHECK_EQ(ResourceTypeTraits<RESOURCETYPE::BUNDLE>::recovery, RECOVER_FF_BUNDLE, "bundle trait");
    CHECK_EQ(ResourceTypeTraits<RESOURCETYPE::UNKNOWN>::recovery, FF_RECOVER_UNDEFINED, "unknown trait");
}

// =====================================================================
// Byte-ordering helpers
// =====================================================================
static void test_load_store()
{
    TEST("LOAD_U16 / STORE_U16 round-trip");
    uint8_t buf[8] = {};
    STORE_U16(buf, 0xAABB);
    CHECK_EQ(LOAD_U16(buf), uint16_t(0xAABB), "U16");

    TEST("LOAD_U32 / STORE_U32 round-trip");
    STORE_U32(buf, 0x12345678);
    CHECK_EQ(LOAD_U32(buf), 0x12345678u, "U32");

    TEST("LOAD_U64 / STORE_U64 round-trip");
    STORE_U64(buf, 0xDEADBEEFCAFEBABEull);
    CHECK_EQ(LOAD_U64(buf), 0xDEADBEEFCAFEBABEull, "U64");
}

// =====================================================================
// DATA_BLOCK validate_offset
// =====================================================================
static void test_validate_offset()
{
    TEST("DATA_BLOCK::validate_offset");
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
    const char *filter = (argc > 2 && strcmp(argv[1], "--filter") == 0) ? argv[2] : "";

    auto run = [&](const char *name, auto fn)
    {
        if (filter[0] == '\0' || strstr(name, filter))
        {
            fn();
        }
    };

    printf("DEBUG: main starting\n");
    TEST_GROUP("FF_HEADER");
    printf("DEBUG: calling validate\n");
    run("test_header_validate", test_header_validate);
    printf("DEBUG: calling fhir_rev\n");
    run("test_header_fhir_rev", test_header_fhir_rev);
    printf("DEBUG: calling engine_version\n");
    run("test_header_engine_version", test_header_engine_version);
    printf("DEBUG: calling root\n");
    run("test_header_root", test_header_root);
    printf("DEBUG: calling checksum\n");
    run("test_header_checksum", test_header_checksum);

    TEST_GROUP("RECOVERY_TAG");
    run("test_recovery_array_tag", test_recovery_array_tag);
    run("test_recovery_type_mask", test_recovery_type_mask);
    run("test_recovery_to_array", test_recovery_to_array);
    run("test_recovery_known_tags", test_recovery_known_tags);

    TEST_GROUP("FF_FieldKey");
    run("test_field_key_construction", test_field_key_construction);
    run("test_field_key_scalar", test_field_key_scalar);
    run("test_field_key_array_offsets", test_field_key_array_offsets);

    TEST_GROUP("FF_ARRAY");
    run("test_array_header_rw", test_array_header_rw);

    TEST_GROUP("FF_STRING");
    run("test_string_block_rw", test_string_block_rw);
    run("test_string_block_empty", test_string_block_empty);

    TEST_GROUP("FF_CHECKSUM");
    run("test_checksum_null_sentinel", test_checksum_null_sentinel);

    TEST_GROUP("ResourceType");
    run("test_resource_type_from_recovery", test_resource_type_from_recovery);
    run("test_resource_type_traits", test_resource_type_traits);

    TEST_GROUP("ByteOps");
    run("test_load_store", test_load_store);

    TEST_GROUP("DATA_BLOCK");
    run("test_validate_offset", test_validate_offset);

    std::cout << "\n────────────────────────────────────────────────\n"
              << g_tests << " tests, " << g_failures << " failures\n";
    return g_failures > 0 ? 1 : 0;
}
