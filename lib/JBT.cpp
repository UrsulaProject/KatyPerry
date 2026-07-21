#include <Bemani/JBT.h>

#include <Bemani/BFContainer.h>

#include <json-c/json.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
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
    using CatalogMap = std::unordered_map<uint32_t, bmt::CatalogEntry>;

    struct JBHotDefaultsData
    {
        JBHotMap music;
        std::vector<bmt::Playlist> playlists;
    };

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

    std::vector<bmt::Playlist> BuildJBHotPlaylists(json_object* root)
    {
        json_object* data = JsonDataDictionary(root);
        json_object* playlists = nullptr;
        if (!json_object_object_get_ex(data, "playlist", &playlists) ||
            json_object_get_type(playlists) != json_type_array)
            throw std::runtime_error("decrypted serverData has no playlist array");
        std::vector<bmt::Playlist> output;
        const size_t count = json_object_array_length(playlists);
        output.reserve(count);
        for (size_t index = 0; index < count; ++index)
        {
            json_object* value = json_object_array_get_idx(playlists, index);
            if (!value || json_object_get_type(value) != json_type_object)
                throw std::runtime_error("serverData playlist contains a non-object item");
            bmt::Playlist playlist;
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

    JBHotDefaultsData LoadJBHotDefaults(const fs::path& path)
    {
        auto plist = ParsePlist(ReadFile(path));
        const std::string encodedMusic = PlistString(plist.get(), "musicData");
        const std::string encodedServer = PlistString(plist.get(), "serverData");
        if (encodedMusic.empty() || encodedServer.empty())
            throw std::runtime_error("JBHot defaults plist is missing musicData or serverData");
        const auto musicBytes = DecryptDefaultValue(encodedMusic, "dbfzr5KWvVVAA7FP");
        const auto serverBytes = DecryptDefaultValue(encodedServer, "gh4hh5gh46555fgh");
        auto musicJson = ParseJson(musicBytes);
        auto serverJson = ParseJson(serverBytes);
        return {BuildJBHotMap(musicJson.get()), BuildJBHotPlaylists(serverJson.get())};
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
                throw std::runtime_error("JBHot resource requires --jbhot-plist");
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
            throw std::runtime_error("JBHot pack requires --jbhot-plist");
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

    void NormalizePackRelationships(bmt::PackTable& packs)
    {
        for (auto& [id, instances] : packs)
        {
            for (auto& pack : instances)
            {
                if (!pack.baseID)
                    continue;
                const auto bases = packs.find(pack.baseID);
                if (bases == packs.end())
                    continue;
                for (auto& base : bases->second)
                {
                    if (base.dlcType == pack.dlcType && base.dlcOrder == pack.dlcOrder)
                    {
                        if (!base.extID)
                            base.extID = id;
                        break;
                    }
                }
            }
        }
        for (auto& [id, instances] : packs)
        {
            for (auto& pack : instances)
            {
                if (!pack.extID)
                    continue;
                const auto extensions = packs.find(pack.extID);
                if (extensions == packs.end())
                    continue;
                for (auto& extension : extensions->second)
                {
                    if (extension.dlcType == pack.dlcType && extension.dlcOrder == pack.dlcOrder)
                    {
                        if (!extension.baseID)
                            extension.baseID = id;
                        break;
                    }
                }
            }
        }
    }

    void ApplyCatalog(bmt::LoadResult& result,
                      const std::vector<bmt::CatalogEntry>& explicitCatalog,
                      const std::unordered_map<size_t, CatalogMap>& catalogsByDLC,
                      const JBHotMap& musicData,
                      bool hasExplicitCatalog)
    {
        std::unordered_map<uint32_t, const bmt::CatalogEntry*> officialByID;
        for (const auto& entry : explicitCatalog)
            officialByID.try_emplace(entry.id, &entry);
        for (auto& [id, instances] : result.packs)
        {
            for (auto& pack : instances)
            {
                const auto jbhot = musicData.find(id);
                const auto official = officialByID.find(id);
                if (pack.dlcType == bmt::DLCType::JBHot && jbhot != musicData.end())
                {
                    ApplyCatalogEntry(pack, jbhot->second.catalog);
                    pack.catalogSource = bmt::CatalogSource::JBHot;
                }
                else if (const auto dlc = catalogsByDLC.find(pack.dlcOrder);
                         dlc != catalogsByDLC.end() && dlc->second.contains(id))
                {
                    ApplyCatalogEntry(pack, dlc->second.at(id));
                    pack.catalogSource = bmt::CatalogSource::Official;
                }
                else if (hasExplicitCatalog && official != officialByID.end())
                {
                    ApplyCatalogEntry(pack, *official->second);
                    pack.catalogSource = bmt::CatalogSource::Official;
                }
            }
        }

        NormalizePackRelationships(result.packs);
    }

    uint32_t AllocateID(uint32_t& next,
                        uint32_t last,
                        std::set<uint32_t>& used)
    {
        while (next <= last && used.contains(next))
        {
            if (next == std::numeric_limits<uint32_t>::max())
                break;
            ++next;
        }
        if (next > last || used.contains(next))
            throw std::runtime_error("reserved conflict ID range is exhausted");
        const uint32_t value = next;
        used.insert(value);
        if (next != std::numeric_limits<uint32_t>::max())
            ++next;
        return value;
    }

    int DLCTypePriority(bmt::DLCType type) noexcept
    {
        switch (type)
        {
            case bmt::DLCType::Official: return 0;
            case bmt::DLCType::JBHot: return 1;
            case bmt::DLCType::Custom: return 2;
        }
        return 3;
    }

    bool SamePackContent(bmt::MusicPack& left, bmt::MusicPack& right)
    {
        if (left.resources.size() != right.resources.size())
            return false;
        auto leftResource = left.resources.begin();
        auto rightResource = right.resources.begin();
        for (; leftResource != left.resources.end(); ++leftResource, ++rightResource)
        {
            if (leftResource->first != rightResource->first)
                return false;
            auto& leftValue = leftResource->second;
            auto& rightValue = rightResource->second;
            const bool leftWasMaterialized = leftValue.IsMaterialized();
            const bool rightWasMaterialized = rightValue.IsMaterialized();
            const bool equal = leftValue.Data() == rightValue.Data();
            if (!leftWasMaterialized)
                leftValue.bytes.reset();
            if (!rightWasMaterialized)
                rightValue.bytes.reset();
            if (!equal)
                return false;
        }
        return true;
    }

    void RewriteInfoID(bmt::MusicPack& pack)
    {
        if (pack.id == pack.originalID && !pack.infoMember.empty() &&
            pack.resources.contains(pack.infoMember))
            return;
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

    void AppendJBTDigest(const fs::path& path)
    {
        const auto bytes = ReadFile(path);
        std::array<uint8_t, EVP_MAX_MD_SIZE> digest{};
        unsigned int digestLength = 0;
        if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &digestLength,
                       EVP_md5(), nullptr) != 1 || digestLength != 16)
            throw std::runtime_error("cannot calculate output JBT digest");
        std::ofstream output(path, std::ios::binary | std::ios::app);
        if (!output)
            throw std::runtime_error("cannot append output JBT digest to " + path.string());
        output.write(reinterpret_cast<const char*>(digest.data()), digestLength);
        if (!output)
            throw std::runtime_error("cannot write output JBT digest to " + path.string());
    }

    void WriteJBT(bmt::MusicPack& pack, const fs::path& path, bool encrypt)
    {
        RewriteInfoID(pack);
        int error = 0;
        zip_t* rawArchive = zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
        if (!rawArchive)
            throw std::runtime_error("cannot create output JBT " + path.string());
        ZipPtr archive(rawArchive);
        std::vector<std::vector<uint8_t>> outputMembers;
        outputMembers.reserve(pack.resources.size());
        for (auto& [name, resource] : pack.resources)
        {
            if (encrypt)
            {
                const auto key = name == "infov3" ? IOSKey : IPadKey;
                outputMembers.push_back(bmt::EncryptBFContainer(resource.Data(), key));
            }
            else
            {
                outputMembers.push_back(resource.Data());
            }
            auto& output = outputMembers.back();
            zip_source_t* source = zip_source_buffer(archive.get(), output.data(), output.size(), 0);
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
        AppendJBTDigest(path);
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

    std::vector<uint8_t> BuildOfficialCatalog(bmt::PackTable& packs,
                                              std::vector<bmt::Diagnostic>& warnings)
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
            {
                warnings.push_back({pack.sourcePath,
                                    "omitting missing ext pack " + std::to_string(pack.extID) +
                                    " from mulist for base pack " + std::to_string(id)});
                continue;
            }
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
            if (base->extID && baseByExtension.contains(base->extID))
                extensions.push_back(byID.at(base->extID));
        }

        PlistPtr root(plist_new_array());
        for (auto* pack : mains)
        {
            plist_t item = plist_new_dict();
            std::optional<std::string_view> extURL;
            std::string resolvedExtURL;
            uint32_t extID = 0;
            if (pack->extID && baseByExtension.contains(pack->extID))
            {
                const auto* extension = byID.at(pack->extID);
                resolvedExtURL = !pack->extURL.empty() ? pack->extURL : extension->itemURL;
                extURL = resolvedExtURL;
                extID = pack->extID;
            }
            SetCatalogCommon(item, *pack, pack->name, pack->artist, pack->itemURL,
                             extID, extURL, pack->extendFlag, pack->holdFlag, pack->iTunesURL);
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

    bool IsPlaylistID(std::string_view value) noexcept
    {
        if (value.size() != 32)
            return false;
        return std::all_of(value.begin(), value.end(), [](char character)
        {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f') ||
                   (character >= 'A' && character <= 'F');
        });
    }

    std::string GeneratePlaylistID()
    {
        std::array<uint8_t, 16> bytes{};
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
            throw std::runtime_error("cannot generate playlist PLID");
        constexpr char Hex[] = "0123456789abcdef";
        std::string output(32, '0');
        for (size_t index = 0; index < bytes.size(); ++index)
        {
            output[index * 2] = Hex[bytes[index] >> 4];
            output[index * 2 + 1] = Hex[bytes[index] & 0x0F];
        }
        return output;
    }

    std::vector<uint8_t> BuildOfficialPlaylists(const std::vector<bmt::Playlist>& playlists)
    {
        PlistPtr root(plist_new_array());
        for (const auto& playlist : playlists)
        {
            if (playlist.name.empty())
                throw std::runtime_error("playlist has no name");
            const std::string id = playlist.id.empty() ? GeneratePlaylistID() : playlist.id;
            if (!IsPlaylistID(id))
                throw std::runtime_error("playlist PLID is not a 32-character hex string");
            plist_t item = plist_new_dict();
            plist_t list = plist_new_array();
            for (const uint32_t id : playlist.musicIDs)
                plist_array_append_item(list, plist_new_uint(id));
            plist_dict_set_item(item, "LIST", list);
            plist_dict_set_item(item, "NAME", plist_new_string(playlist.name.c_str()));
            plist_dict_set_item(item, "PLID", plist_new_string(id.c_str()));
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

    std::vector<Playlist> LoadPlaylists(const fs::path& plistPath)
    {
        auto root = ParsePlist(ReadFile(plistPath));
        if (plist_get_node_type(root.get()) != PLIST_ARRAY)
            throw std::runtime_error("playlists plist is not an array");
        std::vector<Playlist> output;
        const uint32_t count = plist_array_get_size(root.get());
        output.reserve(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            plist_t item = plist_array_get_item(root.get(), index);
            if (!item || plist_get_node_type(item) != PLIST_DICT)
                throw std::runtime_error("playlists plist contains a non-dictionary item");
            Playlist playlist;
            playlist.id = PlistString(item, "PLID");
            playlist.name = PlistString(item, "NAME");
            if (playlist.name.empty())
                playlist.name = PlistString(item, "LIST_NAME");
            if (playlist.name.empty())
                throw std::runtime_error("playlist has no NAME");
            if (playlist.id.empty())
                playlist.id = GeneratePlaylistID();
            else if (!IsPlaylistID(playlist.id))
                throw std::runtime_error("playlist PLID is not a 32-character hex string");
            plist_t list = plist_dict_get_item(item, "LIST");
            if (!list || plist_get_node_type(list) != PLIST_ARRAY)
                throw std::runtime_error("playlist has no LIST array");
            const uint32_t musicCount = plist_array_get_size(list);
            playlist.musicIDs.reserve(musicCount);
            for (uint32_t musicIndex = 0; musicIndex < musicCount; ++musicIndex)
            {
                plist_t musicID = plist_array_get_item(list, musicIndex);
                if (!musicID || plist_get_node_type(musicID) != PLIST_UINT)
                    throw std::runtime_error("playlist contains a non-integer music ID");
                uint64_t value = 0;
                plist_get_uint_val(musicID, &value);
                if (value > std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("playlist music ID is outside uint32 range");
                playlist.musicIDs.push_back(static_cast<uint32_t>(value));
            }
            playlist.dlcType = DLCType::Official;
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

    LoadResult LoadPacks(const std::vector<DLCSource>& sources, const LoadOptions& options)
    {
        if (sources.empty())
            throw std::invalid_argument("at least one DLC source is required");
        size_t officialCount = 0;
        size_t jbhotCount = 0;
        std::vector<std::pair<uint32_t, uint32_t>> customRanges;
        std::set<fs::path> sourceDirectories;
        for (const auto& source : sources)
        {
            if (!fs::is_directory(source.directory))
                throw std::invalid_argument("DLC source is not a directory: " + source.directory.string());
            const auto normalized = fs::absolute(source.directory).lexically_normal();
            if (!sourceDirectories.insert(normalized).second)
                throw std::invalid_argument("the same DLC directory was specified more than once");
            officialCount += source.type == DLCType::Official;
            jbhotCount += source.type == DLCType::JBHot;
            if (source.type == DLCType::Custom)
            {
                if (!source.firstID || source.firstID > source.lastID)
                    throw std::invalid_argument("invalid Custom DLC ID range");
                for (const auto& [first, last] : customRanges)
                {
                    if (source.firstID <= last && first <= source.lastID)
                        throw std::invalid_argument("Custom DLC ID ranges overlap");
                }
                customRanges.emplace_back(source.firstID, source.lastID);
            }
        }
        if (officialCount > 1 || jbhotCount > 1)
            throw std::invalid_argument("only one Official and one JBHot DLC source are allowed");
        if (jbhotCount && !options.jbhotDefaultsPlist)
            throw std::invalid_argument("JBHot DLC requires jbhotDefaultsPlist");

        LoadResult result;
        JBHotDefaultsData jbhotDefaults;
        if (options.jbhotDefaultsPlist)
        {
            jbhotDefaults = LoadJBHotDefaults(*options.jbhotDefaultsPlist);
            result.playlists = jbhotDefaults.playlists;
            result.catalog.reserve(jbhotDefaults.music.size());
            for (const auto& [id, entry] : jbhotDefaults.music)
                result.catalog.push_back(entry.catalog);
            std::sort(result.catalog.begin(), result.catalog.end(),
                      [](const auto& left, const auto& right) { return left.id < right.id; });
        }
        auto musicData = std::make_shared<JBHotMap>(std::move(jbhotDefaults.music));

        for (const auto& source : sources)
        {
            const fs::path companion = source.directory / "playlists.plist";
            if (source.type == DLCType::Official && fs::is_regular_file(companion))
            {
                auto officialPlaylists = LoadPlaylists(companion);
                result.playlists.insert(result.playlists.begin(),
                                        std::make_move_iterator(officialPlaylists.begin()),
                                        std::make_move_iterator(officialPlaylists.end()));
            }
        }

        std::vector<CatalogEntry> officialCatalog;
        std::unordered_map<size_t, CatalogMap> catalogsByDLC;
        if (options.catalogPlist)
        {
            officialCatalog = LoadOfficialCatalog(*options.catalogPlist);
            result.catalog.insert(result.catalog.end(), officialCatalog.begin(), officialCatalog.end());
        }
        else
        {
            for (size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex)
            {
                const auto& source = sources[sourceIndex];
                const fs::path companion = source.directory / "mulist.plist";
                if (fs::is_regular_file(companion))
                {
                    auto entries = LoadOfficialCatalog(companion);
                    auto& catalog = catalogsByDLC[sourceIndex];
                    for (const auto& entry : entries)
                        catalog[entry.id] = entry;
                    result.catalog.insert(result.catalog.end(), entries.begin(), entries.end());
                }
            }
        }

        for (size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex)
        {
            const auto& source = sources[sourceIndex];
            for (const auto& path : ExpandInputs({source.directory}))
            {
                try
                {
                    auto pack = LoadOnePack(path, options, musicData);
                    pack.dlcType = source.type;
                    pack.dlcOrder = sourceIndex;
                    if (source.type == DLCType::Custom)
                    {
                        pack.customFirstID = source.firstID;
                        pack.customLastID = source.lastID;
                    }
                    result.packs[pack.id].push_back(std::move(pack));
                }
                catch (const std::exception& error)
                {
                    result.diagnostics.push_back({path, error.what()});
                    if (options.failureMode == FailureMode::Strict)
                        throw;
                }
            }
        }
        ApplyCatalog(result, officialCatalog, catalogsByDLC, *musicData,
                     options.catalogPlist.has_value());
        return result;
    }

    std::vector<IDRemap> ResolveConflicts(LoadResult& result, const ResolveOptions& options)
    {
        if (!options.firstReservedID || options.firstReservedID > options.lastReservedID)
            throw std::invalid_argument("invalid reserved conflict ID range");

        auto& packs = result.packs;
        NormalizePackRelationships(packs);
        std::vector<MusicPack> flat;
        std::vector<uint32_t> oldIDs;
        std::unordered_map<uint32_t, std::vector<size_t>> byOriginalID;
        for (auto& [id, instances] : packs)
        {
            for (auto& pack : instances)
            {
                const size_t index = flat.size();
                byOriginalID[id].push_back(index);
                oldIDs.push_back(id);
                flat.push_back(std::move(pack));
            }
        }

        auto priorityLess = [&](size_t left, size_t right)
        {
            const auto& lhs = flat[left];
            const auto& rhs = flat[right];
            const int lhsPriority = DLCTypePriority(lhs.dlcType);
            const int rhsPriority = DLCTypePriority(rhs.dlcType);
            if (lhsPriority != rhsPriority)
                return lhsPriority < rhsPriority;
            if (lhs.dlcOrder != rhs.dlcOrder)
                return lhs.dlcOrder < rhs.dlcOrder;
            return lhs.sourcePath < rhs.sourcePath;
        };

        auto relationPeer = [&](size_t index) -> std::optional<size_t>
        {
            const auto& pack = flat[index];
            const uint32_t peerID = pack.extID ? pack.extID : pack.baseID;
            const auto candidates = byOriginalID.find(peerID);
            if (!peerID || candidates == byOriginalID.end())
                return std::nullopt;
            for (const size_t candidate : candidates->second)
            {
                if (flat[candidate].dlcType == pack.dlcType &&
                    flat[candidate].dlcOrder == pack.dlcOrder)
                    return candidate;
            }
            return std::nullopt;
        };

        std::vector<bool> active(flat.size(), true);
        for (auto& [id, indices] : byOriginalID)
        {
            std::sort(indices.begin(), indices.end(), priorityLess);
            std::vector<size_t> kept;
            for (const size_t candidate : indices)
            {
                for (const size_t earlier : kept)
                {
                    auto& winner = flat[earlier];
                    auto& duplicate = flat[candidate];
                    const bool compatibleRelations =
                        (!winner.extID || !duplicate.extID || winner.extID == duplicate.extID) &&
                        (!winner.baseID || !duplicate.baseID || winner.baseID == duplicate.baseID);
                    if (!compatibleRelations || !SamePackContent(winner, duplicate))
                        continue;
                    const auto winnerPeer = relationPeer(earlier);
                    const auto duplicatePeer = relationPeer(candidate);
                    if (winnerPeer && duplicatePeer &&
                        !SamePackContent(flat[*winnerPeer], flat[*duplicatePeer]))
                        continue;
                    if (!winner.extID)
                        winner.extID = duplicate.extID;
                    if (!winner.baseID)
                        winner.baseID = duplicate.baseID;
                    active[candidate] = false;
                    ++result.droppedDuplicates;
                    break;
                }
                if (active[candidate])
                    kept.push_back(candidate);
            }
        }

        std::vector<size_t> component(flat.size());
        for (size_t index = 0; index < flat.size(); ++index)
            component[index] = index;
        std::set<size_t> claimedExtensions;
        for (size_t baseIndex = 0; baseIndex < flat.size(); ++baseIndex)
        {
            if (!active[baseIndex] || !flat[baseIndex].extID)
                continue;
            const auto candidates = byOriginalID.find(flat[baseIndex].extID);
            if (candidates == byOriginalID.end())
                continue;
            for (const size_t candidate : candidates->second)
            {
                if (!active[candidate] || claimedExtensions.contains(candidate))
                    continue;
                if (flat[candidate].dlcType == flat[baseIndex].dlcType &&
                    flat[candidate].dlcOrder == flat[baseIndex].dlcOrder)
                {
                    component[candidate] = baseIndex;
                    claimedExtensions.insert(candidate);
                    break;
                }
            }
        }

        std::set<size_t> remapComponents;
        for (const auto& [id, indices] : byOriginalID)
        {
            std::vector<size_t> remaining;
            for (const size_t index : indices)
            {
                if (active[index])
                    remaining.push_back(index);
            }
            std::sort(remaining.begin(), remaining.end(), priorityLess);
            const bool hasOfficial = !remaining.empty() &&
                flat[remaining.front()].dlcType == DLCType::Official;
            for (size_t position = 1; position < remaining.size(); ++position)
            {
                if (flat[remaining[position]].dlcType == DLCType::Official)
                    throw std::runtime_error("non-identical Official packs have the same ID " +
                                             std::to_string(id));
                if (flat[remaining[position]].dlcType == DLCType::JBHot && !hasOfficial)
                    throw std::runtime_error("non-identical JBHot packs have the same ID " +
                                             std::to_string(id));
                remapComponents.insert(component[remaining[position]]);
            }
        }

        std::set<uint32_t> used;
        for (size_t index = 0; index < flat.size(); ++index)
        {
            if (active[index])
                used.insert(flat[index].id);
        }
        uint32_t jbhotNext = options.firstReservedID;
        std::unordered_map<size_t, uint32_t> customNext;
        std::vector<IDRemap> remaps;
        std::unordered_map<size_t, std::vector<size_t>> members;
        for (size_t index = 0; index < component.size(); ++index)
        {
            if (active[index])
                members[component[index]].push_back(index);
        }
        for (const size_t root : remapComponents)
        {
            auto& owner = flat[root];
            if (owner.dlcType == DLCType::Official)
                throw std::runtime_error("Official DLC IDs cannot be remapped");
            uint32_t* next = nullptr;
            uint32_t last = 0;
            if (owner.dlcType == DLCType::JBHot)
            {
                next = &jbhotNext;
                last = options.lastReservedID;
            }
            else
            {
                if (!owner.customFirstID || owner.customFirstID > owner.customLastID)
                    throw std::runtime_error("Custom DLC has no valid ID range");
                auto position = customNext.try_emplace(owner.dlcOrder, owner.customFirstID).first;
                next = &position->second;
                last = owner.customLastID;
            }
            for (const size_t index : members[root])
            {
                const uint32_t oldID = flat[index].id;
                flat[index].id = AllocateID(*next, last, used);
                remaps.push_back({flat[index].sourcePath, oldID, flat[index].id});
            }
            if (members[root].size() == 2)
            {
                const size_t first = members[root][0];
                const size_t second = members[root][1];
                const size_t base = flat[first].extID ? first : second;
                const size_t extension = base == first ? second : first;
                flat[base].extID = flat[extension].id;
                flat[extension].baseID = flat[base].id;
            }
        }

        std::unordered_map<uint32_t, uint32_t> jbhotIDs;
        for (size_t index = 0; index < flat.size(); ++index)
        {
            if (active[index] && flat[index].dlcType == DLCType::JBHot)
                jbhotIDs.try_emplace(oldIDs[index], flat[index].id);
        }
        for (auto& playlist : result.playlists)
        {
            if (playlist.dlcType != DLCType::JBHot)
                continue;
            for (auto& id : playlist.musicIDs)
            {
                if (const auto remapped = jbhotIDs.find(id); remapped != jbhotIDs.end())
                    id = remapped->second;
            }
        }

        packs.clear();
        for (size_t index = 0; index < flat.size(); ++index)
        {
            if (active[index])
                packs[flat[index].id].push_back(std::move(flat[index]));
        }
        return remaps;
    }

    static void ExportPacksImpl(PackTable& packs,
                                const std::vector<Playlist>* playlists,
                                const fs::path& outputDirectory,
                                const bmt::ExportOptions& options,
                                std::vector<Diagnostic>& warnings)
    {
        const auto catalog = BuildOfficialCatalog(packs, warnings);
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

        std::set<size_t> customOrders;
        if (options.separateByDLC)
        {
            for (const auto& [id, instances] : packs)
            {
                const auto& pack = instances.front();
                if (pack.dlcType == DLCType::Custom)
                    customOrders.insert(pack.dlcOrder);
            }
        }
        std::map<size_t, size_t> customNumbers;
        size_t nextCustomNumber = 1;
        for (const auto order : customOrders)
            customNumbers.emplace(order, nextCustomNumber++);

        const auto packOutputDirectory = [&](const MusicPack& pack)
        {
            if (!options.separateByDLC)
                return outputDirectory;
            switch (pack.dlcType)
            {
            case DLCType::Official:
                return outputDirectory / "official";
            case DLCType::JBHot:
                return outputDirectory / "jbhot";
            case DLCType::Custom:
                return outputDirectory /
                       (std::string("custom-") + std::to_string(customNumbers.at(pack.dlcOrder)));
            }
            throw std::runtime_error("unsupported DLC type");
        };

        for (auto& [id, instances] : packs)
        {
            const auto directory = packOutputDirectory(instances.front());
            fs::create_directories(directory);
            WriteJBT(instances.front(), directory / PackFileName(id), options.encryptJBT);
        }
        WriteFile(outputDirectory / "mulist.plist", catalog);
        if (options.mulistKey)
        {
            std::vector<uint8_t> prefixedCatalog(4);
            if (RAND_bytes(prefixedCatalog.data(), static_cast<int>(prefixedCatalog.size())) != 1)
                throw std::runtime_error("cannot generate mulist prefix");
            prefixedCatalog.insert(prefixedCatalog.end(), catalog.begin(), catalog.end());
            const auto encryptedCatalog = bmt::EncryptBFContainer(prefixedCatalog, *options.mulistKey);
            WriteFile(outputDirectory / "mulist", encryptedCatalog);
        }
        if (playlists && !playlists->empty())
            WriteFile(outputDirectory / "playlists.plist", playlistData);
    }

    void ExportPacks(LoadResult& result,
                     const fs::path& outputDirectory,
                     const ExportOptions& options)
    {
        ExportPacksImpl(result.packs, &result.playlists, outputDirectory, options, result.warnings);
    }

    void ExportPlaylists(const std::vector<Playlist>& playlists, const fs::path& plistPath)
    {
        WriteFile(plistPath, BuildOfficialPlaylists(playlists));
    }
}
