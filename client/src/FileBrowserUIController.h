#ifndef CLIENT_SRC_FILE_BROWSER_UI_CONTROLLER_H
#define CLIENT_SRC_FILE_BROWSER_UI_CONTROLLER_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "FileBrowserUIModel.h"
#include "FileBrowserUIView.h"
#include "ScanQueue.h"
#include "TransferQueue.h"
#include "TransferState.h"

// Controller layer: it turns what the view reports into changes to the model, and it is what runs the calls - the
// RPCs on the render thread, the channels, and the two worker queues. It owns the model and the view; the view
// calls back into it when a widget is activated.
//
// The render thread is the only thread that touches the entries, so nothing here needs a lock beyond the tree the
// workers fill: a worker never reaches a ServerEntry, it leaves its result in its queue, and the drains at the
// start of render() are the only place a result becomes part of an entry.
class FileBrowserUIController
{
  public:
    // Builds the state file for the default entry and its channel. The model creates the default entry; the
    // channel is built here because building it is the controller's work, not the model's.
    FileBrowserUIController();

    // Empty on purpose: every entry owns its ServerLauncher, and each launcher stops the child it started when
    // the entry is destroyed, so closing the client stops every server this client launched. The scan worker is
    // stopped by the ScanQueue member and the transfer worker by the TransferQueue member; both cancel a stream
    // that is still in flight first, so the join does not wait for a server that never answers.
    ~FileBrowserUIController();

    // One frame: the worker results are applied here, once per frame, before anything is drawn, and then the view
    // draws the state they left behind. Called from client/bin/main.cpp on the render thread.
    void render();

    // The entry points that are not widgets. scan() is what the Scan button asks for and handleFileDrop() is what
    // the GLFW drop callback calls; both work on the selected entry, which is the tab the user is looking at.
    void scan(const std::string &rootPath);
    void handleFileDrop(const std::vector<std::string> &utf8Paths);

    // The actions a widget triggers. Each one writes the status line of the entry it belongs to, which is what
    // the tab shows on the same frame.
    void selectServer(std::size_t index);
    void addServer();
    void startServer(ServerEntry &entry);
    void stopServer(ServerEntry &entry);
    void firstRead(ServerEntry &entry);
    // The path is taken by value: a caller can hand over a string that lives inside entry.cachedFiles, and this
    // function replaces that vector.
    bool listDirectory(ServerEntry &entry, std::string directory);
    void showCachedRoots(ServerEntry &entry);
    void loadTreeFromFile(ServerEntry &entry);
    void startUpload(ServerEntry &entry, const std::vector<std::string> &droppedUtf8Paths);
    void startDownload(ServerEntry &entry);
    void cancelScan(int tabId);
    void cancelTransfer(int tabId);

    // The channel for an entry, built once and kept, so switching tabs does not build a new one. An action that
    // sends anything builds it first, because a tab that has never been read has no channel yet.
    void ensureChannel(ServerEntry &entry);

    // What the view needs for the two progress lines: whether this tab has a job, and what the worker has done
    // so far. takeProgress clears what it hands over, so a progress line only changes when there is new work.
    bool hasScanJob(int tabId) const;
    bool scanProgress(int tabId, ScanProgress &progress) const;
    bool hasTransferJob(int tabId) const;
    bool transferProgress(int tabId, TransferProgress &progress) const;

  private:
    void drainScanResults();
    void drainTransferResults();

    FileBrowserUIModel model;

    // Declared after the model so it is constructed after it, and destroyed before it; it only holds references
    // to the model and to this controller.
    FileBrowserUIView view;

    ScanQueue scanQueue;

    // shared_ptr, not a member object: a job holds a reference to it, so a job that is still running cannot be
    // left with a dangling pointer when the controller is destroyed.
    std::shared_ptr<TransferStateFile> state;

    TransferQueue transferQueue;
};

#endif
