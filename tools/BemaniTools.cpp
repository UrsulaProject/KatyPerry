#include <Bemani/BFContainer.h>
#include <Bemani/JBT.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    void WriteFile(const fs::path& path, const std::vector<uint8_t>& data)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot create " + path.string());
        output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!output)
            throw std::runtime_error("cannot write " + path.string());
    }

    void Usage()
    {
        std::cerr
            << "Usage:\n"
            << "  BemaniTools mulist-decrypt <mulist> <keychain-plist> <output-plist>\n"
            << "  BemaniTools load [options] <jbt-or-directory>...\n\n"
            << "load options:\n"
            << "  --eager                    materialize every resource\n"
            << "  --strict                   stop on the first invalid pack\n"
            << "  --jbhot-plist <path>       JBHot NSUserDefaults plist\n"
            << "  --music-json <path>        decrypted JBHot musicData JSON\n"
            << "  --server-data <path>       decrypted JBHot serverData JSON\n"
            << "  --catalog <path>           official plaintext mulist plist\n"
            << "  --resolve <first> <last>   resolve duplicate IDs in this range\n"
            << "  --export <directory>       write JBTs, mulist.plist, and playlists.plist\n";
    }

    uint32_t ParseID(const char* text)
    {
        size_t consumed = 0;
        const auto value = std::stoull(text, &consumed, 10);
        if (text[consumed] != '\0' || value > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error(std::string("invalid ID: ") + text);
        return static_cast<uint32_t>(value);
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc < 2)
        {
            Usage();
            return 1;
        }
        const std::string command = argv[1];
        if (command == "mulist-decrypt")
        {
            if (argc != 5)
            {
                Usage();
                return 1;
            }
            const auto plaintext = bmt::DecryptOfficialMusicList(argv[2], argv[3]);
            WriteFile(argv[4], plaintext);
            std::cout << "wrote " << plaintext.size() << " bytes to " << argv[4] << '\n';
            return 0;
        }
        if (command != "load")
        {
            Usage();
            return 1;
        }

        bmt::LoadOptions options;
        bmt::ResolveOptions resolveOptions;
        bool resolve = false;
        std::optional<fs::path> exportDirectory;
        std::vector<fs::path> inputs;
        for (int index = 2; index < argc; ++index)
        {
            const std::string argument = argv[index];
            auto requireValue = [&]() -> fs::path
            {
                if (++index >= argc)
                    throw std::runtime_error(argument + " requires a value");
                return argv[index];
            };
            if (argument == "--eager") options.mode = bmt::LoadMode::Eager;
            else if (argument == "--strict") options.failureMode = bmt::FailureMode::Strict;
            else if (argument == "--jbhot-plist") options.jbhotDefaultsPlist = requireValue();
            else if (argument == "--music-json") options.musicDataJson = requireValue();
            else if (argument == "--server-data") options.serverDataJson = requireValue();
            else if (argument == "--catalog") options.catalogPlist = requireValue();
            else if (argument == "--export") exportDirectory = requireValue();
            else if (argument == "--resolve")
            {
                if (index + 2 >= argc)
                    throw std::runtime_error("--resolve requires first and last IDs");
                resolveOptions.firstReservedID = ParseID(argv[++index]);
                resolveOptions.lastReservedID = ParseID(argv[++index]);
                resolve = true;
            }
            else if (argument.starts_with("--"))
                throw std::runtime_error("unknown option " + argument);
            else
                inputs.emplace_back(argument);
        }
        if (inputs.empty())
            throw std::runtime_error("load requires at least one JBT or directory");
        if (exportDirectory)
            options.failureMode = bmt::FailureMode::Strict;

        auto result = bmt::LoadPacks(inputs, options);
        size_t instances = 0;
        size_t conflicts = 0;
        for (const auto& [id, packs] : result.packs)
        {
            instances += packs.size();
            conflicts += packs.size() > 1;
        }
        std::cout << "loaded " << instances << " packs in " << result.packs.size()
                  << " ID groups; conflicts=" << conflicts
                  << "; playlists=" << result.playlists.size()
                  << "; diagnostics=" << result.diagnostics.size() << '\n';
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << diagnostic.path << ": " << diagnostic.message << '\n';

        if (resolve || exportDirectory)
        {
            const auto remaps = bmt::ResolveConflicts(result, resolveOptions);
            std::cout << "remapped " << remaps.size() << " packs\n";
        }
        if (exportDirectory)
        {
            bmt::ExportPacks(result, *exportDirectory);
            std::cout << "exported to " << *exportDirectory << '\n';
        }
        return result.diagnostics.empty() ? 0 : 2;
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
