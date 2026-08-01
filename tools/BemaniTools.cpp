#include <Bemani/BFContainer.h>
#include <Bemani/JBT.h>
#include <Bemani/Marker.h>
#include <Bemani/RB.h>

#include <boost/program_options.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace po = boost::program_options;

namespace boost::program_options
{
    template<>
    void validate<fs::path, char>(boost::any& value,
                                  const std::vector<std::string>& values,
                                  fs::path*, long)
    {
        validators::check_first_occurrence(value);
        value = boost::any(fs::path(validators::get_single_string(values)));
    }
}

namespace
{
    void WriteFile(const fs::path& path, const std::vector<uint8_t>& data)
    {
        if (!path.parent_path().empty())
            fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot create " + path.string());
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
        if (!output)
            throw std::runtime_error("cannot write " + path.string());
    }

    void WriteText(const fs::path& path, std::string_view data)
    {
        WriteFile(path, std::vector<uint8_t>(data.begin(), data.end()));
    }

    std::vector<std::string> Arguments(int argc, char** argv, int first)
    {
        std::vector<std::string> output;
        for (int index = first; index < argc; ++index)
            output.emplace_back(argv[index]);
        return output;
    }

    po::variables_map Parse(const std::vector<std::string>& arguments,
                            const po::options_description& options,
                            const po::positional_options_description* positional = nullptr)
    {
        auto parser = po::command_line_parser(arguments).options(options);
        if (positional)
            parser.positional(*positional);
        po::variables_map values;
        po::store(parser.run(), values);
        if (!values.count("help"))
            po::notify(values);
        return values;
    }

    void RootUsage()
    {
        std::cout
            << "Usage: BemaniTools <group> <command> [options]\n\n"
            << "Groups and commands:\n"
            << "  bfcodec decrypt|encrypt\n"
            << "  dlc build\n"
            << "  mulist decrypt|encrypt\n"
            << "  jbt decrypt|encrypt|unpack|pack|unpack-dir|pack-dir\n"
            << "  jbhot defaults-dump\n"
            << "  rb build|decrypt|encrypt|unpack|pack|unpack-dir|pack-dir\n"
            << "  rbhot defaults-dump\n"
            << "  nolist decrypt|encrypt\n"
            << "  marker decrypt|encrypt|unpack|pack|unpack-dir|pack-dir|build\n"
            << "  marker-list decrypt\n"
            << "\nUse BemaniTools <group> <command> --help for command options.\n";
    }

    int RunBFCodec(std::string_view command, const std::vector<std::string>& arguments)
    {
        fs::path input;
        fs::path output;
        std::string key;
        po::options_description options("bfcodec " + std::string(command) + " options");
        options.add_options()
            ("help,h", "show help")
            ("input,i", po::value<fs::path>(&input)->required(), "input file")
            ("output,o", po::value<fs::path>(&output)->required(), "output file")
            ("key,k", po::value<std::string>(&key)->required(), "raw key before MD5 derivation");
        const auto values = Parse(arguments, options);
        if (values.count("help"))
        {
            std::cout << options << '\n';
            return 0;
        }
        if (command == "decrypt")
            bmt::DecryptBFFile(input, output, key);
        else if (command == "encrypt")
            bmt::EncryptBFFile(input, output, key);
        else
            throw std::runtime_error("unknown bfcodec command " + std::string(command));
        return 0;
    }

    int RunMulist(std::string_view command, const std::vector<std::string>& arguments)
    {
        fs::path input;
        fs::path output;
        std::string key;
        po::options_description options("mulist " + std::string(command) + " options");
        options.add_options()
            ("help,h", "show help")
            ("input,i", po::value<fs::path>(&input)->required(), "input file")
            ("output,o", po::value<fs::path>(&output)->required(), "output file")
            ("key,k", po::value<std::string>(&key)->required(), "raw key before MD5 derivation");
        const auto values = Parse(arguments, options);
        if (values.count("help"))
        {
            std::cout << options << '\n';
            return 0;
        }
        if (command == "decrypt")
            WriteFile(output, bmt::DecryptOfficialMusicList(input, key));
        else if (command == "encrypt")
            WriteFile(output, bmt::EncryptOfficialMusicList(input, key));
        else
            throw std::runtime_error("unknown mulist command " + std::string(command));
        return 0;
    }

