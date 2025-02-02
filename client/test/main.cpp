#include <fstream>
#include "spdlog/spdlog.h"
#include "gtest/gtest.h"
#include "FileBrowser.grpc.pb.h"
#include "FileBrowser.pb.h"
#include "DirectoryTree.h"
#include "Misc.h"

TEST(DirectoryEntry, init)
{
    DirectoryEntry entry;
    EXPECT_EQ(0, entry.fileSize);
    EXPECT_EQ("", entry.filePath);
    EXPECT_EQ("", entry.getParent());
    EXPECT_EQ(false, entry.isDirectory);
    EXPECT_EQ(0, entry.childs.size());
}

TEST(DirectoryEntry, getParent)
{
    DirectoryEntry entry;
    entry.filePath = "/root/data";
    entry.isDirectory = true;
    EXPECT_EQ("/root", entry.getParent());
}

TEST(DirectoryTree, init)
{
    DirectoryTree tree;
    EXPECT_EQ("", tree.getRootPath());
    EXPECT_EQ(0, tree.directories.size());
}

// TEST(DirectoryTree, insertEntry)
//{
//     DirectoryTree tree;
//     DirectoryEntry entry;
//     entry.filePath = "/root";
//     entry.isDirectory = true;
//     tree.insertEntry(entry);
//     EXPECT_EQ(1, tree.directories.size());
// }

TEST(FileEntryCollection, readFromLocalDisk)
{
    pb::FileEntryCollection fileEntryCollection;
    std::ifstream inputFile("android_files.bin", std::ios::binary);
    EXPECT_EQ(true, inputFile.is_open());
    EXPECT_EQ(true, fileEntryCollection.ParseFromIstream(&inputFile));
    inputFile.close();
    for (const auto &item : fileEntryCollection.fileentrires())
    {
        const std::string filePath = item.filepath();
        try
        {
            const std::filesystem::path path = filePath;
        }
        catch (const std::exception &e)
        {
            spdlog::error("{}, {}", filePath, e.what());
            throw e;
        }
    }
    EXPECT_EQ(346136, fileEntryCollection.fileentrires_size());
}

TEST(FileEntryCollection, rebuildHierarchy)
{
    pb::FileEntryCollection fileEntryCollection;
    std::ifstream inputFile("files.bin", std::ios::binary);
    EXPECT_EQ(true, inputFile.is_open());
    EXPECT_EQ(true, fileEntryCollection.ParseFromIstream(&inputFile));
    inputFile.close();
    std::unordered_map<std::filesystem::path, DirectoryEntry> directories;
    for (const auto &item : fileEntryCollection.fileentrires())
    {
        DirectoryEntry directoryEntry;
        directoryEntry.fileSize = item.filesize();
        directoryEntry.filePath = item.filepath();
        directoryEntry.isDirectory = item.isdirectory();
        directories[directoryEntry.filePath] = directoryEntry;
    }
    DirectoryTree directoryTree(directories);
    EXPECT_EQ(346136, fileEntryCollection.fileentrires_size());
    EXPECT_EQ(true, directoryTree.rebuildHierarchy());
    EXPECT_EQ(true, directoryTree.recalculateDirectorySize());
    EXPECT_EQ(65945917219, directoryTree.getTotalFileSize());
}

TEST(Path, parent)
{
    std::filesystem::path child = "/root/bin";
    std::filesystem::path parent = "/root";
    EXPECT_EQ(child.parent_path(), parent);
}

TEST(FileEntryCollection, rebuild2)
{
    std::unordered_map<std::filesystem::path, DirectoryEntry> directories;
    {
        DirectoryEntry entry;
        entry.filePath = "/root/bin/bin2";
        entry.isDirectory = false;
        entry.fileSize = 1024;
        directories[entry.filePath] = entry;
    }
    {
        DirectoryEntry entry;
        entry.filePath = "/root/bin";
        entry.isDirectory = true;
        entry.fileSize = 0;
        directories[entry.filePath] = entry;
    }
    {
        DirectoryEntry entry;
        entry.filePath = "/root/bin1";
        entry.isDirectory = false;
        entry.fileSize = 4096;
        directories[entry.filePath] = entry;
    }
    {
        DirectoryEntry entry;
        entry.filePath = "/root";
        entry.isDirectory = true;
        entry.fileSize = 0;
        directories[entry.filePath] = entry;
    }

    DirectoryTree directoryTree(directories);
    EXPECT_EQ(true, directoryTree.rebuild());
    EXPECT_EQ(2, directoryTree.directories["/root"].childs.size());
    EXPECT_EQ(4096 + 1024, directoryTree.directories["/root"].fileSize);
}

