/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2013 Nils Maier
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */

#include "MessageDigestImpl.h"

#include <openssl/err.h>
#include <openssl/evp.h>

#include "Adler32MessageDigestImpl.h"
#include "FatalException.h"
#include "fmt.h"
#include "libssl_compat.h"

namespace aria2 {

namespace {
void checkOpenSSL(int result, const char* operation)
{
  if (result == 1) {
    return;
  }

  auto error = ERR_get_error();
  if (error == 0) {
    throw FATAL_EXCEPTION(
        fmt("%s failed: OpenSSL returned no error detail", operation));
  }
  char errorString[256];
  ERR_error_string_n(error, errorString, sizeof(errorString));
  throw FATAL_EXCEPTION(fmt("%s failed: %s", operation, errorString));
}

size_t digestLength(const EVP_MD* md)
{
  if (md == nullptr) {
    return 0;
  }
  auto len = EVP_MD_size(md);
  return len > 0 ? static_cast<size_t>(len) : 0;
}
} // namespace

#if !OPENSSL_101_API
namespace {
EVP_MD_CTX* EVP_MD_CTX_new() { return EVP_MD_CTX_create(); }
} // namespace

namespace {
void EVP_MD_CTX_free(EVP_MD_CTX* ctx) { EVP_MD_CTX_destroy(ctx); }
} // namespace

namespace {
int EVP_MD_CTX_reset(EVP_MD_CTX* ctx)
{
  EVP_MD_CTX_init(ctx);
  return 1;
}
} // namespace
#endif // !OPENSSL_101_API

template <const EVP_MD* (*init_fn)()>
class MessageDigestBase : public MessageDigestImpl {
public:
  MessageDigestBase()
      : ctx_(EVP_MD_CTX_new()),
        md_(init_fn()),
        len_(digestLength(md_))
  {
    if (ctx_ == nullptr || md_ == nullptr || len_ == 0) {
      EVP_MD_CTX_free(ctx_);
      ctx_ = nullptr;
      throw FATAL_EXCEPTION("Failed to create OpenSSL message digest context");
    }
    try {
      checkOpenSSL(EVP_MD_CTX_reset(ctx_), "EVP_MD_CTX_reset");
      reset();
    }
    catch (...) {
      EVP_MD_CTX_free(ctx_);
      ctx_ = nullptr;
      throw;
    }
  }
  virtual ~MessageDigestBase() { EVP_MD_CTX_free(ctx_); }

  static size_t length()
  {
    return digestLength(init_fn());
  }
  virtual size_t getDigestLength() const CXX11_OVERRIDE { return len_; }
  virtual void reset() CXX11_OVERRIDE
  {
    checkOpenSSL(EVP_DigestInit_ex(ctx_, md_, nullptr), "EVP_DigestInit_ex");
  }
  virtual void update(const void* data, size_t length) CXX11_OVERRIDE
  {
    auto bytes = reinterpret_cast<const char*>(data);
    while (length) {
      size_t l = std::min(length, (size_t)std::numeric_limits<uint32_t>::max());
      checkOpenSSL(EVP_DigestUpdate(ctx_, bytes, l), "EVP_DigestUpdate");
      length -= l;
      bytes += l;
    }
  }
  virtual void digest(unsigned char* md) CXX11_OVERRIDE
  {
    unsigned int len = 0;
    checkOpenSSL(EVP_DigestFinal_ex(ctx_, md, &len), "EVP_DigestFinal_ex");
    if (len != len_) {
      throw FATAL_EXCEPTION(fmt("OpenSSL message digest length mismatch: "
                                "expected %zu, got %u",
                                len_, len));
    }
  }

private:
  EVP_MD_CTX* ctx_;
  const EVP_MD* md_;
  const size_t len_;
};

typedef MessageDigestBase<EVP_md5> MessageDigestMD5;
typedef MessageDigestBase<EVP_sha1> MessageDigestSHA1;

std::unique_ptr<MessageDigestImpl> MessageDigestImpl::sha1()
{
  return make_unique<MessageDigestSHA1>();
}

MessageDigestImpl::hashes_t MessageDigestImpl::hashes = {
    {"sha-1", make_hi<MessageDigestSHA1>()},
#ifdef HAVE_EVP_SHA224
    {"sha-224", make_hi<MessageDigestBase<EVP_sha224>>()},
#endif
#ifdef HAVE_EVP_SHA224
    {"sha-256", make_hi<MessageDigestBase<EVP_sha256>>()},
#endif
#ifdef HAVE_EVP_SHA224
    {"sha-384", make_hi<MessageDigestBase<EVP_sha384>>()},
#endif
#ifdef HAVE_EVP_SHA224
    {"sha-512", make_hi<MessageDigestBase<EVP_sha512>>()},
#endif
    {"md5", make_hi<MessageDigestMD5>()},
    ADLER32_MESSAGE_DIGEST};

} // namespace aria2
