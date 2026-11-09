#ifndef NOMINMAX
#define NOMINMAX
#endif
// Same as client/src/ServerLauncher.h: windows.h must not bring in winsock.h, which clashes with winsock2.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <dbghelp.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <iterator>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#include "CrashDump.h"

namespace crash
{

namespace
{

// Fixed storage. The dump is written at the moment a debug CRT heap check fires, and at that moment the heap is the
// part that cannot be trusted, so the path that writes the dump must not allocate from it. dbghelp still allocates
// inside itself, so this only keeps our own part clean: a dump taken at a heap check can still fail.
constexpr std::size_t kPathSize = 1024;
constexpr std::size_t kMessageSize = 512;
// One line of the frame list: 0x, the digits of a 64-bit address, and the newline. The line with the number of frames
// is shorter, so the same size covers it.
constexpr std::size_t kLineSize = 32;
constexpr std::size_t kFrameCount = 16;
wchar_t gFolder[kPathSize] = L".";
bool gWritten = false;

// Written straight to the stderr handle. std::fprintf would need the buffer it owns, which is the heap.
void report(const wchar_t *text)
{
    const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
    {
        return;
    }

    // Four bytes per wide character is the worst case for UTF-8, so the whole message fits.
    char utf8[4 * kMessageSize];
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    if (length <= 0)
    {
        return;
    }

    DWORD written = 0;
    WriteFile(handle, utf8, static_cast<DWORD>(length), &written, nullptr);
}

// filebrowser-crash-<pid>-<yyyymmdd>-<hhmmss> plus the extension, into out. The extension is an argument because the
// dump and the text written for the same report have the same name, so the two files belong to each other. Digits
// and fixed separators only, so nothing here allocates; one name per process, so two runs that crash do not
// overwrite each other.
void formatName(wchar_t *out, std::size_t size, const wchar_t *extension)
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &now);

    swprintf_s(out, size, L"filebrowser-crash-%lu-%04d%02d%02d-%02d%02d%02d%s",
               static_cast<unsigned long>(GetCurrentProcessId()), local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
               local.tm_hour, local.tm_min, local.tm_sec, extension);
}

// Folder and name put together. The lengths are checked before formatting because swprintf_s reports a write that
// does not fit through the invalid parameter handler - the same handler a crash is being written for - and its
// counts are elements, not bytes.
bool joinPath(wchar_t *out, std::size_t size, const wchar_t *folder, const wchar_t *name)
{
    const std::size_t folderLength = wcslen(folder);
    const std::size_t nameLength = wcslen(name);
    if (folderLength + nameLength + 2 > size)
    {
        return false;
    }
    if (folderLength == 0 || folder[folderLength - 1] == L'\\' || folder[folderLength - 1] == L'/')
    {
        swprintf_s(out, size, L"%s%s", folder, name);
    }
    else
    {
        swprintf_s(out, size, L"%s\\%s", folder, name);
    }
    return true;
}

// One piece of the text file. It is written in pieces because the frame list is as long as the message can be: one
// buffer big enough for both is a buffer that can be truncated, and a truncated sprintf_s reports through the invalid
// parameter handler - the same handler a crash is being written for. These lines cannot be truncated.
bool writeText(const HANDLE handle, const char *text, std::size_t length)
{
    DWORD written = 0;
    return WriteFile(handle, text, static_cast<DWORD>(length), &written, nullptr) && written == length;
}

// The three report types as words, so the file says which one fired.
const char *reportName(int reportType)
{
    switch (reportType)
    {
    case _CRT_WARN:
        return "warn";
    case _CRT_ERROR:
        return "error";
    case _CRT_ASSERT:
        return "assert";
    default:
        return "report";
    }
}

// The frames the hook was reached from, one address per line. The message says which check fired, not which code
// reached it, and this is the only thing that can answer that at the moment the report is made. CaptureStackBackTrace
// does not allocate; naming an address would need dbghelp's symbol functions, which do, so the addresses are written
// as they are and the module is worked out afterwards from the module list the dump carries. Frame 0 is the hook
// itself, so the code that reported is the line after the CRT's own report frame. The list holds kFrameCount frames,
// so a stack deeper than that shows only its top.
void writeFrames(const HANDLE handle)
{
    void *frames[kFrameCount];
    const unsigned captured = CaptureStackBackTrace(0, static_cast<USHORT>(kFrameCount), frames, nullptr);

    char line[kLineSize];
    const int heading = sprintf_s(line, sizeof(line), "frames: %u\n", captured);
    if (heading > 0)
    {
        writeText(handle, line, static_cast<std::size_t>(heading));
    }
    for (unsigned i = 0; i < captured; i++)
    {
        const unsigned long long address = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]));
        const int length = sprintf_s(line, sizeof(line), "0x%llx\n", address);
        if (length > 0)
        {
            writeText(handle, line, static_cast<std::size_t>(length));
        }
    }
}

