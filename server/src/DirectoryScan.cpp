#include "DirectoryScan.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// The switch below uses ERROR_* as case constants. windows.h already provides them: winbase.h includes
// winerror.h. The explicit include only makes that dependency visible.
#include <winerror.h>
#include <winternl.h>
#else
#include <sys/stat.h>
#endif
#include "Misc.h"

namespace
{

// Defined below; the Windows fast path falls back to it when ntdll cannot be resolved.
std::error_code scanDirectorySimple(const std::filesystem::path &directory, const ScannedEntryVisitor &visitor);

#ifdef _WIN32

// FILE_INFORMATION_CLASS values. The public SDK only has FileDirectoryInformation = 1 in winternl.h; the full
// numbering and the record structures are in the WDK's ntioapi.h, which is not installed here, so the numbers are
// stated here. FileIdFullDirectoryInformation (38) is the class that carries the file reference;
// FileFullDirectoryInformation (2) is the fallback for the redirectors that do not implement it, which is most
// network shares. Both carry attributes, apparent size, allocated size and the timestamp, so one call per
// directory covers every field of ScannedEntry.
constexpr long kFileFullDirectoryInformation = 2;
constexpr long kFileIdFullDirectoryInformation = 38;

// The NTSTATUS values this code recognizes. ntdll is not in the import libraries this project links, so the codes
// are stated here instead of including a header for them. Four of them are exactly the values in ntstatus.h; the
// last two are codes accepted on top of that header and are not its values - ntstatus.h has STATUS_NO_SUCH_FILE
// as 0xC000000F and STATUS_NO_MORE_FILES as 0x80000006, while 0xC0000114 there is STATUS_ABIOS_INVALID_LID and
// 0xC0000121 is STATUS_CANNOT_DELETE.
constexpr long kStatusSuccess = 0;
constexpr long kStatusInvalidInfoClass = 0xC0000003; // ntstatus.h: STATUS_INVALID_INFO_CLASS
constexpr long kStatusNotSupported = 0xC00000BB;     // ntstatus.h: STATUS_NOT_SUPPORTED
constexpr long kStatusObjectNotFound = 0xC0000034;   // ntstatus.h: STATUS_OBJECT_NAME_NOT_FOUND
constexpr long kStatusAccessDenied = 0xC0000022;     // ntstatus.h: STATUS_ACCESS_DENIED
constexpr long kStatusNoSuchFile = 0xC0000114;       // accepted as an empty directory
constexpr long kStatusNoMoreFiles = 0xC0000121;      // accepted as the end of an enumeration

// The end of an enumeration is accepted in both forms: the two codes above, and the Win32 error codes written
// with the 0x80000000 severity that some redirectors return instead of an NTSTATUS. The sub-code in 0x80000006 is
// 6, not the Win32 number: ERROR_NO_MORE_FILES is 18 in winerror.h, and ntstatus.h itself lists
// STATUS_NO_MORE_FILES as 0x80000006. Likewise 0x80000002 is STATUS_DATATYPE_MISALIGNMENT there; here it is the
// severity form of ERROR_FILE_NOT_FOUND (2). All four mean "the listing is done" in isEndOfEnumeration.
constexpr long kFileErrorBase = 0x80000000;
constexpr long kFileErrorNotFound = kFileErrorBase + 2;    // severity form of ERROR_FILE_NOT_FOUND (2)
constexpr long kFileErrorNoMoreFiles = kFileErrorBase + 6; // sub-code 6, not winerror.h's ERROR_NO_MORE_FILES (18)

bool isEndOfEnumeration(long status)
{
    return status == kStatusNoSuchFile || status == kStatusNoMoreFiles || status == kFileErrorNotFound ||
           status == kFileErrorNoMoreFiles;
}

// One call fills this much of the listing. For a directory with a few thousand entries that is one round
// trip, not one per entry.
constexpr size_t kQueryBufferBytes = 4 * 1024 * 1024;

// The record header exactly as the kernel writes it. Records are 8-byte aligned and chained by
// nextEntryOffset, so the layout is stated here rather than taken from a header this project does not have.
// pack(1) is required: the C++ alignment of the 8-byte members would otherwise pad the header to 72.
#pragma pack(push, 1)
struct DirectoryRecordHeader
{
    uint32_t nextEntryOffset;
    uint32_t fileIndex; // not the file reference; that is the separate 8-byte field after the header
    int64_t creationTime;
    int64_t lastAccessTime;
    int64_t lastWriteTime;
    int64_t changeTime;
    int64_t endOfFile;       // apparent size
    uint64_t allocationSize; // bytes occupied
    uint32_t fileAttributes;
    uint32_t fileNameLength;
    uint32_t eaSize; // the reparse point tag on a reparse point, otherwise 0
};
#pragma pack(pop)
static_assert(sizeof(DirectoryRecordHeader) == 68, "the record header has to be 68 bytes");

// Class 2 puts the name right after the 68-byte header. Class 38 inserts four padding bytes so that the 8-byte
// file reference is aligned, then the name: the reference sits at 72 and the name at 80.
constexpr size_t kNameOffsetWithoutFileId = sizeof(DirectoryRecordHeader);
constexpr size_t kFileIdOffset = sizeof(DirectoryRecordHeader) + 4;
constexpr size_t kNameOffsetWithFileId = kFileIdOffset + 8;

using NtQueryDirectoryFileFn = long(WINAPI *)(HANDLE, HANDLE, void *, void *, IO_STATUS_BLOCK *, void *, ULONG, long,
                                              BOOLEAN, const void *, BOOLEAN);

// Resolved at runtime from the already loaded ntdll, the same way Server.cpp resolves RtlGetVersion.
NtQueryDirectoryFileFn resolveQueryDirectoryFile()
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<NtQueryDirectoryFileFn>(GetProcAddress(ntdll, "NtQueryDirectoryFile"));
}

