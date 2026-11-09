#include "Server.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#include <sys/utsname.h>
#include <unistd.h>
#endif
#include "DirectoryScan.h"
#include "Misc.h"

namespace
{
// Chunk size defaults: 64 KiB, capped at 1 MiB so a single response message stays well below
// gRPC's default message-size limit (4 MiB).
constexpr uint64_t kDefaultChunkSize = 64 * 1024;
constexpr uint64_t kMaxChunkSize = 1024 * 1024;

// How many entries one traversal message carries. One entry per message costs 5 bytes of gRPC frame per
// entry on top of its own fields, and compression has nothing to work on at that size, so the entries are
// grouped instead. A batch of 512 entries is roughly 13 KB of the sample scan, still far below gRPC's
// default 4 MiB message limit.
constexpr std::size_t kTraversalBatchSize = 512;

grpc::StatusCode grpcCodeFromError(const std::error_code &ec)
{
    if (ec == std::errc::no_such_file_or_directory)
    {
        return grpc::StatusCode::NOT_FOUND;
    }
    if (ec == std::errc::permission_denied)
    {
        return grpc::StatusCode::PERMISSION_DENIED;
    }
    if (ec == std::errc::invalid_argument)
    {
        return grpc::StatusCode::INVALID_ARGUMENT;
    }
    return grpc::StatusCode::INTERNAL;
}

grpc::Status fileError(const std::error_code &ec, const char *operation, const std::string &path)
{
    // absl::StrFormat is printf-style: a format string written with {} leaves the braces in the
    // message and silently drops the arguments, so the error the client sees says nothing.
    return grpc::Status(grpcCodeFromError(ec),
                        absl::StrFormat("%s failed on \"%s\": %s", operation, path, ec.message()));
}

// Paths arrive as UTF-8 and building a std::filesystem::path from a narrow string is code-page
// dependent, so the conversion is done explicitly and UTF-8 is always sent back.
std::filesystem::path toPath(const std::string &rawPath)
{
    // utf8ToWstring converts the null terminator as well, and a path that keeps it is not the same
    // path: directory_iterator appends "*" to the path, so the wildcard lands after that null and
    // the search pattern the operating system gets is just the directory, which fails. Every other
    // helper in misc strips the terminator first; this is the same strip.
    return misc::normalizePath(misc::wstringRemoveNullTerminator(misc::utf8ToWstring(rawPath)));
}

std::string toUtf8(const std::filesystem::path &path)
{
    // WideCharToMultiByte is called with -1, so it also converts the null terminator; that is why
    // misc has the *RemoveNullTerminator helpers. A response path must not carry it.
    return misc::pathToUtf8(path);
}

// Roots the server can enumerate, sorted: one entry per logical drive on Windows, a single "/"
// elsewhere. GetLogicalDrives() reports substituted network drives and card readers as present,
// and a present drive is not necessarily readable, so getFiles can still fail on one of them.
// Mounted filesystems are not enumerated on POSIX: that needs platform-specific code, which this
// RPC does not do.
std::vector<std::filesystem::path> listRootDirectories()
{
    std::vector<std::filesystem::path> roots;
#ifdef _WIN32
    const unsigned long driveMask = GetLogicalDrives();
    for (unsigned long bit = 0; bit < 26; ++bit)
    {
        if (driveMask & (1UL << bit))
        {
            roots.push_back(std::filesystem::path(std::wstring(1, static_cast<wchar_t>(L'A' + bit)) + L":\\"));
        }
    }
#else
    roots.push_back(std::filesystem::path(L"/"));
#endif
    return roots;
}

// Detected at compile time, so this reports the platform the binary was built for.
const char *platformName()
{
#ifdef _WIN32
    return "windows";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "apple";
#else
    return "unknown";
#endif
}

const char *architectureName()
{
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

std::string osVersion()
{
#ifdef _WIN32
    // GetVersionExW reports 6.3 on Windows 10/11 unless the manifest claims the newer compatibility
    // GUID, so the real version is taken from RtlGetVersion. It is not in the import libraries this
    // project links, so the address is resolved at runtime from the already loaded ntdll.
    using RtlGetVersionFn = LONG(NTAPI *)(OSVERSIONINFOW *);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
    {
        return {};
    }
    const RtlGetVersionFn rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtlGetVersion == nullptr)
    {
        return {};
    }
    OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0)
    {
        return {};
    }
    return absl::StrFormat("%lu.%lu.%lu", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
#else
    struct utsname info;
    if (uname(&info) != 0)
    {
        return {};
    }
    return std::string(info.release);
#endif
}

std::string hostName()
{
#ifdef _WIN32
    wchar_t buffer[256] = {};
    DWORD length = static_cast<DWORD>(sizeof(buffer) / sizeof(wchar_t) - 1);
    if (!GetComputerNameExW(ComputerNameDnsHostname, buffer, &length))
    {
        return {};
    }
    return misc::wStringToString(std::wstring(buffer, length));
#else
    char buffer[256] = {};
    if (gethostname(buffer, static_cast<int>(sizeof(buffer)) - 1) != 0)
    {
        return {};
    }
    return std::string(buffer);
#endif
}

// Read from the environment rather than from the OS API: GetUserNameW lives in advapi32, which this
// project does not link, and the environment is the only source that works on every target.
std::string userName()
{
#ifdef _WIN32
    wchar_t buffer[256] = {};
    const DWORD length =
        GetEnvironmentVariableW(L"USERNAME", buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(wchar_t) - 1));
    if (length == 0 || length >= static_cast<DWORD>(sizeof(buffer) / sizeof(wchar_t)))
    {
        return {};
    }
    return misc::wStringToString(std::wstring(buffer, length));
#else
    const char *user = std::getenv("USER");
    return user != nullptr ? std::string(user) : std::string();
#endif
}

