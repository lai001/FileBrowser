#include "FileBrowserUIController.h"
#include <fstream>
#include "spdlog/spdlog.h"
#include "imgui.h"
#include "Misc.h"

static int InputTextCallback(ImGuiInputTextCallbackData *data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        std::string *str = reinterpret_cast<std::string *>(data->UserData);
        IM_ASSERT(data->Buf == str->c_str());
        str->resize(data->BufTextLen);
        data->Buf = const_cast<char *>(str->c_str());
    }
    return 0;
}

FileBrowserUIController::FileBrowserUIController()
{
    channel = grpc::CreateChannel("localhost:7700", grpc::InsecureChannelCredentials());
    fileBrowserClient = std::make_unique<FileBrowserClient>(channel);
    currentPath = std::string();
    directoryTree = std::make_shared<Mutex<DirectoryTree>>();
}

FileBrowserUIController::~FileBrowserUIController()
{
}

void FileBrowserUIController::render()
{
    ImGui::Begin("FileBrowser");
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
    const bool isChanged = ImGui::InputTextWithHint("CurrentPath", "Type path here",
                                                    const_cast<char *>(currentPath.c_str()), currentPath.capacity() + 1,
                                                    flags, InputTextCallback, reinterpret_cast<void *>(&currentPath));

    if (ImGui::Button("Scan"))
    {
        spdlog::trace("Click scan button");
        const PathMap directories = fileBrowserClient->recursiveDirectoryTraversal(currentPath);
        DirectoryTree *newDirectoryTree = new DirectoryTree(directories);
        newDirectoryTree->rebuildHierarchy();
        newDirectoryTree->recalculateDirectorySize();
        directoryTree->replace(newDirectoryTree);
    }

    if (ImGui::Button("Load"))
    {
        spdlog::trace("Click load button");

        std::ifstream inputFile(currentPath, std::ios::binary);
        if (inputFile.is_open())
        {
            pb::FileEntryCollection fileEntryCollection;
            if (fileEntryCollection.ParseFromIstream(&inputFile))
            {
                PathMap directories;
                for (const auto &item : fileEntryCollection.fileentrires())
                {
                    const std::string filePath = item.filepath();
                    DirectoryEntry directoryEntry;
                    directoryEntry.fileSize = item.filesize();
                    directoryEntry.filePath = misc::wstringRemoveNullTerminator(misc::utf8ToWstring(filePath));
                    directoryEntry.isDirectory = item.isdirectory();
                    directories[directoryEntry.filePath] = directoryEntry;
                }
                DirectoryTree *newDirectoryTree = new DirectoryTree(directories);
                newDirectoryTree->rebuildHierarchy();
                newDirectoryTree->recalculateDirectorySize();
                directoryTree->replace(newDirectoryTree);
            }
            else
            {
                spdlog::warn("Not support file type, {}", currentPath);
            }
            inputFile.close();
        }
        else
        {
            spdlog::warn("Open file failed, {}", currentPath);
        }
    }

    {
        LockGuard<DirectoryTree> directoryTreeProtect = directoryTree->lock();
        if (directoryTreeProtect)
        {
            renderTreeNode(*directoryTreeProtect.get(),
                           directoryTreeProtect->directories[directoryTreeProtect->getRootPath()]);
        }
    }

    ImGui::End();
}

void FileBrowserUIController::scan(const std::string &rootPath)
{
}

void FileBrowserUIController::renderTreeNode(const DirectoryTree &directoryTree, const DirectoryEntry &entry)
{
    std::string label = entry.filePath.filename().u8string();
    if (label.size() == 0)
    {
        label = entry.filePath.u8string();
    }
    const bool isDirectory = entry.isDirectory;
    const uint64_t fileSize = entry.fileSize;
    const std::string fileSizeStr = misc::formatSize(entry.fileSize);
    label = fmt::format("{} ({})", label, fileSizeStr);
    ImGuiTreeNodeFlags flag = ImGuiTreeNodeFlags_None;
    if (!isDirectory)
    {
        flag |= ImGuiTreeNodeFlags_Leaf;
    }
    if (ImGui::TreeNodeEx(label.c_str(), flag))
    {
        std::vector<std::filesystem::path> sorted;
        sorted.reserve(entry.childs.size());
        for (const std::filesystem::path &item : entry.childs)
        {
            sorted.push_back(item);
        }
        std::sort(sorted.begin(), sorted.end(),
                  [&directoryTree](const std::filesystem::path &lhs, const std::filesystem::path &rhs) {
                      return directoryTree.directories.at(lhs).fileSize > directoryTree.directories.at(rhs).fileSize;
                  });
        for (const std::filesystem::path &item : sorted)
        {
            renderTreeNode(directoryTree, directoryTree.directories.at(item));
        }
        ImGui::TreePop();
    }
}