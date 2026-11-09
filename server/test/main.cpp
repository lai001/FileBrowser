#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
// The test includes windows.h for CreateHardLinkW. Without these two guards its min/max macros break the gRPC
// headers Server.h pulls in, and winsock.h clashes with winsock2.h, as ServerLauncher.h already explains.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "gtest/gtest.h"
#include "Server.h"

namespace
{
// Deterministic content, so no expected byte value is hard-coded in the test.
std::string makeContent(std::size_t size)
{
    std::string data;
    data.resize(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        data[i] = static_cast<char>((i * 7 + 13) % 256);
    }
    return data;
}

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct DownloadResult
{
    grpc::Status status;
    std::string data;
    std::vector<std::uint32_t> sequences;
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint64_t> sizes;
};

class FileBrowserSmokeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Files are created in the test working directory (the build directory, see the
        // WORKING_DIRECTORY in CMakeLists.txt); the system temp directory is not always writable.
        workDir = std::filesystem::current_path() / "filebrowser-smoke-test";
        std::filesystem::remove_all(workDir);
        std::filesystem::create_directories(workDir);

        filePath = workDir / "sample.bin";
        content = makeContent(100000);
        std::ofstream out(filePath, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();

        grpc::ServerBuilder builder;
        // Port 0 lets the operating system pick a free port; AddListeningPort reports the port it
        // actually bound through the pointer it is given, once BuildAndStart() has run.
        int port = 0;
        builder.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        ASSERT_TRUE(server != nullptr);
        ASSERT_GT(port, 0);

        stub = pb::FileBrowser::NewStub(
            grpc::CreateChannel("localhost:" + std::to_string(port), grpc::InsecureChannelCredentials()));
        ASSERT_TRUE(stub != nullptr);
    }

    void TearDown() override
    {
        if (server)
        {
            server->Shutdown();
        }
        std::filesystem::remove_all(workDir);
    }

    DownloadResult download(std::uint64_t offset, std::uint64_t length, std::uint32_t chunkSize)
    {
        pb::DownloadRequest request;
        request.set_filepath(filePath.string());
        request.set_offset(offset);
        request.set_length(length);
        request.set_chunksize(chunkSize);

        grpc::ClientContext context;
        std::unique_ptr<grpc::ClientReader<pb::DownloadChunk>> reader(stub->downloadFile(&context, request));

        DownloadResult result;
        pb::DownloadChunk chunk;
        while (reader->Read(&chunk))
        {
            result.data += chunk.data();
            result.sequences.push_back(chunk.sequence());
            result.offsets.push_back(chunk.offset());
            result.sizes.push_back(chunk.size());
        }
        result.status = reader->Finish();
        return result;
    }

    grpc::Status getFileInfo(pb::FileEntry &entry)
    {
        pb::FileInfoRequest request;
        request.set_filepath(filePath.string());

        grpc::ClientContext context;
        return stub->getFileInfo(&context, request, &entry);
    }

    grpc::Status upload(const std::vector<pb::UploadItem> &items, pb::UploadReply &reply)
    {
        grpc::ClientContext context;
        std::unique_ptr<grpc::ClientWriter<pb::UploadItem>> writer(stub->uploadFiles(&context, &reply));
        for (const pb::UploadItem &item : items)
        {
            // A handler that returns before the client finished sending makes its status visible through a
            // Write that comes back false; the loop ends there and Finish() reports the code.
            if (!writer->Write(item))
            {
                break;
            }
        }
        writer->WritesDone();
        return writer->Finish();
    }

    static void expectContiguousChunks(const DownloadResult &result, std::uint64_t startOffset)
    {
        std::uint64_t expectedOffset = startOffset;
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < result.sequences.size(); ++i)
        {
            EXPECT_EQ(result.sequences[i], static_cast<std::uint32_t>(i)) << "chunk " << i;
            EXPECT_EQ(result.offsets[i], expectedOffset) << "chunk " << i;
            expectedOffset += result.sizes[i];
            total += result.sizes[i];
        }
        EXPECT_EQ(total, result.data.size());
        EXPECT_EQ(expectedOffset, startOffset + result.data.size());
    }

    std::filesystem::path workDir;
    std::filesystem::path filePath;
    std::string content;
    FileBrowserServiceImpl service;
    std::unique_ptr<grpc::Server> server;
    std::unique_ptr<pb::FileBrowser::Stub> stub;
};

