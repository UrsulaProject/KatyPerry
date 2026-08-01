#include "PackageSupport.h"

#include <Bemani/BFContainer.h>

#include "CryptoSupport.h"
#include "FileSupport.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <exception>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace
{
    using Json = nlohmann::json;

    uint32_t ParseSourceID(std::string_view value)
    {
        if (value.size() > 1 && value.front() == '0')
            throw std::runtime_error("mapping.json source ID must not contain leading zeros: " +
                                     std::string(value));
        uint64_t parsed = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size() || !parsed ||
            parsed > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("invalid mapping.json source ID: " + std::string(value));
        return static_cast<uint32_t>(parsed);
    }

    struct DigestContextDeleter
    {
        void operator()(EVP_MD_CTX* context) const noexcept
        {
            EVP_MD_CTX_free(context);
        }
    };
    using DigestContextPtr = std::unique_ptr<EVP_MD_CTX, DigestContextDeleter>;

    void DigestLength(EVP_MD_CTX* context, uint64_t length, std::string_view description)
    {
        std::array<uint8_t, 8> encoded{};
        for (size_t index = 0; index < encoded.size(); ++index)
            encoded[index] = static_cast<uint8_t>(length >> ((encoded.size() - index - 1) * 8));
        if (EVP_DigestUpdate(context, encoded.data(), encoded.size()) != 1)
            throw std::runtime_error("cannot update " + std::string(description) + " content hash");
    }
}

namespace bmt::detail
{
    IDMapping LoadIDMapping(const fs::path& directory,
                            uint32_t minimumTargetID,
                            uint32_t maximumTargetID)
    {
        const fs::path path = directory / "mapping.json";
        if (!fs::exists(path))
            return {};
        if (!fs::is_regular_file(path))
            throw std::runtime_error("mapping.json is not a regular file: " + path.string());

        Json root;
        try
        {
            root = Json::parse(ReadFile(path));
        }
        catch (const Json::parse_error&)
        {
            throw std::runtime_error("invalid JSON data in " + path.string());
        }
        if (!root.is_object())
            throw std::runtime_error("mapping.json root must be an object: " + path.string());

        IDMapping mapping;
        std::set<uint32_t> targets;
        for (const auto& [key, value] : root.items())
        {
            const uint32_t oldID = ParseSourceID(key);
            if (!value.is_number_integer() && !value.is_number_unsigned())
                throw std::runtime_error("mapping.json target for " + std::to_string(oldID) +
                                         " must be an integer");
            uint64_t rawTarget = 0;
            if (value.is_number_unsigned())
                rawTarget = value.get<uint64_t>();
            else
            {
                const int64_t signedTarget = value.get<int64_t>();
                if (signedTarget <= 0)
                    throw std::runtime_error("mapping.json target for " +
                                             std::to_string(oldID) +
                                             " is outside the valid ID range");
                rawTarget = static_cast<uint64_t>(signedTarget);
            }
            if (rawTarget < minimumTargetID)
            {
                if (minimumTargetID == 100000000)
                    throw std::runtime_error("mapping.json target for " +
                                             std::to_string(oldID) +
                                             " must be an unpadded ID of at least nine digits");
                throw std::runtime_error("mapping.json target for " + std::to_string(oldID) +
                                         " is below " + std::to_string(minimumTargetID));
            }
            if (rawTarget > maximumTargetID)
                throw std::runtime_error("mapping.json target for " + std::to_string(oldID) +
                                         " is outside the valid ID range " +
                                         std::to_string(minimumTargetID) + ".." +
                                         std::to_string(maximumTargetID));
            const uint32_t newID = static_cast<uint32_t>(rawTarget);
            if (!mapping.emplace(oldID, newID).second)
                throw std::runtime_error("mapping.json contains the source ID more than once: " +
                                         std::to_string(oldID));
            if (!targets.insert(newID).second)
                throw std::runtime_error("mapping.json maps multiple source IDs to " +
                                         std::to_string(newID));
        }
        return mapping;
    }

    uint32_t MappedID(const IDMapping& mapping, uint32_t id) noexcept
    {
        const auto found = mapping.find(id);
        return found == mapping.end() ? id : found->second;
    }

    void ParallelFor(size_t count,
                     size_t jobs,
                     const std::function<void(size_t)>& operation)
    {
        if (!count)
            return;
        if (!jobs)
            jobs = std::thread::hardware_concurrency();
        jobs = std::max<size_t>(1, std::min(jobs, count));
        if (jobs == 1)
        {
            for (size_t index = 0; index < count; ++index)
                operation(index);
            return;
        }

        std::atomic_size_t next{0};
        std::vector<std::exception_ptr> errors(count);
        const auto worker = [&]
        {
            for (;;)
            {
                const size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= count)
                    return;
                try
                {
                    operation(index);
                }
                catch (...)
                {
                    errors[index] = std::current_exception();
                }
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(jobs - 1);
        for (size_t index = 1; index < jobs; ++index)
            workers.emplace_back(worker);
        worker();
        for (auto& thread : workers)
            thread.join();

        for (const auto& error : errors)
            if (error)
                std::rethrow_exception(error);
    }

    std::array<uint8_t, 32> NamedContentHash(
        std::span<const NamedByteSpan> resources,
        std::string_view description)
    {
        DigestContextPtr context(EVP_MD_CTX_new());
        if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("cannot initialize " + std::string(description) +
                                     " content hash");
        for (const auto& resource : resources)
        {
            DigestLength(context.get(), resource.name.size(), description);
            if (EVP_DigestUpdate(context.get(), resource.name.data(), resource.name.size()) != 1)
                throw std::runtime_error("cannot update " + std::string(description) +
                                         " content hash");
            DigestLength(context.get(), resource.data.size(), description);
            if (EVP_DigestUpdate(context.get(), resource.data.data(), resource.data.size()) != 1)
                throw std::runtime_error("cannot update " + std::string(description) +
                                         " content hash");
        }
        std::array<uint8_t, 32> digest{};
        unsigned int digestLength = 0;
        if (EVP_DigestFinal_ex(context.get(), digest.data(), &digestLength) != 1 ||
            digestLength != digest.size())
            throw std::runtime_error("cannot finalize " + std::string(description) +
                                     " content hash");
        return digest;
    }

    std::vector<uint8_t> DecryptPrefixedBFContainer(
        std::span<const uint8_t> ciphertext,
        std::string_view key,
        size_t prefixSize)
    {
        if (key.empty())
            throw std::runtime_error("BF key must not be empty");
        auto plaintext = DecryptBFContainer(
            std::vector<uint8_t>(ciphertext.begin(), ciphertext.end()), key);
        if (plaintext.size() < prefixSize)
            throw std::runtime_error("decrypted BF data is missing its random prefix");
        plaintext.erase(plaintext.begin(), plaintext.begin() +
                                           static_cast<std::ptrdiff_t>(prefixSize));
        return plaintext;
    }

    std::vector<uint8_t> EncryptPrefixedBFContainer(
        std::span<const uint8_t> plaintext,
        std::string_view key,
        size_t prefixSize)
    {
        if (key.empty())
            throw std::runtime_error("BF key must not be empty");
        return EncryptBFContainer(PrependRandomBytes(plaintext, prefixSize), key);
    }
}
