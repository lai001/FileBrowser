#include "Transfers.h"
#include <algorithm>
#include <cstring>
#include <ios>
#include "absl/crc/crc32c.h"
#include "Misc.h"
#include "spdlog/spdlog.h"

// Declared in Transfers.h: the controller builds the same paths for the progress estimate, so this is shared rather
// than kept in an anonymous namespace.
std::string chainOf(const std::filesystem::path &path, const std::filesystem::path &root)
{
    std::string chain;
    for (const auto &part : path.lexically_relative(root))
    {
        if (!chain.empty())
        {
            chain += "/";
        }
        chain += misc::pathToUtf8(part);
    }
    return chain;
}

grpc::Status localFailure(const char *what, const std::filesystem::path &path, const std::error_code &ec)
{
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        std::string(what) + ": \"" + misc::pathToUtf8(path) + "\" - " + ec.message());
}

int64_t bytesAlreadyThere(const std::filesystem::path &target, bool isDirectory)
{
    std::error_code ec;
    if (!std::filesystem::exists(target, ec))
    {
        return -1;
    }
    if (isDirectory)
    {
        // Nothing has to be written for a directory, so being there is what complete means. A name that is there
        // but is not a directory is not this entry, so it counts as not there.
        return std::filesystem::is_directory(target, ec) ? 0 : -1;
    }
    const uint64_t size = std::filesystem::file_size(target, ec);
    if (ec)
    {
        spdlog::warn("Transfer: could not read the size of \"{}\" - {}", misc::pathToUtf8(target), ec.message());
        return -1;
    }
    return static_cast<int64_t>(size);
}

std::optional<uint32_t> hashPrefix(const std::filesystem::path &file, uint64_t bytes, const HashProgress &report)
{
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open())
    {
        return std::nullopt;
    }

    // Read piece by piece, so a file that is only being checked is never read into memory as a whole.
    std::vector<char> buffer;
    buffer.resize(kTransferChunkSize);
    absl::crc32c_t crc{0};
    uint64_t hashed = 0;
    // Bytes read since the last report. A check is the part of a run that can take seconds without anything coming
    // from the server, so the number goes out on the interval the progress already uses, not once per piece.
    uint64_t reported = 0;
    while (hashed < bytes)
    {
        const std::size_t want =
            static_cast<std::size_t>(std::min<uint64_t>(bytes - hashed, static_cast<uint64_t>(kTransferChunkSize)));
        in.read(buffer.data(), static_cast<std::streamsize>(want));
        const std::streamsize got = in.gcount();
        if (got <= 0)
        {
            // Fewer bytes than the hash claims: what it was made from is not there, so there is nothing to prove.
            return std::nullopt;
        }
        crc = absl::ExtendCrc32c(crc, absl::string_view(buffer.data(), static_cast<std::size_t>(got)));
        hashed += static_cast<uint64_t>(got);
        reported += static_cast<uint64_t>(got);
        if (reported >= kTransferChunkSize * kTransferProgressPublish && report)
        {
            report(reported);
            reported = 0;
        }
    }
    if (reported > 0 && report)
    {
        report(reported);
    }
    return static_cast<uint32_t>(crc);
}

