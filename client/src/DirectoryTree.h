#ifndef CLIENT_SRC_DIRECTORY_TREE_H
#define CLIENT_SRC_DIRECTORY_TREE_H

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include "spdlog/spdlog.h"
#include "Misc.h"

class DirectoryEntry
{
  public:
    std::filesystem::path filePath;
    std::unordered_set<std::filesystem::path> childs;
    bool isDirectory;
    uint64_t fileSize;

    std::filesystem::path getParent() const;

    DirectoryEntry();
};

using PathMap = std::unordered_map<std::filesystem::path, DirectoryEntry>;

class DirectoryTree
{
  public:
    PathMap directories;

    DirectoryTree(PathMap directories);

    DirectoryTree();

    // void insertEntry(DirectoryEntry entry);

    bool rebuildHierarchy();

    bool recalculateDirectorySize();

    bool rebuild();

    uint64_t getTotalFileSize();

    const std::filesystem::path &getRootPath() const;

  private:
    std::unordered_set<std::filesystem::path> findRootPaths();

    bool calculateDirectorySizeRecursively(const std::filesystem::path &filePath);

    void calculateDirectorySize(const std::filesystem::path &filePath);

    void findDuplicatePaths(const std::vector<std::filesystem::path> &paths);

    void fillMissingDirectories();

    std::filesystem::path rootPath;
};

#endif