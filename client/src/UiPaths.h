#ifndef CLIENT_SRC_UI_PATHS_H
#define CLIENT_SRC_UI_PATHS_H

#include <filesystem>
#include <string>

// Path helpers shared by the view and the controller. They work on the UTF-8 strings the RPC carries,
// not on std::filesystem::path: assigning a narrow string to a path goes through the current code page, so a
// name that is not representable in it produces a different path and the tree lookups stop matching. The model
// does not use them; it only holds the strings.

// Parent of a UTF-8 path string, empty when the path is already a root. This works on the string
// because the paths that come back from the RPC are UTF-8 and the UI only needs to walk one level up;
// interpreting them as std::filesystem::path would depend on the native narrow encoding.
// The separator is kept: the parent of "D:\Tmp" is "D:\" and the parent of "/usr" is "/", because
// "D:" is not the root of the drive, it is the process's current directory on that drive.
std::string parentDirectory(const std::string &path);

// A Windows drive-relative path ("D:" or "D:tmp") is not the root of the drive: it is the current
// directory the process has on that drive, so the server lists wherever its process was started
// instead of the drive. A file browser means the drive root, so the missing separator is added before
// the path is used; that also keeps the entry paths the server builds consistent (it appends the file
// name to the directory as given, so a drive-relative directory gives "D:name" with no separator).
std::string normalizeDirectory(const std::string &directory);

// Whether a character may appear in a path being typed: the ones Windows forbids inside a name
// (quote, <, >, |, ?, *) plus control characters are rejected. ':' is not one of them here because the drive
// letter needs it and this is called one character at a time, so it cannot tell a drive colon from another one.
// Everything above ASCII is accepted: the UI font covers Chinese names.
bool isIllegalPathChar(char32_t c);

// The UTF-8 copy of a path for status lines. The project builds with C++17, where MSVC's path::u8string()
// returns std::string rather than std::u8string, so the type is taken from the call instead of named.
std::string pathToUtf8(const std::filesystem::path &path);

// The same conversion Client.cpp uses for the paths coming back from the RPC. The scanned tree is keyed by
// these paths, so a row lookup has to build its key the same way.
std::filesystem::path toTreePath(const std::string &utf8Path);

#endif
