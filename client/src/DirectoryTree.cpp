#include "DirectoryTree.h"
#include <assert.h>
#include <vector>

DirectoryTree::DirectoryTree(std::filesystem::path scanRoot) : scanRoot(scanRoot)
{
    if (!scanRoot.empty())
    {
        scanRootAncestors = misc::getParentDirectories(scanRoot);
        // An empty scanned directory produces no entries at all, so the root node has to be created here.
        rootIndex = insertNode(scanRoot, true, 0, 0, 0);
    }
}

int32_t DirectoryTree::insertNode(const std::filesystem::path &fullPath, bool isDirectory, uint64_t fileSize,
                                  uint64_t allocatedSize, uint64_t fileIndex)
{
    const auto existing = indexByPath.find(fullPath);
    if (existing != indexByPath.end())
    {
        return existing->second;
    }

    TreeEntry node;
    // A node stores only its own name. A root is the exception: "D:\" and "/" have no filename element, and the
    // scanned directory has no parent stored in the tree, so those two keep their whole path as the name. That
    // is what makes pathOf able to rebuild a path from the chain.
    node.name = fullPath.filename();
    if (node.name.empty() || fullPath == scanRoot)
    {
        node.name = fullPath;
    }
    node.isDirectory = isDirectory;
    node.fileSize = fileSize;
    node.allocatedSize = allocatedSize;
    node.fileIndex = fileIndex;

    const int32_t index = static_cast<int32_t>(entries.size());
    entries.push_back(node);
    indexByPath[fullPath] = index;

    if (rootIndex == -1 && fullPath.root_path() == fullPath)
    {
        rootIndex = index;
    }
    return index;
}

int32_t DirectoryTree::insertEntry(const ScanEntry &entry)
{
    if (entry.filePath.empty())
    {
        spdlog::warn("Insert: an entry without a path");
        return -1;
    }

    const auto existing = indexByPath.find(entry.filePath);
    if (existing != indexByPath.end())
    {
        spdlog::warn("Insert: {} is already in the tree", misc::pathToUtf8(entry.filePath));
        return existing->second;
    }

    const int32_t index =
        insertNode(entry.filePath, entry.isDirectory, entry.fileSize, entry.allocatedSize, entry.fileIndex);

    const std::filesystem::path parentPath = entry.filePath.parent_path();
    if (parentPath == entry.filePath)
    {
        return index;
    }

    auto parent = indexByPath.find(parentPath);
    if (parent == indexByPath.end())
    {
        if (!canCreateAncestor(parentPath))
        {
            // The paths above the scanned directory are not part of the scan. Adding them would make the tree
            // root the root of the drive, so the total would be whatever the traversal happened to return
            // instead of the size of the directory that was scanned.
            spdlog::warn("Insert: {} is above the scanned directory", misc::pathToUtf8(entry.filePath));
            return index;
        }

        // A traversal reports the contents of a directory but not the directory itself, so the parent can be a
        // node that has not been seen yet. Collect the chain and add it from the top down so each node knows
        // its parent.
        std::vector<std::filesystem::path> missing;
        for (std::filesystem::path current = parentPath; indexByPath.find(current) == indexByPath.end();
             current = current.parent_path())
        {
            if (!canCreateAncestor(current))
            {
                missing.clear();
                break;
            }
            missing.push_back(current);
            if (current.parent_path() == current)
            {
                break;
            }
        }
        // Create the chain from the top down so that each new node can be linked to the one above it. Creating
        // them without linking is what left a node whose parent chain stops in the middle.
        for (auto it = missing.rbegin(); it != missing.rend(); ++it)
        {
            const int32_t created = insertNode(*it, true, 0, 0, 0);
            const auto above = indexByPath.find(it->parent_path());
            if (above != indexByPath.end() && above->second != created)
            {
                entries[created].parentIndex = above->second;
                entries[above->second].children.push_back(created);
            }
        }
        parent = indexByPath.find(parentPath);
    }

    if (parent == indexByPath.end())
    {
        return index;
    }

    entries[index].parentIndex = parent->second;
    entries[parent->second].children.push_back(index);

    if (!entry.isDirectory)
    {
        if (entry.fileIndex != 0 && !countedFileIndices.insert(entry.fileIndex).second)
        {
            // The same file seen from another path. Its row still shows its size, but the totals above it are
            // not charged twice.
            entries[index].duplicate = true;
        }
        else
        {
            addSizeToAncestors(index);
        }
    }

    return index;
}

void DirectoryTree::addSizeToAncestors(int32_t index)
{
    // A parent index always points at a node created before the child, so the chain cannot loop; the step limit
    // is what keeps a tree corrupted some other way from running away.
    for (int32_t current = entries[index].parentIndex, steps = 0; current >= 0; current = entries[current].parentIndex)
    {
        entries[current].fileSize += entries[index].fileSize;
        entries[current].allocatedSize += entries[index].allocatedSize;
        if (static_cast<size_t>(steps) > entries.size())
        {
            spdlog::error("Add size to ancestors: a parent chain that is longer than the tree");
            break;
        }
        ++steps;
    }
}

