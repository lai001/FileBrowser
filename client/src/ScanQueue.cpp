#include "ScanQueue.h"
#include "Misc.h"
#include "spdlog/spdlog.h"

namespace
{
// The counters are published every kProgressPublish entries. One mutex acquisition per file would make a large
// traversal compete with the render thread for a number nobody reads more than once per frame.
constexpr std::size_t kProgressPublish = 256;
} // namespace

ScanQueue::ScanQueue() : worker(&ScanQueue::run, this)
{
}

ScanQueue::~ScanQueue()
{
    {
        const std::lock_guard<std::mutex> lock(m);
        stopping = true;
        // A server that never answers keeps the worker inside Read(), and join() would wait for it. Cancelling
        // first is what makes the destructor return. TryCancel() is thread-safe, which is the only reason this
        // can be done from another thread at all.
        for (const auto &item : contexts)
        {
            item.second->TryCancel();
        }
    }

    cv.notify_all();
    worker.join();

    // Results the render thread never drained are freed here as well, by the unique_ptr in each ScanResult when
    // this object is destroyed. There is nothing left to free by hand.
}

bool ScanQueue::enqueue(int tabId, std::string key, std::string directory, ScanSource source)
{
    std::shared_ptr<ScanJob> job = std::make_shared<ScanJob>();
    job->tabId = tabId;
    job->key = std::move(key);
    job->directory = std::move(directory);
    job->source = std::move(source);
    job->startedAt = std::chrono::steady_clock::now();

    {
        const std::lock_guard<std::mutex> lock(m);
        if (jobs.count(tabId) != 0)
        {
            return false;
        }
        jobs[tabId] = job;
        pending.push_back(std::move(job));
    }

    cv.notify_one();
    return true;
}

std::vector<ScanResult> ScanQueue::takeResults()
{
    const std::lock_guard<std::mutex> lock(m);
    std::vector<ScanResult> ready;
    ready.swap(results);
    return ready;
}

bool ScanQueue::takeProgress(int tabId, ScanProgress &progress) const
{
    const std::lock_guard<std::mutex> lock(m);
    const auto job = jobs.find(tabId);
    if (job == jobs.end())
    {
        return false;
    }

    // The published counters only exist once the worker started the job; a job still in the queue has nothing
    // but its directory, which is enough for the UI to say "queued".
    const auto published = progressByTab.find(tabId);
    if (published != progressByTab.end())
    {
        progress = published->second;
        progress.running = true;
    }

    progress.directory = job->second->directory;
    progress.elapsedMs = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - job->second->startedAt)
            .count());
    return true;
}

bool ScanQueue::hasJob(int tabId) const
{
    const std::lock_guard<std::mutex> lock(m);
    return jobs.count(tabId) != 0;
}

bool ScanQueue::cancel(int tabId)
{
    const std::lock_guard<std::mutex> lock(m);
    const auto job = jobs.find(tabId);
    if (job == jobs.end())
    {
        return false;
    }

    job->second->cancelRequested = true;

    // Only a job that is inside its RPC has a context. A job still in the queue gets the flag, which the
    // worker checks before it starts, and that is enough: there is nothing to break off.
    const auto context = contexts.find(tabId);
    if (context != contexts.end())
    {
        context->second->TryCancel();
    }

    return true;
}

void ScanQueue::deliver(ScanResult result)
{
    const std::lock_guard<std::mutex> lock(m);
    jobs.erase(result.tabId);
    results.push_back(std::move(result));
}

void ScanQueue::run()
{
    while (true)
    {
        std::shared_ptr<ScanJob> job;
        bool cancelledBeforeItStarted = false;
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait(lock, [this] { return stopping || !pending.empty(); });
            if (stopping)
            {
                return;
            }
            job = pending.front();
            pending.pop_front();
            cancelledBeforeItStarted = job->cancelRequested;
        }

        ScanResult result;
        result.tabId = job->tabId;
        result.key = job->key;
        result.directory = job->directory;
        result.elapsedMs = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - job->startedAt)
                .count());

        if (cancelledBeforeItStarted)
        {
            result.status = grpc::Status(grpc::StatusCode::CANCELLED, "cancelled while it was still queued");
            result.cancelled = true;
            deliver(std::move(result));
            continue;
        }

        // The tree is built here, entry by entry. insertEntry links each node to its parent and adds its size to
        // every ancestor, so the totals are complete while the traversal is still running - the progress line can
        // show a size that is already meaningful, and there is no second pass over the tree.
        std::unique_ptr<DirectoryTree> tree = std::make_unique<DirectoryTree>(misc::utf8ToPath(job->directory));
        ScanProgress progress;
        grpc::ClientContext context;

        {
            const std::lock_guard<std::mutex> lock(m);
            // Registering the context is what makes cancel() able to reach it, and having a progress entry is
            // what tells the UI this job is running rather than queued.
            contexts[job->tabId] = &context;
            progressByTab[job->tabId] = progress;
        }

        // The sink does two things: it inserts what arrives into the tree, and it counts what has been read so
        // the render thread can show something while a big directory is being traversed. tree is captured by
        // reference: a unique_ptr cannot be captured by value, and the lambda has to be copyable anyway because it
        // is handed to the std::function the source takes. The sink does not outlive this iteration, so the
        // reference still names the same tree when the source runs.
        const EntrySink sink = [this, job, &tree, &progress](const ScanEntry &entry) -> bool {
            tree->insertEntry(entry);
            ++progress.entriesRead;
            if (entry.isDirectory)
            {
                ++progress.directories;
            }
            else
            {
                ++progress.files;
            }
            progress.bytes += entry.fileSize;

            if (progress.entriesRead % kProgressPublish == 0)
            {
                const std::lock_guard<std::mutex> lock(m);
                const auto item = progressByTab.find(job->tabId);
                if (item != progressByTab.end())
                {
                    item->second = progress;
                }
            }

            // false is how the source learns to stop. It cannot break a Read() that is already waiting, so the
            // TryCancel() from the UI is what ends a running call; this return value ends it between entries.
            return !job->cancelRequested;
        };

        const grpc::Status status = job->source(context, job->directory, sink);

        {
            const std::lock_guard<std::mutex> lock(m);
            // The call has already returned, and the context is destroyed at the end of this iteration, so
            // removing it here is what keeps cancel() from reaching one that is about to go away.
            contexts.erase(job->tabId);
            progressByTab.erase(job->tabId);
        }

        result.status = status;
        result.cancelled = job->cancelRequested;
        // The progress map is only written on the publish interval, so the value the render thread last saw can
        // be behind what was actually read. This is the true total for the job.
        result.entriesRead = progress.entriesRead;
        result.elapsedMs = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - job->startedAt)
                .count());

        // The tree is the result. It already holds the linked hierarchy and the sums, so nothing is rebuilt here;
        // keeping the scanned directory as the root is what makes the total the size of the directory that was
        // scanned, not of the whole drive. The counts are read before the tree moves, because after the move this
        // function no longer owns it.
        result.fileCount = tree->getFileCount();
        result.directoryCount = tree->getDirectoryCount();
        result.totalSize = tree->getTotalFileSize();
        result.tree = std::move(tree);

        deliver(std::move(result));
    }
}