    int RunJBT(std::string_view command, const std::vector<std::string>& arguments)
    {
        fs::path input;
        fs::path output;
        fs::path defaults;
        bool plain = false;
        unsigned int jobs = 0;
        po::options_description options("jbt " + std::string(command) + " options");
        options.add_options()
            ("help,h", "show help")
            ("input,i", po::value<fs::path>(&input)->required(), "input JBT or directory")
            ("output,o", po::value<fs::path>(&output)->required(), "output JBT or directory")
            ("jbhot-plist", po::value<fs::path>(&defaults), "encrypted JBHot defaults plist")
            ("jobs,j", po::value<unsigned int>(&jobs)->default_value(0),
             "parallel file jobs; 0 uses available CPU cores")
            ("plain", po::bool_switch(&plain), "pack plaintext JBT members");
        const auto values = Parse(arguments, options);
        if (values.count("help"))
        {
            std::cout << options << '\n';
            return 0;
        }
        bmt::LoadOptions load;
        load.jobs = jobs;
        if (!defaults.empty())
            load.jbhotDefaultsPlist = defaults;
        if (command == "decrypt")
            bmt::DecryptJBT(input, output, load);
        else if (command == "encrypt")
            bmt::EncryptJBT(input, output);
        else if (command == "unpack")
            bmt::UnpackJBT(input, output, load);
        else if (command == "pack")
            bmt::PackJBT(input, output, !plain);
        else if (command == "unpack-dir")
            bmt::UnpackJBTDirectory(input, output, load);
        else if (command == "pack-dir")
            bmt::PackJBTDirectory(input, output, !plain, jobs);
        else
            throw std::runtime_error("unknown jbt command " + std::string(command));
        return 0;
    }

    int RunJBHot(std::string_view command, const std::vector<std::string>& arguments)
    {
        if (command != "defaults-dump")
            throw std::runtime_error("unknown jbhot command " + std::string(command));
        fs::path input;
        fs::path output;
        po::options_description options("jbhot defaults-dump options");
        options.add_options()
            ("help,h", "show help")
            ("input,i", po::value<fs::path>(&input)->required(), "encrypted defaults plist")
            ("output-dir,o", po::value<fs::path>(&output)->required(), "JSON output directory");
        const auto values = Parse(arguments, options);
        if (values.count("help"))
        {
            std::cout << options << '\n';
            return 0;
        }
        for (const auto& [name, json] : bmt::DumpJBHotDefaults(input))
            WriteText(output / (name + ".json"), json + "\n");
        return 0;
    }

