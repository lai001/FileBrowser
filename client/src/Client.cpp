#include "Client.h"
#include <chrono>
#include "Misc.h"

// The two info calls the UI makes on the render thread (getRootDirectories and getSystemInfo), so an address
// that does not answer must not stall the window. The deadline only applies to these two cheap calls: a
// directory listing can legitimately take longer, so getFiles keeps none, and the traversal and the three
// transfer calls are no longer called on the render thread at all (see client/src/ScanQueue.cpp and
// client/src/TransferQueue.cpp). getFileSize is not called by the UI; it is kept as a single-file call.
namespace
{
constexpr std::chrono::seconds kInfoCallTimeout(5);
} // namespace

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

grpc::Status FileBrowserClient::getFiles(const std::string &directory, std::vector<pb::FileEntry> &entries)
{
    pb::FilesRequest request;
    request.set_directory(directory);
    pb::FilesReply reply;
    grpc::ClientContext context;
    grpc::Status status = stub_->getFiles(&context, request, &reply);
    if (!status.ok())
    {
        spdlog::warn("getFiles {}: error: {}", directory, status.error_message());
        return status;
    }
    for (const pb::FileEntry &item : reply.fileentrires())
    {
        entries.push_back(item);
    }
    return status;
}

grpc::Status FileBrowserClient::recursiveDirectoryTraversal(grpc::ClientContext &context, const std::string &directory,
                                                            const EntrySink &sink)
{
    pb::FilesRequest request;
    request.set_directory(directory);
    std::unique_ptr<grpc::ClientReader<pb::TraversalBatch>> reader(
        stub_->recursiveDirectoryTraversal(&context, request));

    // The stream carries each entry's own name plus where its parent was sent, so the path is rebuilt here.
    // parentIndex is the position of the entry in the stream, and -1 is the directory the traversal was
    // started on, which the queue already made a node of. pathByIndex is what lets an entry look up the path
    // of an earlier one, and it has to stay in step with the stream order.
    const std::filesystem::path scanRoot = misc::utf8ToPath(directory);
    std::vector<std::filesystem::path> pathByIndex;

    bool stopped = false;
    pb::TraversalBatch batch;
    while (!stopped && reader->Read(&batch))
    {
        for (const pb::TraversalEntry &wireEntry : batch.entries())
        {
            const int32_t parentIndex = wireEntry.parentindex();
            if (parentIndex >= 0 && static_cast<std::size_t>(parentIndex) >= pathByIndex.size())
            {
                // The server emits an entry only while listing its directory, so the parent is always
                // something already read. A larger index here means the stream is not what this client expects.
                // The slot is still filled, with an empty path, so that the entries after it keep counting
                // against the same positions; only this one entry is dropped.
                spdlog::warn("Recursive directory traversal: {} points at parent index {}", wireEntry.name(),
                             parentIndex);
                pathByIndex.push_back(std::filesystem::path{});
                continue;
            }

            const std::filesystem::path parent =
                parentIndex < 0 ? scanRoot : pathByIndex[static_cast<std::size_t>(parentIndex)];

            ScanEntry entry;
            entry.fileSize = wireEntry.filesize();
            entry.allocatedSize = wireEntry.allocatedsize();
            entry.fileIndex = wireEntry.fileindex();
            // The names come back as UTF-8. Assigning a narrow string to std::filesystem::path goes through the
            // current code page, so a name that is not representable in it produces a different path and the tree
            // lookups stop matching. The same explicit conversion the server uses on the way in.
            entry.filePath = parent / misc::utf8ToPath(wireEntry.name());
            entry.isDirectory = wireEntry.isdirectory();

            pathByIndex.push_back(entry.filePath);
            if (!sink(entry))
            {
                // The caller wants the stream to stop. This cannot break a Read() that is already waiting - that
                // needs TryCancel() on the context - so the loop ends between entries, and Finish() reports CANCELLED
                // for a call that was cancelled while it was still streaming.
                stopped = true;
                break;
            }
        }
    }
    grpc::Status status = reader->Finish();
    if (!status.ok())
    {
        spdlog::error("Recursive directory traversal: {}, error: {}", directory, status.error_message());
    }
    return status;
}