bool DirectoryTree::canCreateAncestor(const std::filesystem::path &parent) const
{
    return scanRoot.empty() || scanRootAncestors.find(parent) == scanRootAncestors.end();
}

bool DirectoryTree::rebuild()
{
    for (auto &node : entries)
    {
        node.parentIndex = -1;
        node.children.clear();
        node.duplicate = false;
        if (node.isDirectory)
        {
            node.fileSize = 0;
            node.allocatedSize = 0;
        }
    }
    countedFileIndices.clear();

    // Link first. Accumulating in the same loop would walk up a chain whose upper part is not linked yet, so the
    // sums above it would be missed - the order of the map is not the order of the tree.
    std::size_t roots = 0;
    for (const auto &item : indexByPath)
    {
        const std::filesystem::path parentPath = item.first.parent_path();
        if (parentPath == item.first || item.first == scanRoot)
        {
            ++roots;
            continue;
        }
        const auto parent = indexByPath.find(parentPath);
        if (parent == indexByPath.end())
        {
            spdlog::warn("Rebuild: {} has no parent in the tree", misc::pathToUtf8(item.first));
            continue;
        }
        entries[item.second].parentIndex = parent->second;
        entries[parent->second].children.push_back(item.second);
    }

    for (const auto &item : indexByPath)
    {
        const TreeEntry &node = entries[item.second];
        if (node.isDirectory || node.parentIndex < 0)
        {
            continue;
        }
        if (node.fileIndex != 0 && !countedFileIndices.insert(node.fileIndex).second)
        {
            entries[item.second].duplicate = true;
        }
        else
        {
            addSizeToAncestors(item.second);
        }
    }

    rootIndex = -1;
    if (!scanRoot.empty())
    {
        const auto it = indexByPath.find(scanRoot);
        if (it != indexByPath.end())
        {
            rootIndex = it->second;
        }
    }
    if (rootIndex == -1)
    {
        for (const auto &item : indexByPath)
        {
            if (entries[item.second].parentIndex < 0)
            {
                rootIndex = item.second;
                break;
            }
        }
    }
    if (roots > 1)
    {
        spdlog::warn("Rebuild: {} entries have no parent in the tree", roots);
    }
    return true;
}

int32_t DirectoryTree::indexOf(const std::filesystem::path &path) const
{
    const auto it = indexByPath.find(path);
    return it != indexByPath.end() ? it->second : -1;
}

const TreeEntry &DirectoryTree::entryAt(int32_t index) const
{
    static const TreeEntry empty;
    if (index < 0 || static_cast<size_t>(index) >= entries.size())
    {
        return empty;
    }
    return entries[static_cast<size_t>(index)];
}

std::filesystem::path DirectoryTree::pathOf(int32_t index) const
{
    if (index < 0 || static_cast<size_t>(index) >= entries.size())
    {
        return std::filesystem::path();
    }

    // The entry only stores its own name, so the path is assembled from the chain. The top of the chain is the
    // root, which already carries its separator ("D:\" or "/"), so joining below it gives an absolute path.
    std::vector<std::filesystem::path> names;
    for (int32_t current = index; current >= 0 && names.size() <= entries.size();
         current = entries[current].parentIndex)
    {
        names.push_back(entries[static_cast<size_t>(current)].name);
    }

    std::filesystem::path path = names.back();
    for (auto it = names.rbegin() + 1; it != names.rend(); ++it)
    {
        path /= *it;
    }
    return path;
}

uint64_t DirectoryTree::sizeAt(const std::filesystem::path &path) const
{
    const int32_t index = indexOf(path);
    return index < 0 ? 0 : entries[static_cast<size_t>(index)].fileSize;
}

uint64_t DirectoryTree::getTotalFileSize() const
{
    if (rootIndex < 0)
    {
        return 0;
    }
    return entries[static_cast<size_t>(rootIndex)].fileSize;
}

uint64_t DirectoryTree::getTotalAllocatedSize() const
{
    if (rootIndex < 0)
    {
        return 0;
    }
    return entries[static_cast<size_t>(rootIndex)].allocatedSize;
}

size_t DirectoryTree::getEntryCount() const
{
    return entries.size();
}

size_t DirectoryTree::getFileCount() const
{
    size_t count = 0;
    for (const TreeEntry &node : entries)
    {
        if (!node.isDirectory)
        {
            ++count;
        }
    }
    return count;
}

size_t DirectoryTree::getDirectoryCount() const
{
    size_t count = 0;
    for (const TreeEntry &node : entries)
    {
        if (node.isDirectory)
        {
            ++count;
        }
    }
    return count;
}

int32_t DirectoryTree::getRootIndex() const
{
    return rootIndex;
}

const std::filesystem::path &DirectoryTree::getScanRoot() const
{
    return scanRoot;
}

std::filesystem::path DirectoryTree::getRootPath() const
{
    return pathOf(rootIndex);
}