    int RunDLC(std::string_view command, const std::vector<std::string>& arguments)
    {
        if (command != "build")
            throw std::runtime_error("unknown dlc command " + std::string(command));
        fs::path official;
        fs::path jbhot;
        fs::path defaults;
        fs::path output;
        std::vector<fs::path> custom;
        bool eager = false;
        bool strict = false;
        bool encryptJBT = true;
        bool separate = false;
        unsigned int jobs = 0;
        std::string mulistKey;
        po::options_description options("dlc build options");
        options.add_options()
            ("help,h", "show help")
            ("official", po::value<fs::path>(&official), "Official DLC directory")
            ("jbhot", po::value<fs::path>(&jbhot), "JBHot DLC directory")
            ("jbhot-plist", po::value<fs::path>(&defaults), "encrypted JBHot defaults plist")
            ("custom-dir", po::value<std::vector<fs::path>>(&custom)->composing(), "Custom DLC directory; repeatable")
            ("output,o", po::value<fs::path>(&output)->required(), "output directory")
            ("eager", po::bool_switch(&eager), "materialize every resource while loading")
            ("strict", po::bool_switch(&strict), "stop at the first invalid pack")
            ("jobs,j", po::value<unsigned int>(&jobs)->default_value(0),
             "parallel file jobs; 0 uses available CPU cores")
            ("encrypt-jbt", po::value<bool>(&encryptJBT)->default_value(true), "encrypt output JBT members")
            ("separate-output", po::bool_switch(&separate), "write JBTs in per-DLC subdirectories")
            ("mulist-key", po::value<std::string>(&mulistKey), "also emit encrypted mulist with this raw key");
        const auto values = Parse(arguments, options);
        if (values.count("help"))
        {
            std::cout << options << '\n';
            return 0;
        }
        std::vector<bmt::DLCSource> sources;
        if (!official.empty())
            sources.push_back({bmt::DLCType::Official, official});
        if (!jbhot.empty())
            sources.push_back({bmt::DLCType::JBHot, jbhot});
        for (const auto& path : custom)
            sources.push_back({bmt::DLCType::Custom, path});
        if (sources.empty())
            throw std::runtime_error("dlc build requires --official, --jbhot, or --custom-dir");
        if (!jbhot.empty() && defaults.empty())
            throw std::runtime_error("--jbhot requires --jbhot-plist");
        bmt::LoadOptions load;
        load.mode = eager ? bmt::LoadMode::Eager : bmt::LoadMode::Lazy;
        load.failureMode = strict ? bmt::FailureMode::Strict : bmt::FailureMode::Continue;
        load.jobs = jobs;
        if (!defaults.empty())
            load.jbhotDefaultsPlist = defaults;
        auto result = bmt::LoadPacks(sources, load);
        bmt::ExportOptions exportOptions;
        exportOptions.encryptJBT = encryptJBT;
        exportOptions.separateByDLC = separate;
        exportOptions.jobs = jobs;
        if (!mulistKey.empty())
            exportOptions.mulistKey = mulistKey;
        bmt::ExportPacks(result, output, exportOptions);
        for (const auto& warning : result.warnings)
            std::cerr << "warning: " << warning.path << ": " << warning.message << '\n';
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << "error: " << diagnostic.path << ": " << diagnostic.message << '\n';
        std::cout << "exported " << result.packs.size() << " music IDs; dropped "
                  << result.droppedDuplicates << " duplicates; remapped "
                  << result.remaps.size() << " packs\n";
        return result.diagnostics.empty() ? 0 : 2;
    }

    int RunRBConversion(std::string_view command,
                        const std::vector<std::string>& arguments)
    {
        fs::path input;
        fs::path output;
        fs::path defaults;
        unsigned int decodeType = 0;
        unsigned int jobs = 0;
        bool plain = false;
        po::options_description options("rb " + std::string(command) + " options");
        options.add_options()
            ("help,h", "show help")
            ("input,i", po::value<fs::path>(&input)->required(), "input RB or directory")
            ("output,o", po::value<fs::path>(&output)->required(), "output RB or directory")
            ("rbhot-plist", po::value<fs::path>(&defaults), "encrypted RBHot defaults plist")
            ("decode-type", po::value<unsigned int>(&decodeType)->default_value(0),
             "official BF DecodeType for encrypt/pack (0 or 1)")
            ("jobs,j", po::value<unsigned int>(&jobs)->default_value(0),
             "parallel file jobs; 0 uses available CPU cores")
            ("plain", po::bool_switch(&plain), "write plaintext RB members");
        const auto values = Parse(arguments, options);
        if (values.count("help"))
        {
            std::cout << options << '\n';
            return 0;
        }
        if (decodeType > 1)
            throw std::runtime_error("--decode-type must be 0 or 1");
        bmt::RBLoadOptions load;
        load.jobs = jobs;
        if (!defaults.empty())
            load.rbhotDefaultsPlist = defaults;
        const auto outputType =
            plain ? std::optional<uint8_t>{}
                  : std::optional<uint8_t>{static_cast<uint8_t>(decodeType)};
        if (command == "decrypt")
            bmt::DecryptRB(input, output, load);
        else if (command == "encrypt")
        {
            if (plain)
                bmt::DecryptRB(input, output, load);
            else
                bmt::EncryptRB(input, output, static_cast<uint8_t>(decodeType), load);
        }
        else if (command == "unpack")
            bmt::UnpackRB(input, output, load);
        else if (command == "pack")
            bmt::PackRB(input, output, outputType);
        else if (command == "unpack-dir")
            bmt::UnpackRBDirectory(input, output, load);
        else if (command == "pack-dir")
            bmt::PackRBDirectory(input, output, outputType, jobs);
        else
            throw std::runtime_error("unknown rb command " + std::string(command));
        return 0;
    }

