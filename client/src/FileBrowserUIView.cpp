#include "FileBrowserUIView.h"
#include <algorithm>
#include "spdlog/spdlog.h"
#include "imgui.h"
#include "Misc.h"
#include "FileBrowserUIController.h"
#include "UiPaths.h"

// windows.h is already in through ServerLauncher.h (with NOMINMAX and WIN32_LEAN_AND_MEAN), so this is only the
// shell dialog header the folder picker needs.
#if _WIN32
#include <shobjidl.h>
#endif

// A Selectable row reports a single click, and ImGui reports the double click separately through
// io.MouseDoubleClicked for the item under the mouse. The directory rows in the browse panel mark the
// row on the first click and enter the directory on the second, so this is checked right after the
// row is drawn.
static bool doubleClickedCurrentItem()
{
    return ImGui::IsItemHovered() && ImGui::GetIO().MouseDoubleClicked[ImGuiMouseButton_Left];
}

// ImGui renders textured quads and its own font atlas, it has no SVG support, so an icon is either a
// texture the renderer already knows or something drawn with the draw list. These glyphs are drawn here:
// no asset to load and nothing to cache, and the size follows the font size so they scale with the UI.
static void drawItemIcon(ImDrawList *drawList, const ImVec2 &rowTop, float fontSize, bool isDirectory)
{
    const float glyph = fontSize * 0.80f; // slightly smaller than the text line so it reads as an icon
    const float x = rowTop.x;
    const float y = rowTop.y + (fontSize - glyph) * 0.5f;

    if (isDirectory)
    {
        const ImU32 folderColor = IM_COL32(214, 176, 96, 255);
        const float bodyTop = y + glyph * 0.36f;
        drawList->AddRectFilled(ImVec2(x, y + glyph * 0.16f), ImVec2(x + glyph * 0.44f, bodyTop), folderColor, 1.0f);
        drawList->AddRectFilled(ImVec2(x, bodyTop), ImVec2(x + glyph, y + glyph), folderColor, 1.5f);
        return;
    }

    const ImU32 fileColor = IM_COL32(150, 160, 174, 255);
    const ImU32 foldColor = IM_COL32(108, 118, 132, 255);
    const float cutX = x + glyph * 0.72f;
    const float cutY = y + glyph * 0.16f;

    // Body with the top right corner cut off, then the fold as two lines.
    drawList->PathLineTo(ImVec2(x + glyph * 0.12f, y));
    drawList->PathLineTo(ImVec2(cutX, y));
    drawList->PathLineTo(ImVec2(cutX + glyph * 0.16f, cutY));
    drawList->PathLineTo(ImVec2(cutX + glyph * 0.16f, y + glyph));
    drawList->PathLineTo(ImVec2(x + glyph * 0.12f, y + glyph));
    drawList->PathFillConvex(fileColor);

    drawList->PathLineTo(ImVec2(cutX, y));
    drawList->PathLineTo(ImVec2(cutX, cutY));
    drawList->PathLineTo(ImVec2(cutX + glyph * 0.16f, cutY));
    drawList->PathStroke(foldColor, 0, 1.0f);
}

// ImGui has no file dialogs, so the folder picker is the shell one. It blocks the thread that calls it, which is
// the render thread: a modal dialog is allowed to do that, the window simply does not draw while it is open. COM
// has to be initialised around the call because CoCreateInstance is what creates the dialog object, and the single
// threaded apartment is the model this shell dialog wants. The client has no other COM user. An empty string means
// the user cancelled, or COM could not be initialised.
#if _WIN32
static std::string pickFolder(const std::string &startPathUtf8)
{
    const HRESULT initialised = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialised))
    {
        spdlog::warn("Folder picker: COM could not be initialised on this thread");
        return std::string();
    }

    IFileOpenDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        CoUninitialize();
        return std::string();
    }

    dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (!startPathUtf8.empty())
    {
        IShellItem *startItem = nullptr;
        // utf8ToWstring keeps the terminator, so the string carries one inside it; the API stops at the first.
        const std::wstring startWide = misc::utf8ToWstring(startPathUtf8);
        if (SUCCEEDED(SHCreateItemFromParsingName(startWide.c_str(), nullptr, IID_PPV_ARGS(&startItem))))
        {
            dialog->SetFolder(startItem);
        }
        if (startItem != nullptr)
        {
            startItem->Release();
        }
    }

    std::string picked;
    if (SUCCEEDED(dialog->Show(nullptr)))
    {
        IShellItem *result = nullptr;
        if (SUCCEEDED(dialog->GetResult(&result)))
        {
            // IShellItem gives the path through GetDisplayName; GetParsingPath is the IShellItem2 version of the
            // same thing and this interface is the one the dialog returns.
            PWSTR path = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                picked = misc::wStringToString(std::wstring(path));
                CoTaskMemFree(path);
            }
            if (result != nullptr)
            {
                result->Release();
            }
        }
    }

    dialog->Release();
    CoUninitialize();
    return picked;
}
#else
// The client is only built on Windows (root CMakeLists.txt), so this is the branch that never runs. Without it
// there is no shell dialog to call and the path has to be typed into the field instead.
static std::string pickFolder(const std::string &startPathUtf8)
{
    return startPathUtf8;
}
#endif

