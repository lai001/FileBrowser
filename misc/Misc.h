#ifndef MISC_MISC_H
#define MISC_MISC_H

#include <string>
#include <unordered_set>
#include <filesystem>
#include "spdlog/spdlog.h"

using PathSet = std::unordered_set<std::filesystem::path>;

namespace misc
{
std::string formatSize(const uint64_t size);

// A duration for the progress lines: whole seconds under a minute, minutes with the seconds left under an hour,
// hours with the minutes left above that. A negative value is what an estimate that cannot be made yet looks like
// from the outside, and it reads as 0 s.
std::string formatDuration(const long long milliseconds);

PathSet getParentDirectories(const std::filesystem::path &path);

std::string wStringToString(const std::wstring &wstr);

std::wstring utf8ToWstring(const std::string &utf8_str);

std::string utf8ToGbk(const std::string &utf8_str);

bool startsWith(const std::string &str, const std::string &prefix);

bool endsWith(const std::string &str, const std::string &suffix);

bool endsWithZero(const std::string &str);

std::string stringRemoveNullTerminator(const std::string &str);

std::wstring wstringRemoveNullTerminator(const std::wstring &str);

// A UTF-8 path as std::filesystem::path. Assigning a narrow string to path goes through the current code page,
// so a name that is not representable in it produces a different path and the tree keys stop matching; the same
// conversion is used on both sides, the RPC and the UI lookups. It has to come after both declarations above,
// because utf8ToWstring keeps the terminator and this strips it.
inline std::filesystem::path utf8ToPath(const std::string &utf8_str)
{
    return wstringRemoveNullTerminator(utf8ToWstring(utf8_str));
}

// The other direction. A path stores wide characters on Windows and the RPC always sends UTF-8, so the same
// terminator both helpers produce has to be stripped here too. It lives next to utf8ToPath because server and
// client need the identical value for the same path.
inline std::string pathToUtf8(const std::filesystem::path &path)
{
#ifdef _WIN32
    return stringRemoveNullTerminator(wStringToString(path.native()));
#else
    return path.native();
#endif
}

std::string removePrefix(const std::string &str, const std::string &prefix);

std::string removeSuffix(const std::string &str, const std::string &suffix);

std::filesystem::path normalizePath(const std::filesystem::path &messyPath);

// Protocol between an external tool and a running server instance: writing kShutdownCommand to
// shutdownPipeName(port) makes the server call grpc::Server::Shutdown(). It lives here because
// both sides need the same value (server/src/Shutdown.cpp, client/src/ServerLauncher.cpp).
inline constexpr const char *kShutdownCommand = "SHUTDOWN";

std::string shutdownPipeName(int port);
} // namespace misc

#endif
