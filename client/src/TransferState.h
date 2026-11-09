#ifndef CLIENT_SRC_TRANSFERSTATE_H
#define CLIENT_SRC_TRANSFERSTATE_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Which direction a transfer is, and which way the state file's records are read. It lives here because the state
// file is keyed by it and TransferQueue.h includes this header, so the queue sees it as well.
enum class TransferKind
{
    Upload,
    Download,
};

// One entry as the state file keeps it. name / parentIndex / isDirectory / fileSize are the values the transfer
// used on the wire; position is the entry's place in the stream; path is the same parent chain joined with "/",
// kept next to the chain so a reader of the file sees the path without rebuilding it.
struct StateEntry
{
    int32_t position = 0;
    std::string name;
    int32_t parentIndex = -1;
    bool isDirectory = false;
    uint64_t fileSize = 0;
    // Bytes of this entry as it was when the file was last written. For a download this is what the local file
    // holds: what an earlier run left plus what this run wrote. The state records it but decides nothing for a
    // download, which compares the content of every entry it is given. For an upload this is what the client tells
    // the server to expect (UploadHeader.resumeFrom), so it has to be saved often enough to be right.
    uint64_t bytesWritten = 0;
    // Chunks this run accounted for in this entry, which is the sequence its next chunk carries. For a download
    // that includes the chunks it only compared. It cannot be worked out from bytesWritten, because the last chunk
    // of an interrupted run can be shorter than the chunk size.
    uint32_t chunksSent = 0;
    // The path as this transfer recorded it: root, then the name chain with "/". It is what makes the same entry
    // recognizable in two runs, because the position numbering can change while the path cannot.
    std::string path;
    // CRC32C of the first hashedBytes bytes of this entry's content, as an earlier run saw them. The next run hashes
    // the same bytes on disk and compares: equal means those bytes do not have to come from the server again, and
    // hashedBytes == fileSize means the whole entry is proven without any content for it. 0 for an entry recorded
    // before this existed, for a directory, and for an entry whose bytes could not be hashed. Only a download fills
    // it: an upload has nothing local to hash, so its decision stays on bytesWritten.
    uint64_t hashedBytes = 0;
    uint32_t contentHash = 0;
};

// One transfer. root is what identifies it: the local folder a download wrote into, or the directory an upload
// targeted. Entries are stored in traversal order, so a parent is always stored before its child.
struct StateTransfer
{
    StateTransfer() = default;
    StateTransfer(TransferKind kind, std::string key, std::string root)
        : kind(kind), key(std::move(key)), root(std::move(root))
    {
    }

    TransferKind kind = TransferKind::Download;
    std::string key;
    std::string root;
    std::vector<StateEntry> entries;

    // The path of an entry as it was recorded. Entries are matched by path rather than by position: a directory
    // does not have to list its children in the same order twice.
    std::string pathOf(const StateEntry &entry) const
    {
        return entry.path;
    }

    // The entry for a path, or nullptr when this run has not seen it before.
    const StateEntry *findEntry(const std::string &path) const;

  private:
    // Path -> index in entries. Filled when the file is read and kept up to date when an entry is recorded.
    std::unordered_map<std::string, std::size_t> indexByPath;

    friend class TransferStateFile;
};

// filebrowser-state.json, in the directory the client executable is in. It is what makes a transfer resumable
// after the program has been closed.
//
// Reading and writing go through nlohmann/json: the read uses the parse that does not throw, and the write uses
// ordered_json so the file keeps the order the fields are listed in here.
//
// The file is written on the progress publish interval and at the end of a transfer, so an interrupted run loses
// at most that many messages of the entry it was working on. An entry a download hashed is what decides the next
// run: the hash recorded here plus what the file on disk hashes to is what lets an entry be left alone, so a lost
// save costs the bytes a later run has to take from the server again. An upload does depend on the byte counts, and
// a number that does not match the file on the server is reported as a mismatch there instead of being quietly
// rewritten.
//
// One mutex for the whole file: the worker records while the render thread only reads before a job is built. The
// mutex is never held while a transfer runs.
class TransferStateFile
{
  public:
    explicit TransferStateFile(std::filesystem::path path);

    // Reads the file. False when it is missing or cannot be parsed, which is how a first run or a hand-edited file
    // is handled: the transfer starts as if it had no state.
    bool load();

    // A copy of one transfer's entries, so the caller is not holding the mutex while the worker writes.
    StateTransfer find(TransferKind kind, const std::string &key, const std::string &root) const;

    // Put one entry in, replacing whatever is stored for the same path. entry.path has to be filled by the caller.
    void record(TransferKind kind, const std::string &key, const std::string &root, const StateEntry &entry);

    // Write the file back.
    bool save() const;

    const std::filesystem::path &path() const
    {
        return filePath;
    }

  private:
    // The transfer for kind/key/root, created when it is not there. Called with the mutex held.
    StateTransfer &transferFor(TransferKind kind, const std::string &key, const std::string &root);

    static void buildIndex(StateTransfer &transfer);

    mutable std::mutex m;
    std::filesystem::path filePath;
    std::vector<StateTransfer> transfers;
};

// Where the state file goes: next to the client executable, which is what makes it survive a restart.
// current_path() is the fallback when the module path cannot be read; the tests pass their own path instead.
std::filesystem::path stateFilePath();

#endif