TEST_F(FileBrowserSmokeTest, GetFileInfoReportsTheStoredMetadata)
{
    pb::FileEntry entry;
    const grpc::Status status = getFileInfo(entry);
    ASSERT_TRUE(status.ok()) << status.error_message();

    EXPECT_EQ(entry.filepath(), filePath.string());
    EXPECT_FALSE(entry.isdirectory());
    EXPECT_EQ(entry.filesize(), static_cast<std::uint64_t>(content.size()));

    const std::int64_t now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    // The server converts the timestamp in the record to epoch seconds, so this only compares two clocks. The
    // tolerance covers a machine that is not exactly in sync, and the leap-second offset older standard
    // libraries had when file_clock could not be converted to system_clock.
    EXPECT_GT(entry.lastwritetime(), 0);
    EXPECT_NEAR(now - entry.lastwritetime(), 0, 120);
}

TEST_F(FileBrowserSmokeTest, GetFileInfoReportsADirectoryAsSizeZero)
{
    const std::filesystem::path directory = workDir / "sub dir";
    std::filesystem::create_directories(directory);

    pb::FileEntry entry;
    grpc::ClientContext context;
    pb::FileInfoRequest request;
    request.set_filepath(directory.string());
    const grpc::Status status = stub->getFileInfo(&context, request, &entry);
    ASSERT_TRUE(status.ok()) << status.error_message();

    EXPECT_TRUE(entry.isdirectory());
    EXPECT_EQ(entry.filesize(), 0);
}

