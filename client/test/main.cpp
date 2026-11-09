#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef _MSC_VER
// For _CrtDbgReport in CrashDump.aReportFromTheDebugCRTWritesADump.
#include <crtdbg.h>
#endif
#include "spdlog/spdlog.h"
#include "absl/crc/crc32c.h"
#include "gtest/gtest.h"
#include "FileBrowser.grpc.pb.h"
#include "FileBrowser.pb.h"
#include "DirectoryTree.h"
#include "CrashDump.h"
#include "Misc.h"
#include "ScanQueue.h"
#include "TransferQueue.h"
#include "Transfers.h"
#include "UiPaths.h"

TEST(TreeEntry, init)
{
    TreeEntry entry;
    EXPECT_EQ(0, entry.fileSize);
    EXPECT_EQ(0, entry.allocatedSize);
    EXPECT_EQ(0, entry.fileIndex);
    EXPECT_EQ(std::filesystem::path(""), entry.name);
    EXPECT_EQ(-1, entry.parentIndex);
    EXPECT_EQ(false, entry.isDirectory);
    EXPECT_EQ(false, entry.duplicate);
    EXPECT_EQ(0, entry.children.size());
}

TEST(DirectoryTree, init)
{
    DirectoryTree tree;
    EXPECT_EQ(std::filesystem::path(""), tree.getRootPath());
    EXPECT_EQ(0, tree.getEntryCount());
    EXPECT_EQ(-1, tree.getRootIndex());
}

// A node stores only its own name, so the path is rebuilt from the parent chain. This is what the UI uses when
// it needs the full path of a node it was given as an index.
TEST(DirectoryTree, pathIsRebuiltFromTheParentChain)
{
    DirectoryTree tree;
    ScanEntry entry;
    entry.filePath = "D:/Program Files/bin";
    entry.isDirectory = false;
    entry.fileSize = 1024;
    const int32_t index = tree.insertEntry(entry);

    EXPECT_EQ(std::filesystem::path("bin"), tree.entryAt(index).name);
    // generic_u8string so the comparison does not depend on which separator the join produced.
    EXPECT_EQ("D:/Program Files", tree.pathOf(tree.entryAt(index).parentIndex).generic_u8string());
    EXPECT_EQ("D:/Program Files/bin", tree.pathOf(index).generic_u8string());
}

// These two need a saved FileEntryCollection in the working directory (the add_test WORKING_DIRECTORY is the
// build directory, see README). They are skipped when it is not there. The checks are invariants of the tree
// rather than numbers tied to one data file, so a data file that is not the original still tests something.
TEST(FileEntryCollection, readFromLocalDisk)
{
    std::ifstream inputFile("android_files.bin", std::ios::binary);
    if (!inputFile.is_open())
    {
        GTEST_SKIP() << "android_files.bin is not in the working directory";
    }
    pb::FileEntryCollection fileEntryCollection;
    ASSERT_TRUE(fileEntryCollection.ParseFromIstream(&inputFile));
    inputFile.close();

    DirectoryTree tree;
    uint64_t countedSize = 0;
    std::unordered_set<uint64_t> countedFileIndices;
    for (const auto &item : fileEntryCollection.fileentrires())
    {
        const std::string filePath = item.filepath();
        ScanEntry entry;
        entry.fileSize = item.filesize();
        entry.allocatedSize = item.allocatedsize();
        entry.fileIndex = item.fileindex();
        try
        {
            // spdlog::trace("{}", filePath);
            const std::filesystem::path path = misc::utf8ToPath(filePath);
        }
        catch (const std::system_error &e)
        {
            throw std::runtime_error(fmt::format("{}, {}", filePath, e.what()));
        }
        entry.filePath = misc::utf8ToPath(filePath);
        entry.isDirectory = item.isdirectory();
        if (!entry.isDirectory && (entry.fileIndex == 0 || countedFileIndices.insert(entry.fileIndex).second))
        {
            countedSize += entry.fileSize;
        }
        tree.insertEntry(entry);
    }

    EXPECT_GT(fileEntryCollection.fileentrires_size(), 0);
    EXPECT_EQ(true, tree.rebuild());
    EXPECT_EQ(countedSize, tree.getTotalFileSize());
    EXPECT_GT(tree.getDirectoryCount(), 0u);
}

TEST(FileEntryCollection, rebuildHierarchy)
{
    std::ifstream inputFile("files.bin", std::ios::binary);
    if (!inputFile.is_open())
    {
        GTEST_SKIP() << "files.bin is not in the working directory";
    }
    pb::FileEntryCollection fileEntryCollection;
    ASSERT_TRUE(fileEntryCollection.ParseFromIstream(&inputFile));
    inputFile.close();

    DirectoryTree tree;
    uint64_t countedSize = 0;
    std::unordered_set<uint64_t> countedFileIndices;
    for (const auto &item : fileEntryCollection.fileentrires())
    {
        ScanEntry entry;
        entry.fileSize = item.filesize();
        entry.allocatedSize = item.allocatedsize();
        entry.fileIndex = item.fileindex();
        entry.filePath = misc::utf8ToPath(item.filepath());
        entry.isDirectory = item.isdirectory();
        if (!entry.isDirectory && (entry.fileIndex == 0 || countedFileIndices.insert(entry.fileIndex).second))
        {
            countedSize += entry.fileSize;
        }
        tree.insertEntry(entry);
    }

    EXPECT_GT(fileEntryCollection.fileentrires_size(), 0);
    EXPECT_EQ(true, tree.rebuild());
    EXPECT_EQ(countedSize, tree.getTotalFileSize());
}

TEST(Path, parent)
{
    std::filesystem::path child = "/root/bin";
    std::filesystem::path parent = "/root";
    EXPECT_EQ(child.parent_path(), parent);
}

TEST(FileEntryCollection, rebuild2)
{
    DirectoryTree tree;
    tree.insertEntry(ScanEntry{"/root/bin/bin2", false, 1024, 0, 0});
    tree.insertEntry(ScanEntry{"/root/bin", true, 0, 0, 0});
    tree.insertEntry(ScanEntry{"/root/bin1", false, 4096, 0, 0});
    tree.insertEntry(ScanEntry{"/root", true, 0, 0, 0});

    EXPECT_EQ(true, tree.rebuild());
    const int32_t root = tree.indexOf("/root");
    EXPECT_EQ(2u, tree.entryAt(root).children.size());
    EXPECT_EQ(4096 + 1024, tree.entryAt(root).fileSize);
    EXPECT_EQ(1u, tree.entryAt(tree.indexOf("/root/bin")).children.size());
    EXPECT_EQ(1024, tree.entryAt(tree.indexOf("/root/bin")).fileSize);
}

TEST(FileEntryCollection, rebuild3)
{
    DirectoryTree tree;
    tree.insertEntry(ScanEntry{"/ro ot/bin", false, 1024, 0, 0});
    tree.insertEntry(ScanEntry{"/ro ot/bin1", false, 4096, 0, 0});
    const int32_t root = tree.insertEntry(ScanEntry{"/ro ot", true, 0, 0, 0});

    EXPECT_EQ(true, tree.rebuild());
    EXPECT_EQ(2u, tree.entryAt(root).children.size());
    EXPECT_EQ(1024 + 4096, tree.entryAt(root).fileSize);
}

TEST(FileEntryCollection, rebuild4)
{
    DirectoryTree tree;
    tree.insertEntry(ScanEntry{"D:/Program Files/bin", false, 1024, 0, 0});
    tree.insertEntry(ScanEntry{"D:/Program Files/bi n1", false, 4096, 0, 0});
    const int32_t root = tree.insertEntry(ScanEntry{"D:/Program Files", true, 0, 0, 0});

    EXPECT_EQ(true, tree.rebuild());
    EXPECT_EQ(2u, tree.entryAt(root).children.size());
    EXPECT_EQ(1024 + 4096, tree.entryAt(root).fileSize);
}

TEST(FileEntryCollection, rebuild5)
{
    DirectoryTree tree;
    tree.insertEntry(ScanEntry{"D:/Program Files/bin", false, 1024, 0, 0});

    EXPECT_EQ(true, tree.rebuild());
    EXPECT_EQ(1u, tree.entryAt(tree.indexOf("D:/Program Files")).children.size());
    EXPECT_EQ(1u, tree.entryAt(tree.indexOf("D:/")).children.size());
    EXPECT_EQ(1024, tree.entryAt(tree.indexOf("D:/Program Files")).fileSize);
    EXPECT_EQ(1024, tree.entryAt(tree.indexOf("D:/")).fileSize);
}

TEST(DirectoryTree, folderSizesAreSummedFromTheEntriesBelowThem)
{
    // This is what recursiveDirectoryTraversal actually returns: directories with size 0, because the scanner
    // only has a size for a file in the record that listed it and writes 0 for a directory itself. Every folder
    // size below is therefore a client-side sum.
    DirectoryTree tree("D:/scan");
    tree.insertEntry(ScanEntry{"D:/scan/a.txt", false, 1024, 0, 0});
    tree.insertEntry(ScanEntry{"D:/scan/sub", true, 0, 0, 0});
    tree.insertEntry(ScanEntry{"D:/scan/sub/b.txt", false, 4096, 0, 0});
    tree.insertEntry(ScanEntry{"D:/scan/sub/c.txt", false, 2048, 0, 0});

    EXPECT_EQ(true, tree.rebuild());
    EXPECT_EQ(4096 + 2048, tree.entryAt(tree.indexOf("D:/scan/sub")).fileSize);
    EXPECT_EQ(1024 + 4096 + 2048, tree.entryAt(tree.indexOf("D:/scan")).fileSize);
    EXPECT_EQ(1024 + 4096 + 2048, tree.getTotalFileSize());
}

// insertEntry adds an entry's size to every node above it, so a total is readable while the traversal is still
// running. This is what the progress line shows; nothing has to be rebuilt to get it.
TEST(DirectoryTree, totalsAreCompleteBeforeTheTreeIsRebuilt)
{
    DirectoryTree tree("D:/scan");
    // A file below a directory the traversal has not reported yet: the parent node is created for it.
    const int32_t file = tree.insertEntry(ScanEntry{"D:/scan/sub/b.txt", false, 4096, 8192, 11});
    EXPECT_EQ(4096, tree.getTotalFileSize());
    EXPECT_EQ(8192, tree.getTotalAllocatedSize());
    EXPECT_EQ(4096, tree.entryAt(tree.indexOf("D:/scan/sub")).fileSize);

    tree.insertEntry(ScanEntry{"D:/scan/a.txt", false, 1024, 2048, 12});
    EXPECT_EQ(4096 + 1024, tree.getTotalFileSize());
    EXPECT_EQ(8192 + 2048, tree.getTotalAllocatedSize());
    EXPECT_EQ(tree.indexOf("D:/scan/sub"), tree.entryAt(file).parentIndex);
}

