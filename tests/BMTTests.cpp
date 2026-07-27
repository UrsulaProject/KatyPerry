#ifdef NDEBUG
#undef NDEBUG
#endif

#include <Bemani/BFContainer.h>
#include <Bemani/JBT.h>
#include <Bemani/Marker.h>
#include <Bemani/RB.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <zip.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
    std::string UTF8Path(const std::filesystem::path& path)
    {
        const auto utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

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

    bool HasTrailingMD5(const std::filesystem::path& path)
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
        const std::string archivePath = UTF8Path(path);
        zip_t* archive = zip_open(archivePath.c_str(), ZIP_RDONLY, &error);
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

    std::string Base64Encode(std::span<const uint8_t> bytes)
    {
        if (bytes.empty())
            return {};
        std::string output(((bytes.size() + 2) / 3) * 4, '\0');
        const int size = EVP_EncodeBlock(
            reinterpret_cast<unsigned char*>(output.data()), bytes.data(),
            static_cast<int>(bytes.size()));
        if (size < 0)
            throw std::runtime_error("cannot encode test Base64");
        output.resize(static_cast<size_t>(size));
        return output;
    }

    std::vector<uint8_t> EncryptAES(std::span<const uint8_t> plaintext,
                                    std::span<const uint8_t> key,
                                    std::span<const uint8_t> iv,
                                    const EVP_CIPHER* cipher)
    {
        EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
        if (!context ||
            EVP_EncryptInit_ex(context, cipher, nullptr, key.data(), iv.data()) != 1)
        {
            EVP_CIPHER_CTX_free(context);
            throw std::runtime_error("cannot initialize test AES");
        }
        std::vector<uint8_t> output(
            plaintext.size() + static_cast<size_t>(EVP_CIPHER_block_size(cipher)));
        int first = 0;
        int final = 0;
        const bool success =
            EVP_EncryptUpdate(context, output.data(), &first, plaintext.data(),
                              static_cast<int>(plaintext.size())) == 1 &&
            EVP_EncryptFinal_ex(context, output.data() + first, &final) == 1;
        EVP_CIPHER_CTX_free(context);
        if (!success)
            throw std::runtime_error("cannot encrypt test AES");
        output.resize(static_cast<size_t>(first + final));
        return output;
    }

    std::vector<uint8_t> EncryptRNCryptor(std::span<const uint8_t> plaintext,
                                          std::string_view password,
                                          uint8_t seed)
    {
        std::array<uint8_t, 8> encryptionSalt{};
        std::array<uint8_t, 8> hmacSalt{};
        std::array<uint8_t, 16> iv{};
        for (size_t index = 0; index < encryptionSalt.size(); ++index)
        {
            encryptionSalt[index] = static_cast<uint8_t>(seed + index);
            hmacSalt[index] = static_cast<uint8_t>(seed + 16 + index);
        }
        for (size_t index = 0; index < iv.size(); ++index)
            iv[index] = static_cast<uint8_t>(seed + 32 + index);
        std::array<uint8_t, 32> encryptionKey{};
        std::array<uint8_t, 32> hmacKey{};
        if (PKCS5_PBKDF2_HMAC_SHA1(
                password.data(), static_cast<int>(password.size()),
                encryptionSalt.data(), static_cast<int>(encryptionSalt.size()),
                10000, static_cast<int>(encryptionKey.size()),
                encryptionKey.data()) != 1 ||
            PKCS5_PBKDF2_HMAC_SHA1(
                password.data(), static_cast<int>(password.size()),
                hmacSalt.data(), static_cast<int>(hmacSalt.size()), 10000,
                static_cast<int>(hmacKey.size()), hmacKey.data()) != 1)
            throw std::runtime_error("cannot derive test RNCryptor keys");
        const auto ciphertext =
            EncryptAES(plaintext, encryptionKey, iv, EVP_aes_256_cbc());
        std::vector<uint8_t> blob = {3, 1};
        blob.insert(blob.end(), encryptionSalt.begin(), encryptionSalt.end());
        blob.insert(blob.end(), hmacSalt.begin(), hmacSalt.end());
        blob.insert(blob.end(), iv.begin(), iv.end());
        blob.insert(blob.end(), ciphertext.begin(), ciphertext.end());
        std::array<uint8_t, EVP_MAX_MD_SIZE> digest{};
        unsigned int digestLength = 0;
        if (!HMAC(EVP_sha256(), hmacKey.data(), static_cast<int>(hmacKey.size()),
                  blob.data(), blob.size(), digest.data(), &digestLength) ||
            digestLength != 32)
            throw std::runtime_error("cannot calculate test RNCryptor HMAC");
        blob.insert(blob.end(), digest.begin(), digest.begin() + digestLength);
        return blob;
    }

    void WriteTestZip(
        const std::filesystem::path& path,
        const std::map<std::string, std::vector<uint8_t>>& members)
    {
        std::filesystem::create_directories(path.parent_path());
        int error = 0;
        const auto archivePath = UTF8Path(path);
        zip_t* archive = zip_open(archivePath.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
        if (!archive)
            throw std::runtime_error("cannot create test ZIP");
        for (const auto& [name, data] : members)
        {
            zip_source_t* source =
                zip_source_buffer(archive, data.data(), data.size(), 0);
            if (!source ||
                zip_file_add(archive, name.c_str(), source,
                             ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8) < 0)
            {
                if (source)
                    zip_source_free(source);
                zip_discard(archive);
                throw std::runtime_error("cannot add test ZIP member");
            }
        }
        if (zip_close(archive) != 0)
        {
            zip_discard(archive);
            throw std::runtime_error("cannot close test ZIP");
        }
        const auto bytes = ReadBytes(path);
        std::array<uint8_t, EVP_MAX_MD_SIZE> digest{};
        unsigned int digestLength = 0;
        if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &digestLength,
                       EVP_md5(), nullptr) != 1 ||
            digestLength != 16)
            throw std::runtime_error("cannot calculate test ZIP MD5");
        std::ofstream output(path, std::ios::binary | std::ios::app);
        output.write(reinterpret_cast<const char*>(digest.data()), digestLength);
        if (!output)
            throw std::runtime_error("cannot append test ZIP MD5");
    }

    void SetContent(bmt::MusicPack& pack, std::initializer_list<uint8_t> bytes)
    {
        bmt::PackResource resource;
        resource.name = "payload";
        resource.bytes = std::vector<uint8_t>(bytes);
        pack.resources[resource.name] = std::move(resource);
    }
}

