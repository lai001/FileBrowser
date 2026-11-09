#ifndef CLIENT_SRC_TRANSFER_QUEUE_H
#define CLIENT_SRC_TRANSFER_QUEUE_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "grpcpp/grpcpp.h"
#include "TransferState.h"

// TransferKind comes from TransferState.h, which is where the state file is keyed by it.

// What one transfer has done so far. entries counts the entries the transfer accounted for: for an upload the
// headers it sent, the first being the target directory; for a download the headers it received. bytes counts the
// bytes that actually moved. files counts the entries that were written (a download) or sent (an upload); an entry
// left alone is counted in skipped instead. directories counts directory headers: for a download the ones this
// writer created, for an upload the ones it sent. skipped means an entry this run wrote nothing for: a download
// counts an entry whose content matched what was already on disk (part of it can have been proven by the recorded
// hash instead of compared), and an upload reports an entry an earlier run had finished. verified counts the
// entries a download left alone because the hash the state has for them still matched the bytes on disk - no header
// for them arrived, so they are not in entries.
struct TransferCounters
{
    std::size_t entries = 0;
    std::size_t files = 0;
    std::size_t directories = 0;
    std::size_t skipped = 0;
    std::size_t verified = 0;
    uint64_t bytes = 0;
    // Bytes a download read from disk to check the hash the state had for an entry. Nothing moved over the RPC for
    // them, so they are not in bytes; the progress line shows them because that check can take seconds with nothing
    // coming from the server.
    uint64_t hashChecked = 0;
};

struct TransferJob;

// One transfer. The runner is what talks to the server and to the local disk; the queue only schedules it, so a
// test can put a runner on the queue that does no RPC at all (see client/test/main.cpp). It reports through
// counters and publish(): a transfer is measured in chunks, so taking the mutex once per chunk of a large file
// would make the transfer compete with the render thread for a number that is read once per frame - publish() is
// called on an interval instead.
using PublishProgress = std::function<void()>;
using TransferRunner = std::function<grpc::Status(grpc::ClientContext &context, const TransferJob &job,
                                                  TransferCounters &counters, const PublishProgress &publish)>;

// Everything the runner needs is on the job: a reference to a ServerEntry would not survive the tab being closed
// while the transfer is running, and the runner keeps its own shared_ptr to the client, so the channel cannot be
// destroyed under a call that is still in flight.
struct TransferJob
{
    int tabId = 0;
    std::string key; // address:port, for the status line
    TransferKind kind = TransferKind::Download;

    // Upload: the server directory to write into. Download: the file or directory to read. Both as UTF-8, as the
    // RPC takes them.
    std::string remotePath;
    // Download: which of the two download RPCs to use - a single file has no header, so the runner builds one.
    // An upload target is always a directory.
    bool remoteIsDirectory = false;
    // Download: the size the UI knew for that file, only used to fill the header it synthesizes. 0 when unknown.
    uint64_t remoteFileSize = 0;

    // Upload: what the client puts in header.overwrite for every file. False is the server's default and the UI
    // checkbox default: an upload must not silently replace a file that is already on the server.
    bool overwrite = false;

    // Download: the local directory to write into. Upload: empty, because nothing is written locally; what was
    // dropped is in localEntries.
    std::filesystem::path localRoot;
    // Upload: the paths dropped on the window. A download has no local input, so this stays empty.
    std::vector<std::filesystem::path> localEntries;

    // What this transfer covers, as far as that can be known before it runs: the sizes of the dropped entries for an
    // upload, the entries an earlier run recorded for a transfer that continues, or the size the UI knew for a
    // single file. 0 when nothing is known, which is what keeps the progress line from showing a percentage or an
    // estimate it cannot back up.
    uint64_t totalBytes = 0;
    // Bytes an earlier run had already finished when this run started, read from the state. This run does no work for
    // them, but they belong to the total, so a run that continues one shows where the whole transfer already is
    // instead of starting at 0%.
    uint64_t doneAtStart = 0;