TEST_F(FileBrowserSmokeTest, GetFileInfoReportsMissingAndEmptyPaths)
{
    pb::FileEntry entry;
    grpc::ClientContext missingContext;
    pb::FileInfoRequest missingRequest;
    missingRequest.set_filepath((workDir / "does-not-exist.bin").string());
    const grpc::Status missingStatus = stub->getFileInfo(&missingContext, missingRequest, &entry);
    EXPECT_EQ(missingStatus.error_code(), grpc::StatusCode::NOT_FOUND);

    entry.Clear();
    grpc::ClientContext emptyContext;
    pb::FileInfoRequest emptyRequest;
    const grpc::Status emptyStatus = stub->getFileInfo(&emptyContext, emptyRequest, &entry);
    EXPECT_EQ(emptyStatus.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(FileBrowserSmokeTest, GetRootDirectoriesListsTheRootOfTheWorkingDirectory)
{
    grpc::ClientContext context;
    pb::RootDirectoriesRequest request;
    pb::RootDirectoriesReply reply;
    const grpc::Status status = stub->getRootDirectories(&context, request, &reply);
    ASSERT_TRUE(status.ok()) << status.error_message();

    ASSERT_GT(reply.roots_size(), 0);
    const std::vector<std::string> roots(reply.roots().begin(), reply.roots().end());
    EXPECT_TRUE(std::is_sorted(roots.begin(), roots.end()));

    for (const std::string &root : roots)
    {
        EXPECT_FALSE(root.empty());
        // Every value must be a root on its own, so the client can send it straight back to getFiles.
        EXPECT_EQ(std::filesystem::path(root).root_path(), std::filesystem::path(root)) << root;
    }

    // A drive reported as present is not necessarily readable (an empty card reader), so only the
    // root the test runs on is required to be a directory.
    const std::string workRoot = workDir.root_path().string();
    const auto found = std::find(roots.begin(), roots.end(), workRoot);
    ASSERT_NE(found, roots.end()) << workRoot;
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::is_directory(*found, ec)) << *found << ": " << ec.message();
}

TEST_F(FileBrowserSmokeTest, GetFilesListsTheDirectoryItIsGiven)
{
    const std::filesystem::path directory = workDir / "sub dir";
    std::filesystem::create_directories(directory);

    grpc::ClientContext context;
    pb::FilesRequest request;
    request.set_directory(workDir.string());
    pb::FilesReply reply;
    const grpc::Status status = stub->getFiles(&context, request, &reply);
    ASSERT_TRUE(status.ok()) << status.error_message();

    // The working directory holds exactly sample.bin plus the directory created above.
    ASSERT_EQ(reply.fileentrires_size(), 2) << "entries were skipped, check the spdlog warnings";
    std::size_t directoryCount = 0;
    for (const pb::FileEntry &entry : reply.fileentrires())
    {
        // Paths come back as UTF-8 and must stay inside the requested directory, so the client can
        // send them straight back to getFiles or downloadFile.
        EXPECT_EQ(std::filesystem::path(entry.filepath()).parent_path(), workDir);
        directoryCount += entry.isdirectory() ? 1 : 0;
    }
    EXPECT_EQ(directoryCount, 1);
}

TEST_F(FileBrowserSmokeTest, GetSystemInfoReportsTheMachineTheServerRunsOn)
{
    grpc::ClientContext context;
    pb::SystemInfoRequest request;
    pb::SystemInfoReply reply;
    const grpc::Status status = stub->getSystemInfo(&context, request, &reply);
    ASSERT_TRUE(status.ok()) << status.error_message();

    const std::vector<std::string> knownPlatforms = {"windows", "linux", "android", "apple", "unknown"};
    EXPECT_TRUE(std::find(knownPlatforms.begin(), knownPlatforms.end(), reply.platform()) != knownPlatforms.end())
        << reply.platform();

    const std::vector<std::string> knownArchitectures = {"x64", "arm64", "x86", "unknown"};
    EXPECT_TRUE(std::find(knownArchitectures.begin(), knownArchitectures.end(), reply.architecture()) !=
                knownArchitectures.end())
        << reply.architecture();

    EXPECT_FALSE(reply.osversion().empty());
    EXPECT_FALSE(reply.hostname().empty());
    EXPECT_GT(reply.logicalcpucount(), 0);
    EXPECT_GT(reply.totalphysicalmemory(), 0);

    // The user name comes from the environment, so it can legitimately be empty on a target that
    // does not carry it; when it is reported, the test process (which hosts the service) sees the
    // same value.
#ifdef _WIN32
    const char *expectedUser = std::getenv("USERNAME");
    EXPECT_EQ(reply.username(), expectedUser != nullptr ? expectedUser : "");
#endif
}

TEST_F(FileBrowserSmokeTest, DownloadFileReturnsTheWholeContentInChunks)
{
    const DownloadResult result = download(0, 0, 0);
    ASSERT_TRUE(result.status.ok()) << result.status.error_message();

    EXPECT_EQ(result.data, content);
    EXPECT_GT(result.sequences.size(), 1); // 100000 bytes with the default 64 KiB chunk size
    expectContiguousChunks(result, 0);
}

TEST_F(FileBrowserSmokeTest, DownloadFileHonoursTheRequestedChunkSize)
{
    const DownloadResult result = download(0, 0, 1024);
    ASSERT_TRUE(result.status.ok()) << result.status.error_message();

    EXPECT_EQ(result.data, content);
    EXPECT_EQ(result.sequences.size(), 98); // 97 full chunks of 1024 bytes + a 672 byte tail
    for (std::uint64_t size : result.sizes)
    {
        EXPECT_LE(size, 1024);
    }
    expectContiguousChunks(result, 0);
}

TEST_F(FileBrowserSmokeTest, DownloadFileResumesFromTheOffsetAlreadyReceived)
{
    const std::uint64_t received = 40000;
    const DownloadResult first = download(0, received, 2048);
    ASSERT_TRUE(first.status.ok()) << first.status.error_message();
    ASSERT_EQ(first.data, content.substr(0, static_cast<std::size_t>(received)));

    // The client keeps the byte count it already wrote, so the next request starts where it stopped.
    const DownloadResult second = download(received, 0, 2048);
    ASSERT_TRUE(second.status.ok()) << second.status.error_message();
    EXPECT_EQ(second.data, content.substr(static_cast<std::size_t>(received)));
    EXPECT_EQ(first.data + second.data, content);
    EXPECT_EQ(second.sequences.front(), 0); // a new call restarts the chunk numbering
    expectContiguousChunks(second, received);
}

TEST_F(FileBrowserSmokeTest, DownloadFileClampsALengthRunningPastTheEndOfTheFile)
{
    const std::uint64_t offset = 50000;
    const DownloadResult result = download(offset, content.size() + 1000, 1024);
    ASSERT_TRUE(result.status.ok()) << result.status.error_message();

    EXPECT_EQ(result.data, content.substr(static_cast<std::size_t>(offset)));
    expectContiguousChunks(result, offset);
}

TEST_F(FileBrowserSmokeTest, DownloadFileRejectsAnOffsetPastTheEndAndADirectory)
{
    grpc::ClientContext context;
    pb::DownloadRequest request;
    request.set_filepath(filePath.string());
    request.set_offset(content.size() + 1);
    request.set_chunksize(1024);

    std::unique_ptr<grpc::ClientReader<pb::DownloadChunk>> reader(stub->downloadFile(&context, request));
    pb::DownloadChunk chunk;
    while (reader->Read(&chunk))
    {
    }
    const grpc::Status status = reader->Finish();
    EXPECT_EQ(status.error_code(), grpc::StatusCode::OUT_OF_RANGE);

    grpc::ClientContext directoryContext;
    pb::DownloadRequest directoryRequest;
    directoryRequest.set_filepath(workDir.string());
    std::unique_ptr<grpc::ClientReader<pb::DownloadChunk>> directoryReader(
        stub->downloadFile(&directoryContext, directoryRequest));
    while (directoryReader->Read(&chunk))
    {
    }
    const grpc::Status directoryStatus = directoryReader->Finish();
    EXPECT_EQ(directoryStatus.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(FileBrowserSmokeTest, GetFilesReportsTheFileIndexAndTheAllocatedSize)
{
    grpc::ClientContext context;
    pb::FilesRequest request;
    request.set_directory(workDir.string());
    pb::FilesReply reply;
    const grpc::Status status = stub->getFiles(&context, request, &reply);
    ASSERT_TRUE(status.ok()) << status.error_message();

    ASSERT_EQ(reply.fileentrires_size(), 1);
    const pb::FileEntry &entry = reply.fileentrires(0);

    // Both values come from the record that listed the entry, not from a query per file. A 100000 byte file
    // occupies at least its own size, and a file the server just listed on its own machine always has a
    // reference.
    EXPECT_EQ(entry.filesize(), static_cast<std::uint64_t>(content.size()));
    EXPECT_GT(entry.fileindex(), 0);
    EXPECT_GE(entry.allocatedsize(), entry.filesize());
}

TEST_F(FileBrowserSmokeTest, HardLinksToTheSameFileShareOneFileIndex)
{
    const std::filesystem::path linkPath = workDir / "hardlink.bin";
#ifdef _WIN32
    if (CreateHardLinkW(linkPath.c_str(), filePath.c_str(), nullptr) == 0)
    {
        GTEST_SKIP() << "CreateHardLinkW failed, this account cannot create hard links";
    }
#else
    if (link(filePath.c_str(), linkPath.c_str()) != 0)
    {
        GTEST_SKIP() << "link() failed, this filesystem does not support hard links";
    }
#endif

    grpc::ClientContext context;
    pb::FilesRequest request;
    request.set_directory(workDir.string());
    pb::FilesReply reply;
    const grpc::Status status = stub->getFiles(&context, request, &reply);
    ASSERT_TRUE(status.ok()) << status.error_message();

    ASSERT_EQ(reply.fileentrires_size(), 2);
    std::uint64_t sharedIndex = 0;
    for (const pb::FileEntry &entry : reply.fileentrires())
    {
        EXPECT_EQ(entry.filesize(), static_cast<std::uint64_t>(content.size()));
        ASSERT_GT(entry.fileindex(), 0);
        if (sharedIndex == 0)
        {
            sharedIndex = entry.fileindex();
        }
        else
        {
            // Two paths, one file. A total that adds both sizes counts the same bytes twice, which is what
            // the client has to stop doing.
            EXPECT_EQ(entry.fileindex(), sharedIndex);
        }
    }
}

TEST_F(FileBrowserSmokeTest, RecursiveDirectoryTraversalDescendsIntoSubdirectories)
{
    const std::filesystem::path sub = workDir / "sub";
    std::filesystem::create_directories(sub);

    const std::string subContent = makeContent(5);
    std::ofstream out(sub / "deep.bin", std::ios::binary);
    out.write(subContent.data(), static_cast<std::streamsize>(subContent.size()));
    out.close();

    grpc::ClientContext context;
    pb::FilesRequest request;
    request.set_directory(workDir.string());
    std::unique_ptr<grpc::ClientReader<pb::TraversalBatch>> reader(
        stub->recursiveDirectoryTraversal(&context, request));

    // What the stream sent, in arrival order: the parent index and the entry's own name. Reading those is
    // only half of the compact form; the test puts the paths back together, because that is what the client
    // has to do.
    std::vector<int32_t> parentIndexes;
    std::vector<std::string> names;
    std::size_t entries = 0;
    std::size_t directories = 0;
    pb::TraversalBatch batch;
    while (reader->Read(&batch))
    {
        for (const pb::TraversalEntry &entry : batch.entries())
        {
            // An entry is sent while its own directory is being listed, so its parent is -1 - the directory
            // the traversal was started on, which is never sent - or an entry read before it.
            EXPECT_TRUE(entry.parentindex() == -1 || entry.parentindex() < static_cast<int32_t>(parentIndexes.size()));

            parentIndexes.push_back(entry.parentindex());
            names.push_back(entry.name());
            ++entries;
            directories += entry.isdirectory() ? 1 : 0;
            EXPECT_GT(entry.fileindex(), 0);
            EXPECT_GE(entry.allocatedsize(), entry.filesize());
        }
    }
    const grpc::Status status = reader->Finish();
    ASSERT_TRUE(status.ok()) << status.error_message();

    // sample.bin, sub, sub/deep.bin. The directory the traversal was started on is not one of them, so the
    // entries directly inside it are the ones whose parentIndex is -1.
    EXPECT_EQ(entries, 3);
    EXPECT_EQ(directories, 1);

    std::set<std::string> rebuilt;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        std::filesystem::path path = names[i];
        for (int32_t parent = parentIndexes[i]; parent >= 0; parent = parentIndexes[static_cast<std::size_t>(parent)])
        {
            path = names[static_cast<std::size_t>(parent)] / path;
        }
        rebuilt.insert((workDir / path).string());
    }

    std::set<std::string> expected;
    expected.insert((workDir / "sample.bin").string());
    expected.insert((workDir / "sub").string());
    expected.insert((workDir / "sub" / "deep.bin").string());
    EXPECT_EQ(rebuilt, expected);
}

TEST_F(FileBrowserSmokeTest, DownloadDirectorySendsOneHeaderPerEntryAndTheContentAfterIt)
{
    const std::filesystem::path sub = workDir / "sub";
    std::filesystem::create_directories(sub);
    const std::string subContent = makeContent(3000);
    std::ofstream out(sub / "deep.bin", std::ios::binary);
    out.write(subContent.data(), static_cast<std::streamsize>(subContent.size()));
    out.close();

    pb::DownloadDirectoryRequest request;
    request.set_directory(workDir.string());
    request.set_chunksize(1024);

    grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReader<pb::DownloadItem>> reader(stub->downloadDirectory(&context, request));

    std::vector<std::string> pathOfPosition;
    std::map<std::string, std::string> contentByPath;
    std::map<std::string, std::uint32_t> nextSequenceByPath;
    std::map<std::string, std::uint64_t> nextOffsetByPath;
    std::size_t entries = 0;
    std::size_t directories = 0;
    const std::string sep(1, std::filesystem::path::preferred_separator);

    // A message with no data is a header, and that is the only message a directory gets. The content of a
    // file follows its header, so the path of the header read last is the path the content belongs to - the
    // same rule the server writes by.
    std::string currentPath;
    pb::DownloadItem item;
    while (reader->Read(&item))
    {
        if (item.data().empty())
        {
            const int32_t parentIndex = item.parentindex();
            if (parentIndex >= 0 && static_cast<std::size_t>(parentIndex) >= pathOfPosition.size())
            {
                ADD_FAILURE() << "entry " << item.name() << " points at parent index " << parentIndex;
                continue;
            }
            const std::string parent =
                parentIndex < 0 ? workDir.string() : pathOfPosition[static_cast<std::size_t>(parentIndex)];
            currentPath = parent + sep + item.name();
            pathOfPosition.push_back(currentPath);
            ++entries;
            directories += item.isdirectory() ? 1 : 0;
            EXPECT_EQ(item.sequence(), 0) << "the header of " << item.name();
            EXPECT_EQ(item.size(), 0) << "the header of " << item.name();
            continue;
        }

        EXPECT_LE(item.data().size(), 1024) << "chunk of " << currentPath;
        EXPECT_EQ(item.sequence(), nextSequenceByPath[currentPath]) << "chunk of " << currentPath;
        EXPECT_EQ(item.offset(), nextOffsetByPath[currentPath]) << "chunk of " << currentPath;
        ++nextSequenceByPath[currentPath];
        nextOffsetByPath[currentPath] += item.data().size();
        contentByPath[currentPath] += item.data();
    }

    const grpc::Status status = reader->Finish();
    ASSERT_TRUE(status.ok()) << status.error_message();

    // sample.bin, sub, sub/deep.bin. The directory the download was started on is not one of them, so the
    // entries directly inside it are the ones whose parentIndex is -1.
    EXPECT_EQ(entries, 3);
    EXPECT_EQ(directories, 1);

    EXPECT_EQ(contentByPath[workDir.string() + sep + "sample.bin"], content);
    EXPECT_EQ(contentByPath[workDir.string() + sep + "sub" + sep + "deep.bin"], subContent);
}

TEST_F(FileBrowserSmokeTest, DownloadDirectoryResumesFromTheEntryAndOffsetAlreadyReceived)
{
    const std::filesystem::path sub = workDir / "sub";
    std::filesystem::create_directories(sub);
    const std::string subContent = makeContent(3000);
    std::ofstream out(sub / "deep.bin", std::ios::binary);
    out.write(subContent.data(), static_cast<std::streamsize>(subContent.size()));
    out.close();

    // Which entry the walk produces first is not guaranteed, so the order is read once and the last entry
    // with content is the one to resume from.
    pb::DownloadDirectoryRequest request;
    request.set_directory(workDir.string());
    request.set_chunksize(1024);

    grpc::ClientContext firstContext;
    std::unique_ptr<grpc::ClientReader<pb::DownloadItem>> firstReader(stub->downloadDirectory(&firstContext, request));

    std::vector<std::int32_t> parentIndexOfPosition;
    std::vector<std::string> nameOfPosition;
    std::map<std::size_t, std::string> dataByPosition;
    std::size_t lastPositionWithData = 0;
    pb::DownloadItem item;
    while (firstReader->Read(&item))
    {
        if (item.data().empty())
        {
            parentIndexOfPosition.push_back(item.parentindex());
            nameOfPosition.push_back(item.name());
            continue;
        }
        const std::size_t position = parentIndexOfPosition.size() - 1;
        dataByPosition[position] += item.data();
        lastPositionWithData = position;
    }
    ASSERT_TRUE(firstReader->Finish().ok());
    ASSERT_FALSE(dataByPosition[lastPositionWithData].empty());
    ASSERT_GT(dataByPosition[lastPositionWithData].size(), 1024);

    request.set_startentryindex(static_cast<std::uint32_t>(lastPositionWithData));
    request.set_startoffset(1024);

    grpc::ClientContext secondContext;
    std::unique_ptr<grpc::ClientReader<pb::DownloadItem>> secondReader(
        stub->downloadDirectory(&secondContext, request));

    std::string resumed;
    std::uint32_t chunks = 0;
    bool headerSeen = false;
    while (secondReader->Read(&item))
    {
        if (!headerSeen)
        {
            ASSERT_TRUE(item.data().empty());
            // The parent of the resumed entry was sent by the first call and not by this one. A client that
            // wants the path back has to have kept that header, which is what the proto comment says.
            EXPECT_EQ(item.parentindex(), parentIndexOfPosition[lastPositionWithData]);
            if (item.parentindex() >= 0)
            {
                EXPECT_LT(static_cast<std::size_t>(item.parentindex()), lastPositionWithData);
            }
            EXPECT_EQ(item.name(), nameOfPosition[lastPositionWithData]);
            EXPECT_EQ(item.filesize(), dataByPosition[lastPositionWithData].size())
                << "the header still reports the whole size, not what is left";
            headerSeen = true;
            continue;
        }

        // Nothing from the entries before the resumed one is sent, so every message after the header still
        // belongs to it.
        EXPECT_EQ(item.name(), nameOfPosition[lastPositionWithData]);
        EXPECT_EQ(item.sequence(), chunks) << "chunk " << chunks;
        EXPECT_EQ(item.offset(), 1024 + resumed.size()) << "chunk " << chunks;
        resumed += item.data();
        ++chunks;
    }

    ASSERT_TRUE(secondReader->Finish().ok());
    ASSERT_TRUE(headerSeen);
    EXPECT_EQ(resumed, dataByPosition[lastPositionWithData].substr(1024));
}

TEST_F(FileBrowserSmokeTest, UploadFilesWritesFilesAndCreatesDirectories)
{
    const std::string data = makeContent(2500);

    std::vector<pb::UploadItem> items;

    pb::UploadItem first;
    pb::UploadHeader &targetHeader = *first.mutable_header();
    targetHeader.set_name((workDir / "upload").string());
    targetHeader.set_parentindex(-1);
    targetHeader.set_isdirectory(true);
    items.push_back(first);

    pb::UploadItem subItem;
    pb::UploadHeader &subHeader = *subItem.mutable_header();
    subHeader.set_name("sub");
    subHeader.set_parentindex(0);
    subHeader.set_isdirectory(true);
    items.push_back(subItem);

    pb::UploadItem fileItem;
    pb::UploadHeader &fileHeader = *fileItem.mutable_header();
    fileHeader.set_name("a.bin");
    fileHeader.set_parentindex(1);
    fileHeader.set_filesize(data.size());
    items.push_back(fileItem);

    for (std::size_t offset = 0; offset < data.size(); offset += 1000)
    {
        const std::size_t length = std::min<std::size_t>(1000, data.size() - offset);
        pb::UploadItem chunkItem;
        pb::UploadChunk &chunk = *chunkItem.mutable_chunk();
        chunk.set_sequence(static_cast<std::uint32_t>(offset / 1000));
        chunk.set_offset(offset);
        chunk.set_size(length);
        chunk.set_data(data.substr(offset, length));
        items.push_back(chunkItem);
    }

    pb::UploadReply reply;
    const grpc::Status status = upload(items, reply);
    ASSERT_TRUE(status.ok()) << status.error_message();

    EXPECT_EQ(reply.fileswritten(), 1);
    // The target directory and the one inside it, both created by this call.
    EXPECT_EQ(reply.directoriescreated(), 2);
    EXPECT_EQ(reply.byteswritten(), data.size());

    EXPECT_EQ(readFile(workDir / "upload" / "sub" / "a.bin"), data);
}

TEST_F(FileBrowserSmokeTest, UploadFilesRejectsAnExistingFileUnlessTheHeaderAsksForOverwrite)
{
    std::vector<pb::UploadItem> items;

    pb::UploadItem first;
    pb::UploadHeader &targetHeader = *first.mutable_header();
    targetHeader.set_name(workDir.string());
    targetHeader.set_parentindex(-1);
    targetHeader.set_isdirectory(true);
    items.push_back(first);

    pb::UploadItem fileItem;
    pb::UploadHeader &fileHeader = *fileItem.mutable_header();
    // sample.bin is the file SetUp wrote in the working directory.
    fileHeader.set_name("sample.bin");
    fileHeader.set_parentindex(0);
    fileHeader.set_filesize(3);
    items.push_back(fileItem);

    pb::UploadItem chunkItem;
    pb::UploadChunk &chunk = *chunkItem.mutable_chunk();
    chunk.set_sequence(0);
    chunk.set_offset(0);
    chunk.set_size(3);
    chunk.set_data("abc");
    items.push_back(chunkItem);

    pb::UploadReply rejected;
    const grpc::Status status = upload(items, rejected);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
    // The server stopped at the header, before opening anything, so the file is still what it was.
    EXPECT_EQ(readFile(filePath), content);

    // items holds a copy of the message, so the change has to be made on the copy that is sent.
    items[1].mutable_header()->set_overwrite(true);
    pb::UploadReply replaced;
    const grpc::Status replacedStatus = upload(items, replaced);
    ASSERT_TRUE(replacedStatus.ok()) << replacedStatus.error_message();
    EXPECT_EQ(readFile(filePath), "abc");
    EXPECT_EQ(replaced.fileswritten(), 1);
    EXPECT_EQ(replaced.byteswritten(), 3);
    // The target directory was already there, so this call created nothing.
    EXPECT_EQ(replaced.directoriescreated(), 0);
}

TEST_F(FileBrowserSmokeTest, UploadFilesRejectsAChunkThatIsNotTheNextOneForItsEntry)
{
    std::vector<pb::UploadItem> items;

    pb::UploadItem first;
    pb::UploadHeader &targetHeader = *first.mutable_header();
    targetHeader.set_name(workDir.string());
    targetHeader.set_parentindex(-1);
    targetHeader.set_isdirectory(true);
    items.push_back(first);

    pb::UploadItem fileItem;
    pb::UploadHeader &fileHeader = *fileItem.mutable_header();
    fileHeader.set_name("b.bin");
    fileHeader.set_parentindex(0);
    fileHeader.set_filesize(6);
    items.push_back(fileItem);

    pb::UploadItem goodChunk;
    pb::UploadChunk &goodChunkFields = *goodChunk.mutable_chunk();
    goodChunkFields.set_sequence(0);
    goodChunkFields.set_offset(0);
    goodChunkFields.set_size(3);
    goodChunkFields.set_data("abc");
    items.push_back(goodChunk);

    pb::UploadItem badChunk;
    pb::UploadChunk &badChunkFields = *badChunk.mutable_chunk();
    badChunkFields.set_sequence(2);
    badChunkFields.set_offset(6);
    badChunkFields.set_size(3);
    badChunkFields.set_data("def");
    items.push_back(badChunk);

    pb::UploadReply reply;
    const grpc::Status status = upload(items, reply);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    // The bytes written before the bad chunk are what the file holds: the server does not roll them back,
    // it stops at the message it cannot place.
    EXPECT_EQ(readFile(workDir / "b.bin"), "abc");
}

TEST_F(FileBrowserSmokeTest, UploadFilesRejectsAMessageThatCarriesAHeaderAndAChunkTogether)
{
    pb::UploadItem item;
    pb::UploadHeader &targetHeader = *item.mutable_header();
    targetHeader.set_name(workDir.string());
    targetHeader.set_parentindex(-1);
    targetHeader.set_isdirectory(true);
    pb::UploadChunk &chunk = *item.mutable_chunk();
    chunk.set_sequence(0);
    chunk.set_offset(0);
    chunk.set_size(1);
    chunk.set_data("x");

    pb::UploadReply reply;
    const grpc::Status status = upload({item}, reply);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    // The message was rejected before anything was written.
    EXPECT_EQ(reply.fileswritten(), 0);
    EXPECT_EQ(reply.directoriescreated(), 0);
}
// A resumed entry: the header says how many bytes the server already has, so the file is opened for append and the
// chunks that come after it start at that offset with the sequence the client remembered. What is already on disk
// stays; this call only adds the rest.
TEST_F(FileBrowserSmokeTest, UploadFilesAppendsToTheFileTheClientIsResuming)
{
    const std::string already = makeContent(1000);
    const std::string rest = makeContent(1000);
    const std::filesystem::path partial = workDir / "resume.bin";
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    out.write(already.data(), static_cast<std::streamsize>(already.size()));
    out.close();

    std::vector<pb::UploadItem> items;

    pb::UploadItem target;
    pb::UploadHeader &targetHeader = *target.mutable_header();
    targetHeader.set_name(workDir.string());
    targetHeader.set_parentindex(-1);
    targetHeader.set_isdirectory(true);
    items.push_back(target);

    pb::UploadItem fileItem;
    pb::UploadHeader &fileHeader = *fileItem.mutable_header();
    fileHeader.set_name("resume.bin");
    fileHeader.set_parentindex(0);
    fileHeader.set_filesize(2000);
    fileHeader.set_resumefrom(1000);
    fileHeader.set_startsequence(1);
    items.push_back(fileItem);

    pb::UploadItem chunk;
    pb::UploadChunk &chunkFields = *chunk.mutable_chunk();
    chunkFields.set_sequence(1);
    chunkFields.set_offset(1000);
    chunkFields.set_size(rest.size());
    chunkFields.set_data(rest);
    items.push_back(chunk);

    pb::UploadReply reply;
    const grpc::Status status = upload(items, reply);
    ASSERT_TRUE(status.ok()) << status.error_message();

    EXPECT_EQ(reply.fileswritten(), 1);
    EXPECT_EQ(reply.entriesskipped(), 0);
    // Only what this call sent, not what the earlier run had already written.
    EXPECT_EQ(reply.byteswritten(), rest.size());

    EXPECT_EQ(readFile(partial), already + rest);
}

// An entry the client says is already complete is reported as skipped and nothing is written for it. The header is
// still read because the entries after it count their positions against it, so the state has to agree with the file
// on the server: a size that does not match is an error, not a reason to start over.
TEST_F(FileBrowserSmokeTest, UploadFilesReportsAnEntryTheClientSaysIsAlreadyComplete)
{
    const std::string data = makeContent(50);
    const std::filesystem::path done = workDir / "done.bin";
    std::ofstream out(done, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();

    std::vector<pb::UploadItem> items;

    pb::UploadItem target;
    pb::UploadHeader &targetHeader = *target.mutable_header();
    targetHeader.set_name(workDir.string());
    targetHeader.set_parentindex(-1);
    targetHeader.set_isdirectory(true);
    items.push_back(target);

    pb::UploadItem doneItem;
    pb::UploadHeader &doneHeader = *doneItem.mutable_header();
    doneHeader.set_name("done.bin");
    doneHeader.set_parentindex(0);
    doneHeader.set_filesize(data.size());
    doneHeader.set_alreadycomplete(true);
    items.push_back(doneItem);

    pb::UploadItem afterIt;
    pb::UploadHeader &afterHeader = *afterIt.mutable_header();
    afterHeader.set_name("new.bin");
    afterHeader.set_parentindex(0);
    afterHeader.set_filesize(3);
    items.push_back(afterIt);

    pb::UploadItem chunk;
    pb::UploadChunk &chunkFields = *chunk.mutable_chunk();
    chunkFields.set_sequence(0);
    chunkFields.set_offset(0);
    chunkFields.set_size(3);
    chunkFields.set_data("abc");
    items.push_back(chunk);

    pb::UploadReply reply;
    const grpc::Status status = upload(items, reply);
    ASSERT_TRUE(status.ok()) << status.error_message();

    // Only the entry that was not already there was written.
    EXPECT_EQ(reply.fileswritten(), 1);
    EXPECT_EQ(reply.entriesskipped(), 1);
    EXPECT_EQ(reply.byteswritten(), 3u);

    EXPECT_EQ(readFile(done), data);
    EXPECT_EQ(readFile(workDir / "new.bin"), "abc");
}

// The offset the client is resuming from has to be what the file on the server actually holds. A state file that
// describes a different run is reported instead of the file being written over.
TEST_F(FileBrowserSmokeTest, UploadFilesRejectsAResumeOffsetThatDoesNotMatchTheFileOnDisk)
{
    const std::string data = makeContent(1000);
    const std::filesystem::path partial = workDir / "stale.bin";
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();

    std::vector<pb::UploadItem> items;

    pb::UploadItem target;
    pb::UploadHeader &targetHeader = *target.mutable_header();
    targetHeader.set_name(workDir.string());
    targetHeader.set_parentindex(-1);
    targetHeader.set_isdirectory(true);
    items.push_back(target);

    pb::UploadItem fileItem;
    pb::UploadHeader &fileHeader = *fileItem.mutable_header();
    fileHeader.set_name("stale.bin");
    fileHeader.set_parentindex(0);
    fileHeader.set_filesize(data.size());
    fileHeader.set_resumefrom(500);
    fileHeader.set_startsequence(0);
    items.push_back(fileItem);

    pb::UploadReply reply;
    const grpc::Status status = upload(items, reply);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_NE(std::string::npos, status.error_message().find("stale.bin"));

    // The file was not opened, so the bytes it holds are the ones an earlier run left.
    EXPECT_EQ(readFile(partial), data);
}
} // namespace

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
