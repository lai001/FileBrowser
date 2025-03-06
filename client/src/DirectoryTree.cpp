#include "DirectoryTree.h"
#include <assert.h>
#include <stack>
#include "spdlog/spdlog.h"
#include "Misc.h"

DirectoryEntry::DirectoryEntry()
    : filePath(std::filesystem::path()), childs(std::unordered_set<std::filesystem::path>()), isDirectory(false),
      fileSize(0)
{
}

std::filesystem::path DirectoryEntry::getParent() const
{
    return filePath.parent_path();
}

DirectoryTree::DirectoryTree(PathMap directories) : rootPath(std::filesystem::path()), directories(directories)
{
    std::unordered_set<std::filesystem::path> rootPaths = findRootPaths();
    if (rootPaths.size() > 1)
    {
        spdlog::error("Find multiple root paths");
        for (const auto &item : rootPaths)
        {
            rootPath = item;
            spdlog::error("Find root path: {}", item.string());
        }
    }
    else
    {
        for (const auto &item : rootPaths)
        {
            rootPath = item;
            spdlog::trace("Find root path: {}", item.string());
        }
    }
    fillMissingDirectories();
}

DirectoryTree::DirectoryTree() : rootPath(std::filesystem::path()), directories(PathMap())
{
}

// void DirectoryTree::insertEntry(DirectoryEntry entry)
//{
//     const std::filesystem::path parentPath = entry.filePath.parent_path();
//     if (directories.find(parentPath) != directories.end())
//     {
//         directories[parentPath].childs.insert(entry.filePath);
//     }
//     directories[entry.filePath] = entry;
// }

bool DirectoryTree::rebuild()
{
    if (!rebuildHierarchy())
    {
        return false;
    }
    return recalculateDirectorySize();
}

uint64_t DirectoryTree::getTotalFileSize()
{
    if (directories.find(rootPath) != directories.end())
    {
        return directories[rootPath].fileSize;
    }
    return 0;
}

const std::filesystem::path &DirectoryTree::getRootPath() const
{
    return rootPath;
}

bool DirectoryTree::rebuildHierarchy()
{
    for (auto &pair : directories)
    {
        pair.second.childs.clear();
        if (pair.second.isDirectory)
        {
            pair.second.fileSize = 0;
        }
    }

    for (auto &pair : directories)
    {
        const std::filesystem::path key = pair.second.getParent();
        if (key == pair.second.filePath)
        {
            continue;
        }
        if (directories.find(key) != directories.end())
        {
            if (directories[key].isDirectory)
            {
                directories[key].childs.insert(pair.second.filePath);
            }
            else
            {
                spdlog::error("{} is not a directory.", key.string());
                return false;
            }
        }
    }
    return true;
}

bool DirectoryTree::recalculateDirectorySize()
{
    return calculateDirectorySizeRecursively(rootPath);
}

void DirectoryTree::calculateDirectorySize(const std::filesystem::path &filePath)
{
    if (directories.find(filePath) != directories.end())
    {
        if (directories[filePath].isDirectory)
        {
            uint64_t fileSize = 0;
            for (const std::filesystem::path &childPath : directories[filePath].childs)
            {
                if (directories.find(childPath) != directories.end())
                {
                    fileSize += directories[childPath].fileSize;
                }
                else
                {
                    spdlog::warn("Calculate directory size: {}", childPath.string());
                }
            }
            directories[filePath].fileSize = fileSize;
        }
    }
    else
    {
        spdlog::warn("Calculate directory size: {}", filePath.string());
    }
}

void DirectoryTree::findDuplicatePaths(const std::vector<std::filesystem::path> &paths)
{
    std::unordered_map<std::filesystem::path, int> pathCount;
    for (const auto &path : paths)
    {
        pathCount[path]++;
    }
    for (const auto &entry : pathCount)
    {
        if (entry.second > 1)
        {
            spdlog::trace("Find duplicate paths: {}, count: {}", entry.first.string(), entry.second);
        }
    }
}

bool DirectoryTree::calculateDirectorySizeRecursively(const std::filesystem::path &filePath)
{
    std::stack<std::filesystem::path> stack;
    std::stack<std::filesystem::path> output;
    stack.push(filePath);
    while (!stack.empty())
    {
        std::filesystem::path currentPath = stack.top();
        stack.pop();
        output.push(currentPath);
        if (output.size() > directories.size())
        {
            std::vector<std::filesystem::path> paths;
            paths.reserve(stack.size());
            while (!stack.empty())
            {
                std::filesystem::path p = stack.top();
                stack.pop();
                paths.push_back(p);
            }
            spdlog::error("Too many files. {} > {}", output.size(), directories.size());
            findDuplicatePaths(paths);
            return false;
        }
        if (directories.find(currentPath) != directories.end())
        {
            for (const std::filesystem::path &childPath : directories[currentPath].childs)
            {
                stack.push(childPath);
            }
        }
    }
    if (output.size() != directories.size())
    {
        spdlog::error("{} != {}", output.size(), directories.size());
        return false;
    }
    while (!output.empty())
    {
        const std::filesystem::path currentPath = output.top();
        output.pop();
        calculateDirectorySize(currentPath);
    }
    return true;
}

std::unordered_set<std::filesystem::path> DirectoryTree::findRootPaths()
{
    std::unordered_set<std::filesystem::path> rootPaths;
    for (const auto &pair : directories)
    {
        rootPaths.insert(pair.first.root_path());
    }
    return rootPaths;
}

void DirectoryTree::fillMissingDirectories()
{
    PathSet missingDirectories;
    for (const auto &pair : directories)
    {
        const PathSet parentDirectories = misc::getParentDirectories(pair.second.filePath);
        missingDirectories.insert(parentDirectories.begin(), parentDirectories.end());
    }
    for (const std::filesystem::path &dir : missingDirectories)
    {
        if (directories.find(dir) != directories.end())
        {
            missingDirectories.erase(dir);
        }
    }
    for (const std::filesystem::path &missingDirectory : missingDirectories)
    {
        DirectoryEntry entry;
        entry.filePath = missingDirectory;
        entry.isDirectory = true;
        entry.fileSize = 0;
        directories[entry.filePath] = entry;
    }
}
