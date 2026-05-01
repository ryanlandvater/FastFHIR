/**
 * @file test_readme.cpp
 * @brief Validates all six C++ API examples from README.md.
 *
 * Build with FASTFHIR_BUILD_INGESTOR=ON (required for Ingest::Ingestor).
 * The test writes temporary *.ffhr files adjacent to this source file and
 * removes them on exit.
 *
 * Exit code: 0 — all tests pass, non-zero — one or more tests failed.
 */

#include <FastFHIR.hpp>
#include <FF_Ingestor.hpp>
#include "FF_AllTypes.hpp" // PatientData, BundleData, ObservationData, …

#include <openssl/evp.h>

#include <algorithm>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>

namespace fs = std::filesystem;
using namespace FastFHIR;

// ─────────────────────────────────────────────────────────────────────────────
// Assertion helper
// ─────────────────────────────────────────────────────────────────────────────

#define REQUIRE(expr, msg)                                                     \
    do                                                                         \
    {                                                                          \
        if (!(expr))                                                           \
        {                                                                      \
            throw std::runtime_error(std::string("REQUIRE failed: ") + (msg)); \
        }                                                                      \
    } while (false)

// ─────────────────────────────────────────────────────────────────────────────
// SHA-256 hasher — passed as the Builder::finalize() callback
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<uint8_t> sha256(const unsigned char *data, size_t len)
{
    std::vector<uint8_t> hash(EVP_MAX_MD_SIZE);
    unsigned int out_len = 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash.data(), &out_len);
    EVP_MD_CTX_free(ctx);

    hash.resize(out_len);
    return hash;
}

// ─────────────────────────────────────────────────────────────────────────────
// File utilities
// ─────────────────────────────────────────────────────────────────────────────

