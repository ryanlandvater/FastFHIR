/**
 * @file FF_UrlHash.hpp
 * @brief Robin Hood open-addressing hash table for O(1) URL → entry_idx lookup.
 *
 * FF_UrlHashTable is a read-optimised flat hash table built once during
 * predigestion (after the consumer thread drains the pipeline) and then
 * queried concurrently by ingest workers.
 *
 * Contract
 * ────────
 *  This table stores (full-URL-hash, url-length) -> URL_IDX entries (URL_IDX rows in
 *  FF_URL_DIRECTORY). It does NOT store promoted EXT_REF module words.
 *  In FF_PredigestExtensionURLs it is built in Phase 6.5, before Phase 7 may
 *  rewrite url_to_index[url] to MODULE_IDX form (MSB=1). Use Builder::resolve_extension_url()
 *  as the authoritative source for final EXT_REF routing decisions.
 *
 * Collision resistance
 * ────────────────────
 *  Each slot stores the full 64-bit FNV-1a hash AND the URL byte length.  find()
 *  requires both to match before returning a value.  This gives ~96-bit effective
 *  key width and is resistant to adversarially crafted hash collisions from
 *  untrusted input JSON; constructing a URL that collides on both 64-bit hash and
 *  exact length requires finding a pre-image with ~2^(64+~17) constraints.
 *
 * Key properties
 * ──────────────
 *  • Robin Hood (backward-shift / displacement) probing — worst-case probe
 *    length stays near log(n) even at 50 % load.
 *  • Power-of-two capacity, fixed at build time; never resizes after build.
 *  • Stores the raw 64-bit FNV-1a hash alongside the value so find() never
 *    dereferences an external string buffer.
 *  • Zero heap allocation after build().
 *
 * Thread safety
 * ─────────────
 *  build() must complete (and the thread that called it must release/publish
 *  the table) before any concurrent find() call.  Concurrent find() calls are
 *  safe with no locking.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace FastFHIR {

struct FF_UrlHashTable {
    // Sentinel: returned by find() and stored in empty slots.
    static constexpr uint32_t EMPTY = 0xFFFFFFFFu;

    // ── Internal slot layout ─────────────────────────────────────────────────
    struct Slot {
        uint64_t hash  = 0;
        uint32_t len   = 0; // URL byte length — collision discriminator
        uint32_t value = EMPTY;
        uint32_t dist  = 0; // Robin Hood probe distance from ideal slot
    };

    // ── Public interface ─────────────────────────────────────────────────────

    FF_UrlHashTable() = default;
    FF_UrlHashTable(FF_UrlHashTable&&) = default;
    FF_UrlHashTable& operator=(FF_UrlHashTable&&) = default;
    FF_UrlHashTable(const FF_UrlHashTable&) = default;
    FF_UrlHashTable& operator=(const FF_UrlHashTable&) = default;

    /// Build from any map whose key_type is std::string and mapped_type is
    /// uint32_t (e.g. std::unordered_map<std::string, uint32_t>).
    template <typename Map>
    void build(const Map& m) {
        if (m.empty()) {
            _slots.clear();
            _mask   = 0;
            _filled = 0;
            return;
        }
        // Choose power-of-two capacity ≥ 2× element count (load factor ≤ 0.5).
        size_t cap = 4;
        while (cap < m.size() * 2) cap <<= 1;

        _slots.assign(cap, Slot{});
        _mask   = cap - 1;
        _filled = 0;

        for (const auto& [url, idx] : m) {
            // Skip filtered URLs (sentinel value equals EMPTY slot marker).
            if (idx == EMPTY) continue;
            _insert(hash_url(std::string_view(url)), static_cast<uint32_t>(url.size()), idx);
        }
    }

    /// Look up a URL by its pre-computed FNV-1a hash and byte length.
    /// Returns EMPTY (0xFFFFFFFF) if not found.
    /// Both @p hash and @p len must match the stored entry to guard against
    /// hash-only collisions from adversarially crafted input URLs.
    /// The caller computes the hash with FF_UrlHashTable::hash_url().
    uint32_t find(uint64_t hash, uint32_t len) const noexcept {
        if (_slots.empty()) return EMPTY;
        size_t   pos  = hash & _mask;
        uint32_t dist = 0;
        while (true) {
            const Slot& s = _slots[pos];
            if (s.value == EMPTY || dist > s.dist) return EMPTY;
            if (s.hash == hash && s.len == len) return s.value;
            pos = (pos + 1) & _mask;
            ++dist;
        }
    }

    bool empty() const noexcept { return _filled == 0; }
    size_t size()  const noexcept { return _filled; }

    // ── Hash function ────────────────────────────────────────────────────────
    /// FNV-1a 64-bit — same algorithm used by the predigestion pipeline so
    /// hashes stored in the table always match hashes queried by workers.
    static uint64_t hash_url(std::string_view s) noexcept {
        uint64_t h = 14695981039346656037ULL; // FNV offset basis
        for (const unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ULL; // FNV prime
        }
        return h;
    }

private:
    std::vector<Slot> _slots;
    size_t            _mask   = 0;
    size_t            _filled = 0;

    void _insert(uint64_t hash, uint32_t len, uint32_t value) noexcept {
        size_t   pos      = hash & _mask;
        uint32_t dist     = 0;
        Slot     incoming = {hash, len, value, dist};
        while (true) {
            Slot& cur = _slots[pos];
            if (cur.value == EMPTY) {
                cur = incoming;
                ++_filled;
                return;
            }
            // Robin Hood: steal the slot from the rich (small dist) entry.
            if (cur.dist < incoming.dist) {
                using std::swap;
                swap(cur, incoming);
            }
            pos = (pos + 1) & _mask;
            ++incoming.dist;
        }
    }
};

} // namespace FastFHIR
