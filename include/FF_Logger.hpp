/**
 * @file FF_Logger.hpp
 * @author Ryan Landvater
 * @brief Lock-free concurrent text buffer for ingestion warnings.
 */
#pragma once

#include <atomic>
#include <memory>
#include <string_view>
#include <cstring>
#include <iostream>

namespace FastFHIR {

class ConcurrentLogger {
    std::unique_ptr<char[]> m_buffer;
    size_t m_capacity;
    std::atomic<size_t> m_head{0};

public:
    // Default to a 64MB log buffer (enough for millions of warnings)
    explicit ConcurrentLogger(size_t capacity = 64 * 1024 * 1024) 
        : m_buffer(std::make_unique<char[]>(capacity)), m_capacity(capacity) {}

    /**
     * @brief Lock-free O(1) log reservation and write.
     */
    void log(std::string_view msg) {
        size_t len = msg.size() + 1; // +1 for the newline character
        
        // 1. Lock-free reservation (1 CPU cycle)
        size_t offset = m_head.fetch_add(len, std::memory_order_relaxed);

        // 2. Safe memory copy
        if (offset + len <= m_capacity) {
            std::memcpy(m_buffer.get() + offset, msg.data(), msg.size());
            m_buffer[offset + msg.size()] = '\n';
        }
    }

    /**
     * @brief Extracts all logs as a single standard string.
     */
    std::string to_string() const {
        size_t total = m_head.load(std::memory_order_acquire);
        if (total == 0) return "";
        
        size_t safe_total = std::min(total, m_capacity);
        std::string result(m_buffer.get(), safe_total);
        
        if (total > m_capacity) {
            result += "\n[FastFHIR] Warning: Logger capacity exceeded. " 
                      + std::to_string(total - m_capacity) + " bytes truncated.\n";
        }
        return result;
    }

    /**
     * @brief Prints all collected logs safely.
     * @note Must only be called after all worker threads have joined.
     */
    void flush_to(std::ostream& out) const {
        size_t total = m_head.load(std::memory_order_acquire);
        if (total > 0) {
            size_t safe_total = std::min(total, m_capacity);
            out.write(m_buffer.get(), safe_total);
            if (total > m_capacity) {
                out << "\n[FastFHIR] Warning: Logger capacity exceeded. " 
                    << (total - m_capacity) << " bytes truncated.\n";
            }
        }
    }

    bool has_logs() const {
        return m_head.load(std::memory_order_relaxed) > 0;
    }

    /**
     * @brief Lock-free log clearing.
     */
    void clear() {
        m_head.store(0, std::memory_order_release);
    }
};

} // namespace FastFHIR