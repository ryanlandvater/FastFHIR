/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// AR-4.4: direct tests for FIFO::Queue.
//
// Nothing tested this type until now, which is how a lock-free primitive
// shipped silently dropping 2,000 tasks per affected ingest (TASKS.md AR-3).
// The defect was never visible from the ingestor's side: FF_SUCCESS, no
// warning, a valid but truncated document. It reproduced only under CPU
// contention, so it also could not be found by running the ingestor serially.
//
// Two properties are pinned here, and they are deliberately opposite:
//
//   1. LATCH-BEFORE-PUSH DELIVERS EVERYTHING. The documented convention -- get
//      the Consumer on the spawning thread before the first push, then move it
//      into the worker -- must deliver every item across a node boundary
//      (NODE_ENTRIES = 2000), under contention, with more than one consumer.
//
//   2. THE CANARY CATCHES THE VIOLATION. The chain collapsing when consumers
//      leave is by design ("no consumers, no reason to live"), so the type
//      cannot prevent the misuse -- which is exactly why the debug canary in
//      ~Node exists. A node freed holding ENTRY_PENDING work must throw rather
//      than silently discard, because silent discard is the original bug.
//
// Test 2 is the one that matters: a canary nobody has seen fire is a canary
// nobody knows is alive.

#include "FF_Queue.hpp"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace FastFHIR;

static int failures = 0;
static void CHECK(bool ok, const char* what) {
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

// Comfortably past one node so the producer must allocate and link a second.
static constexpr uint32_t ITEMS = 5000;

struct Task { uint32_t id; };

// ── 1. The documented ordering delivers every item ──────────────────────────
static void test_latch_before_push_delivers_all(unsigned n_consumers) {
    FIFO::Queue<Task, 256> q;

    // Consumers FIRST, on this thread -- the contract. Each pins the head node,
    // so nothing can retire out from under a worker that has not started yet.
    std::vector<FIFO::Queue<Task, 256>::Consumer> consumers;
    consumers.reserve(n_consumers);
    for (unsigned i = 0; i < n_consumers; ++i)
        consumers.emplace_back(q.get_consumer());

    std::atomic<uint32_t> seen{0};
    std::atomic<bool> done{false};
    std::vector<std::thread> workers;

    for (unsigned i = 0; i < n_consumers; ++i) {
        workers.emplace_back([c = std::move(consumers[i]), &seen, &done]() mutable {
            Task t;
            while (true) {
                if (c.pop(t)) { seen.fetch_add(1, std::memory_order_relaxed); continue; }
                if (done.load(std::memory_order_acquire) && c.at_end()) break;
                std::this_thread::yield();
            }
        });
    }

    {
        auto injector = q.get_injector();
        for (uint32_t i = 0; i < ITEMS; ++i) injector.push(Task{i});
    }
    done.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();

    const uint32_t got = seen.load();
    CHECK(got == ITEMS,
          (std::to_string(n_consumers) + " consumer(s): all " +
           std::to_string(ITEMS) + " delivered (got " + std::to_string(got) + ")").c_str());
}

// ── 2. A LATE consumer loses the nodes retired before it existed ────────────
// The exact AR-3 shape: the producer fills and advances past node 0 before any
// consumer latches, so the head moves on and the late consumer starts
// mid-stream. This is the misuse the convention exists to prevent and the
// canary exists to make audible; it is asserted here rather than described, so
// the guarantee in test 1 has a demonstrated counterpart.
//
// The queue cannot prevent it -- the chain collapsing when no consumer is
// present is deliberate ("no consumers, no reason to live"). What must NOT
// happen is losing the work silently: a debug build has to report it. The
// report is the queue's debug_violations() counter, not a throw: a throw from
// ~Node runs inside the retire path after the slot was exchanged, stranding
// the caller's NodeRef on a dead slot (the "Double-free detected" chain below)
// and terminating inside the noexcept ~NodeRef on every destructor route --
// so the counter is the only reliable report.
static void test_late_consumer_is_not_silent() {
    uint32_t delivered = 0;
    FIFO::Queue<Task, 256> q;
    {
        auto injector = q.get_injector();
        for (uint32_t i = 0; i < ITEMS; ++i) injector.push(Task{i});
    }
    // Latching only now: everything retired before this point is gone.
    auto consumer = q.get_consumer();
    Task t;
    while (consumer.pop(t)) ++delivered;

    printf("    late consumer saw %u of %u items, debug_violations=%u\n",
           delivered, ITEMS, q.debug_violations());
#if FASTFHIR_DEBUG
    // The work must be delivered in full or the loss must be recorded.
    CHECK(q.debug_violations() > 0,
          "late-consumer loss recorded by the canary");
#else
    CHECK(true, "release: not asserted (guards compiled out by design)");
#endif
}

int main() {
#if FASTFHIR_DEBUG
    printf("FIFO::Queue tests (FASTFHIR_DEBUG on -- canary active)\n");
#else
    printf("FIFO::Queue tests (release -- canary compiled out)\n");
#endif
    test_latch_before_push_delivers_all(1);
    test_latch_before_push_delivers_all(4);
    test_late_consumer_is_not_silent(); // no longer aborts: the canary records

    printf("%s\n", failures ? "FAILURES" : "all queue checks pass");
    return failures ? 1 : 0;
}
