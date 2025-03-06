#include <fstream>
#include <filesystem>
#include "CLI/CLI.hpp"
#include "spdlog/spdlog.h"
#include "FileBrowser.grpc.pb.h"
#include "FileBrowser.pb.h"

int main(int argc, char **argv)
{
    spdlog::set_level(spdlog::level::trace);

    CLI::App app{"CollectFiles"};
    argv = app.ensure_utf8(argv);
    std::string scanPath = "";
    std::string outputPath = "";
    int maxDepth = INT32_MAX;
    app.add_option("-i,--input-location", scanPath, "Input location")->check(CLI::ExistingDirectory)->required(true);
    app.add_option("-o,--output-path", outputPath, "Output path")->required(true);
    app.add_option("-d,--depth", maxDepth, "Depth")->required(false);
    CLI11_PARSE(app, argc, argv);
    spdlog::trace("Scan path: {}", scanPath);
    pb::FileEntryCollection fileEntryCollection;
    fileEntryCollection.set_rootpath(scanPath);
    try
    {
        auto rootFileEntry = fileEntryCollection.add_fileentrires();
        std::filesystem::directory_entry rootEntry(scanPath);
        if (!rootEntry.is_directory())
        {
            rootFileEntry->set_filesize(rootEntry.file_size());
        }
        else
        {
            rootFileEntry->set_filesize(0);
        }
        rootFileEntry->set_filepath(rootEntry.path().u8string());
        rootFileEntry->set_isdirectory(rootEntry.is_directory());
        auto iter = std::filesystem::recursive_directory_iterator(
            scanPath, std::filesystem::directory_options::skip_permission_denied);
        for (const auto &entry : iter)
        {
            if (iter.depth() > maxDepth)
            {
                break;
            }
            auto fileEntry = fileEntryCollection.add_fileentrires();
            if (!entry.is_directory())
            {
                fileEntry->set_filesize(entry.file_size());
            }
            else
            {
                fileEntry->set_filesize(0);
            }
            fileEntry->set_filepath(entry.path().u8string());
            fileEntry->set_isdirectory(entry.is_directory());
        }
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        spdlog::error("{}", e.what());
        return 1;
    }

    std::string serializedData;
    if (!fileEntryCollection.SerializeToString(&serializedData))
    {
        spdlog::error("Failed to serialize message.");
        return 1;
    }

    std::ofstream output_file(outputPath, std::ios::binary);
    if (!output_file)
    {
        spdlog::error("Failed to open file for writing.");
        return 1;
    }
    output_file.write(serializedData.data(), serializedData.size());
    output_file.close();
    return 0;
}