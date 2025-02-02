#include "Misc.h"
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
    else if (size >= static_cast<uint64_t>(1024) * 1024 * 1024 &&
             size < static_cast<uint64_t>(1024) * 1024 * 1024 * 1024)
    {
        return fmt::format("{:.3f} gb", static_cast<double>(size) / static_cast<double>((1024 * 1024 * 1024)));
    }
    return std::string();
}

std::unordered_set<std::filesystem::path> getParentDirectories(const std::filesystem::path &path)
{
    std::unordered_set<std::filesystem::path> paths;
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
} // namespace misc
