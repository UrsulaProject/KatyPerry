#include <Bemani/BFContainer.h>

#include <Bemani/BFCodec.h>
#include <Bemani/BFDefaults.h>

#include "FileSupport.h"

#include <openssl/evp.h>

#include <limits>
#include <stdexcept>

namespace
{
    uint32_t ReadBE32(const uint8_t* value) noexcept
    {
        return (static_cast<uint32_t>(value[0]) << 24) |
               (static_cast<uint32_t>(value[1]) << 16) |
               (static_cast<uint32_t>(value[2]) << 8) |
               static_cast<uint32_t>(value[3]);
    }

    void WriteBE32(uint8_t* value, uint32_t number) noexcept
    {
        value[0] = static_cast<uint8_t>(number >> 24);
        value[1] = static_cast<uint8_t>(number >> 16);
        value[2] = static_cast<uint8_t>(number >> 8);
        value[3] = static_cast<uint8_t>(number);
    }
}

namespace bmt
{
    std::array<uint8_t, 16> MD5Key(std::string_view text)
    {
        std::array<uint8_t, 16> digest{};
        unsigned int digestLength = 0;
        if (EVP_Digest(text.data(), text.size(), digest.data(), &digestLength, EVP_md5(), nullptr) != 1 ||
            digestLength != digest.size())
            throw std::runtime_error("MD5 key derivation failed");
        return digest;
    }

    bool IsBFContainer(const std::vector<uint8_t>& data) noexcept
    {
        if (data.size() < 8)
            return false;
        const uint32_t originalLength = ReadBE32(data.data() + data.size() - 8);
        const uint32_t encryptedLength = ReadBE32(data.data() + data.size() - 4);
        return encryptedLength == data.size() - 8 &&
               encryptedLength == ((static_cast<uint64_t>(originalLength) + 7U) & ~7ULL);
    }

    std::vector<uint8_t> DecryptBFContainer(const std::vector<uint8_t>& data,
                                            std::string_view keyString)
    {
        if (!IsBFContainer(data))
            throw std::runtime_error("invalid BF container trailer");
        const uint32_t originalLength = ReadBE32(data.data() + data.size() - 8);
        const auto keyArray = MD5Key(keyString);
        const std::vector<uint8_t> key(keyArray.begin(), keyArray.end());
        const std::vector<uint8_t> ciphertext(data.begin(), data.end() - 8);
        BFCodec codec(DefaultBlowfishP(), DefaultBlowfishS(), key);
        auto plaintext = codec.DecryptCBC(ciphertext, DefaultBlowfishIV());
        plaintext.resize(originalLength);
        return plaintext;
    }

    std::vector<uint8_t> EncryptBFContainer(const std::vector<uint8_t>& plainText,
                                            std::string_view keyString)
    {
        if (plainText.size() > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("BF plaintext is too large");
        const uint32_t originalLength = static_cast<uint32_t>(plainText.size());
        const uint64_t paddedLength = (static_cast<uint64_t>(originalLength) + 7U) & ~7ULL;
        if (paddedLength > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("BF padded plaintext is too large");
        const uint32_t encryptedLength = static_cast<uint32_t>(paddedLength);
        std::vector<uint8_t> padded(plainText);
        padded.resize(encryptedLength, 0);
        const auto keyArray = MD5Key(keyString);
        const std::vector<uint8_t> key(keyArray.begin(), keyArray.end());
        BFCodec codec(DefaultBlowfishP(), DefaultBlowfishS(), key);
        auto output = codec.EncryptCBC(padded, DefaultBlowfishIV());
        output.resize(output.size() + 8);
        WriteBE32(output.data() + output.size() - 8, originalLength);
        WriteBE32(output.data() + output.size() - 4, encryptedLength);
        return output;
    }

    void DecryptBFFile(const std::filesystem::path& input,
                       const std::filesystem::path& output,
                       std::string_view keyString)
    {
        detail::WriteFile(output, DecryptBFContainer(detail::ReadFile(input), keyString));
    }

    void EncryptBFFile(const std::filesystem::path& input,
                       const std::filesystem::path& output,
                       std::string_view keyString)
    {
        detail::WriteFile(output, EncryptBFContainer(detail::ReadFile(input), keyString));
    }
}
