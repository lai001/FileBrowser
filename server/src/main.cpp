#include "spdlog/spdlog.h"
#include "Server.h"

int runServer(uint16_t port)
{
    spdlog::set_level(spdlog::level::trace);
    std::setlocale(LC_ALL, "en_US.UTF-8");
    const std::string server_address = absl::StrFormat("localhost:%d", port);
    FileBrowserServiceImpl service;
    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    auto cq = builder.AddCompletionQueue();
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server)
    {
        return 1;
    }
    spdlog::trace("Server listening on {}", server_address);
    server->Wait();
    // pb::FileBrowser::AsyncService asyncService;
    return 0;
}

int main(int argc, char **argv)
{
    const int code = runServer(7700);
    return code;
}