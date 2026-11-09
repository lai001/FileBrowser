#include "ServerLauncher.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "spdlog/spdlog.h"
#include "Misc.h"

#if _WIN32
#include <windows.h>
#endif

namespace
{
#if _WIN32
std::string windowsError(const char *action)
{
    return std::string(action) + ", Windows error " + std::to_string(GetLastError());
}

// client/<config> and server/<config> share the build root, so walking up from the directory the
// client executable lives in reaches the root where the server target was written.
std::vector<std::filesystem::path> buildTreeRoots(const std::filesystem::path &clientDirectory)
{
    std::vector<std::filesystem::path> roots;
    std::filesystem::path directory = clientDirectory;
    for (int level = 0; level < 3 && !directory.empty(); ++level)
    {
        roots.push_back(directory);
        directory = directory.parent_path();
    }
    return roots;
}
#endif
} // namespace

ServerLauncher::~ServerLauncher()
{
    if (isRunning())
    {
        // There is no UI left at this point to show what happened, so wait a little for the
        // graceful path and let stop() fall back to TerminateProcess.
        stop(2000);
    }
}

std::string ServerLauncher::defaultServerExecutable()
{
#if _WIN32
    wchar_t buffer[MAX_PATH] = {0};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0)
    {
        return "FileBrowserServer.exe";
    }
    const std::filesystem::path clientExecutable(buffer);
    const std::filesystem::path clientDirectory = clientExecutable.parent_path();

    // A deployed client and server sit in the same directory. Running the client straight out of the
    // build tree does not, because the Visual Studio generator puts each target in its own folder
    // (client/Debug and server/Debug), so those are checked as fallbacks. The first path that exists
    // is returned; when none does, the same-directory path is returned so that start() can report a
    // meaningful path in its error message.
    const std::filesystem::path deployed = clientDirectory / "FileBrowserServer.exe";
    for (const std::filesystem::path &directory : buildTreeRoots(clientDirectory))
    {
        for (const char *config : {"Debug", "Release"})
        {
            const std::filesystem::path candidate = directory / "server" / config / "FileBrowserServer.exe";
            if (std::filesystem::exists(candidate))
            {
                return candidate.string();
            }
        }
    }
    return deployed.string();
#else
    return "FileBrowserServer.exe";
#endif
}

std::string ServerLauncher::start(const std::string &executable, const std::string &address, int port)
{
    if (isRunning())
    {
        return "The server process is already running";
    }

#if _WIN32
    const std::filesystem::path exePath(executable);
    if (exePath.empty())
    {
        return "Server executable path is empty";
    }
    if (!std::filesystem::exists(exePath))
    {
        return "Server executable does not exist: " + exePath.string();
    }

    // CreateProcessW wants a writable command line, and the executable has to be quoted for a
    // path containing spaces. misc::utf8ToWstring converts the null terminator as well, hence
    // wstringRemoveNullTerminator.
    const std::wstring commandLine =
        L'"' + misc::wstringRemoveNullTerminator(misc::utf8ToWstring(executable)) + L"\" --address " +
        misc::wstringRemoveNullTerminator(misc::utf8ToWstring(address)) + L" --port " + std::to_wstring(port);

    STARTUPINFOW startupInfo = {0};
    startupInfo.cb = sizeof(STARTUPINFOW);
    PROCESS_INFORMATION processInfo = {0};

    // CREATE_NEW_CONSOLE: the client is a GUI process without a console, and that new console is
    // where the server's spdlog output goes.
    const BOOL result = CreateProcessW(exePath.c_str(), const_cast<wchar_t *>(commandLine.c_str()), nullptr, nullptr,
                                       FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &startupInfo, &processInfo);
    if (!result)
    {
        return windowsError("CreateProcess failed");
    }

    CloseHandle(processInfo.hThread);
    processHandle = processInfo.hProcess;
    pid = static_cast<int>(processInfo.dwProcessId);
    // The shutdown pipe name depends on the port this instance was started with.
    startedPort = port;
    lastExitCode = -1;
    spdlog::trace("Started the server process, pid {}, command line: {}", pid, misc::wStringToString(commandLine));
    return std::string();
#else
    return "Starting a server process is only implemented on Windows";
#endif
}

#if _WIN32
bool ServerLauncher::sendShutdownCommand() const
{
    const std::string pipeName = misc::shutdownPipeName(startedPort);
    // The server instance is inbound-only, so this handle is write-only.
    const HANDLE pipe = CreateFileW(misc::wstringRemoveNullTerminator(misc::utf8ToWstring(pipeName)).c_str(),
                                    GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
    {
        spdlog::warn("Shutdown pipe {}: {}", pipeName, windowsError("CreateFile failed"));
        return false;
    }

    DWORD written = 0;
    const bool result = WriteFile(pipe, misc::kShutdownCommand, static_cast<DWORD>(std::strlen(misc::kShutdownCommand)),
                                  &written, nullptr);
    CloseHandle(pipe);
    if (!result || written == 0)
    {
        spdlog::warn("Shutdown pipe {}: {}", pipeName, windowsError("WriteFile failed"));
        return false;
    }
    spdlog::trace("Shutdown pipe {}: wrote \"{}\"", pipeName, misc::kShutdownCommand);
    return true;
}
#endif

std::string ServerLauncher::stop(int waitMilliseconds)
{
#if _WIN32
    if (processHandle == nullptr)
    {
        return "The server process is not running";
    }

    const bool commandSent = sendShutdownCommand();
    if (commandSent && waitMilliseconds > 0)
    {
        // The server's own default is a 5 s Shutdown() deadline, so a short wait here only catches
        // the normal case; the UI passes 0 and checks isRunning() on the next frame instead.
        for (int attempt = 0; attempt * 100 < waitMilliseconds && isRunning(); ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool terminated = false;
    if (isRunning())
    {
        if (!TerminateProcess(processHandle, 0))
        {
            return windowsError("TerminateProcess failed");
        }
        // The process is gone; record the code we terminated it with so the UI status line does
        // not keep showing the last STILL_ACTIVE reading.
        lastExitCode = 0;
        terminated = true;
    }

    const int code = lastExitCode;
    CloseHandle(processHandle);
    processHandle = nullptr;
    pid = 0;
    spdlog::trace("Server process stopped, exit code {}", code);

    if (terminated)
    {
        if (commandSent)
        {
            return "Stopped by terminating the process, the shutdown command did not finish in time";
        }
        return "Stopped by terminating the process, the shutdown pipe was not reachable";
    }
    if (!commandSent)
    {
        return "The process had already exited";
    }
    return "Stopped by the shutdown command";
#else
    return "Stopping a server process is only implemented on Windows";
#endif
}

bool ServerLauncher::isRunning() const
{
#if _WIN32
    if (processHandle == nullptr)
    {
        return false;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(processHandle, &code))
    {
        return false;
    }
    // STILL_ACTIVE (259) is also a legal process exit code, so a process that really exited with
    // 259 is reported as still running; that is good enough for a UI status line.
    lastExitCode = static_cast<int>(code);
    return code == STILL_ACTIVE;
#else
    return false;
#endif
}

int ServerLauncher::exitCode() const
{
    return lastExitCode;
}

int ServerLauncher::processId() const
{
    return pid;
}
