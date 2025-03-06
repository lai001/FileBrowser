#include "Misc.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include <locale>
#include <codecvt>
#include "spdlog/spdlog.h"
#include "spdlog/fmt/fmt.h"

#ifdef __ANDROID__
#ifdef __clang__
namespace std
{
template <> struct hash<std::filesystem::path>
{
    std::size_t operator()(const std::filesystem::path &p) const noexcept
    {
        return std::hash<std::string>()(p.string());
    }
};
} // namespace std
#endif
#endif

// struct PathHash
// {
//     std::size_t operator()(const std::filesystem::path &p) const noexcept
//     {
//         return std::hash<std::string>()(p.string());
//     }
// };

namespace misc
{

std::string formatSize(const uint64_t size)
{
    if (size >= 0 && size < 1024)
    {
        return fmt::format("{} bytes", size);
    }
    else if (size >= 1024 && size < static_cast<uint64_t>(1024) * 1024)
    {
        return fmt::format("{:.3f} kb", static_cast<double>(size) / static_cast<double>(1024));
    }
    else if (size >= static_cast<uint64_t>(1024) * 1024 && size < static_cast<uint64_t>(1024) * 1024 * 1024)
    {
        return fmt::format("{:.3f} mb", static_cast<double>(size) / static_cast<double>((1024 * 1024)));
    }
    return fmt::format("{:.3f} gb", static_cast<double>(size) / static_cast<double>((1024 * 1024 * 1024)));
}

bool misc::endsWithZero(const std::string &str)
{
    if (str.size() == 0)
    {
        return false;
    }
    const char *cstr = str.c_str();
    if (cstr[str.size()] == '\0')
    {
        return true;
    }
    return false;
}

std::string stringRemoveNullTerminator(const std::string &str)
{
    std::string s = str;
    if (!s.empty() && s.back() == '\0')
    {
        s.pop_back();
    }
    return s;
}

std::wstring misc::wstringRemoveNullTerminator(const std::wstring &str)
{
    std::wstring s = str;
    if (!s.empty() && s.back() == '\0')
    {
        s.pop_back();
    }
    return s;
}

PathSet getParentDirectories(const std::filesystem::path &path)
{
    PathSet paths;
    std::filesystem::path current = path;
    std::filesystem::path parent = path.parent_path();
    while (current != parent)
    {
        paths.insert(parent);
        current = parent;
        parent = current.parent_path();
    }
    return paths;
}

std::wstring utf8ToWstring(const std::string &utf8_str)
{
#ifdef _WIN32
    if (utf8_str.empty())
        return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, wstr.data(), len);
    return wstr;
#else
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.from_bytes(utf8_str);
#endif
}

std::string utf8ToGbk(const std::string &utf8_str)
{
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, wstr.data(), len);
    len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string gbk_str(len, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, gbk_str.data(), len, nullptr, nullptr);
    return gbk_str;
#else
    return utf8_str;
#endif
}

bool startsWith(const std::string &str, const std::string &prefix)
{
    return str.compare(0, prefix.length(), prefix) == 0;
}

bool endsWith(const std::string &str, const std::string &suffix)
{
    return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string removePrefix(const std::string &str, const std::string &prefix)
{
    return startsWith(str, prefix) ? str.substr(prefix.length()) : str;
}

std::string removeSuffix(const std::string &str, const std::string &suffix)
{
    return endsWith(str, suffix) ? str.substr(0, str.size() - suffix.size()) : str;
}

std::filesystem::path misc::normalizePath(const std::filesystem::path &messyPath)
{
    return messyPath;
    // return messyPath.lexically_normal();
    std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(messyPath);
    return canonicalPath.make_preferred();
}

std::string wStringToString(const std::wstring &wstr)
{
#ifdef _WIN32
    const int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wstr.size(), NULL, 0, NULL, NULL);
    std::string str(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wstr.size(), str.data(), len, NULL, NULL);
    return str;
#else
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    std::string str = converter.to_bytes(wstr);
    return str;
#endif
}

} // namespace misc
