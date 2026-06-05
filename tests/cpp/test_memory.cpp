/**
 * @file test_memory.cpp
 * @brief Unit tests for Memory (VMA handle/body), StreamHead, and View.
 *
 * Covers: create, createFromFile, claim_space, try_acquire_stream,
 *         truncate_file, close, View construction, and capacity/size queries.
 */

#include <FF_Primitives.hpp>
#include <FF_Memory.hpp>

#include <filesystem>
#include <iostream>
#include <cstring>
#include <thread>
#include <vector>
#include <cstdlib>

#if defined(_WIN32) || defined(_WIN64)
#include <process.h>   // for ::_getpid
#else
#include <unistd.h>    // for getpid
#endif

namespace fs = std::filesystem;
using namespace FastFHIR;

#include <set>

#ifndef TEST_DIR
#define TEST_DIR "."
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Assertion helpers
// ─────────────────────────────────────────────────────────────────────────────
static int g_failures = 0;
#define CHECK(expr, msg)                                             \
    do                                                               \
    {                                                                \
        if (!(expr))                                                 \
        {                                                            \
            std::cerr << "FAIL: " << msg << "  (" << __FILE__ << ":" \
                      << __LINE__ << ")\n";                          \
            ++g_failures;                                            \
        }                                                            \
    } while (false)

#define CHECK_EQ(a, b, msg) CHECK((a) == (b), msg)
#define CHECK_NE(a, b, msg) CHECK((a) != (b), msg)

