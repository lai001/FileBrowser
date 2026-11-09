#include "FileBrowserUIController.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include "spdlog/spdlog.h"
#include "Misc.h"
#include "Transfers.h"
#include "UiPaths.h"

// gRPC reads http_proxy/https_proxy from the environment for every channel target, so a local
// server can end up being reached through the system proxy: the proxy accepts the TCP connection,
// does not understand the HTTP/2 preface, closes it, and the call fails with
// UNAVAILABLE "End of TCP stream". gRPC also honours no_proxy, but only when the target host is
// listed there, so loopback targets are excluded from the proxy per channel instead of relying on
// the environment; ensureChannel() is where that flag is set.
static bool isLoopbackAddress(const std::string &address)
{
    std::string host = address;
    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
    {
        host = host.substr(1, host.size() - 2);
    }
    if (host.empty())
    {
        return false;
    }
    std::string lower;
    lower.reserve(host.size());
    for (char c : host)
    {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    // 127.0.0.0/8 is loopback as a whole, not only 127.0.0.1.
    return lower == "localhost" || lower == "::1" || lower == "0:0:0:0:0:0:1" || lower.rfind("127.", 0) == 0;
}

// The target for one entry. A named target goes through c-ares, and c-ares does not consult the hosts file:
// the first RPC on a new channel sends a real DNS query for the host even when the host is "localhost", and the
// whole query timeout is spent when the resolver forwards that query to a nameserver that cannot be reached
// before c-ares falls back to the localhost address (RFC 6761 says a resolver should answer localhost itself,
// and c-ares only does that after the query failed). Writing the loopback target as an address literal makes
// gRPC use its ipv4/ipv6 resolver, which does no lookup at all, so the first RPC costs only the connection
// attempt itself.
static std::string channelTarget(const std::string &address, int port)
{
    std::string host = address;
    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
    {
        host = host.substr(1, host.size() - 2);
    }
    if (host.empty())
    {
        host = "localhost";
    }

    const std::string portText = std::to_string(port);

    if (isLoopbackAddress(host))
    {
        if (host.find(':') != std::string::npos)
        {
            // An IPv6 literal needs brackets in an authority-style target.
            return "ipv6:[" + host + "]:" + portText;
        }
        return "ipv4:" + (host == "localhost" ? "127.0.0.1" : host) + ":" + portText;
    }

    // The resolver scheme has to be part of the target. With a bare "host:port" the proxy mapper
    // reads the host as the URI scheme, cannot split host and port out of it, and so never consults
    // no_proxy; dns:///host:port is the form that keeps no_proxy working.
    return "dns:///" + host + ":" + portText;
}

FileBrowserUIController::FileBrowserUIController() : model(), view(model, *this)
{
    // The state file for a resumable transfer lives next to the executable, which is what makes it survive the
    // client being closed. Reading it here is what lets the first transfer of this run know what an earlier run
    // did; a missing or unreadable file is not an error, it only means there is nothing to resume from.
    state = std::make_shared<TransferStateFile>(stateFilePath());
    state->load();

    // The default entry is created by the model; this is the first channel, and the only one built without a
    // widget being clicked.
    ensureChannel(model.selected());
}

FileBrowserUIController::~FileBrowserUIController()
{
    // Empty on purpose: every entry owns its ServerLauncher, and each launcher stops the child it started
    // when the entry is destroyed, so closing the client stops every server this client launched. The scan
    // worker is stopped by the ScanQueue member, and the transfer worker by the TransferQueue member: both cancel
    // a stream that is still in flight first, so the join does not wait for a server that never answers.
}

void FileBrowserUIController::render()
{
    // Results are applied here, once per frame, before anything is drawn. The worker thread never touches a
    // ServerEntry; it leaves its results in the queue, and this is the only place they become part of one.
    drainScanResults();
    drainTransferResults();

    view.render();
}

void FileBrowserUIController::ensureChannel(ServerEntry &entry)
{
    const std::string target = channelTarget(entry.address, entry.port);
    if (entry.channel != nullptr && entry.channelTarget == target)
    {
        return;
    }

    grpc::ChannelArguments args;
    if (isLoopbackAddress(entry.address))
    {
        args.SetInt("grpc.enable_http_proxy", 0);
    }
    // c-ares' default query timeout is 120 s, so a lookup that lands on a dead nameserver keeps the first
    // call of that channel pending for minutes before it reports anything. Capping it makes an unresolvable
    // name fail in a few seconds instead of hanging the render thread. A loopback target never reaches this
    // because it is written as an address literal, which needs no lookup.
    args.SetInt("grpc.dns_ares_query_timeout", 5000);

    // gRPC connects lazily, so this only describes the target address; nothing is waited for here. The
    // channel stays in the entry, so going back to a tab reuses its resolution instead of resolving again.
    entry.channelTarget = target;
    entry.channel = grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(), args);
    entry.client = std::make_shared<FileBrowserClient>(entry.channel);
}

