#ifndef SRC_GREETER_CLIENT_H
#define SRC_GREETER_CLIENT_H

#include <functional>
#include <memory>
#include <vector>
#include "spdlog/spdlog.h"
#include "grpcpp/grpcpp.h"
#include "FileBrowser.pb.h"
#include "FileBrowser.grpc.pb.h"
#include "DirectoryTree.h"

// The traversal reports one entry at a time, so it hands them to a sink instead of filling a map: the queue
// counts what arrives while the RPC is running, and the sink returning false is how the caller stops a stream
// that was cancelled.
using EntrySink = std::function<bool(const ScanEntry &)>;

// The same call as the queue runs it. The context is passed in because the queue is what cancels it, and a test
// can supply a source that yields fixed entries without a server.
using ScanSource =
    std::function<grpc::Status(grpc::ClientContext &context, const std::string &directory, const EntrySink &sink)>;

// The download streams hand each message to a sink; returning false is how the caller stops a stream that was
// cancelled, the same contract EntrySink has.
using DownloadItemSink = std::function<bool(const pb::DownloadItem &)>;
using DownloadChunkSink = std::function<bool(const pb::DownloadChunk &)>;

// Upload goes the other way: the client pushes, so the writer asks for the next message instead of being handed
// one. Producing one chunk at a time is what keeps a large file from being read into memory; returning false
// ends the stream.
using UploadItemProducer = std::function<bool(pb::UploadItem &)>;

// Called after the server took a message. The producer prepares a message before it is written, and what the state
// has to describe is what reached the server, so the count for a message is committed here: a message the stream
// refused never got there and is not recorded.
using WriteConfirmed = std::function<void()>;

class FileBrowserClient
{
  public:
    FileBrowserClient(std::shared_ptr<grpc::Channel> channel) : stub_(pb::FileBrowser::NewStub(channel))
    {
    }

    uint64_t getFileSize(const std::string &filePath);

    // Returns the status like the other calls the UI caches, so a failed traversal can be told apart from
    // an empty directory. Paths are UTF-8 and are converted to std::filesystem::path explicitly (see the cpp
    // file), which is what makes the tree the client builds usable as a lookup table.
    grpc::Status recursiveDirectoryTraversal(grpc::ClientContext &context, const std::string &directory,
                                             const EntrySink &sink);

    // These three return the status instead of logging and returning an empty value like
    // getFileSize: the UI caches what it reads per server, so it has to tell a failed call apart
    // from a legitimately empty result, and that is also what decides whether a server shows its
    // roots or its cached listing.
    grpc::Status getFiles(const std::string &directory, std::vector<pb::FileEntry> &entries);

    grpc::Status getRootDirectories(std::vector<std::string> &roots);

    grpc::Status getSystemInfo(pb::SystemInfoReply &systemInfo);

    // The three transfer calls. They keep no deadline: a transfer of a large folder can run for minutes, and
    // they are not called on the render thread (see client/src/TransferQueue.cpp), so the deadline the two info
    // calls have would only cut them off.
    grpc::Status downloadFile(grpc::ClientContext &context, const pb::DownloadRequest &request,
                              const DownloadChunkSink &sink);

    grpc::Status downloadDirectory(grpc::ClientContext &context, const pb::DownloadDirectoryRequest &request,
                                   const DownloadItemSink &sink);

    // The reply is filled by Finish(), so it only carries the counts for a stream that completed. onWritten runs
    // after each message the write accepted, which is when the producer records what the server has.
    grpc::Status uploadFiles(grpc::ClientContext &context, const UploadItemProducer &nextItem,
                             const WriteConfirmed &onWritten, pb::UploadReply &reply);

  private:
    std::unique_ptr<pb::FileBrowser::Stub> stub_;
};

#endif