DownloadPlan planDownload(const StateContext &state, const std::vector<std::string> &currentOrder,
                          const HashProgress &report)
{
    DownloadPlan plan;
    if (state.prior.entries.empty())
    {
        return plan;
    }

    // Skipping is decided on positions, and a position is only the same entry in two runs when the traversal came
    // out in the same order. An order that does not match entry for entry therefore skips nothing: every entry gets
    // its header and is compared as it always was.
    if (currentOrder.size() != state.prior.entries.size())
    {
        return plan;
    }

    for (std::size_t i = 0; i < state.prior.entries.size(); ++i)
    {
        const StateEntry &entry = state.prior.entries[i];
        if (entry.path != currentOrder[i])
        {
            return plan;
        }

        if (entry.isDirectory)
        {
            // Nothing has to be proven for a directory and no content comes for one, so being there is enough. One
            // that is not there has to come back as a header, because the entries below it point at it.
            if (bytesAlreadyThere(misc::utf8ToPath(entry.path), true) < 0)
            {
                plan.startEntryIndex = i;
                return plan;
            }
            plan.left.push_back(entry);
            continue;
        }

        if (entry.hashedBytes == 0 || entry.hashedBytes > entry.fileSize)
        {
            // Nothing recorded, or a hash covering more than this entry has: it cannot be trusted, so the whole
            // entry is compared from its first byte as before.
            plan.startEntryIndex = i;
            return plan;
        }

        const std::optional<uint32_t> local = hashPrefix(misc::utf8ToPath(entry.path), entry.hashedBytes, report);
        if (!local)
        {
            // The bytes the hash was made from are not on disk any more, so the whole entry has to come back.
            plan.startEntryIndex = i;
            plan.rewriteStartEntry = true;
            return plan;
        }
        if (*local != entry.contentHash)
        {
            // What is on disk is not what an earlier run hashed. What a hash cannot prove is not kept, so the entry
            // is written over in full, from the first byte the stream is asked for.
            spdlog::warn("Download: the hash of \"{}\" does not match the state, the entry is written over",
                         entry.path);
            plan.startEntryIndex = i;
            plan.rewriteStartEntry = true;
            return plan;
        }

        ++plan.verifiedFiles;
        if (entry.hashedBytes == entry.fileSize)
        {
            // The whole entry is proven, so nothing is asked for it.
            plan.left.push_back(entry);
            continue;
        }

        // The prefix is proven and the rest of the entry still has to arrive: this is where the stream starts.
        plan.startEntryIndex = i;
        plan.startOffset = entry.hashedBytes;
        plan.startHash = entry.contentHash;
        return plan;
    }

    // Every entry the state knows is proven, so no content is asked for at all.
    plan.nothingToFetch = true;
    return plan;
}

void LocalDownloadWriter::record(const StateEntry &entry)
{
    if (state.file == nullptr)
    {
        return;
    }
    state.file->record(state.kind, state.key, state.root, entry);
    if (++stateRecords % kTransferProgressPublish == 0)
    {
        state.file->save();
    }
}

void LocalDownloadWriter::saveState()
{
    if (state.file != nullptr)
    {
        state.file->save();
    }
}

std::string LocalDownloadWriter::statePathFor(int32_t parentIndex, const std::string &name) const
{
    if (parentIndex < 0)
    {
        return state.root + "/" + name;
    }
    return statePathByIndex[static_cast<std::size_t>(parentIndex)] + "/" + name;
}

void LocalDownloadWriter::finishCurrentEntry()
{
    if (comparingCurrentEntry && !wroteInCurrentEntry)
    {
        // Nothing in this entry differed from what was already on disk, so the two contents are the same and this
        // run wrote nothing for it. Part of it can have been proven by the recorded hash instead of read, because
        // the plan started the stream after that part. That is the entry skipped counts.
        ++counters.skipped;
        spdlog::info("Download: \"{}\" matches what is on disk, nothing written", misc::pathToUtf8(currentFile));
    }
    comparingCurrentEntry = false;
    wroteInCurrentEntry = false;
}

LocalDownloadWriter::~LocalDownloadWriter()
{
    // The last entry has no following header to finish it, and the counters are read after the writer is gone.
    finishCurrentEntry();
}

LocalDownloadWriter::LocalDownloadWriter(const std::filesystem::path &root, TransferCounters &counters,
                                         const std::atomic<bool> &cancelRequested, const StateContext &state,
                                         const DownloadPlan &plan)
    : root(root), counters(counters), cancelRequested(cancelRequested), state(state), plan(plan)
{
    // The folder has to exist before the first file can be opened in it. A failure here is recorded as a
    // failure of the transfer, not as an error of the stream.
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec)
    {
        failure = localFailure("could not create the download folder", root, ec);
        return;
    }

    // The entries the plan left alone never come as headers, but the parentIndex of an entry that does come can
    // point at one of them, so their paths go into the two tables here in traversal order. A parent is always
    // recorded before its child, so each path is rebuilt from the entry's own name. Nothing is created for them:
    // the plan only leaves an entry alone when it is already on disk.
    for (const StateEntry &entry : plan.left)
    {
        const std::filesystem::path parent =
            entry.parentIndex < 0 ? root : pathByIndex[static_cast<std::size_t>(entry.parentIndex)];
        pathByIndex.push_back(parent / misc::utf8ToPath(entry.name));
        statePathByIndex.push_back(entry.path);
    }

    // What the plan proved is counted before any message arrives, so a run that asked for nothing still reports
    // what it decided.
    counters.verified += plan.verifiedFiles;
}

