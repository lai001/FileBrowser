#ifndef SERVER_SRC_SERVER_H
#define SERVER_SRC_SERVER_H

#include <iostream>
#include <memory>
#include <string>
#include "grpcpp/ext/proto_server_reflection_plugin.h"
#include "grpcpp/grpcpp.h"
#include "grpcpp/health_check_service_interface.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"
#include "spdlog/spdlog.h"
#include "FileBrowser.grpc.pb.h"

class FileBrowserServiceImpl final : public pb::FileBrowser::Service
{
    grpc::Status getFileSize(grpc::ServerContext *context, const pb::FileSizeRequest *request,
                             pb::FileSizeReply *response) override;

    grpc::Status getFiles(grpc::ServerContext *context, const pb::FilesRequest *request,
                          pb::FilesReply *response) override;

    grpc::Status recursiveDirectoryTraversal(grpc::ServerContext *context, const pb::FilesRequest *request,
                                             grpc::ServerWriter<pb::FileEntry> *writer) override;
};

#endif