/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FF_UrlDirectory.hpp"

#include "FF_Ops.hpp"        // SIZE_FF_STRING / STORE_FF_STRING / STORE_U*
#include "FF_SIMD.hpp"       // ff_match_mask_u64x8: 8-lane child-hash compare
#include "FF_Utilities.hpp"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace FastFHIR
{
namespace
{

// The trie below is the one FF_PredigestExtensionURLs built inline. It is moved
// here verbatim, minus the producer/consumer plumbing that stayed in the
// ingestor: the queue funnels URLs from many scan threads to the one consumer
// that calls intern(), which is unchanged.

constexpr uint32_t TRIE_FANOUT = 8;
constexpr uint32_t TRIE_NULL = 0xFFFFFFFFu;

constexpr uint64_t FNV1A_OFFSET = 14695981039346656037ULL;
constexpr uint64_t FNV1A_PRIME = 1099511628211ULL;

[[nodiscard]] inline uint64_t fnv1a(std::string_view s) noexcept
{
    uint64_t h = FNV1A_OFFSET;
    for (const unsigned char c : s)
    {
        h ^= c;
        h *= FNV1A_PRIME;
    }
    return h;
}

/// One node in the radix trie. FANOUT=8 inline child slots; overflow nodes
/// (overflow_idx != TRIE_NULL) extend the child array.
struct TrieNode
{
    uint32_t entry_idx = TRIE_NULL;          // row in `entries`; TRIE_NULL = interior
    Offset   seg_arena_off = FF_NULL_OFFSET; // arena offset of this segment's FF_STRING
    uint32_t overflow_idx = TRIE_NULL;       // next overflow node; TRIE_NULL = none
    uint16_t child_count = 0;                // populated children in THIS node (<= FANOUT)
    uint64_t child_hashes[TRIE_FANOUT] = {}; // FNV-1a hash of each child's segment
    uint32_t child_idx[TRIE_FANOUT] = {};    // index in `nodes`; TRIE_NULL = unused
};

/// Flat row per trie node — the prior-chain data written into FF_URL_DIRECTORY.
struct TrieEntry
{
    uint32_t prior;   // parent's entry_idx, or FF_URL_DIRECTORY::NO_PRIOR for roots
    Offset   seg_off; // arena offset of this segment's FF_STRING (FF_NULL_OFFSET for "")
};

/// Search @p start_idx and its overflow chain for a child whose hash matches
/// AND whose stored bytes match. Returns the child node index, or TRIE_NULL.
[[nodiscard]] uint32_t trie_find_child(const std::vector<TrieNode> &nodes, uint32_t start_idx,
                                       uint64_t seg_hash, std::string_view seg,
                                       const uint8_t *base) noexcept
{
    uint32_t nidx = start_idx;
    while (nidx != TRIE_NULL)
    {
        const TrieNode &n = nodes[nidx];

        // All eight lanes are always compared: fixed latency beats branching on
        // child_count, and valid_mask zeroes the uninitialised slots so a
        // spurious hash match never survives to the memcmp.
        const uint8_t valid_mask = (n.child_count >= TRIE_FANOUT)
                                       ? static_cast<uint8_t>(0xFFu)
                                       : static_cast<uint8_t>((1u << n.child_count) - 1u);
        uint8_t mask = ff_match_mask_u64x8(n.child_hashes, seg_hash) & valid_mask;

        while (mask != 0)
        {
            const uint32_t i = static_cast<uint32_t>(__builtin_ctz(mask));
            mask &= mask - 1u;
            const uint32_t cid = n.child_idx[i];
            const TrieNode &child = nodes[cid];

            if (child.seg_arena_off == FF_NULL_OFFSET)
            {
                if (seg.empty()) return cid;  // the empty segment ("//")
                continue;
            }
            const uint32_t slen = FF_GET_STRING_LENGTH(base, child.seg_arena_off);
            if (slen == static_cast<uint32_t>(seg.size()))
            {
                const char *sdata = reinterpret_cast<const char *>(
                    base + child.seg_arena_off + FF_STRING::STRING_DATA);
                if (std::memcmp(sdata, seg.data(), slen) == 0) return cid;
            }
        }
        nidx = n.overflow_idx;
    }
    return TRIE_NULL;
}

/// Append (seg_hash, child_node_idx) to the node's child list, creating overflow
/// nodes as needed. Index-based, so a push_back that reallocates is safe.
void trie_add_child(std::vector<TrieNode> &nodes, uint32_t node_idx, uint64_t seg_hash,
                    uint32_t child_node_idx)
{
    while (true)
    {
        if (nodes[node_idx].child_count < TRIE_FANOUT)
        {
            const uint16_t cnt = nodes[node_idx].child_count;
            nodes[node_idx].child_hashes[cnt] = seg_hash;
            nodes[node_idx].child_idx[cnt] = child_node_idx;
            nodes[node_idx].child_count++;
            return;
        }
        if (nodes[node_idx].overflow_idx == TRIE_NULL)
        {
            const uint32_t ov_idx = static_cast<uint32_t>(nodes.size());
            nodes.push_back({});
            nodes[node_idx].overflow_idx = ov_idx;
        }
        node_idx = nodes[node_idx].overflow_idx;
    }
}

/// Find or create the child for @p seg under @p parent_node_idx. @p
/// parent_entry_idx is the prior_idx to record on any new entry.
uint32_t trie_get_or_create_child(std::vector<TrieNode> &nodes, std::vector<TrieEntry> &entries,
                                  const Memory &mem, uint8_t *base, uint32_t parent_node_idx,
                                  uint32_t parent_entry_idx, std::string_view seg,
                                  uint64_t seg_hash)
{
    if (const uint32_t found = trie_find_child(nodes, parent_node_idx, seg_hash, seg, base);
        found != TRIE_NULL)
        return found;

    // Empty segments (from "http://…") use FF_NULL_OFFSET so no bytes are
    // written; seg_string() answers "" for them.
    Offset seg_off = FF_NULL_OFFSET;
    if (!seg.empty())
    {
        seg_off = mem->claim_space(SIZE_FF_STRING(seg));
        STORE_FF_STRING(base, seg_off, seg);
    }

    const uint32_t new_entry_idx = static_cast<uint32_t>(entries.size());
    entries.push_back({parent_entry_idx, seg_off});

    TrieNode new_node;
    new_node.entry_idx       = new_entry_idx;
    new_node.seg_arena_off   = seg_off;
    const uint32_t new_node_idx = static_cast<uint32_t>(nodes.size());
    nodes.push_back(new_node);

    trie_add_child(nodes, parent_node_idx, seg_hash, new_node_idx);
    return new_node_idx;
}

/// Insert every '/' segment of @p url. Returns the leaf's entry_idx.
uint32_t insert_url_to_trie(std::vector<TrieNode> &nodes, std::vector<TrieEntry> &entries,
                            const Memory &mem, uint8_t *base, std::string_view url)
{
    uint32_t node_idx  = 0;          // virtual root: no segment, no entry
    uint32_t entry_idx = TRIE_NULL;  // NO_PRIOR for root-level children

    std::size_t pos = 0;
    while (true)
    {
        const std::size_t next = url.find('/', pos);
        const bool        last = (next == std::string_view::npos);
        const std::string_view seg = url.substr(pos, last ? std::string_view::npos : next - pos);

        node_idx = trie_get_or_create_child(nodes, entries, mem, base, node_idx, entry_idx, seg,
                                            fnv1a(seg));
        entry_idx = nodes[node_idx].entry_idx;

        if (last) break;
        pos = next + 1;
    }
    return nodes[node_idx].entry_idx;
}

// Transparent hash/equal so the retrieve map can be probed with a string_view
// without building a std::string.
struct UrlHash
{
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept
    {
        return std::hash<std::string_view>{}(sv);
    }
};
struct UrlEqual
{
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};

} // namespace

struct UrlDirectory::Impl
{
    std::unordered_map<std::string, uint32_t, UrlHash, UrlEqual> retrieve;
    std::vector<TrieNode>  nodes;    // scratch trie (not arena-backed)
    std::vector<TrieEntry> entries;  // parallel to the FF_URL_DIRECTORY entry table

    Impl() { nodes.push_back({}); }  // virtual root at index 0
};

UrlDirectory::UrlDirectory() : m_impl(std::make_unique<Impl>()) {}
UrlDirectory::~UrlDirectory() = default;
UrlDirectory::UrlDirectory(UrlDirectory &&) noexcept = default;
UrlDirectory &UrlDirectory::operator=(UrlDirectory &&) noexcept = default;

uint32_t UrlDirectory::intern(std::string_view url, const Memory &mem, BYTE *base, bool suppress)
{
    if (url.empty()) return FF_NULL_UINT32;

    // Idempotent: a URL already recorded keeps its ref, which is what lets the
    // ingestor's producer-side dedup stay a pure optimisation.
    if (const auto it = m_impl->retrieve.find(url); it != m_impl->retrieve.end())
        return it->second;

    if (suppress)
    {
        m_impl->retrieve.emplace(std::string(url), FF_NULL_UINT32);
        return FF_NULL_UINT32;
    }

    const uint32_t idx = insert_url_to_trie(m_impl->nodes, m_impl->entries, mem, base, url);
    m_impl->retrieve.emplace(std::string(url), idx);
    return idx;
}

void UrlDirectory::assign(std::string_view url, uint32_t ref)
{
    if (url.empty()) return;
    m_impl->retrieve.insert_or_assign(std::string(url), ref);
}

bool UrlDirectory::contains(std::string_view url) const noexcept
{
    return m_impl->retrieve.contains(url);
}

uint32_t UrlDirectory::lookup(std::string_view url) const noexcept
{
    const auto it = m_impl->retrieve.find(url);
    return it != m_impl->retrieve.end() ? it->second : FF_EXT_REF_NULL;
}

std::size_t UrlDirectory::size() const noexcept { return m_impl->entries.size(); }
bool UrlDirectory::empty() const noexcept { return m_impl->entries.empty(); }

void UrlDirectory::load(const FF_URL_DIRECTORY &dir, const BYTE *base)
{
    const uint32_t n = dir.entry_count(base);
    if (n == 0) return;

    m_impl->nodes.clear();
    m_impl->nodes.push_back({});            // virtual root
    m_impl->nodes.resize(n + 1);            // node index = entry index + 1
    m_impl->entries.assign(n, {});

    for (uint32_t i = 0; i < n; ++i)
    {
        // Entries are written in creation order, so a parent's entry index is
        // below its children's and re-linking in order needs no second pass.
        const Offset   seg_off = dir.seg_offset(base, i);
        const uint32_t prior   = dir.prior_idx(base, i);
        const std::string_view seg = dir.seg_string(base, i);

        TrieNode &node    = m_impl->nodes[i + 1];
        node.entry_idx    = i;
        node.seg_arena_off = seg_off;
        m_impl->entries[i] = {prior, seg_off};

        const uint32_t parent_node = (prior == FF_URL_DIRECTORY::NO_PRIOR) ? 0u : prior + 1;
        trie_add_child(m_impl->nodes, parent_node, fnv1a(seg), i + 1);
    }

    for (uint32_t i = 0; i < n; ++i)
        m_impl->retrieve.emplace(dir.get_url(base, i), i);
}

Offset UrlDirectory::write(const Memory &mem, BYTE *base) const
{
    const uint32_t n = static_cast<uint32_t>(m_impl->entries.size());
    if (n == 0) return FF_NULL_OFFSET;

    const Size total = FF_URL_DIRECTORY::HEADER_SIZE +
                       static_cast<Size>(n) * FF_URL_DIRECTORY::URL_ENTRY_SIZE;
    const Offset off = mem->claim_space(total);
    BYTE *const  p   = base + off;

    STORE_U64(p + FF_URL_DIRECTORY::VALIDATION, off);
    STORE_U16(p + FF_URL_DIRECTORY::RECOVERY, RECOVER_FF_URL_DIRECTORY);
    STORE_U16(p + FF_URL_DIRECTORY::PAD, 0);
    STORE_U32(p + FF_URL_DIRECTORY::ENTRY_COUNT, n);

    BYTE *const table = p + FF_URL_DIRECTORY::HEADER_SIZE;
    for (uint32_t i = 0; i < n; ++i)
    {
        BYTE *const ep = table + static_cast<Size>(i) * FF_URL_DIRECTORY::URL_ENTRY_SIZE;
        STORE_U32(ep + FF_URL_DIRECTORY::URL_ENTRY_PRIOR_IDX, m_impl->entries[i].prior);
        STORE_U32(ep + FF_URL_DIRECTORY::URL_ENTRY_PAD, 0);
        STORE_U64(ep + FF_URL_DIRECTORY::URL_ENTRY_SEG_OFFSET, m_impl->entries[i].seg_off);
    }
    return off;
}

void UrlDirectory::for_each(const std::function<void(std::string_view, uint32_t)> &fn) const
{
    for (const auto &[url, ref] : m_impl->retrieve)
        fn(static_cast<std::string_view>(url), ref);
}

} // namespace FastFHIR
