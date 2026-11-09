#ifndef CLIENT_SRC_CRASHDUMP_H
#define CLIENT_SRC_CRASHDUMP_H

#include <cstddef>
#include <filesystem>

// Crash dumps written by the client itself. The client is built on Windows only (root CMakeLists.txt), so
// dbghelp, crtdbg and the SEH filter can be used without a platform guard; MiniDumpWriteDump is in Dbghelp,
// which fb_system_links does not list, so client/CMakeLists.txt links it on its own.
namespace crash
{

// The folder the exe itself sits in, the rule the state file already uses (stateFilePath() in
// client/src/TransferState.cpp). The current directory is what it falls back to when GetModuleFileNameW fails.
std::filesystem::path moduleDirectory();

// filebrowser-crash-<pid>-<yyyymmdd>-<hhmmss>.dmp. This only makes the name; the folder is made in install() and
// created again here when it is missing. One name per process, so two runs that crash do not overwrite each other.
std::filesystem::path dumpPath(const std::filesystem::path &folder);

// Writes a dump of the running process into folder. pointers is an EXCEPTION_POINTERS * from the exception
// filter, or nullptr when the caller has none: the invalid-parameter callback gets no exception record, and
// the thread stacks in the dump are then the only thing that names the caller. Null is also what a test
// passes, so this header does not include windows.h.
// Full memory is chosen because the failures worth catching here are the debug CRT checks: they report the
// heap, not the call that damaged it, so the heap has to be readable in the dump afterwards. MiniDumpWithThreadInfo
// is added on top because the flags as they stand record the stack only for the thread that raised the
// exception, and the transfer workers are on other threads.
// Nothing in this function allocates from the heap, because it runs at the moment a heap check fires; dbghelp
// still allocates inside itself, so a dump taken at a heap check can still fail.
bool writeDump(const std::filesystem::path &folder, void *pointers);

// Installs the handlers and makes folder the place they write to. The folder is created here so a crash
// does not have to create it again. The handlers are the ones listed in CrashDump.cpp: the top-level exception
// filter, the invalid-parameter handler, std::set_terminate, and the debug CRT report hook - abort() and the
// heap checks are reported rather than thrown, so the report hook is the only place they can be caught, and the
// report modes are pointed at stderr so the message is there instead of in a dialog box. The hook also saves that
// message as a .txt with the same name next to the dump, because stderr is nothing when the process has no
// console; the file is written only with the debug CRT, since that is the only build with a report hook. After the
// message the file lists the frames the hook was reached from, because the message names the check and not the code
// that called it: the addresses are written as they are, since naming them needs dbghelp's symbol functions, which
// allocate, so they are matched to the module list the dump carries.
void install(const std::filesystem::path &folder);
} // namespace crash

#endif
