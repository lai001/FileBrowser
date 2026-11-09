#ifndef CLIENT_SRC_SCAN_QUEUE_H
#define CLIENT_SRC_SCAN_QUEUE_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "grpcpp/grpcpp.h"
#include "Client.h"

// One scan request. Everything the worker needs is on the job itself: a reference to a ServerEntry would not
// survive the tab being closed while the scan is running, and the source keeps its own shared_ptr to the client
// so the channel cannot be destroyed under a call that is still in flight.
struct ScanJob
{
    int tabId;
    std::string key;       // address:port, for the status line
    std::string directory; // UTF-8 path the traversal was started on
    ScanSource source;
    std::chrono::steady_clock::time_point startedAt;
    std::atomic<bool> cancelRequested{false};
};

// A finished scan. Ownership of the tree moves to whoever takes the result.
struct ScanResult
{
    int tabId;
    std::string key;
    std::string directory;
    grpc::Status status;
    // Empty only when the job was cancelled before the worker started it. Once the worker starts there is a
    // tree even for a failed or empty traversal: it holds at least the scanned directory node. The result owns it,
    // so exactly one place can free it and that is whoever took the result.
    std::unique_ptr<DirectoryTree> tree;
    std::size_t entriesRead = 0;
    std::size_t fileCount = 0;
    std::size_t directoryCount = 0;
    uint64_t totalSize = 0;
    long long elapsedMs = 0;
    bool cancelled = false;
};

// What a running job has read so far, for the progress line. These are entries the source emitted, not nodes
// in the tree: insertEntry adds the parent nodes the traversal never reported. running is what tells the UI a
// job is being worked on rather than waiting in the queue, and directory is the path the job was started on,
// not the path the panel is showing now (the field can be edited while the scan runs).
struct ScanProgress
{
    std::string directory;
    std::size_t entriesRead = 0;
    std::size_t files = 0;
    std::size_t directories = 0;
    uint64_t bytes = 0;
    long long elapsedMs = 0;
    bool running = false;
};

// The traversal is the RPC that can run for a long time - the two info calls have a deadline and it has none -
// so it cannot run on the render thread: the sync streaming API has to sit inside reader->Read() until the
// traversal ends, and that is what froze the UI. The
// queue is the handoff between the two threads. The render thread enqueues, reads progress, and drains finished
// results; the worker thread runs the RPC, builds the tree, and leaves the result here. Everything shared is
// guarded by one mutex, and the worker never touches a ServerEntry.
//
// One worker, so jobs run one at a time. A job whose server never answers keeps the worker until it is
// cancelled, which is why cancel() exists.
class ScanQueue
{
  public:
    ScanQueue();

    // Stops the worker. A call still in flight is cancelled first, otherwise this waits for a server that
    // never answers.
    ~ScanQueue();

    // False when this tab already has a job: two jobs for one tab would overwrite each other's progress.
    bool enqueue(int tabId, std::string key, std::string directory, ScanSource source);

    // Takes everything the worker finished. Called once per frame, on the render thread.
    std::vector<ScanResult> takeResults();

    // Snapshot for one tab. This function takes the mutex itself, so the UI never holds it while drawing the
    // progress line. running is false when the job is still queued. False when this tab has no job.
    bool takeProgress(int tabId, ScanProgress &progress) const;

    // True when this tab has a job queued or running.
    bool hasJob(int tabId) const;

    // Cancels the job. If its RPC is in flight, grpc::ClientContext::TryCancel() is what ends the stream, and it
    // is documented as thread-safe ("can be called multiple times from any thread", client_context.h). The
    // worker registers its context under this mutex and removes it before the context is destroyed, so a lookup
    // here can never reach a context that is already gone. TryCancel is best-effort: the source still has to
    // notice, which it does through the sink return value or through Read() returning false.
    bool cancel(int tabId);

  private:
    void run();

    // Pushing a finished result and forgetting the job is one step: the UI must not be able to see a job with
    // no result on its way.
    void deliver(ScanResult result);

    mutable std::mutex m;
    std::condition_variable cv;
    std::deque<std::shared_ptr<ScanJob>> pending;
    std::vector<ScanResult> results;
    // Every job that has not delivered yet, queued or running, so cancel() can find it either way.
    std::unordered_map<int, std::shared_ptr<ScanJob>> jobs;
    // Contexts of the jobs currently inside their RPC.
    std::unordered_map<int, grpc::ClientContext *> contexts;
    std::unordered_map<int, ScanProgress> progressByTab;
    bool stopping = false;
    std::thread worker;
};

#endif
