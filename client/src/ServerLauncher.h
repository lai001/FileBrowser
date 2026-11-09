#ifndef CLIENT_SRC_SERVER_LAUNCHER_H
#define CLIENT_SRC_SERVER_LAUNCHER_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
// Keeps windows.h from pulling in winsock.h, which clashes with winsock2.h (included by
// client/bin/main.cpp before this header).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if _WIN32
#include <windows.h>
#endif

#include <string>

// Runs FileBrowserServer.exe as a child process for the Start/Stop buttons of the client UI.
// Windows only, which is also the only platform the client is built on (root CMakeLists.txt).
class ServerLauncher
{
  public:
    ServerLauncher() = default;

    ~ServerLauncher();

    ServerLauncher(const ServerLauncher &) = delete;
    ServerLauncher &operator=(const ServerLauncher &) = delete;

    // FileBrowserServer.exe next to the client executable. The install rules put both
    // executables in the same bin directory, which is why this default works after an install.
    // Static: it only reads the path of the running executable, and the client now keeps one
    // launcher per server entry, so there is no single instance to ask.
    static std::string defaultServerExecutable();

    // Returns an empty string on success, otherwise the error message to show in the UI.
    // The child is started with "--address <address> --port <port>".
    std::string start(const std::string &executable, const std::string &address, int port);

    // Returns the message to show in the UI. The child is first asked to stop itself: the
    // kShutdownCommand message is written to misc::shutdownPipeName(startedPort), which makes the
    // server call grpc::Server::Shutdown() and let the RPCs in flight finish. When the pipe cannot
    // be reached (server build without the pipe, instance not created yet, non-Windows) the
    // process is terminated instead, because its console is not one this process can send a
    // control event to.
    // waitMilliseconds is for callers that want the outcome here (the destructor); the UI passes 0
    // and reads isRunning() on the next frame, so the window does not freeze while the server
    // finishes its RPCs.
    std::string stop(int waitMilliseconds = 0);

    bool isRunning() const;

    // -1 while the process is running or when it was never started.
    int exitCode() const;

    // 0 when no process was ever started.
    int processId() const;

  private:
#if _WIN32
    HANDLE processHandle = nullptr;

    // Writes one message to the shutdown pipe of the instance started on this port.
    bool sendShutdownCommand() const;
#endif
    int pid = 0;
    int startedPort = 0;
    mutable int lastExitCode = -1;
};

#endif
