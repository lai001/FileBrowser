#include "Shutdown.h"

#include <chrono>
#include <csignal>

#include "spdlog/spdlog.h"
#include "Misc.h"

#if _WIN32
#include <windows.h>
#endif

namespace
{
std::atomic<bool> shutdownFlag{false};

// Longest a single wait in the pipe reader may last, so the flags are re-checked often enough:
// a blocking ConnectNamedPipe/ReadFile could not be interrupted from the main thread.
constexpr unsigned int kWaitStepMilliseconds = 200;

#if _WIN32
std::string windowsError(const char *action)
{
    return std::string(action) + ", Windows error " + std::to_string(GetLastError());
}

BOOL WINAPI consoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT)
    {
        // Only the flag is set here; runServer() polls it and calls grpc::Server::Shutdown().
        // Returning TRUE stops the default handler, which would end the process without letting
        // the RPCs in flight finish. CTRL_CLOSE_EVENT is left to the default handler.
        shutdownFlag.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
extern "C" void signalHandler(int signalNumber)
{
    (void)signalNumber;
    // A lock-free std::atomic<bool> is the one thing a handler is allowed to touch.
    shutdownFlag.store(true);
}
#endif
} // namespace

bool shutdownRequested()
{
    return shutdownFlag.load();
}

void requestShutdown()
{
    shutdownFlag.store(true);
}

std::string installShutdownHandlers()
{
#if _WIN32
    if (!SetConsoleCtrlHandler(consoleHandler, TRUE))
    {
        return windowsError("SetConsoleCtrlHandler failed");
    }
    return std::string();
#else
    if (std::signal(SIGINT, signalHandler) == SIG_ERR)
    {
        return "signal(SIGINT) failed";
    }
    if (std::signal(SIGTERM, signalHandler) == SIG_ERR)
    {
        return "signal(SIGTERM) failed";
    }
    return std::string();
#endif
}

ShutdownPipeListener::~ShutdownPipeListener()
{
    stop();
}

std::string ShutdownPipeListener::start(int port)
{
    pipeName = misc::shutdownPipeName(port);
#if _WIN32
    // PIPE_ACCESS_INBOUND: the other side writes, this process reads. One instance, message mode,
    // overlapped so the waits below are bounded.
    instance = CreateNamedPipeW(misc::wstringRemoveNullTerminator(misc::utf8ToWstring(pipeName)).c_str(),
                                PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 256, 0, 0, nullptr);
    if (instance == INVALID_HANDLE_VALUE)
    {
        return "Shutdown pipe " + pipeName + ": " + windowsError("CreateNamedPipe failed");
    }

    runningFlag.store(true);
    worker = std::thread([this] { loop(); });
    return std::string();
#else
    return "The shutdown pipe is only implemented on Windows; signals are the only trigger here";
#endif
}

void ShutdownPipeListener::loop()
{
#if _WIN32
    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr)
    {
        spdlog::warn("Shutdown pipe: {}", windowsError("CreateEvent failed"));
        return;
    }

    while (runningFlag.load() && !shutdownRequested())
    {
        ResetEvent(overlapped.hEvent);
        if (!ConnectNamedPipe(instance, &overlapped))
        {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED)
            {
                // A client connected while this thread was between two waits.
            }
            else if (error == ERROR_IO_PENDING)
            {
                if (WaitForSingleObject(overlapped.hEvent, kWaitStepMilliseconds) == WAIT_TIMEOUT)
                {
                    continue;
                }
            }
            else
            {
                spdlog::warn("Shutdown pipe: {}", windowsError("ConnectNamedPipe failed"));
                break;
            }
        }

        if (!runningFlag.load() || shutdownRequested())
        {
            break;
        }

        char buffer[256] = {0};
        DWORD read = 0;
        ResetEvent(overlapped.hEvent);
        if (!ReadFile(instance, buffer, static_cast<DWORD>(sizeof(buffer)), &read, &overlapped))
        {
            if (GetLastError() != ERROR_IO_PENDING)
            {
                spdlog::warn("Shutdown pipe: {}", windowsError("ReadFile failed"));
                DisconnectNamedPipe(instance);
                continue;
            }
            if (WaitForSingleObject(overlapped.hEvent, kWaitStepMilliseconds) == WAIT_TIMEOUT)
            {
                CancelIoEx(instance, &overlapped);
                continue;
            }
        }

        if (read == 0)
        {
            DisconnectNamedPipe(instance);
            continue;
        }

        const std::string message(buffer, read);
        spdlog::trace("Shutdown pipe: got \"{}\"", message);
        if (message == misc::kShutdownCommand)
        {
            requestShutdown();
        }
        DisconnectNamedPipe(instance);
    }

    CloseHandle(overlapped.hEvent);
#endif
}

void ShutdownPipeListener::stop()
{
    runningFlag.store(false);
    if (worker.joinable())
    {
        worker.join();
    }
#if _WIN32
    if (instance != INVALID_HANDLE_VALUE)
    {
        CloseHandle(instance);
        instance = INVALID_HANDLE_VALUE;
    }
#endif
}

bool ShutdownPipeListener::running() const
{
    return runningFlag.load();
}
