#ifndef BMT_RB_H
#define BMT_RB_H

#include <Bemani/JBT.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bmt
{
    enum class RBPackFormat
    {
        Plain,
        OfficialBF,
        RBHot,
    };

    enum class RBOutputKey
    {
        Preserve,
        Type0,
        Type1,
    };

    struct RBCatalogEntry
    {
        uint32_t id = 0;
        std::string name;
        std::string artist;
        std::string itemURL;
        std::string iTunesURL;
    };

    struct RBExtendRelation
    {
        uint32_t extID = 0;
        uint32_t packID = 0;
        uint32_t baseID = 0;
        uint32_t extLevel = 0;
        std::string comment;
        std::string extURL;
        std::string extURL2;
        bool hasPackID = false;
        DLCType dlcType = DLCType::Custom;
        size_t dlcOrder = 0;
    };

    struct RBMusicPack
    {
        uint32_t sourceFileID = 0;
        uint32_t originalID = 0;
        uint32_t id = 0;
        std::filesystem::path sourcePath;
        RBPackFormat format = RBPackFormat::Plain;
        std::optional<uint8_t> decodeType;
        DLCType dlcType = DLCType::Custom;
        size_t dlcOrder = 0;
        std::string musicName;
        std::string musicNameHira;
        std::string musicNameRoman;
        std::string artistName;
        std::string artistNameHira;
        std::string artistNameRoman;
        uint32_t basic = 0;
        uint32_t medium = 0;
        uint32_t hard = 0;
        uint32_t bpmMin = 0;
        uint32_t bpmMax = 0;
        uint32_t hotMainID = 0;
        std::map<std::string, PackResource> resources;
    };

    using RBPackTable = std::map<uint32_t, std::vector<RBMusicPack>>;

    struct RBLoadOptions
    {
        LoadMode mode = LoadMode::Lazy;
        FailureMode failureMode = FailureMode::Continue;
        std::optional<std::filesystem::path> rbhotDefaultsPlist;
        std::optional<std::string> mulistKey;
    };

    struct RBSource
    {
        DLCType type = DLCType::Custom;
        std::filesystem::path directory;
    };

    struct RBLoadResult
    {
        RBPackTable packs;
        std::vector<RBCatalogEntry> catalog;
        std::vector<RBExtendRelation> extensions;
        std::vector<Playlist> playlists;
        std::vector<Diagnostic> diagnostics;
        std::vector<Diagnostic> warnings;
        std::vector<IDRemap> remaps;
        size_t droppedDuplicates = 0;
    };

    struct RBExportOptions
    {
        bool encryptRB = true;
        RBOutputKey outputKey = RBOutputKey::Preserve;
        std::optional<std::string> mulistKey;
        bool separateByDLC = false;
    };

    RBLoadResult LoadRBPacks(const std::vector<RBSource>& sources,
                             const RBLoadOptions& options = {});
    void ExportRBPacks(RBLoadResult& result,
                       const std::filesystem::path& outputDirectory,
                       const RBExportOptions& options = {});

    void DecryptRB(const std::filesystem::path& inputRB,
                   const std::filesystem::path& outputRB,
                   const RBLoadOptions& options = {});
    void EncryptRB(const std::filesystem::path& inputRB,
                   const std::filesystem::path& outputRB,
                   uint8_t decodeType = 0,
                   const RBLoadOptions& options = {});
    void UnpackRB(const std::filesystem::path& inputRB,
                  const std::filesystem::path& outputDirectory,
                  const RBLoadOptions& options = {});
    void PackRB(const std::filesystem::path& inputDirectory,
                const std::filesystem::path& outputRB,
                std::optional<uint8_t> decodeType = uint8_t{0});
    void UnpackRBDirectory(const std::filesystem::path& inputDirectory,
                           const std::filesystem::path& outputDirectory,
                           const RBLoadOptions& options = {});
    void PackRBDirectory(const std::filesystem::path& inputDirectory,
                         const std::filesystem::path& outputDirectory,
                         std::optional<uint8_t> decodeType = uint8_t{0});

    std::map<std::string, std::string> DumpRBHotDefaults(
        const std::filesystem::path& defaultsPlist);

    std::vector<uint8_t> DecryptRBList(const std::filesystem::path& encryptedPath,
                                       std::string_view key);
    std::vector<uint8_t> EncryptRBList(const std::filesystem::path& plaintextPath,
                                       std::string_view key);
}

#endif
