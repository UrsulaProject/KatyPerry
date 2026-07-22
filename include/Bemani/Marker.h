#ifndef BMT_MARKER_H
#define BMT_MARKER_H

#include <Bemani/JBT.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bmt
{
    enum class MarkerFormat
    {
        Plain,
        OfficialBF,
        JBHot,
    };

    struct MarkerFrame
    {
        std::string name;
        std::vector<uint8_t> png;
    };

    struct MarkerPack
    {
        uint32_t originalID = 0;
        uint32_t id = 0;
        std::filesystem::path sourcePath;
        std::filesystem::path bannerPath;
        MarkerFormat format = MarkerFormat::Plain;
        DLCType dlcType = DLCType::Custom;
        size_t dlcOrder = 0;
        std::string version = "1.0.0";
        std::vector<MarkerFrame> frames;
        std::optional<std::vector<uint8_t>> banner;
    };

    struct MarkerListEntry
    {
        std::string markerID;
        std::string bannerName;
        std::string version = "1.0.0";
    };

    struct MarkerSource
    {
        DLCType type = DLCType::Custom;
        std::filesystem::path directory;
    };

    struct MarkerLoadOptions
    {
        FailureMode failureMode = FailureMode::Continue;
    };

    struct MarkerLoadResult
    {
        std::map<uint32_t, MarkerPack> packs;
        std::vector<Diagnostic> diagnostics;
        std::vector<IDRemap> remaps;
        size_t droppedDuplicates = 0;
    };

    struct MarkerExportOptions
    {
        std::optional<std::filesystem::path> markerListOutput;
    };

    void DecryptMarker(const std::filesystem::path& inputZip,
                       const std::filesystem::path& outputZip);
    void EncryptMarker(const std::filesystem::path& inputZip,
                       const std::filesystem::path& outputZip);
    void UnpackMarker(const std::filesystem::path& inputZip,
                      const std::filesystem::path& outputDirectory);
    void PackMarker(const std::filesystem::path& inputDirectory,
                    const std::filesystem::path& outputZip);
    void UnpackMarkerDirectory(const std::filesystem::path& inputDirectory,
                               const std::filesystem::path& outputDirectory);
    void PackMarkerDirectory(const std::filesystem::path& inputDirectory,
                             const std::filesystem::path& outputDirectory);

    MarkerLoadResult LoadMarkers(const std::vector<MarkerSource>& sources,
                                 const MarkerLoadOptions& options = {});
    void ExportMarkers(const MarkerLoadResult& result,
                       const std::filesystem::path& outputDirectory,
                       const MarkerExportOptions& options = {});

    std::vector<MarkerListEntry> LoadMarkerListXML(const std::filesystem::path& path);
    std::vector<uint8_t> BuildMarkerListXML(const std::vector<MarkerListEntry>& entries);
    std::vector<MarkerListEntry> DecryptMarkerList(const std::filesystem::path& input);
}

#endif
