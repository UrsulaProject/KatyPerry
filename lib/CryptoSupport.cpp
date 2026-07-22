#include "CryptoSupport.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <limits>
#include <memory>
#include <stdexcept>

namespace
{
    struct CipherContextDeleter
    {
        void operator()(EVP_CIPHER_CTX* value) const noexcept
        {
            EVP_CIPHER_CTX_free(value);
        }
    };
    using CipherContextPtr = std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter>;
}

namespace bmt::detail
{
    std::vector<uint8_t> Base64Decode(std::string_view encoded)
    {
        std::string compact;
        compact.reserve(encoded.size() + 3);
        for (const char character : encoded)
            if (character != ' ' && character != '\n' && character != '\r' && character != '\t')
                compact.push_back(character);
        while (compact.size() % 4)
            compact.push_back('=');
        if (compact.empty())
            return {};
        if (compact.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("Base64 input is too large");
        std::vector<uint8_t> output((compact.size() / 4) * 3);
        const int size = EVP_DecodeBlock(output.data(),
            reinterpret_cast<const unsigned char*>(compact.data()),
            static_cast<int>(compact.size()));
        if (size < 0)
            throw std::runtime_error("invalid Base64 data");
        size_t actual = static_cast<size_t>(size);
        if (compact.back() == '=')
            --actual;
        if (compact.size() >= 2 && compact[compact.size() - 2] == '=')
            --actual;
        output.resize(actual);
        return output;
    }

    std::vector<uint8_t> AESDecrypt(std::span<const uint8_t> ciphertext,
                                    std::span<const uint8_t> key,
                                    std::span<const uint8_t> iv,
                                    const EVP_CIPHER* cipher)
    {
        CipherContextPtr context(EVP_CIPHER_CTX_new());
        if (!context || !cipher ||
            EVP_DecryptInit_ex(context.get(), cipher, nullptr, key.data(), iv.data()) != 1)
            throw std::runtime_error("AES initialization failed");
        const int blockSize = EVP_CIPHER_block_size(cipher);
        if (blockSize <= 0 || ciphertext.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("invalid or oversized AES payload");
        std::vector<uint8_t> plaintext(ciphertext.size() + static_cast<size_t>(blockSize));
        int firstLength = 0;
        int finalLength = 0;
        if (EVP_DecryptUpdate(context.get(), plaintext.data(), &firstLength,
                              ciphertext.data(), static_cast<int>(ciphertext.size())) != 1 ||
            EVP_DecryptFinal_ex(context.get(), plaintext.data() + firstLength, &finalLength) != 1)
            throw std::runtime_error("AES decryption or PKCS7 validation failed");
        plaintext.resize(static_cast<size_t>(firstLength + finalLength));
        return plaintext;
    }

    std::vector<uint8_t> DecryptRNCryptor(std::span<const uint8_t> blob,
                                          std::string_view password)
    {
        if (blob.size() < 66 || (blob[0] != 2 && blob[0] != 3) || !(blob[1] & 1))
            throw std::runtime_error("unsupported RNCryptor payload");
        if (password.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("RNCryptor password is too large");
        const auto encryptionSalt = blob.subspan(2, 8);
        const auto hmacSalt = blob.subspan(10, 8);
        const auto iv = blob.subspan(18, 16);
        const auto ciphertext = blob.subspan(34, blob.size() - 66);
        const auto expectedHmac = blob.last(32);
        std::array<uint8_t, 32> encryptionKey{};
        std::array<uint8_t, 32> hmacKey{};
        if (PKCS5_PBKDF2_HMAC_SHA1(password.data(), static_cast<int>(password.size()),
                                   encryptionSalt.data(), static_cast<int>(encryptionSalt.size()),
                                   10000, encryptionKey.size(), encryptionKey.data()) != 1 ||
            PKCS5_PBKDF2_HMAC_SHA1(password.data(), static_cast<int>(password.size()),
                                   hmacSalt.data(), static_cast<int>(hmacSalt.size()),
                                   10000, hmacKey.size(), hmacKey.data()) != 1)
            throw std::runtime_error("RNCryptor PBKDF2 failed");
        const auto hmacInput = blob[0] == 3 ? blob.first(blob.size() - 32) : ciphertext;
        std::array<uint8_t, EVP_MAX_MD_SIZE> actualHmac{};
        unsigned int hmacLength = 0;
        if (!HMAC(EVP_sha256(), hmacKey.data(), static_cast<int>(hmacKey.size()),
                  hmacInput.data(), hmacInput.size(), actualHmac.data(), &hmacLength) ||
            hmacLength != 32 || CRYPTO_memcmp(actualHmac.data(), expectedHmac.data(), 32) != 0)
            throw std::runtime_error("RNCryptor HMAC validation failed");
        return AESDecrypt(ciphertext, encryptionKey, iv, EVP_aes_256_cbc());
    }

    std::vector<uint8_t> RandomBytes(size_t count)
    {
        if (count > static_cast<size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("random byte request is too large");
        std::vector<uint8_t> output(count);
        if (!output.empty() && RAND_bytes(output.data(), static_cast<int>(output.size())) != 1)
            throw std::runtime_error("cannot generate random bytes");
        return output;
    }

    std::vector<uint8_t> PrependRandomBytes(std::span<const uint8_t> data,
                                            size_t prefixSize)
    {
        if (data.size() > std::numeric_limits<size_t>::max() - prefixSize)
            throw std::runtime_error("prefixed data is too large");
        auto output = RandomBytes(prefixSize);
        output.insert(output.end(), data.begin(), data.end());
        return output;
    }
}