int RunTests()
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
        {"0123456789abcdef0123456789abcdef", "JBHot songs",
         {600000000, 600000001, 999}});
    playlistConflict.playlists.push_back(
        {"", "Official songs", {100, 200}, bmt::DLCType::Official});

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path outputName(u8"bmt-tests-测试-");
    outputName += std::to_string(unique);
    const std::filesystem::path output = std::filesystem::temp_directory_path() / outputName;

    bmt::LoadResult lowCustomResult;
    bmt::MusicPack lowCustomPack;
    lowCustomPack.originalID = lowCustomPack.id = 12345678;
    SetContent(lowCustomPack, {1});
    lowCustomResult.packs[lowCustomPack.id].push_back(std::move(lowCustomPack));
    bool rejectedLowCustomID = false;
    try
    {
        bmt::ExportPacks(lowCustomResult, output / "low-custom");
    }
    catch (const std::runtime_error& error)
    {
        rejectedLowCustomID =
            std::string_view(error.what()).find("at least nine digits") != std::string_view::npos;
    }
    assert(rejectedLowCustomID);

    bmt::LoadResult lowOfficialResult;
    bmt::MusicPack lowOfficialPack;
    lowOfficialPack.originalID = lowOfficialPack.id = 12345678;
    lowOfficialPack.dlcType = bmt::DLCType::Official;
    SetContent(lowOfficialPack, {2});
    lowOfficialResult.packs[lowOfficialPack.id].push_back(std::move(lowOfficialPack));
    bmt::ExportPacks(lowOfficialResult, output / "low-official");
    assert(std::filesystem::is_regular_file(output / "low-official" / "012345678.jbt"));

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
    assert(HasTrailingMD5(output / "123456789.jbt"));
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
    assert(HasTrailingMD5(plaintextOutput / "123456789.jbt"));
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

    WriteText(conflictingDirectory / "mapping.json", "{\n  \"123456789\": 23456789\n}\n");
    bool rejectedLowMappingTarget = false;
    try
    {
        (void)bmt::LoadPacks(
            {{bmt::DLCType::Custom, conflictingDirectory}},
            {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    }
    catch (const std::runtime_error& error)
    {
        rejectedLowMappingTarget =
            std::string_view(error.what()).find("at least nine digits") != std::string_view::npos;
    }
    assert(rejectedLowMappingTarget);

    WriteText(conflictingDirectory / "mapping.json", "{\n  \"012345678\": 223456789\n}\n");
    bool rejectedPaddedMappingSource = false;
    try
    {
        (void)bmt::LoadPacks(
            {{bmt::DLCType::Custom, conflictingDirectory}},
            {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    }
    catch (const std::runtime_error& error)
    {
        rejectedPaddedMappingSource =
            std::string_view(error.what()).find("leading zeros") != std::string_view::npos;
    }
    assert(rejectedPaddedMappingSource);
    WriteText(conflictingDirectory / "mapping.json", "{\n  \"123456789\": 223456789\n}\n");

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

    const auto metadataDuplicateDirectory = output / "metadata-duplicate-source";
    bmt::LoadResult metadataDuplicateExport;
    bmt::MusicPack metadataDuplicate = loaded.packs.at(123456789).front();
    auto& metadataInfo = metadataDuplicate.resources.at("infov2");
    auto changedInfo = metadataInfo.Data();
    const std::string oldName = "Test Song";
    const std::string newName = "Alt Song!";
    const auto infoNamePosition = std::search(changedInfo.begin(), changedInfo.end(),
                                              oldName.begin(), oldName.end());
    assert(infoNamePosition != changedInfo.end());
    std::copy(newName.begin(), newName.end(), infoNamePosition);
    metadataInfo.bytes = std::move(changedInfo);
    metadataInfo.lazyLoader = {};
    metadataDuplicateExport.packs[metadataDuplicate.id].push_back(std::move(metadataDuplicate));
    bmt::ExportPacks(metadataDuplicateExport, metadataDuplicateDirectory,
                     {.encryptJBT = false});
    auto metadataDeduplicated = bmt::LoadPacks({
        {bmt::DLCType::Official, output},
        {bmt::DLCType::Custom, metadataDuplicateDirectory},
    }, {.mode = bmt::LoadMode::Lazy, .failureMode = bmt::FailureMode::Strict});
    assert(metadataDeduplicated.packs.size() == 5);
    assert(metadataDeduplicated.droppedDuplicates == 1);

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

    WriteText(duplicateDirectory / "mapping.json",
              "{\n  \"123456789\": 223456780,\n  \"323456789\": 323456780\n}\n");
    auto sharedInfoIDMapped = bmt::LoadPacks(
        {{bmt::DLCType::Custom, duplicateDirectory}},
        {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    assert(sharedInfoIDMapped.packs.size() == 2);
    assert(sharedInfoIDMapped.packs.contains(223456780));
    assert(sharedInfoIDMapped.packs.contains(323456780));
    assert(sharedInfoIDMapped.packs.at(223456780).front().sourceFileID == 123456789);
    assert(sharedInfoIDMapped.packs.at(323456780).front().sourceFileID == 323456789);
    assert(sharedInfoIDMapped.packs.at(223456780).front().originalID == 123456789);
    assert(sharedInfoIDMapped.packs.at(323456780).front().originalID == 123456789);

    bmt::ExportPacks(playlistConflict, output / "playlist-export");
    assert(std::filesystem::is_regular_file(output / "playlist-export" / "playlists.plist"));
    const auto playlistBytes = ReadBytes(output / "playlist-export" / "playlists.plist");
    const std::string playlistXML(playlistBytes.begin(), playlistBytes.end());
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

    const auto realIDCatalogPath = output / "real-id-catalog.plist";
    WriteText(realIDCatalogPath,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<plist version=\"1.0\"><array><dict>"
        "<key>ID</key><real>780502690</real>"
        "<key>Name</key><string>Real ID Song</string>"
        "</dict></array></plist>");
    const auto realIDCatalog = bmt::LoadOfficialCatalog(realIDCatalogPath);
    assert(realIDCatalog.size() == 1);
    assert(realIDCatalog.front().id == 780502690);

    const std::vector<uint8_t> tinyPNG = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 1, 2, 3, 4
    };
    const auto markerRoot = output / "markers";
    const auto markerExpanded = markerRoot / "expanded" / "mk0048";
    WriteBytes(markerExpanded / "h100", tinyPNG);
    WriteBytes(markerExpanded / "ma00", tinyPNG);
    const auto markerOfficial = markerRoot / "official";
    bmt::PackMarker(markerExpanded, markerOfficial / "mk0048.zip");
    assert(HasTrailingMD5(markerOfficial / "mk0048.zip"));
    const auto markerUnpacked = markerRoot / "unpacked";
    bmt::UnpackMarker(markerOfficial / "mk0048.zip", markerUnpacked);
    assert(ReadBytes(markerUnpacked / "h100") == tinyPNG);
    assert(ReadBytes(markerUnpacked / "ma00") == tinyPNG);
    const auto markerPlainZip = markerRoot / "mk0048-plain.zip";
    bmt::DecryptMarker(markerOfficial / "mk0048.zip", markerPlainZip);
    assert(HasTrailingMD5(markerPlainZip));
    const auto markerReencrypted = markerRoot / "mk0048-reencrypted.zip";
    bmt::EncryptMarker(markerPlainZip, markerReencrypted);
    assert(HasTrailingMD5(markerReencrypted));
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
    const auto markerListXML = markerExport / "marker-list.plist";
    bmt::ExportMarkers(markerLoaded, markerExport,
        {.markerListOutput = markerListXML});
    assert(std::filesystem::is_regular_file(markerExport / "mk0048.zip"));
    assert(HasTrailingMD5(markerExport / "mk0048.zip"));
    assert(std::filesystem::is_regular_file(markerExport / "banner" / "tm0048_banner.png"));
    const auto markerEntries = bmt::LoadMarkerListXML(markerListXML);
    assert(markerEntries.size() == 1);
    assert(markerEntries.front().markerID == "mk0048");
    assert(markerEntries.front().bannerName == "tm0048_banner");
    assert(markerEntries.front().version == "1.0.0");
    assert(bmt::LoadMarkerListXML(markerListXML).front().markerID == "mk0048");
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

    const auto rbRoot = output / "rb";
    const auto rbExpanded = rbRoot / "expanded";
    const std::string rbInfo =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<plist version=\"1.0\"><dict>"
        "<key>ID</key><integer>123456789</integer>"
        "<key>MusicName</key><string>RB Test</string>"
        "<key>MusicNameHira</key><string>てすと</string>"
        "<key>MusicNameRoman</key><string>Test</string>"
        "<key>ArtistName</key><string>Test Artist</string>"
        "<key>Basic</key><integer>3</integer>"
        "<key>Medium</key><integer>6</integer>"
        "<key>Hard</key><integer>9</integer>"
        "<key>BpmMin</key><integer>120</integer>"
        "<key>BpmMax</key><integer>180</integer>"
        "<key>Version</key><integer>2</integer>"
        "<key>Options</key><dict>"
        "<key>note_har2</key><integer>1</integer>"
        "<key>Unknown</key><true/>"
        "</dict>"
        "</dict></plist>";
    WriteText(rbExpanded / "info", rbInfo);
    WriteBytes(rbExpanded / "bgm", {'M', '4', 'A', ' ', 1, 2, 3, 4});
    WriteBytes(rbExpanded / "pre", {'M', '4', 'A', ' ', 5, 6, 7, 8});
    WriteBytes(rbExpanded / "note_bas", {'R', 'B', 'F', 'F', 1, 2, 3, 4});
    WriteBytes(rbExpanded / "note_med", {'R', 'B', 'F', 'F', 2, 3, 4, 5});
    WriteBytes(rbExpanded / "note_har", {'R', 'B', 'F', 'F', 3, 4, 5, 6});
    WriteBytes(rbExpanded / "note_har2", {'R', 'B', 'F', 'F', 5, 6, 7, 8});
    WriteBytes(rbExpanded / "artwork", tinyPNG);
    WriteBytes(rbExpanded / "unknown/member", {9, 8, 7, 6});

    const auto rbPlain = rbRoot / "123456789.rb";
    bmt::PackRB(rbExpanded, rbPlain, std::nullopt);
    assert(HasTrailingMD5(rbPlain));
    assert(ReadZipEntry(rbPlain, "info") == ReadBytes(rbExpanded / "info"));
    assert(ReadZipEntry(rbPlain, "unknown/member") ==
           ReadBytes(rbExpanded / "unknown/member"));
    const auto typedRB = bmt::LoadRBPacks(
        {{bmt::DLCType::Official, rbRoot}},
        {.mode = bmt::LoadMode::Eager,
         .failureMode = bmt::FailureMode::Strict});
    assert(typedRB.packs.at(123456789).front().version == 2);
    assert(typedRB.packs.at(123456789).front().options.contains("note_har2"));
    assert(typedRB.packs.at(123456789).front().options.contains("Unknown"));
    assert(typedRB.packs.at(123456789).front().resources.at("note_har2").Data() ==
           ReadBytes(rbExpanded / "note_har2"));
    const auto& typedPack = typedRB.packs.at(123456789).front();
    const auto* hardLight = bmt::ResolveRBResource(
        typedPack,
        bmt::SelectRBNoteResource(bmt::RBDifficulty::Hard, true));
    assert(hardLight && hardLight->name == "note_har2");
    const auto* mediumLight = bmt::ResolveRBResource(
        typedPack,
        bmt::SelectRBNoteResource(bmt::RBDifficulty::Medium, true));
    assert(mediumLight && mediumLight->name == "note_med");
    const auto* mediumMusic = bmt::ResolveRBResource(
        typedPack,
        bmt::SelectRBAudioResource(bmt::RBDifficulty::Medium));
    assert(mediumMusic && mediumMusic->name == "bgm");
    const auto* commonArtwork = bmt::ResolveRBResource(
        typedPack,
        bmt::SelectRBImageResource(
            bmt::RBImageKind::Artwork, bmt::RBImageScale::OneX));
    assert(commonArtwork && commonArtwork->name == "artwork");
    assert(!bmt::ResolveRBResource(
        typedPack,
        bmt::SelectRBImageResource(
            bmt::RBImageKind::Artwork, bmt::RBImageScale::OneX,
            bmt::RBDifficulty::Basic)));

#ifndef _WIN32
    const auto rbSymlinkInput = rbRoot / "symlink-input";
    const auto rbSymlinkOutput = rbRoot / "symlink-output";
    std::filesystem::create_directories(rbSymlinkInput);
    std::filesystem::create_symlink(
        std::filesystem::absolute(rbPlain),
        rbSymlinkInput / "123456789.rb");
    bmt::UnpackRBDirectory(rbSymlinkInput, rbSymlinkOutput);
    assert(ReadBytes(rbSymlinkOutput / "123456789" / "note_har2") ==
           ReadBytes(rbExpanded / "note_har2"));
#endif

    const auto paddedRBDirectory = rbRoot / "padded-id";
    std::filesystem::create_directories(paddedRBDirectory);
    std::filesystem::copy_file(rbPlain,
                               paddedRBDirectory / "0123456789.rb");
    bool rejectedPaddedRBID = false;
    try
    {
        (void)bmt::LoadRBPacks(
            {{bmt::DLCType::Official, paddedRBDirectory}},
            {.mode = bmt::LoadMode::Eager,
             .failureMode = bmt::FailureMode::Strict});
    }
    catch (const std::runtime_error& error)
    {
        rejectedPaddedRBID =
            std::string_view(error.what()).find("leading zeros") !=
            std::string_view::npos;
    }
    assert(rejectedPaddedRBID);

    for (uint8_t decodeType = 0; decodeType < 2; ++decodeType)
    {
        const auto encryptedRB =
            rbRoot / (std::string("type-") + std::to_string(decodeType)) / "123456789.rb";
        bmt::PackRB(rbExpanded, encryptedRB, decodeType);
        assert(HasTrailingMD5(encryptedRB));
        assert(bmt::IsBFContainer(ReadZipEntry(encryptedRB, "info")));
        const auto unpacked =
            rbRoot / (std::string("unpacked-") + std::to_string(decodeType));
        bmt::UnpackRB(encryptedRB, unpacked);
        assert(ReadBytes(unpacked / "info") == ReadBytes(rbExpanded / "info"));
        assert(ReadBytes(unpacked / "note_bas") == ReadBytes(rbExpanded / "note_bas"));
        assert(ReadBytes(unpacked / "note_har2") == ReadBytes(rbExpanded / "note_har2"));
        assert(ReadBytes(unpacked / "unknown/member") ==
               ReadBytes(rbExpanded / "unknown/member"));

        const auto decryptedRB =
            rbRoot / (std::string("decrypted-") + std::to_string(decodeType)) /
            "123456789.rb";
        bmt::DecryptRB(encryptedRB, decryptedRB);
        assert(!bmt::IsBFContainer(ReadZipEntry(decryptedRB, "info")));
        const auto reencryptedRB =
            rbRoot / (std::string("reencrypted-") + std::to_string(decodeType)) /
            "123456789.rb";
        bmt::EncryptRB(decryptedRB, reencryptedRB, decodeType);
        const auto roundTrip =
            rbRoot / (std::string("roundtrip-") + std::to_string(decodeType));
        bmt::UnpackRB(reencryptedRB, roundTrip);
        assert(ReadBytes(roundTrip / "info") == ReadBytes(rbExpanded / "info"));
        assert(ReadBytes(roundTrip / "note_har2") ==
               ReadBytes(rbExpanded / "note_har2"));
        assert(ReadBytes(roundTrip / "unknown/member") ==
               ReadBytes(rbExpanded / "unknown/member"));
    }

    const auto rbListPlain = rbRoot / "nolist.plist";
    WriteText(rbListPlain,
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
              "<plist version=\"1.0\"><array><dict>"
              "<key>ID</key><integer>123456789</integer>"
              "</dict></array></plist>");
    const auto rbListEncrypted = rbRoot / "nolist";
    WriteBytes(rbListEncrypted, bmt::EncryptRBList(rbListPlain, "RB-LIST-KEY"));
    assert(bmt::DecryptRBList(rbListEncrypted, "RB-LIST-KEY") ==
           ReadBytes(rbListPlain));

    const std::array<uint8_t, 16> rbType0MD5 = {
        0x8f, 0x26, 0xf6, 0x77, 0x51, 0xad, 0x54, 0x94,
        0x15, 0x3a, 0x6c, 0x98, 0xfa, 0x85, 0xfe, 0x1f,
    };
    const std::array<uint8_t, 16> rbType1MD5 = {
        0x40, 0x4b, 0xd8, 0x02, 0x6b, 0xec, 0x64, 0x66,
        0xe5, 0x32, 0xdb, 0xf0, 0x2d, 0x0d, 0xe5, 0xe4,
    };
    assert(bmt::MD5Key("Konami ReflecBeat For iOS.") == rbType0MD5);
    assert(bmt::MD5Key("Konami ReflecBeatplus.") == rbType1MD5);

    static constexpr std::array<std::string_view, 42> rbHotFields = {
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
    constexpr uint32_t rbHotID = 806202001;
    constexpr std::string_view rbHotPassword = "RBHotPass!";
    const uint32_t rbHotLow = rbHotID % 23456;
    const uint32_t rbHotHigh = rbHotID / 23456;
    assert(rbHotHigh <= std::numeric_limits<uint16_t>::max());
    std::map<std::string, std::vector<uint8_t>> rbHotMembers;
    std::map<std::string, std::vector<uint8_t>> rbHotPlaintext;
    std::string rbHotInfo = rbInfo;
    const auto rbHotInfoIDPosition = rbHotInfo.find("123456789");
    assert(rbHotInfoIDPosition != std::string::npos);
    rbHotInfo.replace(rbHotInfoIDPosition, 9, std::to_string(rbHotID));
    std::ostringstream rbHotMusicJSON;
    rbHotMusicJSON << "{\"data\":{\"" << rbHotID << "\":{\"id\":" << rbHotID
                   << ",\"mainId\":806202000,\"password\":\"" << rbHotPassword
                   << "\"";
    for (uint8_t type = 0; type < rbHotFields.size(); ++type)
    {
        const std::string name =
            type == 0 ? "info" : "type_" + std::to_string(type);
        std::vector<uint8_t> plaintext;
        if (type == 0)
            plaintext.assign(rbHotInfo.begin(), rbHotInfo.end());
        else
            plaintext = {'T', 'Y', 'P', 'E', type, 0, 1, 2, 3};
        const auto encoded =
            Base64Encode(EncryptRNCryptor(plaintext, rbHotPassword, type));
        const size_t prefixSize = std::min<size_t>(17, encoded.size());
        rbHotMusicJSON << ",\"" << rbHotFields[type] << "\":\""
                       << encoded.substr(0, prefixSize) << "\"";
        std::vector<uint8_t> resource(
            {'=', 'R', 'B', 'H', 'O', 'T', '=',
             static_cast<uint8_t>(rbHotLow),
             static_cast<uint8_t>(rbHotLow >> 8), type,
             static_cast<uint8_t>(rbHotHigh),
             static_cast<uint8_t>(rbHotHigh >> 8)});
        resource.insert(resource.end(), encoded.begin() +
                                          static_cast<std::ptrdiff_t>(prefixSize),
                        encoded.end());
        rbHotMembers[name] = std::move(resource);
        rbHotPlaintext[name] = std::move(plaintext);
    }
    rbHotMusicJSON << "}}}";
    const std::string rbHotMusic = rbHotMusicJSON.str();
    const std::array<uint8_t, 16> rbHotDefaultsKey = {
        'b', '3', '8', 'f', 'h', '4', '9', '5',
        'h', '9', 'f', 'j', 'h', 'w', '3', '4',
    };
    const std::array<uint8_t, 16> zeroIV{};
    const auto rbHotMusicCipher = EncryptAES(
        std::span(reinterpret_cast<const uint8_t*>(rbHotMusic.data()),
                  rbHotMusic.size()),
        rbHotDefaultsKey, zeroIV, EVP_aes_128_cbc());
    const std::string rbHotDefaultsXML =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<plist version=\"1.0\"><dict><key>musicData</key><string>" +
        Base64Encode(rbHotMusicCipher) + "</string></dict></plist>";
    const auto rbHotFixture = rbRoot / "rbhot-fixture" / "806202001.rb";
    const auto rbHotDefaults = rbRoot / "rbhot-fixture" / "defaults.plist";
    WriteTestZip(rbHotFixture, rbHotMembers);
    WriteText(rbHotDefaults, rbHotDefaultsXML);
    const auto rbHotUnpacked = rbRoot / "rbhot-fixture" / "unpacked";
    bmt::UnpackRB(rbHotFixture, rbHotUnpacked,
                  {.rbhotDefaultsPlist = rbHotDefaults});
    for (const auto& [name, plaintext] : rbHotPlaintext)
        assert(ReadBytes(rbHotUnpacked / name) == plaintext);

    std::string wrongPasswordJSON = rbHotMusic;
    const auto passwordPosition = wrongPasswordJSON.find(rbHotPassword);
    assert(passwordPosition != std::string::npos);
    wrongPasswordJSON.replace(passwordPosition, rbHotPassword.size(), "WrongPass!");
    const auto wrongCipher = EncryptAES(
        std::span(reinterpret_cast<const uint8_t*>(wrongPasswordJSON.data()),
                  wrongPasswordJSON.size()),
        rbHotDefaultsKey, zeroIV, EVP_aes_128_cbc());
    const std::string wrongDefaultsXML =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<plist version=\"1.0\"><dict><key>musicData</key><string>" +
        Base64Encode(wrongCipher) + "</string></dict></plist>";
    const auto wrongDefaults = rbRoot / "rbhot-fixture" / "wrong-defaults.plist";
    WriteText(wrongDefaults, wrongDefaultsXML);
    bool rejectedWrongRBHotPassword = false;
    try
    {
        bmt::UnpackRB(rbHotFixture, rbRoot / "rbhot-fixture" / "wrong",
                      {.rbhotDefaultsPlist = wrongDefaults});
    }
    catch (const std::runtime_error& error)
    {
        rejectedWrongRBHotPassword =
            std::string_view(error.what()).find("HMAC") != std::string_view::npos;
    }
    assert(rejectedWrongRBHotPassword);

    const auto rbBuild = rbRoot / "build";
    const auto rbOfficial = rbBuild / "official";
    const auto rbDuplicate = rbBuild / "custom-duplicate";
    std::filesystem::create_directories(rbOfficial);
    std::filesystem::create_directories(rbDuplicate);
    std::filesystem::copy_file(
        rbRoot / "type-0" / "123456789.rb",
        rbOfficial / "123456789.rb");
    std::filesystem::copy_file(
        rbRoot / "type-0" / "123456789.rb",
        rbDuplicate / "123456789.rb");

    const auto rbExtExpanded = rbBuild / "ext-expanded";
    std::string rbExtInfo = rbInfo;
    const auto rbExtIDPosition = rbExtInfo.find("123456789");
    assert(rbExtIDPosition != std::string::npos);
    rbExtInfo.replace(rbExtIDPosition, 9, "123456790");
    WriteText(rbExtExpanded / "info", rbExtInfo);
    WriteBytes(rbExtExpanded / "note_bas", {'R', 'B', 'F', 'F', 9, 9, 9});
    bmt::PackRB(rbExtExpanded, rbOfficial / "123456790.rb", uint8_t{1});

    WriteText(
        rbOfficial / "mulist.plist",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<plist version=\"1.0\"><array><dict>"
        "<key>ID</key><integer>123456789</integer>"
        "<key>Name</key><string>Catalog Name</string>"
        "<key>Artist</key><string>Catalog Artist</string>"
        "<key>ItemURL</key><string>https://example.invalid/base</string>"
        "</dict></array></plist>");
    WriteText(
        rbOfficial / "nolist.plist",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<plist version=\"1.0\"><array><dict>"
        "<key>ExtID</key><integer>123456790</integer>"
        "<key>PackID</key><integer>42</integer>"
        "<key>ID</key><integer>123456789</integer>"
        "<key>ExtLevel</key><integer>3</integer>"
        "<key>Comment</key><string>SPECIAL</string>"
        "</dict></array></plist>");
    bmt::ExportPlaylists(
        {{"0123456789abcdef0123456789abcdef", "RB Playlist",
          {123456789, 123456790}}},
        rbOfficial / "playlist.plist");

    auto rbLoaded = bmt::LoadRBPacks({
        {bmt::DLCType::Official, rbOfficial},
        {bmt::DLCType::Custom, rbDuplicate},
    }, {.mode = bmt::LoadMode::Lazy, .failureMode = bmt::FailureMode::Strict});
    assert(rbLoaded.packs.size() == 2);
    assert(rbLoaded.droppedDuplicates == 1);
    assert(rbLoaded.extensions.size() == 1);
    assert(rbLoaded.playlists.size() == 1);
    assert(rbLoaded.packs.at(123456789).front().decodeType == 0);
    assert(rbLoaded.packs.at(123456790).front().decodeType == 1);

    const auto rbMerged = rbBuild / "merged";
    bmt::ExportRBPacks(
        rbLoaded, rbMerged,
        {.encryptRB = true,
         .outputKey = bmt::RBOutputKey::Preserve,
         .mulistKey = std::string("RB-LIST-KEY")});
    assert(std::filesystem::is_regular_file(rbMerged / "123456789.rb"));
    assert(std::filesystem::is_regular_file(rbMerged / "123456790.rb"));
    assert(std::filesystem::is_regular_file(rbMerged / "mulist.plist"));
    assert(std::filesystem::is_regular_file(rbMerged / "nolist.plist"));
    assert(std::filesystem::is_regular_file(rbMerged / "playlist.plist"));
    assert(std::filesystem::is_regular_file(rbMerged / "mulist"));
    assert(std::filesystem::is_regular_file(rbMerged / "nolist"));
    assert(std::filesystem::is_regular_file(rbMerged / "playlist"));
    assert(bmt::DecryptRBList(rbMerged / "mulist", "RB-LIST-KEY") ==
           ReadBytes(rbMerged / "mulist.plist"));
    assert(bmt::DecryptRBList(rbMerged / "nolist", "RB-LIST-KEY") ==
           ReadBytes(rbMerged / "nolist.plist"));
    const auto rbReloaded = bmt::LoadRBPacks(
        {{bmt::DLCType::Official, rbMerged}},
        {.mode = bmt::LoadMode::Eager,
         .failureMode = bmt::FailureMode::Strict,
         .mulistKey = std::string("RB-LIST-KEY")});
    assert(rbReloaded.packs.size() == 2);
    assert(rbReloaded.packs.at(123456789).front().resources.at("unknown/member").Data() ==
           ReadBytes(rbExpanded / "unknown/member"));
    assert(rbReloaded.extensions.size() == 1);
    assert(rbReloaded.extensions.front().packID == 42);
    assert(rbReloaded.playlists.front().musicIDs ==
           (std::vector<uint32_t>{123456789, 123456790}));

    const auto rbConflict = rbBuild / "custom-conflict";
    const auto rbConflictExpanded = rbBuild / "custom-conflict-expanded";
    WriteText(rbConflictExpanded / "info", rbInfo);
    WriteBytes(rbConflictExpanded / "note_bas", {'R', 'B', 'F', 'F', 4, 3, 2, 1});
    WriteBytes(rbConflictExpanded / "unknown/member", {1, 1, 2, 3});
    bmt::PackRB(rbConflictExpanded, rbConflict / "123456789.rb", std::nullopt);
    WriteText(rbConflict / "mapping.json", "{\"123456789\":223456789}\n");
    WriteText(
        rbConflict / "mulist.plist",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<plist version=\"1.0\"><array><dict>"
        "<key>ID</key><integer>123456789</integer>"
        "<key>Name</key><string>Mapped Song</string>"
        "<key>Artist</key><string>Mapped Artist</string>"
        "</dict></array></plist>");
    bmt::ExportPlaylists(
        {{"abcdef0123456789abcdef0123456789", "Mapped Playlist", {123456789}}},
        rbConflict / "playlist.plist");
    auto rbMapped = bmt::LoadRBPacks({
        {bmt::DLCType::Custom, rbConflict},
        {bmt::DLCType::Official, rbOfficial},
    }, {.mode = bmt::LoadMode::Eager, .failureMode = bmt::FailureMode::Strict});
    assert(rbMapped.packs.size() == 3);
    assert(rbMapped.packs.at(123456789).front().dlcType == bmt::DLCType::Official);
    assert(rbMapped.packs.at(223456789).front().originalID == 123456789);
    assert(rbMapped.packs.at(223456789).front().id == 223456789);
    assert(rbMapped.remaps.size() == 1);
    assert(std::find_if(
               rbMapped.catalog.begin(), rbMapped.catalog.end(),
               [](const bmt::RBCatalogEntry& entry)
               {
                   return entry.id == 223456789 && entry.name == "Mapped Song";
               }) != rbMapped.catalog.end());
    assert(std::find_if(
               rbMapped.playlists.begin(), rbMapped.playlists.end(),
               [](const bmt::Playlist& playlist)
               {
                   return playlist.name == "Mapped Playlist" &&
                          playlist.musicIDs == (std::vector<uint32_t>{223456789});
               }) != rbMapped.playlists.end());

    const auto rbOrphan = rbBuild / "orphan";
    std::filesystem::create_directories(rbOrphan);
    std::filesystem::copy_file(rbOfficial / "123456790.rb",
                               rbOrphan / "123456790.rb");
    WriteText(
        rbOrphan / "nolist.plist",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<plist version=\"1.0\"><array><dict>"
        "<key>ExtID</key><integer>123456790</integer>"
        "<key>PackID</key><integer>7</integer>"
        "<key>ID</key><integer>999999999</integer>"
        "<key>ExtLevel</key><integer>3</integer>"
        "</dict></array></plist>");
    bool rejectedOrphan = false;
    try
    {
        (void)bmt::LoadRBPacks(
            {{bmt::DLCType::Custom, rbOrphan}},
            {.mode = bmt::LoadMode::Eager,
             .failureMode = bmt::FailureMode::Strict});
    }
    catch (const std::runtime_error& error)
    {
        rejectedOrphan =
            std::string_view(error.what()).find("dangling nolist") !=
            std::string_view::npos;
    }
    assert(rejectedOrphan);
    auto retainedOrphan = bmt::LoadRBPacks(
        {{bmt::DLCType::Custom, rbOrphan}},
        {.mode = bmt::LoadMode::Eager,
         .failureMode = bmt::FailureMode::Continue});
    assert(retainedOrphan.packs.size() == 1);
    assert(!retainedOrphan.warnings.empty());
    const auto rbOrphanOutput = rbBuild / "orphan-output";
    bmt::ExportRBPacks(retainedOrphan, rbOrphanOutput,
                       {.encryptRB = false});
    assert(std::filesystem::is_regular_file(
        rbOrphanOutput / "123456790.rb"));

    const auto rbMulti = rbBuild / "one-to-many";
    const std::array<uint32_t, 3> rbMultiIDs = {
        323456789, 323456790, 323456791,
    };
    for (size_t index = 0; index < rbMultiIDs.size(); ++index)
    {
        const auto expanded = rbBuild / ("multi-expanded-" + std::to_string(index));
        std::string info = rbInfo;
        const auto position = info.find("123456789");
        assert(position != std::string::npos);
        info.replace(position, 9, std::to_string(rbMultiIDs[index]));
        WriteText(expanded / "info", info);
        WriteBytes(expanded / "note_bas",
                   {'R', 'B', 'F', 'F', static_cast<uint8_t>(index)});
        bmt::PackRB(expanded, rbMulti / (std::to_string(rbMultiIDs[index]) + ".rb"),
                    uint8_t{0});
    }
    WriteText(
        rbMulti / "nolist.plist",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<plist version=\"1.0\"><array>"
        "<dict><key>ExtID</key><integer>323456790</integer>"
        "<key>PackID</key><integer>8</integer>"
        "<key>ID</key><integer>323456789</integer>"
        "<key>ExtLevel</key><integer>4</integer></dict>"
        "<dict><key>ExtID</key><integer>323456791</integer>"
        "<key>PackID</key><integer>8</integer>"
        "<key>ID</key><integer>323456789</integer>"
        "<key>ExtLevel</key><integer>5</integer></dict>"
        "</array></plist>");
    auto rbMultiLoaded = bmt::LoadRBPacks(
        {{bmt::DLCType::Custom, rbMulti}},
        {.mode = bmt::LoadMode::Lazy,
         .failureMode = bmt::FailureMode::Strict});
    assert(rbMultiLoaded.packs.size() == 3);
    assert(rbMultiLoaded.extensions.size() == 2);
    const auto rbMultiOutput = rbBuild / "one-to-many-output";
    bmt::ExportRBPacks(rbMultiLoaded, rbMultiOutput,
                       {.encryptRB = true,
                        .outputKey = bmt::RBOutputKey::Type1});
    auto rbMultiReloaded = bmt::LoadRBPacks(
        {{bmt::DLCType::Official, rbMultiOutput}},
        {.mode = bmt::LoadMode::Eager,
         .failureMode = bmt::FailureMode::Strict});
    assert(rbMultiReloaded.packs.size() == 3);
    assert(rbMultiReloaded.extensions.size() == 2);

    std::filesystem::remove_all(output);

    std::cout << "BMTTests passed\n";
    return 0;
}

int main()
{
    try
    {
        return RunTests();
    }
    catch (const std::exception& error)
    {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
