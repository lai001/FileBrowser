#include "TransferQueue.h"
#include "spdlog/spdlog.h"

TransferQueue::TransferQueue() : worker(&TransferQueue::run, this)
{
}

TransferQueue::~TransferQueue()
{
    {
        const std::lock_guard<std::mutex> lock(m);
        stopping = true;
        // A server that never answers keeps the worker inside Read() or Write(), and join() would wait for it.
        // Cancelling first is what makes the destructor return. TryCancel() is thread-safe, which is the only
        // reason this can be done from another thread at all.
        for (const auto &item : contexts)
        {
            item.second->TryCancel();
        }
    }

    cv.notify_all();
    worker.join();
}

bool TransferQueue::enqueue(std::shared_ptr<TransferJob> job)
{
    {
        const std::lock_guard<std::mutex> lock(m);
        if (jobs.count(job->tabId) != 0)
        {
            return false;
        }
        jobs[job->tabId] = job;
        pending.push_back(std::move(job));
    }

    cv.notify_one();
    return true;
}

std::vector<TransferResult> TransferQueue::takeResults()
{
    const std::lock_guard<std::mutex> lock(m);
    std::vector<TransferResult> ready;
    ready.swap(results);
    return ready;
}

bool TransferQueue::takeProgress(int tabId, TransferProgress &progress) const
{
    const std::lock_guard<std::mutex> lock(m);
    const auto job = jobs.find(tabId);
    if (job == jobs.end())
    {
        return false;
    }

    // The published counters only exist once the worker started the transfer; a job still in the queue has
    // nothing but its path, which is enough for the UI to say "queued".
    const auto published = progressByTab.find(tabId);
    if (published != progressByTab.end())
    {
        progress = published->second;
        progress.running = true;
    }

    progress.kind = job->second->kind;
    progress.remotePath = job->second->remotePath;
    progress.localRoot = job->second->localRoot;
    progress.elapsedMs = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - job->second->startedAt)
            .count());
    return true;
}

bool TransferQueue::hasJob(int tabId) const
{
    const std::lock_guard<std::mutex> lock(m);
    return jobs.count(tabId) != 0;
}

bool TransferQueue::cancel(int tabId)
{
    const std::lock_guard<std::mutex> lock(m);
    const auto job = jobs.find(tabId);
    if (job == jobs.end())
    {
        return false;
    }

    job->second->cancelRequested = true;

    // Only a transfer that is inside its RPC has a context. A job still in the queue gets the flag, which the
    // worker checks before it starts, and that is enough: there is nothing to break off.
    const auto context = contexts.find(tabId);
    if (context != contexts.end())
    {
        context->second->TryCancel();
    }

    return true;
}

void TransferQueue::deliver(TransferResult result)
{
    const std::lock_guard<std::mutex> lock(m);
    jobs.erase(result.tabId);
    results.push_back(std::move(result));
}

void TransferQueue::run()
{
    while (true)
    {
        std::shared_ptr<TransferJob> job;
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

        TransferResult result;
        result.tabId = job->tabId;
        result.key = job->key;
        result.kind = job->kind;
        result.remotePath = job->remotePath;
        result.localRoot = job->localRoot;
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

        TransferCounters counters;
        grpc::ClientContext context;

        {
            const std::lock_guard<std::mutex> lock(m);
            // Registering the context is what makes cancel() able to reach it, and having a progress entry is
            // what tells the UI this transfer is running rather than queued.
            contexts[job->tabId] = &context;
            TransferProgress progress;
            progress.kind = job->kind;
            progress.remotePath = job->remotePath;
            progress.localRoot = job->localRoot;
            progress.totalBytes = job->totalBytes;
            progress.doneAtStart = job->doneAtStart;
            progressByTab[job->tabId] = progress;
        }

        // The rate is measured between publish calls, so the time the job spent waiting in the queue is not part of
        // it. An interval that moved nothing leaves the anchor where it is, so the next rate covers the stall instead
        // of pretending it did not happen.
        std::chrono::steady_clock::time_point lastSampleAt = std::chrono::steady_clock::now();
        uint64_t lastSampledBytes = 0;
        uint64_t bytesPerSecond = 0;

        // The runner does the RPC and the local disk work; this is the only part the queue owns: copying what it
        // counted into the map the render thread reads.
        const PublishProgress publish = [this, job, &counters, &lastSampleAt, &lastSampledBytes, &bytesPerSecond] {
            const std::lock_guard<std::mutex> lock(m);
            const auto item = progressByTab.find(job->tabId);
            if (item == progressByTab.end())
            {
                return;
            }

            const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            const long long intervalMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSampleAt).count();
            const uint64_t moved = counters.bytes - lastSampledBytes;
            if (intervalMs > 0 && moved > 0)
            {
                const uint64_t instant = static_cast<uint64_t>(moved * 1000 / static_cast<uint64_t>(intervalMs));
                bytesPerSecond = bytesPerSecond == 0 ? instant : (bytesPerSecond * 3 + instant) / 4;
                lastSampleAt = now;
                lastSampledBytes = counters.bytes;
            }

            item->second.counters = counters;
            item->second.bytesPerSecond = bytesPerSecond;
            // What the estimate is made from: the total the job was built with, minus what an earlier run finished
            // and what this run has moved. The estimate is only shown when a total is known, so nothing is divided by
            // a number that is not there.
            const uint64_t done = job->doneAtStart + counters.bytes;
            const uint64_t remaining = job->totalBytes > done ? job->totalBytes - done : 0;
            item->second.estimatedRemainingMs =
                bytesPerSecond > 0 && remaining > 0 ? static_cast<long long>(remaining * 1000 / bytesPerSecond) : 0;
        };

        const grpc::Status status = job->runner(context, *job, counters, publish);

        {
            const std::lock_guard<std::mutex> lock(m);
            // The call has already returned, and the context is destroyed at the end of this iteration, so
            // removing it here is what keeps cancel() from reaching one that is about to go away.
            contexts.erase(job->tabId);
            progressByTab.erase(job->tabId);
        }

        result.status = status;
        result.cancelled = job->cancelRequested;
        // The map is only written on the publish interval, so the value the render thread last saw can be behind
        // what was actually done. These are the true totals for this transfer.
        result.counters = counters;
        result.elapsedMs = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - job->startedAt)
                .count());

        deliver(std::move(result));
    }
}