// Two paths with the same file index are the same file: its size counts once, on the path that was seen first.
// The row for the other path still shows its size, and is marked as the duplicate.
TEST(DirectoryTree, hardLinksAreCountedOnce)
{
    DirectoryTree tree("D:/scan");
    const int32_t first = tree.insertEntry(ScanEntry{"D:/scan/a.txt", false, 4096, 8192, 7});
    const int32_t second = tree.insertEntry(ScanEntry{"D:/scan/sub/b.txt", false, 4096, 8192, 7});

    EXPECT_FALSE(tree.entryAt(first).duplicate);
    EXPECT_TRUE(tree.entryAt(second).duplicate);
    EXPECT_EQ(4096, tree.entryAt(second).fileSize);
    EXPECT_EQ(4096, tree.getTotalFileSize());
    EXPECT_EQ(8192, tree.getTotalAllocatedSize());
    EXPECT_EQ(0u, tree.entryAt(tree.indexOf("D:/scan/sub")).fileSize);

    // An entry with no index cannot be recognised as a duplicate, so both of its sizes count.
    tree.insertEntry(ScanEntry{"D:/scan/c.txt", false, 1024, 2048, 0});
    tree.insertEntry(ScanEntry{"D:/scan/d.txt", false, 1024, 2048, 0});
    EXPECT_EQ(4096 + 1024 + 1024, tree.getTotalFileSize());
}

TEST(DirectoryTree, scanRootKeepsTheTreeInsideTheScannedDirectory)
{
    DirectoryTree tree("D:/scan");
    tree.insertEntry(ScanEntry{"D:/scan/sub/b.txt", false, 4096, 0, 0});
    tree.insertEntry(ScanEntry{"D:/scan/a.txt", false, 1024, 0, 0});

    // The traversal does not report the directory it was started on, so this tree has the scanned directory only
    // because the constructor created it as the root node. Nothing above it belongs to the scan, which is what
    // makes getTotalFileSize the size of the scanned directory rather than of the drive.
    EXPECT_EQ(true, tree.rebuild());
    EXPECT_EQ(std::filesystem::path("D:/scan"), tree.getScanRoot());
    EXPECT_EQ(std::filesystem::path("D:/scan"), tree.getRootPath());
    EXPECT_EQ(-1, tree.indexOf("D:/"));
    EXPECT_EQ(2u, tree.getFileCount());
    // Two directory nodes: the scanned directory, which the constructor created, and "sub", which insertEntry
    // created as the parent of the entries the traversal returned.
    EXPECT_EQ(2u, tree.getDirectoryCount());
    EXPECT_EQ(1024 + 4096, tree.getTotalFileSize());
    EXPECT_EQ(4096, tree.entryAt(tree.indexOf("D:/scan/sub")).fileSize);
}

TEST(DirectoryTree, scanRootOfAnEmptyDirectoryStillHasItsNode)
{
    DirectoryTree tree("D:/scan");
    EXPECT_EQ(true, tree.rebuild());
    EXPECT_EQ(1u, tree.getEntryCount());
    EXPECT_EQ(0u, tree.getTotalFileSize());
}

TEST(Misc, formatSize)
{
    EXPECT_EQ("1 bytes", misc::formatSize(1));
    EXPECT_EQ("1023 bytes", misc::formatSize(1023));
    EXPECT_EQ("1.000 kb", misc::formatSize(1024));
    EXPECT_EQ("1.000 mb", misc::formatSize(1024 * 1024));
    EXPECT_EQ("1.000 gb", misc::formatSize(1024 * 1024 * 1024));
}

TEST(Misc, formatDuration)
{
    EXPECT_EQ("0 s", misc::formatDuration(999));
    EXPECT_EQ("12 s", misc::formatDuration(12000));
    EXPECT_EQ("1 min 5 s", misc::formatDuration(65000));
    EXPECT_EQ("59 min 59 s", misc::formatDuration(3599000));
    EXPECT_EQ("1 h 1 min", misc::formatDuration(3660000));
    EXPECT_EQ("0 s", misc::formatDuration(-1));
}

TEST(Misc, getParentDirectories)
{
    const PathSet parents = misc::getParentDirectories(std::filesystem::path("D:/Program Files/A/B/C"));
    EXPECT_EQ(4, parents.size());
    EXPECT_TRUE(parents.find(std::filesystem::path("D:/")) != parents.end());
    EXPECT_TRUE(parents.find(std::filesystem::path("D:/Program Files")) != parents.end());
    EXPECT_TRUE(parents.find(std::filesystem::path("D:/Program Files/A")) != parents.end());
    EXPECT_TRUE(parents.find(std::filesystem::path("D:/Program Files/A/B")) != parents.end());
    EXPECT_FALSE(parents.find(std::filesystem::path("D:/Program Files/A/B/C")) != parents.end());
}

TEST(Misc, getParentDirectories1)
{
    const PathSet parents = misc::getParentDirectories(std::filesystem::path("D:/"));
    EXPECT_EQ(0, parents.size());
}

namespace
{
// The worker runs on its own thread, so a test has to wait for what it delivers. Polling with a deadline keeps
// a queue that never delivers from hanging the run. The tree is a unique_ptr: the helper moves the wanted result
// out, so the caller owns the tree from there and it is freed when that caller's result goes out of scope. The
// results the helper passes over are freed by the vector takeResults() returned.
ScanResult takeResultFor(ScanQueue &queue, int tabId, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (ScanResult &result : queue.takeResults())
        {
            if (result.tabId == tabId)
            {
                return std::move(result);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ScanResult();
}
} // namespace

TEST(UiPaths, isIllegalPathCharRejectsWhatWindowsForbidsInAName)
{
    // The input filter is asked one character at a time, so this is the set it answers with.
    EXPECT_TRUE(isIllegalPathChar('"'));
    EXPECT_TRUE(isIllegalPathChar('<'));
    EXPECT_TRUE(isIllegalPathChar('>'));
    EXPECT_TRUE(isIllegalPathChar('|'));
    EXPECT_TRUE(isIllegalPathChar('?'));
    EXPECT_TRUE(isIllegalPathChar('*'));
    EXPECT_TRUE(isIllegalPathChar(0x01)); // a control character
    EXPECT_TRUE(isIllegalPathChar(0x7F));

    // Separators, a space and anything above ASCII stay allowed: the font covers Chinese names. ':' is allowed
    // as well, because a character at a time cannot tell the drive colon from another one.
    EXPECT_FALSE(isIllegalPathChar('\\'));
    EXPECT_FALSE(isIllegalPathChar('/'));
    EXPECT_FALSE(isIllegalPathChar(':'));
    EXPECT_FALSE(isIllegalPathChar(' '));
}

TEST(Transfers, chainOfBuildsThePathTheStateRecordsForADroppedEntry)
{
    // The upload progress estimate looks a file up in the state by the path the producer will record it under: the
    // target directory, then the chain the producer builds from the dropped entry. The dropped entry is itself an
    // entry, so the chain starts with its name, which is why the controller passes its parent as the root.
    const std::filesystem::path drops = misc::utf8ToPath("D:/drops");
    EXPECT_EQ("a.txt", chainOf(misc::utf8ToPath("D:/drops/a.txt"), drops));
    EXPECT_EQ("dir", chainOf(misc::utf8ToPath("D:/drops/dir"), drops));
    EXPECT_EQ("dir/sub/a.txt", chainOf(misc::utf8ToPath("D:/drops/dir/sub/a.txt"), drops));
}

TEST(ScanQueue, theWorkerBuildsTheTreeAndHandsTheResultBack)
{
    ScanQueue queue;

    const bool queued = queue.enqueue(
        1, "localhost:7700", "D:/scan",
        [](grpc::ClientContext &context, const std::string &directory, const EntrySink &sink) -> grpc::Status {
            auto emit = [&sink](const std::string &path, uint64_t size, bool isDirectory) {
                ScanEntry entry;
                entry.filePath = misc::utf8ToPath(path);
                entry.fileSize = size;
                entry.isDirectory = isDirectory;
                return sink(entry);
            };

            // What a traversal actually reports: the contents, never the directory it
            // was started on, and 0 for each directory.
            emit("D:/scan/a.txt", 1024, false);
            emit("D:/scan/sub", 0, true);
            emit("D:/scan/sub/b.bin", 4096, false);
            return grpc::Status(grpc::StatusCode::OK, "");
        });
    EXPECT_TRUE(queued);

    const ScanResult result = takeResultFor(queue, 1, std::chrono::seconds(5));
    EXPECT_TRUE(result.status.ok());
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(3u, result.entriesRead);
    EXPECT_EQ(2u, result.fileCount);
    // The scanned directory, created by the constructor, plus "sub", created by insertEntry as a parent.
    EXPECT_EQ(2u, result.directoryCount);
    EXPECT_EQ(1024 + 4096, result.totalSize);
}

TEST(ScanQueue, aRunningScanPublishesProgressAndStopsWhenCancelled)
{
    ScanQueue queue;

    const bool queued = queue.enqueue(
        2, "localhost:7700", "D:/scan",
        [](grpc::ClientContext &context, const std::string &directory, const EntrySink &sink) -> grpc::Status {
            for (int i = 0; i < 100000; ++i)
            {
                ScanEntry entry;
                entry.filePath = misc::utf8ToPath("D:/scan/f" + std::to_string(i));
                entry.fileSize = 1;
                entry.isDirectory = false;
                // The queue cannot break a Read() that is already waiting; the sink
                // returning false is the part of the contract a source can act on.
                if (!sink(entry))
                {
                    return grpc::Status(grpc::StatusCode::CANCELLED, "cancelled");
                }
            }
            return grpc::Status(grpc::StatusCode::OK, "");
        });
    EXPECT_TRUE(queued);

    // The counters are published on an interval, so the test has to see one publish before cancelling. That is
    // the same number the UI shows while a scan is running.
    ScanProgress progress;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline &&
           !(queue.takeProgress(2, progress) && progress.entriesRead > 0))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(progress.entriesRead > 0);

    EXPECT_TRUE(queue.cancel(2));

    const ScanResult result = takeResultFor(queue, 2, std::chrono::seconds(5));
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(static_cast<int>(grpc::StatusCode::CANCELLED), static_cast<int>(result.status.error_code()));
    // The source stops at the first entry after the cancel, so it counted at least what was published.
    EXPECT_TRUE(result.entriesRead >= progress.entriesRead);
    // A cancelled scan still delivers what it managed to read, so a tree exists for it. The published progress
    // can be behind the final counts because it is written on an interval.
    ASSERT_NE(nullptr, result.tree);
    EXPECT_TRUE(result.fileCount >= progress.files);
}

TEST(ScanQueue, aJobCancelledWhileItIsStillQueuedNeverRunsItsSource)
{
    // The promise must outlive the queue: the worker is inside the first source while the queue is destroyed.
    std::promise<void> gate;
    std::future<void> blocker = gate.get_future();

    ScanQueue queue;

    const bool first = queue.enqueue(
        3, "localhost:7700", "D:/one",
        [&blocker](grpc::ClientContext &context, const std::string &directory, const EntrySink &sink) -> grpc::Status {
            // Stands in for a server that keeps streaming: the worker is busy here,
            // so the job behind it stays in the queue.
            blocker.wait();
            return grpc::Status(grpc::StatusCode::OK, "");
        });
    EXPECT_TRUE(first);

    // A progress entry exists only for a running job, so this is what proves the worker is inside the first one.
    ScanProgress progress;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !queue.takeProgress(3, progress))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(queue.takeProgress(3, progress));

