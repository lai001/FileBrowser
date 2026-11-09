#ifdef _WIN32
#include <windows.h>
#endif
#include <fstream>
#include "spdlog/spdlog.h"
#include "Misc.h"
#include "TransferState.h"
#include "nlohmann/json.hpp"

namespace
{

const char *kindName(TransferKind kind)
{
    return kind == TransferKind::Upload ? "upload" : "download";
}

TransferKind kindFromName(const std::string &name)
{
    return name == "upload" ? TransferKind::Upload : TransferKind::Download;
}

// ordered_json rather than the default json: a plain nlohmann object keeps its keys sorted, so the file would come
// out in a different order from the one the fields are listed in here. Reading does not care about the order, only
// writing does, and this file is meant to be readable by eye.
using Json = nlohmann::ordered_json;

// One field read as it is. A key that is missing or is not the type this file writes gives the value an entry with
// nothing recorded has, so a hand-edited or half written file still gives back what it can instead of failing the
// whole transfer.
std::string jsonString(const Json &object, const char *key)
{
    const auto item = object.find(key);
    return item == object.end() || !item->is_string() ? std::string() : item->get<std::string>();
}

int64_t jsonInt(const Json &object, const char *key)
{
    const auto item = object.find(key);
    return item == object.end() || !item->is_number_integer() ? 0 : item->get<int64_t>();
}

bool jsonBool(const Json &object, const char *key)
{
    const auto item = object.find(key);
    return item != object.end() && item->is_boolean() && item->get<bool>();
}

StateEntry readEntry(const Json &item)
{
    StateEntry entry;
    entry.position = static_cast<int32_t>(jsonInt(item, "position"));
    entry.name = jsonString(item, "name");
    entry.parentIndex = static_cast<int32_t>(jsonInt(item, "parentIndex"));
    entry.isDirectory = jsonBool(item, "isDirectory");
    entry.fileSize = static_cast<uint64_t>(jsonInt(item, "fileSize"));
    entry.bytesWritten = static_cast<uint64_t>(jsonInt(item, "bytesWritten"));
    entry.chunksSent = static_cast<uint64_t>(jsonInt(item, "chunksSent"));
    entry.path = jsonString(item, "path");
    entry.hashedBytes = static_cast<uint64_t>(jsonInt(item, "hashedBytes"));
    entry.contentHash = static_cast<uint32_t>(jsonInt(item, "contentHash"));
    return entry;
}

} // namespace

TransferStateFile::TransferStateFile(std::filesystem::path path) : filePath(std::move(path))
{
}

void TransferStateFile::buildIndex(StateTransfer &transfer)
{
    transfer.indexByPath.clear();
    for (std::size_t i = 0; i < transfer.entries.size(); ++i)
    {
        transfer.indexByPath[transfer.entries[i].path] = i;
    }
}

const StateEntry *StateTransfer::findEntry(const std::string &path) const
{
    const auto item = indexByPath.find(path);
    return item == indexByPath.end() ? nullptr : &entries[item->second];
}

bool TransferStateFile::load()
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    // The third argument is what keeps a broken file from throwing: parse comes back as a discarded value, which is
    // checked here, and the transfer runs as if nothing had been recorded.
    const Json root = Json::parse(file, nullptr, false);
    if (root.is_discarded())
    {
        spdlog::warn("State file: could not read \"{}\", starting without it", misc::pathToUtf8(filePath));
        return false;
    }

    const auto transfersItem = root.find("transfers");
    if (transfersItem == root.end() || !transfersItem->is_array())
    {
        spdlog::warn("State file: \"{}\" has no transfers array, starting without it", misc::pathToUtf8(filePath));
        return false;
    }

    std::vector<StateTransfer> parsed;
    for (const Json &item : *transfersItem)
    {
        if (!item.is_object())
        {
            continue;
        }

        StateTransfer transfer;
        transfer.kind = kindFromName(jsonString(item, "kind"));
        transfer.key = jsonString(item, "key");
        transfer.root = jsonString(item, "root");

        const auto entriesItem = item.find("entries");
        if (entriesItem == item.end() || !entriesItem->is_array())
        {
            // A transfer with no entries is still a transfer: its kind/key/root is what the next run looks for.
            parsed.push_back(std::move(transfer));
            continue;
        }
        for (const Json &entryItem : *entriesItem)
        {
            if (entryItem.is_object())
            {
                transfer.entries.push_back(readEntry(entryItem));
            }
        }
        parsed.push_back(std::move(transfer));
    }

    const std::lock_guard<std::mutex> lock(m);
    transfers.swap(parsed);
    for (StateTransfer &transfer : transfers)
    {
        buildIndex(transfer);
    }
    return true;
}

