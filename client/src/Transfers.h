#ifndef CLIENT_SRC_TRANSFERS_H
#define CLIENT_SRC_TRANSFERS_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
#include "grpcpp/grpcpp.h"
#include "Client.h"
#include "TransferQueue.h"
#include "TransferState.h"

// A local problem is reported as INTERNAL: nothing on the server side is wrong, the disk refused it. The status
// line shows the message, so the path has to be in it.
grpc::Status localFailure(const char *what, const std::filesystem::path &path, const std::error_code &ec);

// The chain of names below the directory a traversal was started on, written the way the state writes a path: the
// names joined with "/". The state's root is the local folder and the traversal runs against the server's directory,
// so the chain is the part of the two that can be compared. The controller needs it for the same reason: it measures
// the progress of an upload against the paths the state will record, and those are built the same way.
std::string chainOf(const std::filesystem::path &path, const std::filesystem::path &root);

// The chunk size both directions use. The server default is 64 KiB with a 1 MiB cap, so this is what the server
// would have picked anyway, and the client never sends a chunk bigger than that cap.
constexpr std::size_t kTransferChunkSize = 64 * 1024;

// Progress is published every kTransferProgressPublish messages. For a transfer the message is the unit that
// moves - a large file is thousands of chunks - and one mutex per chunk would make the transfer compete with the
// render thread for a number that is read once per frame. The state file is written on the same interval, which is
// also the most a crashed run can lose.
constexpr std::size_t kTransferProgressPublish = 64;

// Bytes already on disk for one entry, or -1 when the entry is not there. A directory that is there counts as 0
// because nothing has to be written for it; a file counts as its size.
int64_t bytesAlreadyThere(const std::filesystem::path &target, bool isDirectory);

// What a hash check read from disk, reported while it reads. Called on the same interval as the progress publish,
// because one call per piece would make the check compete with the render thread for a number that is read once per
// frame. An empty function is what a test uses: nothing is reported.
using HashProgress = std::function<void(uint64_t bytesChecked)>;

// CRC32C of the first bytes of a local file, or nullopt when the file is not there, cannot be read, or is shorter
// than what the hash claims. Read piece by piece, so a large file is not read into memory. absl's crc32c is hardware
// accelerated on the CPUs that have the instruction and falls back to a table otherwise, so the value is the same
// either way and a state file read on another machine still means the same thing.
std::optional<uint32_t> hashPrefix(const std::filesystem::path &file, uint64_t bytes,
                                   const HashProgress &report = HashProgress());

// What a writer or a producer needs from the state file: the file itself, what this transfer is, and what an
// earlier run left behind. An empty context (file == nullptr) means no state, which is what the tests use: every
// state decision then falls back to the plain rule.
struct StateContext
{
    TransferStateFile *file = nullptr;
    TransferKind kind = TransferKind::Download;
    std::string key;
    // root as the state names it: the local folder for a download, the target directory for an upload.
    std::string root;
    StateTransfer prior;
};

// What a download run decides before it asks the server for anything.
//
// Entries are matched by path, but DownloadDirectoryRequest.startEntryIndex is a position in the traversal, so an
// entry can only be left alone when the position the state has it at is the position this run gets. currentOrder is
// therefore the state path of every entry in the order the traversal produced them; an order that does not match
// entry for entry means nothing can be skipped and the run compares as it always did.
//
// A file is left alone when the hash the state has for it still matches the bytes on disk. A directory is left alone
// when it is already there: nothing about a directory has to be proven and no content comes for it either.
struct DownloadPlan
{
    // Entries that can be left alone, in traversal order. The writer needs their paths because the parentIndex of an
    // entry it does receive can point at one of them.
    std::vector<StateEntry> left;
    // Position the stream starts at, which is left.size(): the first entry that still needs work.
    std::size_t startEntryIndex = 0;
    // Bytes of that entry the recorded hash proves, so the server does not send them. 0 when the hash cannot be
    // trusted and the entry has to come from its first byte.
    uint64_t startOffset = 0;
    // Hash those bytes had: the writer hashes on from here instead of starting this entry over.
    uint32_t startHash = 0;
    // The recorded hash does not match what is on disk, so this entry is written over in full.
    bool rewriteStartEntry = false;
    // Every entry the state knows is proven, so no content is asked for at all.
    bool nothingToFetch = false;
    // Files left alone, which is what the verified counter reports. The directories in left are not counted: they
    // were made by an earlier run and nothing was proven about them.
    std::size_t verifiedFiles = 0;
};