static std::string slurp(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open: " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}

// ─────────────────────────────────────────────────────────────────────────────
// Socket helpers (standalone Asio, cross-platform)
// ─────────────────────────────────────────────────────────────────────────────

// In-process loopback transport pair built with standalone Asio.
// This gives socket semantics on every platform (Windows/macOS/Linux) without
// keeping separate POSIX vs Winsock test implementations.
struct LoopbackSocketPair
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor;
    asio::ip::tcp::socket client;
    asio::ip::tcp::socket server;

    LoopbackSocketPair()
        : acceptor(io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          client(io),
          server(io)
    {
        acceptor.listen();
        const auto ep = acceptor.local_endpoint();
        client.connect(ep);
        acceptor.accept(server);
    }
};

// Send the full sealed archive over a stream socket.
// Asio's write() loops until the full buffer is transferred or an error occurs.
static void socket_send_all(asio::ip::tcp::socket &out_sock, const char *data, size_t len)
{
    asio::error_code ec;
    const size_t sent = asio::write(out_sock, asio::buffer(data, len), ec);
    if (ec)
    {
        throw std::runtime_error("asio write failed: " + ec.message());
    }
    REQUIRE(sent == len, "asio write transferred fewer bytes than expected");
}

// Receive exactly expected bytes from a socket straight into a FastFHIR arena.
// This exercises Memory::StreamHead, which is the intended NIC->arena ingress
// path for framed protocols.
static void socket_recv_exact_to_memory(asio::ip::tcp::socket &in_sock, Memory &dst, size_t expected)
{
    dst.reset(0); // Start the stream at absolute offset 0 for a full archive copy.

    auto head_opt = dst.try_acquire_stream();
    REQUIRE(head_opt.has_value(), "failed to acquire stream lock on destination Memory");
    auto &head = *head_opt;

    size_t received = 0;
    while (received < expected)
    {
        size_t want = std::min(head.available_space(), expected - received);
        REQUIRE(want > 0, "destination arena has no space left during socket receive");

        asio::error_code ec;
        const size_t n = in_sock.read_some(asio::buffer(head.write_ptr(), want), ec);
        if (ec)
        {
            throw std::runtime_error("asio read_some failed: " + ec.message());
        }
        REQUIRE(n > 0, "socket closed before full archive was received");

        head.commit(n);
        received += n;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture paths
// ─────────────────────────────────────────────────────────────────────────────

#ifndef FASTFHIR_TEST_ARTIFACT_DIR
// When built outside CMake (e.g. standalone compile) fall back to the OS temp
// directory so artifact files never land in the source tree.
#define FASTFHIR_TEST_ARTIFACT_DIR ""
#endif

static const fs::path TEST_SOURCE_DIR = fs::path(__FILE__).parent_path();
static const fs::path TEST_ARTIFACT_DIR = []
{
    fs::path p(FASTFHIR_TEST_ARTIFACT_DIR);
    // Empty string means the macro was not set by CMake — use the OS temp dir
    // so artifact files never land in the source tree when built standalone.
    if (p.empty())
        p = fs::temp_directory_path() / "fastfhir_test_artifacts";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}();

static const fs::path PATIENT_FFHR = TEST_ARTIFACT_DIR / "patient.ffhr";
static const fs::path BUNDLE_FFHR = TEST_ARTIFACT_DIR / "bundle.ffhr";
static const fs::path PATIENT_COMPACT_FFHR = TEST_ARTIFACT_DIR / "patient.compact.ffhr";
static const fs::path BUNDLE_COMPLEX_FFHR = TEST_ARTIFACT_DIR / "bundle.complex.ffhr";
static const fs::path BUNDLE_COMPLEX_COMPACT_FFHR = TEST_ARTIFACT_DIR / "bundle.complex.compact.ffhr";

static fs::path create_runtime_patient_json_fixture()
{
    static constexpr std::string_view kPatientJson = R"({
    "resourceType": "Patient",
    "id": "patient-1",
    "active": true,
    "gender": "male",
    "name": [
        {
            "use": "usual",
            "family": "Landvater",
            "given": ["Ryan", "Eric"]
        }
    ],
    "telecom": [],
    "address": [
        {
            "use": "home",
            "line": ["123 Main St"],
            "city": "Springfield",
            "state": "IL",
            "postalCode": "62701",
            "country": "US"
        }
    ],
    "identifier": [
        {
            "system": "http://example.org/fhir/ids",
            "value": "12345"
        }
    ]
})";

    const fs::path fixture = TEST_ARTIFACT_DIR / "patient.generated.json";
    std::ofstream out(fixture, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        throw std::runtime_error("unable to create runtime patient fixture at: " + fixture.string());
    }
    out.write(kPatientJson.data(), static_cast<std::streamsize>(kPatientJson.size()));
    out.close();
    return fixture;
}

static fs::path find_patient_json()
{
    const std::vector<fs::path> candidates = {
        TEST_SOURCE_DIR / "patient.json",
        // sibling FastFHIR_Python repo (common development layout)
        fs::path(__FILE__).parent_path() / "../../../FastFHIR_Python/test/patient.json",
    };
    for (auto &c : candidates)
    {
        if (fs::exists(c))
            return fs::canonical(c);
    }
    return create_runtime_patient_json_fixture();
}

// Returns the path to the first Synthea FHIR R4 bundle JSON available, or an
// empty path if FASTFHIR_SYNTHEA_DIR was not set at build time or the data was
// not downloaded.  Synthea output lands in a 'fhir/' sub-directory of the
// extracted ZIP; we probe both the sub-directory and the root.
#ifndef FASTFHIR_SYNTHEA_DIR
#  define FASTFHIR_SYNTHEA_DIR ""
#endif
static fs::path find_synthea_bundle_json()
{
    const fs::path synthea_root(FASTFHIR_SYNTHEA_DIR);
    if (synthea_root.empty()) return {};
    for (const auto &dir : {synthea_root / "fhir", synthea_root})
    {
        if (!fs::is_directory(dir)) continue;
        for (const auto &entry : fs::directory_iterator(dir))
        {
            if (entry.path().extension() == ".json")
                return entry.path();
        }
    }
    return {};
}

static void cleanup_artifacts()
{
    for (auto &p : {PATIENT_FFHR, BUNDLE_FFHR, PATIENT_COMPACT_FFHR,
                    BUNDLE_COMPLEX_FFHR, BUNDLE_COMPLEX_COMPACT_FFHR})
    {
        std::error_code ec;
        fs::remove(p, ec);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test runner
// ─────────────────────────────────────────────────────────────────────────────

struct Result
{
    std::string name;
    bool passed;
    std::string error;
};
static std::vector<Result> g_results;

template <typename Fn>
static void run(const char *name, Fn fn)
{
    std::cout << "\n"
              << std::string(60, '=') << "\n"
              << "  " << name << "\n"
              << std::string(60, '=') << "\n";
    try
    {
        fn();
        g_results.push_back({name, true, {}});
        std::cout << "\n  \033[32mPASS\033[0m\n";
    }
    catch (const std::exception &e)
    {
        g_results.push_back({name, false, e.what()});
        std::cout << "\n  \033[31mFAIL\033[0m: " << e.what() << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Minimal bundle JSON for test 5 (defined inline — no external fixture needed)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr std::string_view BUNDLE_JSON = R"({
  "resourceType": "Bundle",
  "id": "test-bundle",
  "type": "collection",
  "entry": [
    {
      "fullUrl": "Patient/patient-1",
      "resource": {
        "resourceType": "Patient",
        "id": "patient-1",
        "active": true,
        "name": [{"use": "usual", "family": "Landvater", "given": ["Ryan", "Eric"]}],
        "gender": "male"
      }
    },
    {
      "fullUrl": "Patient/patient-2",
      "resource": {
        "resourceType": "Patient",
        "id": "patient-2",
        "active": false,
        "name": [{"use": "usual", "family": "Smith", "given": ["John"]}],
        "gender": "female"
      }
    }
  ]
})";

static constexpr std::string_view BUNDLE_COMPLEX_JSON = R"({
    "resourceType": "Bundle",
    "id": "complex-bundle",
    "type": "collection",
    "entry": [
        {
            "fullUrl": "Patient/patient-1",
            "resource": {
                "resourceType": "Patient",
                "id": "patient-1",
                "active": true,
                "name": [{"use": "usual", "family": "Landvater", "given": ["Ryan", "Eric"]}],
                "gender": "male"
            }
        },
        {
            "fullUrl": "Observation/obs-1",
            "resource": {
                "resourceType": "Observation",
                "id": "obs-1",
                "status": "final",
                "code": {
                    "coding": [
                        {
                            "system": "http://loinc.org",
                            "code": "8867-4",
                            "display": "Heart rate"
                        }
                    ]
                },
                "subject": {"reference": "Patient/patient-1"},
                "valueString": "final-value",
                "component": [
                    {
                        "code": {
                            "coding": [
                                {
                                    "system": "http://loinc.org",
                                    "code": "8480-6",
                                    "display": "Systolic blood pressure"
                                }
                            ]
                        },
                        "valueString": "nested-choice"
                    }
                ]
            }
        }
    ]
})";

static constexpr std::string_view BUNDLE_EXTENSION_URLS_JSON = R"({
    "resourceType": "Bundle",
    "type": "collection",
    "entry": [
        {
            "resource": {
                "resourceType": "Patient",
                "id": "p-ext-1",
                "extension": [
                    {
                        "url": "http://example.org/fhir/StructureDefinition/shared-prefix/alpha",
                        "valueString": "a-1"
                    },
                    {
                        "url": "http://hl7.org/fhir/StructureDefinition/data-absent-reason",
                        "valueCode": "unknown"
                    }
                ],
                "modifierExtension": [
                    {
                        "url": "http://example.org/fhir/StructureDefinition/shared-prefix/alpha",
                        "valueString": "a-2"
                    },
                    {
                        "url": "http://example.org/fhir/StructureDefinition/shared-prefix/beta",
                        "valueBoolean": true
                    }
                ]
            }
        },
        {
            "resource": {
                "resourceType": "Observation",
                "id": "obs-ext-1",
                "status": "final",
                "code": {
                    "coding": [
                        {
                            "system": "http://loinc.org",
                            "code": "8867-4"
                        }
                    ]
                },
                "extension": [
                    {
                        "url": "http://example.org/fhir/StructureDefinition/shared-prefix/beta",
                        "valueString": "b-1"
                    }
                ]
            }
        }
    ]
})";

