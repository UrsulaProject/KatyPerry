#ifndef BMT_BFCONTAINER_H
#define BMT_BFCONTAINER_H

#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace bmt
{
    std::array<uint8_t, 16> MD5Key(std::string_view text);
    bool IsBFContainer(const std::vector<uint8_t>& data) noexcept;
    std::vector<uint8_t> DecryptBFContainer(const std::vector<uint8_t>& data,
                                            std::string_view keyString);
    std::vector<uint8_t> EncryptBFContainer(const std::vector<uint8_t>& plainText,
                                            std::string_view keyString);
    void DecryptBFFile(const std::filesystem::path& input,
                       const std::filesystem::path& output,
                       std::string_view keyString);
    void EncryptBFFile(const std::filesystem::path& input,
                       const std::filesystem::path& output,
                       std::string_view keyString);
}

#endif