// The plan for one download run, from what an earlier run recorded and the order this run will get. An empty order
// is what a run with no state gets, and what a run whose order could not be read gets: the plan then skips nothing.
// report gets what each entry's check read from disk, which is the part of a run that can take seconds without
// anything coming from the server.
DownloadPlan planDownload(const StateContext &state, const std::vector<std::string> &currentOrder,
                          const HashProgress &report = HashProgress());

// Writes a download stream into a local directory. The stream carries each entry's own name and the position of
// its parent, so the path is rebuilt here the way the client tree rebuilds it. parentIndex -1 is the directory
// the download was started on, which the server never sends, so the writer's own root stands in for it.
//
// A file that is already on disk is not left alone because its size matches: what is there is compared with the
// chunks as they arrive and only the bytes that differ are written. An entry nothing was written for is counted as
// skipped, which is the "the two files are the same, leave it alone" case. A file bigger than the entry cannot be
// what this download is meant to produce, so it is written over in full. The same happens to an entry whose recorded
// hash does not match what is on disk: what a hash cannot prove is not kept.
//
// Every byte of a file entry is hashed as it is read, compared or written either way, so what the state records is a
// hash for the content this run saw. The plan is handed in here: its entries go into the two path tables, because a
// parentIndex in the stream can point at an entry that never arrives, and the first entry that does arrive carries
// the hash its proven prefix already had.
class LocalDownloadWriter
{
  public:
    LocalDownloadWriter(const std::filesystem::path &root, TransferCounters &counters,
                        const std::atomic<bool> &cancelRequested, const StateContext &state = StateContext(),
                        const DownloadPlan &plan = DownloadPlan());
    // The last entry has no header after it to finish it, so its count is made here, before the counters are read.
    ~LocalDownloadWriter();

    // A message whose size field is 0 is a header: it records the position and then creates the directory or opens
    // the file the chunks after it belong to. A message with content is compared with what the file already holds
    // and written only when it differs. Returning false is how the caller stops the stream.
    bool write(const pb::DownloadItem &item);

    // A local failure. The runner returns it because an RPC status cannot tell "the server sent it" apart from
    // "the disk refused it"; the code is INTERNAL since nothing on the server side is wrong.
    grpc::Status failure;

    // Write the state file. Called on the publish interval while entries come in, and once when the transfer ends.
    void saveState();

  private:
    // The path string an entry is recorded under: the parent's path plus the name, always with "/". Entries are
    // matched by that path rather than by position, because a directory does not have to list its children in the
    // same order twice.
    std::string statePathFor(int32_t parentIndex, const std::string &name) const;

    // Put one entry in the state with what this run has counted for it so far, and write the file on the interval.
    void record(const StateEntry &entry);

    // The entry that just ended. An entry nothing was written for is counted as skipped, and that cannot be decided
    // until the entry is finished, because a difference can show up in any of its chunks.
    void finishCurrentEntry();

    std::filesystem::path root;
    std::vector<std::filesystem::path> pathByIndex;
    // The same table as the state knows it, in position order. pathByIndex is what the files are written to, this
    // is what the state is written under.
    std::vector<std::string> statePathByIndex;
    // The entry being written, as the state has it, so the chunks that follow update it in place. For a download
    // bytesWritten is what the file holds: what was already there plus what this run wrote.
    StateEntry currentRecord;
    // The file the chunks currently being written belong to. It is opened for reading as well, so what is already
    // there can be compared before anything is written.
    std::fstream file;
    // One chunk read from the file, for the comparison.
    std::vector<char> buffer;
    // Bytes the file had when it was opened. Nothing below that is written without being compared first.
    int64_t localSize = 0;
    // The file the chunks currently being written belong to, so a write error can name it.
    std::filesystem::path currentFile;
    // True while the entry being read is one that was already on disk, so its chunks are compared.
    bool comparingCurrentEntry = false;
    // True when something in the entry being compared has been written, which is what makes it count as a file and
    // not as a skipped entry.
    bool wroteInCurrentEntry = false;
    // Entry at which the state file was last written, so a save happens on the interval instead of per entry.
    std::size_t stateRecords = 0;
    StateContext state;
    TransferCounters &counters;
    const std::atomic<bool> &cancelRequested;
    // The plan this writer was built with: the entries it left alone are put in the two path tables, and the hash of
    // the entry this run starts at is known before its first chunk arrives.
    DownloadPlan plan;
};

// What the server actually has, by the path the state knows the entries under. It is read once before an upload
// stream opens: the sync server runs a handler to completion on its poller thread, so a lookup from inside the
// stream would never be answered. An empty map means the sizes could not be read and the state is trusted as it
// always was; an entry missing from a non-empty map is not on the server, so that entry starts from its first byte.
using RemoteSizes = std::unordered_map<std::string, uint64_t>;