uint64_t totalPhysicalMemoryBytes()
{
#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status))
    {
        return 0;
    }
    return static_cast<uint64_t>(status.ullTotalPhys);
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || pageSize <= 0)
    {
        return 0;
    }
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize);
#endif
}

// Copies a ScannedEntry into a FileEntry. Nothing here is computed: the scanner already read the values from
// the record that listed the entry, which is the whole reason the entry carries them.
void fillFileEntry(pb::FileEntry &entry, const ScannedEntry &scanned)
{
    entry.set_filepath(toUtf8(scanned.path));
    entry.set_isdirectory(scanned.isDirectory);
    entry.set_filesize(scanned.fileSize);
    entry.set_lastwritetime(scanned.lastWriteTime);
    entry.set_allocatedsize(scanned.allocatedSize);
    entry.set_fileindex(scanned.fileIndex);
}

// The same values as fillFileEntry, in the shape the stream sends: the entry's own name and the position of
// its parent, which is what the client already keeps in its tree. Nothing is computed here either.
void fillTraversalEntry(pb::TraversalEntry &entry, const ScannedEntry &scanned, int32_t parentIndex)
{
    entry.set_name(toUtf8(scanned.path.filename()));
    entry.set_parentindex(parentIndex);
    entry.set_isdirectory(scanned.isDirectory);
    entry.set_filesize(scanned.fileSize);
    entry.set_lastwritetime(scanned.lastWriteTime);
    entry.set_allocatedsize(scanned.allocatedSize);
    entry.set_fileindex(scanned.fileIndex);
}
} // namespace

grpc::Status FileBrowserServiceImpl::getFileSize(grpc::ServerContext *context, const pb::FileSizeRequest *request,
                                                 pb::FileSizeReply *response)
{
    const std::string rawPath = misc::stringRemoveNullTerminator(request->filepath());
    if (rawPath.empty())
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "filePath is empty");
    }

    const std::filesystem::path filePath = toPath(rawPath);
    std::error_code ec;
    if (std::filesystem::is_directory(filePath, ec))
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, absl::StrFormat("\"%s\" is a directory", rawPath));
    }
    if (ec)
    {
        return fileError(ec, "is_directory", rawPath);
    }

    const uintmax_t fileSize = std::filesystem::file_size(filePath, ec);
    if (ec)
    {
        return fileError(ec, "file_size", rawPath);
    }

    response->set_filesize(static_cast<uint64_t>(fileSize));
    return grpc::Status::OK;
}

