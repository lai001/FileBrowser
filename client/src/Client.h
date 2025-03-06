#ifndef SRC_GREETER_CLIENT_H
#define SRC_GREETER_CLIENT_H

#include <memory>
#include <vector>
#include "spdlog/spdlog.h"
#include "grpcpp/grpcpp.h"
#include "FileBrowser.pb.h"
#include "FileBrowser.grpc.pb.h"
#include "DirectoryTree.h"

class FileBrowserClient
{
  public:
    FileBrowserClient(std::shared_ptr<grpc::Channel> channel) : stub_(pb::FileBrowser::NewStub(channel))
    {
    }

    uint64_t getFileSize(const std::string &filePath);

    std::vector<pb::FileEntry> getFiles(const std::string &directory);

    PathMap recursiveDirectoryTraversal(const std::string &filePath);

  private:
    std::unique_ptr<pb::FileBrowser::Stub> stub_;
};

#endif