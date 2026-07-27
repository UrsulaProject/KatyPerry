#include <Bemani/RB.h>

#include <Bemani/BFContainer.h>

#include "CryptoSupport.h"
#include "FileSupport.h"
#include "PackageSupport.h"
#include "PlistSupport.h"
#include "ZipSupport.h"

#include <nlohmann/json.hpp>
#include <plist/plist.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iterator>
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
    using Json = nlohmann::json;
    using bmt::detail::Base64Decode;
    using bmt::detail::DecryptRNCryptor;
    using bmt::detail::ListZipEntries;
    using bmt::detail::ParsePlist;
    using bmt::detail::PlistPtr;
    using bmt::detail::PlistString;
    using bmt::detail::ReadFile;
    using bmt::detail::ReadZipEntry;
    using bmt::detail::SerializePlist;
    using bmt::detail::WriteFile;

    constexpr std::array<std::string_view, 2> RBKeys = {
        "Konami ReflecBeat For iOS.",
        "Konami ReflecBeatplus.",
    };
    constexpr std::string_view RBHotMagic = "=RBHOT=";
    constexpr uint32_t MinimumRuntimeID = 100000000;
    constexpr uint32_t MaximumRuntimeID = std::numeric_limits<int32_t>::max();
    constexpr std::array<std::string_view, 42> RBHotTypes = {
        "info",
        "noteBasic", "noteMedium", "noteHard",
        "bgmBasic", "bgmMedium", "bgmHard",
        "preBasic", "preMedium", "preHard",
        "artworkBasic1x", "artworkMedium1x", "artworkHard1x", "artworkPack1x",
        "artworkBasic2x", "artworkMedium2x", "artworkHard2x", "artworkPack2x",
        "titleBlackBasic1x", "titleBlackMedium1x", "titleBlackHard1x",
        "titleWhiteBasic1x", "titleWhiteMedium1x", "titleWhiteHard1x",
        "titleBlackBasic2x", "titleBlackMedium2x", "titleBlackHard2x",
        "titleWhiteBasic2x", "titleWhiteMedium2x", "titleWhiteHard2x",
        "artistBlackBasic1x", "artistBlackMedium1x", "artistBlackHard1x",
        "artistWhiteBasic1x", "artistWhiteMedium1x", "artistWhiteHard1x",
        "artistBlackBasic2x", "artistBlackMedium2x", "artistBlackHard2x",
        "artistWhiteBasic2x", "artistWhiteMedium2x", "artistWhiteHard2x",
    };

    struct RBHotEntry
    {
        uint32_t id = 0;
        uint32_t mainID = 0;
        std::string password;
        std::array<std::string, RBHotTypes.size()> prefixes;
        bmt::RBCatalogEntry catalog;
    };
    using RBHotMap = std::unordered_map<uint32_t, RBHotEntry>;

    struct RBHotDefaultsData
    {
        RBHotMap music;
        std::vector<bmt::Playlist> playlists;
        std::map<std::string, std::string> dumps;
    };

    bool StartsWith(std::span<const uint8_t> data, std::string_view value) noexcept
    {
        return data.size() >= value.size() &&
               std::memcmp(data.data(), value.data(), value.size()) == 0;
    }

    uint32_t ParseUInt(std::string_view value, std::string_view context)
    {
        uint64_t parsed = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size() ||
            parsed > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("invalid " + std::string(context) + ": " +
                                     std::string(value));
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
            return ParseUInt(PlistString(dictionary, key), key);
        if (type == PLIST_REAL)
        {
            double number = 0;
            plist_get_real_val(value, &number);
            if (!std::isfinite(number) || number < 0 || number != std::floor(number) ||
                number > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error(std::string(key) + " is outside uint32 range");
            return static_cast<uint32_t>(number);
        }
        return fallback;
    }

    std::string JsonString(const Json& dictionary, std::string_view key)
    {
        if (!dictionary.is_object())
            return {};
        const auto value = dictionary.find(std::string(key));
        if (value == dictionary.end())
            return {};
        if (value->is_string())
            return value->get<std::string>();
        if (value->is_number_integer() || value->is_number_unsigned())
            return value->dump();
        return {};
    }

    uint32_t JsonUInt(const Json& dictionary, std::string_view key, uint32_t fallback = 0)
    {
        if (!dictionary.is_object())
            return fallback;
        const auto value = dictionary.find(std::string(key));
        if (value == dictionary.end())
            return fallback;
        if (value->is_number_unsigned())
        {
            const uint64_t number = value->get<uint64_t>();
            if (number > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error(std::string(key) + " is outside uint32 range");
            return static_cast<uint32_t>(number);
        }
        if (value->is_number_integer())
        {
            const int64_t number = value->get<int64_t>();
            if (number < 0 || number > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error(std::string(key) + " is outside uint32 range");
            return static_cast<uint32_t>(number);
        }
        if (value->is_string())
            return ParseUInt(value->get_ref<const std::string&>(), key);
        return fallback;
    }

    Json ParseJson(std::span<const uint8_t> data, std::string_view description)
    {
        try
        {
            return Json::parse(data.begin(), data.end());
        }
        catch (const Json::parse_error&)
        {
            throw std::runtime_error("invalid " + std::string(description) + " JSON data");
        }
    }

    std::vector<uint8_t> DecryptDefaultValue(std::string_view encoded,
                                             std::string_view key)
    {
        const auto ciphertext = Base64Decode(encoded);
        const std::array<uint8_t, 16> iv{};
        return bmt::detail::AESDecrypt(
            ciphertext,
            std::span(reinterpret_cast<const uint8_t*>(key.data()), key.size()),
            iv, EVP_aes_128_cbc());
    }

    const Json& JsonData(const Json& root, std::string_view description)
    {
        if (!root.is_object())
            throw std::runtime_error(std::string(description) + " JSON root is not an object");
        const auto data = root.find("data");
        return data != root.end() && data->is_object() ? *data : root;
    }

    Json DecryptDefaultsItem(plist_t defaults,
                             std::string_view name,
                             std::string_view key)
    {
        const std::string storageKey(name);
        const auto encoded = PlistString(defaults, storageKey.c_str());
        if (encoded.empty())
            return {};
        return ParseJson(DecryptDefaultValue(encoded, key), name);
    }

    void MergeMusicData(Json& destination, const Json& source)
    {
        if (source.is_null())
            return;
        const auto& data = JsonData(source, "musicData");
        for (const auto& [key, value] : data.items())
        {
            auto& target = destination[key];
            if (target.is_object() && value.is_object())
            {
                for (const auto& [field, fieldValue] : value.items())
                    target[field] = fieldValue;
            }
            else
                target = value;
        }
    }

    RBHotMap BuildRBHotMap(const Json& data)
    {
        RBHotMap output;
        for (const auto& [key, value] : data.items())
        {
            if (!value.is_object())
                continue;
            const uint32_t fallbackID = ParseUInt(key, "RBHot musicData ID");
            const uint32_t id = JsonUInt(value, "id", fallbackID);
            RBHotEntry entry;
            entry.id = id;
            entry.mainID = JsonUInt(value, "mainId");
            entry.password = JsonString(value, "password");
            for (size_t index = 0; index < RBHotTypes.size(); ++index)
                entry.prefixes[index] = JsonString(value, RBHotTypes[index]);
            entry.catalog.id = id;
            entry.catalog.name = JsonString(value, "title");
            if (entry.catalog.name.empty())
                entry.catalog.name = JsonString(value, "name");
            entry.catalog.artist = JsonString(value, "artist");
            entry.catalog.itemURL = JsonString(value, "item");
            entry.catalog.iTunesURL = JsonString(value, "itunes");
            output[id] = std::move(entry);
        }
        return output;
    }

    std::vector<bmt::Playlist> BuildRBHotPlaylists(const Json& root)
    {
        std::vector<bmt::Playlist> output;
        if (root.is_null())
            return output;
        const auto& data = JsonData(root, "serverData");
        const auto found = data.find("playlist");
        if (found == data.end())
            return output;
        if (!found->is_array())
            throw std::runtime_error("RBHot serverData playlist is not an array");
        for (const auto& value : *found)
        {
            if (!value.is_object())
                throw std::runtime_error("RBHot playlist contains a non-object item");
            bmt::Playlist playlist;
            playlist.id = JsonString(value, "id");
            playlist.name = JsonString(value, "name");
            const auto list = value.find("list");
            if (list == value.end() || !list->is_array())
                throw std::runtime_error("RBHot playlist has no list array");
            for (const auto& id : *list)
            {
                if (id.is_number_unsigned())
                {
                    const auto number = id.get<uint64_t>();
                    if (number > std::numeric_limits<uint32_t>::max())
                        throw std::runtime_error("RBHot playlist ID is outside uint32 range");
                    playlist.musicIDs.push_back(static_cast<uint32_t>(number));
                }
                else if (id.is_number_integer())
                {
                    const auto number = id.get<int64_t>();
                    if (number < 0 || number > std::numeric_limits<uint32_t>::max())
                        throw std::runtime_error("RBHot playlist ID is outside uint32 range");
                    playlist.musicIDs.push_back(static_cast<uint32_t>(number));
                }
                else if (id.is_string())
                    playlist.musicIDs.push_back(
                        ParseUInt(id.get_ref<const std::string&>(), "RBHot playlist ID"));
                else
                    throw std::runtime_error("RBHot playlist contains a non-integer ID");
            }
            output.push_back(std::move(playlist));
        }
        return output;
    }

    RBHotDefaultsData LoadRBHotDefaults(const fs::path& path)
    {
        auto defaults = ParsePlist(ReadFile(path));
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> Keys = {{
            {"musicData", "b38fh495h9fjhw34"},
            {"serverData", "gh4hh5gh46555fgh"},
            {"userData", "fh1dghh4dfg87dfg"},
            {"offlineData", "efg4df21gvb4dfv4"},
            {"scoreData", "fdg2df1gh32er1tg"},
        }};

        RBHotDefaultsData output;
        Json music = Json::object();
        Json server;
        for (const auto& [name, key] : Keys)
        {
            auto json = DecryptDefaultsItem(defaults.get(), name, key);
            if (json.is_null())
                continue;
            output.dumps.emplace(std::string(name), json.dump(4));
            if (name == "musicData")
                MergeMusicData(music, json);
            else if (name == "serverData")
                server = std::move(json);
        }

        Json detailData = Json::object();
        uint32_t detailVersion = 0;
        const auto directDetail =
            DecryptDefaultsItem(defaults.get(), "musicDetail", "b38fh495h9fjhw34");
        if (!directDetail.is_null())
        {
            MergeMusicData(music, directDetail);
            MergeMusicData(detailData, directDetail);
        }
        for (uint32_t index = 1; ; ++index)
        {
            const std::string storageKey = "musicDetail" + std::to_string(index);
            const auto detail =
                DecryptDefaultsItem(defaults.get(), storageKey, "b38fh495h9fjhw34");
            if (detail.is_null())
                break;
            MergeMusicData(music, detail);
            MergeMusicData(detailData, detail);
            detailVersion = index;
        }
        if (!detailData.empty())
        {
            Json merged;
            merged["version"] = detailVersion;
            merged["data"] = std::move(detailData);
            output.dumps.emplace("musicDetail", merged.dump(4));
        }
        if (music.empty())
            throw std::runtime_error("RBHot defaults plist is missing musicData");
        output.music = BuildRBHotMap(music);
        output.playlists = BuildRBHotPlaylists(server);
        return output;
    }

    struct RBHotHeader
    {
        uint32_t id = 0;
        uint8_t type = 0;
    };

    RBHotHeader ParseRBHotHeader(std::span<const uint8_t> data)
    {
        if (!StartsWith(data, RBHotMagic))
            throw std::runtime_error("resource does not have an RBHot header");
        if (data.size() <= 12)
            throw std::runtime_error("truncated RBHot resource header");
        const uint32_t low = static_cast<uint32_t>(data[7]) |
                             (static_cast<uint32_t>(data[8]) << 8);
        const uint32_t high = static_cast<uint32_t>(data[10]) |
                              (static_cast<uint32_t>(data[11]) << 8);
        const uint8_t type = data[9];
        if (type >= RBHotTypes.size())
            throw std::runtime_error("unknown RBHot resource type " + std::to_string(type));
        const uint64_t id = static_cast<uint64_t>(high) * 23456U + low;
        if (id > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("RBHot resource ID is outside uint32 range");
        return {static_cast<uint32_t>(id), type};
    }

    std::vector<uint8_t> DecryptRBHotResource(std::span<const uint8_t> data,
                                              const RBHotMap& music,
                                              uint32_t expectedID)
    {
        const auto header = ParseRBHotHeader(data);
        if (expectedID && header.id != expectedID)
            throw std::runtime_error("RBHot header ID " + std::to_string(header.id) +
                                     " does not match package ID " +
                                     std::to_string(expectedID));
        const auto found = music.find(header.id);
        if (found == music.end())
            throw std::runtime_error("musicData is missing RBHot ID " +
                                     std::to_string(header.id));
        const auto& entry = found->second;
        if (entry.password.empty())
            throw std::runtime_error("musicData is missing the RBHot password for ID " +
                                     std::to_string(header.id));
        const auto& prefix = entry.prefixes[header.type];
        if (prefix.empty())
            throw std::runtime_error("musicData is missing RBHot prefix " +
                                     std::string(RBHotTypes[header.type]) + " for ID " +
                                     std::to_string(header.id));
        std::string encoded = prefix;
        encoded.append(reinterpret_cast<const char*>(data.data() + 12), data.size() - 12);
        return DecryptRNCryptor(Base64Decode(encoded), entry.password);
    }

    void ParseInfo(bmt::RBMusicPack& pack, std::span<const uint8_t> plaintext)
    {
        auto info = ParsePlist(plaintext);
        if (plist_get_node_type(info.get()) != PLIST_DICT)
            throw std::runtime_error("RB info is not a plist dictionary");
        pack.originalID = pack.id = PlistUInt(info.get(), "ID");
        if (!pack.id)
            throw std::runtime_error("RB info has no ID");
        pack.musicName = PlistString(info.get(), "MusicName");
        pack.musicNameHira = PlistString(info.get(), "MusicNameHira");
        pack.musicNameRoman = PlistString(info.get(), "MusicNameRoman");
        pack.artistName = PlistString(info.get(), "ArtistName");
        pack.artistNameHira = PlistString(info.get(), "ArtistNameHira");
        pack.artistNameRoman = PlistString(info.get(), "ArtistNameRoman");
        pack.basic = PlistUInt(info.get(), "Basic");
        pack.medium = PlistUInt(info.get(), "Medium");
        pack.hard = PlistUInt(info.get(), "Hard");
        pack.bpmMin = PlistUInt(info.get(), "BpmMin");
        pack.bpmMax = PlistUInt(info.get(), "BpmMax");
    }

    std::optional<uint8_t> DetectOfficialType(const std::vector<uint8_t>& encryptedInfo,
                                              uint32_t fileID,
                                              std::vector<uint8_t>& plaintext)
    {
        for (uint8_t type = 0; type < RBKeys.size(); ++type)
        {
            try
            {
                auto candidate = bmt::DecryptBFContainer(encryptedInfo, RBKeys[type]);
                bmt::RBMusicPack parsed;
                ParseInfo(parsed, candidate);
                if (parsed.id == fileID)
                {
                    plaintext = std::move(candidate);
                    return type;
                }
            }
            catch (const std::exception&)
            {
            }
        }
        return std::nullopt;
    }

    std::vector<uint8_t> DecodeResource(std::vector<uint8_t> data,
                                        std::optional<uint8_t> decodeType,
                                        const std::shared_ptr<const RBHotMap>& hot,
                                        uint32_t expectedID)
    {
        if (StartsWith(data, RBHotMagic))
        {
            if (!hot || hot->empty())
                throw std::runtime_error("RBHot resource requires --rbhot-plist");
            return DecryptRBHotResource(data, *hot, expectedID);
        }
        if (bmt::IsBFContainer(data))
        {
            if (!decodeType || *decodeType >= RBKeys.size())
                throw std::runtime_error("official BF resource has no detected DecodeType");
            return bmt::DecryptBFContainer(data, RBKeys[*decodeType]);
        }
        return data;
    }

    bmt::RBMusicPack LoadOneRB(const fs::path& path,
                               const bmt::RBLoadOptions& options,
                               const std::shared_ptr<const RBHotMap>& hot)
    {
        if (!fs::is_regular_file(path))
            throw std::invalid_argument("RB input is not a regular file: " + path.string());
        const std::string filenameID = path.stem().string();
        if (filenameID.size() > 1 && filenameID.front() == '0')
            throw std::runtime_error("RB filename ID must not contain leading zeros: " +
                                     filenameID);
        const uint32_t fileID = ParseUInt(filenameID, "RB filename ID");
        const auto names = ListZipEntries(path);
        if (std::find(names.begin(), names.end(), "info") == names.end())
            throw std::runtime_error("RB package has no info member");

        auto rawInfo = ReadZipEntry(path, "info");
        bmt::RBMusicPack pack;
        pack.sourcePath = path;
        pack.sourceFileID = fileID;
        std::vector<uint8_t> info;
        if (StartsWith(rawInfo, RBHotMagic))
        {
            if (!hot || hot->empty())
                throw std::runtime_error("RBHot package requires --rbhot-plist");
            info = DecryptRBHotResource(rawInfo, *hot, fileID);
            pack.format = bmt::RBPackFormat::RBHot;
            if (const auto found = hot->find(fileID); found != hot->end())
                pack.hotMainID = found->second.mainID;
        }
        else if (bmt::IsBFContainer(rawInfo))
        {
            pack.decodeType = DetectOfficialType(rawInfo, fileID, info);
            if (!pack.decodeType)
                throw std::runtime_error("cannot decrypt RB info with DecodeType 0 or 1");
            pack.format = bmt::RBPackFormat::OfficialBF;
        }
        else
        {
            info = std::move(rawInfo);
            pack.format = bmt::RBPackFormat::Plain;
        }
        ParseInfo(pack, info);
        if (pack.id != fileID)
            throw std::runtime_error("RB info ID " + std::to_string(pack.id) +
                                     " does not match filename ID " +
                                     std::to_string(fileID));

        for (const auto& name : names)
        {
            bmt::PackResource resource;
            resource.name = name;
            if (name == "info")
                resource.bytes = info;
            else
            {
                const auto loader = [path, name, decodeType = pack.decodeType, hot, fileID]
                {
                    return DecodeResource(ReadZipEntry(path, name), decodeType, hot, fileID);
                };
                if (options.mode == bmt::LoadMode::Eager)
                    resource.bytes = loader();
                else
                    resource.lazyLoader = loader;
            }
            pack.resources.emplace(name, std::move(resource));
        }
        return pack;
    }

    void RewriteInfoID(bmt::RBMusicPack& pack)
    {
        auto found = pack.resources.find("info");
        if (found == pack.resources.end())
            throw std::runtime_error("RB package has no info resource");
        if (pack.id == pack.originalID)
            return;
        plist_format_t format = PLIST_FORMAT_NONE;
        auto info = ParsePlist(found->second.Data(), &format);
        if (plist_get_node_type(info.get()) != PLIST_DICT)
            throw std::runtime_error("RB info is not a plist dictionary");
        plist_dict_set_item(info.get(), "ID", plist_new_uint(pack.id));
        found->second.bytes = SerializePlist(info.get(), format);
        found->second.lazyLoader = {};
    }

    void WriteRB(bmt::RBMusicPack& pack,
                 const fs::path& path,
                 std::optional<uint8_t> decodeType)
    {
        RewriteInfoID(pack);
        if (decodeType && *decodeType >= RBKeys.size())
            throw std::invalid_argument("RB DecodeType must be 0 or 1");
        std::vector<bmt::detail::ZipMember> members;
        members.reserve(pack.resources.size());
        for (auto& [name, resource] : pack.resources)
        {
            auto bytes = resource.Data();
            if (decodeType)
                bytes = bmt::EncryptBFContainer(bytes, RBKeys[*decodeType]);
            members.push_back({name, std::move(bytes)});
        }
        bmt::detail::WriteStoredZipWithMD5(path, members);
    }

    fs::path SafeMemberPath(std::string_view name)
    {
        const fs::path path = fs::path(std::string(name)).lexically_normal();
        if (path.empty() || path.is_absolute())
            throw std::runtime_error("unsafe RB member path: " + std::string(name));
        for (const auto& component : path)
            if (component == "..")
                throw std::runtime_error("unsafe RB member path: " + std::string(name));
        return path;
    }

    void ExtractPack(bmt::RBMusicPack& pack, const fs::path& outputDirectory)
    {
        for (auto& [name, resource] : pack.resources)
            WriteFile(outputDirectory / SafeMemberPath(name), resource.Data());
    }

    bmt::RBMusicPack LoadExpandedRB(const fs::path& directory)
    {
        if (!fs::is_directory(directory))
            throw std::invalid_argument("expanded RB input is not a directory: " +
                                        directory.string());
        bmt::RBMusicPack pack;
        pack.sourcePath = directory;
        pack.format = bmt::RBPackFormat::Plain;
        for (const auto& entry : fs::recursive_directory_iterator(directory))
        {
            if (entry.is_symlink())
                throw std::runtime_error("expanded RB contains a symlink: " +
                                         entry.path().string());
            if (!entry.is_regular_file())
                continue;
            const std::string name = fs::relative(entry.path(), directory).generic_string();
            SafeMemberPath(name);
            bmt::PackResource resource;
            resource.name = name;
            resource.bytes = ReadFile(entry.path());
            pack.resources.emplace(name, std::move(resource));
        }
        const auto info = pack.resources.find("info");
        if (info == pack.resources.end())
            throw std::runtime_error("expanded RB has no info member");
        ParseInfo(pack, info->second.Data());
        pack.sourceFileID = pack.originalID;
        return pack;
    }

    std::vector<fs::path> RecursiveRBFiles(const fs::path& directory)
    {
        if (!fs::is_directory(directory))
            throw std::invalid_argument("RB input is not a directory: " + directory.string());
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(directory))
            if (entry.is_regular_file() && entry.path().extension() == ".rb")
                files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        return files;
    }

    std::vector<fs::path> ExpandedRBDirectories(const fs::path& directory)
    {
        if (!fs::is_directory(directory))
            throw std::invalid_argument("expanded RB input is not a directory: " +
                                        directory.string());
        std::set<fs::path> directories;
        for (const auto& entry : fs::recursive_directory_iterator(directory))
            if (entry.is_regular_file() && entry.path().filename() == "info")
                directories.insert(entry.path().parent_path());
        return {directories.begin(), directories.end()};
    }
}

namespace bmt
{
    void DecryptRB(const fs::path& inputRB,
                   const fs::path& outputRB,
                   const RBLoadOptions& options)
    {
        std::shared_ptr<const RBHotMap> hot = std::make_shared<RBHotMap>();
        if (options.rbhotDefaultsPlist)
            hot = std::make_shared<RBHotMap>(
                LoadRBHotDefaults(*options.rbhotDefaultsPlist).music);
        auto eager = options;
        eager.mode = LoadMode::Eager;
        auto pack = LoadOneRB(inputRB, eager, hot);
        WriteRB(pack, outputRB, std::nullopt);
    }

    void EncryptRB(const fs::path& inputRB,
                   const fs::path& outputRB,
                   uint8_t decodeType,
                   const RBLoadOptions& options)
    {
        if (decodeType >= RBKeys.size())
            throw std::invalid_argument("RB DecodeType must be 0 or 1");
        std::shared_ptr<const RBHotMap> hot = std::make_shared<RBHotMap>();
        if (options.rbhotDefaultsPlist)
            hot = std::make_shared<RBHotMap>(
                LoadRBHotDefaults(*options.rbhotDefaultsPlist).music);
        auto eager = options;
        eager.mode = LoadMode::Eager;
        auto pack = LoadOneRB(inputRB, eager, hot);
        WriteRB(pack, outputRB, decodeType);
    }

    void UnpackRB(const fs::path& inputRB,
                  const fs::path& outputDirectory,
                  const RBLoadOptions& options)
    {
        std::shared_ptr<const RBHotMap> hot = std::make_shared<RBHotMap>();
        if (options.rbhotDefaultsPlist)
            hot = std::make_shared<RBHotMap>(
                LoadRBHotDefaults(*options.rbhotDefaultsPlist).music);
        auto eager = options;
        eager.mode = LoadMode::Eager;
        auto pack = LoadOneRB(inputRB, eager, hot);
        ExtractPack(pack, outputDirectory);
    }

    void PackRB(const fs::path& inputDirectory,
                const fs::path& outputRB,
                std::optional<uint8_t> decodeType)
    {
        auto pack = LoadExpandedRB(inputDirectory);
        WriteRB(pack, outputRB, decodeType);
    }

    void UnpackRBDirectory(const fs::path& inputDirectory,
                           const fs::path& outputDirectory,
                           const RBLoadOptions& options)
    {
        std::shared_ptr<const RBHotMap> hot = std::make_shared<RBHotMap>();
        if (options.rbhotDefaultsPlist)
            hot = std::make_shared<RBHotMap>(
                LoadRBHotDefaults(*options.rbhotDefaultsPlist).music);
        auto eager = options;
        eager.mode = LoadMode::Eager;
        for (const auto& input : RecursiveRBFiles(inputDirectory))
        {
            fs::path relative = fs::relative(input, inputDirectory);
            relative.replace_extension();
            auto pack = LoadOneRB(input, eager, hot);
            ExtractPack(pack, outputDirectory / relative);
        }
    }

    void PackRBDirectory(const fs::path& inputDirectory,
                         const fs::path& outputDirectory,
                         std::optional<uint8_t> decodeType)
    {
        for (const auto& directory : ExpandedRBDirectories(inputDirectory))
        {
            fs::path relative = fs::relative(directory, inputDirectory);
            if (relative.empty() || relative == ".")
                relative = inputDirectory.filename();
            relative += ".rb";
            auto pack = LoadExpandedRB(directory);
            WriteRB(pack, outputDirectory / relative, decodeType);
        }
    }

    std::map<std::string, std::string> DumpRBHotDefaults(const fs::path& defaultsPlist)
    {
        return LoadRBHotDefaults(defaultsPlist).dumps;
    }

    std::vector<uint8_t> DecryptRBList(const fs::path& encryptedPath,
                                       std::string_view key)
    {
        auto plaintext =
            detail::DecryptPrefixedBFContainer(ReadFile(encryptedPath), key);
        auto validation = ParsePlist(plaintext);
        if (plist_get_node_type(validation.get()) != PLIST_ARRAY)
            throw std::runtime_error("decrypted RB list is not a plist array");
        return plaintext;
    }

    std::vector<uint8_t> EncryptRBList(const fs::path& plaintextPath,
                                       std::string_view key)
    {
        const auto plaintext = ReadFile(plaintextPath);
        auto validation = ParsePlist(plaintext);
        if (plist_get_node_type(validation.get()) != PLIST_ARRAY)
            throw std::runtime_error("RB list plaintext is not a plist array");
        return detail::EncryptPrefixedBFContainer(plaintext, key);
    }
}

namespace
{
    std::vector<uint8_t> LoadListBytes(const fs::path& directory,
                                       std::string_view plaintextName,
                                       std::string_view runtimeName,
                                       const std::optional<std::string>& key)
    {
        const fs::path plaintext = directory / plaintextName;
        if (fs::is_regular_file(plaintext))
            return ReadFile(plaintext);
        const fs::path runtime = directory / runtimeName;
        if (!fs::is_regular_file(runtime))
            return {};
        if (!key)
            throw std::runtime_error(runtime.filename().string() +
                                     " is encrypted and requires --mulist-key");
        return bmt::detail::DecryptPrefixedBFContainer(ReadFile(runtime), *key);
    }

    std::vector<bmt::RBCatalogEntry> ParseRBCatalog(std::span<const uint8_t> bytes)
    {
        if (bytes.empty())
            return {};
        auto root = ParsePlist(bytes);
        if (plist_get_node_type(root.get()) != PLIST_ARRAY)
            throw std::runtime_error("RB mulist is not a plist array");
        std::vector<bmt::RBCatalogEntry> output;
        const uint32_t count = plist_array_get_size(root.get());
        output.reserve(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            plist_t item = plist_array_get_item(root.get(), index);
            if (!item || plist_get_node_type(item) != PLIST_DICT)
                throw std::runtime_error("RB mulist contains a non-dictionary item");
            bmt::RBCatalogEntry entry;
            entry.id = PlistUInt(item, "ID");
            entry.name = PlistString(item, "Name");
            entry.artist = PlistString(item, "Artist");
            entry.itemURL = PlistString(item, "ItemURL");
            entry.iTunesURL = PlistString(item, "iTunesURL");
            if (!entry.id)
                throw std::runtime_error("RB mulist item has no ID");
            output.push_back(std::move(entry));
        }
        return output;
    }

    std::vector<bmt::RBExtendRelation> ParseRBExtensions(
        std::span<const uint8_t> bytes,
        bmt::DLCType dlcType,
        size_t dlcOrder)
    {
        if (bytes.empty())
            return {};
        auto root = ParsePlist(bytes);
        if (plist_get_node_type(root.get()) != PLIST_ARRAY)
            throw std::runtime_error("RB nolist is not a plist array");
        std::vector<bmt::RBExtendRelation> output;
        const uint32_t count = plist_array_get_size(root.get());
        output.reserve(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            plist_t item = plist_array_get_item(root.get(), index);
            if (!item || plist_get_node_type(item) != PLIST_DICT)
                throw std::runtime_error("RB nolist contains a non-dictionary item");
            bmt::RBExtendRelation relation;
            relation.extID = PlistUInt(item, "ExtID");
            relation.packID = PlistUInt(item, "PackID");
            relation.baseID = PlistUInt(item, "ID");
            relation.extLevel = PlistUInt(item, "ExtLevel");
            relation.comment = PlistString(item, "Comment");
            relation.extURL = PlistString(item, "ExtURL");
            relation.extURL2 = PlistString(item, "ExtURL2");
            relation.hasPackID = plist_dict_get_item(item, "PackID") != nullptr;
            relation.dlcType = dlcType;
            relation.dlcOrder = dlcOrder;
            if (!relation.extID || !relation.baseID)
                throw std::runtime_error("RB nolist item has no ExtID or ID");
            output.push_back(std::move(relation));
        }
        return output;
    }

    std::vector<bmt::Playlist> LoadRBPlaylists(const fs::path& directory)
    {
        const fs::path plaintext = directory / "playlist.plist";
        if (fs::is_regular_file(plaintext))
            return bmt::LoadPlaylists(plaintext);
        const fs::path runtime = directory / "playlist";
        if (fs::is_regular_file(runtime))
            return bmt::LoadPlaylists(runtime);
        return {};
    }

    std::array<uint8_t, 32> RBContentHash(bmt::RBMusicPack& pack)
    {
        std::vector<bmt::detail::NamedByteSpan> resources;
        std::vector<bmt::PackResource*> transient;
        resources.reserve(pack.resources.size());
        for (auto& [name, resource] : pack.resources)
        {
            if (name == "info")
                continue;
            const bool materialized = resource.IsMaterialized();
            const auto& data = resource.Data();
            resources.push_back({name, data});
            if (!materialized)
                transient.push_back(&resource);
        }
        const auto digest = bmt::detail::NamedContentHash(resources, "RB");
        for (auto* resource : transient)
            resource->bytes.reset();
        return digest;
    }

    bool SameRBContent(bmt::RBMusicPack& left, bmt::RBMusicPack& right)
    {
        return RBContentHash(left) == RBContentHash(right);
    }

    void ApplyRBMapping(bmt::RBLoadResult& result,
                        const bmt::detail::IDMapping& mapping,
                        const fs::path& sourceDirectory)
    {
        std::set<uint32_t> loadedIDs;
        bmt::RBPackTable mapped;
        for (auto& [id, instances] : result.packs)
        {
            (void)id;
            for (auto& instance : instances)
            {
                auto pack = std::move(instance);
                loadedIDs.insert(pack.sourceFileID);
                const uint32_t finalID =
                    bmt::detail::MappedID(mapping, pack.sourceFileID);
                pack.id = finalID;
                pack.hotMainID = bmt::detail::MappedID(mapping, pack.hotMainID);
                if (pack.id != pack.originalID)
                {
                    result.remaps.push_back({pack.sourcePath, pack.originalID, pack.id});
                    RewriteInfoID(pack);
                }
                mapped[pack.id].push_back(std::move(pack));
            }
        }
        for (auto& [id, instances] : mapped)
        {
            if (instances.size() < 2)
                continue;
            auto& winner = instances.front();
            for (size_t index = 1; index < instances.size(); ++index)
            {
                if (!SameRBContent(winner, instances[index]))
                    throw std::runtime_error(
                        "conflicting file " + instances[index].sourcePath.filename().string() +
                        " requires an entry in " +
                        (sourceDirectory / "mapping.json").string());
                ++result.droppedDuplicates;
            }
            instances.erase(instances.begin() + 1, instances.end());
        }
        for (const auto& [oldID, newID] : mapping)
        {
            (void)newID;
            if (!loadedIDs.contains(oldID))
                throw std::runtime_error("mapping.json in " + sourceDirectory.string() +
                                         " references missing file ID " +
                                         std::to_string(oldID));
        }
        for (auto& entry : result.catalog)
            entry.id = bmt::detail::MappedID(mapping, entry.id);
        for (auto& relation : result.extensions)
        {
            relation.extID = bmt::detail::MappedID(mapping, relation.extID);
            relation.baseID = bmt::detail::MappedID(mapping, relation.baseID);
        }
        for (auto& playlist : result.playlists)
            for (auto& id : playlist.musicIDs)
                id = bmt::detail::MappedID(mapping, id);
        result.packs = std::move(mapped);
    }

    std::vector<std::vector<uint32_t>> RBRelationshipComponents(
        const bmt::RBLoadResult& result)
    {
        std::map<uint32_t, uint32_t> parent;
        for (const auto& [id, instances] : result.packs)
        {
            (void)instances;
            parent[id] = id;
        }
        auto find = [&](uint32_t id)
        {
            uint32_t root = id;
            while (parent.at(root) != root)
                root = parent.at(root);
            while (parent.at(id) != id)
            {
                const uint32_t next = parent.at(id);
                parent[id] = root;
                id = next;
            }
            return root;
        };
        auto unite = [&](uint32_t left, uint32_t right)
        {
            if (!parent.contains(left) || !parent.contains(right))
                return;
            const uint32_t leftRoot = find(left);
            const uint32_t rightRoot = find(right);
            if (leftRoot != rightRoot)
                parent[rightRoot] = leftRoot;
        };
        for (const auto& relation : result.extensions)
            unite(relation.baseID, relation.extID);
        for (const auto& [id, instances] : result.packs)
            if (!instances.empty() && instances.front().hotMainID &&
                instances.front().hotMainID != id)
                unite(instances.front().hotMainID, id);

        std::map<uint32_t, std::vector<uint32_t>> grouped;
        for (const auto& [id, ignored] : parent)
        {
            (void)ignored;
            grouped[find(id)].push_back(id);
        }
        std::vector<std::vector<uint32_t>> output;
        output.reserve(grouped.size());
        for (auto& [root, ids] : grouped)
        {
            (void)root;
            output.push_back(std::move(ids));
        }
        return output;
    }

    void MergeRBSource(bmt::RBLoadResult& result,
                       bmt::RBLoadResult& source,
                       const fs::path& sourceDirectory)
    {
        for (const auto& component : RBRelationshipComponents(source))
        {
            for (const uint32_t id : component)
            {
                const auto existing = result.packs.find(id);
                if (existing == result.packs.end())
                    continue;
                auto& incoming = source.packs.at(id).front();
                auto& winner = existing->second.front();
                if (!SameRBContent(winner, incoming))
                {
                    if (incoming.id != incoming.originalID)
                        throw std::runtime_error("mapping.json target " + std::to_string(id) +
                                                 " is already occupied while loading " +
                                                 sourceDirectory.string());
                    throw std::runtime_error("conflicting RB ID " + std::to_string(id) +
                                             " from " + incoming.sourcePath.string() +
                                             " requires an entry in " +
                                             (sourceDirectory / "mapping.json").string());
                }
            }
            for (const uint32_t id : component)
            {
                auto existing = result.packs.find(id);
                if (existing == result.packs.end())
                    result.packs.emplace(id, std::move(source.packs.at(id)));
                else
                    ++result.droppedDuplicates;
            }
        }
        result.catalog.insert(result.catalog.end(),
                              std::make_move_iterator(source.catalog.begin()),
                              std::make_move_iterator(source.catalog.end()));
        result.extensions.insert(result.extensions.end(),
                                 std::make_move_iterator(source.extensions.begin()),
                                 std::make_move_iterator(source.extensions.end()));
        result.playlists.insert(result.playlists.end(),
                                std::make_move_iterator(source.playlists.begin()),
                                std::make_move_iterator(source.playlists.end()));
        result.remaps.insert(result.remaps.end(),
                             std::make_move_iterator(source.remaps.begin()),
                             std::make_move_iterator(source.remaps.end()));
        result.droppedDuplicates += source.droppedDuplicates;
    }

    void ValidateRBID(uint32_t id, std::string_view description)
    {
        if (id < MinimumRuntimeID || id > MaximumRuntimeID)
            throw std::runtime_error(std::string(description) + " " + std::to_string(id) +
                                     " must be an unpadded nine-digit-or-longer ID not exceeding " +
                                     std::to_string(MaximumRuntimeID));
    }

    void AddRBWarning(bmt::RBLoadResult& result,
                      const fs::path& path,
                      std::string message)
    {
        const auto duplicate = std::find_if(
            result.warnings.begin(), result.warnings.end(),
            [&](const bmt::Diagnostic& warning)
            {
                return warning.path == path && warning.message == message;
            });
        if (duplicate == result.warnings.end())
            result.warnings.push_back({path, std::move(message)});
    }

    void ValidateRBRelationships(bmt::RBLoadResult& result,
                                 bmt::FailureMode failureMode)
    {
        std::set<std::pair<uint32_t, uint32_t>> relationsWithPackID;
        for (const auto& relation : result.extensions)
            if (relation.hasPackID)
                relationsWithPackID.emplace(relation.baseID, relation.extID);
        for (const auto& relation : result.extensions)
        {
            if (result.packs.contains(relation.baseID) &&
                result.packs.contains(relation.extID))
                continue;
            const std::string message =
                "omitting dangling nolist relation " +
                std::to_string(relation.baseID) + " -> " +
                std::to_string(relation.extID);
            AddRBWarning(result, {}, message);
            if (failureMode == bmt::FailureMode::Strict)
                throw std::runtime_error(message);
        }
        for (const auto& [id, instances] : result.packs)
        {
            const auto& pack = instances.front();
            if (!pack.hotMainID || pack.hotMainID == id)
                continue;
            if (!result.packs.contains(pack.hotMainID))
            {
                const std::string message =
                    "RBHot extension " + std::to_string(id) +
                    " is missing base pack " + std::to_string(pack.hotMainID);
                AddRBWarning(result, pack.sourcePath, message);
                if (failureMode == bmt::FailureMode::Strict)
                    throw std::runtime_error(message);
            }
            if (!relationsWithPackID.contains({pack.hotMainID, id}))
                AddRBWarning(
                    result, pack.sourcePath,
                    "omitting nolist relation " + std::to_string(pack.hotMainID) +
                    " -> " + std::to_string(id) +
                    " because no source PackID is available");
        }
    }

    std::string RBFileName(uint32_t id)
    {
        std::ostringstream stream;
        stream << std::setfill('0') << std::setw(9) << id << ".rb";
        return stream.str();
    }

    std::vector<uint8_t> BuildRBCatalog(
        bmt::RBLoadResult& result,
        const std::set<uint32_t>& extensionIDs)
    {
        std::map<uint32_t, const bmt::RBCatalogEntry*> catalog;
        for (const auto& entry : result.catalog)
            catalog.try_emplace(entry.id, &entry);
        PlistPtr root(plist_new_array());
        for (auto& [id, instances] : result.packs)
        {
            if (extensionIDs.contains(id))
                continue;
            auto& pack = instances.front();
            const auto found = catalog.find(id);
            const bmt::RBCatalogEntry* source =
                found == catalog.end() ? nullptr : found->second;
            plist_t item = plist_new_dict();
            plist_dict_set_item(item, "ID", plist_new_uint(id));
            const std::string& name =
                source && !source->name.empty() ? source->name : pack.musicName;
            const std::string& artist =
                source && !source->artist.empty() ? source->artist : pack.artistName;
            plist_dict_set_item(item, "Name", plist_new_string(name.c_str()));
            plist_dict_set_item(item, "Artist", plist_new_string(artist.c_str()));
            if (source && !source->itemURL.empty())
                plist_dict_set_item(item, "ItemURL",
                                    plist_new_string(source->itemURL.c_str()));
            if (source && !source->iTunesURL.empty())
                plist_dict_set_item(item, "iTunesURL",
                                    plist_new_string(source->iTunesURL.c_str()));
            plist_array_append_item(root.get(), item);
        }
        return SerializePlist(root.get(), PLIST_FORMAT_XML);
    }

    std::vector<uint8_t> BuildRBExtensions(bmt::RBLoadResult& result,
                                           std::set<uint32_t>& extensionIDs)
    {
        using RelationKey = std::pair<uint32_t, uint32_t>;
        std::map<RelationKey, bmt::RBExtendRelation> relations;
        std::map<uint32_t, uint32_t> baseByExtension;
        for (const auto& relation : result.extensions)
        {
            extensionIDs.insert(relation.extID);
            const RelationKey key{relation.baseID, relation.extID};
            const auto owner = baseByExtension.find(relation.extID);
            if (owner != baseByExtension.end() && owner->second != relation.baseID)
            {
                AddRBWarning(
                    result, {},
                    "omitting duplicate nolist owner " +
                    std::to_string(relation.baseID) + " -> " +
                    std::to_string(relation.extID) + "; extension is already owned by " +
                    std::to_string(owner->second));
                continue;
            }
            baseByExtension.emplace(relation.extID, relation.baseID);
            const auto found = relations.find(key);
            if (found == relations.end() ||
                (!found->second.hasPackID && relation.hasPackID))
                relations[key] = relation;
        }
        for (const auto& [id, instances] : result.packs)
        {
            const auto& pack = instances.front();
            if (!pack.hotMainID || pack.hotMainID == id)
                continue;
            extensionIDs.insert(id);
            const RelationKey key{pack.hotMainID, id};
            const auto owner = baseByExtension.find(id);
            if (owner != baseByExtension.end() && owner->second != pack.hotMainID)
            {
                AddRBWarning(
                    result, pack.sourcePath,
                    "ignoring RBHot mainId " + std::to_string(pack.hotMainID) +
                    " for extension " + std::to_string(id) +
                    "; source nolist assigns it to " +
                    std::to_string(owner->second));
                continue;
            }
            baseByExtension.emplace(id, pack.hotMainID);
            if (!relations.contains(key))
            {
                bmt::RBExtendRelation relation;
                relation.baseID = pack.hotMainID;
                relation.extID = id;
                relation.extLevel = pack.basic;
                relation.dlcType = pack.dlcType;
                relation.dlcOrder = pack.dlcOrder;
                relations.emplace(key, std::move(relation));
            }
        }

        PlistPtr root(plist_new_array());
        for (auto& [key, relation] : relations)
        {
            (void)key;
            const auto base = result.packs.find(relation.baseID);
            const auto extension = result.packs.find(relation.extID);
            if (base == result.packs.end() || extension == result.packs.end())
            {
                const fs::path path = extension == result.packs.end()
                    ? fs::path{} : extension->second.front().sourcePath;
                const std::string message =
                    "omitting dangling nolist relation " +
                    std::to_string(relation.baseID) + " -> " +
                    std::to_string(relation.extID);
                AddRBWarning(result, path, message);
                continue;
            }
            if (!relation.hasPackID)
            {
                AddRBWarning(
                    result, extension->second.front().sourcePath,
                    "omitting nolist relation " + std::to_string(relation.baseID) +
                    " -> " + std::to_string(relation.extID) +
                    " because no source PackID is available");
                continue;
            }
            if (!relation.extLevel)
                relation.extLevel = extension->second.front().basic;
            plist_t item = plist_new_dict();
            plist_dict_set_item(item, "ExtID", plist_new_uint(relation.extID));
            plist_dict_set_item(item, "PackID", plist_new_uint(relation.packID));
            plist_dict_set_item(item, "ID", plist_new_uint(relation.baseID));
            plist_dict_set_item(item, "ExtLevel", plist_new_uint(relation.extLevel));
            if (!relation.comment.empty())
                plist_dict_set_item(item, "Comment",
                                    plist_new_string(relation.comment.c_str()));
            if (!relation.extURL.empty())
                plist_dict_set_item(item, "ExtURL",
                                    plist_new_string(relation.extURL.c_str()));
            if (!relation.extURL2.empty())
                plist_dict_set_item(item, "ExtURL2",
                                    plist_new_string(relation.extURL2.c_str()));
            plist_array_append_item(root.get(), item);
        }
        return SerializePlist(root.get(), PLIST_FORMAT_XML);
    }

    bool IsPlaylistID(std::string_view value) noexcept
    {
        return value.size() == 32 &&
               std::all_of(value.begin(), value.end(), [](unsigned char character)
               {
                   return (character >= '0' && character <= '9') ||
                          (character >= 'a' && character <= 'f') ||
                          (character >= 'A' && character <= 'F');
               });
    }

    std::string GeneratePlaylistID()
    {
        const auto bytes = bmt::detail::RandomBytes(16);
        static constexpr char Hex[] = "0123456789abcdef";
        std::string output;
        output.reserve(32);
        for (const auto byte : bytes)
        {
            output.push_back(Hex[byte >> 4]);
            output.push_back(Hex[byte & 15]);
        }
        return output;
    }

    std::vector<uint8_t> BuildRBPlaylists(std::vector<bmt::Playlist>& playlists)
    {
        PlistPtr root(plist_new_array());
        for (auto& playlist : playlists)
        {
            if (playlist.id.empty())
                playlist.id = GeneratePlaylistID();
            else if (!IsPlaylistID(playlist.id))
                throw std::runtime_error("RB playlist PLID is not a 32-character hex string");
            plist_t item = plist_new_dict();
            plist_dict_set_item(item, "PLID", plist_new_string(playlist.id.c_str()));
            plist_dict_set_item(item, "NAME", plist_new_string(playlist.name.c_str()));
            plist_t list = plist_new_array();
            for (const auto id : playlist.musicIDs)
                plist_array_append_item(list, plist_new_uint(id));
            plist_dict_set_item(item, "LIST", list);
            plist_array_append_item(root.get(), item);
        }
        return SerializePlist(root.get(), PLIST_FORMAT_XML);
    }
}

namespace bmt
{
    RBLoadResult LoadRBPacks(const std::vector<RBSource>& sources,
                             const RBLoadOptions& options)
    {
        if (sources.empty())
            throw std::invalid_argument("at least one RB DLC source is required");
        size_t officialCount = 0;
        size_t hotCount = 0;
        std::set<fs::path> directories;
        for (const auto& source : sources)
        {
            if (!fs::is_directory(source.directory))
                throw std::invalid_argument("RB DLC source is not a directory: " +
                                            source.directory.string());
            const auto normalized = fs::absolute(source.directory).lexically_normal();
            if (!directories.insert(normalized).second)
                throw std::invalid_argument("the same RB DLC directory was specified twice");
            officialCount += source.type == DLCType::Official;
            hotCount += source.type == DLCType::JBHot;
        }
        if (officialCount > 1 || hotCount > 1)
            throw std::invalid_argument("only one Official and one RBHot source are allowed");
        if (hotCount && !options.rbhotDefaultsPlist)
            throw std::invalid_argument("RBHot source requires rbhotDefaultsPlist");

        std::vector<RBSource> orderedSources = sources;
        const auto sourceRank = [](DLCType type)
        {
            switch (type)
            {
            case DLCType::Official:
                return 0;
            case DLCType::JBHot:
                return 1;
            case DLCType::Custom:
                return 2;
            }
            return 3;
        };
        std::stable_sort(orderedSources.begin(), orderedSources.end(),
                         [&](const RBSource& left, const RBSource& right)
                         {
                             return sourceRank(left.type) < sourceRank(right.type);
                         });

        RBHotDefaultsData hotDefaults;
        auto hotMusic = std::make_shared<RBHotMap>();
        if (options.rbhotDefaultsPlist)
        {
            hotDefaults = LoadRBHotDefaults(*options.rbhotDefaultsPlist);
            *hotMusic = hotDefaults.music;
        }

        RBLoadResult result;
        for (size_t sourceIndex = 0; sourceIndex < orderedSources.size(); ++sourceIndex)
        {
            const auto& source = orderedSources[sourceIndex];
            RBLoadResult current;
            current.catalog = ParseRBCatalog(
                LoadListBytes(source.directory, "mulist.plist", "mulist",
                              options.mulistKey));
            current.extensions = ParseRBExtensions(
                LoadListBytes(source.directory, "nolist.plist", "nolist",
                              options.mulistKey),
                source.type, sourceIndex);
            current.playlists = source.type == DLCType::JBHot
                ? hotDefaults.playlists : LoadRBPlaylists(source.directory);
            for (auto& playlist : current.playlists)
            {
                playlist.dlcType = source.type;
                playlist.dlcOrder = sourceIndex;
            }

            std::vector<fs::path> files;
            for (const auto& entry : fs::directory_iterator(source.directory))
                if (entry.is_regular_file() && entry.path().extension() == ".rb")
                    files.push_back(entry.path());
            std::sort(files.begin(), files.end());
            for (const auto& path : files)
            {
                try
                {
                    auto pack = LoadOneRB(path, options, hotMusic);
                    pack.dlcType = source.type;
                    pack.dlcOrder = sourceIndex;
                    current.packs[pack.id].push_back(std::move(pack));
                }
                catch (const std::exception& error)
                {
                    result.diagnostics.push_back({path, error.what()});
                    if (options.failureMode == FailureMode::Strict)
                        throw;
                }
            }
            if (source.type == DLCType::JBHot)
            {
                for (const auto& [id, instances] : current.packs)
                {
                    (void)instances;
                    const auto found = hotMusic->find(id);
                    if (found != hotMusic->end())
                        current.catalog.push_back(found->second.catalog);
                }
            }
            const auto mapping = detail::LoadIDMapping(
                source.directory, MinimumRuntimeID, MaximumRuntimeID);
            ApplyRBMapping(current, mapping, source.directory);
            MergeRBSource(result, current, source.directory);
        }
        for (const auto& [id, instances] : result.packs)
        {
            ValidateRBID(id, "final RB ID");
            if (instances.size() != 1)
                throw std::runtime_error("unresolved duplicate RB ID " +
                                         std::to_string(id));
        }
        ValidateRBRelationships(result, options.failureMode);
        return result;
    }

    void ExportRBPacks(RBLoadResult& result,
                       const fs::path& outputDirectory,
                       const RBExportOptions& options)
    {
        for (const auto& [id, instances] : result.packs)
        {
            ValidateRBID(id, "final RB ID");
            if (instances.size() != 1)
                throw std::runtime_error("cannot export unresolved RB ID " +
                                         std::to_string(id));
        }

        std::set<uint32_t> extensionIDs;
        const auto nolist = BuildRBExtensions(result, extensionIDs);
        const auto mulist = BuildRBCatalog(result, extensionIDs);
        const auto playlist = result.playlists.empty()
            ? std::vector<uint8_t>{} : BuildRBPlaylists(result.playlists);

        fs::create_directories(outputDirectory);
        std::set<size_t> customOrders;
        if (options.separateByDLC)
            for (const auto& [id, instances] : result.packs)
            {
                (void)id;
                if (instances.front().dlcType == DLCType::Custom)
                    customOrders.insert(instances.front().dlcOrder);
            }
        std::map<size_t, size_t> customNumbers;
        size_t number = 1;
        for (const auto order : customOrders)
            customNumbers[order] = number++;
        const auto packDirectory = [&](const RBMusicPack& pack)
        {
            if (!options.separateByDLC)
                return outputDirectory;
            if (pack.dlcType == DLCType::Official)
                return outputDirectory / "official";
            if (pack.dlcType == DLCType::JBHot)
                return outputDirectory / "rbhot";
            return outputDirectory /
                   ("custom-" + std::to_string(customNumbers.at(pack.dlcOrder)));
        };

        for (auto& [id, instances] : result.packs)
        {
            auto& pack = instances.front();
            const auto directory = packDirectory(pack);
            fs::create_directories(directory);
            std::optional<uint8_t> decodeType;
            if (options.encryptRB)
            {
                switch (options.outputKey)
                {
                case RBOutputKey::Preserve:
                    decodeType = pack.decodeType.value_or(0);
                    break;
                case RBOutputKey::Type0:
                    decodeType = 0;
                    break;
                case RBOutputKey::Type1:
                    decodeType = 1;
                    break;
                }
            }
            WriteRB(pack, directory / RBFileName(id), decodeType);
        }
        WriteFile(outputDirectory / "mulist.plist", mulist);
        WriteFile(outputDirectory / "nolist.plist", nolist);
        if (!playlist.empty())
            WriteFile(outputDirectory / "playlist.plist", playlist);
        if (options.mulistKey)
        {
            WriteFile(outputDirectory / "mulist",
                      detail::EncryptPrefixedBFContainer(mulist, *options.mulistKey));
            WriteFile(outputDirectory / "nolist",
                      detail::EncryptPrefixedBFContainer(nolist, *options.mulistKey));
            if (!playlist.empty())
                WriteFile(outputDirectory / "playlist", playlist);
        }
    }
}