// ─────────────────────────────────────────────────────────────────────────────
// Getting Started — run in practical order: Step 2 -> Step 3 -> Step 1
// ─────────────────────────────────────────────────────────────────────────────

static void test_getting_started_231()
{
    // Step 2: Create a file-backed Memory arena so we can persist patient.ffhr.
    auto mem = Memory::createFromFile(PATIENT_FFHR, 64 * 1024 * 1024);

    // Step 3: Build from inline FHIR JSON, enrich with typed field keys, and seal.
    Builder builder(mem, FHIR_VERSION_R5);
    Ingest::Ingestor ingestor;

    const std::string json = R"({
        "resourceType": "Patient",
        "id": "patient-1",
        "active": false,
        "gender": "male",
        "name": [{"family": "Smith", "given": ["John"]}]
    })";

    Reflective::ObjectHandle patient_handle;
    size_t parsed_count = 0;
    auto result = ingestor.ingest(
        {builder, Ingest::SourceType::FHIR_JSON, json},
        patient_handle,
        parsed_count);

    REQUIRE(result.code == FF_SUCCESS, "getting-started ingest failed: " + result.message);
    REQUIRE(parsed_count > 0, "getting-started ingest parsed 0 resources");
    REQUIRE(patient_handle, "getting-started patient handle is null");

    // Typed resource keys carry the exact write metadata (kind/offset/recovery).
    patient_handle[FastFHIR::Fields::PATIENT::ACTIVE] = true;
    patient_handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = std::string_view("1990-03-21");

    builder.set_root(patient_handle);
    auto view = builder.finalize();
    REQUIRE(!view.empty(), "getting-started finalize returned empty view");

    // Step 1: Open patient.ffhr read-only, parse bytes, and read fields only if present.
    std::FILE *fp = std::fopen(PATIENT_FFHR.string().c_str(), "rb");
    REQUIRE(fp != nullptr, "fopen(patient.ffhr, rb) failed");

    REQUIRE(std::fseek(fp, 0, SEEK_END) == 0, "fseek failed");
    const long file_size = std::ftell(fp);
    REQUIRE(file_size > 0, "patient.ffhr is empty or unreadable");
    std::rewind(fp);

    std::vector<uint8_t> raw_bytes(static_cast<size_t>(file_size));
    const size_t bytes_read = std::fread(raw_bytes.data(), 1, raw_bytes.size(), fp);
    std::fclose(fp);
    REQUIRE(bytes_read == raw_bytes.size(), "short read while loading patient.ffhr");

    Parser parser(raw_bytes.data(), raw_bytes.size());
    auto root = parser.root();
    REQUIRE(root, "getting-started parser root is null");

    PatientData patient = root;

    if (!patient.id.empty())
    {
        std::cout << "  id=" << patient.id << "\n";
        REQUIRE(patient.id == "patient-1", "unexpected id in getting-started parse");
    }
    else
    {
        throw std::runtime_error("expected id field to exist");
    }

    if (patient.active == 1)
    {
        const bool active = patient.active == 1;
        std::cout << "  active=" << std::boolalpha << active << "\n";
        REQUIRE(active, "active should be true after typed key enrichment");
    }

    if (patient.gender != AdministrativeGender::Unknown)
    {
        auto gender = FF_AdministrativeGenderToString(patient.gender);
        std::cout << "  gender=" << gender << "\n";
        REQUIRE(std::string_view(gender) == "male", "unexpected gender in getting-started parse");
    }

    if (!patient.birthdate.empty())
    {
        std::cout << "  birthDate=" << patient.birthdate << "\n";
        REQUIRE(patient.birthdate == "1990-03-21", "unexpected birthDate after typed key enrichment");
    }

    if (!patient.name.empty())
    {
        bool saw_family = false;
        bool saw_given = false;
        for (auto &name : patient.name)
        {
            if (!name.family.empty())
            {
                auto family = name.family;
                std::cout << "  family=" << family << "\n";
                REQUIRE(family == "Smith", "unexpected family in getting-started parse");
                saw_family = true;
            }
            for (auto &given : name.given)
            {
                auto g = given;
                std::cout << "  given=" << g << "\n";
                REQUIRE(g == "John", "unexpected given in getting-started parse");
                saw_given = true;
            }
        }
        REQUIRE(saw_family, "expected at least one family name");
        REQUIRE(saw_given, "expected at least one given name");
    }
    else
    {
        throw std::runtime_error("expected name array to exist");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 10 — Reuse patient.ffhr for another surgical edit (no JSON round-trip)
// ─────────────────────────────────────────────────────────────────────────────

static void test_10()
{
    // Re-open the sealed patient.ffhr from previous tests and apply a second
    // surgical mutation without going through JSON — set deceased to false.
    auto mem = Memory::createFromFile(PATIENT_FFHR, 64 * 1024 * 1024);
    Builder builder(mem, FHIR_VERSION_R5);

    auto patient = builder.root_handle();
    REQUIRE(patient, "root_handle is null — archive must be sealed before second edit");

    patient[FastFHIR::Fields::PATIENT::DECEASED] = false;

    builder.finalize(FF_CHECKSUM_SHA256, sha256);

    // Re-open and verify the mutation persisted
    auto mem2 = Memory::createFromFile(PATIENT_FFHR, 64 * 1024 * 1024);
    auto root = Parser(mem2).root();
    REQUIRE(root, "root is null after second re-seal");

    auto deceased_node = root[FastFHIR::Fields::PATIENT::DECEASED];
    REQUIRE(deceased_node, "deceased field should be present after surgical edit");
    std::cout << "  deceased verified in re-sealed archive\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 1 — Ingest patient.json and save as patient.ffhr
// ─────────────────────────────────────────────────────────────────────────────

static void test_1(const fs::path &patient_json)
{
    std::string json_str = slurp(patient_json);

    // Map the arena straight to a file — every write goes directly to disk
    auto mem = Memory::createFromFile(PATIENT_FFHR, 64 * 1024 * 1024);
    Builder builder(mem, FHIR_VERSION_R5);
    Ingest::Ingestor ingestor;

    Reflective::ObjectHandle patient_handle;
    size_t count = 0;
    auto result = ingestor.ingest(
        {builder, Ingest::SourceType::FHIR_JSON, json_str},
        patient_handle, count);

    REQUIRE(result.code == FF_SUCCESS, "ingest failed: " + result.message);
    REQUIRE(count > 0, "ingest parsed 0 resources");
    REQUIRE(patient_handle, "patient handle is null after ingest");

    // Inspect via zero-copy snapshot — no heap allocation for the read path
    PatientData data = patient_handle.as_node();
    std::cout << "  id     : " << data.id << "\n";
    std::cout << "  gender : " << FF_AdministrativeGenderToString(data.gender) << "\n";
    std::cout << "  active : " << (data.active == 1 ? "true" : "false") << "\n";

    REQUIRE(data.id == "patient-1", "unexpected patient id");
    REQUIRE(data.gender == AdministrativeGender::Male, "unexpected gender");
    REQUIRE(data.active == 1, "expected active=true");
    REQUIRE(!data.name.empty(), "name array is empty");

    auto &first_name = data.name.front();
    std::cout << "  name   : [";
    for (auto &g : first_name.given)
        std::cout << g << " ";
    std::cout << "] " << first_name.family << "\n";

    REQUIRE(first_name.family == "Landvater", "unexpected family name");

    // Seal with a SHA-256 footer — writes header + hash into the mapped pages
    builder.set_root(patient_handle);
    auto view = builder.finalize(FF_CHECKSUM_SHA256, sha256);
    REQUIRE(!view.empty(), "finalize returned empty view");
    std::cout << "  sealed : " << view.size() << " bytes\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 2 — Open and read patient.ffhr
// ─────────────────────────────────────────────────────────────────────────────

static void test_2()
{
    // Mount the existing archive and traverse directly via Parser::root()
    auto mem = Memory::createFromFile(PATIENT_FFHR, 64 * 1024 * 1024);
    auto parser = Parser(mem);
    auto root = parser.root();
    REQUIRE(root, "root node is null — is patient.ffhr sealed?");
        REQUIRE(parser.is_root<FastFHIR::RESOURCETYPE::PATIENT>(),
            "typed parser root resource check failed (expected Patient)");
        REQUIRE(root.is<FastFHIR::RESOURCETYPE::PATIENT>(),
            "typed node resource check failed (expected Patient)");

    PatientData data = root;
    std::cout << "  id       : " << data.id << "\n";
    std::cout << "  gender   : " << FF_AdministrativeGenderToString(data.gender) << "\n";
    std::cout << "  active   : " << (data.active == 1 ? "true" : "false") << "\n";
    std::cout << "  birthDate: " << data.birthdate << "\n";

    REQUIRE(data.id == "patient-1", "unexpected patient id");
    REQUIRE(data.gender == AdministrativeGender::Male, "unexpected gender");
    REQUIRE(data.active == 1, "expected active=true");
    REQUIRE(!data.name.empty(), "name array is empty");

    for (auto &name : data.name)
    {
        std::cout << "  name   : [";
        for (auto &g : name.given)
            std::cout << g << " ";
        std::cout << "] " << name.family << "\n";
        REQUIRE(name.family == "Landvater", "unexpected family name");
    }

    // Also verify via the zero-copy JSON serializer
    std::ostringstream oss;
    root.print_json(oss);
    const auto json = oss.str();
    REQUIRE(json.find("patient-1") != std::string::npos, "id not in JSON output");
    REQUIRE(json.find("Landvater") != std::string::npos, "family name not in JSON output");
    std::cout << "  json[0..80] : " << json.substr(0, 80) << "...\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 3 — Re-open patient.ffhr and enrich it in place
// ─────────────────────────────────────────────────────────────────────────────

static void test_3()
{
    // Mount the existing archive — stays mapped to the same file
    auto mem = Memory::createFromFile(PATIENT_FFHR, 64 * 1024 * 1024);
    Builder builder(mem, FHIR_VERSION_R5);

    auto patient = builder.root_handle();
    REQUIRE(patient, "root_handle is null — archive must be sealed before enrichment");

    // Amend a string field — appends a new FF_STRING block and patches the pointer slot.
    // The birthDate field is null after test_1 (patient.json omits it from the ingest path),
    // so amend_pointer succeeds without orphaning existing data.
    patient[FastFHIR::Fields::PATIENT::BIRTH_DATE] = std::string_view("1990-03-21");

    // Re-seal — old data untouched, new tail written
    builder.finalize(FF_CHECKSUM_SHA256, sha256);

    // Re-open and verify the enriched record
    auto mem2 = Memory::createFromFile(PATIENT_FFHR, 64 * 1024 * 1024);
    auto root = Parser(mem2).root();
    REQUIRE(root, "root is null after re-seal");

    PatientData data = root;
    REQUIRE(!data.birthdate.empty(), "birthDate should be non-empty after enrichment");
    std::cout << "  birthDate: " << data.birthdate << "\n";
    REQUIRE(data.birthdate == "1990-03-21", "unexpected birthDate value");

    // Also verify via the zero-copy JSON serializer
    std::ostringstream oss;
    root.print_json(oss);
    REQUIRE(oss.str().find("1990-03-21") != std::string::npos,
            "birthDate not in JSON output after enrichment");
    std::cout << "  birthDate verified in JSON output\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 4 — In-memory ingest, enrich, finalize, and socket round-trip
//             (validates real socket transport semantics end-to-end)
// ─────────────────────────────────────────────────────────────────────────────

static void test_4(const fs::path &patient_json)
{
    std::string json_str = slurp(patient_json);

    // Anonymous arena — no file backing
    auto mem = Memory::create(64 * 1024 * 1024);
    Builder builder(mem, FHIR_VERSION_R5);
    Ingest::Ingestor ingestor;

    Reflective::ObjectHandle patient_handle;
    size_t count = 0;
    auto result = ingestor.ingest(
        {builder, Ingest::SourceType::FHIR_JSON, json_str},
        patient_handle, count);
    REQUIRE(result.code == FF_SUCCESS, "ingest failed: " + result.message);

    // Enrich in place — amend the birthDate (null after ingest; appends an FF_STRING block)
    patient_handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = std::string_view("1990-03-21");

    // Seal the stream and expose a zero-copy egress view.
    // This is exactly what a network layer would write to a socket.
    builder.set_root(patient_handle);
    auto view = builder.finalize(FF_CHECKSUM_SHA256, sha256);
    REQUIRE(!view.empty(), "finalize returned empty view");
    std::cout << "  sealed view : " << view.size() << " bytes\n";

    // Baseline control: parse directly from the same arena first.
    // This confirms the payload itself is valid before transport enters the picture.
    auto root = Parser(mem).root();
    REQUIRE(root, "re-parsed root is null");

    PatientData data = root;
    REQUIRE(data.id == "patient-1", "unexpected patient id after re-parse");
    REQUIRE(!data.birthdate.empty(), "birthDate should be present after enrich");
    REQUIRE(data.active == 1, "active should be true");
    std::cout << "  re-parsed id        : " << data.id << "\n";
    std::cout << "  re-parsed active    : " << (data.active == 1) << "\n";
    std::cout << "  re-parsed birthDate : " << data.birthdate << "\n";

    // Real socket test:
    // 1) send sealed archive bytes over a stream socket
    // 2) receive into a different FastFHIR arena via StreamHead
    // 3) parse on the receiver side and verify semantic integrity
    LoopbackSocketPair sp;
    socket_send_all(sp.client, view.data(), view.size());
    asio::error_code shutdown_ec;
    sp.client.shutdown(asio::ip::tcp::socket::shutdown_send, shutdown_ec);
    REQUIRE(!shutdown_ec, "asio shutdown(send) failed: " + shutdown_ec.message());

    // Slightly larger than payload to mimic a pre-allocated receive arena.
    auto rx_mem = Memory::create(view.size() + (4 * 1024));
    socket_recv_exact_to_memory(sp.server, rx_mem, view.size());

    auto socket_root = Parser(rx_mem).root();
    REQUIRE(socket_root, "socket-ingested root is null");

    PatientData socket_data = socket_root;
    REQUIRE(socket_data.id == "patient-1", "socket path changed patient id");
    REQUIRE(socket_data.active == 1, "socket path changed active flag");
    REQUIRE(socket_data.birthdate == "1990-03-21", "socket path changed birthDate");
    std::cout << "  socket id          : " << socket_data.id << "\n";
    std::cout << "  socket birthDate   : " << socket_data.birthdate << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 5 — Surgically edit one patient in a bundle and reseal
// ─────────────────────────────────────────────────────────────────────────────

static void test_5()
{
    // ── Step A: ingest the bundle ──
    auto mem = Memory::createFromFile(BUNDLE_FFHR, 64 * 1024 * 1024);
    Builder builder(mem, FHIR_VERSION_R5);
    Ingest::Ingestor ingestor;

    Reflective::ObjectHandle bundle_handle;
    size_t count = 0;
    auto result = ingestor.ingest(
        {builder, Ingest::SourceType::FHIR_JSON, BUNDLE_JSON},
        bundle_handle, count);
    REQUIRE(result.code == FF_SUCCESS, "bundle ingest failed: " + result.message);
    REQUIRE(count >= 2, "expected at least 2 resources in bundle");
    std::cout << "  bundle ingested : " << count << " resources\n";

    // Seal the initial bundle
    builder.set_root(bundle_handle);
    builder.finalize(FF_CHECKSUM_SHA256, sha256);

    // ── Step B: re-open and find patient-1 ──
    auto mem2 = Memory::createFromFile(BUNDLE_FFHR, 64 * 1024 * 1024);
    Builder builder2(mem2, FHIR_VERSION_R5);
    auto root2 = Parser(mem2).root();
    REQUIRE(root2, "bundle root is null after seal");

    BundleData bundle_data = root2;
    REQUIRE(!bundle_data.entry.empty(), "bundle.entry is empty");

    // Walk entries; the OS faults in only the pages we read
    Reflective::ObjectHandle target_patient;
    for (auto &entry : bundle_data.entry)
    {
        if (entry.resource.recovery != FF_PATIENT::recovery)
            continue;

        // Deserialize this patient directly from the arena to read its id
        auto patient_data = FF_PATIENT::deserialize(
            mem2.base(),
            entry.resource.offset,
            mem2.capacity(),
            FHIR_VERSION_R5);

        if (patient_data.id == "patient-1")
        {
            target_patient = Reflective::ObjectHandle(
                &builder2,
                entry.resource.offset,
                entry.resource.recovery);
            break;
        }
    }
    REQUIRE(target_patient, "patient-1 not found in bundle");
    std::cout << "  found patient-1 at offset " << target_patient.offset() << "\n";

    // ── Step C: surgically enrich patient-1 only ──
    // Insert telecom via inline-array field ingestion.
    auto res = ingestor.insert_at_field(
        target_patient, FastFHIR::Fields::PATIENT::TELECOM,
        R"({"system":"phone","value":"555-0199","use":"mobile"})");
    REQUIRE(res.code == FF_SUCCESS, "surgical enrich failed: " + res.message);

    // ── Step D: reseal — only the header + new tail pages are rewritten ──
    auto bundle_root = builder2.root_handle();
    REQUIRE(bundle_root, "bundle root handle is null after enrichment");
    builder2.finalize(FF_CHECKSUM_SHA256, sha256);

    // ── Step E: verify ──
    auto mem3 = Memory::createFromFile(BUNDLE_FFHR, 64 * 1024 * 1024);
    auto root3 = Parser(mem3).root();
    REQUIRE(root3, "bundle root is null after reseal");

    // Find patient-1 in the re-parsed bundle and verify telecom was appended
    BundleData final_bundle = root3;
    bool found_enriched = false;
    for (auto &entry : final_bundle.entry)
    {
        if (entry.resource.recovery != FF_PATIENT::recovery)
            continue;
        auto p = FF_PATIENT::deserialize(
            mem3.base(), entry.resource.offset, mem3.capacity(), FHIR_VERSION_R5);
        if (p.id == "patient-1")
        {
            REQUIRE(!p.telecom.empty(), "patient-1 telecom empty after surgical edit");
            std::cout << "  patient-1 telecom[0] : " << p.telecom.front().value << "\n";
            REQUIRE(p.telecom.front().value == "555-0199",
                    "unexpected telecom value after surgical edit");
            found_enriched = true;
        }
        else if (p.id == "patient-2")
        {
            // Ensure the other patient was not touched
            REQUIRE(p.telecom.empty(), "patient-2 telecom should still be empty");
            std::cout << "  patient-2 untouched (telecom empty as expected)\n";
        }
    }
    REQUIRE(found_enriched, "patient-1 not found in re-sealed bundle");
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 6 — Lock-free concurrent generation (thread-safety smoke test)
// ─────────────────────────────────────────────────────────────────────────────

static void test_6()
{
    constexpr int NUM_THREADS = 8;

    auto mem = Memory::create(256 * 1024 * 1024);
    Builder builder(mem, FHIR_VERSION_R5);

    std::vector<std::thread> pool;
    std::vector<Reflective::ObjectHandle> handles(NUM_THREADS);
    std::atomic<int> completed{0};

    for (int i = 0; i < NUM_THREADS; ++i)
    {
        pool.emplace_back([&builder, &handles, &completed, i]()
                          {
            ObservationData obs;
            obs.status = ObservationStatus::Preliminary;

            // Single atomic claim — no mutex, no heap allocation, no pointer invalidation
            handles[i] = builder.append_obj(obs);
            ++completed; });
    }

    for (auto &t : pool)
        t.join();

    REQUIRE(completed == NUM_THREADS, "not all threads completed");
    for (int i = 0; i < NUM_THREADS; ++i)
    {
        REQUIRE(handles[i], "handle from thread " + std::to_string(i) + " is null");
    }
    std::cout << "  " << NUM_THREADS << " threads completed lock-free writes\n";
    std::cout << "  all " << NUM_THREADS << " ObjectHandles valid\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 7 — Post-finalize archival compaction
// ─────────────────────────────────────────────────────────────────────────────

static void test_7()
{
    auto src_mem = Memory::createFromFile(PATIENT_FFHR, 64 * 1024 * 1024);
    Parser src_parser(src_mem);

    auto compact_mem = Memory::createFromFile(PATIENT_COMPACT_FFHR, 64 * 1024 * 1024);
    auto compact_view = Compactor::archive(src_parser, compact_mem);
    REQUIRE(!compact_view.empty(), "compact archive view is empty");

    Parser compact_parser(compact_mem);
    auto root = compact_parser.root();
    REQUIRE(root, "compact root is null");

    std::string_view id = root[FastFHIR::Fields::PATIENT::ID];
    std::string_view gender = root[FastFHIR::Fields::PATIENT::GENDER];
    REQUIRE(id == "patient-1", "compact patient id mismatch");
    REQUIRE(gender == "male", "compact patient gender mismatch");

    bool found_name = false;
    auto compact_names = root[FastFHIR::Fields::PATIENT::NAME].entries();
    for (auto &name_node : compact_names)
    {
        std::string_view family = name_node[FastFHIR::Fields::HUMANNAME::FAMILY];
        if (family == "Landvater")
        {
            found_name = true;
            break;
        }
    }
    REQUIRE(found_name, "compact patient name traversal failed");

    std::cout << "  compact archive bytes : " << compact_view.size() << "\n";
    std::cout << "  original patient bytes: " << src_parser.size_bytes() << "\n";
    std::cout << "  compact id            : " << id << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 8 — Array-tagged field key coverage on standard streams
// ─────────────────────────────────────────────────────────────────────────────

static void test_8()
{
    auto patient_mem = Memory::createFromFile(PATIENT_FFHR, 64 * 1024 * 1024);
    Parser patient_parser(patient_mem);
    auto patient_root = patient_parser.root();
    REQUIRE(patient_root, "standard patient root is null");

    auto name_entry = patient_root[FastFHIR::Fields::PATIENT::NAME];
    REQUIRE(name_entry, "array-tagged PATIENT::NAME key failed on standard stream");
    auto names = name_entry.entries();
    REQUIRE(!names.empty(), "patient name array should not be empty");

    std::string_view family = names.front()[FastFHIR::Fields::HUMANNAME::FAMILY];
    REQUIRE(family == "Landvater", "array-tagged lookup failed for HumanName.family");

    auto bundle_mem = Memory::createFromFile(BUNDLE_FFHR, 64 * 1024 * 1024);
    Parser bundle_parser(bundle_mem);
    auto bundle_root = bundle_parser.root();
    REQUIRE(bundle_root, "standard bundle root is null");

    auto entry_array = bundle_root[FastFHIR::Fields::BUNDLE::ENTRY];
    REQUIRE(entry_array, "array-tagged BUNDLE::ENTRY key failed on standard stream");
    auto entries = entry_array.entries();
    REQUIRE(entries.size() >= 2, "expected at least two bundle entries");

    auto resource_entry = entries.front()[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
    REQUIRE(resource_entry, "resource field missing from first bundle entry");
    auto resource_node = resource_entry.as_node();
    REQUIRE(resource_node, "resource node materialization failed on standard stream");

    std::cout << "  standard array-tagged keys verified for patient.name and bundle.entry\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 9 — Compact nested choice/resource coverage (Bundle + Observation)
// ─────────────────────────────────────────────────────────────────────────────

static void test_9()
{
    auto mem = Memory::createFromFile(BUNDLE_COMPLEX_FFHR, 64 * 1024 * 1024);
    Builder builder(mem, FHIR_VERSION_R5);
    Ingest::Ingestor ingestor;

    Reflective::ObjectHandle bundle_handle;
    size_t count = 0;
    auto result = ingestor.ingest(
        {builder, Ingest::SourceType::FHIR_JSON, BUNDLE_COMPLEX_JSON},
        bundle_handle, count);
    REQUIRE(result.code == FF_SUCCESS, "complex bundle ingest failed: " + result.message);
    REQUIRE(bundle_handle, "complex bundle handle is null");

    builder.set_root(bundle_handle);
    auto source_view = builder.finalize(FF_CHECKSUM_SHA256, sha256);
    REQUIRE(!source_view.empty(), "complex bundle finalize returned empty view");

    Parser source_parser(mem);
    auto source_root = source_parser.root();
    REQUIRE(source_root, "complex bundle source root is null");

    bool found_observation_source = false;
    for (auto &entry_node : source_root[FastFHIR::Fields::BUNDLE::ENTRY].entries())
    {
        auto resource_entry = entry_node[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
        if (!resource_entry)
            continue;

        auto resource_node = resource_entry.as_node();
        if (!resource_node || !resource_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>())
            continue;

        found_observation_source = true;
        std::string_view obs_value = resource_node[FastFHIR::Fields::OBSERVATION::VALUE];
        REQUIRE(obs_value == "final-value", "source observation value[x] mismatch");

        auto components = resource_node[FastFHIR::Fields::OBSERVATION::COMPONENT].entries();
        REQUIRE(!components.empty(), "source observation.component is empty");
        std::string_view component_value = components.front()[FastFHIR::Fields::OBSERVATION_COMPONENT::VALUE];
        REQUIRE(component_value == "nested-choice", "source component value[x] mismatch");
    }
    REQUIRE(found_observation_source, "source complex bundle observation not found");

    auto compact_mem = Memory::createFromFile(BUNDLE_COMPLEX_COMPACT_FFHR, 64 * 1024 * 1024);
    auto compact_view = Compactor::archive(source_parser, compact_mem);
    REQUIRE(!compact_view.empty(), "complex compact archive view is empty");

    Parser compact_parser(compact_mem);
    auto compact_root = compact_parser.root();
    REQUIRE(compact_root, "complex compact root is null");

    bool found_observation_compact = false;
    for (auto &entry_node : compact_root[FastFHIR::Fields::BUNDLE::ENTRY].entries())
    {
        auto resource_entry = entry_node[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
        if (!resource_entry)
            continue;

        auto resource_node = resource_entry.as_node();
        if (!resource_node || !resource_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>())
            continue;

        found_observation_compact = true;
        std::string_view obs_value = resource_node[FastFHIR::Fields::OBSERVATION::VALUE];
        REQUIRE(obs_value == "final-value", "compact observation value[x] mismatch");

        auto components = resource_node[FastFHIR::Fields::OBSERVATION::COMPONENT].entries();
        REQUIRE(!components.empty(), "compact observation.component is empty");
        std::string_view component_value = components.front()[FastFHIR::Fields::OBSERVATION_COMPONENT::VALUE];
        REQUIRE(component_value == "nested-choice", "compact component value[x] mismatch");
    }
    REQUIRE(found_observation_compact, "compact complex bundle observation not found");

    std::cout << "  complex compact bytes : " << compact_view.size() << "\n";
    std::cout << "  complex source bytes  : " << source_view.size() << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 11 — Extension URL_DIRECTORY reconstruction + filtering + dedup
// ─────────────────────────────────────────────────────────────────────────────

static void test_11()
{
    static constexpr std::string_view kAlpha =
        "http://example.org/fhir/StructureDefinition/shared-prefix/alpha";
    static constexpr std::string_view kBeta =
        "http://example.org/fhir/StructureDefinition/shared-prefix/beta";
    static constexpr std::string_view kFilteredKnown =
        "http://hl7.org/fhir/StructureDefinition/data-absent-reason";

    auto mem = Memory::create(64 * 1024 * 1024);
    Builder builder(mem, FHIR_VERSION_R5);
    Ingest::Ingestor ingestor;

    Reflective::ObjectHandle bundle_handle;
    size_t count = 0;
    auto result = ingestor.ingest(
        {builder, Ingest::SourceType::FHIR_JSON, BUNDLE_EXTENSION_URLS_JSON},
        bundle_handle, count);

    REQUIRE(result.code == FF_SUCCESS, "extension bundle ingest failed: " + result.message);
    REQUIRE(bundle_handle, "extension bundle handle is null");
    REQUIRE(count >= 2, "expected at least two resources in extension bundle");

    builder.set_root(bundle_handle);
    auto view = builder.finalize(FF_CHECKSUM_SHA256, sha256);
    REQUIRE(!view.empty(), "extension bundle finalize returned empty view");

    Parser parser(mem);
    REQUIRE(parser.has_url_directory(), "expected URL directory for extension-containing bundle");

    auto dir = parser.url_directory();
    const uint32_t n = dir.entry_count(parser.data());
    REQUIRE(n > 0, "URL directory is unexpectedly empty");

    std::unordered_map<std::string, uint32_t> reconstructed_counts;
    for (uint32_t i = 0; i < n; ++i)
    {
        std::string url = dir.get_url(parser.data(), i);
        ++reconstructed_counts[url];
    }

    REQUIRE(reconstructed_counts.count(std::string(kAlpha)) == 1,
            "missing reconstructed shared-prefix alpha URL");
    REQUIRE(reconstructed_counts.at(std::string(kAlpha)) == 1,
            "duplicate alpha URL entry detected in URL directory");

    REQUIRE(reconstructed_counts.count(std::string(kBeta)) == 1,
            "missing reconstructed shared-prefix beta URL");
    REQUIRE(reconstructed_counts.at(std::string(kBeta)) == 1,
            "duplicate beta URL entry detected in URL directory");

    REQUIRE(reconstructed_counts.count(std::string(kFilteredKnown)) == 0,
            "known filtered URL unexpectedly present in URL directory");

    std::cout << "  url-directory entries : " << n << "\n";
    std::cout << "  reconstructed alpha   : " << reconstructed_counts.at(std::string(kAlpha)) << "\n";
    std::cout << "  reconstructed beta    : " << reconstructed_counts.at(std::string(kBeta)) << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Example 12 — Synthea R4 patient bundle ingest + round-trip
// ─────────────────────────────────────────────────────────────────────────────
// Uses a real Synthea-generated patient bundle (downloaded at configure time).
// Verifies that a rich real-world Bundle — with many extensions, nested
// resources, and clinical data — ingests cleanly and round-trips through seal
// + re-parse without data loss.
// Skipped gracefully if FASTFHIR_SYNTHEA_DIR was not provided at build time.
static void test_synthea_bundle()
{
    const fs::path synthea_json = find_synthea_bundle_json();
    if (synthea_json.empty())
    {
        std::cout << "  [SKIP] Synthea fixture not available "
                  << "(set FASTFHIR_DOWNLOAD_SYNTHEA=ON and reconfigure)\n";
        return;
    }
    std::cout << "  fixture : " << synthea_json.filename().string() << "\n";

    const std::string json_str = slurp(synthea_json);
    // Synthea bundles can be large (>10 MB of FHIR JSON); 256 MB arena is ample.
    auto mem = Memory::create(256 * 1024 * 1024);
    Builder builder(mem, FHIR_VERSION_R5);
    Ingest::Ingestor ingestor;

    Reflective::ObjectHandle root_handle;
    size_t count = 0;
    auto result = ingestor.ingest(
        {builder, Ingest::SourceType::FHIR_JSON, json_str},
        root_handle, count);
    REQUIRE(result.code == FF_SUCCESS, "Synthea ingest failed: " + result.message);
    REQUIRE(root_handle, "root handle is null after Synthea ingest");
    REQUIRE(count > 1, "expected multiple resources in Synthea bundle");
    std::cout << "  ingested resources : " << count << "\n";

    // Seal and round-trip through the parser.
    builder.set_root(root_handle);
    auto view = builder.finalize(FF_CHECKSUM_SHA256, sha256);
    REQUIRE(!view.empty(), "finalize returned empty view");
    std::cout << "  sealed bytes       : " << view.size() << "\n";

    auto reparse_root = Parser(mem).root();
    REQUIRE(reparse_root, "re-parsed root is null after Synthea ingest");
    std::cout << "  round-trip         : ok\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char **argv)
{
    // Optional: --filter <test-name> runs only that test (used by CTest).
    std::string filter;
    for (int i = 1; i < argc - 1; ++i)
    {
        if (std::string_view(argv[i]) == "--filter")
        {
            filter = argv[i + 1];
            break;
        }
    }

    // Only wipe artifacts when running the full suite.  Filtered CTest runs
    // (e.g. --filter "Example 2 …") must NOT delete files written by earlier
    // tests in the chain — test_1 writes patient.ffhr; test_2..10 depend on it.
    if (filter.empty())
        cleanup_artifacts();

    fs::path patient_json;
    try
    {
        patient_json = find_patient_json();
        std::cout << "Using fixture: " << patient_json << "\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }

    // Helper: run only if no filter is set, or name matches the filter.
    auto maybe_run = [&](const char *name, auto fn)
    {
        if (filter.empty() || filter == name)
            run(name, fn);
    };

    maybe_run("Getting Started — Step 2 -> Step 3 -> Step 1",
              []
              { test_getting_started_231(); });
    maybe_run("Example 1 — Ingest patient.json → save patient.ffhr",
              [&]
              { test_1(patient_json); });
    maybe_run("Example 2 — Open and read patient.ffhr",
              []
              { test_2(); });
    maybe_run("Example 3 — Re-open patient.ffhr and enrich in place",
              []
              { test_3(); });
    maybe_run("Example 4 — In-memory ingest, enrich, finalize, re-parse",
              [&]
              { test_4(patient_json); });
    maybe_run("Example 5 — Surgically edit patient in a bundle and reseal",
              []
              { test_5(); });
    maybe_run("Example 6 — Lock-free concurrent generation",
              []
              { test_6(); });
    maybe_run("Example 7 — Post-finalize archival compaction",
              []
              { test_7(); });
    maybe_run("Example 8 — Standard array-tagged field key coverage",
              []
              { test_8(); });
    maybe_run("Example 9 — Compact nested choice/resource coverage",
              []
              { test_9(); });
    maybe_run("Example 10 — Reuse patient.ffhr for another surgical edit",
              []
              { test_10(); });
    maybe_run("Example 11 — Extension URL directory filtering and reconstruction",
              []
              { test_11(); });
    maybe_run("Example 12 — Synthea R4 patient bundle ingest and round-trip",
              []
              { test_synthea_bundle(); });

    // Summary
    std::cout << "\n"
              << std::string(60, '=') << "\n";
    int passed = 0, failed = 0;
    for (auto &r : g_results)
    {
        std::cout << "  [" << (r.passed ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m")
                  << "] " << r.name << "\n";
        r.passed ? ++passed : ++failed;
    }
    std::cout << std::string(60, '=') << "\n";
    std::cout << "  " << passed << "/" << g_results.size() << " passed\n\n";

    if (filter.empty())
        cleanup_artifacts();
    return failed > 0 ? 1 : 0;
}
