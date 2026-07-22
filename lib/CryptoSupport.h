#ifndef BMT_CRYPTO_SUPPORT_H
#define BMT_CRYPTO_SUPPORT_H

#include <openssl/evp.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bmt::detail
{
    std::vector<uint8_t> Base64Decode(std::string_view encoded);
    std::vector<uint8_t> AESDecrypt(std::span<const uint8_t> ciphertext,
                                    std::span<const uint8_t> key,
                                    std::span<const uint8_t> iv,
                                    const EVP_CIPHER* cipher);
    std::vector<uint8_t> DecryptRNCryptor(std::span<const uint8_t> blob,
                                          std::string_view password);
    std::vector<uint8_t> RandomBytes(size_t count);
    std::vector<uint8_t> PrependRandomBytes(std::span<const uint8_t> data,
                                            size_t prefixSize);
}

#endif