bool LocalDownloadWriter::write(const pb::DownloadItem &item)
{
    if (cancelRequested)
    {
        return false;
    }

    if (item.size() == 0)
    {
        // A header. The position is recorded for the entries below it, and the path is rebuilt from the parent
        // chain the same way the client tree does.
        const int32_t parentIndex = item.parentindex();
        std::filesystem::path target;
        std::string entryPath;
        if (parentIndex >= 0)
        {
            if (static_cast<std::size_t>(parentIndex) >= pathByIndex.size())
            {
                // The server sends an entry only after its header, so this cannot happen on a stream that was
                // read from the start. The slot is still filled, with an empty path, so the entries after it keep
                // counting against the same positions; only this one entry is dropped.
                spdlog::warn("Download: \"{}\" points at parent index {}", item.name(), parentIndex);
                pathByIndex.push_back(std::filesystem::path{});
                statePathByIndex.push_back(std::string{});
                return !cancelRequested;
            }
            target = pathByIndex[static_cast<std::size_t>(parentIndex)] / misc::utf8ToPath(item.name());
            entryPath = statePathFor(parentIndex, item.name());
        }
        else
        {
            // -1 is the directory the download was started on, which is not sent: the root this writer was given.
            target = root / misc::utf8ToPath(item.name());
            entryPath = statePathFor(-1, item.name());
        }

        const int32_t position = static_cast<int32_t>(pathByIndex.size());
        pathByIndex.push_back(target);
        statePathByIndex.push_back(entryPath);
        ++counters.entries;
        // The entry before this header is finished: what it was counted as depends on whether anything in it
        // differed from what was already on disk, and that is only known once its last chunk has been read.
        finishCurrentEntry();

        // The entry the plan decided to start the stream at carries the hash its proven prefix already had, so the
        // chunks after it are hashed on from there. Every other entry starts with nothing proven, and with an empty
        // plan this is the first entry with an empty hash, which is what the plain rule does.
        const bool isStartEntry = static_cast<std::size_t>(position) == plan.startEntryIndex;
        currentRecord = StateEntry{position,
                                   item.name(),
                                   parentIndex,
                                   item.isdirectory(),
                                   item.filesize(),
                                   0,
                                   0,
                                   entryPath,
                                   isStartEntry ? plan.startOffset : 0,
                                   isStartEntry ? plan.startHash : 0};

        if (item.isdirectory())
        {
            std::error_code ec;
            if (std::filesystem::create_directories(target, ec))
            {
                ++counters.directories;
                currentRecord.bytesWritten = 0;
            }
            else
            {
                // The directory was already there, which is not an error: the earlier run made it.
                currentRecord.bytesWritten = bytesAlreadyThere(target, true);
            }
            if (ec)
            {
                failure = localFailure("could not create the directory", target, ec);
                return false;
            }
            record(currentRecord);
            return !cancelRequested;
        }

        if (file.is_open())
        {
            file.close();
        }

        const int64_t already = bytesAlreadyThere(target, false);
        // A file bigger than the entry cannot be what this download is meant to produce: the bytes past the entry's
        // size are not part of it, so the entry is written over it in full. The entry whose recorded hash did not
        // match what is on disk is written over in full for the same reason - what a hash cannot prove is not kept.
        // A file of the same size, or one an interrupted run stopped in, is opened for reading as well and its
        // content is compared chunk by chunk.
        const bool rewrite =
            already < 0 || already > static_cast<int64_t>(item.filesize()) || (isStartEntry && plan.rewriteStartEntry);

        currentFile = target;
        std::ios::openmode mode = std::ios::binary | std::ios::in | std::ios::out;
        if (rewrite)
        {
            mode |= std::ios::trunc;
        }
        file.open(currentFile, mode);
        if (!file.is_open())
        {
            // The same failure for both modes: a file being written over, and a file that has to be read as well
            // as written because its content is compared.
            failure = grpc::Status(grpc::StatusCode::INTERNAL,
                                   "could not open \"" + misc::pathToUtf8(currentFile) +
                                       (rewrite ? "\" for writing" : "\" for reading and writing"));
            return false;
        }

        // What the file holds from now on: 0 for a file this run created or replaced, otherwise what was there.
        localSize = rewrite ? 0 : already;
        comparingCurrentEntry = !rewrite;
        wroteInCurrentEntry = false;
        currentRecord.bytesWritten = static_cast<uint64_t>(localSize);

        if (rewrite)
        {
            // Everything of this entry comes from the stream: the file was not there, or what was there is being
            // replaced. It is counted as a file of this run here, and wroteInCurrentEntry says so so its chunks do
            // not count it a second time.
            wroteInCurrentEntry = true;
            ++counters.files;
            if (already > 0)
            {
                spdlog::warn("Download: \"{}\" has {} bytes, the entry has {} - the entry is written over it",
                             misc::pathToUtf8(currentFile), already, item.filesize());
            }
        }
        record(currentRecord);
        return !cancelRequested;
    }

    const int64_t offset = static_cast<int64_t>(item.offset());
    const std::size_t chunkSize = item.data().size();

    // The part of this chunk that can be compared with what is already on disk. Past what the file holds there is
    // nothing to compare with, and a part that cannot be read is written as well: what is there cannot be proven
    // right, so it is not kept on the assumption that it is the same.
    std::size_t compared = 0;
    bool same = false;
    if (comparingCurrentEntry && offset < localSize)
    {
        compared = std::min<std::size_t>(chunkSize, static_cast<std::size_t>(localSize - offset));
        buffer.resize(compared);
        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        same = file.read(buffer.data(), static_cast<std::streamsize>(compared)) &&
               std::memcmp(buffer.data(), item.data().data(), compared) == 0;
    }

    // What has to be written is the whole chunk when the part that could be compared differs, and only the part
    // past what the file already holds when that part is the same.
    const std::size_t writeFrom = same ? compared : 0;
    if (writeFrom < chunkSize)
    {
        if (!wroteInCurrentEntry)
        {
            // The first byte this run wrote for this entry: it counts as a file this run changed, not as an entry
            // it left alone.
            wroteInCurrentEntry = true;
            ++counters.files;
        }
        file.seekp(static_cast<std::streamoff>(offset + static_cast<int64_t>(writeFrom)), std::ios::beg);
        file.write(item.data().data() + writeFrom, static_cast<std::streamsize>(chunkSize - writeFrom));
        if (!file)
        {
            failure =
                grpc::Status(grpc::StatusCode::INTERNAL, "write failed on \"" + misc::pathToUtf8(currentFile) + "\"");
            return false;
        }
        counters.bytes += chunkSize - writeFrom;
    }

    // Every byte of this chunk is content this run saw, whether it was compared or written, so the hash covers it
    // either way. It grows from the hash the plan carried in for the proven prefix, and the stream starts at that
    // prefix, so a chunk cannot overlap what is already hashed.
    currentRecord.contentHash = static_cast<uint32_t>(absl::ExtendCrc32c(
        absl::crc32c_t{currentRecord.contentHash}, absl::string_view(item.data().data(), chunkSize)));
    currentRecord.hashedBytes += chunkSize;

    // bytesWritten is what the file holds after this chunk, whether the chunk was written or only compared.
    currentRecord.bytesWritten = std::max<uint64_t>(currentRecord.bytesWritten,
                                                    static_cast<uint64_t>(offset) + static_cast<uint64_t>(chunkSize));
    ++currentRecord.chunksSent;
    record(currentRecord);
    return !cancelRequested;
}