    bool sourceRan = false;
    const bool second = queue.enqueue(4, "localhost:7700", "D:/two",
                                      [&sourceRan](grpc::ClientContext &context, const std::string &directory,
                                                   const EntrySink &sink) -> grpc::Status {
                                          sourceRan = true;
                                          return grpc::Status(grpc::StatusCode::OK, "");
                                      });
    EXPECT_TRUE(second);
    EXPECT_TRUE(queue.hasJob(4));

    EXPECT_TRUE(queue.cancel(4));

    // Let the worker reach the second job. It is already cancelled, so its source must not run.
    gate.set_value();

    const ScanResult result = takeResultFor(queue, 4, std::chrono::seconds(5));
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(0u, result.entriesRead);
    EXPECT_FALSE(sourceRan);
}

namespace
{
// The transfer tests write into the working directory (the build directory, see README) and remove it again.
void writeLocalFile(const std::filesystem::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
}

std::string readLocalFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return std::string();
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Same shape as takeResultFor: the worker runs on its own thread, so a test waits for what it delivers, and a
// queue that never delivers cannot hang the run.
TransferResult takeTransferResultFor(TransferQueue &queue, int tabId, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const TransferResult &result : queue.takeResults())
        {
            if (result.tabId == tabId)
            {
                return result;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return TransferResult();
}
} // namespace

// The stream gives each entry its own name and the position of its parent; the writer rebuilds the path from
// those two, with its own root standing in for the directory the download was started on.
TEST(LocalDownloadWriter, rebuildsPathsFromPositionsAndWritesFiles)
{
    std::atomic<bool> cancel{false};
    TransferCounters counters;
    const std::filesystem::path root = "download_writer";
    std::filesystem::remove_all(root);

    {
        LocalDownloadWriter writer(root, counters, cancel);

        pb::DownloadItem item;
        item.set_name("a.txt");
        item.set_parentindex(-1);
        item.set_isdirectory(false);
        item.set_filesize(4);
        ASSERT_TRUE(writer.write(item)); // header, position 0

        item.Clear();
        item.set_size(4);
        item.set_data("abcd");
        ASSERT_TRUE(writer.write(item)); // content of the file that header opened

        item.Clear();
        item.set_name("sub");
        item.set_parentindex(-1);
        item.set_isdirectory(true);
        ASSERT_TRUE(writer.write(item)); // position 1

        item.Clear();
        item.set_name("b.bin");
        item.set_parentindex(1); // the directory just written
        item.set_isdirectory(false);
        item.set_filesize(2);
        ASSERT_TRUE(writer.write(item)); // position 2

        item.Clear();
        item.set_size(2);
        item.set_data("xy");
        ASSERT_TRUE(writer.write(item));

        EXPECT_TRUE(writer.failure.ok());
    } // the writer closes the file it has open here, which is what flushes the last chunk

    EXPECT_EQ("abcd", readLocalFile(root / "a.txt"));
    EXPECT_TRUE(std::filesystem::is_directory(root / "sub"));
    EXPECT_EQ("xy", readLocalFile(root / "sub" / "b.bin"));

    EXPECT_EQ(3u, counters.entries);
    EXPECT_EQ(2u, counters.files);
    EXPECT_EQ(1u, counters.directories);
    EXPECT_EQ(0u, counters.skipped);
    EXPECT_EQ(6u, counters.bytes);

    std::filesystem::remove_all(root);
}

// A file that is already on disk with the size the entry says is not skipped on the size alone: the chunks are
// compared with what is there, and an entry nothing was written for is the one counted as skipped.
TEST(LocalDownloadWriter, anExistingFileWithMatchingContentIsSkippedWithoutWriting)
{
    std::atomic<bool> cancel{false};
    TransferCounters counters;
    const std::filesystem::path root = "download_match";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    writeLocalFile(root / "keep.txt", "old");

    {
        LocalDownloadWriter writer(root, counters, cancel);

        pb::DownloadItem item;
        item.set_name("keep.txt");
        item.set_parentindex(-1);
        item.set_isdirectory(false);
        item.set_filesize(3);
        ASSERT_TRUE(writer.write(item));

        item.Clear();
        item.set_offset(0);
        item.set_size(3);
        item.set_data("old");
        ASSERT_TRUE(writer.write(item));

        // The entry before this header is finished here, which is where keep.txt turns out to be the same.
        item.Clear();
        item.set_name("new.txt");
        item.set_parentindex(-1);
        item.set_isdirectory(false);
        item.set_filesize(2);
        ASSERT_TRUE(writer.write(item));

        item.Clear();
        item.set_offset(0);
        item.set_size(2);
        item.set_data("hi");
        ASSERT_TRUE(writer.write(item));
    }

    // What was on disk before the download is still there: the two contents were the same, so nothing was written.
    EXPECT_EQ("old", readLocalFile(root / "keep.txt"));
    EXPECT_EQ("hi", readLocalFile(root / "new.txt"));

    EXPECT_EQ(2u, counters.entries);
    // Only new.txt was written; keep.txt is the entry that matched.
    EXPECT_EQ(1u, counters.files);
    EXPECT_EQ(1u, counters.skipped);
    EXPECT_EQ(2u, counters.bytes);

    std::filesystem::remove_all(root);
}

TEST(LocalDownloadWriter, aCancelFlagStopsTheSinkBeforeTheNextMessage)
{
    std::atomic<bool> cancel{false};
    TransferCounters counters;
    const std::filesystem::path root = "download_cancel";

    {
        LocalDownloadWriter writer(root, counters, cancel);

        pb::DownloadItem item;
        item.set_name("one.txt");
        item.set_parentindex(-1);
        item.set_isdirectory(false);
        ASSERT_TRUE(writer.write(item));

        // The flag is what the queue sets; the sink is what reads it, because a Read() that is already waiting
        // cannot be broken from here.
        cancel = true;
        EXPECT_FALSE(writer.write(item));
    }

    std::filesystem::remove_all(root);
}

// The first message is the target directory, and its name is the whole path: client streaming has no request
// message, so that header is where the server learns which directory to write into.
// The directory a download starts on is not sent on the stream, so the folder named after it has to be the root
// the writer was given: entries whose parentIndex is -1 go inside it, and it is created by the constructor.
TEST(LocalDownloadWriter, theFolderNamedAfterTheStartDirectoryIsCreatedAndIsWhatMinusOnePointsAt)
{
    std::atomic<bool> cancel{false};
    TransferCounters counters;
    const std::filesystem::path root = "download_start/update";

    {
        LocalDownloadWriter writer(root, counters, cancel);

        pb::DownloadItem item;
        item.set_name("file.txt");
        item.set_parentindex(-1);
        item.set_isdirectory(false);
        ASSERT_TRUE(writer.write(item));

        item.Clear();
        item.set_size(3);
        item.set_data("abc");
        ASSERT_TRUE(writer.write(item));

        EXPECT_TRUE(writer.failure.ok());
    }

    EXPECT_TRUE(std::filesystem::is_directory(root));
    EXPECT_EQ("abc", readLocalFile(root / "file.txt"));
    // The folder was created but not counted: counters.directories counts directory headers, and this one was
    // never in the stream.
    EXPECT_EQ(0u, counters.directories);

    std::filesystem::remove_all("download_start");
}

TEST(LocalUploadProducer, firstMessageIsTheTargetDirectoryThenEntriesInTraversalOrder)
{
    std::atomic<bool> cancel{false};
    TransferCounters counters;
    const std::filesystem::path local = "upload_test";
    std::filesystem::remove_all(local);
    std::filesystem::create_directories(local / "sub");
    writeLocalFile(local / "sub" / "b.bin", "zy");

    LocalUploadProducer producer("/server/target", {local}, false, counters, cancel);

    pb::UploadItem item;
    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_header());
    EXPECT_EQ("/server/target", item.header().name());
    EXPECT_EQ(-1, item.header().parentindex());
    EXPECT_TRUE(item.header().isdirectory());

    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_header());
    EXPECT_EQ("upload_test", item.header().name());
    EXPECT_EQ(0, item.header().parentindex()); // the target, position 0
    EXPECT_TRUE(item.header().isdirectory());

    // Only one child, so the order is fixed and the parent index is the position the header above got.
    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_header());
    EXPECT_EQ("sub", item.header().name());
    EXPECT_EQ(1, item.header().parentindex());

    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_header());
    EXPECT_EQ("b.bin", item.header().name());
    EXPECT_EQ(2, item.header().parentindex());
    EXPECT_EQ(2u, item.header().filesize());
    EXPECT_TRUE(item.header().overwrite() == false);

    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_chunk());
    EXPECT_EQ(0u, item.chunk().sequence());
    EXPECT_EQ(0u, item.chunk().offset());
    EXPECT_EQ(2u, item.chunk().size());
    EXPECT_EQ("zy", item.chunk().data());

    // Nothing left: the stream is finished.
    EXPECT_FALSE(producer.next(item));

    EXPECT_EQ(4u, counters.entries);
    EXPECT_EQ(1u, counters.files);
    EXPECT_EQ(3u, counters.directories); // the target, the dropped folder, and sub
    EXPECT_EQ(2u, counters.bytes);

    std::filesystem::remove_all(local);
}

TEST(TransferQueue, runsTheRunnerAndHandsTheCountsBack)
{
    TransferQueue queue;

    std::shared_ptr<TransferJob> job = std::make_shared<TransferJob>();
    job->tabId = 1;
    job->key = "localhost:7700";
    job->kind = TransferKind::Upload;
    job->remotePath = "/server/target";
    job->localEntries.push_back(misc::utf8ToPath("upload_one"));
    job->startedAt = std::chrono::steady_clock::now();
    // No RPC here: the queue is what is under test, so the runner only reports what it "did".
    job->runner = [](grpc::ClientContext &context, const TransferJob &job, TransferCounters &counters,
                     const PublishProgress &publish) -> grpc::Status {
        counters.entries = 2;
        counters.files = 1;
        counters.bytes = 8;
        publish();
        return grpc::Status(grpc::StatusCode::OK, "");
    };

    ASSERT_TRUE(queue.enqueue(job));

    const TransferResult result = takeTransferResultFor(queue, 1, std::chrono::seconds(5));
    EXPECT_TRUE(result.status.ok());
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(2u, result.counters.entries);
    EXPECT_EQ(1u, result.counters.files);
    EXPECT_EQ(8u, result.counters.bytes);
    EXPECT_EQ("localhost:7700", result.key);
    EXPECT_EQ("/server/target", result.remotePath);
    EXPECT_TRUE(result.localRoot.empty());
}

