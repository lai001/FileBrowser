#ifndef CLIENT_SRC_FILE_BROWSER_UI_VIEW_H
#define CLIENT_SRC_FILE_BROWSER_UI_VIEW_H

#include <cstdint>
#include <string>
#include <vector>
#include "DirectoryTree.h"
#include "FileBrowserUIModel.h"
#include "ScanQueue.h"
#include "TransferQueue.h"

class FileBrowserUIController;

// View layer: everything that draws with ImGui. One window, one tab bar, one tab per entry, and inside a tab the
// launcher section, the system info summary, the browse panel, the transfer section and the right-click menu, in
// that order.
//
// The view runs no RPC and owns no worker: a widget that needs one calls the controller, which is what holds the
// queues. What the view writes back to the model is what the mouse was on - the marked row, the drop target, the
// row a right click landed on - because the menu and the drop callback read those after the rows are drawn. The
// fields the user types into (the executable path, address, port, path, download folder) are model data too, so
// the input widgets bind straight to them.
class FileBrowserUIView
{
  public:
    FileBrowserUIView(FileBrowserUIModel &model, FileBrowserUIController &controller);

    // Draws one frame. The controller calls this after it drained the worker results, so what is drawn is the
    // state the workers left behind.
    void render();

  private:
    // One tab per server. Clicking a tab is what selects that server, so the tab bar and the channel always
    // agree, and only the active tab is drawn: its content is therefore always for the selected entry. The "+"
    // trailing tab button is the only way to add a tab.
    void renderServerTabs();

    // What one tab shows: this server's launcher section, its system info, its current directory and cached
    // listing, and the status line for this entry. Only the active tab is drawn, so this is always called for
    // the selected entry.
    void renderServerTab(ServerEntry &entry);

    // The launcher section inside a tab: exe path, this entry's address/port, its pid/exit code, Start/Stop.
    // Item labels must stay unique within the window: item IDs are window-scoped, not tab-scoped, so an input
    // and a button with the same label share one ID, which asserts in ButtonBehavior.
    void renderServerLauncherSection(ServerEntry &entry);

    void renderSystemInfoSummary(const ServerEntry &entry);

    // The browse panel writes into the model (the path, the cache), so the entry is not const.
    void renderBrowsePanel(ServerEntry &entry);

    // Same reason for the tree: double clicking a node enters that directory, which updates the model.
    void renderTreeNode(ServerEntry &entry, const DirectoryTree &directoryTree, int32_t index);

    // The transfer part of a tab: the local folder a download writes into, the overwrite checkbox, where a drop
    // would go, and what the transfer worker has done so far. Transfers are not run here either: the buttons and
    // the menu hand the request to the controller, which puts it on the queue and the window keeps drawing.
    void renderTransferSection(ServerEntry &entry);

    // The menu a right click on a row opens. It is drawn once for the tab, not inside the row loop, because a
    // popup is identified by an id that has to stay the same across frames.
    void renderContextMenu(ServerEntry &entry);

    // The shell folder dialog, called from the two widgets that offer it. It blocks the thread that calls it,
    // which is the render thread: a modal dialog is allowed to do that, the window simply does not draw while it
    // is open. It writes the picked folder into the model; an empty result means the user cancelled.
    void chooseDownloadFolder(ServerEntry &entry);

    FileBrowserUIModel &model;
    FileBrowserUIController &controller;

    // Set by a right click on a row and consumed by the menu drawn after the rows. It lives here because only the
    // view uses it: the rows record it, the popup reads it in the same frame, and only one menu can be open
    // because only the active tab is drawn.
    bool contextMenuRequested = false;
};

#endif