LocalUploadProducer::LocalUploadProducer(const std::string &targetDirectoryUtf8,
                                         const std::vector<std::filesystem::path> &localPaths, bool overwrite,
                                         TransferCounters &counters, const std::atomic<bool> &cancelRequested,
                                         const StateContext &state, const RemoteSizes &remoteSizes)
    : targetDirectory(targetDirectoryUtf8), overwrite(overwrite), remoteSizes(remoteSizes), counters(counters),
      cancelRequested(cancelRequested), state(state)
{
    // Pushed in reverse so that the first path dropped is the first one sent.
    for (auto it = localPaths.rbegin(); it != localPaths.rend(); ++it)
    {
        std::error_code ec;
        if (!std::filesystem::exists(*it, ec))
        {
            spdlog::warn("Upload: \"{}\" does not exist, skipped", misc::pathToUtf8(*it));
            continue;
        }
        if (std::filesystem::is_symlink(*it, ec))
        {
            spdlog::warn("Upload: \"{}\" is a symlink, skipped", misc::pathToUtf8(*it));
            continue;
        }
        const bool isDirectory = std::filesystem::is_directory(*it, ec);
        if (ec)
        {
            spdlog::warn("Upload: \"{}\" could not be classified - {}", misc::pathToUtf8(*it), ec.message());
            continue;
        }
        // Every dropped path hangs directly on the target directory, which is position 0.
        pending.push_back({*it, misc::pathToUtf8(it->filename()), 0, isDirectory});
    }
}

