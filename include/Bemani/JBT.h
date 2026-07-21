#ifndef BMT_JBT_H
#define BMT_JBT_H

#include <cstddef>
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

    enum class DLCType
    {
        Official,
        JBHot,
        Custom,
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
        DLCType dlcType = DLCType::Custom;
        size_t dlcOrder = 0;
        uint32_t customFirstID = 0;
        uint32_t customLastID = 0;
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
        std::optional<std::filesystem::path> catalogPlist;
    };

    struct DLCSource
    {
        DLCType type = DLCType::Custom;
        std::filesystem::path directory;
        uint32_t firstID = 0;
        uint32_t lastID = 0;
    };

    struct LoadResult
    {
        PackTable packs;
        std::vector<CatalogEntry> catalog;
        std::vector<Playlist> playlists;
        std::vector<Diagnostic> diagnostics;
        std::vector<Diagnostic> warnings;
        size_t droppedDuplicates = 0;
    };

    struct ResolveOptions
    {
        uint32_t firstReservedID = 600000000;
        uint32_t lastReservedID = 899999999;
    };

    struct ExportOptions
    {
        bool encryptJBT = true;
        std::optional<std::string> mulistKey;
    };

    struct IDRemap
    {
        std::filesystem::path sourcePath;
        uint32_t oldID = 0;
        uint32_t newID = 0;
    };

    LoadResult LoadPacks(const std::vector<DLCSource>& sources,
                         const LoadOptions& options = {});
    std::vector<IDRemap> ResolveConflicts(LoadResult& result,
                                          const ResolveOptions& options = {});
    void ExportPacks(LoadResult& result,
                     const std::filesystem::path& outputDirectory,
                     const ExportOptions& options = {});

    std::vector<CatalogEntry> LoadOfficialCatalog(const std::filesystem::path& plistPath);
    std::vector<uint8_t> DecryptOfficialMusicList(const std::filesystem::path& encryptedPath,
                                                  const std::filesystem::path& keychainDump,
                                                  std::string_view bundleID = "jp.konami.jubeatplus");
}

#endif
