/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Shared assertion harness for the standalone C++ tests.
 *
 * Every test in tests/cpp had its own failure counter and its own CHECK, six
 * of which differed only in whitespace and in whether they counted passes.
 * That is one contract implemented nineteen times, so a fix to any of them
 * reached one file.
 *
 * DEPENDENCY-FREE ON PURPOSE. Corpus discovery lives in FFHR_test_corpus.hpp
 * (<filesystem>) and checksums in FFHR_test_checksum.hpp (OpenSSL), so a test
 * that only needs assertions does not pull either in.
 */
#ifndef FFHR_TESTS_HPP
#define FFHR_TESTS_HPP

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace ff_test
{
    inline int g_failures = 0;
    inline int g_checks = 0;   // individual CHECK assertions
    inline int g_cases = 0;    // named cases dispatched through run()
    inline const char *g_group = "";

    // Per-check PASS lines are off by default and restored with
    // FF_TEST_VERBOSE=1. Tests used to disagree about this per file; one
    // switch replaces that, and ctest only surfaces output on failure anyway.
    inline bool verbose()
    {
        static const bool on = std::getenv("FF_TEST_VERBOSE") != nullptr;
        return on;
    }

    inline void group(const char *name)
    {
        g_group = name;
        std::cout << "\n[" << name << "]\n";
    }

    // `--filter <substring>` selects a subset of named cases. Four tests
    // carried the same capture-by-reference lambda for this; one function with
    // a file-static filter replaces it and drops the per-file `filter` local.
    inline const char *g_filter = "";

    inline void set_filter(int argc, char **argv)
    {
        for (int i = 1; i + 1 < argc; ++i)
        {
            if (std::string_view(argv[i]) == "--filter")
            {
                g_filter = argv[i + 1];
                return;
            }
        }
    }

    template <class Fn>
    void run(const char *name, Fn fn)
    {
        if (g_filter[0] == '\0' || std::string_view(name).find(g_filter) != std::string_view::npos)
        {
            ++g_cases;
            fn();
        }
    }

    /// Print the summary and return the process exit code.
    /// @param ok_message printed when every check passed.
    inline int report(const char *ok_message)
    {
        std::cout << "\n";
        if (g_cases)
            std::cout << g_cases << " cases, ";
        std::cout << g_checks << " checks, " << g_failures << " failures\n";
        if (!g_failures)
            std::cout << ok_message << "\n";
        return g_failures ? 1 : 0;
    }
} // namespace ff_test

// `msg` is a STREAM expression, so both CHECK(ok, "text") and
// CHECK(ok, "got " << n) compile. The location is printed because a bare
// message cannot be found in a file that runs a hundred checks.
#define CHECK(cond, msg)                                                        \
    do                                                                          \
    {                                                                           \
        ++::ff_test::g_checks;                                                  \
        if (!(cond))                                                            \
        {                                                                       \
            ++::ff_test::g_failures;                                            \
            std::cerr << "  FAIL " << ::ff_test::g_group << "::" << __func__     \
                      << " line " << __LINE__ << ": " << msg << "\n";           \
        }                                                                       \
        else if (::ff_test::verbose())                                          \
        {                                                                       \
            std::cerr << "  PASS " << ::ff_test::g_group << "::" << __func__     \
                      << " line " << __LINE__ << "\n";                          \
        }                                                                       \
    } while (0)

// Group label for the failure messages; also prints a section header.
#define TEST_GROUP(name) ::ff_test::group(name)

#define CHECK_EQ(a, b, msg) CHECK((a) == (b), msg << " expected " << (b) << " got " << (a))
#define CHECK_NE(a, b, msg) CHECK((a) != (b), msg)

// Abandon the enclosing void function when a precondition fails: the checks
// after it would report cascade failures against state that was never built.
#define REQUIRE(cond, msg)  \
    do                      \
    {                       \
        CHECK(cond, msg);   \
        if (!(cond))        \
            return;         \
    } while (0)

#endif // FFHR_TESTS_HPP