void LocalUploadProducer::record(const StateEntry &entry)
{
    if (state.file == nullptr)
    {
        return;
    }
    state.file->record(state.kind, state.key, state.root, entry);
    if (++stateRecords % kTransferProgressPublish == 0)
    {
        state.file->save();
    }
}

// The record for the message being prepared. It is held here until the caller confirms that the write took the
// message, because the state has to say what the server has, not what this run handed to the stream.
void LocalUploadProducer::stage(const StateEntry &entry)
{
    stagedRecord = entry;
}

void LocalUploadProducer::confirm()
{
    if (!stagedRecord.has_value())
    {
        return;
    }
    record(*stagedRecord);
    stagedRecord.reset();
}

void LocalUploadProducer::saveState()
{
    if (state.file != nullptr)
    {
        state.file->save();
    }
}

void LocalUploadProducer::pushChildren(const PendingEntry &entry, int32_t position)
{
    std::vector<PendingEntry> children;
    std::error_code ec;
    for (const std::filesystem::directory_entry &child : std::filesystem::directory_iterator(entry.path, ec))
    {
        if (ec)
        {
            break;
        }
        if (std::filesystem::is_symlink(child.path(), ec))
        {
            spdlog::warn("Upload: \"{}\" is a symlink, skipped", misc::pathToUtf8(child.path()));
            continue;
        }
        const bool isDirectory = child.is_directory(ec);
        if (ec)
        {
            spdlog::warn("Upload: \"{}\" could not be classified - {}", misc::pathToUtf8(child.path()), ec.message());
            continue;
        }
        children.push_back({child.path(), misc::pathToUtf8(child.path().filename()), position, isDirectory});
    }
    if (ec)
    {
        spdlog::warn("Upload: listing \"{}\" failed - {}", misc::pathToUtf8(entry.path), ec.message());
    }

    for (auto it = children.rbegin(); it != children.rend(); ++it)
    {
        pending.push_back(std::move(*it));
    }
}

