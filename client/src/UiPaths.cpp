#include "UiPaths.h"
#include "Misc.h"

std::string parentDirectory(const std::string &path)
{
    std::size_t end = path.size();
    while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\'))
    {
        --end;
    }
    if (end == 0)
    {
        return std::string(); // already a root: nothing to go up to
    }
    while (end > 0 && path[end - 1] != '/' && path[end - 1] != '\\')
    {
        --end;
    }
    if (end == 0)
    {
        return std::string(); // relative path: the parent is not knowable from the string alone
    }
    return path.substr(0, end); // keeps the separator, so it stays the root of the drive
}

std::string normalizeDirectory(const std::string &directory)
{
    if (directory.size() >= 2)
    {
        const char first = directory[0];
        const bool driveLetter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
        if (driveLetter && directory[1] == ':' && directory.size() == 2)
        {
            return directory + '\\';
        }
        if (driveLetter && directory[1] == ':' && directory[2] != '/' && directory[2] != '\\')
        {
            return directory.substr(0, 2) + '\\' + directory.substr(2);
        }
    }
    return directory;
}

bool isIllegalPathChar(char32_t c)
{
    // Control characters as a group: 0x00 to 0x1F and 0x7F.
    if (c < 0x20 || c == 0x7F)
    {
        return true;
    }
    // The six characters Windows forbids inside a name. ':' is left out because the drive letter needs it.
    return c == '"' || c == '<' || c == '>' || c == '|' || c == '?' || c == '*';
}

std::string pathToUtf8(const std::filesystem::path &path)
{
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
}

std::filesystem::path toTreePath(const std::string &utf8Path)
{
    return misc::utf8ToPath(utf8Path);
}