grpc::Status FileBrowserClient::getRootDirectories(std::vector<std::string> &roots)
{
    pb::RootDirectoriesRequest request;
    pb::RootDirectoriesReply reply;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + kInfoCallTimeout);
    grpc::Status status = stub_->getRootDirectories(&context, request, &reply);
    if (!status.ok())
    {
        spdlog::warn("Root directories: error: {}", status.error_message());
        return status;
    }
    for (const std::string &root : reply.roots())
    {
        roots.push_back(root);
    }
    return status;
}

grpc::Status FileBrowserClient::downloadFile(grpc::ClientContext &context, const pb::DownloadRequest &request,
                                             const DownloadChunkSink &sink)
{
    std::unique_ptr<grpc::ClientReader<pb::DownloadChunk>> reader(stub_->downloadFile(&context, request));

    pb::DownloadChunk chunk;
    bool stopped = false;
    while (!stopped && reader->Read(&chunk))
    {
        // The same contract as the traversal: false is how the caller says "stop". A Read() that is already
        // waiting cannot be broken from here - that needs TryCancel() on the context, which the transfer worker
        // does - so the loop ends between messages and Finish() reports CANCELLED.
        if (!sink(chunk))
        {
            stopped = true;
            break;
        }
    }

    grpc::Status status = reader->Finish();
    if (!status.ok())
    {
        spdlog::error("Download file: {}, error: {}", request.filepath(), status.error_message());
    }
    return status;
}

grpc::Status FileBrowserClient::downloadDirectory(grpc::ClientContext &context,
                                                  const pb::DownloadDirectoryRequest &request,
                                                  const DownloadItemSink &sink)
{
    std::unique_ptr<grpc::ClientReader<pb::DownloadItem>> reader(stub_->downloadDirectory(&context, request));

    pb::DownloadItem item;
    bool stopped = false;
    while (!stopped && reader->Read(&item))
    {
        if (!sink(item))
        {
            stopped = true;
            break;
        }
    }

    grpc::Status status = reader->Finish();
    if (!status.ok())
    {
        spdlog::error("Download directory: {}, error: {}", request.directory(), status.error_message());
    }
    return status;
}

grpc::Status FileBrowserClient::uploadFiles(grpc::ClientContext &context, const UploadItemProducer &nextItem,
                                            const WriteConfirmed &onWritten, pb::UploadReply &reply)
{
    std::unique_ptr<grpc::ClientWriter<pb::UploadItem>> writer(stub_->uploadFiles(&context, &reply));

    // The producer is asked for one message at a time, so a large file never exists in memory as a whole.
    bool producedEverything = true;
    pb::UploadItem item;
    while (nextItem(item))
    {
        if (!writer->Write(item))
        {
            // false here means the server stopped taking messages - the call was cancelled or the stream already
            // ended. Finish() is what gives the reason, so the reply is not read as if the upload completed.
            producedEverything = false;
            break;
        }
        // What the state records for a message is committed only now: a message the stream refused is not something
        // the server has.
        onWritten();
    }

    // Only a stream that was written to the end gets closed by the client; after a failed Write the call is
    // already over and Finish() reports it.
    if (producedEverything && !writer->WritesDone())
    {
        producedEverything = false;
    }

    grpc::Status status = writer->Finish();
    if (!status.ok())
    {
        spdlog::error("Upload files: error: {} ({} files, {} bytes written before it ended)", status.error_message(),
                      reply.fileswritten(), reply.byteswritten());
    }
    return status;
}

grpc::Status FileBrowserClient::getSystemInfo(pb::SystemInfoReply &systemInfo)
{
    pb::SystemInfoRequest request;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + kInfoCallTimeout);
    grpc::Status status = stub_->getSystemInfo(&context, request, &systemInfo);
    if (!status.ok())
    {
        spdlog::warn("System info: error: {}", status.error_message());
    }
    return status;
}