static int InputTextCallback(ImGuiInputTextCallbackData *data)
{
    // Only the path input asks for this event (see renderBrowsePanel); the other inputs share this callback
    // without the flag. Returning non-zero drops the character, and ImGui runs the filter on pasted text as
    // well, so a pasted path loses the illegal characters before they reach the buffer.
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter)
    {
        return isIllegalPathChar(static_cast<char32_t>(data->EventChar)) ? 1 : 0;
    }

    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        std::string *str = reinterpret_cast<std::string *>(data->UserData);
        IM_ASSERT(data->Buf == str->c_str());
        str->resize(data->BufTextLen);
        data->Buf = const_cast<char *>(str->c_str());
    }
    return 0;
}

FileBrowserUIView::FileBrowserUIView(FileBrowserUIModel &model, FileBrowserUIController &controller)
    : model(model), controller(controller)
{
}

void FileBrowserUIView::render()
{
    // The window stays: it is the scope every widget needs (item IDs, clipping, hover, focus), and a tab bar
    // cannot exist outside one. Only its decorations are removed: no title bar, no border, no resize. Its
    // background is still drawn, so the widgets sit on the WindowBg colour instead of on the clear colour.
    // Position and size are set every frame instead of FirstUseEver: with NoMove and NoResize a remembered
    // size would survive a restart, and DisplaySize is whatever the backend reported for this frame, so the
    // window follows the GLFW window as it is resized.
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    // The background covers the whole window, so this padding only decides where the content starts: with the
    // default inset the first row would not sit at the top left corner.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    // No decoration and no resize; the mouse wheel over the window still scrolls the content, only the
    // scrollbar itself is hidden.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("FileBrowser", nullptr, flags);

    // One window with one tab bar: one tab per server, and each tab carries its own launcher section.
    renderServerTabs();

    ImGui::End();
    ImGui::PopStyleVar(3);
}

