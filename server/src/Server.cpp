#include "Server.h"
#include <filesystem>

grpc::Status FileBrowserServiceImpl::getFileSize(grpc::ServerContext *context, const pb::FileSizeRequest *request,
                                                 pb::FileSizeReply *response)
{
    const std::filesystem::path filePath = request->filepath();
    const uintmax_t fileSize = std::filesystem::file_size(filePath);
    response->set_filesize(static_cast<uint64_t>(fileSize));
    return grpc::Status::OK;
}

grpc::Status FileBrowserServiceImpl::getFiles(grpc::ServerContext *context, const pb::FilesRequest *request,
                                              pb::FilesReply *response)
{
    const std::filesystem::path filePath = request->directory();
    for (const std::filesystem::directory_entry &dirEntry : std::filesystem::directory_iterator(filePath))
    {
        pb::FileEntry fileEntry;
        fileEntry.set_filepath(dirEntry.path().string());
        fileEntry.set_isdirectory(dirEntry.is_directory());
        fileEntry.set_filesize(static_cast<uint64_t>(std::filesystem::file_size(dirEntry.path())));
        *response->add_fileentrires() = fileEntry;
    }
    return grpc::Status::OK;
}

grpc::Status FileBrowserServiceImpl::recursiveDirectoryTraversal(grpc::ServerContext *context,
                                                                 const pb::FilesRequest *request,
                                                                 grpc::ServerWriter<pb::FileEntry> *writer)
{
    const std::filesystem::path filePath = request->directory();
    std::filesystem::recursive_directory_iterator iter;
    try
    {
        iter = std::filesystem::recursive_directory_iterator(
            filePath, std::filesystem::directory_options::skip_permission_denied);
    }
    catch (const std::exception &e)
    {
        return grpc::Status(grpc::StatusCode::ABORTED, e.what());
    }

    for (const auto &entry : iter)
    {
        pb::FileEntry fileEntry;
        if (!entry.is_directory())
        {
            fileEntry.set_filesize(entry.file_size());
        }
        else
        {
            fileEntry.set_filesize(0);
        }
        fileEntry.set_filepath(entry.path().string());
        fileEntry.set_isdirectory(entry.is_directory());
        const bool writeResult = writer->Write(fileEntry);
        if (!writeResult)
        {
            spdlog::trace("Recursive directory traversal: the stream has been closed");
        }
    }
    return grpc::Status::OK;
}