bool LocalUploadProducer::next(pb::UploadItem &item)
{
    while (!cancelRequested)
    {
        item.Clear();

        if (!targetSent)
        {
            targetSent = true;
            // The target directory is the whole path here, not a leaf name: as the first message it has no
            // parent to be joined to, and that is how the server reads it. It is position 0, so the entries below
            // it start counting at 1.
            pb::UploadHeader *header = item.mutable_header();
            header->set_name(targetDirectory);
            header->set_parentindex(-1);
            header->set_isdirectory(true);
            statePathByIndex.push_back(state.root);
            nextPosition = 1;
            ++counters.entries;
            ++counters.directories;
            stage(StateEntry{0, targetDirectory, -1, true, 0, 0, 0, state.root});
            return true;
        }

        if (!headerSent)
        {
            if (pending.empty())
            {
                return false;
            }
            current = pending.back();
            pending.pop_back();

            // The path is worked out before the position is taken: an entry that cannot be read is dropped here,
            // and dropping it must not consume a position the server would count.
            const std::string entryPath =
                current.parentIndex < 0
                    ? state.root + "/" + current.name
                    : statePathByIndex[static_cast<std::size_t>(current.parentIndex)] + "/" + current.name;

            std::error_code ec;
            uint64_t fileSize = 0;
            if (!current.isDirectory)
            {
                fileSize = std::filesystem::file_size(current.path, ec);
                if (ec)
                {
                    ++counters.skipped;
                    spdlog::warn("Upload: \"{}\" could not be read - {}", misc::pathToUtf8(current.path), ec.message());
                    continue;
                }
            }

            const StateEntry *prior = state.file == nullptr ? nullptr : state.prior.findEntry(entryPath);
            bool alreadyComplete = false;
            uint64_t resumeFrom = 0;
            uint32_t startSequence = 0;
            if (prior != nullptr && !current.isDirectory)
            {
                if (overwrite)
                {
                    // Overwrite was asked for, so nothing is continued and the file is sent in full: continuing would
                    // keep bytes the server already has, which is not what replacing a file means.
                    prior = nullptr;
                }
                else if (prior->fileSize != fileSize)
                {
                    // The recorded number does not describe this file: it changed since the earlier run, so there is
                    // nothing to continue from and the entry is sent from the start. The server will report the file
                    // as already existing unless overwrite was asked for.
                    spdlog::warn(
                        "Upload: \"{}\" was recorded with {} bytes, this run sends {} - starting from the start",
                        entryPath, prior->fileSize, fileSize);
                    prior = nullptr;
                }
                else
                {
                    uint64_t continueFrom = prior->bytesWritten;
                    if (!remoteSizes.empty())
                    {
                        // What the state recorded is what an earlier run handed to the stream, and a stream that
                        // ended there leaves those bytes behind: the server has fewer. The size read before this
                        // stream opened is what the server has, so the resume point moves to it, and the bytes
                        // between the two are sent again from the same local file - the result is the same file.
                        const auto it = remoteSizes.find(entryPath);
                        const uint64_t there = it == remoteSizes.end() ? 0 : it->second;
                        if (there != continueFrom)
                        {
                            spdlog::warn("Upload: \"{}\" had {} bytes in the state, the server has {} - continuing "
                                         "from there",
                                         entryPath, continueFrom, there);
                            continueFrom = there;
                        }
                    }
                    if (continueFrom >= fileSize)
                    {
                        alreadyComplete = true;
                    }
                    else
                    {
                        resumeFrom = continueFrom;
                        // The sequence cannot be worked out from a byte count because the last chunk can have been
                        // short, so what the state has is kept - but only when bytes really are already there: a
                        // header that carries no resume point is a new entry to the server, and a new entry numbers
                        // its chunks from 0.
                        startSequence = resumeFrom == 0 ? 0 : prior->chunksSent;
                    }
                }
            }

            if (!current.isDirectory && !alreadyComplete)
            {
                in.open(current.path, std::ios::binary);
                if (!in.is_open())
                {
                    ++counters.skipped;
                    spdlog::warn("Upload: could not open \"{}\"", misc::pathToUtf8(current.path));
                    continue;
                }
                // The server has these bytes already, so this run reads the local file from there.
                if (resumeFrom > 0 && !in.seekg(static_cast<std::streamoff>(resumeFrom)))
                {
                    in.close();
                    ++counters.skipped;
                    spdlog::warn("Upload: \"{}\" could not be continued at {} bytes", misc::pathToUtf8(current.path),
                                 resumeFrom);
                    continue;
                }
            }

            const int32_t position = nextPosition++;
            statePathByIndex.push_back(entryPath);

            pb::UploadHeader *header = item.mutable_header();
            header->set_name(current.name);
            header->set_parentindex(current.parentIndex);
            header->set_isdirectory(current.isDirectory);
            header->set_filesize(fileSize);
            header->set_overwrite(overwrite);
            if (alreadyComplete)
            {
                // Nothing follows this header; it is only here so the entries after it keep counting against it.
                header->set_alreadycomplete(true);
                ++counters.skipped;
                spdlog::info("Upload: \"{}\" was already completed by an earlier run", entryPath);
            }
            else if (resumeFrom > 0)
            {
                header->set_resumefrom(resumeFrom);
                header->set_startsequence(startSequence);
                spdlog::info("Upload: \"{}\" is continued at {} bytes", entryPath, resumeFrom);
            }
            ++counters.entries;
            if (current.isDirectory)
            {
                ++counters.directories;
                pushChildren(current, position);
            }
            else if (!alreadyComplete)
            {
                ++counters.files;
            }
            currentRecord = StateEntry{position,
                                       current.name,
                                       current.parentIndex,
                                       current.isDirectory,
                                       fileSize,
                                       alreadyComplete ? fileSize : resumeFrom,
                                       startSequence,
                                       entryPath};
            stage(currentRecord);
            headerSent = true;
            sequence = startSequence;
            offset = resumeFrom;
            return true;
        }

        if (!in.is_open())
        {
            // A directory has no content, and an entry that was already complete has none either: its header was
            // the whole entry.
            headerSent = false;
            continue;
        }

        std::string data;
        data.resize(chunkSize);
        in.read(&data[0], static_cast<std::streamsize>(chunkSize));
        const std::streamsize got = in.gcount();
        if (got <= 0)
        {
            in.close();
            headerSent = false;
            continue;
        }
        data.resize(static_cast<std::size_t>(got));

        pb::UploadChunk *chunk = item.mutable_chunk();
        chunk->set_sequence(sequence);
        chunk->set_offset(offset);
        chunk->set_size(static_cast<uint64_t>(got));
        chunk->set_data(std::move(data));
        ++sequence;
        offset += static_cast<uint64_t>(got);
        counters.bytes += static_cast<uint64_t>(got);
        currentRecord.bytesWritten = offset;
        currentRecord.chunksSent = sequence;
        stage(currentRecord);
        return true;
    }

    return false;
}