void FileBrowserUIController::firstRead(ServerEntry &entry)
{
    entry.systemInfo.Clear();
    entry.roots.clear();

    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    const grpc::Status infoStatus = entry.client->getSystemInfo(entry.systemInfo);
    const grpc::Status rootsStatus = entry.client->getRootDirectories(entry.roots);
    spdlog::info(
        "First read of {} took {} ms", entry.key(),
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    if (!infoStatus.ok() || !rootsStatus.ok())
    {
        // The entry is not marked as read, so the next visit to this server tries again.
        entry.lastStatus = "First read of " + entry.key() +
                           " failed: " + (infoStatus.ok() ? rootsStatus.error_message() : infoStatus.error_message());
        return;
    }

    entry.firstConnection = false;
    entry.showRoots = true; // the first connection shows the roots
    entry.lastStatus = "First read of " + entry.key() + ": " + std::to_string(entry.roots.size()) + " roots";
}

bool FileBrowserUIController::listDirectory(ServerEntry &entry, std::string directory)
{
    directory = normalizeDirectory(directory);

    // Each entry owns its own stub, so the listing goes to the entry it belongs to. The browse panel only
    // draws the active tab, so this is always the selected entry.
    std::vector<pb::FileEntry> entries;
    const grpc::Status status = entry.client->getFiles(directory, entries);
    if (!status.ok())
    {
        // The cache is left as it was, so the panel still shows what this server answered before.
        entry.lastStatus = entry.key() + ": " + directory + " - " + status.error_message();
        return false;
    }

    // This listing is the cache a later visit to the same server shows.
    entry.cachedFiles = entries;
    entry.currentPath = directory;
    entry.showRoots = false;
    entry.lastStatus = entry.key() + ": " + directory + " (" + std::to_string(entries.size()) + " entries)";
    return true;
}

void FileBrowserUIController::selectServer(std::size_t index)
{
    if (index >= model.entries().size())
    {
        return;
    }

    // The view is on the tab it is drawing, so it is what tells the model which entry is selected.
    model.selectEntry(index);

    ServerEntry &entry = model.selected();
    ensureChannel(entry);

    // Selecting a tab does not read anything. Creating a tab and switching to it only builds the channel
    // description (lazy, no name lookup, no socket), and the first read happens when the Read button in that
    // tab is clicked, so a new tab stays inert until the user asks for it. The status line is left as it is
    // for an entry that has not been read yet, which is how "New tab localhost:7701" survives the switch.
    if (!entry.firstConnection)
    {
        if (entry.showRoots)
        {
            entry.lastStatus = "Cached roots for " + entry.key();
        }
        else
        {
            entry.lastStatus = "Cached listing for " + entry.key() + " at " + entry.currentPath;
        }
    }
}

void FileBrowserUIController::addServer()
{
    ServerEntry &entry = model.addEntry();
    entry.lastStatus = "New tab " + entry.key();
}

void FileBrowserUIController::startServer(ServerEntry &entry)
{
    spdlog::trace("Click start server button");
    entry.lastStatus = entry.launcher->start(model.serverExecutable(), entry.address, entry.port);
    if (entry.lastStatus.empty())
    {
        entry.lastStatus = "Started on " + entry.key();
    }
    else
    {
        // The reason is also shown in the UI status line; logging it keeps the log and the UI in
        // agreement, because a failed start does not produce any other log line.
        spdlog::warn("Start server: {}", entry.lastStatus);
    }
    // The tab being clicked is the selected entry, so its channel is already the one in use; this rebuilds
    // it only when the address or port was just edited, because the target is built from them.
    ensureChannel(entry);
}

void FileBrowserUIController::stopServer(ServerEntry &entry)
{
    spdlog::trace("Click stop server button");
    // Non-blocking: the message says which path was taken, and the tab keeps showing the process as
    // running until it actually exits.
    entry.lastStatus = entry.launcher->stop();
}

void FileBrowserUIController::showCachedRoots(ServerEntry &entry)
{
    entry.showRoots = true; // no RPC: the roots of this server are already cached
}

void FileBrowserUIController::loadTreeFromFile(ServerEntry &entry)
{
    spdlog::trace("Click load button");

    std::ifstream inputFile(entry.currentPath, std::ios::binary);
    if (inputFile.is_open())
    {
        pb::FileEntryCollection fileEntryCollection;
        if (fileEntryCollection.ParseFromIstream(&inputFile))
        {
            std::unique_ptr<DirectoryTree> newDirectoryTree = std::make_unique<DirectoryTree>();
            for (const auto &item : fileEntryCollection.fileentrires())
            {
                ScanEntry entryItem;
                entryItem.fileSize = item.filesize();
                entryItem.allocatedSize = item.allocatedsize();
                entryItem.fileIndex = item.fileindex();
                entryItem.filePath = misc::utf8ToPath(item.filepath());
                entryItem.isDirectory = item.isdirectory();
                newDirectoryTree->insertEntry(entryItem);
            }
            const std::string loadedRoot = pathToUtf8(newDirectoryTree->getRootPath());
            const std::size_t loadedFiles = newDirectoryTree->getFileCount();
            const std::size_t loadedDirectories = newDirectoryTree->getDirectoryCount();
            entry.scanRoot = loadedRoot;
            entry.directoryTree->replace(std::move(newDirectoryTree));
            entry.lastStatus = entry.key() + ": loaded " + std::to_string(loadedFiles) + " files and " +
                               std::to_string(loadedDirectories) + " directories from " + entry.currentPath;
        }
        else
        {
            spdlog::warn("Not support file type, {}", entry.currentPath);
        }
        inputFile.close();
    }
    else
    {
        spdlog::warn("Open file failed, {}", entry.currentPath);
    }
}

void FileBrowserUIController::scan(const std::string &rootPath)
{
    ServerEntry &entry = model.selected();
    if (rootPath.empty())
    {
        entry.lastStatus = "Scan needs a directory: type a path or double click a root first";
        return;
    }
    const std::string directory = normalizeDirectory(rootPath);

    // Nothing is called here: the request goes on the queue and the worker thread runs the RPC, so the window
    // keeps drawing while a large directory is traversed. The lambda keeps its own shared_ptr to the client, so
    // closing this tab while the scan is running cannot destroy the stub under a call that is still in flight.
    const std::shared_ptr<FileBrowserClient> client = entry.client;
    const bool queued = scanQueue.enqueue(
        entry.tabId, entry.key(), directory,
        [client](grpc::ClientContext &context, const std::string &scanned, const EntrySink &sink) -> grpc::Status {
            return client->recursiveDirectoryTraversal(context, scanned, sink);
        });
    if (!queued)
    {
        entry.lastStatus = entry.key() + ": a scan for this server is already running";
        return;
    }

    entry.lastStatus = entry.key() + ": scan queued for " + directory;
}

void FileBrowserUIController::handleFileDrop(const std::vector<std::string> &utf8Paths)
{
    if (utf8Paths.empty())
    {
        return;
    }

    ServerEntry &entry = model.selected();

    // Nothing was hovered, so the drop does nothing. That is the agreed behaviour: a drop only uploads to the row
    // the mouse is on, and a row that is not a directory row is handled by taking the directory it is in.
    if (entry.dropTargetPath.empty())
    {
        entry.lastStatus = entry.key() + ": drop ignored, the mouse was not on a row";
        spdlog::info("Drop: {} paths ignored, no row hovered", utf8Paths.size());
        return;
    }

    startUpload(entry, utf8Paths);
}

void FileBrowserUIController::startUpload(ServerEntry &entry, const std::vector<std::string> &droppedUtf8Paths)
{
    // One worker, and the progress line is per tab, so a second transfer for the same tab is refused instead of
    // being queued behind one the user cannot see.
    if (transferQueue.hasJob(entry.tabId))
    {
        entry.lastStatus = entry.key() + ": this tab already has a transfer";
        return;
    }

    // A channel is needed to send anything; a tab that has never been read has none yet.
    ensureChannel(entry);

    std::shared_ptr<TransferJob> job = std::make_shared<TransferJob>();
    job->tabId = entry.tabId;
    job->key = entry.key();
    job->kind = TransferKind::Upload;
    // The directory the mouse was on, which handleFileDrop already resolved. Nothing is written locally, so
    // localRoot stays empty and what was dropped is in localEntries.
    job->remotePath = entry.dropTargetPath;
    job->remoteIsDirectory = true;
    job->overwrite = entry.uploadOverwrite;
    for (const std::string &path : droppedUtf8Paths)
    {
        job->localEntries.push_back(misc::utf8ToPath(path));
    }
    job->startedAt = std::chrono::steady_clock::now();
    job->runner = makeUploadRunner(entry.client);
    job->state = state;

    // What the progress line measures this run against: the size of every file the drop gave, and the part of it the
    // state says an earlier run already finished for the same path. With overwrite asked for nothing is continued, so
    // the state does not count here. A file the walk cannot read is left out, as the producer would skip it too, and
    // so is a symlink. The state path is the target directory plus the chain below the dropped entry, which is the
    // way LocalUploadProducer builds it.
    const StateTransfer prior = state->find(TransferKind::Upload, entry.key(), job->remotePath);
    const auto countFile = [&](const std::filesystem::path &file, const std::filesystem::path &droppedRoot) {
        std::error_code ec;
        const uint64_t size = std::filesystem::file_size(file, ec);
        if (ec)
        {
            return;
        }
        job->totalBytes += size;
        if (job->overwrite)
        {
            return;
        }
        const StateEntry *recorded = prior.findEntry(job->remotePath + "/" + chainOf(file, droppedRoot.parent_path()));
        if (recorded != nullptr)
        {
            job->doneAtStart += std::min(recorded->bytesWritten, size);
        }
    };
    for (const std::filesystem::path &dropped : job->localEntries)
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(dropped, ec))
        {
            countFile(dropped, dropped);
            continue;
        }
        for (const auto &child : std::filesystem::recursive_directory_iterator(dropped, ec))
        {
            if (ec)
            {
                break;
            }
            if (!std::filesystem::is_symlink(child.path(), ec) && child.is_regular_file(ec))
            {
                countFile(child.path(), dropped);
            }
        }
    }

    const std::string target = job->remotePath;
    const std::size_t dropped = job->localEntries.size();
    if (!transferQueue.enqueue(std::move(job)))
    {
        entry.lastStatus = entry.key() + ": this tab already has a transfer";
        return;
    }

    spdlog::info("Drop: {} paths queued for upload to {}", dropped, target);
    entry.lastStatus = entry.key() + ": uploading " + std::to_string(dropped) + " paths to " + target;
    // What the state knows about this target. Entries it says were completed are sent as headers only, and entries
    // it stopped in are continued, so the line says which case this is instead of pretending the run is new.
    if (!prior.entries.empty())
    {
        entry.lastStatus +=
            " (continuing an earlier run, " + std::to_string(prior.entries.size()) + " entries recorded)";
    }
}

