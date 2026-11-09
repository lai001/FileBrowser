#include "FileBrowserUIModel.h"

namespace
{
// What the "+" button uses for a new tab: the tab carries no add-row of its own any more, so a new entry
// starts from these defaults and its port is bumped in addEntry() until the key is free.
const char defaultServerAddress[] = "localhost";
constexpr int defaultServerPort = 7700;
} // namespace

FileBrowserUIModel::FileBrowserUIModel() : serverExecutablePath(ServerLauncher::defaultServerExecutable())
{
    // Entry 0 is the default tab. Any tab can start a server for its own entry, so every entry carries its
    // own launcher. Nothing is read yet, so the first visit to this tab shows its roots.
    addEntry();
}

std::vector<ServerEntry> &FileBrowserUIModel::entries()
{
    return servers;
}

const std::vector<ServerEntry> &FileBrowserUIModel::entries() const
{
    return servers;
}

ServerEntry &FileBrowserUIModel::selected()
{
    return servers[selectedServer];
}

std::size_t FileBrowserUIModel::selectedIndex() const
{
    return selectedServer;
}

void FileBrowserUIModel::selectEntry(std::size_t index)
{
    selectedServer = index;
}

ServerEntry *FileBrowserUIModel::findEntryByTabId(int tabId)
{
    for (ServerEntry &entry : servers)
    {
        if (entry.tabId == tabId)
        {
            return &entry;
        }
    }
    return nullptr;
}

bool FileBrowserUIModel::keyExists(const std::string &key) const
{
    for (const ServerEntry &other : servers)
    {
        if (other.key() == key)
        {
            return true;
        }
    }
    return false;
}

ServerEntry &FileBrowserUIModel::addEntry()
{
    ServerEntry entry;
    entry.address = defaultServerAddress;
    entry.port = defaultServerPort;

    // The tab title is the key, so a second entry with the same address and port would look like a copy of
    // the first tab even though PushID(tabId) keeps their IDs apart. Bumping the port is what makes the "+"
    // button usable more than once: a new tab is a server that can be started on its own port, so two
    // entries cannot both listen on 7700. The address itself can still be edited inside the new tab.
    while (entry.port < 65535 && keyExists(entry.key()))
    {
        ++entry.port;
    }

    entry.launcher = std::make_unique<ServerLauncher>();
    entry.tabId = nextTabId++;
    entry.firstConnection = true;
    entry.showRoots = true;
    entry.directoryTree = std::make_shared<misc::Guarded<DirectoryTree>>();
    servers.push_back(std::move(entry));
    return servers.back();
}

void FileBrowserUIModel::removeEntry(std::size_t index)
{
    servers.erase(servers.begin() + static_cast<std::ptrdiff_t>(index));
    if (index < selectedServer)
    {
        --selectedServer;
    }
    else
    {
        selectedServer = 0;
    }
}

void FileBrowserUIModel::clampPort(ServerEntry &entry)
{
    if (entry.port < 1)
    {
        entry.port = 1;
    }
    if (entry.port > 65535)
    {
        entry.port = 65535;
    }
}

std::string &FileBrowserUIModel::serverExecutable()
{
    return serverExecutablePath;
}

const std::string &FileBrowserUIModel::serverExecutable() const
{
    return serverExecutablePath;
}