// The message a report carries is the part that says which check fired, and the default handling only prints it: it
// is gone when the process has no console. So the hook saves it next to the dump, same name and a .txt extension.
// The message is written as it came in, in the code page the CRT used, which is why nothing here re-encodes it.
// The frames follow the message, so the first two lines stay what a reader looks for.
bool writeReportText(const wchar_t *folder, int reportType, const char *message)
{
    wchar_t name[kPathSize];
    wchar_t path[kPathSize];
    formatName(name, kPathSize, L".txt");
    if (!joinPath(path, kPathSize, folder, name))
    {
        report(L"crash dump: the path is too long\n");
        return false;
    }

    const HANDLE handle = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        wchar_t failure[kMessageSize];
        swprintf_s(failure, std::size(failure), L"crash dump: could not write \"%s\": %lu\n", path, GetLastError());
        report(failure);
        return false;
    }

    // Checked before copying, for the same reason joinPath checks: a truncated write goes through the invalid
    // parameter handler, and that handler is what a heap check reaches.
    const char *type = reportName(reportType);
    const std::size_t typeLength = strlen(type);
    const std::size_t messageLength = strlen(message);
    if (typeLength + messageLength + 2 > kMessageSize)
    {
        report(L"crash dump: the report message is too long\n");
        CloseHandle(handle);
        return false;
    }

    char text[kMessageSize];
    memcpy(text, type, typeLength);
    text[typeLength] = '\n';
    memcpy(text + typeLength + 1, message, messageLength);

    // The frame list has to start on the line after the message. An assertion message already ends with the newline
    // the CRT puts there - abort() does not give one - so the second newline is only added when the message has none.
    const bool messageEndsInNewline = messageLength > 0 && message[messageLength - 1] == '\n';
    if (!messageEndsInNewline)
    {
        text[typeLength + 1 + messageLength] = '\n';
    }

    const bool ok = writeText(handle, text, typeLength + messageLength + 2 - (messageEndsInNewline ? 1 : 0));
    writeFrames(handle);
    CloseHandle(handle);
    return ok;
}

bool writeDumpNoAlloc(const wchar_t *folder, EXCEPTION_POINTERS *pointers)
{
    wchar_t message[kMessageSize];

    // install() makes the folder ahead of time; it is created again here for a caller that did not install() (the
    // tests). A name that exists but is not a directory is reported instead of created.
    const DWORD attributes = GetFileAttributesW(folder);
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        if (!CreateDirectoryW(folder, nullptr))
        {
            swprintf_s(message, std::size(message), L"crash dump: could not create \"%s\": %lu\n", folder,
                       GetLastError());
            report(message);
            return false;
        }
    }
    else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        swprintf_s(message, std::size(message), L"crash dump: \"%s\" is not a directory\n", folder);
        report(message);
        return false;
    }

    wchar_t name[kPathSize];
    wchar_t path[kPathSize];
    formatName(name, kPathSize, L".dmp");
    if (!joinPath(path, kPathSize, folder, name))
    {
        report(L"crash dump: the path is too long\n");
        return false;
    }

    const HANDLE handle = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        swprintf_s(message, std::size(message), L"crash dump: could not open \"%s\": %lu\n", path, GetLastError());
        report(message);
        return false;
    }

    // Reading the whole process means the heap is in the file, which is what a debug CRT heap check needs.
    const MINIDUMP_TYPE flags = static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithThreadInfo);

    // The exception information is its own struct, not the pointers, so it has to be assembled. The filter runs on
    // the thread that faulted, which is why the current thread is the one to report. Without it the dump still has
    // the thread stacks, only no exception record.
    MINIDUMP_EXCEPTION_INFORMATION exceptionInformation{};
    exceptionInformation.ThreadId = GetCurrentThreadId();
    exceptionInformation.ClientPointers = FALSE;
    exceptionInformation.ExceptionPointers = pointers;

    const bool ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), handle, flags,
                                      pointers == nullptr ? nullptr : &exceptionInformation, nullptr, nullptr);
    CloseHandle(handle);

    if (!ok)
    {
        swprintf_s(message, std::size(message), L"crash dump: MiniDumpWriteDump failed on \"%s\": %lu\n", path,
                   GetLastError());
        report(message);
        return false;
    }

    swprintf_s(message, std::size(message), L"crash dump written: %s\n", path);
    report(message);
    return true;
}