std::error_code ntStatusToError(long status)
{
    switch (status)
    {
    case kStatusAccessDenied:
        return std::make_error_code(std::errc::permission_denied);
    case kStatusObjectNotFound:
    case kStatusNoSuchFile:
        return std::make_error_code(std::errc::no_such_file_or_directory);
    case kStatusInvalidInfoClass:
    case kStatusNotSupported:
        return std::make_error_code(std::errc::not_supported);
    default:
        return std::make_error_code(std::errc::invalid_argument);
    }
}

// GetLastError() is mapped to the same errno values the RPC layer compares against
// (grpcCodeFromError matches std::errc), so the status a client sees does not depend on which category
// std::error_code happened to carry.
std::error_code win32ToError(unsigned long error)
{
    switch (error)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return std::make_error_code(std::errc::no_such_file_or_directory);
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return std::make_error_code(std::errc::permission_denied);
    case ERROR_DIRECTORY: // 267, the value documented as ERROR_NOT_A_DIRECTORY
        return std::make_error_code(std::errc::not_a_directory);
    default:
        return std::error_code(static_cast<int>(error), std::system_category());
    }
}

// winternl.h declares _FILETIME without the QuadPart member that the other header adds, so the two halves are
// combined here instead of using the union.
int64_t fileTimeToTicks(const FILETIME &time)
{
    return (static_cast<int64_t>(time.dwHighDateTime) << 32) | static_cast<uint32_t>(time.dwLowDateTime);
}

int64_t ticksToEpochSeconds(int64_t ticks)
{
    // 100 ns ticks since 1601-01-01; 116444736000000000 of them separate 1601 from 1970.
    return (ticks - 116444736000000000LL) / 10000000LL;
}

// std::string::starts_with is C++20 and this project builds with C++17.
bool startsWith(const std::wstring &path, const wchar_t *prefix)
{
    return path.compare(0, wcslen(prefix), prefix) == 0;
}