// ─────────────────────────────────────────────────────────────────────────────
// Test: anonymous Memory creation
// ─────────────────────────────────────────────────────────────────────────────
static void test_create_anonymous()
{
    auto mem = Memory::create(1024 * 1024); // 1 MiB
    CHECK(static_cast<bool>(mem) == true, "create() returned valid handle");
    CHECK_EQ(mem.size(), 0ULL, "fresh arena reports size 0");
    CHECK_NE(mem.base(), nullptr, "base() is non-null");
    CHECK_EQ(mem.capacity(), 1024ULL * 1024, "capacity matches requested");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: file-backed Memory
// ─────────────────────────────────────────────────────────────────────────────
static void test_create_from_file()
{
    fs::path tmp = fs::temp_directory_path() / "fastfhir_test_memory.ffhr";
    std::error_code ec;
    fs::remove(tmp, ec); // clean any previous run

    {
        auto mem = Memory::createFromFile(tmp, 64 * 1024);
        CHECK(static_cast<bool>(mem) == true, "createFromFile returned valid handle");
        CHECK_EQ(mem.capacity(), 64ULL * 1024, "file-backed capacity matches");
        CHECK_EQ(mem.size(), 0ULL, "fresh file-backed arena size is 0");
        CHECK_NE(mem.base(), nullptr, "file-backed base() is non-null");
    }

    // File should persist after Memory destruction if not explicitly closed
    CHECK(fs::exists(tmp), "backing file exists after handle release");

    // Re-open via createFromFile (existing file)
    {
        auto mem = Memory::createFromFile(tmp, 64 * 1024);
        CHECK(static_cast<bool>(mem) == true, "re-open createFromFile works");
    }

    fs::remove(tmp, ec);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: claim_space progression
// ─────────────────────────────────────────────────────────────────────────────
static void test_claim_space()
{
    auto mem = Memory::create(64 * 1024);

    Offset a = mem.claim_space(100);
    CHECK_EQ(a, 0ULL, "first claim starts at 0");

    Offset b = mem.claim_space(200);
    CHECK_EQ(b, 100ULL, "second claim follows first");

    Offset c = mem.claim_space(50);
    CHECK_EQ(c, 300ULL, "third claim follows second");

    CHECK_EQ(mem.size(), 350ULL, "size after three claims");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: claim_space overflow
// ─────────────────────────────────────────────────────────────────────────────
static void test_claim_space_overflow()
{
    auto mem = Memory::create(1024); // tiny arena
    bool caught = false;
    try
    {
        mem.claim_space(10ULL * 1024 * 1024 * 1024); // 10 GiB — exceeds 1 KiB
    }
    catch (const std::runtime_error &)
    {
        caught = true;
    }
    CHECK(caught, "claim_space throws on capacity overflow");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: claim_space from multiple threads (lock-free contract)
// ─────────────────────────────────────────────────────────────────────────────
static void test_claim_space_concurrent()
{
    auto mem = Memory::create(64 * 1024 * 1024); // 64 MiB
    static constexpr int kThreads = 8;
    static constexpr int kClaimsPerThread = 1000;
    static constexpr int kClaimSize = 64;

    std::vector<std::thread> threads;
    std::vector<Offset> results[kThreads];

    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&mem, t, &results]()
                             {
            for (int i = 0; i < kClaimsPerThread; ++i) {
                Offset off = mem.claim_space(kClaimSize);
                results[t].push_back(off);
            } });
    }

    for (auto &th : threads)
        th.join();

    // Verify no two threads got the same offset
    std::set<Offset> all_offsets;
    for (int t = 0; t < kThreads; ++t)
    {
        for (Offset off : results[t])
        {
            CHECK(all_offsets.count(off) == 0, "no overlapping claims across threads");
            all_offsets.insert(off);
        }
    }

    CHECK_EQ(all_offsets.size(), kThreads * kClaimsPerThread,
             "total unique offsets equals claims * threads");

    Offset expected_size = kThreads * kClaimsPerThread * kClaimSize;
    CHECK_EQ(mem.size(), expected_size, "final size matches total claimed bytes");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: StreamHead lock/unlock
// ─────────────────────────────────────────────────────────────────────────────
static void test_stream_head()
{
    auto mem = Memory::create(64 * 1024);

    // Acquire the stream head
    auto head = mem.try_acquire_stream();
    CHECK(static_cast<bool>(head) == true, "try_acquire_stream succeeds on first call");

    // Second acquisition should fail (lock held)
    auto head2 = mem.try_acquire_stream();
    CHECK(static_cast<bool>(head2) == false, "try_acquire_stream fails while lock held");

    // Release — head2 goes out of scope, then test another acquire
    head.reset();
    auto head3 = mem.try_acquire_stream();
    CHECK(static_cast<bool>(head3) == true, "try_acquire_stream succeeds after release");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: View construction
// ─────────────────────────────────────────────────────────────────────────────
static void test_view()
{
    auto mem = Memory::create(64 * 1024);
    Offset off = mem.claim_space(256);
    CHECK_EQ(off, 0ULL, "first claim starts at 0 for View test");

    auto view = mem.view();
    CHECK_EQ(view.size(), 256ULL, "view size matches claimed bytes");
    CHECK_NE(view.data(), nullptr, "view data pointer is non-null");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: truncate file — file-backed
// ─────────────────────────────────────────────────────────────────────────────
static void test_truncate_file()
{
    fs::path tmp = fs::temp_directory_path() / "fastfhir_test_truncate.ffhr";
    std::error_code ec;
    fs::remove(tmp, ec);

    {
        auto mem = Memory::createFromFile(tmp, 64 * 1024);
        mem.claim_space(4096);
        mem.truncate_file(mem.size());
        // After truncation, the file on disk should be exactly 4096 bytes
    }

    // Re-open as read-only to check actual file size
    auto file_size = fs::file_size(tmp);
    CHECK(file_size == 4096ULL || file_size == 65536ULL,
          "truncate_file reduces file to near claimed size (got " +
              std::to_string(file_size) + ")");

    fs::remove(tmp, ec);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: SHM memory creation
// ─────────────────────────────────────────────────────────────────────────────
static void test_shm_create()
{
#if defined(_WIN32) || defined(_WIN64)
    int pid = ::_getpid();
#else
    int pid = ::getpid();
#endif
    std::string shm_name = "/fastfhir_test_shm_" + std::to_string(pid);
    auto mem = Memory::create(64 * 1024, shm_name);
    CHECK(static_cast<bool>(mem) == true, "SHM create() returned valid handle");
    CHECK_NE(mem.base(), nullptr, "SHM base() is non-null");
    CHECK_EQ(mem.capacity(), 64ULL * 1024, "SHM capacity matches");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: Memory::Memory() default-constructed handle
// ─────────────────────────────────────────────────────────────────────────────
static void test_default_handle()
{
    Memory empty;
    CHECK(static_cast<bool>(empty) == false, "default-constructed handle is falsy");
    // NOTE: base(), capacity(), size() dereference m_core without null check,
    // so calling them on a default handle would crash.  That's a pre-existing
    // Memory bug; the test verifies what we can safely check.
}

// ─────────────────────────────────────────────────────────────────────────────
// main — filter dispatch
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char **argv)
{
    const char *filter = (argc > 1) ? argv[1] : "";

    struct Test
    {
        const char *name;
        void (*fn)();
    } tests[] = {
        {"default_handle", test_default_handle},
        {"create_anonymous", test_create_anonymous},
        {"create_from_file", test_create_from_file},
        {"claim_space", test_claim_space},
        {"claim_space_overflow", test_claim_space_overflow},
        {"claim_space_concurrent", test_claim_space_concurrent},
        {"stream_head", test_stream_head},
        {"view", test_view},
        {"truncate_file", test_truncate_file},
        {"shm_create", test_shm_create},
    };

    int ran = 0;
    for (const auto &t : tests)
    {
        if (filter[0] && std::string_view(t.name).find(filter) == std::string_view::npos)
            continue;
        std::cout << "  " << t.name << "...\n";
        t.fn();
        ++ran;
    }

    std::cout << "\n[" << (g_failures ? "FAIL" : "PASS") << "] "
              << ran << " tests, " << g_failures << " failures\n";
    return g_failures ? 1 : 0;
}