TEST(TransferQueue, aRunningTransferPublishesProgressAndStopsWhenCancelled)
{
    TransferQueue queue;

    std::shared_ptr<TransferJob> job = std::make_shared<TransferJob>();
    job->tabId = 2;
    job->key = "localhost:7700";
    job->kind = TransferKind::Download;
    job->remotePath = "/server/dir";
    job->localRoot = misc::utf8ToPath("download_progress");
    job->startedAt = std::chrono::steady_clock::now();
    job->runner = [](grpc::ClientContext &context, const TransferJob &job, TransferCounters &counters,
                     const PublishProgress &publish) -> grpc::Status {
        for (int i = 0; i < 100000; ++i)
        {
            ++counters.bytes;
            if (counters.bytes % kTransferProgressPublish == 0)
            {
                publish();
            }
            // The runner has to notice the flag itself: nothing can break a call that is already inside the
            // stream, which is why TryCancel() on the context is paired with this check.
            if (job.cancelRequested)
            {
                return grpc::Status(grpc::StatusCode::CANCELLED, "cancelled");
            }
            // Stands in for the time a chunk takes. Without it the runner would deliver its result before the
            // test could see a published number.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return grpc::Status(grpc::StatusCode::OK, "");
    };

    ASSERT_TRUE(queue.enqueue(job));

    TransferProgress progress;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline &&
           !(queue.takeProgress(2, progress) && progress.counters.bytes > 0))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(progress.running);
    EXPECT_TRUE(progress.counters.bytes > 0);
    EXPECT_EQ("/server/dir", progress.remotePath);

    EXPECT_TRUE(queue.cancel(2));

    const TransferResult result = takeTransferResultFor(queue, 2, std::chrono::seconds(5));
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(static_cast<int>(grpc::StatusCode::CANCELLED), static_cast<int>(result.status.error_code()));
    // The published progress can be behind the final counts because it is written on an interval.
    EXPECT_TRUE(result.counters.bytes >= progress.counters.bytes);
}

