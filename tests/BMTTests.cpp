#include <Bemani/BFContainer.h>
#include <Bemani/JBT.h>
#include <Bemani/Marker.h>

#include <openssl/evp.h>
#include <zip.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string_view>
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

    void WriteText(const std::filesystem::path& path, std::string_view text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot create test file " + path.string());
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output)
            throw std::runtime_error("cannot write test file " + path.string());
    }

    void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output)
            throw std::runtime_error("cannot write test file " + path.string());
    }

    bool HasJBTDigest(const std::filesystem::path& path)
    {
        const auto bytes = ReadBytes(path);
        if (bytes.size() < 16)
            return false;
        std::array<uint8_t, EVP_MAX_MD_SIZE> digest{};
        unsigned int digestLength = 0;
        if (EVP_Digest(bytes.data(), bytes.size() - 16, digest.data(), &digestLength,
                       EVP_md5(), nullptr) != 1 || digestLength != 16)
            return false;
        return std::equal(digest.begin(), digest.begin() + 16, bytes.end() - 16);
    }

    std::vector<uint8_t> ReadZipEntry(const std::filesystem::path& path,
                                      const char* name)
    {
        int error = 0;
        zip_t* archive = zip_open(path.c_str(), ZIP_RDONLY, &error);
        if (!archive)
            throw std::runtime_error("cannot open test ZIP " + path.string());
        zip_stat_t stat{};
        zip_stat_init(&stat);
        if (zip_stat(archive, name, 0, &stat) != 0)
        {
            zip_close(archive);
            throw std::runtime_error("cannot stat test ZIP member");
        }
        zip_file_t* member = zip_fopen(archive, name, 0);
        if (!member)
        {
            zip_close(archive);
            throw std::runtime_error("cannot open test ZIP member");
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(stat.size));
        const auto read = zip_fread(member, bytes.data(), bytes.size());
        zip_fclose(member);
        zip_close(archive);
        if (read != static_cast<zip_int64_t>(bytes.size()))
            throw std::runtime_error("cannot read test ZIP member");
        return bytes;
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

    bmt::LoadResult playlistConflict;
    bmt::MusicPack officialBase;
    officialBase.originalID = officialBase.id = 100;
    officialBase.sourcePath = "/official/000000100.jbt";
    officialBase.catalogSource = bmt::CatalogSource::Official;
    officialBase.dlcType = bmt::DLCType::Official;
    SetContent(officialBase, {1});
    bmt::MusicPack hotBase = officialBase;
    hotBase.originalID = 100;
    hotBase.id = 600000000;
    hotBase.sourcePath = "/hot/000000100.jbt";
    hotBase.catalogSource = bmt::CatalogSource::JBHot;
    hotBase.dlcType = bmt::DLCType::JBHot;
    hotBase.extID = 600000001;
    SetContent(hotBase, {2});
    bmt::MusicPack officialExtension;
    officialExtension.originalID = officialExtension.id = 200;
    officialExtension.sourcePath = "/official/000000200.jbt";
    officialExtension.catalogSource = bmt::CatalogSource::Official;
    officialExtension.dlcType = bmt::DLCType::Official;
    SetContent(officialExtension, {3});
    bmt::MusicPack hotExtension = officialExtension;
    hotExtension.originalID = 200;
    hotExtension.id = 600000001;
    hotExtension.sourcePath = "/hot/000000200.jbt";
    hotExtension.catalogSource = bmt::CatalogSource::JBHot;
    hotExtension.dlcType = bmt::DLCType::JBHot;
    hotExtension.baseID = 600000000;
    SetContent(hotExtension, {4});
    playlistConflict.packs[100] = {officialBase};
    playlistConflict.packs[200] = {officialExtension};
    playlistConflict.packs[600000000] = {hotBase};
    playlistConflict.packs[600000001] = {hotExtension};
    playlistConflict.playlists.push_back(
        {"0123456789abcdef0123456789abcdef", "JBHot songs", {100, 200, 999}});
    playlistConflict.playlists.push_back({"", "Official songs", {100, 200}, bmt::DLCType::Official});
    assert(playlistConflict.playlists.front().musicIDs ==
           (std::vector<uint32_t>{600000000, 600000001, 999}));
    assert(playlistConflict.playlists.back().musicIDs == (std::vector<uint32_t>{100, 200}));

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
    v3Resource.bytes = std::vector<uint8_t>(v3Info.begin(), v3Info.end());
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
    auto loaded = bmt::LoadPacks({{bmt::DLCType::Custom, output}},
                                 {.mode = bmt::LoadMode::Eager,
                                  .failureMode = bmt::FailureMode::Strict});
    assert(loaded.packs.size() == 5);
    assert(loaded.packs.contains(123456789));
    assert(loaded.packs.at(123456789).front().resources.at("seq_bas").Data() ==
           (std::vector<uint8_t>{'J', 'B', 'S', 'Q', 1, 2, 3, 4, 5}));
    assert(loaded.packs.at(123456789).front().resources.at("infov2").Data() ==
           std::vector<uint8_t>(info.begin(), info.end()));
    assert(HasJBTDigest(output / "123456789.jbt"));
    assert(loaded.packs.contains(123456790));
    assert(loaded.packs.at(123456790).front().infoRevision == bmt::InfoRevision::InfoV3);
    assert(loaded.packs.at(123456790).front().resources.at("infov3").Data() ==
           std::vector<uint8_t>(v3Info.begin(), v3Info.end()));
    const auto encryptedV3 = bmt::DecryptBFContainer(
        ReadZipEntry(output / "123456790.jbt", "infov3"),
        "Konami Bemani Mobile iOS");
    assert(encryptedV3.size() == v3Info.size() + 4);
    assert(std::equal(v3Info.begin(), v3Info.end(), encryptedV3.begin() + 4));
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
        {{bmt::DLCType::Custom, plaintextOutput}},
        {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    assert(plaintextLoaded.packs.size() == 5);
    assert(plaintextLoaded.packs.at(123456789).front().format == bmt::PackFormat::Plain);
    assert(HasJBTDigest(plaintextOutput / "123456789.jbt"));
    assert(plaintextLoaded.packs.at(123456789).front().resources.at("seq_bas").Data() ==
           loaded.packs.at(123456789).front().resources.at("seq_bas").Data());
    assert(plaintextLoaded.packs.at(123456790).front().resources.at("infov3").Data() ==
           std::vector<uint8_t>(v3Info.begin(), v3Info.end()));
    assert(ReadZipEntry(plaintextOutput / "123456790.jbt", "infov3") ==
           std::vector<uint8_t>(v3Info.begin(), v3Info.end()));

    const auto transformRoot = output / "jbt-transforms";
    const auto decryptedDirectory = transformRoot / "decrypted";
    const auto encryptedDirectory = transformRoot / "encrypted";
    bmt::DecryptJBT(output / "123456789.jbt",
                    decryptedDirectory / "123456789.jbt");
    auto singlePlain = bmt::LoadPacks(
        {{bmt::DLCType::Custom, decryptedDirectory}},
        {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    assert(singlePlain.packs.at(123456789).front().format == bmt::PackFormat::Plain);
    bmt::EncryptJBT(decryptedDirectory / "123456789.jbt",
                    encryptedDirectory / "123456789.jbt");
    auto singleEncrypted = bmt::LoadPacks(
        {{bmt::DLCType::Custom, encryptedDirectory}},
        {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    assert(singleEncrypted.packs.at(123456789).front().format == bmt::PackFormat::OfficialBF);
    assert(singleEncrypted.packs.at(123456789).front().resources.at("seq_bas").Data() ==
           singlePlain.packs.at(123456789).front().resources.at("seq_bas").Data());

    const auto unpackedDirectory = transformRoot / "unpacked" / "123456789";
    bmt::UnpackJBT(output / "123456789.jbt", unpackedDirectory);
    assert(ReadBytes(unpackedDirectory / "seq_bas") ==
           loaded.packs.at(123456789).front().resources.at("seq_bas").Data());
    const auto repackedDirectory = transformRoot / "repacked";
    bmt::PackJBT(unpackedDirectory, repackedDirectory / "123456789.jbt");
    auto repacked = bmt::LoadPacks(
        {{bmt::DLCType::Custom, repackedDirectory}},
        {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    assert(repacked.packs.at(123456789).front().resources.at("seq_bas").Data() ==
           loaded.packs.at(123456789).front().resources.at("seq_bas").Data());

    const auto batchInput = transformRoot / "batch-input";
    std::filesystem::create_directories(batchInput / "nested");
    std::filesystem::copy_file(output / "123456789.jbt", batchInput / "123456789.jbt");
    std::filesystem::copy_file(output / "123456790.jbt",
                               batchInput / "nested" / "123456790.jbt");
    const auto batchExpanded = transformRoot / "batch-expanded";
    bmt::UnpackJBTDirectory(batchInput, batchExpanded);
    assert(std::filesystem::is_regular_file(batchExpanded / "123456789" / "infov2"));
    assert(std::filesystem::is_regular_file(batchExpanded / "nested" / "123456790" / "infov3"));
    assert(ReadBytes(batchExpanded / "nested" / "123456790" / "infov3") ==
           std::vector<uint8_t>(v3Info.begin(), v3Info.end()));
    const auto batchRepacked = transformRoot / "batch-repacked";
    bmt::PackJBTDirectory(batchExpanded, batchRepacked);
    assert(std::filesystem::is_regular_file(batchRepacked / "123456789.jbt"));
    assert(std::filesystem::is_regular_file(batchRepacked / "nested" / "123456790.jbt"));
    bmt::DecryptJBT(batchRepacked / "nested" / "123456790.jbt",
                    transformRoot / "batch-v3-plain.jbt");
    assert(ReadZipEntry(transformRoot / "batch-v3-plain.jbt", "infov3") ==
           std::vector<uint8_t>(v3Info.begin(), v3Info.end()));

    const auto customDirectory = output / "custom";
    std::filesystem::create_directory(customDirectory);
    std::filesystem::copy_file(output / "123456792.jbt", customDirectory / "123456792.jbt");
    std::filesystem::copy_file(output / "123456793.jbt", customDirectory / "123456793.jbt");
    auto customLoaded = bmt::LoadPacks(
        {{bmt::DLCType::Custom, customDirectory}},
        {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    assert(customLoaded.packs.size() == 2);
    assert(customLoaded.packs.at(123456792).front().format == bmt::PackFormat::OfficialBF);

    const auto conflictingDirectory = output / "mapping-source";
    bmt::LoadResult conflictingExport;
    bmt::MusicPack conflictingPack = loaded.packs.at(123456789).front();
    conflictingPack.resources.at("seq_bas").bytes =
        std::vector<uint8_t>{'J', 'B', 'S', 'Q', 9, 9, 9, 9};
    conflictingPack.resources.at("seq_bas").lazyLoader = {};
    conflictingExport.packs[123456789].push_back(std::move(conflictingPack));
    conflictingExport.playlists.push_back(
        {"11111111111111111111111111111111", "Mapped songs", {123456789}});
    bmt::ExportPacks(conflictingExport, conflictingDirectory, {.encryptJBT = false});
    bool rejectedMissingMapping = false;
    try
    {
        (void)bmt::LoadPacks({
            {bmt::DLCType::Official, output},
            {bmt::DLCType::Custom, conflictingDirectory},
        }, {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    }
    catch (const std::runtime_error& error)
    {
        rejectedMissingMapping = std::string_view(error.what()).find("mapping.json") !=
                                 std::string_view::npos;
    }
    assert(rejectedMissingMapping);

    WriteText(conflictingDirectory / "mapping.json", "{\n  \"123456789\": 223456789\n}\n");
    auto mappedLoaded = bmt::LoadPacks({
        {bmt::DLCType::Official, output},
        {bmt::DLCType::Custom, conflictingDirectory},
    }, {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    assert(mappedLoaded.packs.size() == 6);
    assert(mappedLoaded.packs.contains(123456789));
    assert(mappedLoaded.packs.contains(223456789));
    assert(mappedLoaded.packs.at(223456789).front().originalID == 123456789);
    assert(mappedLoaded.packs.at(223456789).front().id == 223456789);
    assert(mappedLoaded.remaps.size() == 1);
    assert(mappedLoaded.remaps.front().oldID == 123456789);
    assert(mappedLoaded.remaps.front().newID == 223456789);
    assert(mappedLoaded.playlists.size() == 1);
    assert(mappedLoaded.playlists.front().musicIDs == (std::vector<uint32_t>{223456789}));

    const auto duplicateDirectory = output / "duplicate-source";
    std::filesystem::create_directory(duplicateDirectory);
    std::filesystem::copy_file(output / "123456789.jbt",
                               duplicateDirectory / "123456789.jbt");
    std::filesystem::copy_file(output / "123456789.jbt",
                               duplicateDirectory / "323456789.jbt");
    auto deduplicated = bmt::LoadPacks({
        {bmt::DLCType::Official, output},
        {bmt::DLCType::Custom, duplicateDirectory},
    }, {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    assert(deduplicated.packs.size() == 5);
    assert(deduplicated.droppedDuplicates == 2);
    assert(deduplicated.packs.at(123456789).front().dlcType == bmt::DLCType::Official);

    WriteText(duplicateDirectory / "mapping.json", "{\n  \"323456789\": 323456780\n}\n");
    auto fileIDMapped = bmt::LoadPacks(
        {{bmt::DLCType::Custom, duplicateDirectory}},
        {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    assert(fileIDMapped.packs.size() == 2);
    assert(fileIDMapped.packs.contains(123456789));
    assert(fileIDMapped.packs.contains(323456780));
    assert(fileIDMapped.packs.at(123456789).front().sourceFileID == 123456789);
    assert(fileIDMapped.packs.at(323456780).front().sourceFileID == 323456789);
    assert(fileIDMapped.packs.at(323456780).front().originalID == 123456789);

    bmt::ExportPacks(playlistConflict, output / "playlist-export");
    assert(std::filesystem::is_regular_file(output / "playlist-export" / "playlists.plist"));
    std::ifstream playlistInput(output / "playlist-export" / "playlists.plist");
    const std::string playlistXML((std::istreambuf_iterator<char>(playlistInput)),
                                  std::istreambuf_iterator<char>());
    const auto listPosition = playlistXML.find("<key>LIST</key>");
    const auto namePosition = playlistXML.find("<key>NAME</key>");
    const auto playlistIDPosition = playlistXML.find("<key>PLID</key>");
    assert(listPosition < namePosition && namePosition < playlistIDPosition);
    assert(playlistXML.find("<integer>600000000</integer>") != std::string::npos);
    assert(playlistXML.find("<string>JBHot songs</string>") != std::string::npos);
    assert(playlistXML.find("<string>0123456789abcdef0123456789abcdef</string>") != std::string::npos);
    const auto reloadedPlaylists = bmt::LoadPlaylists(output / "playlist-export" / "playlists.plist");
    assert(reloadedPlaylists.size() == 2);
    assert(reloadedPlaylists.front().name == "JBHot songs");
    assert(reloadedPlaylists.front().id == "0123456789abcdef0123456789abcdef");
    assert(reloadedPlaylists.back().id.size() == 32);
    assert(std::all_of(reloadedPlaylists.back().id.begin(), reloadedPlaylists.back().id.end(),
                       [](char character)
                       {
                           return (character >= '0' && character <= '9') ||
                                  (character >= 'a' && character <= 'f');
                       }));
    assert(reloadedPlaylists.back().musicIDs == (std::vector<uint32_t>{100, 200}));

    const auto separateOutput = output / "separate-export";
    bmt::ExportPacks(playlistConflict, separateOutput, {.separateByDLC = true});
    assert(std::filesystem::is_regular_file(separateOutput / "official" / "000000100.jbt"));
    assert(std::filesystem::is_regular_file(separateOutput / "official" / "000000200.jbt"));
    assert(std::filesystem::is_regular_file(separateOutput / "jbhot" / "600000000.jbt"));
    assert(std::filesystem::is_regular_file(separateOutput / "jbhot" / "600000001.jbt"));
    assert(!std::filesystem::exists(separateOutput / "000000100.jbt"));
    assert(std::filesystem::is_regular_file(separateOutput / "mulist.plist"));
    assert(std::filesystem::is_regular_file(separateOutput / "playlists.plist"));

    const auto separateCustomOutput = output / "separate-custom-export";
    bmt::ExportPacks(exportResult, separateCustomOutput, {.separateByDLC = true});
    assert(std::filesystem::is_regular_file(
        separateCustomOutput / "custom-1" / "123456789.jbt"));
    assert(!std::filesystem::exists(separateCustomOutput / "123456789.jbt"));
    assert(std::filesystem::is_regular_file(separateCustomOutput / "mulist.plist"));

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

    const auto mulistPlainPath = output / "list-crypto" / "mulist.plist";
    const std::string minimalList =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<plist version=\"1.0\"><array/></plist>";
    WriteText(mulistPlainPath, minimalList);
    const auto encryptedList = bmt::EncryptOfficialMusicList(mulistPlainPath, "SHARED_KEY");
    const auto encryptedListPath = output / "list-crypto" / "mulist";
    WriteBytes(encryptedListPath, encryptedList);
    assert(bmt::DecryptOfficialMusicList(encryptedListPath, "SHARED_KEY") ==
           ReadBytes(mulistPlainPath));

    const std::vector<uint8_t> tinyPNG = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 1, 2, 3, 4
    };
    const auto markerRoot = output / "markers";
    const auto markerExpanded = markerRoot / "expanded" / "mk0048";
    WriteBytes(markerExpanded / "h100", tinyPNG);
    WriteBytes(markerExpanded / "ma00", tinyPNG);
    const auto markerOfficial = markerRoot / "official";
    bmt::PackMarker(markerExpanded, markerOfficial / "mk0048.zip");
    const auto markerUnpacked = markerRoot / "unpacked";
    bmt::UnpackMarker(markerOfficial / "mk0048.zip", markerUnpacked);
    assert(ReadBytes(markerUnpacked / "h100") == tinyPNG);
    assert(ReadBytes(markerUnpacked / "ma00") == tinyPNG);
    const auto markerPlainZip = markerRoot / "mk0048-plain.zip";
    bmt::DecryptMarker(markerOfficial / "mk0048.zip", markerPlainZip);
    const auto markerReencrypted = markerRoot / "mk0048-reencrypted.zip";
    bmt::EncryptMarker(markerPlainZip, markerReencrypted);
    bmt::UnpackMarker(markerReencrypted, markerRoot / "reencrypted-unpacked");
    assert(ReadBytes(markerRoot / "reencrypted-unpacked" / "h100") == tinyPNG);

    auto missingBanner = bmt::LoadMarkers({{bmt::DLCType::Official, markerOfficial}});
    assert(missingBanner.packs.size() == 1);
    assert(missingBanner.diagnostics.size() == 1);
    WriteBytes(markerOfficial / "banner" / "tm0048_banner.png", tinyPNG);
    auto markerLoaded = bmt::LoadMarkers(
        {{bmt::DLCType::Official, markerOfficial}},
        {.failureMode = bmt::FailureMode::Strict});
    assert(markerLoaded.packs.size() == 1);
    assert(markerLoaded.diagnostics.empty());

    const auto markerExport = markerRoot / "export";
    const auto markerListRaw = markerExport / "PrefMarkerInfoList";
    bmt::ExportMarkers(markerLoaded, markerExport,
        {.markerListOutput = markerListRaw,
         .markerListEncoding = bmt::MarkerListEncoding::Raw});
    assert(std::filesystem::is_regular_file(markerExport / "mk0048.zip"));
    assert(std::filesystem::is_regular_file(markerExport / "banner" / "tm0048_banner.png"));
    const auto markerEntries = bmt::DecryptMarkerList(markerListRaw);
    assert(markerEntries.size() == 1);
    assert(markerEntries.front().markerID == "mk0048");
    assert(markerEntries.front().bannerName == "tm0048_banner");
    assert(markerEntries.front().version == "1.0.0");
    const auto markerXML = bmt::BuildMarkerListXML(markerEntries);
    const auto markerXMLPath = markerRoot / "marker-list.plist";
    WriteBytes(markerXMLPath, markerXML);
    assert(bmt::LoadMarkerListXML(markerXMLPath).front().markerID == "mk0048");
    const auto markerListBase64 = bmt::EncryptMarkerList(
        markerEntries, bmt::MarkerListEncoding::Base64);
    const auto markerListBase64Path = markerRoot / "PrefMarkerInfoList.base64";
    WriteBytes(markerListBase64Path, markerListBase64);
    assert(bmt::DecryptMarkerList(markerListBase64Path).front().markerID == "mk0048");

    const auto duplicateMarkers = markerRoot / "duplicates";
    std::filesystem::create_directories(duplicateMarkers / "banner");
    std::filesystem::copy_file(markerOfficial / "mk0048.zip", duplicateMarkers / "mk0048.zip");
    std::filesystem::copy_file(markerOfficial / "banner" / "tm0048_banner.png",
                               duplicateMarkers / "banner" / "tm0048_banner.png");
    auto deduplicatedMarkers = bmt::LoadMarkers({
        {bmt::DLCType::Official, markerOfficial},
        {bmt::DLCType::Custom, duplicateMarkers},
    });
    assert(deduplicatedMarkers.packs.size() == 1);
    assert(deduplicatedMarkers.droppedDuplicates == 1);
    WriteText(duplicateMarkers / "mapping.json", "{\"48\":49}\n");
    auto mappedDuplicateMarkers = bmt::LoadMarkers({
        {bmt::DLCType::Official, markerOfficial},
        {bmt::DLCType::Custom, duplicateMarkers},
    });
    assert(mappedDuplicateMarkers.packs.size() == 1);
    assert(mappedDuplicateMarkers.droppedDuplicates == 1);
    assert(mappedDuplicateMarkers.remaps.empty());

    const auto conflictingMarkers = markerRoot / "conflicting";
    const auto conflictingExpanded = markerRoot / "conflicting-expanded";
    auto differentPNG = tinyPNG;
    differentPNG.back() = 9;
    WriteBytes(conflictingExpanded / "h100", differentPNG);
    bmt::PackMarker(conflictingExpanded, conflictingMarkers / "mk0048.zip");
    WriteBytes(conflictingMarkers / "banner" / "tm0048_banner.png", differentPNG);
    WriteText(conflictingMarkers / "mapping.json", "{\"48\":49}\n");
    auto remappedMarkers = bmt::LoadMarkers({
        {bmt::DLCType::Official, markerOfficial},
        {bmt::DLCType::Custom, conflictingMarkers},
    }, {.failureMode = bmt::FailureMode::Strict});
    assert(remappedMarkers.packs.size() == 2);
    assert(remappedMarkers.packs.contains(48));
    assert(remappedMarkers.packs.contains(49));
    assert(remappedMarkers.remaps.size() == 1);
    assert(remappedMarkers.remaps.front().oldID == 48);
    assert(remappedMarkers.remaps.front().newID == 49);

    std::filesystem::remove_all(output);

    std::cout << "BMTTests passed\n";
    return 0;
}