grpc::Status FileBrowserServiceImpl::getFiles(grpc::ServerContext *context, const pb::FilesRequest *request,
                                              pb::FilesReply *response)
{
    const std::string rawPath = misc::stringRemoveNullTerminator(request->directory());
    if (rawPath.empty())
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "directory is empty");
    }

    const std::filesystem::path filePath = toPath(rawPath);
    std::error_code ec = scanDirectory(filePath, [&](const ScannedEntry &scanned) {
        pb::FileEntry fileEntry;
        fillFileEntry(fileEntry, scanned);
        *response->add_fileentrires() = fileEntry;
        return true;
    });
    if (ec)
    {
        return fileError(ec, "getFiles", rawPath);
    }
    return grpc::Status::OK;
}

grpc::Status FileBrowserServiceImpl::getRootDirectories(grpc::ServerContext *context,
                                                        const pb::RootDirectoriesRequest *request,
                                                        pb::RootDirectoriesReply *response)
{
    for (const std::filesystem::path &root : listRootDirectories())
    {
        response->add_roots(toUtf8(root));
    }
    spdlog::debug("getRootDirectories: {} roots", response->roots_size());
    return grpc::Status::OK;
}

grpc::Status FileBrowserServiceImpl::getSystemInfo(grpc::ServerContext *context, const pb::SystemInfoRequest *request,
                                                   pb::SystemInfoReply *response)
{
    response->set_platform(platformName());
    response->set_osversion(osVersion());
    response->set_hostname(hostName());
    response->set_username(userName());
    response->set_architecture(architectureName());
    response->set_logicalcpucount(std::thread::hardware_concurrency());
    response->set_totalphysicalmemory(totalPhysicalMemoryBytes());

    spdlog::debug("getSystemInfo: {} {} ({}), {} cores, {} MiB", response->platform(), response->osversion(),
                  response->hostname(), response->logicalcpucount(), response->totalphysicalmemory() / (1024 * 1024));
    return grpc::Status::OK;
}

grpc::Status FileBrowserServiceImpl::getFileInfo(grpc::ServerContext *context, const pb::FileInfoRequest *request,
                                                 pb::FileEntry *response)
{
    const std::string rawPath = misc::stringRemoveNullTerminator(request->filepath());
    if (rawPath.empty())
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "filePath is empty");
    }

    const std::filesystem::path filePath = toPath(rawPath);
    ScannedEntry scanned;
    std::error_code ec = scanEntry(filePath, scanned);
    if (ec)
    {
        return fileError(ec, "getFileInfo", rawPath);
    }
    fillFileEntry(*response, scanned);
    return grpc::Status::OK;
}