TEST(FileEntryCollection, rebuild3)
{
    std::unordered_map<std::filesystem::path, DirectoryEntry> directories;
    {
        DirectoryEntry entry;
        entry.filePath = "/ro ot/bin";
        entry.isDirectory = false;
        entry.fileSize = 1024;
        directories[entry.filePath] = entry;
    }
    {
        DirectoryEntry entry;
        entry.filePath = "/ro ot/bin1";
        entry.isDirectory = false;
        entry.fileSize = 4096;
        directories[entry.filePath] = entry;
    }
    {
        DirectoryEntry entry;
        entry.filePath = "/ro ot";
        entry.isDirectory = true;
        entry.fileSize = 0;
        directories[entry.filePath] = entry;
    }

    DirectoryTree directoryTree(directories);
    EXPECT_EQ(true, directoryTree.rebuild());
    EXPECT_EQ(2, directoryTree.directories["/ro ot"].childs.size());
    EXPECT_EQ(1024 + 4096, directoryTree.directories["/ro ot"].fileSize);
}

TEST(FileEntryCollection, rebuild4)
{
    std::unordered_map<std::filesystem::path, DirectoryEntry> directories;
    {
        DirectoryEntry entry;
        entry.filePath = "D:/Program Files/bin";
        entry.isDirectory = false;
        entry.fileSize = 1024;
        directories[entry.filePath] = entry;
    }
    {
        DirectoryEntry entry;
        entry.filePath = "D:/Program Files/bi n1";
        entry.isDirectory = false;
        entry.fileSize = 4096;
        directories[entry.filePath] = entry;
    }
    {
        DirectoryEntry entry;
        entry.filePath = "D:/Program Files";
        entry.isDirectory = true;
        entry.fileSize = 0;
        directories[entry.filePath] = entry;
    }

    DirectoryTree directoryTree(directories);

    EXPECT_EQ(true, directoryTree.rebuild());
    EXPECT_EQ(2, directoryTree.directories["D:/Program Files"].childs.size());
    EXPECT_EQ(1024 + 4096, directoryTree.directories["D:/Program Files"].fileSize);
}

TEST(FileEntryCollection, rebuild5)
{
    std::unordered_map<std::filesystem::path, DirectoryEntry> directories;
    {
        DirectoryEntry entry;
        entry.filePath = "D:/Program Files/bin";
        entry.isDirectory = false;
        entry.fileSize = 1024;
        directories[entry.filePath] = entry;
    }
    DirectoryTree directoryTree(directories);
    EXPECT_EQ(true, directoryTree.rebuild());
    EXPECT_EQ(1, directoryTree.directories["D:/Program Files"].childs.size());
    EXPECT_EQ(1, directoryTree.directories["D:/"].childs.size());
    EXPECT_EQ(1024, directoryTree.directories["D:/Program Files"].fileSize);
    EXPECT_EQ(1024, directoryTree.directories["D:/"].fileSize);
}

TEST(Misc, formatSize)
{
    EXPECT_EQ("1 bytes", misc::formatSize(1));
    EXPECT_EQ("1023 bytes", misc::formatSize(1023));
    EXPECT_EQ("1.000 kb", misc::formatSize(1024));
    EXPECT_EQ("1.000 mb", misc::formatSize(1024 * 1024));
    EXPECT_EQ("1.000 gb", misc::formatSize(1024 * 1024 * 1024));
}

TEST(Misc, getParentDirectories)
{
    const std::unordered_set<std::filesystem::path> parents =
        misc::getParentDirectories(std::filesystem::path("D:/Program Files/A/B/C"));
    EXPECT_EQ(4, parents.size());
    EXPECT_TRUE(parents.find(std::filesystem::path("D:/")) != parents.end());
    EXPECT_TRUE(parents.find(std::filesystem::path("D:/Program Files")) != parents.end());
    EXPECT_TRUE(parents.find(std::filesystem::path("D:/Program Files/A")) != parents.end());
    EXPECT_TRUE(parents.find(std::filesystem::path("D:/Program Files/A/B")) != parents.end());
    EXPECT_FALSE(parents.find(std::filesystem::path("D:/Program Files/A/B/C")) != parents.end());
}

TEST(Misc, getParentDirectories1)
{
    const std::unordered_set<std::filesystem::path> parents = misc::getParentDirectories(std::filesystem::path("D:/"));
    EXPECT_EQ(0, parents.size());
}

int main(int argc, char **argv)
{
    spdlog::set_level(spdlog::level::trace);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}