// The first handler to reach this gets the record: the same failure can reach two handlers, and the second must not
// write over what the first wrote.
bool claimRecord()
{
    if (gWritten)
    {
        return false;
    }
    gWritten = true;
    return true;
}

void writeOnce(EXCEPTION_POINTERS *pointers)
{
    if (claimRecord())
    {
        writeDumpNoAlloc(gFolder, pointers);
    }
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *pointers)
{
    writeOnce(pointers);
    return EXCEPTION_EXECUTE_HANDLER;
}

// The invalid parameter path: the default handler shows its message in a dialog box, so the message is lost when
// nobody is watching. Here it goes to stderr and the dump is written before the process stops. What the default
// does afterwards - stop - is kept by calling abort().
void invalidParameterHandler(const wchar_t *file, const wchar_t *function, const wchar_t *line, unsigned int reason,
                             uintptr_t argument)
{
    wchar_t message[512];
    swprintf_s(message, std::size(message),
               L"CRT invalid parameter: file=%ls function=%ls line=%ls reason=%u "
               L"argument=%llu\n",
               file ? file : L"", function ? function : L"", line ? line : L"", reason,
               static_cast<unsigned long long>(argument));
    report(message);

    writeOnce(nullptr);
    std::abort();
}

// An uncaught C++ exception does not reach the exception filter, so this is the only way to get a dump for it.
void terminateHandler()
{
    writeOnce(nullptr);
    std::abort();
}

#ifdef _DEBUG
// abort() and the debug heap checks are reported, not thrown: pressing the dialog's Abort button is an exit, so the
// exception filter above never sees them and std::set_terminate never sees them either. This hook is the only place
// they can be caught. Returning 0 leaves the default report alone, which is the message on stderr that install()
// set up, so the hook does not print it again. The hook exists only with the debug CRT; in a release build
// _CrtSetReportHook is a macro that does nothing.
int reportHook(int reportType, char *message, int *returnValue)
{
    if (claimRecord())
    {
        // The text first: at a heap check the dump can fail and the text cannot, and the text is the only record of
        // which check fired when there is no console to print it to.
        writeReportText(gFolder, reportType, message);
        writeDumpNoAlloc(gFolder, nullptr);
    }
    return 0;
}
#endif

} // namespace

std::filesystem::path moduleDirectory()
{
    wchar_t buffer[MAX_PATH + 1];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH + 1);
    if (length == 0)
    {
        return std::filesystem::current_path();
    }
    // GetModuleFileNameW does not terminate the string when the buffer is too small, so the length is what the
    // path is built from, not the pointer.
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
}

std::filesystem::path dumpPath(const std::filesystem::path &folder)
{
    wchar_t name[kPathSize];
    formatName(name, kPathSize, L".dmp");
    return folder / std::wstring(name);
}

bool writeDump(const std::filesystem::path &folder, void *pointers)
{
    // The caller already holds a path, so the only thing allocated here is what the caller allocated.
    return writeDumpNoAlloc(folder.c_str(), static_cast<EXCEPTION_POINTERS *>(pointers));
}

void install(const std::filesystem::path &folder)
{
    // Checked before copying: a truncated swprintf_s goes through the invalid parameter handler, and that is the
    // handler writing the dump here.
    if (wcslen(folder.c_str()) >= std::size(gFolder))
    {
        report(L"crash dump: the folder name is too long\n");
        return;
    }
    swprintf_s(gFolder, std::size(gFolder), L"%s", folder.c_str());

    wchar_t message[kMessageSize];
    if (GetFileAttributesW(gFolder) == INVALID_FILE_ATTRIBUTES && !CreateDirectoryW(gFolder, nullptr))
    {
        swprintf_s(message, std::size(message), L"crash dump: could not create \"%s\": %lu\n", gFolder, GetLastError());
        report(message);
    }

    // The report for an assertion and for abort() goes to stderr instead of a dialog box, so it is there when the
    // process is run from a terminal. _CRT_WARN keeps its default mode.
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_invalid_parameter_handler(invalidParameterHandler);
#ifdef _DEBUG
    _CrtSetReportHook(reportHook);
#endif
#endif
    std::set_terminate(terminateHandler);

    // Reached only when no debugger is attached: with one, the debugger gets the exception first, so running in
    // Visual Studio still stops at the fault instead of writing a dump.
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
}

} // namespace crash