TEST(TransferQueue, thePublishedRateSmoothsEachIntervalIntoThePreviousValue)
{
    // The gates are what let the test read one interval before the next one runs: the two intervals have different
    // shapes, and the number the test reads has to sit between them instead of becoming the newest one.
    std::promise<void> firstIntervalPublished;
    std::future<void> firstIntervalReady = firstIntervalPublished.get_future();
    std::promise<void> firstIntervalRead;
    std::future<void> readGate = firstIntervalRead.get_future();
    std::promise<void> secondIntervalPublished;
    std::future<void> secondIntervalReady = secondIntervalPublished.get_future();
    std::promise<void> secondIntervalRead;
    std::future<void> secondReadGate = secondIntervalRead.get_future();

    TransferQueue queue;

    std::shared_ptr<TransferJob> job = std::make_shared<TransferJob>();
    job->tabId = 11;
    job->key = "localhost:7700";
    job->kind = TransferKind::Download;
    job->remotePath = "/server/dir";
    job->localRoot = misc::utf8ToPath("download_rate");
    // What the line measures against: 1000 bytes for the transfer, 100 of them recorded as finished by an earlier
    // run, so the estimate is made from what this run has left.
    job->totalBytes = 1000;
    job->doneAtStart = 100;
    job->startedAt = std::chrono::steady_clock::now();
    job->runner = [&firstIntervalPublished, &readGate, &secondIntervalPublished,
                   &secondReadGate](grpc::ClientContext &context, const TransferJob &job, TransferCounters &counters,
                                    const PublishProgress &publish) -> grpc::Status {
        // The two intervals have to come out at different rates, so the difference is put in the bytes per sleep,
        // not in the length of the sleep: Windows rounds a short sleep up to the system timer resolution, and 2 ms
        // against 10 ms can take exactly the same time, which would make the two rates equal. Two bytes per sleep
        // against one byte per sleep keeps the second interval slower whatever the resolution turns out to be.
        for (int i = 0; i < 100; ++i)
        {
            counters.bytes += 2;
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        publish();
        firstIntervalPublished.set_value();
        readGate.wait();

        for (int i = 0; i < 100; ++i)
        {
            ++counters.bytes;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        publish();
        secondIntervalPublished.set_value();
        // The progress entry is gone once the transfer is delivered, so the runner waits here until the test has
        // read the number this publish wrote.
        secondReadGate.wait();
        return grpc::Status(grpc::StatusCode::OK, "");
    };

    ASSERT_TRUE(queue.enqueue(job));

    ASSERT_TRUE(firstIntervalReady.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    TransferProgress progress;
    EXPECT_TRUE(queue.takeProgress(11, progress));
    EXPECT_TRUE(progress.running);
    EXPECT_EQ(1000u, progress.totalBytes);
    EXPECT_EQ(100u, progress.doneAtStart);
    // The first interval is the whole rate so far: 200 bytes over at least 400 ms.
    const uint64_t firstRate = progress.bytesPerSecond;
    EXPECT_TRUE(firstRate > 0 && firstRate <= 500);

    const auto readStarted = std::chrono::steady_clock::now();
    firstIntervalRead.set_value();

    ASSERT_TRUE(secondIntervalReady.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const long long secondIntervalMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - readStarted).count();
    EXPECT_TRUE(queue.takeProgress(11, progress));

    // The measured time covers the gate wait as well as the sleeps, so it is an upper bound on how long the second
    // interval took, and the rate it gives is a lower bound on that interval's own rate. The smoothed number has to
    // sit above that and below the faster first rate.
    const uint64_t slowerIntervalRate = secondIntervalMs > 0 ? 100 * 1000 / static_cast<uint64_t>(secondIntervalMs) : 0;
    EXPECT_TRUE(progress.bytesPerSecond > slowerIntervalRate && progress.bytesPerSecond < firstRate);

    // The estimate is what the bytes this run has left, at the rate it has measured, would take.
    EXPECT_TRUE(progress.estimatedRemainingMs > 0);
    EXPECT_EQ(((1000 - 100 - progress.counters.bytes) * 1000) / progress.bytesPerSecond, progress.estimatedRemainingMs);

    secondIntervalRead.set_value();

    const TransferResult result = takeTransferResultFor(queue, 11, std::chrono::seconds(5));
    EXPECT_TRUE(result.status.ok());
}

TEST(TransferQueue, aJobCancelledWhileItIsStillQueuedNeverRunsItsRunner)
{
    // The promise must outlive the queue: the worker is inside the first runner while the queue is destroyed.
    std::promise<void> gate;
    std::future<void> blocker = gate.get_future();

    TransferQueue queue;

    std::shared_ptr<TransferJob> first = std::make_shared<TransferJob>();
    first->tabId = 3;
    first->key = "localhost:7700";
    first->kind = TransferKind::Download;
    first->remotePath = "/one";
    first->startedAt = std::chrono::steady_clock::now();
    first->runner = [&blocker](grpc::ClientContext &context, const TransferJob &job, TransferCounters &counters,
                               const PublishProgress &publish) -> grpc::Status {
        // Stands in for a server that keeps streaming: the worker is busy here, so the job behind it stays
        // in the queue.
        blocker.wait();
        return grpc::Status(grpc::StatusCode::OK, "");
    };
    ASSERT_TRUE(queue.enqueue(first));

    // A progress entry exists only for a running transfer, so this is what proves the worker is inside the
    // first runner.
    TransferProgress progress;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !queue.takeProgress(3, progress))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(queue.takeProgress(3, progress));

    bool runnerRan = false;
    std::shared_ptr<TransferJob> second = std::make_shared<TransferJob>();
    second->tabId = 4;
    second->key = "localhost:7700";
    second->kind = TransferKind::Upload;
    second->remotePath = "/two";
    second->startedAt = std::chrono::steady_clock::now();
    second->runner = [&runnerRan](grpc::ClientContext &context, const TransferJob &job, TransferCounters &counters,
                                  const PublishProgress &publish) -> grpc::Status {
        runnerRan = true;
        return grpc::Status(grpc::StatusCode::OK, "");
    };
    ASSERT_TRUE(queue.enqueue(second));
    EXPECT_TRUE(queue.hasJob(4));

    EXPECT_TRUE(queue.cancel(4));

    // Let the worker reach the second job. It is already cancelled, so its runner must not run.
    gate.set_value();

    const TransferResult result = takeTransferResultFor(queue, 4, std::chrono::seconds(5));
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(0u, result.counters.entries);
    EXPECT_FALSE(runnerRan);
}

// A single file download has no header of its own: the runner builds one and hands it to the writer before the
// RPC. sink() returning false on that header can mean two things - the cancel flag, or the local side refusing
// the file - and the status has to say which. The channel is never used here, because the transfer ends at the
// header, before the call that would need it.
TEST(DownloadRunner, aLocalFailureAtTheHeaderIsReportedInsteadOfTheCancelText)
{
    const std::shared_ptr<grpc::Channel> channel =
        grpc::CreateChannel("localhost:7700", grpc::InsecureChannelCredentials());
    const std::shared_ptr<FileBrowserClient> client = std::make_shared<FileBrowserClient>(channel);

    TransferQueue queue;
    std::shared_ptr<TransferJob> job = std::make_shared<TransferJob>();
    job->tabId = 5;
    job->key = "localhost:7700";
    job->kind = TransferKind::Download;
    // The leaf name of the remote path is what the writer turns into the file it opens. An asterisk cannot be
    // opened on Windows, so this transfer fails on the local side, not on the stream.
    job->remotePath = "/server/a*b.png";
    job->remoteIsDirectory = false;
    job->remoteFileSize = 0;
    job->localRoot = misc::utf8ToPath("download_report");
    job->startedAt = std::chrono::steady_clock::now();
    job->runner = makeDownloadRunner(client);

    ASSERT_TRUE(queue.enqueue(job));

    const TransferResult result = takeTransferResultFor(queue, 5, std::chrono::seconds(5));
    EXPECT_FALSE(result.cancelled);
    ASSERT_FALSE(result.status.ok());
    EXPECT_EQ(static_cast<int>(grpc::StatusCode::INTERNAL), static_cast<int>(result.status.error_code()));
    EXPECT_NE(std::string::npos, result.status.error_message().find("could not open"));

    std::filesystem::remove_all("download_report");
}

// The state file is the only thing that carries a transfer across a restart, so what it writes has to read back the
// same way: the same transfer, the same entries, and a name that needed escaping.
TEST(TransferStateFile, savesAndReadsBackWhatItRecorded)
{
    const std::filesystem::path path = "transfer_state_test.json";
    std::filesystem::remove(path);

    TransferStateFile state(path);
    // The file is not there yet, which is how the first run of a transfer looks.
    EXPECT_FALSE(state.load());

    StateEntry entry{2, "a\"b.bin", 0, false, 100, 40, 1, "/download/a\"b.bin"};
    state.record(TransferKind::Download, "localhost:7700", "/download", entry);
    ASSERT_TRUE(state.save());

    TransferStateFile reloaded(path);
    ASSERT_TRUE(reloaded.load());
    const StateTransfer found = reloaded.find(TransferKind::Download, "localhost:7700", "/download");
    ASSERT_EQ(1u, found.entries.size());
    EXPECT_EQ("a\"b.bin", found.entries[0].name);
    EXPECT_EQ(2, found.entries[0].position);
    EXPECT_EQ(40u, found.entries[0].bytesWritten);
    EXPECT_EQ(1u, found.entries[0].chunksSent);

    const StateEntry *looked = found.findEntry("/download/a\"b.bin");
    ASSERT_TRUE(looked != nullptr);
    EXPECT_EQ(found.entries[0].path, looked->path);

    // The same path recorded again updates what is stored instead of appending a second entry.
    entry.bytesWritten = 100;
    entry.chunksSent = 2;
    state.record(TransferKind::Download, "localhost:7700", "/download", entry);
    const StateTransfer same = state.find(TransferKind::Download, "localhost:7700", "/download");
    ASSERT_EQ(1u, same.entries.size());
    EXPECT_EQ(100u, same.entries[0].bytesWritten);
    EXPECT_EQ(2u, same.entries[0].chunksSent);

    // Another folder is another transfer.
    EXPECT_EQ(0u, state.find(TransferKind::Download, "localhost:7700", "/other").entries.size());

    // A file that cannot be parsed is read as no state at all, not as an error that stops the transfer.
    std::ofstream broken(path, std::ios::binary | std::ios::trunc);
    broken << "{ \"transfers\": [ { \"kind\": \"download\" ";
    broken.close();
    EXPECT_FALSE(state.load());

    std::filesystem::remove(path);
}

// A file that is there but smaller than the entry is the one an interrupted run stopped in, so the chunks after its
// header continue the file instead of starting it over.
// A file an interrupted run stopped in is not started over: what is there is compared, and only what comes past it
// is written. Its prefix is checked as well, so a half file whose content is not the same is corrected from where
// it first differs.
TEST(LocalDownloadWriter, aPartialLocalFileIsFilledInFromWhatArrives)
{
    std::atomic<bool> cancel{false};
    TransferCounters counters;
    const std::filesystem::path root = "download_partial";
    std::filesystem::remove_all(root);
    // The folder has to be there before a file can be written into it, which is what the earlier run is meant to
    // have left behind.
    std::filesystem::create_directories(root);
    writeLocalFile(root / "part.bin", "ab");

    {
        LocalDownloadWriter writer(root, counters, cancel);

        pb::DownloadItem item;
        item.set_name("part.bin");
        item.set_parentindex(-1);
        item.set_isdirectory(false);
        item.set_filesize(6);
        ASSERT_TRUE(writer.write(item));

        item.Clear();
        item.set_offset(0);
        item.set_size(2);
        item.set_data("ab");
        ASSERT_TRUE(writer.write(item)); // the same as what is already there, so nothing is written

        item.Clear();
        item.set_offset(2);
        item.set_size(4);
        item.set_data("cdef");
        ASSERT_TRUE(writer.write(item)); // past what the file holds, so this part is written
    }

    EXPECT_EQ("abcdef", readLocalFile(root / "part.bin"));
    EXPECT_EQ(1u, counters.entries);
    EXPECT_EQ(1u, counters.files);
    EXPECT_EQ(0u, counters.skipped);
    EXPECT_EQ(4u, counters.bytes);

    std::filesystem::remove_all(root);
}

// The comparison is what decides, not the size: the first chunk is the same as what is on disk, the difference
// starts in the second one, and only that part is written.
TEST(LocalDownloadWriter, anExistingFileWithDifferentContentIsWrittenFromTheFirstDifference)
{
    std::atomic<bool> cancel{false};
    TransferCounters counters;
    const std::filesystem::path root = "download_diff";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    writeLocalFile(root / "diff.txt", "abxy");

    {
        LocalDownloadWriter writer(root, counters, cancel);

        pb::DownloadItem item;
        item.set_name("diff.txt");
        item.set_parentindex(-1);
        item.set_isdirectory(false);
        item.set_filesize(4);
        ASSERT_TRUE(writer.write(item));

        item.Clear();
        item.set_offset(0);
        item.set_size(2);
        item.set_data("ab");
        ASSERT_TRUE(writer.write(item));

        item.Clear();
        item.set_offset(2);
        item.set_size(2);
        item.set_data("cd");
        ASSERT_TRUE(writer.write(item));
    }

    EXPECT_EQ("abcd", readLocalFile(root / "diff.txt"));
    EXPECT_EQ(1u, counters.entries);
    // The entry counts as written because something in it differed, and only the differing part counts as bytes.
    EXPECT_EQ(1u, counters.files);
    EXPECT_EQ(0u, counters.skipped);
    EXPECT_EQ(2u, counters.bytes);

    std::filesystem::remove_all(root);
}

// A file bigger than the entry cannot be what this download is meant to produce: the bytes past the entry are not
// part of it, so the entry is written over the file in full.
TEST(LocalDownloadWriter, aLocalFileBiggerThanTheEntryIsWrittenOverInFull)
{
    std::atomic<bool> cancel{false};
    TransferCounters counters;
    const std::filesystem::path root = "download_bigger";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    writeLocalFile(root / "big.bin", "abcde");

    {
        LocalDownloadWriter writer(root, counters, cancel);

        pb::DownloadItem item;
        item.set_name("big.bin");
        item.set_parentindex(-1);
        item.set_isdirectory(false);
        item.set_filesize(3);
        ASSERT_TRUE(writer.write(item));

        item.Clear();
        item.set_offset(0);
        item.set_size(3);
        item.set_data("xyz");
        ASSERT_TRUE(writer.write(item));
    }

    EXPECT_EQ("xyz", readLocalFile(root / "big.bin"));
    EXPECT_EQ(1u, counters.entries);
    EXPECT_EQ(1u, counters.files);
    EXPECT_EQ(0u, counters.skipped);
    EXPECT_EQ(3u, counters.bytes);

    std::filesystem::remove_all(root);
}

// What a download run writes to the state as it goes: the entries it accounted for, under the paths it rebuilt from
// the parent chain. For a download the state decides nothing - every entry is streamed and compared either way -
// but this is what the file shows afterwards.
TEST(LocalDownloadWriter, theEntriesItAccountedForAreRecordedUnderThePathsItRebuilt)
{
    const std::filesystem::path path = "transfer_state_download_run.json";
    std::filesystem::remove(path);

    const std::string rootUtf8 = "download_record";
    const std::filesystem::path root = rootUtf8;
    std::filesystem::remove_all(root);

    TransferStateFile state(path);
    const StateContext context{&state, TransferKind::Download, "localhost:7700", rootUtf8,
                               state.find(TransferKind::Download, "localhost:7700", rootUtf8)};

    std::atomic<bool> cancel{false};
    TransferCounters counters;

    {
        LocalDownloadWriter writer(root, counters, cancel, context);

        pb::DownloadItem item;
        item.set_name("sub");
        item.set_parentindex(-1);
        item.set_isdirectory(true);
        ASSERT_TRUE(writer.write(item));

        item.Clear();
        item.set_name("keep.bin");
        item.set_parentindex(0);
        item.set_isdirectory(false);
        item.set_filesize(2);
        ASSERT_TRUE(writer.write(item));

        item.Clear();
        item.set_offset(0);
        item.set_size(2);
        item.set_data("xy");
        ASSERT_TRUE(writer.write(item));
    }

    const StateTransfer recorded = state.find(TransferKind::Download, "localhost:7700", rootUtf8);
    ASSERT_EQ(2u, recorded.entries.size());
    EXPECT_EQ(rootUtf8 + "/sub", recorded.entries[0].path);
    EXPECT_EQ(rootUtf8 + "/sub/keep.bin", recorded.entries[1].path);
    // bytesWritten is what the file holds, which for this entry is the chunk this run wrote.
    EXPECT_EQ(2u, recorded.entries[1].bytesWritten);

    std::filesystem::remove_all(root);
    std::filesystem::remove(path);
}

// The hash a run records covers the content it saw, compared or written either way. That is what a later run can
// decide from without taking those bytes from the server again.
TEST(LocalDownloadWriter, theHashItRecordsCoversTheContentItSaw)
{
    const std::filesystem::path path = "transfer_state_download_hash.json";
    std::filesystem::remove(path);

    const std::string rootUtf8 = "download_hash";
    const std::filesystem::path root = rootUtf8;
    std::filesystem::remove_all(root);

    TransferStateFile state(path);
    const StateContext context{&state, TransferKind::Download, "localhost:7700", rootUtf8,
                               state.find(TransferKind::Download, "localhost:7700", rootUtf8)};

    std::atomic<bool> cancel{false};
    TransferCounters counters;

    {
        LocalDownloadWriter writer(root, counters, cancel, context);

        pb::DownloadItem item;
        item.set_name("a.bin");
        item.set_parentindex(-1);
        item.set_isdirectory(false);
        item.set_filesize(6);
        ASSERT_TRUE(writer.write(item));

        item.Clear();
        item.set_offset(0);
        item.set_size(3);
        item.set_data("abc");
        ASSERT_TRUE(writer.write(item));

        // The part that differs is the part written, but the hash covers the whole chunk.
        item.Clear();
        item.set_offset(3);
        item.set_size(3);
        item.set_data("xyz");
        ASSERT_TRUE(writer.write(item));
    }

    const StateTransfer recorded = state.find(TransferKind::Download, "localhost:7700", rootUtf8);
    ASSERT_EQ(1u, recorded.entries.size());
    EXPECT_EQ(6u, recorded.entries[0].hashedBytes);
    // The file now holds the content this run saw, so the recorded hash is the hash of the file.
    EXPECT_EQ(hashPrefix(root / "a.bin", 6), recorded.entries[0].contentHash);
    EXPECT_EQ(1u, counters.files);

    std::filesystem::remove_all(root);
    std::filesystem::remove(path);
}

// Hashing a file in pieces gives the same value as hashing it at once, which is what lets a run hash a prefix it
// already proved and carry on from there.
TEST(hashPrefix, theSameBytesGiveTheSameValueInPieces)
{
    const std::filesystem::path path = "hash_prefix.bin";
    writeLocalFile(path, "abcdef");

    const std::optional<uint32_t> whole = hashPrefix(path, 6);
    ASSERT_TRUE(whole.has_value());
    EXPECT_EQ(whole, hashPrefix(path, 6));

    const std::optional<uint32_t> first = hashPrefix(path, 3);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*whole, static_cast<uint32_t>(absl::ExtendCrc32c(absl::crc32c_t{*first}, absl::string_view("def", 3))));

    // Fewer bytes on disk than the hash claims: there is nothing to prove.
    EXPECT_FALSE(hashPrefix(path, 7).has_value());

    std::filesystem::remove(path);
}

// The check reports what it reads on the interval the progress uses, so a run that has to prove a large prefix shows
// up on the UI instead of looking like it is waiting for the server.
TEST(hashPrefix, reportsWhatItReadsOnThePublishInterval)
{
    const std::filesystem::path path = "hash_prefix_report.bin";
    const std::size_t chunks = kTransferProgressPublish + 1;
    const std::string content(chunks * kTransferChunkSize, 'a');
    writeLocalFile(path, content);

    std::vector<uint64_t> reports;
    hashPrefix(path, content.size(), [&](uint64_t checked) { reports.push_back(checked); });

    // One report for the first kTransferProgressPublish pieces and one for the piece left over, and the totals add up
    // to what was read.
    ASSERT_EQ(2u, reports.size());
    EXPECT_EQ(kTransferChunkSize * kTransferProgressPublish, reports[0]);
    EXPECT_EQ(kTransferChunkSize, reports[1]);
    uint64_t total = 0;
    for (uint64_t checked : reports)
    {
        total += checked;
    }
    EXPECT_EQ(content.size(), total);

    // A file that cannot be read reports nothing: there is nothing there to show.
    std::vector<uint64_t> none;
    EXPECT_FALSE(
        hashPrefix("hash_prefix_report_absent.bin", 1, [&](uint64_t checked) { none.push_back(checked); }).has_value());
    EXPECT_TRUE(none.empty());

    std::filesystem::remove(path);
}

// What the plan makes of a state that still matches what is on disk: an entry whose hash covers the whole file is
// left alone, and the stream is not asked for anything when every entry is like that.
TEST(DownloadPlan, entriesWhoseRecordedHashStillMatchesTheDiskAreLeftAlone)
{
    const std::filesystem::path path = "transfer_state_plan_verified.json";
    std::filesystem::remove(path);

    const std::string rootUtf8 = "plan_verified";
    const std::filesystem::path root = rootUtf8;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "sub");
    writeLocalFile(root / "sub" / "a.bin", "abcdef");

    TransferStateFile state(path);
    const StateEntry dir{0, "sub", -1, true, 0, 0, 0, rootUtf8 + "/sub"};
    const StateEntry file{
        1, "a.bin", 0, false, 6, 6, 1, rootUtf8 + "/sub/a.bin", 6, hashPrefix(root / "sub" / "a.bin", 6).value_or(0)};
    state.record(TransferKind::Download, "localhost:7700", rootUtf8, dir);
    state.record(TransferKind::Download, "localhost:7700", rootUtf8, file);

    const StateContext context{&state, TransferKind::Download, "localhost:7700", rootUtf8,
                               state.find(TransferKind::Download, "localhost:7700", rootUtf8)};

    // The traversal this run got came out in the order the state recorded.
    const std::vector<std::string> order{rootUtf8 + "/sub", rootUtf8 + "/sub/a.bin"};
    const DownloadPlan plan = planDownload(context, order);

    EXPECT_TRUE(plan.nothingToFetch);
    EXPECT_EQ(2u, plan.left.size());
    // Only the file counts as verified: nothing had to be proven about the directory, and it was not made by this
    // run either.
    EXPECT_EQ(1u, plan.verifiedFiles);
    // startEntryIndex says nothing here: with every entry proven there is no stream to start.
    EXPECT_EQ(0u, plan.startEntryIndex);

    std::filesystem::remove_all(root);
    std::filesystem::remove(path);
}

