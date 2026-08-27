#include "diagnostics/CrashDumpHandler.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstring>

namespace
{
    // All paths are preformatted at install time so the exception filter only performs
    // Win32 calls on static buffers (a crashed process may have a corrupt heap).
    wchar_t gAppDataMiniDawDir[MAX_PATH] = {};
    wchar_t gCrashDumpDir[MAX_PATH] = {};
    wchar_t gLastOperationPath[MAX_PATH] = {};
    wchar_t gExePath[MAX_PATH] = {};
    char gVersionString[64] = "unknown";
    volatile LONG gCrashHandlerEntered = 0;

#if defined(NDEBUG)
    constexpr const char* kBuildConfig = "Release";
#else
    constexpr const char* kBuildConfig = "Debug";
#endif

    // MiniDumpNormal + indirectly referenced memory + scanned stacks keeps dumps in the
    // low tens of MB even with plugin-heavy sessions, while still capturing the memory
    // that crash-stack pointers reference (enough to inspect the faulting object).
    // Thread info + unloaded modules cost almost nothing and help triage.
    constexpr MINIDUMP_TYPE kDumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory
        | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

    void writeTextFileRaw(const wchar_t* path, const char* text) noexcept
    {
        const HANDLE h = ::CreateFileW(
            path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return;
        }
        DWORD written = 0;
        ::WriteFile(h, text, static_cast<DWORD>(::strlen(text)), &written, nullptr);
        ::CloseHandle(h);
    }

