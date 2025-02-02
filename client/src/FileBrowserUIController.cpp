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
        const std::unordered_map<std::filesystem::path, DirectoryEntry> directories =
            fileBrowserClient->recursiveDirectoryTraversal(currentPath);
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
                std::unordered_map<std::filesystem::path, DirectoryEntry> directories;
                for (const auto &item : fileEntryCollection.fileentrires())
                {
                    DirectoryEntry directoryEntry;
                    directoryEntry.fileSize = item.filesize();
                    directoryEntry.filePath = item.filepath();
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
    std::string label = entry.filePath.filename().string();
    if (label.size() == 0)
    {
        label = entry.filePath.string();
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
        for (const std::filesystem::path &item : entry.childs)
        {
            renderTreeNode(directoryTree, directoryTree.directories.at(item));
        }
        ImGui::TreePop();
    }
}