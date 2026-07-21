#include <Bemani/BFContainer.h>
#include <Bemani/JBT.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

int main()
{
    const std::vector<uint8_t> plaintext = {
        'b', 'p', 'l', 'i', 's', 't', '0', '0', 0, 1, 2, 3, 4, 5, 6, 7, 8
    };
    const auto encrypted = bmt::EncryptBFContainer(plaintext, "Konami Bemani Mobile iPad");
    assert(bmt::IsBFContainer(encrypted));
    assert(bmt::DecryptBFContainer(encrypted, "Konami Bemani Mobile iPad") == plaintext);
    auto invalid = encrypted;
    invalid.back() ^= 1;
    assert(!bmt::IsBFContainer(invalid));

    bmt::PackTable packs;
    bmt::MusicPack first;
    first.originalID = first.id = 100;
    first.sourcePath = "/one/000000100.jbt";
    bmt::MusicPack second = first;
    second.sourcePath = "/two/000000100.jbt";
    packs[100].push_back(first);
    packs[100].push_back(second);
    const auto remaps = bmt::ResolveConflicts(packs, {600000000, 600000010});
    assert(remaps.size() == 1);
    assert(packs.size() == 2);
    assert(packs.contains(100));
    assert(packs.contains(600000000));

    bmt::PackTable paired;
    bmt::MusicPack baseOne;
    baseOne.originalID = baseOne.id = 100;
    baseOne.extID = 200;
    baseOne.sourcePath = "/one/000000100.jbt";
    bmt::MusicPack baseTwo = baseOne;
    baseTwo.sourcePath = "/two/000000100.jbt";
    bmt::MusicPack extOne;
    extOne.originalID = extOne.id = 200;
    extOne.baseID = 100;
    extOne.sourcePath = "/one/000000200.jbt";
    bmt::MusicPack extTwo = extOne;
    extTwo.sourcePath = "/two/000000200.jbt";
    paired[100] = {baseOne, baseTwo};
    paired[200] = {extOne, extTwo};
    const auto pairRemaps = bmt::ResolveConflicts(paired, {600000000, 600000010});
    assert(pairRemaps.size() == 2);
    assert(paired.at(600000000).front().extID == 600000001);
    assert(paired.at(600000001).front().baseID == 600000000);

    bmt::LoadResult playlistConflict;
    bmt::MusicPack officialBase;
    officialBase.originalID = officialBase.id = 100;
    officialBase.sourcePath = "/official/000000100.jbt";
    officialBase.catalogSource = bmt::CatalogSource::Official;
    bmt::MusicPack hotBase = officialBase;
    hotBase.sourcePath = "/hot/000000100.jbt";
    hotBase.catalogSource = bmt::CatalogSource::JBHot;
    hotBase.extID = 200;
    bmt::MusicPack officialExtension;
    officialExtension.originalID = officialExtension.id = 200;
    officialExtension.sourcePath = "/official/000000200.jbt";
    officialExtension.catalogSource = bmt::CatalogSource::Official;
    bmt::MusicPack hotExtension = officialExtension;
    hotExtension.sourcePath = "/hot/000000200.jbt";
    hotExtension.catalogSource = bmt::CatalogSource::JBHot;
    hotExtension.baseID = 100;
    playlistConflict.packs[100] = {officialBase, hotBase};
    playlistConflict.packs[200] = {officialExtension, hotExtension};
    playlistConflict.playlists.push_back({"playlist-id", "JBHot songs", {100, 200, 999}});
    const auto playlistRemaps = bmt::ResolveConflicts(playlistConflict, {600000000, 600000010});
    assert(playlistRemaps.size() == 2);
    assert(playlistConflict.playlists.front().musicIDs ==
           (std::vector<uint32_t>{600000000, 600000001, 999}));

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path output = std::filesystem::temp_directory_path() /
                                         ("bmt-tests-" + std::to_string(unique));
    bmt::PackTable exportPacks;
    bmt::MusicPack exportPack;
    exportPack.originalID = exportPack.id = 123456789;
    exportPack.infoRevision = bmt::InfoRevision::InfoV2;
    exportPack.infoMember = "infov2";
    exportPack.name = "Test Song";
    exportPack.artist = "Test Artist";
    const std::string info =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<plist version=\"1.0\"><dict><key>Artist</key><string>Test Artist</string>"
        "<key>ID</key><integer>123456789</integer><key>LvAdv</key><integer>5</integer>"
        "<key>LvBas</key><integer>3</integer><key>LvExt</key><integer>8</integer>"
        "<key>Name</key><string>Test Song</string></dict></plist>";
    bmt::PackResource infoResource;
    infoResource.name = "infov2";
    infoResource.bytes = std::vector<uint8_t>(info.begin(), info.end());
    exportPack.resources.emplace("infov2", std::move(infoResource));
    bmt::PackResource sequenceResource;
    sequenceResource.name = "seq_bas";
    sequenceResource.bytes = std::vector<uint8_t>{'J', 'B', 'S', 'Q', 1, 2, 3, 4, 5};
    exportPack.resources.emplace("seq_bas", std::move(sequenceResource));
    exportPacks[exportPack.id].push_back(std::move(exportPack));

    bmt::MusicPack v3Pack;
    v3Pack.originalID = v3Pack.id = 123456790;
    v3Pack.infoRevision = bmt::InfoRevision::InfoV3;
    v3Pack.infoMember = "infov3";
    v3Pack.name = "V3 Song";
    v3Pack.artist = "V3 Artist";
    std::string v3Info = info;
    const auto idPosition = v3Info.find("123456789");
    assert(idPosition != std::string::npos);
    v3Info.replace(idPosition, 9, "123456790");
    bmt::PackResource v3Resource;
    v3Resource.name = "infov3";
    v3Resource.bytes = std::vector<uint8_t>{0xde, 0xad, 0xbe, 0xef};
    v3Resource.bytes->insert(v3Resource.bytes->end(), v3Info.begin(), v3Info.end());
    v3Pack.resources.emplace("infov3", std::move(v3Resource));
    exportPacks[v3Pack.id].push_back(std::move(v3Pack));

    bmt::MusicPack generatedPack;
    generatedPack.originalID = generatedPack.id = 123456791;
    generatedPack.name = "Generated Song";
    generatedPack.artist = "Generated Artist";
    generatedPack.levelBasic = 2;
    generatedPack.levelAdvanced = 5;
    generatedPack.levelExtreme = 9;
    exportPacks[generatedPack.id].push_back(std::move(generatedPack));

    bmt::MusicPack catalogBase;
    catalogBase.originalID = catalogBase.id = 123456792;
    catalogBase.extID = 123456793;
    catalogBase.name = "Paired Song";
    catalogBase.artist = "Paired Artist";
    bmt::MusicPack catalogExtension;
    catalogExtension.originalID = catalogExtension.id = 123456793;
    catalogExtension.baseID = 123456792;
    catalogExtension.extendFlag = 4;
    catalogExtension.holdFlag = 0;
    catalogExtension.hasExtendFlag = true;
    catalogExtension.hasHoldFlag = true;
    exportPacks[catalogBase.id].push_back(std::move(catalogBase));
    exportPacks[catalogExtension.id].push_back(std::move(catalogExtension));

    bmt::ExportPacks(exportPacks, output);
    auto loaded = bmt::LoadPacks({output}, {.mode = bmt::LoadMode::Eager,
                                            .failureMode = bmt::FailureMode::Strict});
    assert(loaded.packs.size() == 5);
    assert(loaded.packs.contains(123456789));
    assert(loaded.packs.at(123456789).front().resources.at("seq_bas").Data() ==
           (std::vector<uint8_t>{'J', 'B', 'S', 'Q', 1, 2, 3, 4, 5}));
    assert(loaded.packs.contains(123456790));
    assert(loaded.packs.at(123456790).front().infoRevision == bmt::InfoRevision::InfoV3);
    assert(loaded.packs.at(123456791).front().infoRevision == bmt::InfoRevision::InfoV2);
    const auto catalog = bmt::LoadOfficialCatalog(output / "mulist.plist");
    const auto extensionEntry = std::find_if(catalog.begin(), catalog.end(), [](const auto& entry)
    {
        return entry.id == 123456793;
    });
    assert(extensionEntry != catalog.end());
    assert(extensionEntry->extendFlag == 4);
    assert(extensionEntry->holdFlag == 0);
    assert(std::filesystem::is_regular_file(output / "mulist.plist"));

    // Some JBHot distributions contain BF-encrypted JBTs. Their catalog still
    // comes from musicData when there is no official mulist beside the packs.
    const auto hotDirectory = output / "hot";
    std::filesystem::create_directory(hotDirectory);
    std::filesystem::copy_file(output / "123456792.jbt", hotDirectory / "123456792.jbt");
    std::filesystem::copy_file(output / "123456793.jbt", hotDirectory / "123456793.jbt");
    const auto musicDataPath = output / "musicData.json";
    {
        std::ofstream musicData(musicDataPath);
        musicData
            << R"({"data":{"123456792":{"id":123456792,"title":"Paired Song","artist":"Paired Artist","extendId":123456793,"extendFlag":0,"holdFlag":0},"123456793":{"id":123456793,"title":"Paired Song [ 2 ]","artist":"Paired Artist","origId":123456792,"extendId":0,"extendFlag":7,"holdFlag":7}}})";
    }
    const auto serverDataPath = output / "serverData.json";
    {
        std::ofstream serverData(serverDataPath);
        serverData
            << R"({"data":{"playlist":[{"id":"playlist-id","list":[123456792,123456793,42],"name":"Test Playlist"}]}})";
    }
    auto hotLoaded = bmt::LoadPacks({hotDirectory}, {
        .mode = bmt::LoadMode::Eager,
        .failureMode = bmt::FailureMode::Strict,
        .musicDataJson = musicDataPath,
        .serverDataJson = serverDataPath,
    });
    assert(hotLoaded.packs.at(123456792).front().extID == 123456793);
    assert(hotLoaded.packs.at(123456793).front().baseID == 123456792);
    assert(hotLoaded.playlists.size() == 1);
    assert(hotLoaded.playlists.front().musicIDs ==
           (std::vector<uint32_t>{123456792, 123456793, 42}));
    bmt::ExportPacks(hotLoaded, output / "hot-export");
    assert(std::filesystem::is_regular_file(output / "hot-export" / "playlists.plist"));
    std::ifstream playlistInput(output / "hot-export" / "playlists.plist");
    const std::string playlistXML((std::istreambuf_iterator<char>(playlistInput)),
                                  std::istreambuf_iterator<char>());
    const auto listPosition = playlistXML.find("<key>LIST</key>");
    const auto namePosition = playlistXML.find("<key>NAME</key>");
    const auto idPositionInPlaylist = playlistXML.find("<key>PLID</key>");
    assert(listPosition < namePosition && namePosition < idPositionInPlaylist);
    assert(playlistXML.find("<integer>123456792</integer>") != std::string::npos);
    assert(playlistXML.find("<string>Test Playlist</string>") != std::string::npos);
    assert(playlistXML.find("<string>playlist-id</string>") != std::string::npos);
    const auto hotReloaded = bmt::LoadPacks({output / "hot-export"}, {
        .mode = bmt::LoadMode::Eager,
        .failureMode = bmt::FailureMode::Strict,
    });
    assert(hotReloaded.packs.size() == 2);

    bmt::PackTable invalidExport;
    bmt::MusicPack invalidPack;
    invalidPack.originalID = invalidPack.id = 123456794;
    invalidPack.name = "Invalid Song";
    bmt::PackResource invalidResource;
    invalidResource.name = "seq_bas";
    invalidResource.lazyLoader = []() -> std::vector<uint8_t>
    {
        throw std::runtime_error("intentional lazy resource failure");
    };
    invalidPack.resources.emplace("seq_bas", std::move(invalidResource));
    invalidExport[invalidPack.id].push_back(std::move(invalidPack));
    bool rejectedInvalidExport = false;
    try
    {
        bmt::ExportPacks(invalidExport, output / "invalid-export");
    }
    catch (const std::runtime_error&)
    {
        rejectedInvalidExport = true;
    }
    assert(rejectedInvalidExport);
    assert(!std::filesystem::exists(output / "invalid-export"));
    std::filesystem::remove_all(output);

    std::cout << "BMTTests passed\n";
    return 0;
}