TransferRunner makeDownloadRunner(std::shared_ptr<FileBrowserClient> client)
{
    return [client](grpc::ClientContext &context, const TransferJob &job, TransferCounters &counters,
                    const PublishProgress &publish) -> grpc::Status {
        const std::string root = misc::pathToUtf8(job.localRoot);
        const StateTransfer prior = job.state == nullptr ? StateTransfer() : job.state->find(job.kind, job.key, root);
        const StateContext state{job.state.get(), job.kind, job.key, root, prior};

        // The order this run will get, not the order an earlier run got: entries are matched by path, but
        // startEntryIndex is a position, so an entry can only be left alone when the two agree entry for entry. A
        // single file has one entry, so its order is its own name and nothing is traversed for it.
        std::vector<std::string> order;
        if (!prior.entries.empty())
        {
            if (!job.remoteIsDirectory)
            {
                order.push_back(root + "/" + misc::pathToUtf8(misc::utf8ToPath(job.remotePath).filename()));
            }
            else
            {
                const std::filesystem::path scanned = misc::utf8ToPath(job.remotePath);
                // One ClientContext carries one call, so the probe gets its own. cancel() reaches the call the queue
                // registered, which is the download below; while the probe runs the stop is the flag the sink checks.
                grpc::ClientContext probeContext;
                const grpc::Status check = client->recursiveDirectoryTraversal(
                    probeContext, job.remotePath, [&](const ScanEntry &entry) -> bool {
                        order.push_back(root + "/" + chainOf(entry.filePath, scanned));
                        return !job.cancelRequested;
                    });
                if (!check.ok())
                {
                    if (job.cancelRequested)
                    {
                        // The probe is stopped by this flag, not by the context the queue registered, so a stream
                        // that ended here is still the transfer being stopped, not an order that could not be read.
                        return grpc::Status(grpc::StatusCode::CANCELLED, "cancelled while the order was read");
                    }
                    // The order could not be read, so nothing can be left alone and the run compares every entry as
                    // it always did.
                    spdlog::warn("Download: could not read the order for \"{}\", comparing every entry",
                                 job.remotePath);
                    order.clear();
                }
            }
        }

        // What the check read from disk, put on the progress the same way the counters are: proving a large prefix
        // can take seconds with nothing coming from the server, and a line that only counts entries cannot tell that
        // apart from waiting.
        const HashProgress report = [&](uint64_t checked) {
            counters.hashChecked += checked;
            publish();
        };
        const DownloadPlan plan = planDownload(state, order, report);

        // The writer creates the folder it writes into, so nothing is sent to the server when the local side cannot
        // be prepared. The plan goes with it: an entry the plan left alone never arrives as a header, and the
        // parentIndex of an entry that does arrive can point at one of them.
        LocalDownloadWriter writer(job.localRoot, counters, job.cancelRequested, state, plan);
        if (!writer.failure.ok())
        {
            return writer.failure;
        }

        if (plan.nothingToFetch)
        {
            // Every entry the state knows is proven by the hash it recorded, so no content is asked for and the
            // state is left as it stands: what it has for those entries is what this run would have recorded.
            spdlog::info("Download: nothing to fetch for \"{}\", {} entries proven by their hash", job.remotePath,
                         plan.verifiedFiles);
            return grpc::Status::OK;
        }

        std::size_t messages = 0;

        const DownloadItemSink sink = [&](const pb::DownloadItem &item) -> bool {
            const bool ok = writer.write(item);
            if (++messages % kTransferProgressPublish == 0)
            {
                writer.saveState();
                publish();
            }
            return ok;
        };

        if (job.remoteIsDirectory)
        {
            // What the plan decided: entries before startEntryIndex are proven and are not sent, and the entry at
            // that position is read from the byte its recorded hash covers. An empty plan gives 0 for both, which is
            // the whole tree compared entry by entry.
            pb::DownloadDirectoryRequest request;
            request.set_directory(job.remotePath);
            request.set_chunksize(static_cast<uint32_t>(kTransferChunkSize));
            request.set_startentryindex(static_cast<uint32_t>(plan.startEntryIndex));
            request.set_startoffset(plan.startOffset);
            const grpc::Status status = client->downloadDirectory(context, request, sink);
            writer.saveState();
            return writer.failure.ok() ? status : writer.failure;
        }

        // A single file: this RPC has no header of its own, so the writer is handed one built from the job, and
        // each chunk is put into the same message shape so the writer has one entry point.
        pb::DownloadItem item;
        item.set_name(misc::pathToUtf8(misc::utf8ToPath(job.remotePath).filename()));
        item.set_parentindex(-1);
        item.set_isdirectory(false);
        item.set_filesize(job.remoteFileSize);
        if (!sink(item))
        {
            // sink() returns false for two different reasons: the cancel flag, or a local failure the writer
            // recorded on this message. The failure names the file it could not create or open, so it is what gets
            // reported when there is one; the cancel text only covers the case where nothing on the local side
            // went wrong.
            return writer.failure.ok()
                       ? grpc::Status(grpc::StatusCode::CANCELLED, "cancelled before the file was opened")
                       : writer.failure;
        }

        const DownloadChunkSink chunkSink = [&](const pb::DownloadChunk &chunk) -> bool {
            item.set_sequence(chunk.sequence());
            item.set_offset(chunk.offset());
            item.set_size(chunk.size());
            item.set_data(chunk.data());
            return sink(item);
        };

        pb::DownloadRequest request;
        request.set_filepath(job.remotePath);
        request.set_chunksize(static_cast<uint32_t>(kTransferChunkSize));
        // What the plan proved by hash is not asked for again. Where nothing could be proven the whole content is
        // asked for: the writer can only say the two are the same after comparing them, and it writes only what
        // differs.
        request.set_offset(plan.startOffset);
        const grpc::Status status = client->downloadFile(context, request, chunkSink);
        writer.saveState();
        return writer.failure.ok() ? status : writer.failure;
    };
}

