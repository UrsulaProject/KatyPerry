#include <Bemani/JBT.h>

#include <Bemani/BFContainer.h>

#include <json-c/json.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <plist/plist.h>
#include <zip.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace
{
    constexpr std::string_view IPadKey = "Konami Bemani Mobile iPad";
    constexpr std::string_view IOSKey = "Konami Bemani Mobile iOS";
    constexpr std::array<std::string_view, 12> JBHotTypes = {
        "marker", "info", "infoV2", "seqBas", "seqAdv", "seqExt",
        "bgm", "index", "smallArtwork", "artwork", "nameBlack", "nameWhite"
    };

    struct PlistDeleter
    {
        void operator()(plist_t value) const noexcept
        {
            if (value)
                plist_free(value);
        }
    };
    using PlistPtr = std::unique_ptr<std::remove_pointer_t<plist_t>, PlistDeleter>;

    struct JsonDeleter
    {
        void operator()(json_object* value) const noexcept
        {
            if (value)
                json_object_put(value);
        }
    };
    using JsonPtr = std::unique_ptr<json_object, JsonDeleter>;

    struct ZipDeleter
    {
        void operator()(zip_t* value) const noexcept
        {
            if (value)
                zip_discard(value);
        }
    };
    using ZipPtr = std::unique_ptr<zip_t, ZipDeleter>;

    struct ZipFileDeleter
    {
        void operator()(zip_file_t* value) const noexcept
        {
            if (value)
                zip_fclose(value);
        }
    };
    using ZipFilePtr = std::unique_ptr<zip_file_t, ZipFileDeleter>;

    struct CipherContextDeleter
    {
        void operator()(EVP_CIPHER_CTX* value) const noexcept
        {
            EVP_CIPHER_CTX_free(value);
        }
    };
    using CipherContextPtr = std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter>;

    struct JBHotEntry
    {
        std::unordered_map<std::string, std::string> prefixes;
        std::string password;
        bmt::CatalogEntry catalog;
    };
    using JBHotMap = std::unordered_map<uint32_t, JBHotEntry>;

    std::vector<uint8_t> ReadFile(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open " + path.string());
        input.seekg(0, std::ios::end);
        const auto size = input.tellg();
        if (size < 0)
            throw std::runtime_error("cannot determine size of " + path.string());
        input.seekg(0, std::ios::beg);
        std::vector<uint8_t> output(static_cast<size_t>(size));
        if (!output.empty())
            input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
        if (!input)
            throw std::runtime_error("cannot read " + path.string());
        return output;
    }

    void WriteFile(const fs::path& path, std::span<const uint8_t> data)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot create " + path.string());
        if (!data.empty())
            output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!output)
            throw std::runtime_error("cannot write " + path.string());
    }

    PlistPtr ParsePlist(std::span<const uint8_t> data, plist_format_t* format = nullptr)
    {
        if (data.size() > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("plist is too large");
        std::string normalized;
        const char* input = reinterpret_cast<const char*>(data.data());
        size_t inputSize = data.size();
        if (data.size() >= 5 && std::memcmp(data.data(), "<?xml", 5) == 0)
        {
            normalized.assign(input, input + inputSize);
            size_t position = 0;
            while ((position = normalized.find("<integer>", position)) != std::string::npos)
            {
                const size_t valueStart = position + 9;
                const size_t valueEnd = normalized.find("</integer>", valueStart);
                if (valueEnd == std::string::npos)
                    break;
                size_t firstDigit = valueStart;
                if (firstDigit < valueEnd && normalized[firstDigit] == '-')
                    ++firstDigit;
                size_t nonZero = firstDigit;
                while (nonZero + 1 < valueEnd && normalized[nonZero] == '0')
                    ++nonZero;
                if (nonZero > firstDigit)
                    normalized.erase(firstDigit, nonZero - firstDigit);
                const size_t normalizedEnd = normalized.find("</integer>", firstDigit);
                position = normalizedEnd == std::string::npos ? normalized.size() : normalizedEnd + 10;
            }
            input = normalized.data();
            inputSize = normalized.size();
        }
        plist_t raw = nullptr;
        plist_format_t parsedFormat = PLIST_FORMAT_NONE;
        if (plist_from_memory(input, static_cast<uint32_t>(inputSize),
                              &raw, &parsedFormat) != PLIST_ERR_SUCCESS || !raw)
            throw std::runtime_error("invalid property list");
        if (format)
            *format = parsedFormat;
        return PlistPtr(raw);
    }

    std::vector<uint8_t> SerializePlist(plist_t value, plist_format_t format)
    {
        char* bytes = nullptr;
        uint32_t size = 0;
        plist_err_t result = format == PLIST_FORMAT_BINARY
            ? plist_to_bin(value, &bytes, &size)
            : plist_to_xml(value, &bytes, &size);
        if (result != PLIST_ERR_SUCCESS || !bytes)
            throw std::runtime_error("property list serialization failed");
        std::vector<uint8_t> output(bytes, bytes + size);
        plist_mem_free(bytes);
        return output;
    }

    std::string PlistString(plist_t dictionary, const char* key)
    {
        plist_t value = plist_dict_get_item(dictionary, key);
        if (!value)
            return {};
        if (plist_get_node_type(value) != PLIST_STRING)
            return {};
        char* string = nullptr;
        plist_get_string_val(value, &string);
        std::string output = string ? string : "";
        if (string)
            plist_mem_free(string);
        return output;
    }

    uint32_t ParseUInt(std::string_view value)
    {
        uint64_t parsed = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size() ||
            parsed > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("invalid uint32 value: " + std::string(value));
        return static_cast<uint32_t>(parsed);
    }

    uint32_t PlistUInt(plist_t dictionary, const char* key, uint32_t fallback = 0)
    {
        plist_t value = plist_dict_get_item(dictionary, key);
        if (!value)
            return fallback;
        const auto type = plist_get_node_type(value);
        if (type == PLIST_UINT)
        {
            uint64_t number = 0;
            plist_get_uint_val(value, &number);
            if (number > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error(std::string(key) + " is outside uint32 range");
            return static_cast<uint32_t>(number);
        }
        if (type == PLIST_STRING)
            return ParseUInt(PlistString(dictionary, key));
        return fallback;
    }

    std::string JsonString(json_object* dictionary, const char* key)
    {
        json_object* value = nullptr;
        if (!dictionary || !json_object_object_get_ex(dictionary, key, &value) ||
            json_object_get_type(value) != json_type_string)
            return {};
        return json_object_get_string(value);
    }

    uint32_t JsonUInt(json_object* dictionary, const char* key, uint32_t fallback = 0)
    {
        json_object* value = nullptr;
        if (!dictionary || !json_object_object_get_ex(dictionary, key, &value))
            return fallback;
        if (json_object_get_type(value) == json_type_int)
        {
            const int64_t number = json_object_get_int64(value);
            if (number < 0 || number > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error(std::string(key) + " is outside uint32 range");
            return static_cast<uint32_t>(number);
        }
        if (json_object_get_type(value) == json_type_string)
            return ParseUInt(json_object_get_string(value));
        return fallback;
    }

    std::vector<uint8_t> Base64Decode(std::string_view encoded)
    {
        std::string compact;
        compact.reserve(encoded.size() + 3);
        for (const char character : encoded)
        {
            if (character != ' ' && character != '\n' && character != '\r' && character != '\t')
                compact.push_back(character);
        }
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
        if (!compact.empty() && compact.back() == '=')
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
        if (!context || EVP_DecryptInit_ex(context.get(), cipher, nullptr, key.data(), iv.data()) != 1)
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

    std::vector<uint8_t> DecryptDefaultValue(std::string_view encoded, std::string_view key)
    {
        const auto ciphertext = Base64Decode(encoded);
        const std::array<uint8_t, 16> iv{};
        return AESDecrypt(ciphertext,
                          std::span(reinterpret_cast<const uint8_t*>(key.data()), key.size()),
                          iv, EVP_aes_128_cbc());
    }

    std::vector<uint8_t> DecryptRNCryptor(std::span<const uint8_t> blob,
                                         std::string_view password)
    {
        if (blob.size() < 66 || (blob[0] != 2 && blob[0] != 3) || !(blob[1] & 1))
            throw std::runtime_error("unsupported RNCryptor payload");
        const auto encryptionSalt = blob.subspan(2, 8);
        const auto hmacSalt = blob.subspan(10, 8);
        const auto iv = blob.subspan(18, 16);
        const auto ciphertext = blob.subspan(34, blob.size() - 66);
        const auto expectedHmac = blob.last(32);
        if (password.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("RNCryptor password is too large");
        std::array<uint8_t, 32> encryptionKey{};
        std::array<uint8_t, 32> hmacKey{};
        if (PKCS5_PBKDF2_HMAC_SHA1(password.data(), static_cast<int>(password.size()),
                                   encryptionSalt.data(), static_cast<int>(encryptionSalt.size()), 10000,
                                   encryptionKey.size(), encryptionKey.data()) != 1 ||
            PKCS5_PBKDF2_HMAC_SHA1(password.data(), static_cast<int>(password.size()),
                                   hmacSalt.data(), static_cast<int>(hmacSalt.size()), 10000,
                                   hmacKey.size(), hmacKey.data()) != 1)
            throw std::runtime_error("RNCryptor PBKDF2 failed");
        const auto hmacInput = blob[0] == 3 ? blob.first(blob.size() - 32) : ciphertext;
        std::array<uint8_t, EVP_MAX_MD_SIZE> actualHmac{};
        unsigned int hmacLength = 0;
        if (!HMAC(EVP_sha256(), hmacKey.data(), static_cast<int>(hmacKey.size()),
                  hmacInput.data(), hmacInput.size(), actualHmac.data(), &hmacLength) || hmacLength != 32 ||
            CRYPTO_memcmp(actualHmac.data(), expectedHmac.data(), 32) != 0)
            throw std::runtime_error("RNCryptor HMAC validation failed");
        return AESDecrypt(ciphertext, encryptionKey, iv, EVP_aes_256_cbc());
    }

    JsonPtr ParseJson(std::span<const uint8_t> data)
    {
        json_tokener* tokener = json_tokener_new();
        if (!tokener)
            throw std::bad_alloc();
        json_object* value = json_tokener_parse_ex(tokener,
            reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
        const auto error = json_tokener_get_error(tokener);
        json_tokener_free(tokener);
        if (error != json_tokener_success || !value)
            throw std::runtime_error("invalid JSON data");
        return JsonPtr(value);
    }

    std::vector<uint8_t> ReadZipEntry(const fs::path& path, std::string_view name)
    {
        int error = 0;
        ZipPtr archive(zip_open(path.c_str(), ZIP_RDONLY, &error));
        if (!archive)
            throw std::runtime_error("cannot open ZIP " + path.string());
        zip_stat_t stat{};
        zip_stat_init(&stat);
        if (zip_stat(archive.get(), std::string(name).c_str(), ZIP_FL_ENC_GUESS, &stat) != 0)
            throw std::runtime_error("ZIP member not found: " + std::string(name));
        ZipFilePtr member(zip_fopen(archive.get(), std::string(name).c_str(), ZIP_FL_ENC_GUESS));
        if (!member)
            throw std::runtime_error("cannot open ZIP member: " + std::string(name));
        if (stat.size > std::numeric_limits<size_t>::max())
            throw std::runtime_error("ZIP member is too large");
        std::vector<uint8_t> data(static_cast<size_t>(stat.size));
        size_t offset = 0;
        while (offset < data.size())
        {
            const zip_int64_t count = zip_fread(member.get(), data.data() + offset, data.size() - offset);
            if (count <= 0)
                throw std::runtime_error("cannot read ZIP member: " + std::string(name));
            offset += static_cast<size_t>(count);
        }
        return data;
    }

    std::vector<std::string> ListZipEntries(const fs::path& path)
    {
        int error = 0;
        ZipPtr archive(zip_open(path.c_str(), ZIP_RDONLY, &error));
        if (!archive)
            throw std::runtime_error("cannot open JBT ZIP " + path.string());
        const zip_int64_t count = zip_get_num_entries(archive.get(), 0);
        if (count < 0)
            throw std::runtime_error("cannot enumerate JBT ZIP " + path.string());
        std::vector<std::string> names;
        names.reserve(static_cast<size_t>(count));
        for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(count); ++index)
        {
            const char* name = zip_get_name(archive.get(), index, ZIP_FL_ENC_GUESS);
            if (name && name[0] && name[std::strlen(name) - 1] != '/')
                names.emplace_back(name);
        }
        return names;
    }

    json_object* JsonDataDictionary(json_object* root)
    {
        json_object* data = nullptr;
        if (root && json_object_get_type(root) == json_type_object &&
            json_object_object_get_ex(root, "data", &data) &&
            json_object_get_type(data) == json_type_object)
            return data;
        if (root && json_object_get_type(root) == json_type_object)
            return root;
        throw std::runtime_error("musicData JSON does not contain an object data field");
    }

    JBHotMap BuildJBHotMap(json_object* root)
    {
        JBHotMap output;
        json_object* data = JsonDataDictionary(root);
        json_object_object_foreach(data, key, value)
        {
            if (!value || json_object_get_type(value) != json_type_object)
                continue;
            const uint32_t id = JsonUInt(value, "id", ParseUInt(key));
            JBHotEntry entry;
            entry.password = JsonString(value, "password");
            for (const auto type : JBHotTypes)
            {
                const std::string field(type);
                entry.prefixes.emplace(field, JsonString(value, field.c_str()));
            }
            entry.catalog.id = id;
            entry.catalog.name = JsonString(value, "title");
            entry.catalog.artist = JsonString(value, "artist");
            entry.catalog.itemURL = JsonString(value, "item");
            entry.catalog.iTunesURL = JsonString(value, "itunes");
            entry.catalog.extID = JsonUInt(value, "extendId");
            entry.catalog.extURL = JsonString(value, "extendItem");
            entry.catalog.extendFlag = JsonUInt(value, "extendFlag");
            entry.catalog.holdFlag = JsonUInt(value, "holdFlag");
            json_object* flagValue = nullptr;
            entry.catalog.hasExtendFlag = json_object_object_get_ex(value, "extendFlag", &flagValue);
            entry.catalog.hasHoldFlag = json_object_object_get_ex(value, "holdFlag", &flagValue);
            entry.catalog.originalID = JsonUInt(value, "origId");
            output[id] = std::move(entry);
        }
        return output;
    }

    JBHotMap LoadJBHotMap(const bmt::LoadOptions& options)
    {
        if (options.musicDataJson)
        {
            const auto bytes = ReadFile(*options.musicDataJson);
            auto json = ParseJson(bytes);
            return BuildJBHotMap(json.get());
        }
        if (options.jbhotDefaultsPlist)
        {
            const auto bytes = ReadFile(*options.jbhotDefaultsPlist);
            auto plist = ParsePlist(bytes);
            const std::string encoded = PlistString(plist.get(), "musicData");
            if (encoded.empty())
                throw std::runtime_error("JBHot defaults plist does not contain musicData");
            const auto jsonBytes = DecryptDefaultValue(encoded, "dbfzr5KWvVVAA7FP");
            auto json = ParseJson(jsonBytes);
            return BuildJBHotMap(json.get());
        }
        return {};
    }

    bool StartsWith(std::span<const uint8_t> data, std::string_view value) noexcept
    {
        return data.size() >= value.size() &&
               std::memcmp(data.data(), value.data(), value.size()) == 0;
    }

    std::vector<uint8_t> DecryptJBHotResource(const std::vector<uint8_t>& data,
                                              const JBHotMap& musicData)
    {
        if (!StartsWith(data, "=JBHOT="))
            return data;
        if (data.size() <= 12)
            throw std::runtime_error("truncated JBHot resource header");
        const uint32_t lowWord = static_cast<uint32_t>(data[7]) |
                                 (static_cast<uint32_t>(data[8]) << 8);
        const uint32_t highWord = static_cast<uint32_t>(data[10]) |
                                  (static_cast<uint32_t>(data[11]) << 8);
        const uint8_t typeIndex = data[9];
        if (typeIndex >= JBHotTypes.size())
            throw std::runtime_error("unknown JBHot resource type");
        const uint32_t id = 19735U * highWord + lowWord;
        std::string prefix;
        std::string password;
        if (typeIndex == 0)
        {
            password = "myR8PfjD";
        }
        else
        {
            const auto entry = musicData.find(id);
            if (entry == musicData.end())
                throw std::runtime_error("musicData is missing JBHot ID " + std::to_string(id));
            const auto field = entry->second.prefixes.find(std::string(JBHotTypes[typeIndex]));
            if (field == entry->second.prefixes.end())
                throw std::runtime_error("musicData is missing the JBHot Base64 prefix");
            prefix = field->second;
            password = entry->second.password;
            if (password.empty())
                throw std::runtime_error("musicData is missing the JBHot password");
        }
        prefix.append(reinterpret_cast<const char*>(data.data() + 12), data.size() - 12);
        const auto payload = Base64Decode(prefix);
        return DecryptRNCryptor(payload, password);
    }

    std::vector<uint8_t> DecodeResource(const std::vector<uint8_t>& data,
                                        bmt::PackFormat format,
                                        std::string_view member,
                                        const std::shared_ptr<const JBHotMap>& musicData)
    {
        if (StartsWith(data, "=JBHOT="))
        {
            if (musicData->empty())
                throw std::runtime_error("JBHot resource requires --jbhot-plist or --music-json");
            return DecryptJBHotResource(data, *musicData);
        }
        if (bmt::IsBFContainer(data))
            return bmt::DecryptBFContainer(data, member == "infov3" ? IOSKey : IPadKey);
        (void)format;
        return data;
    }

    std::pair<bmt::InfoRevision, std::string> SelectInfoMember(const std::vector<std::string>& names)
    {
        const std::set<std::string> entries(names.begin(), names.end());
        if (entries.contains("infov3"))
            return {bmt::InfoRevision::InfoV3, "infov3"};
        if (entries.contains("infov2"))
            return {bmt::InfoRevision::InfoV2, "infov2"};
        if (entries.contains("info"))
            return {bmt::InfoRevision::Info, "info"};
        throw std::runtime_error("JBT has no infov3, infov2, or info member");
    }

    bmt::PackFormat DetectPackFormat(const std::vector<uint8_t>& info)
    {
        if (StartsWith(info, "=JBHOT="))
            return bmt::PackFormat::JBHot;
        if (bmt::IsBFContainer(info))
            return bmt::PackFormat::OfficialBF;
        return bmt::PackFormat::Plain;
    }

    void ParseInfo(bmt::MusicPack& pack, const std::vector<uint8_t>& plaintext)
    {
        if (pack.infoRevision == bmt::InfoRevision::InfoV3 && plaintext.size() < 4)
            throw std::runtime_error("infov3 plaintext is missing its four-byte prefix");
        const std::span<const uint8_t> plistBytes = pack.infoRevision == bmt::InfoRevision::InfoV3
            ? std::span<const uint8_t>(plaintext).subspan(4)
            : std::span<const uint8_t>(plaintext);
        auto plist = ParsePlist(plistBytes);
        if (plist_get_node_type(plist.get()) != PLIST_DICT)
            throw std::runtime_error("JBT info plist is not a dictionary");
        pack.originalID = PlistUInt(plist.get(), "ID");
        if (!pack.originalID)
            throw std::runtime_error("JBT info plist has no valid ID");
        pack.id = pack.originalID;
        pack.name = PlistString(plist.get(), "Name");
        pack.nameYomi = PlistString(plist.get(), "NameYomi");
        pack.artist = PlistString(plist.get(), "Artist");
        pack.artistYomi = PlistString(plist.get(), "ArtistYomi");
        pack.levelBasic = PlistUInt(plist.get(), "LvBas");
        pack.levelAdvanced = PlistUInt(plist.get(), "LvAdv");
        pack.levelExtreme = PlistUInt(plist.get(), "LvExt");
    }

    bmt::MusicPack LoadOnePack(const fs::path& path,
                               const bmt::LoadOptions& options,
                               const std::shared_ptr<const JBHotMap>& musicData)
    {
        const auto names = ListZipEntries(path);
        const auto [revision, infoMember] = SelectInfoMember(names);
        const auto encryptedInfo = ReadZipEntry(path, infoMember);
        const auto format = DetectPackFormat(encryptedInfo);
        if (format == bmt::PackFormat::JBHot && musicData->empty())
            throw std::runtime_error("JBHot pack requires --jbhot-plist or --music-json");
        const auto plainInfo = DecodeResource(encryptedInfo, format, infoMember, musicData);

        bmt::MusicPack pack;
        pack.sourcePath = path;
        pack.format = format;
        pack.infoRevision = revision;
        pack.infoMember = infoMember;
        ParseInfo(pack, plainInfo);
        for (const auto& name : names)
        {
            bmt::PackResource resource;
            resource.name = name;
            if (name == infoMember)
            {
                resource.bytes = plainInfo;
            }
            else if (options.mode == bmt::LoadMode::Eager)
            {
                resource.bytes = DecodeResource(ReadZipEntry(path, name), format, name, musicData);
            }
            else
            {
                resource.lazyLoader = [path, name, format, musicData]()
                {
                    return DecodeResource(ReadZipEntry(path, name), format, name, musicData);
                };
            }
            pack.resources.emplace(name, std::move(resource));
        }
        return pack;
    }

    std::vector<fs::path> ExpandInputs(const std::vector<fs::path>& inputs)
    {
        std::vector<fs::path> files;
        for (const auto& input : inputs)
        {
            if (fs::is_regular_file(input))
            {
                files.push_back(input);
                continue;
            }
            if (!fs::is_directory(input))
                throw std::runtime_error("input does not exist: " + input.string());
            std::vector<fs::path> directoryFiles;
            for (const auto& entry : fs::directory_iterator(input))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".jbt")
                    directoryFiles.push_back(entry.path());
            }
            std::sort(directoryFiles.begin(), directoryFiles.end());
            files.insert(files.end(), directoryFiles.begin(), directoryFiles.end());
        }
        return files;
    }

    void ApplyCatalogEntry(bmt::MusicPack& pack, const bmt::CatalogEntry& entry)
    {
        if (!entry.name.empty()) pack.name = entry.name;
        if (!entry.artist.empty()) pack.artist = entry.artist;
        if (!entry.itemURL.empty()) pack.itemURL = entry.itemURL;
        if (!entry.iTunesURL.empty()) pack.iTunesURL = entry.iTunesURL;
        if (entry.extID) pack.extID = entry.extID;
        if (!entry.extURL.empty()) pack.extURL = entry.extURL;
        if (entry.hasExtendFlag) pack.extendFlag = entry.extendFlag;
        if (entry.hasHoldFlag) pack.holdFlag = entry.holdFlag;
        pack.hasExtendFlag = entry.hasExtendFlag;
        pack.hasHoldFlag = entry.hasHoldFlag;
        if (entry.originalID) pack.baseID = entry.originalID;
    }

    void ApplyCatalog(bmt::LoadResult& result,
                      const std::vector<bmt::CatalogEntry>& officialCatalog,
                      const JBHotMap& musicData,
                      const std::set<fs::path>& officialCatalogDirectories,
                      bool explicitOfficialCatalog)
    {
        std::unordered_map<uint32_t, const bmt::CatalogEntry*> officialByID;
        for (const auto& entry : officialCatalog)
            officialByID.try_emplace(entry.id, &entry);
        for (auto& [id, instances] : result.packs)
        {
            for (auto& pack : instances)
            {
                const auto jbhot = musicData.find(id);
                const auto official = officialByID.find(id);
                const bool belongsToOfficialCatalog = explicitOfficialCatalog ||
                    officialCatalogDirectories.contains(pack.sourcePath.parent_path());
                if (belongsToOfficialCatalog && official != officialByID.end())
                {
                    ApplyCatalogEntry(pack, *official->second);
                    pack.catalogSource = bmt::CatalogSource::Official;
                }
                else if (jbhot != musicData.end())
                {
                    ApplyCatalogEntry(pack, jbhot->second.catalog);
                    pack.catalogSource = bmt::CatalogSource::JBHot;
                }
            }
        }

        std::unordered_map<uint32_t, uint32_t> extToBase;
        for (const auto& [id, instances] : result.packs)
        {
            for (const auto& pack : instances)
            {
                if (pack.extID)
                    extToBase[pack.extID] = id;
                if (pack.baseID)
                    extToBase[id] = pack.baseID;
            }
        }
        for (auto& [id, instances] : result.packs)
        {
            if (const auto base = extToBase.find(id); base != extToBase.end())
            {
                for (auto& pack : instances)
                {
                    if (!pack.baseID)
                        pack.baseID = base->second;
                }
            }
        }
    }

    uint32_t AllocateID(uint32_t& next,
                        const bmt::ResolveOptions& options,
                        std::set<uint32_t>& used)
    {
        while (next <= options.lastReservedID && used.contains(next))
        {
            if (next == std::numeric_limits<uint32_t>::max())
                break;
            ++next;
        }
        if (next > options.lastReservedID || used.contains(next))
            throw std::runtime_error("reserved conflict ID range is exhausted");
        const uint32_t value = next;
        used.insert(value);
        if (next != std::numeric_limits<uint32_t>::max())
            ++next;
        return value;
    }

    void RewriteInfoID(bmt::MusicPack& pack)
    {
        if (pack.infoMember.empty())
        {
            pack.infoRevision = bmt::InfoRevision::InfoV2;
            pack.infoMember = "infov2";
        }
        auto resource = pack.resources.find(pack.infoMember);
        if (resource == pack.resources.end())
        {
            PlistPtr generated(plist_new_dict());
            plist_dict_set_item(generated.get(), "Artist", plist_new_string(pack.artist.c_str()));
            plist_dict_set_item(generated.get(), "ID", plist_new_uint(pack.id));
            plist_dict_set_item(generated.get(), "LvAdv", plist_new_uint(pack.levelAdvanced));
            plist_dict_set_item(generated.get(), "LvBas", plist_new_uint(pack.levelBasic));
            plist_dict_set_item(generated.get(), "LvExt", plist_new_uint(pack.levelExtreme));
            plist_dict_set_item(generated.get(), "Name", plist_new_string(pack.name.c_str()));
            if (!pack.nameYomi.empty())
                plist_dict_set_item(generated.get(), "NameYomi", plist_new_string(pack.nameYomi.c_str()));
            if (!pack.artistYomi.empty())
                plist_dict_set_item(generated.get(), "ArtistYomi", plist_new_string(pack.artistYomi.c_str()));
            bmt::PackResource generatedResource;
            generatedResource.name = pack.infoMember;
            generatedResource.bytes = SerializePlist(generated.get(), PLIST_FORMAT_BINARY);
            resource = pack.resources.emplace(pack.infoMember, std::move(generatedResource)).first;
        }
        const auto& plaintext = resource->second.Data();
        if (pack.infoRevision == bmt::InfoRevision::InfoV3 && plaintext.size() < 4)
            throw std::runtime_error("infov3 plaintext is missing its four-byte prefix");
        const size_t prefixLength = pack.infoRevision == bmt::InfoRevision::InfoV3 ? 4 : 0;
        plist_format_t format = PLIST_FORMAT_NONE;
        auto plist = ParsePlist(std::span<const uint8_t>(plaintext).subspan(prefixLength), &format);
        plist_dict_set_item(plist.get(), "ID", plist_new_uint(pack.id));
        auto serialized = SerializePlist(plist.get(), format);
        if (prefixLength)
            serialized.insert(serialized.begin(), plaintext.begin(), plaintext.begin() + 4);
        resource->second.bytes = std::move(serialized);
        resource->second.lazyLoader = {};
    }

    std::string PackFileName(uint32_t id)
    {
        std::ostringstream stream;
        stream << std::setfill('0') << std::setw(9) << id << ".jbt";
        return stream.str();
    }

    void WriteEncryptedJBT(bmt::MusicPack& pack, const fs::path& path)
    {
        RewriteInfoID(pack);
        int error = 0;
        zip_t* rawArchive = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
        if (!rawArchive)
            throw std::runtime_error("cannot create output JBT " + path.string());
        ZipPtr archive(rawArchive);
        std::vector<std::vector<uint8_t>> encryptedMembers;
        encryptedMembers.reserve(pack.resources.size());
        for (auto& [name, resource] : pack.resources)
        {
            const auto key = name == "infov3" ? IOSKey : IPadKey;
            encryptedMembers.push_back(bmt::EncryptBFContainer(resource.Data(), key));
            auto& encrypted = encryptedMembers.back();
            zip_source_t* source = zip_source_buffer(archive.get(), encrypted.data(), encrypted.size(), 0);
            if (!source)
                throw std::runtime_error("cannot allocate ZIP source for " + name);
            const zip_int64_t index = zip_file_add(archive.get(), name.c_str(), source,
                                                   ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
            if (index < 0)
            {
                zip_source_free(source);
                throw std::runtime_error("cannot add ZIP member " + name);
            }
            if (zip_set_file_compression(archive.get(), static_cast<zip_uint64_t>(index), ZIP_CM_STORE, 0) != 0)
                throw std::runtime_error("cannot set ZIP store mode for " + name);
        }
        if (zip_close(archive.get()) != 0)
            throw std::runtime_error("cannot finalize output JBT " + path.string());
        archive.release();
    }

    void SetCatalogCommon(plist_t dictionary,
                          const bmt::MusicPack& pack,
                          std::string_view name,
                          std::string_view artist,
                          std::string_view itemURL,
                          uint32_t extID,
                          std::optional<std::string_view> extURL,
                          uint32_t extendFlag,
                          uint32_t holdFlag,
                          std::string_view iTunesURL)
    {
        plist_dict_set_item(dictionary, "Artist", plist_new_string(std::string(artist).c_str()));
        plist_dict_set_item(dictionary, "ID", plist_new_uint(pack.id));
        plist_dict_set_item(dictionary, "ItemURL", plist_new_string(std::string(itemURL).c_str()));
        plist_dict_set_item(dictionary, "Name", plist_new_string(std::string(name).c_str()));
        plist_dict_set_item(dictionary, "extID", plist_new_uint(extID));
        if (extURL)
            plist_dict_set_item(dictionary, "extURL", plist_new_string(std::string(*extURL).c_str()));
        plist_dict_set_item(dictionary, "extendFlag", plist_new_uint(extendFlag));
        plist_dict_set_item(dictionary, "holdFlag", plist_new_uint(holdFlag));
        if (!iTunesURL.empty())
            plist_dict_set_item(dictionary, "iTunesURL", plist_new_string(std::string(iTunesURL).c_str()));
    }

    std::vector<uint8_t> BuildOfficialCatalog(bmt::PackTable& packs)
    {
        std::unordered_map<uint32_t, bmt::MusicPack*> byID;
        for (auto& [id, instances] : packs)
        {
            if (instances.size() != 1)
                throw std::runtime_error("unresolved duplicate pack ID " + std::to_string(id));
            byID[id] = &instances.front();
        }

        std::unordered_map<uint32_t, bmt::MusicPack*> baseByExtension;
        std::vector<bmt::MusicPack*> mains;
        std::vector<bmt::MusicPack*> extensions;
        for (auto& [id, instances] : packs)
        {
            auto& pack = instances.front();
            if (!pack.extID)
                continue;
            const auto extension = byID.find(pack.extID);
            if (extension == byID.end())
                throw std::runtime_error("base pack " + std::to_string(id) +
                                         " references missing ext pack " + std::to_string(pack.extID));
            if (!baseByExtension.emplace(pack.extID, &pack).second)
                throw std::runtime_error("multiple base packs reference ext pack " + std::to_string(pack.extID));
            extension->second->baseID = id;
        }
        for (auto& [id, instances] : packs)
        {
            auto& pack = instances.front();
            if (baseByExtension.contains(id))
                continue;
            if (pack.baseID || pack.extendFlag)
                throw std::runtime_error("extension pack " + std::to_string(id) + " has no loaded base pack");
            mains.push_back(&pack);
        }
        for (auto* base : mains)
        {
            if (base->extID)
                extensions.push_back(byID.at(base->extID));
        }

        PlistPtr root(plist_new_array());
        for (auto* pack : mains)
        {
            plist_t item = plist_new_dict();
            std::optional<std::string_view> extURL;
            std::string resolvedExtURL;
            if (pack->extID)
            {
                const auto* extension = byID.at(pack->extID);
                resolvedExtURL = !pack->extURL.empty() ? pack->extURL : extension->itemURL;
                extURL = resolvedExtURL;
            }
            SetCatalogCommon(item, *pack, pack->name, pack->artist, pack->itemURL,
                             pack->extID, extURL, pack->extendFlag, pack->holdFlag, pack->iTunesURL);
            plist_array_append_item(root.get(), item);
        }
        for (auto* extension : extensions)
        {
            auto* base = baseByExtension.at(extension->id);
            const uint32_t extendFlag = extension->hasExtendFlag ? extension->extendFlag : 7;
            const uint32_t holdFlag = extension->hasHoldFlag ? extension->holdFlag : 7;
            const std::string& itemURL = !base->extURL.empty() ? base->extURL : extension->itemURL;
            plist_t item = plist_new_dict();
            SetCatalogCommon(item, *extension, base->name, base->artist, itemURL,
                             0, std::nullopt, extendFlag, holdFlag, base->iTunesURL);
            plist_array_append_item(root.get(), item);
        }
        return SerializePlist(root.get(), PLIST_FORMAT_XML);
    }

    std::vector<uint8_t> BuildOfficialPlaylists(const std::vector<bmt::Playlist>& playlists)
    {
        PlistPtr root(plist_new_array());
        for (const auto& playlist : playlists)
        {
            if (playlist.id.empty())
                throw std::runtime_error("playlist has no PLID");
            plist_t item = plist_new_dict();
            plist_t list = plist_new_array();
            for (const uint32_t id : playlist.musicIDs)
                plist_array_append_item(list, plist_new_uint(id));
            plist_dict_set_item(item, "LIST", list);
            plist_dict_set_item(item, "NAME", plist_new_string(playlist.name.c_str()));
            plist_dict_set_item(item, "PLID", plist_new_string(playlist.id.c_str()));
            plist_array_append_item(root.get(), item);
        }
        return SerializePlist(root.get(), PLIST_FORMAT_XML);
    }
}

namespace bmt
{
    const std::vector<uint8_t>& PackResource::Data() const
    {
        if (!bytes)
        {
            if (!lazyLoader)
                throw std::runtime_error("resource has neither data nor a loader");
            bytes = lazyLoader();
        }
        return *bytes;
    }

    std::vector<CatalogEntry> LoadOfficialCatalog(const fs::path& plistPath)
    {
        auto plist = ParsePlist(ReadFile(plistPath));
        if (plist_get_node_type(plist.get()) != PLIST_ARRAY)
            throw std::runtime_error("official mulist plist is not an array");
        std::vector<CatalogEntry> output;
        const uint32_t count = plist_array_get_size(plist.get());
        output.reserve(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            plist_t item = plist_array_get_item(plist.get(), index);
            if (!item || plist_get_node_type(item) != PLIST_DICT)
                throw std::runtime_error("official mulist contains a non-dictionary item");
            CatalogEntry entry;
            entry.id = PlistUInt(item, "ID");
            entry.name = PlistString(item, "Name");
            entry.artist = PlistString(item, "Artist");
            entry.itemURL = PlistString(item, "ItemURL");
            entry.iTunesURL = PlistString(item, "iTunesURL");
            entry.extID = PlistUInt(item, "extID");
            entry.extURL = PlistString(item, "extURL");
            entry.extendFlag = PlistUInt(item, "extendFlag");
            entry.holdFlag = PlistUInt(item, "holdFlag");
            entry.hasExtendFlag = plist_dict_get_item(item, "extendFlag") != nullptr;
            entry.hasHoldFlag = plist_dict_get_item(item, "holdFlag") != nullptr;
            if (entry.id)
                output.push_back(std::move(entry));
        }
        return output;
    }

    std::vector<Playlist> LoadJBHotPlaylists(const fs::path& serverDataJson)
    {
        auto root = ParseJson(ReadFile(serverDataJson));
        json_object* data = JsonDataDictionary(root.get());
        json_object* playlists = nullptr;
        if (!json_object_object_get_ex(data, "playlist", &playlists) ||
            json_object_get_type(playlists) != json_type_array)
            throw std::runtime_error("serverData JSON has no playlist array");

        std::vector<Playlist> output;
        const size_t count = json_object_array_length(playlists);
        output.reserve(count);
        for (size_t index = 0; index < count; ++index)
        {
            json_object* value = json_object_array_get_idx(playlists, index);
            if (!value || json_object_get_type(value) != json_type_object)
                throw std::runtime_error("serverData playlist contains a non-object item");
            Playlist playlist;
            playlist.id = JsonString(value, "id");
            playlist.name = JsonString(value, "name");
            if (playlist.id.empty())
                throw std::runtime_error("serverData playlist has no id");
            json_object* list = nullptr;
            if (!json_object_object_get_ex(value, "list", &list) ||
                json_object_get_type(list) != json_type_array)
                throw std::runtime_error("serverData playlist has no list array");
            const size_t musicCount = json_object_array_length(list);
            playlist.musicIDs.reserve(musicCount);
            for (size_t musicIndex = 0; musicIndex < musicCount; ++musicIndex)
            {
                json_object* musicID = json_object_array_get_idx(list, musicIndex);
                if (!musicID || json_object_get_type(musicID) != json_type_int)
                    throw std::runtime_error("serverData playlist contains a non-integer music ID");
                const int64_t number = json_object_get_int64(musicID);
                if (number < 0 || number > std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("serverData playlist music ID is outside uint32 range");
                playlist.musicIDs.push_back(static_cast<uint32_t>(number));
            }
            output.push_back(std::move(playlist));
        }
        return output;
    }

    std::vector<uint8_t> DecryptOfficialMusicList(const fs::path& encryptedPath,
                                                  const fs::path& keychainDump,
                                                  std::string_view bundleID)
    {
        auto keychain = ParsePlist(ReadFile(keychainDump));
        plist_t genericPasswords = plist_dict_get_item(keychain.get(), "genp");
        if (!genericPasswords || plist_get_node_type(genericPasswords) != PLIST_ARRAY)
            throw std::runtime_error("keychain dump has no genp array");
        std::string key;
        const uint32_t count = plist_array_get_size(genericPasswords);
        for (uint32_t index = 0; index < count; ++index)
        {
            plist_t item = plist_array_get_item(genericPasswords, index);
            if (PlistString(item, "acct") != "ApplicationUniqueID" ||
                PlistString(item, "svce") != bundleID)
                continue;
            plist_t value = plist_dict_get_item(item, "v_Data");
            if (!value || plist_get_node_type(value) != PLIST_DATA)
                continue;
            char* bytes = nullptr;
            uint64_t length = 0;
            plist_get_data_val(value, &bytes, &length);
            key.assign(bytes, bytes + length);
            if (bytes)
                plist_mem_free(bytes);
            break;
        }
        if (key.empty())
            throw std::runtime_error("ApplicationUniqueID was not found in the keychain dump");
        auto plaintext = DecryptBFContainer(ReadFile(encryptedPath), key);
        if (plaintext.size() < 4)
            throw std::runtime_error("decrypted mulist is missing its four-byte prefix");
        plaintext.erase(plaintext.begin(), plaintext.begin() + 4);
        auto validation = ParsePlist(plaintext);
        if (plist_get_node_type(validation.get()) != PLIST_ARRAY)
            throw std::runtime_error("decrypted mulist is not a plist array");
        return plaintext;
    }

    LoadResult LoadPacks(const std::vector<fs::path>& inputs, const LoadOptions& options)
    {
        LoadResult result;
        if (options.serverDataJson)
            result.playlists = LoadJBHotPlaylists(*options.serverDataJson);
        std::vector<CatalogEntry> officialCatalog;
        std::set<fs::path> officialCatalogDirectories;
        auto musicData = std::make_shared<JBHotMap>(LoadJBHotMap(options));
        if (!musicData->empty())
        {
            result.catalog.reserve(musicData->size());
            for (const auto& [id, entry] : *musicData)
                result.catalog.push_back(entry.catalog);
            std::sort(result.catalog.begin(), result.catalog.end(),
                      [](const auto& left, const auto& right) { return left.id < right.id; });
        }
        if (options.catalogPlist)
        {
            officialCatalog = LoadOfficialCatalog(*options.catalogPlist);
            result.catalog.insert(result.catalog.end(),
                                  officialCatalog.begin(), officialCatalog.end());
        }
        else
        {
            std::set<fs::path> companions;
            for (const auto& input : inputs)
            {
                const fs::path directory = fs::is_directory(input) ? input : input.parent_path();
                const fs::path candidate = directory / "mulist.plist";
                if (fs::is_regular_file(candidate))
                    companions.insert(candidate);
            }
            for (const auto& companion : companions)
            {
                auto companionCatalog = LoadOfficialCatalog(companion);
                officialCatalogDirectories.insert(companion.parent_path());
                officialCatalog.insert(officialCatalog.end(),
                                       companionCatalog.begin(), companionCatalog.end());
                result.catalog.insert(result.catalog.end(),
                                      companionCatalog.begin(), companionCatalog.end());
            }
        }

        for (const auto& path : ExpandInputs(inputs))
        {
            try
            {
                auto pack = LoadOnePack(path, options, musicData);
                result.packs[pack.id].push_back(std::move(pack));
            }
            catch (const std::exception& error)
            {
                result.diagnostics.push_back({path, error.what()});
                if (options.failureMode == FailureMode::Strict)
                    throw;
            }
        }
        ApplyCatalog(result, officialCatalog, *musicData,
                     officialCatalogDirectories, options.catalogPlist.has_value());
        return result;
    }

    static std::vector<IDRemap> ResolveConflictsImpl(PackTable& packs,
                                                      std::vector<Playlist>* playlists,
                                                      const ResolveOptions& options)
    {
        if (!options.firstReservedID || options.firstReservedID > options.lastReservedID)
            throw std::invalid_argument("invalid reserved conflict ID range");

        std::vector<MusicPack> flat;
        std::vector<uint32_t> oldIDs;
        std::unordered_map<uint32_t, std::vector<size_t>> byOriginalID;
        std::set<uint32_t> used;
        for (auto& [id, instances] : packs)
        {
            used.insert(id);
            for (auto& pack : instances)
            {
                const size_t index = flat.size();
                byOriginalID[id].push_back(index);
                oldIDs.push_back(id);
                flat.push_back(std::move(pack));
            }
        }

        std::vector<size_t> component(flat.size());
        for (size_t index = 0; index < flat.size(); ++index)
            component[index] = index;
        std::set<size_t> claimedExtensions;
        for (size_t baseIndex = 0; baseIndex < flat.size(); ++baseIndex)
        {
            const auto extID = flat[baseIndex].extID;
            if (!extID)
                continue;
            const auto candidates = byOriginalID.find(extID);
            if (candidates == byOriginalID.end())
                continue;
            size_t chosen = std::numeric_limits<size_t>::max();
            for (const size_t candidate : candidates->second)
            {
                if (claimedExtensions.contains(candidate))
                    continue;
                if (flat[candidate].sourcePath.parent_path() == flat[baseIndex].sourcePath.parent_path())
                {
                    chosen = candidate;
                    break;
                }
                if (chosen == std::numeric_limits<size_t>::max())
                    chosen = candidate;
            }
            if (chosen != std::numeric_limits<size_t>::max())
            {
                component[chosen] = baseIndex;
                claimedExtensions.insert(chosen);
            }
        }

        std::set<size_t> remapComponents;
        for (const auto& [id, indices] : byOriginalID)
        {
            for (size_t position = 1; position < indices.size(); ++position)
                remapComponents.insert(component[indices[position]]);
        }

        uint32_t next = options.firstReservedID;
        std::vector<IDRemap> remaps;
        std::unordered_map<size_t, std::vector<size_t>> members;
        for (size_t index = 0; index < component.size(); ++index)
            members[component[index]].push_back(index);
        for (const size_t root : remapComponents)
        {
            for (const size_t index : members[root])
            {
                const uint32_t oldID = flat[index].id;
                flat[index].id = AllocateID(next, options, used);
                remaps.push_back({flat[index].sourcePath, oldID, flat[index].id});
            }
            if (members[root].size() == 2)
            {
                const size_t first = members[root][0];
                const size_t second = members[root][1];
                size_t base = flat[first].extID ? first : second;
                size_t extension = base == first ? second : first;
                flat[base].extID = flat[extension].id;
                flat[extension].baseID = flat[base].id;
            }
        }

        if (playlists)
        {
            std::unordered_map<uint32_t, uint32_t> jbhotIDs;
            for (size_t index = 0; index < flat.size(); ++index)
            {
                if (flat[index].catalogSource == CatalogSource::JBHot)
                    jbhotIDs.try_emplace(oldIDs[index], flat[index].id);
            }
            for (auto& playlist : *playlists)
            {
                for (auto& id : playlist.musicIDs)
                {
                    if (const auto remapped = jbhotIDs.find(id); remapped != jbhotIDs.end())
                        id = remapped->second;
                }
            }
        }

        packs.clear();
        for (auto& pack : flat)
            packs[pack.id].push_back(std::move(pack));
        return remaps;
    }

    std::vector<IDRemap> ResolveConflicts(PackTable& packs, const ResolveOptions& options)
    {
        return ResolveConflictsImpl(packs, nullptr, options);
    }

    std::vector<IDRemap> ResolveConflicts(LoadResult& result, const ResolveOptions& options)
    {
        return ResolveConflictsImpl(result.packs, &result.playlists, options);
    }

    static void ExportPacksImpl(PackTable& packs,
                                const std::vector<Playlist>* playlists,
                                const fs::path& outputDirectory)
    {
        const auto catalog = BuildOfficialCatalog(packs);
        const auto playlistData = playlists ? BuildOfficialPlaylists(*playlists) : std::vector<uint8_t>{};
        for (auto& [id, instances] : packs)
        {
            if (instances.size() != 1)
                throw std::runtime_error("cannot export unresolved duplicate ID " + std::to_string(id));
            auto& pack = instances.front();
            RewriteInfoID(pack);
            for (const auto& [name, resource] : pack.resources)
            {
                const auto size = resource.Data().size();
                const auto paddedSize = (static_cast<uint64_t>(size) + 7U) & ~7ULL;
                if (paddedSize > std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("resource is too large to export: " + name);
            }
        }
        fs::create_directories(outputDirectory);
        for (auto& [id, instances] : packs)
        {
            WriteEncryptedJBT(instances.front(), outputDirectory / PackFileName(id));
        }
        WriteFile(outputDirectory / "mulist.plist", catalog);
        if (playlists && !playlists->empty())
            WriteFile(outputDirectory / "playlists.plist", playlistData);
    }

    void ExportPacks(PackTable& packs, const fs::path& outputDirectory)
    {
        ExportPacksImpl(packs, nullptr, outputDirectory);
    }

    void ExportPacks(LoadResult& result, const fs::path& outputDirectory)
    {
        ExportPacksImpl(result.packs, &result.playlists, outputDirectory);
    }
}
