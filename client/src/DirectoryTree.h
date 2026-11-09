#ifndef CLIENT_SRC_DIRECTORY_TREE_H
#define CLIENT_SRC_DIRECTORY_TREE_H

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include "spdlog/spdlog.h"
#include "Misc.h"

// What one entry from the RPC carries. It is the input; the node it becomes is TreeEntry below.
struct ScanEntry
{
    std::filesystem::path filePath;
    bool isDirectory = false;
    uint64_t fileSize = 0;
    uint64_t allocatedSize = 0;
    uint64_t fileIndex = 0;
};

// One node of the tree. Only its own name is stored: the full path is rebuilt from the parent chain, so a scan
// of a hundred thousand entries keeps one short name per node instead of a full path per node, and the children
// of a directory are indices instead of a set of paths. A file has an empty children vector, which allocates
// nothing. The full paths live in the map below, because the UI looks nodes up by path.
struct TreeEntry
{
    std::filesystem::path name;
    int32_t parentIndex = -1;
    std::vector<int32_t> children;

    bool isDirectory = false;
    // For a file these are its own values from the RPC. For a directory they are the sums of the entries below
    // it, because a directory has no size of its own: the server reports 0 for one.
    uint64_t fileSize = 0;
    uint64_t allocatedSize = 0;
    // The reference the file system gave for the file. Two paths with the same index are the same file, so a
    // size is counted once for an index. It is 0 when the enumeration could not give one (info class 2, most
    // network shares), and an entry with no index is never treated as a duplicate.
    uint64_t fileIndex = 0;
    bool duplicate = false;
};

using PathMap = std::unordered_map<std::filesystem::path, int32_t>;

class DirectoryTree
{
  public:
    std::vector<TreeEntry> entries;

    // scanRoot is the directory the traversal was started on. The traversal does not report the directory it was
    // started on, only what is inside it, so the scanned directory becomes a node here, and the tree stops there
    // instead of growing up to the root of the drive. That is what makes getTotalFileSize the size of the
    // directory that was scanned.
    explicit DirectoryTree(std::filesystem::path scanRoot = std::filesystem::path());

    // Adds one node, links it to its parent (creating the parent when the traversal did not report it), and adds
    // its size to every ancestor. The totals are therefore complete while a scan is still running, without a
    // second pass over the tree. Returns the index of the node.
    int32_t insertEntry(const ScanEntry &entry);

    int32_t indexOf(const std::filesystem::path &path) const;
    const TreeEntry &entryAt(int32_t index) const;
    std::filesystem::path pathOf(int32_t index) const;
    uint64_t sizeAt(const std::filesystem::path &path) const;

    // Re-links everything from the paths that were inserted and recounts. insertEntry already did both, so this
    // is only for a tree that was filled in one go without it, such as the Load file button.
    bool rebuild();

    uint64_t getTotalFileSize() const;
    uint64_t getTotalAllocatedSize() const;

    size_t getEntryCount() const;
    size_t getFileCount() const;
    size_t getDirectoryCount() const;

    int32_t getRootIndex() const;
    const std::filesystem::path &getScanRoot() const;
    std::filesystem::path getRootPath() const;

  private:
    PathMap indexByPath;
    std::unordered_set<uint64_t> countedFileIndices;

    std::filesystem::path scanRoot;

    // The paths above scanRoot. They are not part of the scan, so insertEntry must not add them.
    std::unordered_set<std::filesystem::path> scanRootAncestors;

    int32_t rootIndex = -1;

    int32_t insertNode(const std::filesystem::path &fullPath, bool isDirectory, uint64_t fileSize,
                       uint64_t allocatedSize, uint64_t fileIndex);

    // Adds one entry's own sizes to every node above it, stopping at the top of the tree.
    void addSizeToAncestors(int32_t index);

    bool canCreateAncestor(const std::filesystem::path &parent) const;

    int32_t findRootIndex();
};

#endif