    int RunRBBuild(const std::vector<std::string>& arguments)
    {
        fs::path official;
        fs::path rbhot;
        fs::path defaults;
        fs::path output;
        std::vector<fs::path> custom;
        bool eager = false;
        bool strict = false;
        bool encryptRB = true;
        bool separate = false;
        unsigned int jobs = 0;
        std::string mulistKey;
        std::string outputKey = "preserve";
        po::options_description options("rb build options");
        options.add_options()
            ("help,h", "show help")
            ("official", po::value<fs::path>(&official), "Official RB DLC directory")
            ("rbhot", po::value<fs::path>(&rbhot), "RBHot DLC directory")
            ("rbhot-plist", po::value<fs::path>(&defaults), "encrypted RBHot defaults plist")
            ("custom-dir", po::value<std::vector<fs::path>>(&custom)->composing(),
             "Custom RB DLC directory; repeatable")
            ("output,o", po::value<fs::path>(&output)->required(), "output directory")
            ("eager", po::bool_switch(&eager), "materialize every resource while loading")
            ("strict", po::bool_switch(&strict), "stop at the first invalid pack or relation")
            ("jobs,j", po::value<unsigned int>(&jobs)->default_value(0),
             "parallel file jobs; 0 uses available CPU cores")
            ("encrypt-rb", po::value<bool>(&encryptRB)->default_value(true),
             "encrypt output RB members")
            ("output-key", po::value<std::string>(&outputKey)->default_value("preserve"),
             "output DecodeType: preserve, 0, or 1")
            ("separate-output", po::bool_switch(&separate),
             "write RBs in official/rbhot/custom-N subdirectories")
            ("mulist-key", po::value<std::string>(&mulistKey),
             "raw ApplicationUniqueID before MD5; read/write runtime mulist and nolist");
        const auto values = Parse(arguments, options);
        if (values.count("help"))
        {
            std::cout << options << '\n';
            return 0;
        }
        std::vector<bmt::RBSource> sources;
        if (!official.empty())
            sources.push_back({bmt::DLCType::Official, official});
        if (!rbhot.empty())
            sources.push_back({bmt::DLCType::JBHot, rbhot});
        for (const auto& path : custom)
            sources.push_back({bmt::DLCType::Custom, path});
        if (sources.empty())
            throw std::runtime_error("rb build requires --official, --rbhot, or --custom-dir");
        if (!rbhot.empty() && defaults.empty())
            throw std::runtime_error("--rbhot requires --rbhot-plist");

        bmt::RBOutputKey parsedOutputKey;
        if (outputKey == "preserve")
            parsedOutputKey = bmt::RBOutputKey::Preserve;
        else if (outputKey == "0")
            parsedOutputKey = bmt::RBOutputKey::Type0;
        else if (outputKey == "1")
            parsedOutputKey = bmt::RBOutputKey::Type1;
        else
            throw std::runtime_error("--output-key must be preserve, 0, or 1");

        bmt::RBLoadOptions load;
        load.mode = eager ? bmt::LoadMode::Eager : bmt::LoadMode::Lazy;
        load.failureMode = strict ? bmt::FailureMode::Strict : bmt::FailureMode::Continue;
        load.jobs = jobs;
        if (!defaults.empty())
            load.rbhotDefaultsPlist = defaults;
        if (!mulistKey.empty())
            load.mulistKey = mulistKey;
        auto result = bmt::LoadRBPacks(sources, load);
        bmt::RBExportOptions exportOptions;
        exportOptions.encryptRB = encryptRB;
        exportOptions.outputKey = parsedOutputKey;
        exportOptions.separateByDLC = separate;
        exportOptions.jobs = jobs;
        if (!mulistKey.empty())
            exportOptions.mulistKey = mulistKey;
        bmt::ExportRBPacks(result, output, exportOptions);
        for (const auto& warning : result.warnings)
            std::cerr << "warning: " << warning.path << ": " << warning.message << '\n';
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << "error: " << diagnostic.path << ": " << diagnostic.message << '\n';
        std::cout << "exported " << result.packs.size() << " RB music IDs; dropped "
                  << result.droppedDuplicates << " duplicates; remapped "
                  << result.remaps.size() << " packs\n";
        return result.diagnostics.empty() ? 0 : 2;
    }