grpc::Status FileBrowserServiceImpl::downloadFile(grpc::ServerContext *context, const pb::DownloadRequest *request,
                                                  grpc::ServerWriter<pb::DownloadChunk> *writer)
{
    const std::string rawPath = misc::stringRemoveNullTerminator(request->filepath());
    if (rawPath.empty())
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "filePath is empty");
    }

    const std::filesystem::path filePath = toPath(rawPath);
    std::error_code ec;
    if (std::filesystem::is_directory(filePath, ec))
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, absl::StrFormat("\"%s\" is a directory", rawPath));
    }
    if (ec)
    {
        return fileError(ec, "is_directory", rawPath);
    }

    const uintmax_t fileSize = std::filesystem::file_size(filePath, ec);
    if (ec)
    {
        return fileError(ec, "file_size", rawPath);
    }

    uint64_t offset = request->offset();
    uint64_t length = request->length();
    uint64_t chunkSize = request->chunksize();

    if (chunkSize == 0)
    {
        chunkSize = kDefaultChunkSize;
    }
    chunkSize = std::min(chunkSize, kMaxChunkSize);

    if (offset > static_cast<uint64_t>(fileSize))
    {
        return grpc::Status(grpc::StatusCode::OUT_OF_RANGE,
                            absl::StrFormat("offset %llu is past the end of the file (size %llu)",
                                            static_cast<unsigned long long>(offset),
                                            static_cast<unsigned long long>(fileSize)));
    }
    // length == 0 means "up to the end of the file"; a length running past the end is clamped, which
    // also makes the stream safe when the file shrank between the metadata call and the read.
    if (length == 0 || length > static_cast<uint64_t>(fileSize) - offset)
    {
        length = static_cast<uint64_t>(fileSize) - offset;
    }

    std::ifstream input(filePath, std::ios::binary);
    if (!input.is_open())
    {
        const std::error_code openError(errno, std::generic_category());
        return fileError(openError, "open", rawPath);
    }
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input)
    {
        return grpc::Status(
            grpc::StatusCode::INTERNAL,
            absl::StrFormat("seek to %llu failed on \"%s\"", static_cast<unsigned long long>(offset), rawPath));
    }

    std::string buffer;
    buffer.resize(static_cast<size_t>(chunkSize));
    pb::DownloadChunk chunk;
    uint32_t sequence = 0;

    while (length > 0)
    {
        if (context->IsCancelled())
        {
            spdlog::trace("downloadFile: cancelled by the client after {} chunks", sequence);
            return grpc::Status(grpc::StatusCode::CANCELLED, "download cancelled by the client");
        }

        const uint64_t wanted = std::min(chunkSize, length);
        input.read(buffer.data(), static_cast<std::streamsize>(wanted));
        const std::streamsize read = input.gcount();
        if (read <= 0)
        {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                absl::StrFormat("unexpected end of file while reading \"%s\"", rawPath));
        }

        chunk.set_sequence(sequence++);
        chunk.set_offset(offset);
        chunk.set_size(static_cast<uint64_t>(read));
        chunk.set_data(buffer.data(), static_cast<size_t>(read));

        if (!writer->Write(chunk))
        {
            spdlog::trace("downloadFile: the stream has been closed after {} chunks", sequence);
            if (context->IsCancelled())
            {
                return grpc::Status(grpc::StatusCode::CANCELLED, "download cancelled by the client");
            }
            return grpc::Status(grpc::StatusCode::ABORTED, "the stream has been closed");
        }

        offset += static_cast<uint64_t>(read);
        length -= static_cast<uint64_t>(read);
    }

    return grpc::Status::OK;
}

