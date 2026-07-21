#ifndef BMT_JBT_H
#define BMT_JBT_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bmt
{
    enum class PackFormat
    {
        Plain,
        OfficialBF,
        JBHot,
    };

    enum class InfoRevision
    {
        Info,
        InfoV2,
        InfoV3,
    };

    enum class LoadMode
    {
        Lazy,
        Eager,
    };

    enum class FailureMode
    {
        Continue,
        Strict,
    };

    enum class CatalogSource
    {
        None,
        Official,
        JBHot,
    };

    struct PackResource
    {
        std::string name;
        mutable std::optional<std::vector<uint8_t>> bytes;
        std::function<std::vector<uint8_t>()> lazyLoader;

        const std::vector<uint8_t>& Data() const;
        bool IsMaterialized() const noexcept { return bytes.has_value(); }
    };

    struct CatalogEntry
    {
        uint32_t id = 0;
        std::string name;
        std::string artist;
        std::string itemURL;
        std::string iTunesURL;
        uint32_t extID = 0;
        std::string extURL;
        uint32_t extendFlag = 0;
        uint32_t holdFlag = 0;
        bool hasExtendFlag = false;
        bool hasHoldFlag = false;
        uint32_t originalID = 0;
    };

    struct MusicPack
    {
        uint32_t originalID = 0;
        uint32_t id = 0;
        std::filesystem::path sourcePath;
        PackFormat format = PackFormat::Plain;
        CatalogSource catalogSource = CatalogSource::None;
        InfoRevision infoRevision = InfoRevision::InfoV2;
        std::string infoMember;
        std::string name;
        std::string nameYomi;
        std::string artist;
        std::string artistYomi;
        uint32_t levelBasic = 0;
        uint32_t levelAdvanced = 0;
        uint32_t levelExtreme = 0;
        uint32_t extID = 0;
        uint32_t baseID = 0;
        uint32_t extendFlag = 0;
        uint32_t holdFlag = 0;
        bool hasExtendFlag = false;
        bool hasHoldFlag = false;
        std::string itemURL;
        std::string extURL;
        std::string iTunesURL;
        std::map<std::string, PackResource> resources;
    };

    using PackTable = std::map<uint32_t, std::vector<MusicPack>>;

    struct Playlist
    {
        std::string id;
        std::string name;
        std::vector<uint32_t> musicIDs;
    };

    struct Diagnostic
    {
        std::filesystem::path path;
        std::string message;
    };

    struct LoadOptions
    {
        LoadMode mode = LoadMode::Lazy;
        FailureMode failureMode = FailureMode::Continue;
        std::optional<std::filesystem::path> jbhotDefaultsPlist;
        std::optional<std::filesystem::path> musicDataJson;
        std::optional<std::filesystem::path> serverDataJson;
        std::optional<std::filesystem::path> catalogPlist;
    };

    struct LoadResult
    {
        PackTable packs;
        std::vector<CatalogEntry> catalog;
        std::vector<Playlist> playlists;
        std::vector<Diagnostic> diagnostics;
    };

    struct ResolveOptions
    {
        uint32_t firstReservedID = 600000000;
        uint32_t lastReservedID = 899999999;
    };

    struct IDRemap
    {
        std::filesystem::path sourcePath;
        uint32_t oldID = 0;
        uint32_t newID = 0;
    };

    LoadResult LoadPacks(const std::vector<std::filesystem::path>& inputs,
                         const LoadOptions& options = {});
    std::vector<IDRemap> ResolveConflicts(PackTable& packs,
                                          const ResolveOptions& options = {});
    std::vector<IDRemap> ResolveConflicts(LoadResult& result,
                                          const ResolveOptions& options = {});
    void ExportPacks(PackTable& packs, const std::filesystem::path& outputDirectory);
    void ExportPacks(LoadResult& result, const std::filesystem::path& outputDirectory);

    std::vector<CatalogEntry> LoadOfficialCatalog(const std::filesystem::path& plistPath);
    std::vector<Playlist> LoadJBHotPlaylists(const std::filesystem::path& serverDataJson);
    std::vector<uint8_t> DecryptOfficialMusicList(const std::filesystem::path& encryptedPath,
                                                  const std::filesystem::path& keychainDump,
                                                  std::string_view bundleID = "jp.konami.jubeatplus");
}

#endif