    int RunRBHot(std::string_view command, const std::vector<std::string>& arguments)
    {
        if (command != "defaults-dump")
            throw std::runtime_error("unknown rbhot command " + std::string(command));
        fs::path input;
        fs::path output;
        po::options_description options("rbhot defaults-dump options");
        options.add_options()
            ("help,h", "show help")
            ("input,i", po::value<fs::path>(&input)->required(), "encrypted RBHot defaults plist")
            ("output-dir,o", po::value<fs::path>(&output)->required(), "JSON output directory");
        const auto values = Parse(arguments, options);
        if (values.count("help"))
        {
            std::cout << options << '\n';
            return 0;
        }
        for (const auto& [name, json] : bmt::DumpRBHotDefaults(input))
            WriteText(output / (name + ".json"), json + "\n");
        return 0;
    }

    int RunNolist(std::string_view command, const std::vector<std::string>& arguments)
    {
        fs::path input;
        fs::path output;
        std::string key;
        po::options_description options("nolist " + std::string(command) + " options");
        options.add_options()
            ("help,h", "show help")
            ("input,i", po::value<fs::path>(&input)->required(), "input nolist file")
            ("output,o", po::value<fs::path>(&output)->required(), "output nolist file")
            ("key,k", po::value<std::string>(&key)->required(), "raw key before MD5 derivation");
        const auto values = Parse(arguments, options);
        if (values.count("help"))
        {
            std::cout << options << '\n';
            return 0;
        }
        if (command == "decrypt")
            WriteFile(output, bmt::DecryptRBList(input, key));
        else if (command == "encrypt")
            WriteFile(output, bmt::EncryptRBList(input, key));
        else
            throw std::runtime_error("unknown nolist command " + std::string(command));
        return 0;
    }

    int RunMarkerConversion(std::string_view command,
                            const std::vector<std::string>& arguments)
    {
        fs::path input;
        fs::path output;
        po::options_description options("marker " + std::string(command) + " options");
        options.add_options()
            ("help,h", "show help")
            ("input,i", po::value<fs::path>(&input)->required(), "input ZIP or directory")
            ("output,o", po::value<fs::path>(&output)->required(), "output ZIP or directory");
        const auto values = Parse(arguments, options);
        if (values.count("help"))
        {
            std::cout << options << '\n';
            return 0;
        }
        if (command == "decrypt")
            bmt::DecryptMarker(input, output);
        else if (command == "encrypt")
            bmt::EncryptMarker(input, output);
        else if (command == "unpack")
            bmt::UnpackMarker(input, output);
        else if (command == "pack")
            bmt::PackMarker(input, output);
        else if (command == "unpack-dir")
            bmt::UnpackMarkerDirectory(input, output);
        else if (command == "pack-dir")
            bmt::PackMarkerDirectory(input, output);
        else
            throw std::runtime_error("unknown marker command " + std::string(command));
        return 0;
    }

