/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file FF_Logger.hpp
 * @author Ryan Landvater
 * @brief Lock-free concurrent text buffer for ingestion warnings.
 *
 * Any number of threads drop entries in at once. A writer CLAIMS its range
 * with a compare-and-swap on the head and then copies into it; nothing else
 * is shared, so writers never wait on each other (TASKS.md LOG-1).
 *
 * Why a CAS and not fetch_add. fetch_add cannot refuse a claim: the head moved
 * past capacity before the bounds check ran, so an entry that did not fit left
 * a hole of never-written bytes INSIDE the readable range, and readers returned
 * them as NULs. The CAS computes the end first and refuses without moving the
 * head, so `head <= capacity` always holds and every byte below it belongs to
 * a claimed entry. Refused entries are COUNTED, never silently lost -- and a
 * later, shorter entry may still fit in what is left, so lines are independent
 * and are not guaranteed to appear in claim-attempt order.
 *
 * READ ONLY WHEN QUIESCENT. A claim and its copy are two steps, so a reader
 * running beside a writer can see a range that is claimed but not yet filled.
 * to_string(), flush_to() and clear() are valid only once every writer has
 * stopped (the Ingestor reads after its pool joins). has_logs() and dropped()
 * are safe at any time. No lock is added to make the rest concurrent:
 * CLAUDE.md invariant 6.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace FastFHIR {

class ConcurrentLogger {
    std::unique_ptr<char[]> m_buffer;
    size_t m_capacity;
    std::atomic<size_t> m_head{0};    ///< Claimed bytes. Never exceeds m_capacity.
    std::atomic<size_t> m_dropped{0}; ///< Entries refused for want of space.

public:
    // Default to a 64MB log buffer (enough for millions of warnings)
    explicit ConcurrentLogger(size_t capacity = 64 * 1024 * 1024)
        : m_buffer(std::make_unique<char[]>(capacity)), m_capacity(capacity) {}

    /**
     * @brief Lock-free claim-then-copy of one newline-terminated entry.
     *
     * Safe from any number of threads. An entry that does not fit in the space
     * left is refused whole and counted in dropped(); it is never truncated.
     */
    void log(std::string_view msg) noexcept {
        const size_t len = msg.size() + 1; // +1 for the newline character

        // 1. Claim [offset, offset + len) by CAS. The bound is checked BEFORE
        //    the swap, so a refused claim leaves the head where it was.
        size_t offset = m_head.load(std::memory_order_acquire);
        do {
            if (len > m_capacity - offset) { // offset <= m_capacity: no underflow
                m_dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        } while (!m_head.compare_exchange_weak(offset, offset + len,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire));

        // 2. The range is this thread's alone.
        std::memcpy(m_buffer.get() + offset, msg.data(), msg.size());
        m_buffer[offset + msg.size()] = '\n';
    }

    /**
     * @brief Extracts all logs as a single standard string.
     * @note Valid only when no writer is running (see the file comment).
     */
    std::string to_string() const {
        const size_t total = m_head.load(std::memory_order_acquire);
        std::string result(m_buffer.get(), total);
        if (const size_t dropped = m_dropped.load(std::memory_order_acquire); dropped > 0)
            result += overflow_line(dropped);
        return result;
    }

    /**
     * @brief Prints all collected logs.
     * @note Valid only when no writer is running (see the file comment).
     */
    void flush_to(std::ostream& out) const {
        const size_t total = m_head.load(std::memory_order_acquire);
        out.write(m_buffer.get(), static_cast<std::streamsize>(total));
        if (const size_t dropped = m_dropped.load(std::memory_order_acquire); dropped > 0)
            out << overflow_line(dropped);
    }

    bool has_logs() const noexcept {
        return m_head.load(std::memory_order_relaxed) > 0 ||
               m_dropped.load(std::memory_order_relaxed) > 0;
    }

    /// Entries refused because the buffer was full. Safe at any time.
    size_t dropped() const noexcept {
        return m_dropped.load(std::memory_order_acquire);
    }

    /**
     * @brief Discards every entry and the dropped count.
     * @note Single owner: no writer may be running. A writer that claimed
     *       before the reset would copy into space the next claim is handed
     *       again, interleaving two entries byte for byte.
     */
    void clear() noexcept {
        m_head.store(0, std::memory_order_release);
        m_dropped.store(0, std::memory_order_release);
    }

private:
    /// A [Warning] line, so the Ingestor lifts it into the FF_Result like any
    /// other warning: a full log must not lose diagnostics without saying so.
    std::string overflow_line(size_t dropped) const {
        return "[Warning] FastFHIR: logger capacity of " + std::to_string(m_capacity) +
               " bytes exhausted; " + std::to_string(dropped) +
               (dropped == 1 ? " entry was" : " entries were") + " dropped.\n";
    }
};

} // namespace FastFHIR
