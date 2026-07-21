#include <Bemani/BFContainer.h>
#include <Bemani/JBT.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace
{
    std::vector<uint8_t> ReadBytes(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open test file " + path.string());
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void SetContent(bmt::MusicPack& pack, std::initializer_list<uint8_t> bytes)
    {
        bmt::PackResource resource;
        resource.name = "payload";
        resource.bytes = std::vector<uint8_t>(bytes);
        pack.resources[resource.name] = std::move(resource);
    }
}

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

    bmt::LoadResult conflict;
    bmt::MusicPack first;
    first.originalID = first.id = 100;
    first.sourcePath = "/one/000000100.jbt";
    first.dlcType = bmt::DLCType::Official;
    SetContent(first, {1});
    bmt::MusicPack second = first;
    second.sourcePath = "/two/000000100.jbt";
    second.dlcType = bmt::DLCType::JBHot;
    SetContent(second, {2});
    conflict.packs[100] = {first, second};
    const auto remaps = bmt::ResolveConflicts(conflict, {600000000, 600000010});
    assert(remaps.size() == 1);
    assert(conflict.packs.contains(100));
    assert(conflict.packs.contains(600000000));

    bmt::LoadResult hotCustomConflict;
    bmt::MusicPack stableHot = second;
    stableHot.id = stableHot.originalID = 300;
    stableHot.sourcePath = "/hot/000000300.jbt";
    bmt::MusicPack rangedCustom = stableHot;
    rangedCustom.dlcType = bmt::DLCType::Custom;
    rangedCustom.dlcOrder = 1;
    rangedCustom.customFirstID = 1100;
    rangedCustom.customLastID = 1200;
    rangedCustom.sourcePath = "/custom/000000300.jbt";
    SetContent(rangedCustom, {3});
    hotCustomConflict.packs[300] = {rangedCustom, stableHot};
    const auto hotCustomRemaps = bmt::ResolveConflicts(hotCustomConflict);
    assert(hotCustomRemaps.size() == 1);
    assert(hotCustomConflict.packs.contains(300));
    assert(hotCustomConflict.packs.at(300).front().dlcType == bmt::DLCType::JBHot);
    assert(hotCustomConflict.packs.contains(1100));

    bmt::LoadResult paired;
    bmt::MusicPack baseOne;
    baseOne.originalID = baseOne.id = 100;
    baseOne.extID = 200;
    baseOne.sourcePath = "/one/000000100.jbt";
    baseOne.dlcType = bmt::DLCType::Official;
    SetContent(baseOne, {1});
    bmt::MusicPack baseTwo = baseOne;
    baseTwo.sourcePath = "/two/000000100.jbt";
    baseTwo.dlcType = bmt::DLCType::Custom;
    baseTwo.dlcOrder = 1;
    baseTwo.customFirstID = 600000000;
    baseTwo.customLastID = 600000010;
    SetContent(baseTwo, {2});
    bmt::MusicPack extOne;
    extOne.originalID = extOne.id = 200;
    extOne.baseID = 100;
    extOne.sourcePath = "/one/000000200.jbt";
    extOne.dlcType = bmt::DLCType::Official;
    SetContent(extOne, {3});
    bmt::MusicPack extTwo = extOne;
    extTwo.sourcePath = "/two/000000200.jbt";
    extTwo.dlcType = bmt::DLCType::Custom;
    extTwo.dlcOrder = 1;
    extTwo.customFirstID = 600000000;
    extTwo.customLastID = 600000010;
    SetContent(extTwo, {4});
    paired.packs[100] = {baseOne, baseTwo};
    paired.packs[200] = {extOne, extTwo};
    const auto pairRemaps = bmt::ResolveConflicts(paired, {600000000, 600000010});
    assert(pairRemaps.size() == 2);
    assert(paired.packs.at(600000000).front().extID == 600000001);
    assert(paired.packs.at(600000001).front().baseID == 600000000);

    bmt::LoadResult identical;
    bmt::MusicPack identicalOfficial = first;
    bmt::MusicPack identicalHot = first;
    identicalHot.dlcType = bmt::DLCType::JBHot;
    identicalHot.sourcePath = "/hot/000000100.jbt";
    identical.packs[100] = {identicalHot, identicalOfficial};
    assert(bmt::ResolveConflicts(identical).empty());
    assert(identical.droppedDuplicates == 1);
    assert(identical.packs.at(100).front().dlcType == bmt::DLCType::Official);

    bmt::LoadResult playlistConflict;
    bmt::MusicPack officialBase;
    officialBase.originalID = officialBase.id = 100;
    officialBase.sourcePath = "/official/000000100.jbt";
    officialBase.catalogSource = bmt::CatalogSource::Official;
    officialBase.dlcType = bmt::DLCType::Official;
    SetContent(officialBase, {1});
    bmt::MusicPack hotBase = officialBase;
    hotBase.sourcePath = "/hot/000000100.jbt";
    hotBase.catalogSource = bmt::CatalogSource::JBHot;
    hotBase.dlcType = bmt::DLCType::JBHot;
    hotBase.extID = 200;
    SetContent(hotBase, {2});
    bmt::MusicPack officialExtension;
    officialExtension.originalID = officialExtension.id = 200;
    officialExtension.sourcePath = "/official/000000200.jbt";
    officialExtension.catalogSource = bmt::CatalogSource::Official;
    officialExtension.dlcType = bmt::DLCType::Official;
    SetContent(officialExtension, {3});
    bmt::MusicPack hotExtension = officialExtension;
    hotExtension.sourcePath = "/hot/000000200.jbt";
    hotExtension.catalogSource = bmt::CatalogSource::JBHot;
    hotExtension.dlcType = bmt::DLCType::JBHot;
    hotExtension.baseID = 100;
    SetContent(hotExtension, {4});
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
    bmt::LoadResult exportResult;
    auto& exportPacks = exportResult.packs;
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

    bmt::ExportPacks(exportResult, output);
    auto loaded = bmt::LoadPacks({{bmt::DLCType::Custom, output, 700000000, 700000100}},
                                 {.mode = bmt::LoadMode::Eager,
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
    assert(!std::filesystem::exists(output / "mulist"));

    const auto plaintextOutput = output / "plaintext-export";
    bmt::ExportPacks(exportResult, plaintextOutput,
                     {.encryptJBT = false, .mulistKey = "SHARED_KEY"});
    assert(std::filesystem::is_regular_file(plaintextOutput / "mulist.plist"));
    assert(std::filesystem::is_regular_file(plaintextOutput / "mulist"));
    const auto decryptedCatalog = bmt::DecryptBFContainer(ReadBytes(plaintextOutput / "mulist"),
                                                           "SHARED_KEY");
    assert(decryptedCatalog.size() >= 4);
    assert(std::vector<uint8_t>(decryptedCatalog.begin() + 4, decryptedCatalog.end()) ==
           ReadBytes(plaintextOutput / "mulist.plist"));
    auto plaintextLoaded = bmt::LoadPacks(
        {{bmt::DLCType::Custom, plaintextOutput, 720000000, 720000100}},
        {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    assert(plaintextLoaded.packs.size() == 5);
    assert(plaintextLoaded.packs.at(123456789).front().format == bmt::PackFormat::Plain);
    assert(plaintextLoaded.packs.at(123456789).front().resources.at("seq_bas").Data() ==
           loaded.packs.at(123456789).front().resources.at("seq_bas").Data());

    const auto customDirectory = output / "custom";
    std::filesystem::create_directory(customDirectory);
    std::filesystem::copy_file(output / "123456792.jbt", customDirectory / "123456792.jbt");
    std::filesystem::copy_file(output / "123456793.jbt", customDirectory / "123456793.jbt");
    auto customLoaded = bmt::LoadPacks(
        {{bmt::DLCType::Custom, customDirectory, 710000000, 710000100}},
        {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    assert(customLoaded.packs.size() == 2);
    assert(customLoaded.packs.at(123456792).front().format == bmt::PackFormat::OfficialBF);

    bmt::ExportPacks(playlistConflict, output / "playlist-export");
    assert(std::filesystem::is_regular_file(output / "playlist-export" / "playlists.plist"));
    std::ifstream playlistInput(output / "playlist-export" / "playlists.plist");
    const std::string playlistXML((std::istreambuf_iterator<char>(playlistInput)),
                                  std::istreambuf_iterator<char>());
    const auto listPosition = playlistXML.find("<key>LIST</key>");
    const auto namePosition = playlistXML.find("<key>NAME</key>");
    const auto idPositionInPlaylist = playlistXML.find("<key>PLID</key>");
    assert(listPosition < namePosition && namePosition < idPositionInPlaylist);
    assert(playlistXML.find("<integer>600000000</integer>") != std::string::npos);
    assert(playlistXML.find("<string>JBHot songs</string>") != std::string::npos);
    assert(playlistXML.find("<string>playlist-id</string>") != std::string::npos);

    bmt::LoadResult danglingExtension;
    bmt::MusicPack danglingBase;
    danglingBase.originalID = danglingBase.id = 123456795;
    danglingBase.extID = 123456796;
    danglingBase.name = "Missing Extension Song";
    danglingBase.artist = "Missing Extension Artist";
    danglingBase.sourcePath = "/hot/123456795.jbt";
    danglingExtension.packs[danglingBase.id].push_back(std::move(danglingBase));
    const auto danglingOutput = output / "dangling-export";
    bmt::ExportPacks(danglingExtension, danglingOutput);
    assert(danglingExtension.warnings.size() == 1);
    assert(danglingExtension.warnings.front().message.find("123456796") != std::string::npos);
    assert(std::filesystem::is_regular_file(danglingOutput / "123456795.jbt"));
    const auto danglingCatalog = bmt::LoadOfficialCatalog(danglingOutput / "mulist.plist");
    assert(danglingCatalog.size() == 1);
    assert(danglingCatalog.front().id == 123456795);
    assert(danglingCatalog.front().extID == 0);
    assert(danglingCatalog.front().extURL.empty());

    const auto secondCustomDirectory = output / "custom-two";
    std::filesystem::create_directory(secondCustomDirectory);
    bool rejectedOverlappingRanges = false;
    try
    {
        (void)bmt::LoadPacks({
            {bmt::DLCType::Custom, customDirectory, 1000, 2000},
            {bmt::DLCType::Custom, secondCustomDirectory, 2000, 3000},
        });
    }
    catch (const std::invalid_argument&)
    {
        rejectedOverlappingRanges = true;
    }
    assert(rejectedOverlappingRanges);

    bmt::LoadResult invalidExport;
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
    invalidExport.packs[invalidPack.id].push_back(std::move(invalidPack));
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