    int RunMarkerBuild(const std::vector<std::string>& arguments)
    {
        fs::path official;
        fs::path jbhot;
        fs::path output;
        fs::path markerListOutput;
        std::vector<fs::path> custom;
        bool strict = false;
        po::options_description options("marker build options");
        options.add_options()
            ("help,h", "show help")
            ("official", po::value<fs::path>(&official), "Official marker directory")
            ("jbhot", po::value<fs::path>(&jbhot), "JBHot marker directory")
            ("custom-dir", po::value<std::vector<fs::path>>(&custom)->composing(), "Custom marker directory; repeatable")
            ("output,o", po::value<fs::path>(&output)->required(), "output marker directory")
            ("strict", po::bool_switch(&strict), "stop at the first invalid marker")
            ("marker-list-output", po::value<fs::path>(&markerListOutput),
             "plain XML plist output for MarkerManager setMarkerList:");
        const auto values = Parse(arguments, options);
        if (values.count("help"))
        {
            std::cout << options << '\n';
            return 0;
        }
        std::vector<bmt::MarkerSource> sources;
        if (!official.empty())
            sources.push_back({bmt::DLCType::Official, official});
        if (!jbhot.empty())
            sources.push_back({bmt::DLCType::JBHot, jbhot});
        for (const auto& path : custom)
            sources.push_back({bmt::DLCType::Custom, path});
        if (sources.empty())
            throw std::runtime_error("marker build requires --official, --jbhot, or --custom-dir");
        auto result = bmt::LoadMarkers(sources,
            {.failureMode = strict ? bmt::FailureMode::Strict : bmt::FailureMode::Continue});
        bmt::MarkerExportOptions exportOptions;
        if (!markerListOutput.empty())
            exportOptions.markerListOutput = markerListOutput;
        bmt::ExportMarkers(result, output, exportOptions);
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << "error: " << diagnostic.path << ": " << diagnostic.message << '\n';
        std::cout << "exported " << result.packs.size() << " markers; dropped "
                  << result.droppedDuplicates << " duplicates; remapped "
                  << result.remaps.size() << " markers\n";
        return result.diagnostics.empty() ? 0 : 2;
    }

    int RunMarkerList(std::string_view command,
                      const std::vector<std::string>& arguments)
    {
        if (command != "decrypt")
            throw std::runtime_error("unknown marker-list command " + std::string(command));
        fs::path input;
        fs::path output;
        po::options_description options("marker-list " + std::string(command) + " options");
        options.add_options()
            ("help,h", "show help")
            ("input,i", po::value<fs::path>(&input)->required(), "input file")
            ("output,o", po::value<fs::path>(&output)->required(), "output XML plist");
        const auto values = Parse(arguments, options);
        if (values.count("help"))
        {
            std::cout << options << '\n';
            return 0;
        }
        WriteFile(output, bmt::BuildMarkerListXML(bmt::DecryptMarkerList(input)));
        return 0;
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc < 3)
        {
            RootUsage();
            return argc == 2 && std::string_view(argv[1]) == "--help" ? 0 : 1;
        }
        const std::string_view group = argv[1];
        const std::string_view command = argv[2];
        const auto arguments = Arguments(argc, argv, 3);
        if (group == "bfcodec")
            return RunBFCodec(command, arguments);
        if (group == "dlc")
            return RunDLC(command, arguments);
        if (group == "mulist")
            return RunMulist(command, arguments);
        if (group == "jbt")
            return RunJBT(command, arguments);
        if (group == "jbhot")
            return RunJBHot(command, arguments);
        if (group == "rb")
            return command == "build" ? RunRBBuild(arguments)
                                      : RunRBConversion(command, arguments);
        if (group == "rbhot")
            return RunRBHot(command, arguments);
        if (group == "nolist")
            return RunNolist(command, arguments);
        if (group == "marker")
            return command == "build" ? RunMarkerBuild(arguments)
                                      : RunMarkerConversion(command, arguments);
        if (group == "marker-list")
            return RunMarkerList(command, arguments);
        throw std::runtime_error("unknown command group " + std::string(group));
    }
    catch (const po::error& error)
    {
        std::cerr << "argument error: " << error.what() << '\n';
        return 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