void FileBrowserUIView::renderServerTabs()
{
    std::vector<ServerEntry> &entries = model.entries();
    const std::size_t noServer = entries.size();
    std::size_t removeIndex = noServer;

    // One tab per server. Clicking a tab is what selects that server, so the tab bar and the channel always
    // agree, and only the active tab is drawn: its content is therefore always for the selected entry.
    // AutoSelectNewTabs is the only way to select a tab from code in ImGui 1.89.2: a tab that appears gets
    // NextSelectedTabId, which is applied at EndTabBar, so the new tab becomes active on the next frame. Being
    // active is not a read: its first read happens when that tab's Read button is clicked.
    if (ImGui::BeginTabBar("Servers", ImGuiTabBarFlags_AutoSelectNewTabs))
    {
        // The "+" after the last tab, and the only way to create a tab now. It is submitted before the tabs,
        // but ImGuiTabItemFlags_Trailing keeps it at the right end of the tab bar; a tab button cannot be
        // selected, so clicking it only runs the action here and the tab it creates is the one that becomes
        // active. The new entry takes the default address and port, which can then be edited inside its tab.
        // Labels below stay unique within the window: item IDs are window-scoped, not tab-scoped, so two
        // widgets sharing a label would share one ID.
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
        {
            controller.addServer();
        }

        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            ServerEntry &entry = entries[i];
            // PushID with the entry's own id: the widget labels inside a tab (address, port, Start server) are
            // the same for every entry, and item IDs are window-scoped, so without this two tabs would share
            // one ID for the address field.
            ImGui::PushID(entry.tabId);

            // The tab title is the address:port, and the label is also what ImGui hashes into the tab ID
            // (TabBarCalcTabID calls window->GetID(label)). That made typing in the address field change the
            // tab ID: the tab was no longer the selected one, its content was not drawn on that frame, the
            // input was not processed, ActiveId was cleared at EndFrame and focus was lost after one
            // character. "display###id" is the way out of this: only the part after ### is hashed and only the
            // part before it is displayed, so the tab keeps its ID while the title still follows edits.
            const std::string tabLabel = entry.key() + "###" + std::to_string(entry.tabId);

            // Every tab can be closed, which removes that entry. The last remaining tab has no close button
            // because the channel always needs one entry.
            bool open = true;
            const bool active =
                (noServer == 1) ? ImGui::BeginTabItem(tabLabel.c_str()) : ImGui::BeginTabItem(tabLabel.c_str(), &open);
            if (!active)
            {
                if (!open)
                // A closed tab removes that server, but only after the loop: the loop is iterating the
                // same vector erase() would change. The entry's launcher stops its child process, so
                // closing a tab stops that server (up to 2 seconds for the graceful path, which blocks
                // this frame).
                {
                    removeIndex = i;
                }
                ImGui::PopID();
                continue;
            }

            if (model.selectedIndex() != i)
            {
                controller.selectServer(i);
            }
            renderServerTab(entry);

            ImGui::EndTabItem();
            ImGui::PopID();
        }

        ImGui::EndTabBar();
    }

    if (removeIndex < noServer)
    {
        model.removeEntry(removeIndex);
        model.selected().lastStatus = "Server removed";
        controller.ensureChannel(model.selected());
    }
}

void FileBrowserUIView::renderServerTab(ServerEntry &entry)
{
    // One tab = one server: this entry's launcher section, its system info, its current directory and cached
    // listing, and the status line for this entry.
    renderServerLauncherSection(entry);
    ImGui::Separator();
    renderSystemInfoSummary(entry);
    ImGui::Separator();
    renderBrowsePanel(entry);

    if (!entry.lastStatus.empty())
    {
        ImGui::Text("%s", entry.lastStatus.c_str());
    }
}