void FileBrowserUIController::startDownload(ServerEntry &entry)
{
    if (transferQueue.hasJob(entry.tabId))
    {
        entry.lastStatus = entry.key() + ": this tab already has a transfer";
        return;
    }

    if (entry.localDownloadPath.empty())
    {
        entry.lastStatus = entry.key() + ": the download folder is empty, set it in the transfer section first";
        return;
    }

    ensureChannel(entry);

    std::shared_ptr<TransferJob> job = std::make_shared<TransferJob>();
    job->tabId = entry.tabId;
    job->key = entry.key();
    job->kind = TransferKind::Download;
    // The row the right click landed on, recorded by the view before the menu was drawn.
    job->remotePath = entry.contextMenuPath;
    // Which of the two download RPCs the runner uses. A file row cannot be a directory, so this is the row the
    // right click landed on.
    job->remoteIsDirectory = entry.contextMenuIsDirectory;
    // Only fills the header the single file download synthesizes; a directory download does not use it.
    job->remoteFileSize = entry.contextMenuFileSize;
    // The folder the writer writes into. A directory download gets a folder named after the remote directory the
    // download started on, because that directory is never sent on the stream (parentIndex -1 is it) and the
    // writer only creates folders for entries it is given. A single file does not get a folder of its own: the
    // file is the entry.
    const std::filesystem::path folder = misc::utf8ToPath(entry.localDownloadPath);
    job->localRoot = job->remoteIsDirectory ? folder / misc::utf8ToPath(job->remotePath).filename() : folder;
    job->startedAt = std::chrono::steady_clock::now();
    job->runner = makeDownloadRunner(entry.client);
    job->state = state;

    const std::string remote = job->remotePath;
    const std::string local = misc::pathToUtf8(job->localRoot);

    // The same rule the runner applies, so this line can say what the run is continuing from: the first entry the
    // state recorded that the disk agrees is not finished. A single file has no recorded chain, so the file itself
    // is what the rule looks at.
    const StateTransfer prior = state->find(job->kind, job->key, local);

    // The denominator for the progress line: the sizes the state recorded for this transfer and the part of them an
    // earlier run finished. With nothing recorded, a single file is measured by the size the UI knew for it and a
    // directory by the node this tab scanned, which is 0 when the tree has no node for that path.
    for (const StateEntry &recorded : prior.entries)
    {
        job->totalBytes += recorded.fileSize;
        job->doneAtStart += std::min(recorded.bytesWritten, recorded.fileSize);
    }
    if (job->totalBytes == 0)
    {
        if (!job->remoteIsDirectory)
        {
            job->totalBytes = job->remoteFileSize;
        }
        else
        {
            const misc::LockGuard<DirectoryTree> tree = entry.directoryTree->lock();
            job->totalBytes = tree ? tree->sizeAt(misc::utf8ToPath(job->remotePath)) : 0;
        }
    }

    if (!transferQueue.enqueue(std::move(job)))
    {
        entry.lastStatus = entry.key() + ": this tab already has a transfer";
        return;
    }

    entry.lastStatus = entry.key() + ": downloading " + remote + " to " + local;
    // The whole tree is streamed so every entry can be compared with what is on disk, so what an earlier run left
    // behind is only reported here: this run still walks from the beginning.
    if (!prior.entries.empty())
    {
        entry.lastStatus += " (" + std::to_string(prior.entries.size()) + " entries recorded by an earlier run)";
    }
}