std::wstring toLongPath(const std::wstring &path)
{
    if (path.size() < 4)
    {
        return path;
    }
    if (startsWith(path, L"\\\\?"))
    {
        return path; // already in the long-path form
    }
    if (startsWith(path, L"\\\\"))
    {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

bool isUncPath(const std::wstring &path)
{
    return startsWith(path, L"\\\\") && !startsWith(path, L"\\\\?");
}

std::error_code scanDirectoryFast(const std::filesystem::path &directory, const ScannedEntryVisitor &visitor)
{
    const NtQueryDirectoryFileFn queryDirectoryFile = resolveQueryDirectoryFile();
    if (queryDirectoryFile == nullptr)
    {
        return scanDirectorySimple(directory, visitor);
    }

    const std::wstring wideDirectory = directory.native();
    const HANDLE handle = CreateFileW(toLongPath(wideDirectory).c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return win32ToError(GetLastError());
    }

    // AllocationSize is 0 for a file whose data lives inside its own record, and the value the record reports is
    // not the compressed length for a compressed file, so those cases still need one call per file. UNC paths are
    // skipped: there the call is an IOCTL some redirectors do not implement and it can hang. The cluster size
    // decides which files are worth asking about, and it is read once per directory.
    uint64_t clusterSize = 0;
    unsigned long sectorsPerCluster = 0;
    unsigned long bytesPerSector = 0;
    if (GetDiskFreeSpaceW(toLongPath(wideDirectory).c_str(), &sectorsPerCluster, &bytesPerSector, nullptr, nullptr))
    {
        clusterSize = static_cast<uint64_t>(sectorsPerCluster) * bytesPerSector;
    }
    const bool unc = isUncPath(wideDirectory);

    // Aligned by construction, which is what the kernel requires for the output buffer.
    static thread_local std::vector<uint64_t> buffer(kQueryBufferBytes / sizeof(uint64_t));

    long infoClass = kFileIdFullDirectoryInformation;
    bool restartScan = true;

    while (true)
    {
        IO_STATUS_BLOCK ioStatus = {};
        const long status = queryDirectoryFile(handle, nullptr, nullptr, nullptr, &ioStatus, buffer.data(),
                                               static_cast<ULONG>(buffer.size() * sizeof(uint64_t)), infoClass, FALSE,
                                               nullptr, restartScan ? TRUE : FALSE);
        restartScan = false;

        if (isEndOfEnumeration(status))
        {
            break; // nothing left in this directory
        }
        if (status != kStatusSuccess)
        {
            if (infoClass == kFileIdFullDirectoryInformation &&
                (status == kStatusInvalidInfoClass || status == kStatusNotSupported))
            {
                // A redirector that does not implement class 38: retry with class 2 and accept that the file
                // reference stays 0 for this directory.
                infoClass = kFileFullDirectoryInformation;
                restartScan = true;
                continue;
            }
            CloseHandle(handle);
            spdlog::warn("NtQueryDirectoryFile: status 0x{:X} on {}", static_cast<unsigned long>(status),
                         misc::pathToUtf8(directory));
            return ntStatusToError(status);
        }

        const char *base = reinterpret_cast<const char *>(buffer.data());
        const size_t returned = static_cast<size_t>(ioStatus.Information);
        const size_t nameOffset =
            infoClass == kFileIdFullDirectoryInformation ? kNameOffsetWithFileId : kNameOffsetWithoutFileId;

        size_t consumed = 0;
        while (consumed + sizeof(DirectoryRecordHeader) <= returned)
        {
            const DirectoryRecordHeader &header = *reinterpret_cast<const DirectoryRecordHeader *>(base + consumed);
            const wchar_t *name = reinterpret_cast<const wchar_t *>(base + consumed + nameOffset);
            size_t nameLength = header.fileNameLength / sizeof(wchar_t);
            // Some file systems leave a null inside the reported length.
            if (nameLength > 1 && name[nameLength - 1] == L'\0')
            {
                nameLength -= 1;
            }

            // The enumeration can report "." and "..", and the walk must not re-enter the directory it is
            // already reading.
            const bool selfReference =
                (nameLength == 1 && name[0] == L'.') || (nameLength == 2 && name[0] == L'.' && name[1] == L'.');
            if (nameLength > 0 && !selfReference)
            {
                ScannedEntry entry;
                entry.path = directory / std::wstring(name, nameLength);
                entry.isDirectory = (header.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                entry.isReparsePoint = (header.fileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                entry.fileSize = entry.isDirectory ? 0 : static_cast<uint64_t>(header.endOfFile);
                entry.allocatedSize = entry.isDirectory ? 0 : header.allocationSize;
                entry.fileIndex = 0;
                entry.lastWriteTime = ticksToEpochSeconds(header.lastWriteTime);

                if (infoClass == kFileIdFullDirectoryInformation)
                {
                    // The file reference sits after the header, four bytes past eaSize, so that it is aligned.
                    entry.fileIndex = *reinterpret_cast<const uint64_t *>(base + consumed + kFileIdOffset);
                }

                if (!entry.isDirectory && entry.allocatedSize == 0 && !unc &&
                    (entry.fileSize > clusterSize || (header.fileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0 ||
                     (header.fileAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0))
                {
                    unsigned long highPart = 0;
                    // The plain name resolves to the A version unless UNICODE is defined, so the W version is
                    // called explicitly; the path here is already wide.
                    const unsigned long lowPart =
                        GetCompressedFileSizeW(toLongPath(entry.path.native()).c_str(), &highPart);
                    if (lowPart != INVALID_FILE_SIZE || GetLastError() == NO_ERROR)
                    {
                        entry.allocatedSize = (static_cast<uint64_t>(highPart) << 32) | lowPart;
                    }
                }

                if (!visitor(entry))
                {
                    CloseHandle(handle);
                    return {};
                }
            }

            if (header.nextEntryOffset == 0)
            {
                break;
            }
            consumed += header.nextEntryOffset;
        }
    }

    CloseHandle(handle);
    return {};
}

#endif // _WIN32

// The fallback listing: on Windows it is used when ntdll cannot be resolved, elsewhere it is the normal path.
// Every entry costs its own read - an open plus GetFileInformationByHandle, and two more queries for a file - so
// it is not one call per directory like the fast path above.
std::error_code scanDirectorySimple(const std::filesystem::path &directory, const ScannedEntryVisitor &visitor)
{
    std::error_code ec;
    std::filesystem::directory_iterator iter(directory, ec);
    if (ec)
    {
        return ec;
    }

    for (const std::filesystem::directory_entry &dirEntry : iter)
    {
        // An entry whose metadata cannot be read is skipped instead of failing the whole listing.
        ScannedEntry entry;
        const std::error_code entryEc = scanEntry(dirEntry.path(), entry);
        if (entryEc)
        {
            spdlog::warn("scanDirectory: {} skipped ({})", misc::pathToUtf8(dirEntry.path()), entryEc.message());
            continue;
        }
        if (!visitor(entry))
        {
            return {};
        }
    }
    return {};
}

} // namespace

std::error_code scanEntry(const std::filesystem::path &path, ScannedEntry &entry)
{
    entry = ScannedEntry{};
    entry.path = path;

#ifdef _WIN32
    const HANDLE handle = CreateFileW(toLongPath(path.native()).c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return win32ToError(GetLastError());
    }

    BY_HANDLE_FILE_INFORMATION info = {};
    if (!GetFileInformationByHandle(handle, &info))
    {
        const std::error_code error = win32ToError(GetLastError());
        CloseHandle(handle);
        return error;
    }

    entry.isDirectory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry.isReparsePoint = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    entry.fileIndex = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    entry.lastWriteTime = ticksToEpochSeconds(fileTimeToTicks(info.ftLastWriteTime));

    // The size fields of BY_HANDLE_FILE_INFORMATION are documented as internal, so the apparent size still
    // needs its own call, and the allocated size needs GetCompressedFileSize. A single path is not the hot
    // path: the batched listing above is where these come for free.
    if (!entry.isDirectory)
    {
        LARGE_INTEGER fileSize = {};
        if (!GetFileSizeEx(handle, &fileSize))
        {
            const std::error_code error = win32ToError(GetLastError());
            CloseHandle(handle);
            return error;
        }
        entry.fileSize = static_cast<uint64_t>(fileSize.QuadPart);

        unsigned long highPart = 0;
        const unsigned long lowPart = GetCompressedFileSizeW(toLongPath(path.native()).c_str(), &highPart);
        if (lowPart != INVALID_FILE_SIZE || GetLastError() == NO_ERROR)
        {
            entry.allocatedSize = (static_cast<uint64_t>(highPart) << 32) | lowPart;
        }
    }

    CloseHandle(handle);
    return {};
#else
    struct stat st;
    if (lstat(path.native().c_str(), &st) != 0)
    {
        return std::error_code(errno, std::generic_category());
    }

    entry.isDirectory = S_ISDIR(st.st_mode);
    entry.isReparsePoint = S_ISLNK(st.st_mode);
    entry.fileSize = entry.isDirectory ? 0 : static_cast<uint64_t>(st.st_size);
    entry.allocatedSize = entry.isDirectory ? 0 : static_cast<uint64_t>(st.st_blocks) * 512;
    entry.fileIndex = static_cast<uint64_t>(st.st_ino);
    entry.lastWriteTime = static_cast<int64_t>(st.st_mtime);
    return {};
#endif
}

std::error_code scanDirectory(const std::filesystem::path &directory, const ScannedEntryVisitor &visitor)
{
#ifdef _WIN32
    return scanDirectoryFast(directory, visitor);
#else
    return scanDirectorySimple(directory, visitor);
#endif
}

std::error_code scanDirectoryTree(const std::filesystem::path &root, const ScannedEntryVisitor &visitor)
{
    std::vector<std::filesystem::path> stack;
    stack.push_back(root);

    bool stopped = false;
    bool firstLevel = true;

    while (!stack.empty())
    {
        const std::filesystem::path directory = stack.back();
        stack.pop_back();

        std::error_code ec = scanDirectory(directory, [&visitor, &stack, &stopped](const ScannedEntry &entry) -> bool {
            if (!visitor(entry))
            {
                stopped = true;
                return false;
            }
            if (entry.isDirectory && !entry.isReparsePoint)
            {
                stack.push_back(entry.path);
            }
            return true;
        });

        if (stopped)
        {
            break; // the visitor ended the walk; the reason is its own, not a file system error
        }

        if (ec)
        {
            // A directory that cannot be read is skipped, which is what the traversal did before through
            // directory_options::skip_permission_denied. The first level is different: there is nothing else
            // to walk there, so its failure is the caller's error.
            if (!firstLevel && ec == std::errc::permission_denied)
            {
                spdlog::warn("scanDirectoryTree: {} skipped ({})", misc::pathToUtf8(directory), ec.message());
            }
            else
            {
                return ec;
            }
        }
        firstLevel = false;
    }

    return {};
}
