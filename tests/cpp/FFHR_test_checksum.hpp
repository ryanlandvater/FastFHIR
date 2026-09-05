/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * SHA-256 hasher for Builder::finalize() in the standalone C++ tests.
 *
 * Four test files carried an identical copy of this function.
 *
 * Its own header because it is the only piece of test scaffolding with an
 * external dependency: including it links OpenSSL, and the assertion harness
 * in FFHR_tests.hpp must not drag that into every test.
 */
#ifndef FFHR_TEST_CHECKSUM_HPP
#define FFHR_TEST_CHECKSUM_HPP

#include <FF_Primitives.hpp>

#include <openssl/evp.h>

#include <vector>

namespace ff_test
{
    /// Matches FastFHIR's HashCallback signature, so it is passed straight to
    /// FF_StreamFinalize / Builder::finalize as `.hasher`.
    inline std::vector<BYTE> sha256(const unsigned char *data, Size len)
    {
        std::vector<BYTE> hash(EVP_MAX_MD_SIZE);
        unsigned int out_len = 0;
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        EVP_DigestUpdate(ctx, data, len);
        EVP_DigestFinal_ex(ctx, hash.data(), &out_len);
        EVP_MD_CTX_free(ctx);
        hash.resize(out_len);
        return hash;
    }
} // namespace ff_test

#endif // FFHR_TEST_CHECKSUM_HPP