grpc::Status FileBrowserServiceImpl::downloadDirectory(grpc::ServerContext *context,
                                                       const pb::DownloadDirectoryRequest *request,
                                                       grpc::ServerWriter<pb::DownloadItem> *writer)
{
    const std::string rawPath = misc::stringRemoveNullTerminator(request->directory());
    if (rawPath.empty())
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "directory is empty");
    }

    const std::filesystem::path filePath = toPath(rawPath);

    uint64_t chunkSize = request->chunksize();
    if (chunkSize == 0)
    {
        chunkSize = kDefaultChunkSize;
    }
    chunkSize = std::min(chunkSize, kMaxChunkSize);

    const uint32_t startEntryIndex = request->startentryindex();
    const uint64_t startOffset = request->startoffset();

    bool cancelledByClient = false;
    bool streamClosed = false;
    // The visitor cannot report a status, so a failure that belongs to this handler - an entry that cannot be
    // opened, a file that ends early - is carried out of it the same way the cancellation reason is.
    grpc::Status failure = grpc::Status::OK;

    // Positions count every entry the walk produced, including the ones a resumed call does not send: an entry
    // sent now can have a parent the client received in the earlier run, and the number it looks up is that
    // parent's position, so the numbering has to cover the skipped entries too. Only directories can be a
    // parent, so this holds the directories plus the directory the download was started on as -1.
    std::unordered_map<std::string, int32_t> positionByPath;
    positionByPath[toUtf8(filePath)] = -1;

    uint32_t nextIndex = 0;
    std::string buffer;
    buffer.resize(static_cast<size_t>(chunkSize));
    pb::DownloadItem item;

    std::error_code ec = scanDirectoryTree(filePath, [&](const ScannedEntry &scanned) -> bool {
        if (context->IsCancelled())
        {
            cancelledByClient = true;
            return false;
        }

        const auto parent = positionByPath.find(toUtf8(scanned.path.parent_path()));
        if (parent == positionByPath.end())
        {
            // Same rule as the traversal: an entry is visited while its own directory is being listed, so its
            // parent is either the directory the download was started on or a directory this walk already
            // visited. Reaching here means the scanner produced a path outside the tree it walked.
            spdlog::warn("downloadDirectory: {} has no parent in the tree", toUtf8(scanned.path));
            return true;
        }

        const uint32_t index = nextIndex++;
        if (scanned.isDirectory)
        {
            positionByPath[toUtf8(scanned.path)] = static_cast<int32_t>(index);
        }
        if (index < startEntryIndex)
        {
            return true;
        }

        item.Clear();
        item.set_name(toUtf8(scanned.path.filename()));
        item.set_parentindex(parent->second);
        item.set_isdirectory(scanned.isDirectory);
        item.set_filesize(scanned.fileSize);
        if (!writer->Write(item))
        {
            streamClosed = true;
            cancelledByClient = context->IsCancelled();
            return false;
        }
        if (scanned.isDirectory)
        {
            return true;
        }

        std::ifstream input(scanned.path, std::ios::binary);
        if (!input.is_open())
        {
            const std::error_code openError(errno, std::generic_category());
            failure = fileError(openError, "open", toUtf8(scanned.path));
            return false;
        }

        uint64_t remaining = scanned.fileSize;
        uint64_t offset = 0;
        // Only the entry the resume starts on can have bytes the client already has.
        if (index == startEntryIndex && startOffset > 0)
        {
            if (startOffset > scanned.fileSize)
            {
                failure =
                    grpc::Status(grpc::StatusCode::OUT_OF_RANGE,
                                 absl::StrFormat("start offset %llu is past the end of \"%s\" (size %llu)",
                                                 static_cast<unsigned long long>(startOffset), toUtf8(scanned.path),
                                                 static_cast<unsigned long long>(scanned.fileSize)));
                return false;
            }
            input.seekg(static_cast<std::streamoff>(startOffset), std::ios::beg);
            if (!input)
            {
                failure =
                    grpc::Status(grpc::StatusCode::INTERNAL,
                                 absl::StrFormat("seek to %llu failed on \"%s\"",
                                                 static_cast<unsigned long long>(startOffset), toUtf8(scanned.path)));
                return false;
            }
            offset = startOffset;
            remaining -= startOffset;
        }

        uint32_t sequence = 0;
        while (remaining > 0)
        {
            if (context->IsCancelled())
            {
                cancelledByClient = true;
                return false;
            }

            const uint64_t wanted = std::min(chunkSize, remaining);
            input.read(buffer.data(), static_cast<std::streamsize>(wanted));
            const std::streamsize read = input.gcount();
            if (read <= 0)
            {
                failure =
                    grpc::Status(grpc::StatusCode::INTERNAL,
                                 absl::StrFormat("unexpected end of file while reading \"%s\"", toUtf8(scanned.path)));
                return false;
            }

            // name, parentIndex, isDirectory and fileSize stay on every chunk so that a message can be read on
            // its own; they are already set from the header written above.
            item.set_sequence(sequence++);
            item.set_offset(offset);
            item.set_size(static_cast<uint64_t>(read));
            item.set_data(buffer.data(), static_cast<size_t>(read));

            if (!writer->Write(item))
            {
                streamClosed = true;
                cancelledByClient = context->IsCancelled();
                return false;
            }

            offset += static_cast<uint64_t>(read);
            remaining -= static_cast<uint64_t>(read);
        }
        return true;
    });

    if (!failure.ok())
    {
        return failure;
    }
    if (cancelledByClient)
    {
        spdlog::trace("downloadDirectory: cancelled by the client after {} entries", nextIndex);
        return grpc::Status(grpc::StatusCode::CANCELLED, "download cancelled by the client");
    }
    if (streamClosed)
    {
        spdlog::trace("downloadDirectory: the stream has been closed");
        return grpc::Status(grpc::StatusCode::ABORTED, "the stream has been closed");
    }
    if (ec)
    {
        return fileError(ec, "downloadDirectory", rawPath);
    }
    return grpc::Status::OK;
}

