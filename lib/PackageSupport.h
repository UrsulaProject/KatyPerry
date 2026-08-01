#ifndef BMT_PACKAGE_SUPPORT_H
#define BMT_PACKAGE_SUPPORT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <string_view>
#include <vector>

namespace bmt::detail
{
    using IDMapping = std::map<uint32_t, uint32_t>;

    struct NamedByteSpan
    {
        std::string_view name;
        std::span<const uint8_t> data;
    };

    IDMapping LoadIDMapping(const std::filesystem::path& directory,
                            uint32_t minimumTargetID,
                            uint32_t maximumTargetID = UINT32_MAX);
    uint32_t MappedID(const IDMapping& mapping, uint32_t id) noexcept;

    void ParallelFor(size_t count,
                     size_t jobs,
                     const std::function<void(size_t)>& operation);

    std::array<uint8_t, 32> NamedContentHash(
        std::span<const NamedByteSpan> resources,
        std::string_view description);

    std::vector<uint8_t> DecryptPrefixedBFContainer(
        std::span<const uint8_t> ciphertext,
        std::string_view key,
        size_t prefixSize = 4);
    std::vector<uint8_t> EncryptPrefixedBFContainer(
        std::span<const uint8_t> plaintext,
        std::string_view key,
        size_t prefixSize = 4);
}

#endif