void FileBrowserUIView::renderServerLauncherSection(ServerEntry &entry)
{
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;

    // Item widths are set explicitly here. CalcItemWidth() is the default item width (the whole content width
    // of the window), not what is left on the current row, so a full-width input followed by SameLine() puts
    // the next item past the right edge of the window, where it is clipped: the port value, the "Read" button
    // and the path field were only visible after scrolling. Fixed widths keep every row inside the window.
    ImGui::PushItemWidth(320);
    // One executable path for every tab; what differs per tab is the address and port this entry starts on.
    std::string &serverExecutable = model.serverExecutable();
    ImGui::InputTextWithHint("Server exe", "Path of FileBrowserServer.exe",
                             const_cast<char *>(serverExecutable.c_str()), serverExecutable.capacity() + 1, flags,
                             InputTextCallback, reinterpret_cast<void *>(&serverExecutable));
    ImGui::PopItemWidth();

    ImGui::PushItemWidth(150);
    ImGui::InputTextWithHint("address", "localhost", const_cast<char *>(entry.address.c_str()),
                             entry.address.capacity() + 1, flags, InputTextCallback,
                             reinterpret_cast<void *>(&entry.address));
    ImGui::SameLine();
    // InputInt uses the item width for the whole group: the value field plus the two -/+ buttons, so the width
    // has to leave room for both buttons and for four digits (90 only showed one character).
    ImGui::PushItemWidth(130);
    ImGui::InputInt("port", &entry.port, 1, 1000);
    ImGui::PopItemWidth();
    ImGui::PopItemWidth();
    // The server command line option only accepts 1-65535 (CLI::Range), so clamp here instead
    // of letting the child process reject the value.
    model.clampPort(entry);

    ImGui::SameLine();
    if (entry.launcher->isRunning())
    {
        ImGui::Text("pid %d", entry.launcher->processId());
    }
    else
    {
        ImGui::Text("not running, exit code %d", entry.launcher->exitCode());
    }

    ImGui::BeginDisabled(entry.launcher->isRunning());
    if (ImGui::Button("Start server"))
    {
        controller.startServer(entry);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(!entry.launcher->isRunning());
    if (ImGui::Button("Stop server"))
    {
        controller.stopServer(entry);
    }
    ImGui::EndDisabled();
}

void FileBrowserUIView::renderSystemInfoSummary(const ServerEntry &entry)
{
    if (entry.firstConnection)
    {
        ImGui::Text("Not read yet, the Read button below is what reads it");
        return;
    }
    ImGui::Text("%s %s, %s, %u cores, %s", entry.systemInfo.platform().c_str(), entry.systemInfo.osversion().c_str(),
                entry.systemInfo.hostname().c_str(), entry.systemInfo.logicalcpucount(),
                misc::formatSize(entry.systemInfo.totalphysicalmemory()).c_str());
}

void FileBrowserUIView::renderBrowsePanel(ServerEntry &entry)
{
    // CharFilter is asked for in this panel only: this is the input where a typed or pasted character can make
    // the path unusable on the server (see InputTextCallback and UiPaths).
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackCharFilter;

    // The Read button is only drawn for an entry that has not been read once yet; it is what fills the system
    // info. Listing is possible before that too - the List button below runs the RPC and its result becomes the
    // cache a later visit shows.
    if (entry.firstConnection)
    {
        if (ImGui::Button("Read"))
        {
            controller.firstRead(entry);
        }
    }

    // The directory the panel is on, drawn as text before the input so it stays readable while another
    // path is being typed. Double clicking a directory row below is what changes it.
    ImGui::Text("Current directory: %s",
                entry.currentPath.empty() ? "(nothing listed yet)" : entry.currentPath.c_str());

    // Same reason as in the launcher section: the input has to be narrower than the row, otherwise the List
    // and Roots buttons are drawn past the right edge of the window.
    ImGui::PushItemWidth(320);
    ImGui::InputTextWithHint("CurrentPath", "Type path here", const_cast<char *>(entry.currentPath.c_str()),
                             entry.currentPath.capacity() + 1, flags, InputTextCallback,
                             reinterpret_cast<void *>(&entry.currentPath));
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::Button("List"))
    {
        controller.listDirectory(entry, entry.currentPath);
    }
    ImGui::SameLine();
    if (ImGui::Button("Roots"))
    {
        controller.showCachedRoots(entry);
    }

    // A scan runs on the queue's worker thread, so this row only hands the request over and the window keeps
    // drawing while the traversal runs. While a job for this tab exists the button is Cancel instead: that is
    // what ends a stream that would otherwise run until the server finishes. The line below is what the worker
    // has read so far, which is the only thing the render thread can show while a big directory is being
    // traversed.
    ImGui::SameLine();
    const bool scanRunning = controller.hasScanJob(entry.tabId);
    if (scanRunning)
    {
        if (ImGui::Button("Cancel scan"))
        {
            controller.cancelScan(entry.tabId);
        }
    }
    else
    {
        if (ImGui::Button("Scan"))
        {
            controller.scan(entry.currentPath);
        }
    }

    ScanProgress progress;
    if (scanRunning && controller.scanProgress(entry.tabId, progress))
    {
        if (progress.running)
        {
            ImGui::Text("Scanning %s (%lld ms, %zu entries: %zu files, %zu directories, %s so far)",
                        progress.directory.c_str(), progress.elapsedMs, progress.entriesRead, progress.files,
                        progress.directories, misc::formatSize(progress.bytes).c_str());
        }
        else
        {
            // Not running yet: there is one worker thread, so this job is waiting for the one in front of it.
            ImGui::Text("Scan queued for %s (the worker is on another scan)", progress.directory.c_str());
        }
    }

    // One directory row: the first click marks it, the second click on the same row enters it. Roots,
    // the parent directory and the cached listing all go through this so they behave identically.
    // AllowDoubleClick is required here: a plain Selectable() only returns on the release, so the second
    // press itself never reaches this code. With the flag, ButtonBehavior() reports the press of the second
    // click as pressed, and the double click itself comes from io.MouseDoubleClicked for the hovered row.
    //
    // A double click only records the path; the RPC runs after the rows are drawn. listDirectory replaces
    // entry.cachedFiles, and the listing loop is iterating that same vector, so acting inside a row would
    // let the range-for continue over destroyed elements.
    const float fontSize = ImGui::GetFontSize();
    const float glyphSize = fontSize * 0.80f;
    const float iconAdvance = glyphSize + ImGui::GetStyle().ItemInnerSpacing.x;

    // The drop target is recomputed from scratch every frame: a frame with nothing hovered has no target, so a
    // row that is no longer under the mouse must not keep the value it had. The GLFW drop callback runs before
    // render(), which is why it uses the value this frame recorded (see the controller's handleFileDrop).
    entry.dropTargetPath.clear();

    std::string pendingOpenPath;
    bool rowDoubleClicked = false;
    auto directoryRow = [&](const std::string &path, const std::string &label, bool isDirectory, uint64_t fileSize) {
        const ImVec2 rowTop = ImGui::GetCursorScreenPos();
        const bool isSelected = entry.highlightedPath == path;

        // The icon takes the left part of the row, the label starts after it.
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + iconAdvance);
        const bool clicked = ImGui::Selectable(label.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick);
        const bool hovered = ImGui::IsItemHovered();
        const bool doubleClicked = clicked && doubleClickedCurrentItem();

        // A drop from Explorer arrives for the window as a whole, so the row the mouse is on is what says where
        // the files go (see the controller's handleFileDrop). A file row gives the directory that file is in.
        if (hovered)
        {
            entry.dropTargetPath = isDirectory ? path : parentDirectory(path);
        }
        // A right click records the row; the menu itself is drawn once for the tab, after the rows.
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            entry.contextMenuPath = path;
            entry.contextMenuIsDirectory = isDirectory;
            entry.contextMenuFileSize = fileSize;
            contextMenuRequested = true;
        }

        // Selectable fills from its own cursor, which is already past the icon, so the icon area gets the same
        // color here, extended by half the item spacing that separates two rows, and the highlight stays one
        // continuous block instead of starting after the icon.
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        if (isSelected || hovered)
        {
            const ImGuiStyle &style = ImGui::GetStyle();
            const ImU32 headColor =
                ImGui::GetColorU32(hovered && !isSelected ? ImGuiCol_HeaderHovered : ImGuiCol_Header);
            const float halfX = style.ItemSpacing.x * 0.5f;
            const float halfY = style.ItemSpacing.y * 0.5f;
            const ImVec2 boxMin(rowTop.x - halfX, rowTop.y - halfY);
            const ImVec2 boxMax(rowTop.x + iconAdvance, rowTop.y + fontSize + (style.ItemSpacing.y - halfY));
            drawList->AddRectFilled(boxMin, boxMax, headColor, 0.0f);
        }
        drawItemIcon(drawList, rowTop, fontSize, isDirectory);

        if (!clicked)
        {
            return;
        }
        entry.highlightedPath = path;
        // Copy the path: it can be a reference into entry.cachedFiles, which listDirectory replaces.
        if (doubleClicked)
        {
            pendingOpenPath = path;
            rowDoubleClicked = true;
        }
    };

    if (entry.showRoots)
    {
        ImGui::Text("Roots of %s", entry.key().c_str());
        if (entry.roots.empty())
        {
            ImGui::Text("No roots cached for this server");
        }
        for (const std::string &root : entry.roots)
        {
            directoryRow(root, root, true, 0);
        }
    }
    else
    {
        // A directory size only exists in the scanned tree: the scanner reports 0 for a directory itself, so the
        // folder sizes shown here are the ones this client summed from the entries the traversal returned. The
        // tree is locked once for the whole listing and the double click is handled after the rows are drawn.
        // listDirectory does not take this lock, so calling it after the rows cannot re-enter it; the same holds
        // for renderTreeNode, called inside this scope, whose double click runs an RPC while the lock is held.
        misc::LockGuard<DirectoryTree> tree = entry.directoryTree->lock();
        auto treeSize = [&tree](const std::string &utf8Path) -> uint64_t {
            if (!tree)
            {
                return 0;
            }
            return tree->sizeAt(toTreePath(utf8Path));
        };

        uint64_t totalSize = 0;
        for (const pb::FileEntry &item : entry.cachedFiles)
        {
            totalSize += item.isdirectory() ? treeSize(item.filepath()) : item.filesize();
        }
        ImGui::Text("Cached listing (%zu entries, %s)", entry.cachedFiles.size(), misc::formatSize(totalSize).c_str());
        if (!tree)
        {
            ImGui::Text("Directory sizes need a scan: run Scan on this listing to get them");
        }

        // Walking one level up is a row like any other, so ".. " carries the parent path it enters.
        const std::string parent = parentDirectory(entry.currentPath);
        if (!parent.empty())
        {
            directoryRow(parent, ".. (" + parent + ")", true, 0);
        }

        // The directory itself takes the next row. A drop only uploads to the row the mouse is on (see
        // handleFileDrop), and a listing of nothing but directories has no file row to hover, so without this row
        // there is no way to name the current directory as the target. No size is shown: the line above already
        // sums exactly these entries. Marking it highlights the row, and double clicking lists it again, which is
        // what the List button does for the same path.
        if (!entry.currentPath.empty())
        {
            directoryRow(entry.currentPath, ". (" + entry.currentPath + ")", true, 0);
        }

        for (const pb::FileEntry &item : entry.cachedFiles)
        {
            const uint64_t size = item.isdirectory() ? treeSize(item.filepath()) : item.filesize();
            const std::string label = item.filepath() + " (" + misc::formatSize(size) + ")";
            if (item.isdirectory())
            {
                // A cached entry can be entered because this is the listing of the selected server.
                directoryRow(item.filepath(), label, true, size);
            }
            else
            {
                // Files are not selectable: they are only read through the Load file button, so the row is
                // plain text with its icon.
                const ImVec2 rowTop = ImGui::GetCursorScreenPos();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + iconAdvance);
                ImGui::Text("%s", label.c_str());

                // IsItemHovered and IsItemClicked also work for a plain Text item: hover only needs the item's
                // bounding rectangle, not an id. They have to be read before the icon is drawn, because drawing
                // with the draw list does not change which item counts as the last one. The rectangle is the text
                // itself, so the icon area is not part of the row for a hover.
                const bool hoveredFile = ImGui::IsItemHovered();
                const bool rightClickedFile = ImGui::IsItemClicked(ImGuiMouseButton_Right);
                if (hoveredFile)
                {
                    entry.dropTargetPath = parentDirectory(item.filepath());
                }
                if (rightClickedFile)
                {
                    entry.contextMenuPath = item.filepath();
                    entry.contextMenuIsDirectory = false;
                    entry.contextMenuFileSize = size;
                    contextMenuRequested = true;
                }

                drawItemIcon(ImGui::GetWindowDrawList(), rowTop, fontSize, false);
            }
        }
    }

    // The rows are drawn, so nothing is iterating the cache any more: this is where the double click is
    // acted on.
    if (rowDoubleClicked)
    {
        controller.listDirectory(entry, pendingOpenPath);
    }

    if (ImGui::Button("Load file"))
    {
        controller.loadTreeFromFile(entry);
    }

    ImGui::Separator();

    {
        misc::LockGuard<DirectoryTree> directoryTreeProtect = entry.directoryTree->lock();
        if (directoryTreeProtect)
        {
            if (directoryTreeProtect->getEntryCount() == 0)
            {
                ImGui::Text("No scanned tree for this server");
            }
            else
            {
                const std::filesystem::path root = directoryTreeProtect->getScanRoot().empty()
                                                       ? directoryTreeProtect->getRootPath()
                                                       : directoryTreeProtect->getScanRoot();
                ImGui::Text("Scan tree root: %s", pathToUtf8(root).c_str());
                ImGui::Text("Sizes below it: %zu files, %zu directories, %s (on disk %s)",
                            directoryTreeProtect->getFileCount(), directoryTreeProtect->getDirectoryCount(),
                            misc::formatSize(directoryTreeProtect->getTotalFileSize()).c_str(),
                            misc::formatSize(directoryTreeProtect->getTotalAllocatedSize()).c_str());
                renderTreeNode(entry, *directoryTreeProtect.get(), directoryTreeProtect->getRootIndex());
            }
        }
    }

    ImGui::Separator();
    renderTransferSection(entry);

    // The menu is drawn once for the tab, after the rows and the tree: the row the right click landed on only
    // records the path, and a popup has to be opened at the same id level it is read at.
    renderContextMenu(entry);
}