TransferRunner makeUploadRunner(std::shared_ptr<FileBrowserClient> client)
{
    return [client](grpc::ClientContext &context, const TransferJob &job, TransferCounters &counters,
                    const PublishProgress &publish) -> grpc::Status {
        const StateTransfer prior =
            job.state == nullptr ? StateTransfer() : job.state->find(job.kind, job.key, job.remotePath);
        const StateContext state{job.state.get(), job.kind, job.key, job.remotePath, prior};

        // What the server actually has for the entries an earlier run recorded. This is read before the stream
        // opens because the sync server runs a handler to completion on its poller thread: a lookup from inside the
        // upload would never be answered. One traversal covers every entry the state knows, which is cheaper than
        // one call per entry. When it cannot be read the producer trusts the state as it always did, and a run with
        // nothing to continue from needs no lookup - overwrite does not use the state either.
        RemoteSizes remoteSizes;
        if (!prior.entries.empty() && !job.overwrite)
        {
            const std::filesystem::path scanned = misc::utf8ToPath(job.remotePath);
            // One ClientContext carries one call, so the probe gets its own. cancel() reaches the call the queue
            // registered, which is the upload below; while the probe runs the stop is the flag the sink checks.
            grpc::ClientContext probeContext;
            const grpc::Status probe =
                client->recursiveDirectoryTraversal(probeContext, job.remotePath, [&](const ScanEntry &entry) -> bool {
                    remoteSizes[job.remotePath + "/" + chainOf(entry.filePath, scanned)] = entry.fileSize;
                    return !job.cancelRequested;
                });
            if (!probe.ok())
            {
                if (job.cancelRequested)
                {
                    // The probe is stopped by this flag, not by the context the queue registered, so a stream that
                    // ended here is still the transfer being stopped, not sizes that could not be read.
                    return grpc::Status(grpc::StatusCode::CANCELLED, "cancelled while the server sizes were read");
                }
                spdlog::warn("Upload: could not read what the server has for \"{}\", trusting the state",
                             job.remotePath);
                remoteSizes.clear();
            }
        }

        LocalUploadProducer producer(job.remotePath, job.localEntries, job.overwrite, counters, job.cancelRequested,
                                     state, remoteSizes);
        std::size_t messages = 0;

        const UploadItemProducer nextItem = [&](pb::UploadItem &item) -> bool {
            if (!producer.next(item))
            {
                return false;
            }
            if (++messages % kTransferProgressPublish == 0)
            {
                publish();
            }
            return true;
        };

        // The record for a message goes into the state here, after the write took it.
        const WriteConfirmed onWritten = [&]() { producer.confirm(); };

        pb::UploadReply reply;
        const grpc::Status status = client->uploadFiles(context, nextItem, onWritten, reply);
        producer.saveState();
        return producer.failure.ok() ? status : producer.failure;
    };
}
