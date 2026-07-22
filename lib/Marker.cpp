#include <Bemani/Marker.h>

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
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
    constexpr std::string_view MarkerKey = "copious plus knit ripples";
    constexpr std::string_view MarkerListKey = "jubeatskmpledata";
    constexpr std::string_view JBHotMarkerPassword = "myR8PfjD";

    struct PlistDeleter
    {
        void operator()(plist_t value) const noexcept
        {
            if (value)
                plist_free(value);
        }
    };
    using PlistPtr = std::unique_ptr<std::remove_pointer_t<plist_t>, PlistDeleter>;

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

    std::vector<uint8_t> ReadFile(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open " + path.string());
        return {std::istreambuf_iterator<char>(input), {}};
    }

    void WriteFile(const fs::path& path, std::span<const uint8_t> data)
    {
        if (!path.parent_path().empty())
            fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot create " + path.string());
        if (!data.empty())
            output.write(reinterpret_cast<const char*>(data.data()),
                         static_cast<std::streamsize>(data.size()));
        if (!output)
            throw std::runtime_error("cannot write " + path.string());
    }

    bool StartsWith(std::span<const uint8_t> data, std::string_view prefix) noexcept
    {
        return data.size() >= prefix.size() &&
               std::memcmp(data.data(), prefix.data(), prefix.size()) == 0;
    }

    bool IsPNG(std::span<const uint8_t> data) noexcept
    {
        static constexpr std::array<uint8_t, 8> Signature = {
            0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
        };
        return data.size() >= Signature.size() &&
               std::equal(Signature.begin(), Signature.end(), data.begin());
    }

    std::vector<uint8_t> Base64Decode(std::string_view encoded)
    {
        std::string compact;
        compact.reserve(encoded.size() + 3);
        for (const char value : encoded)
            if (value != ' ' && value != '\n' && value != '\r' && value != '\t')
                compact.push_back(value);
        while (compact.size() % 4)
            compact.push_back('=');
        if (compact.empty())
            return {};
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

    std::string Base64Encode(std::span<const uint8_t> data)
    {
        if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("data is too large for Base64");
        std::string output(((data.size() + 2) / 3) * 4, '\0');
        const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                         data.data(), static_cast<int>(data.size()));
        if (size < 0)
            throw std::runtime_error("Base64 encoding failed");
        output.resize(static_cast<size_t>(size));
        return output;
    }

    std::vector<uint8_t> AESDecrypt(std::span<const uint8_t> ciphertext,
                                    std::span<const uint8_t> key,
                                    std::span<const uint8_t> iv)
    {
        CipherContextPtr context(EVP_CIPHER_CTX_new());
        if (!context ||
            EVP_DecryptInit_ex(context.get(), EVP_aes_256_cbc(), nullptr,
                               key.data(), iv.data()) != 1)
            throw std::runtime_error("AES initialization failed");
        std::vector<uint8_t> plaintext(ciphertext.size() + 16);
        int first = 0;
        int last = 0;
        if (EVP_DecryptUpdate(context.get(), plaintext.data(), &first,
                              ciphertext.data(), static_cast<int>(ciphertext.size())) != 1 ||
            EVP_DecryptFinal_ex(context.get(), plaintext.data() + first, &last) != 1)
            throw std::runtime_error("AES or PKCS7 validation failed");
        plaintext.resize(static_cast<size_t>(first + last));
        return plaintext;
    }

    std::vector<uint8_t> DecryptJBHotMarker(std::span<const uint8_t> data)
    {
        if (!StartsWith(data, "=JBHOT=") || data.size() <= 12)
            throw std::runtime_error("invalid JBHot marker header");
        if (data[9] != 0)
            throw std::runtime_error("JBHot marker has a non-marker resource type");
        const auto blob = Base64Decode(std::string_view(
            reinterpret_cast<const char*>(data.data() + 12), data.size() - 12));
        if (blob.size() < 66 || (blob[0] != 2 && blob[0] != 3) || !(blob[1] & 1))
            throw std::runtime_error("unsupported RNCryptor payload");
        const auto encryptionSalt = std::span(blob).subspan(2, 8);
        const auto hmacSalt = std::span(blob).subspan(10, 8);
        const auto iv = std::span(blob).subspan(18, 16);
        const auto ciphertext = std::span(blob).subspan(34, blob.size() - 66);
        const auto expectedHmac = std::span(blob).last(32);
        std::array<uint8_t, 32> encryptionKey{};
        std::array<uint8_t, 32> hmacKey{};
        if (PKCS5_PBKDF2_HMAC_SHA1(JBHotMarkerPassword.data(), JBHotMarkerPassword.size(),
                                   encryptionSalt.data(), encryptionSalt.size(), 10000,
                                   encryptionKey.size(), encryptionKey.data()) != 1 ||
            PKCS5_PBKDF2_HMAC_SHA1(JBHotMarkerPassword.data(), JBHotMarkerPassword.size(),
                                   hmacSalt.data(), hmacSalt.size(), 10000,
                                   hmacKey.size(), hmacKey.data()) != 1)
            throw std::runtime_error("RNCryptor PBKDF2 failed");
        const auto hmacInput = blob[0] == 3 ? std::span(blob).first(blob.size() - 32)
                                            : ciphertext;
        std::array<uint8_t, EVP_MAX_MD_SIZE> actual{};
        unsigned int actualSize = 0;
        if (!HMAC(EVP_sha256(), hmacKey.data(), hmacKey.size(), hmacInput.data(),
                  hmacInput.size(), actual.data(), &actualSize) || actualSize != 32 ||
            CRYPTO_memcmp(actual.data(), expectedHmac.data(), 32) != 0)
            throw std::runtime_error("RNCryptor HMAC validation failed");
        return AESDecrypt(ciphertext, encryptionKey, iv);
    }

    std::vector<uint8_t> NormalizePNG(std::vector<uint8_t> data)
    {
        if (IsPNG(data))
            return data;
        if (data.size() >= 12 && IsPNG(std::span(data).subspan(4)))
        {
            data.erase(data.begin(), data.begin() + 4);
            return data;
        }
        throw std::runtime_error("decrypted marker member is not PNG data");
    }

    std::pair<std::vector<uint8_t>, bmt::MarkerFormat> DecodeMarkerMember(
        const std::vector<uint8_t>& data)
    {
        if (StartsWith(data, "=JBHOT="))
            return {NormalizePNG(DecryptJBHotMarker(data)), bmt::MarkerFormat::JBHot};
        if (bmt::IsBFContainer(data))
            return {NormalizePNG(bmt::DecryptBFContainer(data, MarkerKey)),
                    bmt::MarkerFormat::OfficialBF};
        return {NormalizePNG(data), bmt::MarkerFormat::Plain};
    }

    std::vector<uint8_t> EncodeOfficialMarkerMember(std::span<const uint8_t> png)
    {
        if (!IsPNG(png))
            throw std::runtime_error("marker member is not a PNG");
        std::vector<uint8_t> prefixed(4);
        if (RAND_bytes(prefixed.data(), static_cast<int>(prefixed.size())) != 1)
            throw std::runtime_error("cannot generate marker prefix");
        prefixed.insert(prefixed.end(), png.begin(), png.end());
        return bmt::EncryptBFContainer(prefixed, MarkerKey);
    }

    std::vector<std::string> ListZipEntries(const fs::path& path)
    {
        int error = 0;
        ZipPtr archive(zip_open(path.c_str(), ZIP_RDONLY, &error));
        if (!archive)
            throw std::runtime_error("cannot open marker ZIP " + path.string());
        const zip_int64_t count = zip_get_num_entries(archive.get(), 0);
        std::vector<std::string> names;
        for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(count); ++index)
        {
            const char* name = zip_get_name(archive.get(), index, ZIP_FL_ENC_GUESS);
            if (!name || !name[0] || name[std::strlen(name) - 1] == '/' ||
                std::string_view(name).starts_with("__MACOSX/"))
                continue;
            names.emplace_back(name);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    std::vector<uint8_t> ReadZipEntry(const fs::path& path, std::string_view name)
    {
        int error = 0;
        ZipPtr archive(zip_open(path.c_str(), ZIP_RDONLY, &error));
        if (!archive)
            throw std::runtime_error("cannot open marker ZIP " + path.string());
        zip_stat_t stat{};
        zip_stat_init(&stat);
        if (zip_stat(archive.get(), std::string(name).c_str(), ZIP_FL_ENC_GUESS, &stat) != 0)
            throw std::runtime_error("marker ZIP member not found: " + std::string(name));
        ZipFilePtr member(zip_fopen(archive.get(), std::string(name).c_str(), ZIP_FL_ENC_GUESS));
        if (!member)
            throw std::runtime_error("cannot open marker ZIP member: " + std::string(name));
        std::vector<uint8_t> output(static_cast<size_t>(stat.size));
        size_t offset = 0;
        while (offset < output.size())
        {
            const zip_int64_t read = zip_fread(member.get(), output.data() + offset,
                                                output.size() - offset);
            if (read <= 0)
                throw std::runtime_error("cannot read marker ZIP member: " + std::string(name));
            offset += static_cast<size_t>(read);
        }
        return output;
    }

    void WriteZip(const fs::path& path, const std::vector<bmt::MarkerFrame>& frames,
                  bool encrypt)
    {
        if (!path.parent_path().empty())
            fs::create_directories(path.parent_path());
        int error = 0;
        ZipPtr archive(zip_open(path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error));
        if (!archive)
            throw std::runtime_error("cannot create marker ZIP " + path.string());
        std::vector<std::vector<uint8_t>> storage;
        storage.reserve(frames.size());
        for (const auto& frame : frames)
        {
            storage.push_back(encrypt ? EncodeOfficialMarkerMember(frame.png) : frame.png);
            const auto& bytes = storage.back();
            zip_source_t* source = zip_source_buffer(archive.get(), bytes.data(), bytes.size(), 0);
            if (!source)
                throw std::runtime_error("cannot create ZIP source for " + frame.name);
            const zip_int64_t index = zip_file_add(archive.get(), frame.name.c_str(), source,
                                                    ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
            if (index < 0)
            {
                zip_source_free(source);
                throw std::runtime_error("cannot add marker ZIP member " + frame.name);
            }
            if (zip_set_file_compression(archive.get(), static_cast<zip_uint64_t>(index),
                                         ZIP_CM_STORE, 0) != 0)
                throw std::runtime_error("cannot select ZIP store for " + frame.name);
        }
        zip_t* raw = archive.release();
        if (zip_close(raw) != 0)
        {
            const std::string message = zip_strerror(raw);
            zip_discard(raw);
            throw std::runtime_error("cannot finalize marker ZIP: " + message);
        }
    }

    bmt::MarkerPack ReadMarkerPack(const fs::path& path)
    {
        bmt::MarkerPack pack;
        pack.sourcePath = path;
        bool sawBF = false;
        bool sawHot = false;
        for (const auto& name : ListZipEntries(path))
        {
            auto [png, format] = DecodeMarkerMember(ReadZipEntry(path, name));
            sawBF |= format == bmt::MarkerFormat::OfficialBF;
            sawHot |= format == bmt::MarkerFormat::JBHot;
            pack.frames.push_back({name, std::move(png)});
        }
        if (pack.frames.empty())
            throw std::runtime_error("marker ZIP has no file members");
        pack.format = sawHot ? bmt::MarkerFormat::JBHot
                             : sawBF ? bmt::MarkerFormat::OfficialBF
                                     : bmt::MarkerFormat::Plain;
        return pack;
    }

    std::string MarkerSuffix(uint32_t id)
    {
        std::string value = std::to_string(id);
        if (value.size() < 4)
            value.insert(value.begin(), 4 - value.size(), '0');
        return value;
    }

    std::optional<uint32_t> MarkerIDFromName(std::string_view name)
    {
        if (!name.starts_with("mk") || !name.ends_with(".zip"))
            return std::nullopt;
        const auto digits = name.substr(2, name.size() - 6);
        if (digits.empty())
            return std::nullopt;
        uint64_t value = 0;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (error != std::errc{} || end != digits.data() + digits.size() ||
            value > std::numeric_limits<uint32_t>::max())
            return std::nullopt;
        return static_cast<uint32_t>(value);
    }

    uint32_t ParseID(std::string_view value, std::string_view context)
    {
        uint64_t number = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc{} || end != value.data() + value.size() ||
            number > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error(std::string(context) + " is not a uint32 ID");
        return static_cast<uint32_t>(number);
    }

    std::map<uint32_t, uint32_t> LoadMapping(const fs::path& directory)
    {
        const fs::path path = directory / "mapping.json";
        if (!fs::exists(path))
            return {};
        const auto bytes = ReadFile(path);
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        json_object* root = json_tokener_parse(text.c_str());
        if (!root || json_object_get_type(root) != json_type_object)
        {
            if (root)
                json_object_put(root);
            throw std::runtime_error("mapping.json root must be an object: " + path.string());
        }
        std::unique_ptr<json_object, decltype(&json_object_put)> holder(root, json_object_put);
        std::map<uint32_t, uint32_t> output;
        std::set<uint32_t> targets;
        json_object_object_foreach(root, key, value)
        {
            if (!value || json_object_get_type(value) != json_type_int)
                throw std::runtime_error("mapping.json values must be integer IDs");
            const int64_t target = json_object_get_int64(value);
            if (target < 0 || target > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("mapping.json target is outside uint32 range");
            const uint32_t source = ParseID(key, "mapping.json source");
            if (!output.emplace(source, static_cast<uint32_t>(target)).second ||
                !targets.insert(static_cast<uint32_t>(target)).second)
                throw std::runtime_error("mapping.json contains duplicate sources or targets");
        }
        return output;
    }

    bool SamePack(const bmt::MarkerPack& left, const bmt::MarkerPack& right)
    {
        if (left.frames.size() != right.frames.size() || left.banner != right.banner)
            return false;
        for (size_t index = 0; index < left.frames.size(); ++index)
            if (left.frames[index].name != right.frames[index].name ||
                left.frames[index].png != right.frames[index].png)
                return false;
        return true;
    }

    PlistPtr ParsePlist(std::span<const uint8_t> data)
    {
        plist_t raw = nullptr;
        plist_format_t format = PLIST_FORMAT_NONE;
        if (plist_from_memory(reinterpret_cast<const char*>(data.data()), data.size(),
                              &raw, &format) != PLIST_ERR_SUCCESS || !raw)
            throw std::runtime_error("invalid property list");
        return PlistPtr(raw);
    }

    std::vector<uint8_t> SerializePlist(plist_t value, plist_format_t format)
    {
        char* bytes = nullptr;
        uint32_t size = 0;
        const plist_err_t result = format == PLIST_FORMAT_BINARY
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
        if (!value || plist_get_node_type(value) != PLIST_STRING)
            return {};
        char* string = nullptr;
        plist_get_string_val(value, &string);
        std::string output = string ? string : "";
        if (string)
            plist_mem_free(string);
        return output;
    }

    uint64_t UIDValue(plist_t value)
    {
        if (!value || plist_get_node_type(value) != PLIST_UID)
            throw std::runtime_error("NSKeyedArchiver object is not a UID");
        uint64_t uid = 0;
        plist_get_uid_val(value, &uid);
        return uid;
    }

    plist_t ResolveUID(plist_t objects, plist_t value)
    {
        const uint64_t index = UIDValue(value);
        if (index >= plist_array_get_size(objects))
            throw std::runtime_error("NSKeyedArchiver UID is outside $objects");
        return plist_array_get_item(objects, static_cast<uint32_t>(index));
    }

    std::vector<uint8_t> BuildMarkerArchive(const std::vector<bmt::MarkerListEntry>& entries)
    {
        const uint64_t dictionaryClass = 2 + entries.size() * 7;
        const uint64_t arrayClass = dictionaryClass + 1;
        PlistPtr root(plist_new_dict());
        plist_dict_set_item(root.get(), "$archiver", plist_new_string("NSKeyedArchiver"));
        plist_dict_set_item(root.get(), "$version", plist_new_uint(100000));
        plist_t objects = plist_new_array();
        plist_array_append_item(objects, plist_new_string("$null"));

        plist_t arrayObject = plist_new_dict();
        plist_dict_set_item(arrayObject, "$class", plist_new_uid(arrayClass));
        plist_t arrayItems = plist_new_array();
        for (size_t index = 0; index < entries.size(); ++index)
            plist_array_append_item(arrayItems, plist_new_uid(2 + index * 7));
        plist_dict_set_item(arrayObject, "NS.objects", arrayItems);
        plist_array_append_item(objects, arrayObject);

        for (size_t index = 0; index < entries.size(); ++index)
        {
            const auto& entry = entries[index];
            const uint64_t base = 2 + index * 7;
            plist_t dictionary = plist_new_dict();
            plist_dict_set_item(dictionary, "$class", plist_new_uid(dictionaryClass));
            plist_t keys = plist_new_array();
            plist_t values = plist_new_array();
            for (uint64_t offset = 1; offset <= 3; ++offset)
                plist_array_append_item(keys, plist_new_uid(base + offset));
            for (uint64_t offset = 4; offset <= 6; ++offset)
                plist_array_append_item(values, plist_new_uid(base + offset));
            plist_dict_set_item(dictionary, "NS.keys", keys);
            plist_dict_set_item(dictionary, "NS.objects", values);
            plist_array_append_item(objects, dictionary);
            plist_array_append_item(objects, plist_new_string("version"));
            plist_array_append_item(objects, plist_new_string("bannerName"));
            plist_array_append_item(objects, plist_new_string("markerID"));
            plist_array_append_item(objects, plist_new_string(entry.version.c_str()));
            plist_array_append_item(objects, plist_new_string(entry.bannerName.c_str()));
            plist_array_append_item(objects, plist_new_string(entry.markerID.c_str()));
        }

        plist_t dictClass = plist_new_dict();
        plist_t dictClasses = plist_new_array();
        plist_array_append_item(dictClasses, plist_new_string("NSDictionary"));
        plist_array_append_item(dictClasses, plist_new_string("NSObject"));
        plist_dict_set_item(dictClass, "$classes", dictClasses);
        plist_dict_set_item(dictClass, "$classname", plist_new_string("NSDictionary"));
        plist_array_append_item(objects, dictClass);

        plist_t listClass = plist_new_dict();
        plist_t listClasses = plist_new_array();
        plist_array_append_item(listClasses, plist_new_string("NSArray"));
        plist_array_append_item(listClasses, plist_new_string("NSObject"));
        plist_dict_set_item(listClass, "$classes", listClasses);
        plist_dict_set_item(listClass, "$classname", plist_new_string("NSArray"));
        plist_array_append_item(objects, listClass);
        plist_dict_set_item(root.get(), "$objects", objects);
        plist_t top = plist_new_dict();
        plist_dict_set_item(top, "root", plist_new_uid(1));
        plist_dict_set_item(root.get(), "$top", top);
        return SerializePlist(root.get(), PLIST_FORMAT_BINARY);
    }

    std::vector<bmt::MarkerListEntry> ParseMarkerArchive(std::span<const uint8_t> data)
    {
        auto archive = ParsePlist(data);
        plist_t objects = plist_dict_get_item(archive.get(), "$objects");
        plist_t top = plist_dict_get_item(archive.get(), "$top");
        if (!objects || plist_get_node_type(objects) != PLIST_ARRAY || !top)
            throw std::runtime_error("invalid NSKeyedArchiver root");
        plist_t rootObject = ResolveUID(objects, plist_dict_get_item(top, "root"));
        plist_t itemUIDs = plist_dict_get_item(rootObject, "NS.objects");
        if (!itemUIDs || plist_get_node_type(itemUIDs) != PLIST_ARRAY)
            throw std::runtime_error("NSKeyedArchiver root is not an NSArray");
        std::vector<bmt::MarkerListEntry> output;
        const uint32_t count = plist_array_get_size(itemUIDs);
        output.reserve(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            plist_t dictionary = ResolveUID(objects, plist_array_get_item(itemUIDs, index));
            plist_t keys = plist_dict_get_item(dictionary, "NS.keys");
            plist_t values = plist_dict_get_item(dictionary, "NS.objects");
            if (!keys || !values || plist_array_get_size(keys) != plist_array_get_size(values))
                throw std::runtime_error("invalid archived marker dictionary");
            bmt::MarkerListEntry entry;
            for (uint32_t field = 0; field < plist_array_get_size(keys); ++field)
            {
                plist_t keyNode = ResolveUID(objects, plist_array_get_item(keys, field));
                plist_t valueNode = ResolveUID(objects, plist_array_get_item(values, field));
                char* key = nullptr;
                char* value = nullptr;
                plist_get_string_val(keyNode, &key);
                plist_get_string_val(valueNode, &value);
                const std::string keyString = key ? key : "";
                const std::string valueString = value ? value : "";
                if (key)
                    plist_mem_free(key);
                if (value)
                    plist_mem_free(value);
                if (keyString == "markerID")
                    entry.markerID = valueString;
                else if (keyString == "bannerName")
                    entry.bannerName = valueString;
                else if (keyString == "version")
                    entry.version = valueString;
            }
            if (entry.markerID.empty() || entry.bannerName.empty() || entry.version.empty())
                throw std::runtime_error("archived marker entry is missing a field");
            output.push_back(std::move(entry));
        }
        return output;
    }
}

namespace bmt
{
    void DecryptMarker(const fs::path& inputZip, const fs::path& outputZip)
    {
        WriteZip(outputZip, ReadMarkerPack(inputZip).frames, false);
    }

    void EncryptMarker(const fs::path& inputZip, const fs::path& outputZip)
    {
        WriteZip(outputZip, ReadMarkerPack(inputZip).frames, true);
    }

    void UnpackMarker(const fs::path& inputZip, const fs::path& outputDirectory)
    {
        const auto pack = ReadMarkerPack(inputZip);
        for (const auto& frame : pack.frames)
            WriteFile(outputDirectory / frame.name, frame.png);
    }

    void PackMarker(const fs::path& inputDirectory, const fs::path& outputZip)
    {
        if (!fs::is_directory(inputDirectory))
            throw std::runtime_error("marker input is not a directory: " + inputDirectory.string());
        std::vector<MarkerFrame> frames;
        for (const auto& item : fs::recursive_directory_iterator(inputDirectory))
        {
            if (!item.is_regular_file())
                continue;
            const auto relative = fs::relative(item.path(), inputDirectory).generic_string();
            auto png = ReadFile(item.path());
            if (!IsPNG(png))
                throw std::runtime_error("marker input member is not PNG: " + item.path().string());
            frames.push_back({relative, std::move(png)});
        }
        std::sort(frames.begin(), frames.end(), [](const auto& left, const auto& right)
        {
            return left.name < right.name;
        });
        if (frames.empty())
            throw std::runtime_error("marker input directory contains no PNG files");
        WriteZip(outputZip, frames, true);
    }

    void UnpackMarkerDirectory(const fs::path& inputDirectory,
                               const fs::path& outputDirectory)
    {
        for (const auto& item : fs::recursive_directory_iterator(inputDirectory))
        {
            if (!item.is_regular_file() || item.path().extension() != ".zip")
                continue;
            const auto relative = fs::relative(item.path(), inputDirectory);
            UnpackMarker(item.path(), outputDirectory / relative.parent_path() /
                                      relative.stem());
        }
    }

    void PackMarkerDirectory(const fs::path& inputDirectory,
                             const fs::path& outputDirectory)
    {
        for (const auto& item : fs::recursive_directory_iterator(inputDirectory))
        {
            if (!item.is_directory() || !MarkerIDFromName(item.path().filename().string() + ".zip"))
                continue;
            const auto relative = fs::relative(item.path(), inputDirectory);
            PackMarker(item.path(), outputDirectory / relative.parent_path() /
                                    (relative.filename().string() + ".zip"));
        }
    }

    MarkerLoadResult LoadMarkers(const std::vector<MarkerSource>& sources,
                                 const MarkerLoadOptions& options)
    {
        MarkerLoadResult result;
        for (size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex)
        {
            const auto& source = sources[sourceIndex];
            const auto mapping = LoadMapping(source.directory);
            std::set<uint32_t> seenMappings;
            std::vector<fs::path> archives;
            for (const auto& item : fs::directory_iterator(source.directory))
                if (item.is_regular_file() && MarkerIDFromName(item.path().filename().string()))
                    archives.push_back(item.path());
            std::sort(archives.begin(), archives.end());
            for (const auto& path : archives)
            {
                try
                {
                    const uint32_t originalID = *MarkerIDFromName(path.filename().string());
                    const auto mapped = mapping.find(originalID);
                    const uint32_t finalID = mapped == mapping.end() ? originalID : mapped->second;
                    if (mapped != mapping.end())
                        seenMappings.insert(originalID);
                    auto pack = ReadMarkerPack(path);
                    pack.originalID = originalID;
                    pack.id = finalID;
                    pack.dlcType = source.type;
                    pack.dlcOrder = sourceIndex;
                    pack.bannerPath = source.directory / "banner" /
                                      ("tm" + MarkerSuffix(originalID) + "_banner.png");
                    if (fs::is_regular_file(pack.bannerPath))
                    {
                        pack.banner = ReadFile(pack.bannerPath);
                        if (!IsPNG(*pack.banner))
                            throw std::runtime_error("marker banner is not PNG data");
                    }
                    else
                    {
                        result.diagnostics.push_back({pack.bannerPath,
                            "marker " + std::to_string(originalID) + " has no banner"});
                        if (options.failureMode == FailureMode::Strict)
                            throw std::runtime_error("marker has no banner: " + pack.bannerPath.string());
                    }

                    const auto original = result.packs.find(originalID);
                    if (original != result.packs.end() && SamePack(original->second, pack))
                    {
                        ++result.droppedDuplicates;
                        continue;
                    }
                    const auto occupied = result.packs.find(finalID);
                    if (occupied != result.packs.end())
                    {
                        if (SamePack(occupied->second, pack))
                        {
                            ++result.droppedDuplicates;
                            continue;
                        }
                        if (mapped == mapping.end())
                            throw std::runtime_error("marker ID conflict requires mapping.json entry for " +
                                                     std::to_string(originalID));
                        throw std::runtime_error("mapping.json target is already occupied: " +
                                                 std::to_string(finalID));
                    }
                    if (mapped != mapping.end())
                        result.remaps.push_back({path, originalID, finalID});
                    result.packs.emplace(finalID, std::move(pack));
                }
                catch (const std::exception& error)
                {
                    if (options.failureMode == FailureMode::Strict)
                        throw;
                    result.diagnostics.push_back({path, error.what()});
                }
            }
            for (const auto& [sourceID, targetID] : mapping)
                if (!seenMappings.contains(sourceID))
                    throw std::runtime_error("stale marker mapping.json entry " +
                                             std::to_string(sourceID) + " -> " +
                                             std::to_string(targetID));
        }
        return result;
    }

    void ExportMarkers(const MarkerLoadResult& result, const fs::path& outputDirectory,
                       const MarkerExportOptions& options)
    {
        fs::create_directories(outputDirectory / "banner");
        std::vector<MarkerListEntry> entries;
        entries.reserve(result.packs.size());
        for (const auto& [id, pack] : result.packs)
        {
            const std::string suffix = MarkerSuffix(id);
            WriteZip(outputDirectory / ("mk" + suffix + ".zip"), pack.frames, true);
            if (pack.banner)
                WriteFile(outputDirectory / "banner" / ("tm" + suffix + "_banner.png"),
                          *pack.banner);
            entries.push_back({"mk" + suffix, "tm" + suffix + "_banner", pack.version});
        }
        if (options.markerListOutput)
            WriteFile(*options.markerListOutput,
                      EncryptMarkerList(entries, options.markerListEncoding));
    }

    std::vector<MarkerListEntry> LoadMarkerListXML(const fs::path& path)
    {
        auto root = ParsePlist(ReadFile(path));
        if (plist_get_node_type(root.get()) != PLIST_ARRAY)
            throw std::runtime_error("marker list plist is not an array");
        std::vector<MarkerListEntry> entries;
        for (uint32_t index = 0; index < plist_array_get_size(root.get()); ++index)
        {
            plist_t item = plist_array_get_item(root.get(), index);
            MarkerListEntry entry{PlistString(item, "markerID"),
                                  PlistString(item, "bannerName"),
                                  PlistString(item, "version")};
            if (entry.markerID.empty() || entry.bannerName.empty() || entry.version.empty())
                throw std::runtime_error("marker list entry is missing a field");
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    std::vector<uint8_t> BuildMarkerListXML(const std::vector<MarkerListEntry>& entries)
    {
        PlistPtr root(plist_new_array());
        for (const auto& entry : entries)
        {
            plist_t item = plist_new_dict();
            plist_dict_set_item(item, "markerID", plist_new_string(entry.markerID.c_str()));
            plist_dict_set_item(item, "bannerName", plist_new_string(entry.bannerName.c_str()));
            plist_dict_set_item(item, "version", plist_new_string(entry.version.c_str()));
            plist_array_append_item(root.get(), item);
        }
        return SerializePlist(root.get(), PLIST_FORMAT_XML);
    }

    std::vector<MarkerListEntry> DecryptMarkerList(const fs::path& input)
    {
        auto encrypted = ReadFile(input);
        if (!IsBFContainer(encrypted))
        {
            const std::string encoded(reinterpret_cast<const char*>(encrypted.data()), encrypted.size());
            encrypted = Base64Decode(encoded);
        }
        return ParseMarkerArchive(DecryptBFContainer(encrypted, MarkerListKey));
    }

    std::vector<uint8_t> EncryptMarkerList(const std::vector<MarkerListEntry>& entries,
                                           MarkerListEncoding encoding)
    {
        const auto encrypted = EncryptBFContainer(BuildMarkerArchive(entries), MarkerListKey);
        if (encoding == MarkerListEncoding::Raw)
            return encrypted;
        const auto base64 = Base64Encode(encrypted);
        return {base64.begin(), base64.end()};
    }
}