// Produces the messages of one upload. The first message is the target directory's header: client streaming has
// no request message, so that is where the server learns which directory to write into, and its name is the whole
// path because there is no parent to join it to. Then one header per local entry in traversal order, with its
// chunks after it.
//
// One chunk at a time, so a large file is never read into memory as a whole. sequence and offset are kept in
// step with what has already been produced, because the server rejects a chunk that does not continue where the
// previous one stopped. With a state, an entry that an earlier run finished is sent as a header only
// (alreadyComplete) and an entry it stopped in is sent with resumeFrom and startSequence, so the server appends
// instead of starting the file over.
//
// Where an entry is continued from is the size the server has, not the number the state recorded: an earlier run
// counts what it handed to the stream, and a stream that ended leaves those bytes behind. With overwrite the state
// is not used for this at all, because the server replaces the file and starting in the middle of a replaced file
// is not what overwrite means.
//
// Symlinks are skipped, not followed: a symlinked directory would make the walk able to come back into itself,
// and the server does not descend into reparse points either.
class LocalUploadProducer
{
  public:
    LocalUploadProducer(const std::string &targetDirectoryUtf8, const std::vector<std::filesystem::path> &localPaths,
                        bool overwrite, TransferCounters &counters, const std::atomic<bool> &cancelRequested,
                        const StateContext &state = StateContext(), const RemoteSizes &remoteSizes = RemoteSizes());

    // One message per call. False when the stream is finished, or when it was cancelled.
    bool next(pb::UploadItem &item);

    // Commit the record next() prepared, which is what the caller does once the write took the message. Without a
    // caller that does this, a run records bytes the server never had.
    void confirm();

    // An entry that could not be read is skipped and counted, which is the same rule the server applies to a
    // directory listing. A failure here is something that cannot be worked around: a file that opened but could
    // not be read, or a write error on the message.
    grpc::Status failure;

    // Write the state file, on the same interval as the progress and once at the end.
    void saveState();

  private:
    struct PendingEntry
    {
        std::filesystem::path path;
        // The name as the server sees it: the leaf name, or the whole path for the target directory itself.
        std::string name;
        int32_t parentIndex;
        bool isDirectory;
    };

    void pushChildren(const PendingEntry &entry, int32_t position);

    // Put one entry in the state with what this run has counted for it so far, and write the file on the interval.
    // Called from confirm(), not from next(): the message this record describes has to reach the server first.
    void record(const StateEntry &entry);

    // Hold the record for the message being prepared, so it can be put in the state only after the write took it.
    void stage(const StateEntry &entry);

    std::string targetDirectory;
    bool overwrite;
    // What the server had for the entries the state knows, read before the stream opened. Empty is the case where
    // it could not be read: the state is then trusted as it always was.
    RemoteSizes remoteSizes;
    // Depth first: children are pushed after their parent's header is out, so they come out below it and every
    // parentIndex points at a header the server has already seen. Pushed in reverse so the entries come out in the
    // order they were dropped and the directory entries in the order the directory lists them.
    std::vector<PendingEntry> pending;
    bool targetSent = false;
    int32_t nextPosition = 0;

    PendingEntry current;
    // False until the header of the current entry has been handed over; a directory needs nothing after that.
    bool headerSent = false;
    std::ifstream in;
    std::size_t chunkSize = kTransferChunkSize;
    uint32_t sequence = 0;
    uint64_t offset = 0;
    // The path strings the state knows the entries under, in position order: position 0 is the target directory.
    std::vector<std::string> statePathByIndex;
    // The entry being produced, as the state has it. bytesWritten starts from the size the server has for the entry
    // and grows with the chunks this run sends, so what the state holds is what the server has.
    StateEntry currentRecord;
    // The same record, waiting for the write to take the message. confirm() is what puts it in the state.
    std::optional<StateEntry> stagedRecord;
    std::size_t stateRecords = 0;
    StateContext state;
    TransferCounters &counters;
    const std::atomic<bool> &cancelRequested;
};

// The two runners the UI puts on the queue. Each one holds a shared_ptr to the client, so closing the tab while a
// transfer is running cannot destroy the stub under a call that is still in flight - the same reason ScanJob
// holds one.
TransferRunner makeDownloadRunner(std::shared_ptr<FileBrowserClient> client);
TransferRunner makeUploadRunner(std::shared_ptr<FileBrowserClient> client);

#endif
