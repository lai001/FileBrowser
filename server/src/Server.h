#ifndef SERVER_SRC_SERVER_H
#define SERVER_SRC_SERVER_H

#include <iostream>
#include <memory>
#include <string>
#include "grpcpp/ext/proto_server_reflection_plugin.h"
#include "grpcpp/grpcpp.h"
#include "grpcpp/health_check_service_interface.h"
#include "spdlog/spdlog.h"
#include "FileBrowser.grpc.pb.h"

class FileBrowserServiceImpl final : public pb::FileBrowser::Service
{
    grpc::Status getFileSize(grpc::ServerContext *context, const pb::FileSizeRequest *request,
                             pb::FileSizeReply *response) override;

    grpc::Status getFiles(grpc::ServerContext *context, const pb::FilesRequest *request,
                          pb::FilesReply *response) override;

    grpc::Status getRootDirectories(grpc::ServerContext *context, const pb::RootDirectoriesRequest *request,
                                    pb::RootDirectoriesReply *response) override;

    grpc::Status getSystemInfo(grpc::ServerContext *context, const pb::SystemInfoRequest *request,
                               pb::SystemInfoReply *response) override;

    grpc::Status getFileInfo(grpc::ServerContext *context, const pb::FileInfoRequest *request,
                             pb::FileEntry *response) override;

    grpc::Status downloadFile(grpc::ServerContext *context, const pb::DownloadRequest *request,
                              grpc::ServerWriter<pb::DownloadChunk> *writer) override;

    grpc::Status downloadDirectory(grpc::ServerContext *context, const pb::DownloadDirectoryRequest *request,
                                   grpc::ServerWriter<pb::DownloadItem> *writer) override;

    grpc::Status uploadFiles(grpc::ServerContext *context, grpc::ServerReader<pb::UploadItem> *reader,
                             pb::UploadReply *response) override;

    grpc::Status recursiveDirectoryTraversal(grpc::ServerContext *context, const pb::FilesRequest *request,
                                             grpc::ServerWriter<pb::TraversalBatch> *writer) override;
};

#endif