void FileBrowserUIView::renderTreeNode(ServerEntry &entry, const DirectoryTree &directoryTree, int32_t index)
{
    const TreeEntry &node = directoryTree.entryAt(index);
    std::string label = node.name.u8string();
    if (label.size() == 0)
    {
        // insertNode keeps the whole path as the name of a root, so this fallback only covers a node whose name
        // is empty.
        label = directoryTree.pathOf(index).u8string();
    }
    const bool isDirectory = node.isDirectory;
    const std::string fileSizeStr = misc::formatSize(node.fileSize);
    label = fmt::format("{} ({})", label, fileSizeStr);
    if (!isDirectory && node.duplicate)
    {
        // The size of this row is not in the totals above it: the same file was counted on the path that was
        // seen first.
        label += " [hard link]";
    }
    ImGuiTreeNodeFlags flag = ImGuiTreeNodeFlags_None;
    if (!isDirectory)
    {
        flag |= ImGuiTreeNodeFlags_Leaf;
    }
    const bool opened = ImGui::TreeNodeEx(label.c_str(), flag);

    // The same two interactions as the listing rows: the node the mouse is on is where a dropped file would go,
    // and a right click records this node for the menu. A file node gives the directory it is in.
    const std::filesystem::path nodePath = directoryTree.pathOf(index);
    if (ImGui::IsItemHovered())
    {
        entry.dropTargetPath = isDirectory ? pathToUtf8(nodePath) : pathToUtf8(nodePath.parent_path());
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        entry.contextMenuPath = pathToUtf8(nodePath);
        entry.contextMenuIsDirectory = isDirectory;
        entry.contextMenuFileSize = isDirectory ? 0 : node.fileSize;
        contextMenuRequested = true;
    }

    // Double clicking a node enters that directory in the browse panel, the same way the cached
    // listing does. This does not touch the tree, so it is safe while its lock is held.
    if (isDirectory && doubleClickedCurrentItem())
    {
        entry.highlightedPath = pathToUtf8(nodePath);
        controller.listDirectory(entry, entry.highlightedPath);
    }

    if (opened)
    {
        std::vector<int32_t> sorted = node.children;
        std::sort(sorted.begin(), sorted.end(), [&directoryTree](int32_t lhs, int32_t rhs) {
            return directoryTree.entryAt(lhs).fileSize > directoryTree.entryAt(rhs).fileSize;
        });
        for (int32_t child : sorted)
        {
            renderTreeNode(entry, directoryTree, child);
        }
        ImGui::TreePop();
    }
}