// A hash that covers only part of an entry proves that part, so the stream starts at the byte it covers and the
// writer carries the hash on from the value the state has.
TEST(DownloadPlan, aProvenPrefixStartsTheStreamAtTheByteTheHashCovers)
{
    const std::filesystem::path path = "transfer_state_plan_prefix.json";
    std::filesystem::remove(path);

    const std::string rootUtf8 = "plan_prefix";
    const std::filesystem::path root = rootUtf8;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    writeLocalFile(root / "a.bin", "abcdef");

    TransferStateFile state(path);
    const StateEntry part{
        0, "a.bin", -1, false, 6, 2, 1, rootUtf8 + "/a.bin", 2, hashPrefix(root / "a.bin", 2).value_or(0)};
    state.record(TransferKind::Download, "localhost:7700", rootUtf8, part);

    const StateContext context{&state, TransferKind::Download, "localhost:7700", rootUtf8,
                               state.find(TransferKind::Download, "localhost:7700", rootUtf8)};

    const DownloadPlan plan = planDownload(context, {rootUtf8 + "/a.bin"});

    EXPECT_FALSE(plan.nothingToFetch);
    EXPECT_FALSE(plan.rewriteStartEntry);
    EXPECT_EQ(0u, plan.startEntryIndex);
    EXPECT_EQ(2u, plan.startOffset);
    EXPECT_EQ(part.contentHash, plan.startHash);

    std::filesystem::remove_all(root);
    std::filesystem::remove(path);
}

// What a hash cannot prove is not kept: the bytes on disk are not what an earlier run hashed, so the entry comes
// back from its first byte and is written over.
TEST(DownloadPlan, aRecordedHashThatDoesNotMatchTheDiskRewritesTheEntryInFull)
{
    const std::filesystem::path path = "transfer_state_plan_mismatch.json";
    std::filesystem::remove(path);

    const std::string rootUtf8 = "plan_mismatch";
    const std::filesystem::path root = rootUtf8;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    writeLocalFile(root / "a.bin", "abcdef");

    TransferStateFile state(path);
    const StateEntry stale{0, "a.bin", -1, false, 6, 2, 1, rootUtf8 + "/a.bin", 2, 0xdeadbeef};
    state.record(TransferKind::Download, "localhost:7700", rootUtf8, stale);

    const StateContext context{&state, TransferKind::Download, "localhost:7700", rootUtf8,
                               state.find(TransferKind::Download, "localhost:7700", rootUtf8)};

    const DownloadPlan plan = planDownload(context, {rootUtf8 + "/a.bin"});

    EXPECT_TRUE(plan.rewriteStartEntry);
    EXPECT_EQ(0u, plan.startOffset);
    EXPECT_EQ(0u, plan.verifiedFiles);

    std::filesystem::remove_all(root);
    std::filesystem::remove(path);
}

// planDownload hands the report on to the check, so what a check reads shows up while the plan is being made instead
// of after it.
TEST(DownloadPlan, reportsWhatTheCheckReads)
{
    const std::filesystem::path path = "transfer_state_plan_report.json";
    std::filesystem::remove(path);

    const std::string rootUtf8 = "plan_report";
    const std::filesystem::path root = rootUtf8;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    writeLocalFile(root / "a.bin", "abcdef");

    TransferStateFile state(path);
    const StateEntry part{
        0, "a.bin", -1, false, 6, 2, 1, rootUtf8 + "/a.bin", 2, hashPrefix(root / "a.bin", 2).value_or(0)};
    state.record(TransferKind::Download, "localhost:7700", rootUtf8, part);

    const StateContext context{&state, TransferKind::Download, "localhost:7700", rootUtf8,
                               state.find(TransferKind::Download, "localhost:7700", rootUtf8)};

    uint64_t checked = 0;
    const DownloadPlan plan = planDownload(context, {rootUtf8 + "/a.bin"}, [&](uint64_t bytes) { checked += bytes; });

    EXPECT_EQ(2u, checked);
    EXPECT_EQ(1u, plan.verifiedFiles);

    std::filesystem::remove_all(root);
    std::filesystem::remove(path);
}

// Entries are matched by path, but startEntryIndex is a position: an order that is not the one the state recorded
// skips nothing, and the run compares every entry as it always did.
TEST(DownloadPlan, anOrderThatIsNotTheOneTheStateRecordedSkipsNothing)
{
    const std::filesystem::path path = "transfer_state_plan_order.json";
    std::filesystem::remove(path);

    const std::string rootUtf8 = "plan_order";
    const std::filesystem::path root = rootUtf8;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    writeLocalFile(root / "b.bin", "ab");
    writeLocalFile(root / "a.bin", "cd");

    TransferStateFile state(path);
    const StateEntry first{
        0, "b.bin", -1, false, 2, 2, 1, rootUtf8 + "/b.bin", 2, hashPrefix(root / "b.bin", 2).value_or(0)};
    const StateEntry second{
        1, "a.bin", -1, false, 2, 2, 1, rootUtf8 + "/a.bin", 2, hashPrefix(root / "a.bin", 2).value_or(0)};
    state.record(TransferKind::Download, "localhost:7700", rootUtf8, first);
    state.record(TransferKind::Download, "localhost:7700", rootUtf8, second);

    const StateContext context{&state, TransferKind::Download, "localhost:7700", rootUtf8,
                               state.find(TransferKind::Download, "localhost:7700", rootUtf8)};

    // The same two files, listed the other way round.
    const DownloadPlan plan = planDownload(context, {rootUtf8 + "/a.bin", rootUtf8 + "/b.bin"});

    EXPECT_FALSE(plan.nothingToFetch);
    EXPECT_EQ(0u, plan.left.size());
    EXPECT_EQ(0u, plan.startEntryIndex);
    EXPECT_EQ(0u, plan.verifiedFiles);

    std::filesystem::remove_all(root);
    std::filesystem::remove(path);
}

