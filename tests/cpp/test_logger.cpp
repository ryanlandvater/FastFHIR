/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// LOG-1: direct tests for ConcurrentLogger.
//
// The logger is the conformance layer's diagnostic sink (Block K, decision D4)
// and the Ingestor's warning buffer, so many ingest workers write into one
// instance at once. It used to claim space with fetch_add, which cannot refuse
// a claim: an entry that overflowed moved the head past capacity anyway, and
// the never-written bytes between its offset and the end of the buffer came
// back from to_string() as NULs. It now claims by CAS and refuses an entry that
// does not fit, without moving the head.
//
// Two shapes of test, for two different questions:
//
//   1-3. Boundaries, single-threaded and exact. A refused claim must leave the
//        head where it was -- so a SHORTER entry after it still fits -- and an
//        entry that fits to the last byte must be accepted.
//   4.   Contention. Eight writers overflow one buffer partway through. Every
//        line read back must be one complete entry, exactly once, with no NUL
//        bytes, and every attempt must be accounted for:
//        lines + dropped == attempts. Run this binary under -fsanitize=thread;
//        a race here is a LOG-1 regression.

#include "FF_Logger.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "FFHR_tests.hpp"

using namespace FastFHIR;

namespace
{

/// The lines a logger holds, without the overflow warning it appends.
std::vector<std::string> entries_of(const std::string& text)
{
    std::vector<std::string> lines;
    for (size_t pos = 0; pos < text.size();)
    {
        const size_t eol  = text.find('\n', pos);
        const size_t end  = (eol == std::string::npos) ? text.size() : eol;
        std::string  line = text.substr(pos, end - pos);
        if (line.rfind("[Warning] FastFHIR: logger capacity", 0) != 0)
            lines.push_back(std::move(line));
        if (eol == std::string::npos)
            break;
        pos = eol + 1;
    }
    return lines;
}

// ── 1. A refused claim does not move the head ─────────────────────────────
void refused_claim_leaves_the_head()
{
    TEST_GROUP("refusal");
    ConcurrentLogger logger(16);

    logger.log("0123456");    // 8 bytes  -> head 8
    logger.log("abcdefghij"); // 11 bytes -> 19 > 16, refused
    CHECK_EQ(logger.dropped(), 1u, "an entry that does not fit is refused and counted");

    logger.log("xyz");        // 4 bytes  -> head 12: fits because the refusal moved nothing
    logger.log("0123");       // 5 bytes  -> 17 > 16, refused
    logger.log("abc");        // 4 bytes  -> head 16: fits to the last byte
    CHECK_EQ(logger.dropped(), 2u, "the second overflow is counted too");

    const std::string text = logger.to_string();
    CHECK(text.find('\0') == std::string::npos, "no never-written byte reaches a reader");
    CHECK(text.rfind("0123456\nxyz\nabc\n", 0) == 0,
          "accepted entries are intact and nothing else precedes the warning: " << text);
    CHECK(text.find("[Warning] FastFHIR: logger capacity of 16 bytes exhausted; 2 entries were dropped.")
              != std::string::npos,
          "the overflow is reported as a [Warning] the Ingestor lifts: " << text);
}

// ── 2. Degenerate sizes ───────────────────────────────────────────────────
void degenerate_sizes()
{
    TEST_GROUP("sizes");
    ConcurrentLogger tiny(4);
    tiny.log("much longer than the buffer");
    CHECK_EQ(tiny.dropped(), 1u, "an entry longer than the whole buffer is refused, not truncated");
    CHECK(entries_of(tiny.to_string()).empty(), "and nothing of it is stored");

    tiny.log("");
    CHECK_EQ(entries_of(tiny.to_string()).size(), 1u, "an empty entry still claims its newline");

    ConcurrentLogger none(0);
    none.log("x");
    CHECK_EQ(none.dropped(), 1u, "a zero-capacity logger refuses everything");
    CHECK(none.has_logs(), "has_logs() is true when entries were dropped, so a drop is never silent");
}

// ── 3. clear() resets both counters ───────────────────────────────────────
void clear_resets_everything()
{
    TEST_GROUP("clear");
    ConcurrentLogger logger(8);
    logger.log("1234567");
    logger.log("overflow");
    REQUIRE(logger.dropped() == 1u, "precondition: one drop");

    logger.clear();
    CHECK(!logger.has_logs(), "clear() empties the log");
    CHECK_EQ(logger.dropped(), 0u, "clear() resets the dropped count");
    CHECK(logger.to_string().empty(), "no stale warning after clear()");

    logger.log("1234567");
    CHECK_EQ(logger.to_string(), std::string("1234567\n"), "the full capacity is available again");
}

// ── 4. Many writers, one buffer, overflow partway ─────────────────────────
constexpr unsigned THREADS = 8;
constexpr unsigned PER_THREAD = 20000;
constexpr size_t   ENTRY_WIDTH = 24; // "tNN-NNNNNN-" + 13 payload chars

/// A fixed-width entry whose payload is derived from its identity, so a torn
/// or interleaved line cannot parse as a valid one.
std::string entry(unsigned thread, unsigned index)
{
    char head[16];
    std::snprintf(head, sizeof head, "t%02u-%06u-", thread, index);
    std::string line(head);
    line.append(ENTRY_WIDTH - line.size(), static_cast<char>('a' + thread));
    return line;
}

void concurrent_writers_overflow_cleanly()
{
    TEST_GROUP("contention");
    const size_t attempts = size_t{THREADS} * PER_THREAD;
    // Room for about half the entries, so the overflow happens under contention.
    ConcurrentLogger logger((ENTRY_WIDTH + 1) * attempts / 2 + 7);

    std::atomic<bool>        go{false};
    std::vector<std::thread> writers;
    for (unsigned t = 0; t < THREADS; ++t)
    {
        writers.emplace_back([t, &logger, &go] {
            go.wait(false, std::memory_order_acquire);
            for (unsigned i = 0; i < PER_THREAD; ++i)
                logger.log(entry(t, i));
        });
    }
    go.store(true, std::memory_order_release);
    go.notify_all();
    for (std::thread& w : writers)
        w.join();

    const std::string text = logger.to_string();
    CHECK(text.find('\0') == std::string::npos, "no NUL byte anywhere in the output");

    const std::vector<std::string> lines = entries_of(text);
    std::vector<bool>              seen(attempts, false);
    size_t                         malformed = 0, duplicates = 0;
    for (const std::string& line : lines)
    {
        unsigned t = 0, i = 0;
        if (line.size() != ENTRY_WIDTH || std::sscanf(line.c_str(), "t%2u-%6u-", &t, &i) != 2 ||
            t >= THREADS || i >= PER_THREAD || line != entry(t, i))
        {
            ++malformed;
            continue;
        }
        const size_t id = size_t{t} * PER_THREAD + i;
        if (seen[id])
            ++duplicates;
        seen[id] = true;
    }

    CHECK_EQ(malformed, 0u, "every line is one complete entry");
    CHECK_EQ(duplicates, 0u, "no entry appears twice");
    CHECK(!lines.empty() && logger.dropped() > 0,
          "the buffer overflowed partway (" << lines.size() << " stored, "
                                           << logger.dropped() << " dropped)");
    CHECK_EQ(lines.size() + logger.dropped(), attempts,
             "every attempt is either stored or counted as dropped");
}

} // namespace

int main(int argc, char** argv)
{
    ff_test::set_filter(argc, argv);
    ff_test::run("refusal", refused_claim_leaves_the_head);
    ff_test::run("sizes", degenerate_sizes);
    ff_test::run("clear", clear_resets_everything);
    ff_test::run("contention", concurrent_writers_overflow_cleanly);
    return ff_test::report("concurrent logger");
}