void FileBrowserUIView::renderTransferSection(ServerEntry &entry)
{
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;

    // The local folder a download writes into, one per tab. The right-click menu uses whatever this field holds
    // at the moment it is clicked, so the folder has to be set before a file is downloaded.
    ImGui::Text("Transfer");
    ImGui::PushItemWidth(320);
    ImGui::InputTextWithHint("Download folder", "Local folder for downloads",
                             const_cast<char *>(entry.localDownloadPath.c_str()),
                             entry.localDownloadPath.capacity() + 1, flags, InputTextCallback,
                             reinterpret_cast<void *>(&entry.localDownloadPath));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Pick folder"))
    {
        chooseDownloadFolder(entry);
    }

    ImGui::SameLine();
    ImGui::Checkbox("Overwrite on the server", &entry.uploadOverwrite);

    // Where a file dropped from Explorer would go. The window as a whole receives the drop, so this line is what
    // shows which row the mouse is on: nothing hovered means a drop does nothing.
    ImGui::Text("Drop files onto a row: %s",
                entry.dropTargetPath.empty() ? "(no row hovered)" : entry.dropTargetPath.c_str());

    // Transfers run on the transfer worker thread, so this row only shows what it has done so far. One worker, so
    // Cancel is what ends a transfer that would otherwise run until the server finishes.
    const bool transferRunning = controller.hasTransferJob(entry.tabId);
    if (transferRunning)
    {
        if (ImGui::Button("Cancel transfer"))
        {
            controller.cancelTransfer(entry.tabId);
        }
    }

    TransferProgress progress;
    if (transferRunning && controller.transferProgress(entry.tabId, progress))
    {
        const char *what = progress.kind == TransferKind::Upload ? "Uploading" : "Downloading";
        if (progress.running)
        {
            // The line is built instead of being one format string so that the two parts only appear when there is
            // something to show: a run that compares every entry and proves nothing still reads the same as it did.
            std::string line =
                std::string(what) + " " + progress.remotePath + " (" + std::to_string(progress.elapsedMs) + " ms";
            // The percentage only means something against a total, so it appears only when the job was built with
            // one. It counts what an earlier run finished plus what this run moved, so a run that continues one does
            // not start at 0%. The denominator is what was known at the time the job was built, so a run that finds
            // more on the server than that can read past 100%.
            if (progress.totalBytes > 0)
            {
                const uint64_t done = progress.doneAtStart + progress.counters.bytes;
                line += ", " + std::to_string(done * 100 / progress.totalBytes) + "% of " +
                        misc::formatSize(progress.totalBytes);
            }
            if (progress.bytesPerSecond > 0)
            {
                line += ", " + misc::formatSize(progress.bytesPerSecond) + "/s";
                if (progress.estimatedRemainingMs > 0)
                {
                    line += ", about " + misc::formatDuration(progress.estimatedRemainingMs) + " left";
                }
            }
            // Bytes read from disk to check the hashes the state recorded. This comes before anything from the
            // server, so without it a run that proves a large prefix looks like it is waiting.
            if (progress.counters.hashChecked > 0)
            {
                line += ", verifying the recorded hash: " + misc::formatSize(progress.counters.hashChecked) + " read";
            }
            line += ", " + std::to_string(progress.counters.entries) +
                    " entries: " + std::to_string(progress.counters.files) + " files, " +
                    std::to_string(progress.counters.directories) + " directories, " +
                    misc::formatSize(progress.counters.bytes) + ", " + std::to_string(progress.counters.skipped) +
                    " skipped";
            if (progress.counters.verified > 0)
            {
                line += ", " + std::to_string(progress.counters.verified) + " verified";
            }
            line += ")";
            ImGui::Text("%s", line.c_str());
        }
        else
        {
            // Not running yet: there is one worker thread, so this transfer is waiting for the one in front of it.
            ImGui::Text("Transfer queued for %s (the worker is on another transfer)", progress.remotePath.c_str());
        }
    }
}