// An entry the plan left alone never arrives as a header, so its path has to be in the writer's tables before the
// first header that points at it.
TEST(LocalDownloadWriter, theEntriesThePlanLeftAloneAreInItsPathTables)
{
    const std::filesystem::path path = "transfer_state_plan_writer.json";
    std::filesystem::remove(path);

    const std::string rootUtf8 = "plan_writer";
    const std::filesystem::path root = rootUtf8;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "sub");
    writeLocalFile(root / "sub" / "a.bin", "ab");

    TransferStateFile state(path);
    const StateEntry dir{0, "sub", -1, true, 0, 0, 0, rootUtf8 + "/sub"};
    const StateEntry file{
        1, "a.bin", 0, false, 2, 2, 1, rootUtf8 + "/sub/a.bin", 2, hashPrefix(root / "sub" / "a.bin", 2).value_or(0)};
    state.record(TransferKind::Download, "localhost:7700", rootUtf8, dir);
    state.record(TransferKind::Download, "localhost:7700", rootUtf8, file);

    const StateContext context{&state, TransferKind::Download, "localhost:7700", rootUtf8,
                               state.find(TransferKind::Download, "localhost:7700", rootUtf8)};

    const DownloadPlan plan = planDownload(context, {rootUtf8 + "/sub", rootUtf8 + "/sub/a.bin"});
    ASSERT_TRUE(plan.nothingToFetch);

    std::atomic<bool> cancel{false};
    TransferCounters counters;
    {
        LocalDownloadWriter writer(root, counters, cancel, context, plan);
        EXPECT_EQ(1u, counters.verified);

        // A file this run has not seen before, hanging on a directory the plan left alone. Its parentIndex is that
        // directory's position, so the path only comes out right if the plan filled the table.
        pb::DownloadItem item;
        item.set_name("new.bin");
        item.set_parentindex(0);
        item.set_isdirectory(false);
        item.set_filesize(2);
        ASSERT_TRUE(writer.write(item));

        item.Clear();
        item.set_offset(0);
        item.set_size(2);
        item.set_data("cd");
        ASSERT_TRUE(writer.write(item));
    }

    EXPECT_EQ("cd", readLocalFile(root / "sub" / "new.bin"));
    EXPECT_EQ(1u, counters.entries);
    EXPECT_EQ(1u, counters.files);
    EXPECT_EQ(1u, counters.verified);

    std::filesystem::remove_all(root);
    std::filesystem::remove(path);
}

// skipped. The header still goes out because the entries after it count their positions against it.
TEST(LocalUploadProducer, anEntryTheStateRecordsAsCompleteIsSentAsHeaderOnly)
{
    const std::filesystem::path path = "transfer_state_upload_complete.json";
    std::filesystem::remove(path);

    const std::string target = "/server/target";
    const std::filesystem::path local = "upload_complete";
    std::filesystem::remove_all(local);
    std::filesystem::create_directories(local);
    writeLocalFile(local / "a.bin", "ab");
    writeLocalFile(local / "b.bin", "cd");

    TransferStateFile state(path);
    const StateEntry done{1, "a.bin", 0, false, 2, 2, 1, target + "/a.bin"};
    state.record(TransferKind::Upload, "localhost:7700", target, done);

    const StateContext context{&state, TransferKind::Upload, "localhost:7700", target,
                               state.find(TransferKind::Upload, "localhost:7700", target)};

    std::atomic<bool> cancel{false};
    TransferCounters counters;
    LocalUploadProducer producer(target, {local / "a.bin", local / "b.bin"}, false, counters, cancel, context);

    // Which entry comes first is the order the folder was read in, so the messages are collected by name instead of
    // asserted in a fixed order. The target header is the first message either way.
    std::unordered_map<std::string, pb::UploadItem> headers;
    std::vector<pb::UploadItem> chunks;
    pb::UploadItem item;

    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_header());
    EXPECT_EQ(target, item.header().name());
    headers[item.header().name()] = item;

    while (producer.next(item))
    {
        if (item.has_header())
        {
            headers[item.header().name()] = item;
        }
        else
        {
            chunks.push_back(item);
        }
    }

    ASSERT_EQ(3u, headers.size()); // the target plus the two dropped files
    EXPECT_TRUE(headers["a.bin"].header().alreadycomplete());
    EXPECT_FALSE(headers["b.bin"].header().alreadycomplete());
    EXPECT_FALSE(headers["a.bin"].header().overwrite());

    // Only the entry that was not recorded as complete has content following it.
    ASSERT_EQ(1u, chunks.size());
    EXPECT_EQ(0u, chunks[0].chunk().sequence());
    EXPECT_EQ("cd", chunks[0].chunk().data());

    EXPECT_EQ(3u, counters.entries);
    EXPECT_EQ(1u, counters.files);
    EXPECT_EQ(1u, counters.directories);
    EXPECT_EQ(1u, counters.skipped);
    EXPECT_EQ(2u, counters.bytes);

    std::filesystem::remove_all(local);
    std::filesystem::remove(path);
}

// A partial entry: the state says the server has these bytes, so the local file is read from there and the first
// chunk carries the sequence the state remembered, which is what the server checks it against.
TEST(LocalUploadProducer, aPartialEntryStartsAtTheOffsetAndSequenceTheStateRecords)
{
    const std::filesystem::path path = "transfer_state_upload_partial.json";
    std::filesystem::remove(path);

    const std::string target = "/server/target";
    const std::filesystem::path local = "upload_partial";
    std::filesystem::remove_all(local);
    std::filesystem::create_directories(local);
    writeLocalFile(local / "a.bin", "abcdef");

    TransferStateFile state(path);
    const StateEntry part{1, "a.bin", 0, false, 6, 2, 1, target + "/a.bin"};
    state.record(TransferKind::Upload, "localhost:7700", target, part);

    const StateContext context{&state, TransferKind::Upload, "localhost:7700", target,
                               state.find(TransferKind::Upload, "localhost:7700", target)};

    std::atomic<bool> cancel{false};
    TransferCounters counters;
    // The file itself is dropped, so it is the entry after the target header and the order is fixed.
    LocalUploadProducer producer(target, {local / "a.bin"}, false, counters, cancel, context);

    pb::UploadItem item;
    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_header());
    EXPECT_EQ(target, item.header().name());

    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_header());
    EXPECT_EQ("a.bin", item.header().name());
    EXPECT_EQ(0, item.header().parentindex());
    EXPECT_EQ(2u, item.header().resumefrom());
    EXPECT_EQ(1u, item.header().startsequence());

    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_chunk());
    EXPECT_EQ(1u, item.chunk().sequence());
    EXPECT_EQ(2u, item.chunk().offset());
    EXPECT_EQ(4u, item.chunk().size());
    EXPECT_EQ("cdef", item.chunk().data());

    EXPECT_FALSE(producer.next(item));

    EXPECT_EQ(2u, counters.entries);
    EXPECT_EQ(1u, counters.files);
    EXPECT_EQ(1u, counters.directories);
    EXPECT_EQ(0u, counters.skipped);
    EXPECT_EQ(4u, counters.bytes);

    std::filesystem::remove_all(local);
    std::filesystem::remove(path);
}

// What the server has is not what the state recorded: an earlier run counted what it handed to the stream, and a
// stream that ended there left those bytes behind. The resume point moves to the size read before this run opened
// its stream, so the bytes between the two are sent again from the same local file.
TEST(LocalUploadProducer, theResumePointMovesToTheSizeTheServerHas)
{
    const std::filesystem::path path = "transfer_state_upload_server_smaller.json";
    std::filesystem::remove(path);

    const std::string target = "/server/target";
    const std::filesystem::path local = "upload_server_smaller";
    std::filesystem::remove_all(local);
    std::filesystem::create_directories(local);
    writeLocalFile(local / "a.bin", "abcdef");

    TransferStateFile state(path);
    const StateEntry part{1, "a.bin", 0, false, 6, 4, 1, target + "/a.bin"};
    state.record(TransferKind::Upload, "localhost:7700", target, part);

    const StateContext context{&state, TransferKind::Upload, "localhost:7700", target,
                               state.find(TransferKind::Upload, "localhost:7700", target)};
    const RemoteSizes remoteSizes = {{target + "/a.bin", 2}};

    std::atomic<bool> cancel{false};
    TransferCounters counters;
    LocalUploadProducer producer(target, {local / "a.bin"}, false, counters, cancel, context, remoteSizes);

    pb::UploadItem item;
    ASSERT_TRUE(producer.next(item));
    EXPECT_EQ(target, item.header().name());

    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_header());
    EXPECT_EQ(2u, item.header().resumefrom());
    EXPECT_EQ(1u, item.header().startsequence());

    // The chunk that follows is the content from the server's size on, not from the size the state had.
    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_chunk());
    EXPECT_EQ(1u, item.chunk().sequence());
    EXPECT_EQ(2u, item.chunk().offset());
    EXPECT_EQ("cdef", item.chunk().data());

    // Read to the end so the file the producer opened is closed before the folder is removed.
    EXPECT_FALSE(producer.next(item));

    std::filesystem::remove_all(local);
    std::filesystem::remove(path);
}

// An entry the state knows but the server does not: there is nothing to continue from, so the entry is sent from
// its first byte and the server creates it.
TEST(LocalUploadProducer, anEntryTheServerDoesNotHasStartsFromItsFirstByte)
{
    const std::filesystem::path path = "transfer_state_upload_server_missing.json";
    std::filesystem::remove(path);

    const std::string target = "/server/target";
    const std::filesystem::path local = "upload_server_missing";
    std::filesystem::remove_all(local);
    std::filesystem::create_directories(local);
    writeLocalFile(local / "a.bin", "abcdef");

    TransferStateFile state(path);
    const StateEntry part{1, "a.bin", 0, false, 6, 4, 1, target + "/a.bin"};
    state.record(TransferKind::Upload, "localhost:7700", target, part);

    const StateContext context{&state, TransferKind::Upload, "localhost:7700", target,
                               state.find(TransferKind::Upload, "localhost:7700", target)};
    // A non-empty map that does not contain this entry is what a run gets when the server has the other entries and
    // not this one. An empty map is the case where the sizes could not be read, which is the test above covers.
    const RemoteSizes remoteSizes = {{target + "/other.bin", 9}};

    std::atomic<bool> cancel{false};
    TransferCounters counters;
    LocalUploadProducer producer(target, {local / "a.bin"}, false, counters, cancel, context, remoteSizes);

    pb::UploadItem item;
    ASSERT_TRUE(producer.next(item));
    EXPECT_EQ(target, item.header().name());

    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_header());
    EXPECT_EQ(0u, item.header().resumefrom());
    EXPECT_EQ(0u, item.header().startsequence());

    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_chunk());
    EXPECT_EQ(0u, item.chunk().sequence());
    EXPECT_EQ(0u, item.chunk().offset());
    EXPECT_EQ("abcdef", item.chunk().data());

    EXPECT_FALSE(producer.next(item));

    std::filesystem::remove_all(local);
    std::filesystem::remove(path);
}