StateTransfer TransferStateFile::find(TransferKind kind, const std::string &key, const std::string &root) const
{
    const std::lock_guard<std::mutex> lock(m);
    for (const StateTransfer &transfer : transfers)
    {
        if (transfer.kind == kind && transfer.key == key && transfer.root == root)
        {
            return transfer;
        }
    }
    return StateTransfer(kind, key, root);
}

void TransferStateFile::record(TransferKind kind, const std::string &key, const std::string &root,
                               const StateEntry &entry)
{
    const std::lock_guard<std::mutex> lock(m);
    StateTransfer &transfer = transferFor(kind, key, root);

    const auto item = transfer.indexByPath.find(entry.path);
    if (item != transfer.indexByPath.end())
    {
        // The same entry seen again: the numbers are what the run counted so far, so the stored entry is updated
        // in place rather than appended a second time.
        transfer.entries[item->second] = entry;
        return;
    }
    transfer.indexByPath[entry.path] = transfer.entries.size();
    transfer.entries.push_back(entry);
}

bool TransferStateFile::save() const
{
    const std::lock_guard<std::mutex> lock(m);

    Json root;
    root["transfers"] = Json::array();
    for (const StateTransfer &transfer : transfers)
    {
        Json item;
        item["kind"] = kindName(transfer.kind);
        item["key"] = transfer.key;
        item["root"] = transfer.root;
        item["entries"] = Json::array();
        for (const StateEntry &entry : transfer.entries)
        {
            Json entryItem;
            entryItem["position"] = entry.position;
            entryItem["name"] = entry.name;
            entryItem["parentIndex"] = entry.parentIndex;
            entryItem["isDirectory"] = entry.isDirectory;
            entryItem["fileSize"] = entry.fileSize;
            entryItem["bytesWritten"] = entry.bytesWritten;
            entryItem["chunksSent"] = entry.chunksSent;
            entryItem["path"] = entry.path;
            entryItem["hashedBytes"] = entry.hashedBytes;
            entryItem["contentHash"] = entry.contentHash;
            item["entries"].push_back(std::move(entryItem));
        }
        root["transfers"].push_back(std::move(item));
    }

    std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        spdlog::warn("State file: could not open \"{}\" for writing", misc::pathToUtf8(filePath));
        return false;
    }
    // dump(2) is the two space indent. The library escapes what JSON has to escape and leaves the UTF-8 of a name
    // as it is, so a name with a quote in it is written escaped and reads back unchanged.
    file << root.dump(2);
    if (!file)
    {
        spdlog::warn("State file: writing \"{}\" failed", misc::pathToUtf8(filePath));
        return false;
    }
    return true;
}

StateTransfer &TransferStateFile::transferFor(TransferKind kind, const std::string &key, const std::string &root)
{
    for (StateTransfer &transfer : transfers)
    {
        if (transfer.kind == kind && transfer.key == key && transfer.root == root)
        {
            return transfer;
        }
    }
    StateTransfer created(kind, key, root);
    transfers.push_back(std::move(created));
    buildIndex(transfers.back());
    return transfers.back();
}

std::filesystem::path stateFilePath()
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0)
    {
        return std::filesystem::path(buffer).parent_path() / "filebrowser-state.json";
    }
#endif
    // The fallback is the working directory, which is also what the tests get.
    return std::filesystem::current_path() / "filebrowser-state.json";
}