void FileBrowserUIController::cancelScan(int tabId)
{
    scanQueue.cancel(tabId);
}

void FileBrowserUIController::cancelTransfer(int tabId)
{
    transferQueue.cancel(tabId);
}

bool FileBrowserUIController::hasScanJob(int tabId) const
{
    return scanQueue.hasJob(tabId);
}

bool FileBrowserUIController::scanProgress(int tabId, ScanProgress &progress) const
{
    return scanQueue.takeProgress(tabId, progress);
}

bool FileBrowserUIController::hasTransferJob(int tabId) const
{
    return transferQueue.hasJob(tabId);
}

bool FileBrowserUIController::transferProgress(int tabId, TransferProgress &progress) const
{
    return transferQueue.takeProgress(tabId, progress);
}

void FileBrowserUIController::drainScanResults()
{
    for (ScanResult &result : scanQueue.takeResults())
    {
        ServerEntry *entry = model.findEntryByTabId(result.tabId);
        if (entry == nullptr)
        {
            // The tab was closed while its scan was running. The result owns the tree, so releasing it here is
            // what frees it: nothing else can reach it, because entries only change on the render thread.
            result.tree.reset();
            spdlog::info("Scan: result for a closed tab, {}", result.directory);
            continue;
        }

        if (result.tree)
        {
            entry->scanRoot = result.directory;
            // The tree moves out of the result into the Guarded, which is the owner from here; the tree this
            // entry held before is freed inside replace().
            entry->directoryTree->replace(std::move(result.tree));
        }

        const std::string elapsed = std::to_string(result.elapsedMs) + " ms";
        if (result.cancelled)
        {
            entry->lastStatus = entry->key() + ": scan of " + result.directory + " cancelled after " +
                                std::to_string(result.entriesRead) + " entries (" + elapsed + ")";
        }
        else if (!result.status.ok())
        {
            entry->lastStatus =
                entry->key() + ": scan of " + result.directory + " failed - " + result.status.error_message();
        }
        else
        {
            // The folder sizes are the ones the worker summed from the entries the traversal returned: the
            // scanner reports 0 for a directory itself, so a directory row only has a size after a scan. The
            // directory count also includes the parent nodes insertEntry created, so it can be larger than the
            // number of directory entries the server sent.
            entry->lastStatus = entry->key() + ": scanned " + result.directory + " (" +
                                std::to_string(result.fileCount) + " files, " + std::to_string(result.directoryCount) +
                                " directories, " + misc::formatSize(result.totalSize) + ", " + elapsed + ")";
        }
    }
}

