/**
 * @file FF_UrlDirectory.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief The stream's URL intern table — text to a stable index, segments shared by prefix.
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 *
 * INTERNAL. Reachable from `FF_Builder.cpp` and `FF_Ingestor.cpp`, not from
 * `FastFHIR.hpp`: this is build-side machinery the Builder owns, and nothing a
 * consumer names. `FF_Builder.hpp` forward-declares it and holds a
 * `unique_ptr` (pimpl), so the type stays out of the public header.
 *
 * WHAT THIS OWNS, AND WHAT IT USED TO BE. `FF_URL_DIRECTORY` has always been
 * the on-wire table — a 16-byte header plus one 16-byte entry per segment, each
 * entry naming its parent so a reader joins the chain back into a full URL. It
 * was built inline by `FF_PredigestExtensionURLs`, which claimed the whole
 * block up front, before the document body, at a size fixed the moment it was
 * written. That made the table un-growable: a re-opened Builder could not add a
 * URL, and a producer building resources in C++ had no way to obtain a valid
 * `Extension.url` index at all.
 *
 * This class holds the same trie and the same entry list, but writes the BLOCK
 * late (from `finalize`, the way the checksum footer is written), so it can grow
 * until the entry set is complete. The segment `FF_STRING`s are still claimed
 * in the arena during `intern()` — only the header-and-entry-table block moves.
 */
#pragma once

#include "FF_Memory.hpp"
#include "FF_Primitives.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace FastFHIR
{

class FF_EXPORT UrlDirectory
{
public:
    UrlDirectory();
    ~UrlDirectory();
    UrlDirectory(UrlDirectory &&) noexcept;
    UrlDirectory &operator=(UrlDirectory &&) noexcept;
    UrlDirectory(const UrlDirectory &) = delete;
    UrlDirectory &operator=(const UrlDirectory &) = delete;

    /**
     * @brief Intern @p url and return its URL index (an ext_ref with MSB clear).
     *
     * Idempotent: the same URL always returns the same index. The segment
     * strings are claimed in the arena here; the directory block itself is
     * written later, by write().
     *
     * @param suppress Record the URL so a later `contains()` sees it, but give
     *                 it no block — FF_NULL_UINT32 is returned and stored. This
     *                 is the filtered/known-url case the ingestor applies.
     *
     * THREADING. This mutates shared state, so it carries the same contract the
     * ingest path already relied on: call it from ONE thread — the ingest
     * consumer, or a single-threaded direct build. Appends are concurrent; the
     * intern table is not, and never was.
     */
    [[nodiscard]] uint32_t intern(std::string_view url, const Memory &mem, BYTE *base,
                                  bool suppress = false);

    /// Record an already-decided mapping (`url -> ref`) without minting an
    /// index. Used to promote a URL to a module reference once its WASM codec
    /// is cached.
    void assign(std::string_view url, uint32_t ref);

    [[nodiscard]] bool contains(std::string_view url) const noexcept;
    /// The ref for @p url, or FF_EXT_REF_NULL when it was never interned.
    [[nodiscard]] uint32_t lookup(std::string_view url) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    /// Rebuild the table from an existing directory block, so a re-opened
    /// stream keeps its URLs and can add more. The segment strings it points at
    /// are reused rather than re-claimed.
    void load(const FF_URL_DIRECTORY &dir, const BYTE *base);

    /// Claim and write the block. FF_NULL_OFFSET when there is nothing to
    /// write. Called from finalize, once, when the entry set is complete.
    [[nodiscard]] Offset write(const Memory &mem, BYTE *base) const;

    void for_each(const std::function<void(std::string_view, uint32_t)> &fn) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace FastFHIR

/// Global alias, matching the FF_String / FF_BundleIndex convention.
using FF_UrlDirectory = FastFHIR::UrlDirectory;