// The other way round: the state stopped in the middle of an entry the server has in full, so the entry is left
// alone and only its header is sent.
TEST(LocalUploadProducer, anEntryTheServerHasInFullIsLeftAloneEvenWhenTheStateStoppedInIt)
{
    const std::filesystem::path path = "transfer_state_upload_server_full.json";
    std::filesystem::remove(path);

    const std::string target = "/server/target";
    const std::filesystem::path local = "upload_server_full";
    std::filesystem::remove_all(local);
    std::filesystem::create_directories(local);
    writeLocalFile(local / "a.bin", "abcdef");

    TransferStateFile state(path);
    const StateEntry part{1, "a.bin", 0, false, 6, 4, 1, target + "/a.bin"};
    state.record(TransferKind::Upload, "localhost:7700", target, part);

    const StateContext context{&state, TransferKind::Upload, "localhost:7700", target,
                               state.find(TransferKind::Upload, "localhost:7700", target)};
    const RemoteSizes remoteSizes = {{target + "/a.bin", 6}};

    std::atomic<bool> cancel{false};
    TransferCounters counters;
    LocalUploadProducer producer(target, {local / "a.bin"}, false, counters, cancel, context, remoteSizes);

    pb::UploadItem item;
    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_header());
    EXPECT_TRUE(item.header().alreadycomplete());

    EXPECT_FALSE(producer.next(item));

    EXPECT_EQ(2u, counters.entries);
    EXPECT_EQ(0u, counters.files);
    EXPECT_EQ(1u, counters.skipped);
    EXPECT_EQ(0u, counters.bytes);

    std::filesystem::remove_all(local);
    std::filesystem::remove(path);
}

// Overwrite means replace, so nothing is continued: the state is not consulted and the whole file is sent, which is
// what lets the server write it over instead of appending to it.
TEST(LocalUploadProducer, overwriteIgnoresWhatTheStateRecordedForAnEntry)
{
    const std::filesystem::path path = "transfer_state_upload_overwrite.json";
    std::filesystem::remove(path);

    const std::string target = "/server/target";
    const std::filesystem::path local = "upload_overwrite";
    std::filesystem::remove_all(local);
    std::filesystem::create_directories(local);
    writeLocalFile(local / "a.bin", "abcdef");

    TransferStateFile state(path);
    const StateEntry part{1, "a.bin", 0, false, 6, 4, 1, target + "/a.bin"};
    state.record(TransferKind::Upload, "localhost:7700", target, part);

    const StateContext context{&state, TransferKind::Upload, "localhost:7700", target,
                               state.find(TransferKind::Upload, "localhost:7700", target)};
    const RemoteSizes remoteSizes = {{target + "/a.bin", 2}};

    std::atomic<bool> cancel{false};
    TransferCounters counters;
    LocalUploadProducer producer(target, {local / "a.bin"}, true, counters, cancel, context, remoteSizes);

    pb::UploadItem item;
    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_header());
    EXPECT_TRUE(item.header().overwrite());
    EXPECT_FALSE(item.header().alreadycomplete());
    EXPECT_EQ(0u, item.header().resumefrom());

    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_chunk());
    EXPECT_EQ(0u, item.chunk().sequence());
    EXPECT_EQ("abcdef", item.chunk().data());

    EXPECT_FALSE(producer.next(item));

    std::filesystem::remove_all(local);
    std::filesystem::remove(path);
}

// A message is recorded when the write took it, not when it was prepared: a message the stream refused never reached
// the server, and the state has to say what the server has.
TEST(LocalUploadProducer, aMessageCountsOnlyAfterTheWriteIsConfirmed)
{
    const std::filesystem::path path = "transfer_state_upload_confirm.json";
    std::filesystem::remove(path);

    const std::string target = "/server/target";
    const std::filesystem::path local = "upload_confirm";
    std::filesystem::remove_all(local);
    std::filesystem::create_directories(local);
    writeLocalFile(local / "a.bin", "ab");

    TransferStateFile state(path);
    const StateContext context{&state, TransferKind::Upload, "localhost:7700", target,
                               state.find(TransferKind::Upload, "localhost:7700", target)};

    std::atomic<bool> cancel{false};
    TransferCounters counters;
    LocalUploadProducer producer(target, {local / "a.bin"}, false, counters, cancel, context);

    pb::UploadItem item;
    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_header());
    EXPECT_EQ(0u, state.find(TransferKind::Upload, "localhost:7700", target).entries.size());

    producer.confirm();
    const StateTransfer recorded = state.find(TransferKind::Upload, "localhost:7700", target);
    ASSERT_EQ(1u, recorded.entries.size());
    EXPECT_EQ(target, recorded.entries[0].path);

    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_header());
    EXPECT_EQ(1u, recorded.entries.size());

    producer.confirm();
    const StateTransfer withEntry = state.find(TransferKind::Upload, "localhost:7700", target);
    ASSERT_EQ(2u, withEntry.entries.size());
    EXPECT_EQ(target + "/a.bin", withEntry.entries[1].path);
    EXPECT_EQ(0u, withEntry.entries[1].bytesWritten);

    ASSERT_TRUE(producer.next(item));
    ASSERT_TRUE(item.has_chunk());
    EXPECT_EQ(0u, withEntry.entries[1].bytesWritten);

    producer.confirm();
    const StateTransfer written = state.find(TransferKind::Upload, "localhost:7700", target);
    EXPECT_EQ(2u, written.entries[1].bytesWritten);
    EXPECT_EQ(1u, written.entries[1].chunksSent);

    EXPECT_FALSE(producer.next(item));

    std::filesystem::remove_all(local);
    std::filesystem::remove(path);
}

TEST(CrashDump, theDumpNameHasTheProcessIdAndTheTime)
{
    const std::string name = crash::dumpPath("crash_dump_test").filename().string();

    const auto digits = [](const std::string &text) -> bool {
        for (const char c : text)
        {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0)
            {
                return false;
            }
        }
        return true;
    };

    // filebrowser-crash-<pid>-<yyyymmdd>-<hhmmss>.dmp. The process id is checked as digits rather than against
    // the running process, because the test would have to include windows.h to ask for it.
    EXPECT_EQ("filebrowser-crash-", name.substr(0, 18));
    const std::size_t dash = name.find('-', 18);
    ASSERT_TRUE(dash != std::string::npos);
    EXPECT_TRUE(digits(name.substr(18, dash - 18)));

    const std::string stamp = name.substr(dash + 1, name.size() - dash - 5);
    EXPECT_EQ(15u, stamp.size());
    EXPECT_EQ('-', stamp[8]);
    EXPECT_TRUE(digits(stamp.substr(0, 8)));
    EXPECT_TRUE(digits(stamp.substr(9)));
    EXPECT_EQ(".dmp", name.substr(name.size() - 4));
}

TEST(CrashDump, writesADumpOfTheProcessItIsRunIn)
{
    const std::filesystem::path folder = "crash_dump_test";
    std::filesystem::remove_all(folder);

    ASSERT_TRUE(crash::writeDump(folder, nullptr));

    std::vector<std::filesystem::directory_entry> files;
    for (const auto &entry : std::filesystem::directory_iterator(folder))
    {
        files.push_back(entry);
    }
    ASSERT_EQ(1u, files.size());
    EXPECT_TRUE(files[0].is_regular_file());
    EXPECT_GT(files[0].file_size(), 0);

    // A minidump starts with the MDMP signature, so this shows a dump was written and not just an empty file.
    std::ifstream file(files[0].path(), std::ios::binary);
    char header[4] = {};
    file.read(header, 4);
    EXPECT_EQ("MDMP", std::string(header, 4));

    // Closed before the folder goes, otherwise the removal sees the file still open.
    file.close();
    std::filesystem::remove_all(folder);
}

TEST(CrashDump, aFolderThatCannotBeCreatedIsReportedAsFalse)
{
    const std::filesystem::path folder = "crash_dump_test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);

    // A regular file where a directory is asked for: create_directories reports it through the error code instead
    // of throwing, which is what a handler that is about to end the process needs.
    const std::filesystem::path taken = folder / "taken";
    std::ofstream(taken) << "x";

    EXPECT_FALSE(crash::writeDump(taken / "deeper", nullptr));

    std::filesystem::remove_all(folder);
}

TEST(CrashDump, installMakesTheFolderBeforeAnythingCrashes)
{
    const std::filesystem::path folder = "crash_dump_test_install";
    std::filesystem::remove_all(folder);

    crash::install(folder);

    // The handlers cannot be checked here: they end the process, and a test that crashes takes the whole run with
    // it. What can be checked is that the place they write to exists before anything is faulted.
    EXPECT_TRUE(std::filesystem::is_directory(folder));

    std::filesystem::remove_all(folder);
}

TEST(CrashDump, aReportFromTheDebugCRTWritesADump)
{
#ifndef _DEBUG
    GTEST_SKIP() << "the report hook is only installed with the debug CRT";
#endif

    const std::filesystem::path folder = "crash_dump_test_report";
    std::filesystem::remove_all(folder);

    crash::install(folder);

    // _CrtDbgReport is what abort() and the heap checks go through, so calling it here is the same path the crash
    // takes: the report itself does not write anything, the hook does.
    _CrtDbgReport(_CRT_ASSERT, nullptr, 0, nullptr, "report hook check");

    std::vector<std::filesystem::directory_entry> files;
    for (const auto &entry : std::filesystem::directory_iterator(folder))
    {
        files.push_back(entry);
    }
    ASSERT_EQ(2u, files.size());

    // The text is what says which check fired when there is no console, so it is checked as content: the report type
    // on the first line, the message on the second. The two files share the name, which is how they are read as one
    // record.
    std::filesystem::path text;
    std::filesystem::path dump;
    for (const auto &entry : files)
    {
        if (entry.path().extension() == ".txt")
        {
            text = entry.path();
        }
        else
        {
            dump = entry.path();
        }
    }
    ASSERT_TRUE(!text.empty());

    std::ifstream file(text);
    std::string type;
    std::string message;
    std::getline(file, type);
    std::getline(file, message);
    EXPECT_EQ("assert", type);
    // The hook gets the whole formatted message, prefix included - "Assertion failed: " is what the CRT puts in
    // front - so the check is for the text that was asked for, not for the line as a whole.
    EXPECT_NE(std::string::npos, message.find("report hook check"));
    EXPECT_EQ(dump.stem(), text.stem());

    // The frames follow the message, and they are the part that says which code reached the report. Which ones are on
    // the stack depends on the CRT, so only the shape is checked here: the count on that line and the lines after it
    // have to agree, and every frame line is an address. That line is right after the message because the message
    // keeps the line ending the CRT gave it - an assertion message carries its own newline, so no second one is
    // written - which is what the second line above ends with here.
    std::string line;
    std::getline(file, line);
    ASSERT_EQ(0u, line.rfind("frames: ", 0));
    const unsigned recorded = static_cast<unsigned>(std::stoul(line.substr(8)));
    EXPECT_GT(recorded, 0u);

    unsigned listed = 0;
    while (std::getline(file, line))
    {
        ++listed;
        EXPECT_EQ(0u, line.rfind("0x", 0));
    }
    EXPECT_EQ(recorded, listed);

    // Closed before the folder goes, otherwise the removal sees the file still open.
    file.close();
    std::filesystem::remove_all(folder);
}

int main(int argc, char **argv)
{
    spdlog::set_level(spdlog::level::trace);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}