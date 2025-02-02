#ifndef CLIENT_SRC_FILE_BROSWER_UI_CONTROLLER_H
#define CLIENT_SRC_FILE_BROSWER_UI_CONTROLLER_H
#include <memory>
#include <vector>
#include <unordered_map>
#include "Client.h"
#include "Mutex.h"
#include "DirectoryTree.h"

class FileBrowserUIController
{
  public:
    FileBrowserUIController();

    ~FileBrowserUIController();

    void render();

    void scan(const std::string &rootPath);

  private:
    std::shared_ptr<grpc::Channel> channel;

    std::unique_ptr<FileBrowserClient> fileBrowserClient;

    std::string currentPath;

    std::shared_ptr<Mutex<DirectoryTree>> directoryTree;

    void renderTreeNode(const DirectoryTree &directoryTree, const DirectoryEntry &entry);
};

#endif