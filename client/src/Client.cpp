#include "Client.h"

uint64_t FileBrowserClient::getFileSize(const std::string &filePath)
{
    pb::FileSizeRequest request;
    request.set_filepath(filePath);
    pb::FileSizeReply reply;
    grpc::ClientContext context;
    grpc::Status status = stub_->getFileSize(&context, request, &reply);
    if (status.ok())
    {
        return reply.filesize();
    }
    else
    {
        spdlog::warn("{}: {}", static_cast<int>(status.error_code()), status.error_message());
        return 0;
    }
}

std::vector<pb::FileEntry> FileBrowserClient::getFiles(const std::string &directory)
{
    pb::FilesRequest request;
    request.set_directory(directory);
    pb::FilesReply reply;
    grpc::ClientContext context;
    grpc::Status status = stub_->getFiles(&context, request, &reply);
    if (status.ok())
    {
        std::vector<pb::FileEntry> fileEntries;
        for (const pb::FileEntry &item : reply.fileentrires())
        {
            fileEntries.push_back(item);
        }
        return fileEntries;
    }
    else
    {
        spdlog::warn("{}: {}", static_cast<int>(status.error_code()), status.error_message());
        return std::vector<pb::FileEntry>();
    }
}

std::unordered_map<std::filesystem::path, DirectoryEntry> FileBrowserClient::recursiveDirectoryTraversal(
    const std::string &filePath)
{
    pb::FilesRequest request;
    request.set_directory(filePath);
    grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReader<pb::FileEntry>> reader(stub_->recursiveDirectoryTraversal(&context, request));
    pb::FileEntry fileEntry;
    std::unordered_map<std::filesystem::path, DirectoryEntry> directories;
    while (reader->Read(&fileEntry))
    {
        DirectoryEntry directoryEntry;
        directoryEntry.fileSize = fileEntry.filesize();
        directoryEntry.filePath = fileEntry.filepath();
        directoryEntry.isDirectory = fileEntry.isdirectory();
        directories[directoryEntry.filePath] = directoryEntry;
    }
    grpc::Status status = reader->Finish();
    if (!status.ok())
    {
        spdlog::error("Recursive directory traversal: {}, error: {}", filePath, status.error_message());
    }
    return directories;
}