grpc::Status FileBrowserServiceImpl::uploadFiles(grpc::ServerContext *context,
                                                 grpc::ServerReader<pb::UploadItem> *reader, pb::UploadReply *response)
{
    // Client-streaming has no request message, so the target directory is the first header. pathByIndex
    // rebuilds each path from the parent chain, which is the same shape the client's tree keeps.
    std::vector<std::filesystem::path> pathByIndex;

    uint32_t filesWritten = 0;
    uint32_t directoriesCreated = 0;
    uint64_t bytesWritten = 0;
    // Entries the client said were already complete (UploadHeader.alreadyComplete), so nothing was written for
    // them. They are not in filesWritten.
    uint32_t entriesSkipped = 0;

    std::ofstream out;
    // What is open right now: the server writes the entries in the order the headers arrive, so this is the
    // entry the next chunk has to belong to.
    std::filesystem::path currentEntry;
    uint64_t entryBytes = 0;
    uint32_t sequence = 0;

    pb::UploadItem item;
    while (reader->Read(&item))
    {
        if (context->IsCancelled())
        {
            spdlog::trace("uploadFiles: cancelled by the client after {} bytes", bytesWritten);
            return grpc::Status(grpc::StatusCode::CANCELLED, "upload cancelled by the client");
        }

        if (item.has_header())
        {
            if (item.has_chunk())
            {
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                    "an upload message cannot carry a header and a chunk");
            }

            const pb::UploadHeader &header = item.header();
            const std::string name = misc::stringRemoveNullTerminator(header.name());
            if (name.empty())
            {
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "upload header has an empty name");
            }

            std::filesystem::path path;
            if (pathByIndex.empty())
            {
                if (!header.isdirectory() || header.parentindex() != -1)
                {
                    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                        "the first upload message must be the header of the target directory");
                }
                path = toPath(name);
            }
            else
            {
                if (header.parentindex() < 0 || static_cast<std::size_t>(header.parentindex()) >= pathByIndex.size())
                {
                    return grpc::Status(
                        grpc::StatusCode::INVALID_ARGUMENT,
                        absl::StrFormat("upload header \"%s\" points at parent index %d", name, header.parentindex()));
                }
                path = pathByIndex[static_cast<std::size_t>(header.parentindex())] / toPath(name);
            }
            pathByIndex.push_back(path);
            currentEntry = path;

            if (out.is_open())
            {
                out.close();
            }

            if (header.isdirectory())
            {
                std::error_code mkdirEc;
                // create_directories reports false when the directory was already there, which is not an error;
                // an existing file is, unless the header asked for overwrite.
                const bool created = std::filesystem::create_directories(path, mkdirEc);
                if (mkdirEc)
                {
                    return fileError(mkdirEc, "create_directories", name);
                }
                if (created)
                {
                    ++directoriesCreated;
                }
                continue;
            }

            std::error_code existsEc;
            const bool alreadyThere = std::filesystem::exists(path, existsEc);
            if (existsEc)
            {
                return fileError(existsEc, "exists", name);
            }

            if (header.alreadycomplete())
            {
                // Nothing follows this header: an earlier run finished this entry and the client is only reporting
                // it. The header is still sent because the entries after it count their positions against it. The
                // state the client is using has to match the file, so the size is checked here instead of trusted.
                if (!alreadyThere)
                {
                    return grpc::Status(
                        grpc::StatusCode::FAILED_PRECONDITION,
                        absl::StrFormat("\"%s\" is not there, but the client's state says an earlier run completed it",
                                        name));
                }
                std::error_code sizeEc;
                const uint64_t existing = std::filesystem::file_size(path, sizeEc);
                if (sizeEc)
                {
                    return fileError(sizeEc, "stat", name);
                }
                if (existing != header.filesize())
                {
                    return grpc::Status(
                        grpc::StatusCode::FAILED_PRECONDITION,
                        absl::StrFormat("\"%s\" has %llu bytes, but the client's state says it was completed at %llu",
                                        name, static_cast<unsigned long long>(existing),
                                        static_cast<unsigned long long>(header.filesize())));
                }
                ++entriesSkipped;
                continue;
            }

            if (header.resumefrom() > 0)
            {
                // The client is continuing an earlier run: its state says this file has exactly this many bytes, so
                // the file is opened for append and the chunks that come next start at that offset. If the file has
                // a different number of bytes the state is stale, and that is reported instead of the file being
                // written over. This is checked before the "already exists" rule because continuing a file is not
                // replacing it.
                if (!alreadyThere)
                {
                    return grpc::Status(
                        grpc::StatusCode::FAILED_PRECONDITION,
                        absl::StrFormat("\"%s\" is not there, but the client is resuming it at %llu bytes", name,
                                        static_cast<unsigned long long>(header.resumefrom())));
                }
                std::error_code sizeEc;
                const uint64_t existing = std::filesystem::file_size(path, sizeEc);
                if (sizeEc)
                {
                    return fileError(sizeEc, "stat", name);
                }
                if (existing != header.resumefrom())
                {
                    return grpc::Status(
                        grpc::StatusCode::FAILED_PRECONDITION,
                        absl::StrFormat("\"%s\" has %llu bytes, but the client is resuming it at %llu; upload with "
                                        "overwrite to replace it",
                                        name, static_cast<unsigned long long>(existing),
                                        static_cast<unsigned long long>(header.resumefrom())));
                }
                out.open(path, std::ios::binary | std::ios::app);
                if (!out.is_open())
                {
                    const std::error_code openError(errno, std::generic_category());
                    return fileError(openError, "open", name);
                }
                ++filesWritten;
                entryBytes = header.resumefrom();
                // The sequence cannot be worked out from the byte count, because the last chunk of the interrupted
                // run can have been shorter than the chunk size, so the client sends what it had counted.
                sequence = header.startsequence();
                continue;
            }

            if (alreadyThere && !header.overwrite())
            {
                return grpc::Status(
                    grpc::StatusCode::ALREADY_EXISTS,
                    absl::StrFormat("\"%s\" already exists; upload with overwrite to replace it", name));
            }

            out.open(path, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                const std::error_code openError(errno, std::generic_category());
                return fileError(openError, "open", name);
            }
            ++filesWritten;
            entryBytes = 0;
            sequence = 0;
            continue;
        }

        if (!out.is_open())
        {
            // Covers a chunk before any header and a chunk after an entry the client said was already complete: in
            // both cases there is no entry open for the bytes to belong to.
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "a chunk arrived without an entry open for it");
        }

        const pb::UploadChunk &chunk = item.chunk();
        if (chunk.data().size() > static_cast<std::size_t>(kMaxChunkSize))
        {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                absl::StrFormat("chunk of %zu bytes is over the %llu byte limit", chunk.data().size(),
                                                static_cast<unsigned long long>(kMaxChunkSize)));
        }
        if (chunk.size() != chunk.data().size())
        {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                absl::StrFormat("chunk says %llu bytes but carries %zu",
                                                static_cast<unsigned long long>(chunk.size()), chunk.data().size()));
        }
        if (chunk.sequence() != sequence || chunk.offset() != entryBytes)
        {
            return grpc::Status(
                grpc::StatusCode::INVALID_ARGUMENT,
                absl::StrFormat("chunk %llu at offset %llu is not the next one for \"%s\" (expected %u at %llu)",
                                static_cast<unsigned long long>(chunk.sequence()),
                                static_cast<unsigned long long>(chunk.offset()), toUtf8(currentEntry), sequence,
                                static_cast<unsigned long long>(entryBytes)));
        }

        out.write(chunk.data().data(), static_cast<std::streamsize>(chunk.data().size()));
        if (!out)
        {
            return fileError(std::error_code(errno, std::generic_category()), "write", toUtf8(currentEntry));
        }

        entryBytes += chunk.data().size();
        bytesWritten += chunk.data().size();
        ++sequence;
    }

    if (out.is_open())
    {
        out.close();
    }

    // This is the message gRPC sends when the call returns OK. A status that is not OK reaches the client
    // without it, which is why the counts are described as the ones for a completed upload.
    response->set_fileswritten(filesWritten);
    response->set_directoriescreated(directoriesCreated);
    response->set_byteswritten(bytesWritten);
    response->set_entriesskipped(entriesSkipped);
    return grpc::Status::OK;
}