    LONG WINAPI crashDumpUnhandledExceptionFilter(EXCEPTION_POINTERS* const info) noexcept
    {
        // One attempt only: a second fault inside the handler must not recurse.
        if (::InterlockedExchange(&gCrashHandlerEntered, 1) != 0)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (gCrashDumpDir[0] == L'\0')
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        ::CreateDirectoryW(gAppDataMiniDawDir, nullptr);
        ::CreateDirectoryW(gCrashDumpDir, nullptr);

        SYSTEMTIME st = {};
        ::GetLocalTime(&st);
        const DWORD pid = ::GetCurrentProcessId();

        wchar_t basePath[MAX_PATH] = {};
        ::swprintf_s(basePath,
                     L"%s\\MiniDAWLab-crash-%04u%02u%02u-%02u%02u%02u-pid%lu",
                     gCrashDumpDir,
                     st.wYear,
                     st.wMonth,
                     st.wDay,
                     st.wHour,
                     st.wMinute,
                     st.wSecond,
                     static_cast<unsigned long>(pid));

        // 1) Minidump.
        wchar_t dumpPath[MAX_PATH] = {};
        ::swprintf_s(dumpPath, L"%s.dmp", basePath);
        BOOL dumpOk = FALSE;
        {
            const HANDLE hDump = ::CreateFileW(
                dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hDump != INVALID_HANDLE_VALUE)
            {
                MINIDUMP_EXCEPTION_INFORMATION mdei = {};
                mdei.ThreadId = ::GetCurrentThreadId();
                mdei.ExceptionPointers = info;
                mdei.ClientPointers = FALSE;
                dumpOk = ::MiniDumpWriteDump(::GetCurrentProcess(),
                                             pid,
                                             hDump,
                                             kDumpType,
                                             info != nullptr ? &mdei : nullptr,
                                             nullptr,
                                             nullptr);
                ::CloseHandle(hDump);
            }
        }

        // 2) Snapshot the last-operation breadcrumb next to the dump.
        wchar_t lastOpCopyPath[MAX_PATH] = {};
        ::swprintf_s(lastOpCopyPath, L"%s-last-operation.txt", basePath);
        const BOOL lastOpCopied = ::CopyFileW(gLastOperationPath, lastOpCopyPath, FALSE);

        // 3) Resolve the faulting module so the metadata file carries a module-relative
        //    offset that `scripts/symbolize-crash.ps1` can look up in the archived PDB.
        DWORD exceptionCode = 0;
        unsigned long long exceptionAddress = 0;
        unsigned long long moduleBase = 0;
        unsigned long long moduleOffset = 0;
        wchar_t faultModulePath[MAX_PATH] = L"(unknown)";
        if (info != nullptr && info->ExceptionRecord != nullptr)
        {
            exceptionCode = info->ExceptionRecord->ExceptionCode;
            void* const addr = info->ExceptionRecord->ExceptionAddress;
            exceptionAddress = reinterpret_cast<unsigned long long>(addr);
            MEMORY_BASIC_INFORMATION mbi = {};
            if (addr != nullptr && ::VirtualQuery(addr, &mbi, sizeof(mbi)) != 0
                && mbi.AllocationBase != nullptr)
            {
                moduleBase = reinterpret_cast<unsigned long long>(mbi.AllocationBase);
                moduleOffset = exceptionAddress - moduleBase;
                ::GetModuleFileNameW(static_cast<HMODULE>(mbi.AllocationBase),
                                     faultModulePath,
                                     MAX_PATH);
            }
        }

        // 4) Human-readable metadata beside the dump.
        wchar_t txtPath[MAX_PATH] = {};
        ::swprintf_s(txtPath, L"%s.txt", basePath);
        char text[3072] = {};
        ::sprintf_s(
            text,
            "MiniDAWLab (Danielssons Audio Lab) crash report\r\n"
            "time (local): %04u-%02u-%02u %02u:%02u:%02u\r\n"
            "version: %s\r\n"
            "build config: %s\r\n"
            "executable: %ws\r\n"
            "process id: %lu\r\n"
            "exception code: 0x%08lX\r\n"
            "exception address: 0x%016llX\r\n"
            "faulting module: %ws\r\n"
            "module base: 0x%016llX\r\n"
            "module offset: 0x%llX\r\n"
            "minidump written: %s (%ws)\r\n"
            "last-operation copied: %s (%ws)\r\n"
            "related logs (in %%APPDATA%%\\MiniDAWLab\\): last-operation.txt,"
            " track-delete-diag.log, mixdown-diag.log, project load/save diag logs\r\n"
            "symbolize: powershell -File scripts\\symbolize-crash.ps1 -CrashText <this file>"
            " -Exe <matching MiniDAWLab.exe with .pdb beside it; releases are archived under"
            " dist\\symbols\\>\r\n",
            st.wYear,
            st.wMonth,
            st.wDay,
            st.wHour,
            st.wMinute,
            st.wSecond,
            gVersionString,
            kBuildConfig,
            gExePath,
            static_cast<unsigned long>(pid),
            static_cast<unsigned long>(exceptionCode),
            exceptionAddress,
            faultModulePath,
            moduleBase,
            moduleOffset,
            dumpOk ? "yes" : "NO",
            dumpPath,
            lastOpCopied ? "yes" : "NO",
            lastOpCopyPath);
        writeTextFileRaw(txtPath, text);

        // Let the OS default handling continue so Windows Error Reporting still records
        // the Event Log entry (Event ID 1000) and the process terminates normally.
        return EXCEPTION_CONTINUE_SEARCH;
    }
} // namespace

void installCrashDumpHandler() noexcept
{
    wchar_t appData[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return; // No APPDATA: crash dumps unavailable; nothing else to do.
    }
    ::swprintf_s(gAppDataMiniDawDir, L"%s\\MiniDAWLab", appData);
    ::swprintf_s(gCrashDumpDir, L"%s\\MiniDAWLab\\crash-dumps", appData);
    ::swprintf_s(gLastOperationPath, L"%s\\MiniDAWLab\\last-operation.txt", appData);
    ::GetModuleFileNameW(nullptr, gExePath, MAX_PATH);
#if defined(JUCE_APPLICATION_VERSION_STRING)
    ::strncpy_s(gVersionString, JUCE_APPLICATION_VERSION_STRING, _TRUNCATE);
#endif
    ::SetUnhandledExceptionFilter(&crashDumpUnhandledExceptionFilter);
}

__declspec(noinline) void triggerIntentionalCrashForStabilityTest() noexcept
{
    // Deliberate access violation with a recognizable stack frame; the volatile write
    // cannot be optimized away.
    volatile int* const nullTarget = nullptr;
    *nullTarget = 0x51AB; // "STAB"ility test marker
}

#else // !_WIN32

void installCrashDumpHandler() noexcept {}
void triggerIntentionalCrashForStabilityTest() noexcept {}

#endif
