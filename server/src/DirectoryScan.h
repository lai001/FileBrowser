#ifndef SERVER_SRC_DIRECTORY_SCAN_H
#define SERVER_SRC_DIRECTORY_SCAN_H

#include <cstdint>
#include <functional>
#include <system_error>
#include <filesystem>

// One entry as the file system reported it. Everything in here comes from the same record that listed the
// entry, which is the point of the module: reading a directory is one call per directory instead of three
// calls per entry (the three the previous fillFileEntry made: is_directory, file_size, last_write_time).
struct ScannedEntry
{
    std::filesystem::path path;
    bool isDirectory = false;
    uint64_t fileSize = 0;       // apparent size; 0 for directories
    uint64_t allocatedSize = 0;  // bytes actually occupied on disk; 0 when the platform cannot tell
    uint64_t fileIndex = 0;      // NTFS record number or POSIX inode; 0 when it cannot be read
    int64_t lastWriteTime = 0;   // epoch seconds
    bool isReparsePoint = false; // junction, symlink, mount point: reported, but not descended into
};

// Returning false stops the walk. This is how a cancelled stream or a failed write ends a scan: the scanner
// reports no error for it because the reason belongs to the caller, not to the file system.
using ScannedEntryVisitor = std::function<bool(const ScannedEntry &)>;

// One level of one directory.
std::error_code scanDirectory(const std::filesystem::path &directory, const ScannedEntryVisitor &visitor);

// Depth-first and iterative, so a deep tree cannot overflow the stack. Reparse points are reported but not
// followed, which is also what keeps a junction cycle from looping forever;
// std::filesystem::recursive_directory_iterator behaves the same way by default (follow_directory_symlink
// is off), so this does not change what the client used to get.
std::error_code scanDirectoryTree(const std::filesystem::path &root, const ScannedEntryVisitor &visitor);

// One path per call. On Windows this is the only way to get the file reference and the allocated size outside a
// batched listing, and it costs an open plus up to three queries; on POSIX a single lstat gives all six fields.
std::error_code scanEntry(const std::filesystem::path &path, ScannedEntry &entry);

#endif
