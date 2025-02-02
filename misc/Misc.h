#ifndef MISC_MISC_H
#define MISC_MISC_H

#include <string>
#include <unordered_set>
#include <filesystem>

namespace misc
{
std::string formatSize(const uint64_t size);

std::unordered_set<std::filesystem::path> getParentDirectories(const std::filesystem::path &path);
} // namespace misc

#endif