void FileBrowserUIController::drainTransferResults()
{
    for (const TransferResult &result : transferQueue.takeResults())
    {
        ServerEntry *entry = model.findEntryByTabId(result.tabId);
        if (entry == nullptr)
        {
            // The tab was closed while the transfer was running. Nothing already written on the local disk is
            // undone by that, so this only logs what happened.
            spdlog::info("Transfer: result for a closed tab, {}", result.remotePath);
            continue;
        }

        const char *what = result.kind == TransferKind::Upload ? "upload" : "download";
        const std::string elapsed = std::to_string(result.elapsedMs) + " ms";

        if (result.cancelled)
        {
            entry->lastStatus = entry->key() + ": " + what + " of " + result.remotePath + " cancelled after " +
                                std::to_string(result.counters.entries) + " entries (" + elapsed + ")";
        }
        else if (!result.status.ok())
        {
            entry->lastStatus =
                entry->key() + ": " + what + " of " + result.remotePath + " failed - " + result.status.error_message();
        }
        else
        {
            std::string line = entry->key() + ": " + what + " " + result.remotePath + " (" +
                               std::to_string(result.counters.files) + " files, " +
                               std::to_string(result.counters.directories) + " directories, " +
                               misc::formatSize(result.counters.bytes);
            // An entry whose content was already the same as what the server sent is left alone, so a number here
            // means some entries were not written. An upload cannot skip anything it was asked to send, so this only
            // appears for a download.
            if (result.counters.skipped > 0)
            {
                line += ", " + std::to_string(result.counters.skipped) + " skipped";
            }
            // Entries the hash in the state proved without any content for them: no header for them arrived, so they
            // are not in the entry count either.
            if (result.counters.verified > 0)
            {
                line += ", " + std::to_string(result.counters.verified) + " verified";
            }
            // The average for the whole run: the finished line has both the bytes and the time, so it can state a
            // rate. While the transfer runs the line shows the smoothed one instead.
            if (result.counters.bytes > 0 && result.elapsedMs > 0)
            {
                const uint64_t rate = result.counters.bytes * 1000 / static_cast<uint64_t>(result.elapsedMs);
                line += ", " + misc::formatSize(rate) + "/s";
            }
            line += ", " + elapsed + ")";
            if (result.kind == TransferKind::Download)
            {
                line += " to " + misc::pathToUtf8(result.localRoot);
            }
            entry->lastStatus = line;
        }
    }
}
