#ifndef SERVER_SRC_SHUTDOWN_H
#define SERVER_SRC_SHUTDOWN_H

#include <atomic>
#include <string>
#include <thread>

#if _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// grpc::Server::Shutdown() has to be called from outside the RPC threads, so the request comes
// from a console control event (Ctrl+C / Ctrl+Break), a SIGINT/SIGTERM, or a SHUTDOWN message
// written by an external tool on the named pipe named by misc::shutdownPipeName(port).
// Nothing here calls Shutdown() directly: the handlers only set the flag and the caller polls it,
// because a signal handler must stay async-signal-safe.
bool shutdownRequested();

void requestShutdown();

// Installs the console control handler (Windows) or the SIGINT/SIGTERM handlers (POSIX).
// Returns an empty string on success, otherwise the error message.
std::string installShutdownHandlers();

// Background reader for the shutdown pipe. Windows only; on POSIX start() fails and the caller
// relies on the signal handlers.
class ShutdownPipeListener
{
  public:
    ShutdownPipeListener() = default;

    ~ShutdownPipeListener();

    ShutdownPipeListener(const ShutdownPipeListener &) = delete;
    ShutdownPipeListener &operator=(const ShutdownPipeListener &) = delete;

    // Returns an empty string on success, otherwise the error message.
    std::string start(int port);

    // Lets the reader thread leave its bounded wait and join.
    void stop();

    bool running() const;

  private:
    void loop();

    std::string pipeName;
#if _WIN32
    HANDLE instance = INVALID_HANDLE_VALUE;
#endif
    std::thread worker;
    std::atomic<bool> runningFlag{false};
};

#endif