    // The state file that makes a transfer resumable, shared by every transfer: the worker is the only thread that
    // records into it while a transfer runs, and the render thread reads it before a job is built. Null for a
    // transfer that does not use the state.
    std::shared_ptr<TransferStateFile> state;

    TransferRunner runner;
    std::chrono::steady_clock::time_point startedAt;
    std::atomic<bool> cancelRequested{false};
};

// What a running transfer has done so far, for the progress line. running is what tells the UI a transfer is
// being worked on rather than waiting in the queue.
struct TransferProgress
{
    TransferKind kind = TransferKind::Download;
    std::string remotePath;
    std::filesystem::path localRoot;
    TransferCounters counters;
    long long elapsedMs = 0;
    bool running = false;
    // Copied from the job when it is registered, so the line can measure what has moved against what the transfer
    // covers. totalBytes 0 means nothing is known, and then there is no percentage and no estimate.
    uint64_t totalBytes = 0;
    uint64_t doneAtStart = 0;
    // Bytes that moved per second. Measured between publish calls and mixed a quarter of each interval into the
    // previous value, so one large chunk does not make the number jump; the first interval that both moved bytes and
    // took time gives its own rate. 0 until there is such an interval.
    uint64_t bytesPerSecond = 0;
    // The bytes still to move at that rate. 0 when there is no total, no rate, or nothing left, which is what keeps
    // the line from stating an estimate it cannot back up.
    long long estimatedRemainingMs = 0;
};

// A finished transfer.
struct TransferResult
{
    int tabId = 0;
    std::string key;
    TransferKind kind = TransferKind::Download;
    std::string remotePath;
    std::filesystem::path localRoot;
    grpc::Status status;
    bool cancelled = false;
    TransferCounters counters;
    long long elapsedMs = 0;
};

// The handoff between the render thread and a transfer worker, in the same shape as ScanQueue: the render thread
// enqueues, reads progress, and drains finished results; the worker runs the RPC, does the local disk work, and
// leaves the result here. Everything shared is guarded by one mutex, and the worker never touches a ServerEntry.
//
// One worker, so transfers run one at a time. A transfer whose server never answers keeps the worker until it is
// cancelled, which is why cancel() exists. A transfer is not cancelled by the progress publish interval: the
// runner has to notice the flag, which it does inside its sink or producer.
class TransferQueue
{
  public:
    TransferQueue();

    // Stops the worker. A transfer still in flight is cancelled first, otherwise this waits for a server that
    // never answers.
    ~TransferQueue();

    // False when this tab already has a transfer: two transfers for one tab would overwrite each other's
    // progress, and the runner is built for one job.
    bool enqueue(std::shared_ptr<TransferJob> job);

    // Takes everything the worker finished. Called once per frame, on the render thread.
    std::vector<TransferResult> takeResults();

    // Snapshot for one tab. This function takes the mutex itself, so the UI never holds it while drawing the
    // progress line. False when this tab has no transfer.
    bool takeProgress(int tabId, TransferProgress &progress) const;

    // True when this tab has a transfer queued or running.
    bool hasJob(int tabId) const;

    // Cancels the transfer. If its RPC is in flight, grpc::ClientContext::TryCancel() is what ends the stream;
    // the worker registers its context under this mutex and removes it before the context is destroyed.
    bool cancel(int tabId);

  private:
    void run();

    // Pushing a finished result and forgetting the job is one step: the UI must not be able to see a job with no
    // result on its way.
    void deliver(TransferResult result);

    mutable std::mutex m;
    std::condition_variable cv;
    std::deque<std::shared_ptr<TransferJob>> pending;
    std::vector<TransferResult> results;
    // Every transfer that has not delivered yet, queued or running, so cancel() can find it either way.
    std::unordered_map<int, std::shared_ptr<TransferJob>> jobs;
    // Contexts of the transfers currently inside their RPC.
    std::unordered_map<int, grpc::ClientContext *> contexts;
    std::unordered_map<int, TransferProgress> progressByTab;
    bool stopping = false;
    std::thread worker;
};

#endif
