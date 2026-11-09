#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "CLI/CLI.hpp"
#include "spdlog/spdlog.h"
#include "Misc.h"
#include "Server.h"
#include "Shutdown.h"

int runServer(const std::string &address, int port, int shutdownTimeoutSeconds)
{
    spdlog::set_level(spdlog::level::trace);
    // std::setlocale(LC_ALL, "en_US.UTF-8");
    const std::string server_address = address + ":" + std::to_string(port);

    const std::string handlerError = installShutdownHandlers();
    if (!handlerError.empty())
    {
        spdlog::warn("{}", handlerError);
    }

    ShutdownPipeListener pipeListener;
    const std::string pipeError = pipeListener.start(port);
    if (!pipeError.empty())
    {
        spdlog::warn("{}; a control event is the only trigger for this instance", pipeError);
    }

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
        pipeListener.stop();
        spdlog::error("Failed to start the server on {}", server_address);
        return 1;
    }
    spdlog::trace("Server listening on {}, shutdown pipe {}", server_address, misc::shutdownPipeName(port));

    // grpc::Server::Shutdown() cannot be called from a handler, so the request is polled here.
    while (!shutdownRequested())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    spdlog::trace("Shutdown requested, waiting up to {} second(s) for the RPCs in flight", shutdownTimeoutSeconds);
    if (shutdownTimeoutSeconds > 0)
    {
        // Shutdown(0) would wait for the RPCs in flight without a limit, so a stuck stream would
        // keep the process alive. The deadline has to be a gpr_timespec or a
        // std::chrono::system_clock::time_point: TimePoint<T> is deleted for other types, so a
        // plain time_t does not compile.
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(shutdownTimeoutSeconds));
    }
    else
    {
        server->Shutdown();
    }

    pipeListener.stop();
    spdlog::trace("Server stopped");
    return 0;
}

int main(int argc, char **argv)
{
    CLI::App app{"FileBrowser gRPC server"};
    argv = app.ensure_utf8(argv);

    std::string address = "localhost";
    // int, not uint16_t: CLI11 runs the check on the raw string with the type the Range literals deduce (int
    // here) and converts the value into the variable afterwards, so a narrower type is checked the same way;
    // int keeps the variable and the check in the same type, which is what --help prints.
    int port = 7700;
    int shutdownTimeout = 5;
    app.add_option("--address", address, "Address to listen on (default: loopback only)");
    app.add_option("-p,--port", port, "Port to listen on (default: 7700)")->check(CLI::Range(1, 65535));
    app.add_option("--shutdown-timeout", shutdownTimeout,
                   "Seconds Shutdown() waits for the RPCs in flight, 0 waits forever (default: 5)")
        ->check(CLI::Range(0, 3600));

    CLI11_PARSE(app, argc, argv);

    return runServer(address, port, shutdownTimeout);
}
