#ifndef CLIENT_SRC_FILE_BROWSER_UI_MODEL_H
#define CLIENT_SRC_FILE_BROWSER_UI_MODEL_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "Client.h"
#include "DirectoryTree.h"
#include "Guarded.h"
#include "ServerLauncher.h"

// One connection target, one tab. Every entry can have a child process started for it from its own tab, so
// each entry keeps its own ServerLauncher and several servers can run at the same time.
struct ServerEntry
{
    std::string address;
    int port;

    // The child process started from this tab, if any. ServerLauncher is non-copyable, so it is held by
    // pointer. Its destructor stops the process: closing this tab stops the server it started.
    std::unique_ptr<ServerLauncher> launcher;

    // Stable ImGui id for this server's tab. It is deliberately not the index in the vector: removing a
    // server shifts every index after it, and the tab label is the address, which can be edited, so neither
    // is a usable identity. ImGui derives the tab id from window->GetID(label), so the label is written as
    // "key###tabId": only the part after ### is hashed, only the part before it is displayed. PushID(tabId)
    // also keeps the widgets inside one tab apart from the same widgets in another tab.
    int tabId;

    // True until this server has been read once: it decides whether the Read button is drawn and whether the
    // system info summary says "Not read yet". What the browse panel shows is decided by showRoots, and the
    // first read is what sets that flag and fills the cache.
    bool firstConnection;

    // One channel per entry, built once and kept, so switching tabs does not build a new one. gRPC connects
    // lazily, but a named target still has to be resolved, and c-ares does not read the hosts file: the first
    // RPC on a new channel sends a real DNS query for the host, which burns the whole query timeout when the
    // nameserver it picked is unreachable. See channelTarget() in the controller for the loopback case.
    // channelTarget is the target this channel was built for; a different address:port means rebuilding it.
    // shared_ptr, not unique_ptr: a scan keeps its own reference to the client, so closing this tab while a
    // traversal is running cannot destroy the stub under a call that is still in flight.
    std::shared_ptr<grpc::Channel> channel;
    std::shared_ptr<FileBrowserClient> client;
    std::string channelTarget;

    // Cached from the first read, so a tab shows its server's info and roots without sending anything when the
    // selection comes back to it.
    pb::SystemInfoReply systemInfo;
    std::vector<std::string> roots;

    // The directory this server was last listed at, and the listing cached for it.
    std::string currentPath;
    std::vector<pb::FileEntry> cachedFiles;

    // Which row the browse panel is pointing at. A single click marks it, the double click is what
    // enters the directory, so the panel needs to remember the marked row between frames.
    std::string highlightedPath;

    // Set by the first read and cleared when a directory listing is cached for this entry; the
    // "Roots" button sets it again without re-reading the server.
    bool showRoots;

    // Scanned tree for this server, kept so switching back to a server restores its tree.
    std::shared_ptr<misc::Guarded<DirectoryTree>> directoryTree;

    // The directory the last scan of this server was started on. Empty until a scan or a loaded file
    // produced a tree. The folder sizes the panel shows come from that tree, not from the RPC.
    std::string scanRoot;

    // Last message about this entry (Start/Stop, the first read, a listing). Status lines are per entry so a
    // message from one server does not show up in another server's tab.
    std::string lastStatus;

    // Local folder a download writes into, in UTF-8 like every other path the UI holds. Empty until the input or
    // the folder picker sets it; the right-click menu downloads into whatever this holds at that moment.
    std::string localDownloadPath;

    // True for an upload that replaces a file that is already on the server. The server default is false, and
    // this stays false until the checkbox in the transfer section is ticked.
    bool uploadOverwrite = false;

    // The row the mouse was over on the frame that just finished, written as the directory a drop would go to.
    // GLFW hands dropped files to the window as a whole, so this is what decides where they land: a directory row
    // gives that directory, a file row gives the directory it is in, and nothing hovered means the drop does
    // nothing. It is recomputed every frame, so a drop uses the row the mouse was really on.
    std::string dropTargetPath;

    // The row the last right click landed on, and what the menu needs to know about it. The menu is opened once
    // for the tab, after the rows are drawn, so the path has to survive until then.
    std::string contextMenuPath;
    bool contextMenuIsDirectory = false;
    // Only used for a file: the size the listing or the tree knew, which is what fills the header the single
    // file download synthesizes. 0 when the row is a directory or the size is not known.
    uint64_t contextMenuFileSize = 0;

    std::string key() const
    {
        return address + ":" + std::to_string(port);
    }
};

// Model layer: the entries and the rules about them - the list, the selection, the ids, the executable path, and
// the port range the server accepts. Nothing here draws, sends an RPC or runs a worker: the view reads it and
// records what the mouse was on, the controller changes it by running the calls. Both go through this object
// because it is the only owner of the entries.
class FileBrowserUIModel
{
  public:
    FileBrowserUIModel();

    // Entries in tab order.
    std::vector<ServerEntry> &entries();
    const std::vector<ServerEntry> &entries() const;

    // The last remaining tab has no close button, so there is always at least one entry and the selected index
    // stays in range.
    ServerEntry &selected();
    std::size_t selectedIndex() const;

    // Moves the selection to an entry by its position in the list. The caller has to check the index; the model
    // does not look at the tab bar, so it cannot know which tab the view is on.
    void selectEntry(std::size_t index);

    // The entry a worker result belongs to. nullptr when the tab was closed while it was running, which is the
    // case where the result itself is the only thing that can free the tree the worker built.
    ServerEntry *findEntryByTabId(int tabId);

    // True when an entry already has this tab title, which is its address:port key. Two entries with the same
    // key would look like a copy of the first tab even though PushID(tabId) keeps their IDs apart, so a new
    // entry bumps its port until the key is free.
    bool keyExists(const std::string &key) const;

    // Adds one entry from the defaults with the first free port. It does not write a status line and it does not
    // switch to the new tab: the caller decides both, because creating a tab is only that.
    ServerEntry &addEntry();

    // Removes one entry and moves the selection to a tab that still exists.
    void removeEntry(std::size_t index);

    // Clamps an edited port into the range the server command line accepts (CLI::Range 1-65535), so the value
    // typed in the UI is not left for the child process to reject.
    void clampPort(ServerEntry &entry);

    // Path of FileBrowserServer.exe, one path for every entry: what differs per entry is the address and port it
    // starts on, not which executable it runs.
    std::string &serverExecutable();
    const std::string &serverExecutable() const;

  private:
    std::vector<ServerEntry> servers;
    std::size_t selectedServer = 0;

    // Id for the next tab. A tab is created only by the "+" button, so there are no add-row fields any more: a
    // new entry starts from the default address and port defined in the cpp file.
    int nextTabId = 0;

    // The path itself. The name differs from the two accessors above, which return it.
    std::string serverExecutablePath;
};

#endif
