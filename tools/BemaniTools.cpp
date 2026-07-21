#include <Bemani/BFContainer.h>
#include <Bemani/JBT.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
            << "  BemaniTools load [options]\n\n"
            << "load options:\n"
            << "  --eager                    materialize every resource\n"
            << "  --strict                   stop on the first invalid pack\n"
            << "  --official <directory>     Official DLC directory (once)\n"
            << "  --jbhot <directory>        JBHot DLC directory (once)\n"
            << "  --jbhot-plist=<path>       encrypted JBHot NSUserDefaults plist\n"
            << "  --custom-dir=<directory>   Custom DLC directory (repeatable)\n"
            << "  --custom-range=<first>,<last>  range for preceding Custom DLC\n"
            << "  --catalog <path>           official plaintext mulist plist\n"
            << "  --resolve <first> <last>   JBHot conflict ID range\n"
            << "  --export <directory>       write JBTs, mulist.plist, and playlists.plist\n"
            << "  --encrypt-jbt=true|false   encrypt output JBT members (default: true)\n"
            << "  --separate-output          place JBTs in per-DLC subdirectories\n"
            << "  --mulist-key=<key>         also write encrypted mulist using this raw key\n"
            << "  --playlist-export <path>   write merged playlists.plist without exporting JBTs\n";
    }

    uint32_t ParseID(const char* text)
    {
        size_t consumed = 0;
        const auto value = std::stoull(text, &consumed, 10);
        if (text[consumed] != '\0' || value > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error(std::string("invalid ID: ") + text);
        return static_cast<uint32_t>(value);
    }

    std::pair<uint32_t, uint32_t> ParseRange(std::string_view text)
    {
        const auto comma = text.find(',');
        if (comma == std::string_view::npos || text.find(',', comma + 1) != std::string_view::npos)
            throw std::runtime_error("Custom range must be <first>,<last>");
        const std::string first(text.substr(0, comma));
        const std::string last(text.substr(comma + 1));
        return {ParseID(first.c_str()), ParseID(last.c_str())};
    }

    bool ParseBoolean(std::string_view value)
    {
        if (value == "true")
            return true;
        if (value == "false")
            return false;
        throw std::runtime_error("boolean value must be true or false");
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
        bmt::ExportOptions exportOptions;
        bool resolve = false;
        std::optional<fs::path> exportDirectory;
        std::optional<fs::path> playlistExport;
        std::optional<fs::path> officialDirectory;
        std::optional<fs::path> jbhotDirectory;
        std::vector<bmt::DLCSource> customSources;
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
            else if (argument == "--official")
            {
                if (officialDirectory)
                    throw std::runtime_error("--official may only be specified once");
                officialDirectory = requireValue();
            }
            else if (argument == "--jbhot")
            {
                if (jbhotDirectory)
                    throw std::runtime_error("--jbhot may only be specified once");
                jbhotDirectory = requireValue();
            }
            else if (argument.starts_with("--jbhot-plist="))
                options.jbhotDefaultsPlist = argument.substr(14);
            else if (argument == "--jbhot-plist") options.jbhotDefaultsPlist = requireValue();
            else if (argument.starts_with("--custom-dir="))
            {
                if (!customSources.empty() && !customSources.back().firstID)
                    throw std::runtime_error("the preceding --custom-dir has no --custom-range");
                customSources.push_back({bmt::DLCType::Custom, argument.substr(13), 0, 0});
            }
            else if (argument.starts_with("--custom-range="))
            {
                if (customSources.empty() || customSources.back().firstID)
                    throw std::runtime_error("--custom-range must follow one --custom-dir");
                const auto [first, last] = ParseRange(std::string_view(argument).substr(15));
                customSources.back().firstID = first;
                customSources.back().lastID = last;
            }
            else if (argument == "--catalog") options.catalogPlist = requireValue();
            else if (argument == "--export") exportDirectory = requireValue();
            else if (argument == "--playlist-export") playlistExport = requireValue();
            else if (argument == "--separate-output") exportOptions.separateByDLC = true;
            else if (argument.starts_with("--encrypt-jbt="))
                exportOptions.encryptJBT = ParseBoolean(std::string_view(argument).substr(14));
            else if (argument.starts_with("--mulist-key="))
                exportOptions.mulistKey = argument.substr(13);
            else if (argument == "--mulist-key")
                exportOptions.mulistKey = requireValue().string();
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
                throw std::runtime_error("unexpected positional input " + argument);
        }
        if (!customSources.empty() && !customSources.back().firstID)
            throw std::runtime_error("the final --custom-dir has no --custom-range");
        std::vector<bmt::DLCSource> sources;
        if (officialDirectory)
            sources.push_back({bmt::DLCType::Official, *officialDirectory});
        if (jbhotDirectory)
            sources.push_back({bmt::DLCType::JBHot, *jbhotDirectory});
        sources.insert(sources.end(), customSources.begin(), customSources.end());
        if (sources.empty())
            throw std::runtime_error("load requires --official, --jbhot, or --custom-dir");
        if (jbhotDirectory && !options.jbhotDefaultsPlist)
            throw std::runtime_error("--jbhot requires --jbhot-plist");
        if (exportDirectory)
            options.failureMode = bmt::FailureMode::Strict;

        auto result = bmt::LoadPacks(sources, options);
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
            std::cout << "dropped " << result.droppedDuplicates
                      << " identical packs; remapped " << remaps.size() << " packs\n";
        }
        if (exportDirectory)
        {
            const size_t warningStart = result.warnings.size();
            bmt::ExportPacks(result, *exportDirectory, exportOptions);
            for (size_t index = warningStart; index < result.warnings.size(); ++index)
                std::cerr << "warning: " << result.warnings[index].path << ": "
                          << result.warnings[index].message << '\n';
            std::cout << "exported to " << *exportDirectory << '\n';
        }
        if (playlistExport)
        {
            bmt::ExportPlaylists(result.playlists, *playlistExport);
            std::cout << "exported " << result.playlists.size()
                      << " playlists to " << *playlistExport << '\n';
        }
        return result.diagnostics.empty() ? 0 : 2;
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