void FileBrowserUIView::chooseDownloadFolder(ServerEntry &entry)
{
    const std::string picked = pickFolder(entry.localDownloadPath);
    if (!picked.empty())
    {
        entry.localDownloadPath = picked;
    }
}

void FileBrowserUIView::renderContextMenu(ServerEntry &entry)
{
    // One id for the whole tab. A popup is identified by an id that has to stay the same across frames, which is
    // why the menu cannot be opened from inside the row loop: the id there would change with every row drawn.
    ImGui::PushID("FileBrowser row menu");

    // OpenPopup only on the frame the click happened; BeginPopup below is what keeps the menu open afterwards.
    if (contextMenuRequested)
    {
        ImGui::OpenPopup("row");
    }

    if (ImGui::BeginPopup("row"))
    {
        ImGui::TextUnformatted(entry.contextMenuPath.c_str());

        if (entry.localDownloadPath.empty())
        {
            // Nothing to write into yet, so the item says why it is not available instead of doing nothing.
            ImGui::TextDisabled("No download folder yet");
        }
        else if (ImGui::MenuItem(("Download to " + entry.localDownloadPath).c_str()))
        {
            controller.startDownload(entry);
        }

        if (ImGui::MenuItem("Choose the download folder..."))
        {
            chooseDownloadFolder(entry);
        }

        ImGui::EndPopup();
    }

    ImGui::PopID();
    contextMenuRequested = false;
}