grpc::Status FileBrowserServiceImpl::recursiveDirectoryTraversal(grpc::ServerContext *context,
                                                                 const pb::FilesRequest *request,
                                                                 grpc::ServerWriter<pb::TraversalBatch> *writer)
{
    const std::string rawPath = misc::stringRemoveNullTerminator(request->directory());
    if (rawPath.empty())
    {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "directory is empty");
    }

    const std::filesystem::path filePath = toPath(rawPath);

    // The visitor return value is how the walk stops, so the reason has to be carried out of it: the
    // scanner reports no error for a visitor that stopped, because the reason is the stream, not the file
    // system.
    bool cancelledByClient = false;
    bool streamClosed = false;
    // parentIndex has to point at an entry that was already sent, so the walk needs a memory of what it
    // emitted. Only directories can be a parent, so this holds the directories, plus the directory the
    // traversal was started on as -1 because that one is never sent. The key is the UTF-8 form of the path,
    // not std::filesystem::path: the NDK's libc++ (r25.1) has no std::hash<filesystem::path>, so a map keyed by
    // path only compiles for Android because misc/Misc.cpp supplies it, and the paths the RPC carries are UTF-8.
    std::unordered_map<std::string, int32_t> sentDirectories;
    sentDirectories[toUtf8(filePath)] = -1;

    int32_t nextIndex = 0;
    pb::TraversalBatch batch;

    const auto flushBatch = [&]() -> bool {
        if (batch.entries_size() == 0)
        {
            return true;
        }
        if (!writer->Write(batch))
        {
            streamClosed = true;
            cancelledByClient = context->IsCancelled();
            return false;
        }
        batch.clear_entries();
        return true;
    };

    std::error_code ec = scanDirectoryTree(filePath, [&](const ScannedEntry &scanned) -> bool {
        if (context->IsCancelled())
        {
            cancelledByClient = true;
            return false;
        }

        const auto parent = sentDirectories.find(toUtf8(scanned.path.parent_path()));
        if (parent == sentDirectories.end())
        {
            // Every entry is emitted while its own directory is being listed, so its parent is either the
            // directory the traversal started on or a directory this walk already emitted. Reaching here
            // means the scanner produced a path outside the tree it walked; the entry is dropped rather
            // than sent with an index that would point at something else on the client.
            spdlog::warn("Recursive directory traversal: {} has no parent in the stream", toUtf8(scanned.path));
            return true;
        }

        fillTraversalEntry(*batch.add_entries(), scanned, parent->second);
        if (scanned.isDirectory)
        {
            // The position this entry takes in the stream. A file can never be a parent, so only the
            // directories need to be remembered; the index itself still counts every entry, because that
            // is what the client counts as it reads.
            sentDirectories[toUtf8(scanned.path)] = nextIndex;
        }
        ++nextIndex;

        if (batch.entries_size() < kTraversalBatchSize)
        {
            return true;
        }
        return flushBatch();
    });

    // A walk that ends between batch boundaries still has entries in the batch, so the last group is smaller
    // than kTraversalBatchSize. Nothing is sent when the client stopped reading: a Write there is what would
    // report the stream as closed, and the status the caller gets should stay CANCELLED.
    if (!streamClosed && !cancelledByClient)
    {
        flushBatch();
    }

    if (ec)
    {
        return fileError(ec, "recursiveDirectoryTraversal", rawPath);
    }
    if (cancelledByClient)
    {
        spdlog::trace("Recursive directory traversal: the stream has been closed");
        return grpc::Status(grpc::StatusCode::CANCELLED, "traversal cancelled by the client");
    }
    if (streamClosed)
    {
        spdlog::trace("Recursive directory traversal: the stream has been closed");
        return grpc::Status(grpc::StatusCode::ABORTED, "the stream has been closed");
    }
    return grpc::Status::OK;
}
