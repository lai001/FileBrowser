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

PathSet getParentDirectories(const std::filesystem::path &path);

std::string wStringToString(const std::wstring &wstr);

std::wstring utf8ToWstring(const std::string &utf8_str);

std::string utf8ToGbk(const std::string &utf8_str);

bool startsWith(const std::string &str, const std::string &prefix);

bool endsWith(const std::string &str, const std::string &suffix);

bool endsWithZero(const std::string &str);

std::string stringRemoveNullTerminator(const std::string &str);

std::wstring wstringRemoveNullTerminator(const std::wstring &str);

std::string removePrefix(const std::string &str, const std::string &prefix);

std::string removeSuffix(const std::string &str, const std::string &suffix);

std::filesystem::path normalizePath(const std::filesystem::path &messyPath);
} // namespace misc

#endif
