/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Synthea corpus discovery for the standalone C++ tests.
 *
 * find_bundles() was byte-identical in three test files, each with its own
 * copy of the same sort-before-truncate comment.
 *
 * Separate from FFHR_tests.hpp because this needs <filesystem> and a test that
 * builds its documents inline should not pay for it.
 */
#ifndef FFHR_TEST_CORPUS_HPP
#define FFHR_TEST_CORPUS_HPP

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef FASTFHIR_SYNTHEA_DIR
#  define FASTFHIR_SYNTHEA_DIR ""
#endif

namespace ff_test
{
    namespace fs = std::filesystem;

    /// The first `limit` corpus bundles in NAME order; empty when the corpus
    /// is not configured, which callers treat as "skip the corpus gate".
    ///
    /// SORTED BEFORE TRUNCATION. directory_iterator yields in filesystem
    /// order, which is neither sorted nor stable across machines, so
    /// truncating it picks an arbitrary subset and a failure does not
    /// reproduce from the same command on another box. Same reason
    /// ff_test_datetime pins and prints its seed.
    inline std::vector<fs::path> find_bundles(std::size_t limit)
    {
        std::vector<fs::path> out;
        const fs::path root(FASTFHIR_SYNTHEA_DIR);
        if (root.empty())
            return out;
        for (const auto &dir : {root / "fhir", root})
        {
            std::error_code ec;
            if (!fs::is_directory(dir, ec))
                continue;
            for (const auto &entry : fs::directory_iterator(dir, ec))
            {
                if (entry.path().extension() == ".json")
                    out.push_back(entry.path());
            }
            if (!out.empty())
                break;
        }
        std::sort(out.begin(), out.end());
        if (out.size() > limit)
            out.resize(limit);
        return out;
    }

    /// Whole-file read; empty string when the file cannot be opened, which is
    /// indistinguishable from an empty file and is why callers check the path
    /// came from find_bundles() first.
    inline std::string read_file(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return {};
        std::ostringstream buf;
        buf << in.rdbuf();
        return buf.str();
    }
} // namespace ff_test

#endif // FFHR_TEST_CORPUS_HPP
