// Checks how the producer's threads end: at the game's ExitProcess, at an Aurie
// unload on an ordinary thread (MdUnmapImage), and at Aurie's own unload from
// DllMain with the loader lock held. Every case runs in a child process (this
// executable, given the case name), because what is under test is how that
// process ends. exit_teardown_probe.cpp describes the shapes it loads.
//
//   hsot_exit_teardown_smoke <probe.dll> <host.dll>
//   hsot_exit_teardown_smoke --case <name> <probe.dll> <host.dll>

#include <Windows.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

namespace {

// How a child ends. None of these goes through Windows Error Reporting, so a
// failing case leaves no report and no dump in %LOCALAPPDATA%\CrashDumps: the
// abort that the game's exit turned into a fast fail ends a child with
// kExitAbort instead (ArmChild).
constexpr UINT kExitClean = 0;
constexpr UINT kExitTerminate = 0x7E1;
constexpr UINT kExitAbort = 0x7E2;
constexpr UINT kExitException = 0x7E3;
constexpr UINT kExitCheckFailed = 0x7E4;
constexpr UINT kExitHung = 0x7E5;
constexpr DWORD kCaseTimeoutMs = 20000;

// exit_teardown_probe.cpp's shapes and hsot::aurie::UnloadOutcome.
constexpr int kShapeCurrent = 0;
constexpr int kShapeStalled = 1;
constexpr int kShapeBeforeFix = 2;
constexpr int kOutcomeStopped = 0;
constexpr int kOutcomePinned = 1;

using ProbeStartFunction = int (*)(int) noexcept;
using ProbeFunction = int (*)() noexcept;
using HostStartFunction = int (*)(const wchar_t*) noexcept;

struct Case {
    const wchar_t* name;
    UINT expected_exit;
    // A second exit code that means the same, or expected_exit again.
    UINT also_accepted;
    const char* what;
};

// The control ends in abort(): at ExitProcess the terminate handler ArmChild
// sets on this thread has been observed not to run, so std::terminate goes
// straight on to abort, as in the game's dumps. A std::terminate that does
// reach the handler is the same failure.
constexpr Case kCases[] = {
    {L"exit-before-fix", kExitAbort, kExitTerminate,
        "control: a std::thread global still joinable at ExitProcess aborts the process"},
    {L"exit", kExitClean, kExitClean,
        "ExitProcess with the publisher and the pipe worker running"},
    {L"unload", kExitClean, kExitClean,
        "ModuleUnload on an ordinary thread joins both threads; FreeLibrary unmaps"},
    {L"unload-stalled", kExitClean, kExitClean,
        "a publisher that does not end in time: the module is pinned and stays mapped"},
    {L"unload-from-dllmain", kExitClean, kExitClean,
        "ModuleUnload from DllMain with the loader lock held returns; the process exits cleanly"},
};

[[noreturn]] void End(const UINT code) noexcept {
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) {
        Sleep(INFINITE);
    }
}

[[noreturn]] void OnTerminate() {
    End(kExitTerminate);
}

[[noreturn]] void OnAbort(int) {
    End(kExitAbort);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS*) {
    End(kExitException);
}

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "     check failed, line %d: %s\n", __LINE__, #condition); \
            std::fflush(stderr);                                                    \
            End(kExitCheckFailed);                                                  \
        }                                                                           \
    } while (false)

void ArmChild() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::set_terminate(&OnTerminate);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    static_cast<void>(std::signal(SIGABRT, &OnAbort));
    SetUnhandledExceptionFilter(&OnUnhandledException);
}

[[nodiscard]] std::wstring WithBackslashes(std::wstring path) {
    for (auto& c : path) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    return path;
}

[[nodiscard]] std::wstring FileName(const std::wstring& path) {
    return path.substr(path.find_last_of(L'\\') + 1U);
}

[[nodiscard]] std::wstring PipeName() {
    return L"\\\\.\\pipe\\HSOfflineTrackerBridge_" + std::to_wstring(GetCurrentProcessId());
}

// Connects to the probe's pipe and reads one message, so the pipe worker has
// written and keeps writing while the case goes on.
[[nodiscard]] HANDLE ConnectAndRead() {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 500 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
        pipe = CreateFileW(PipeName().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(10);
        }
    }
    CHECK(pipe != INVALID_HANDLE_VALUE);
    char buffer[1024]{};
    DWORD read{};
    CHECK(ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) != FALSE && read > 0U);
    return pipe;
}

[[nodiscard]] bool PipeGone() {
    const HANDLE pipe =
        CreateFileW(PipeName().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
        return false;
    }
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

[[nodiscard]] HMODULE StartProbe(const std::wstring& probe_path, const int shape) {
    const HMODULE probe = LoadLibraryW(probe_path.c_str());
    CHECK(probe != nullptr);
    const auto start = reinterpret_cast<ProbeStartFunction>(GetProcAddress(probe, "ProbeStart"));
    CHECK(start != nullptr);
    CHECK(start(shape) == 0);
    return probe;
}

[[nodiscard]] int CallProbe(const HMODULE probe, const char* name) {
    const auto function = reinterpret_cast<ProbeFunction>(GetProcAddress(probe, name));
    CHECK(function != nullptr);
    return function();
}

[[noreturn]] void RunCase(
    const std::wstring_view name,
    const std::wstring& probe_path,
    const std::wstring& host_path) {
    ArmChild();
    const std::wstring probe_name = FileName(probe_path);

    if (name == L"exit-before-fix" || name == L"exit") {
        static_cast<void>(StartProbe(probe_path, name == L"exit" ? kShapeCurrent : kShapeBeforeFix));
        static_cast<void>(ConnectAndRead());
        // How the game ends. The probe's static destructors run in here, after
        // Windows has terminated both of its threads.
        ExitProcess(kExitClean);
    }

    if (name == L"unload") {
        const HMODULE probe = StartProbe(probe_path, kShapeCurrent);
        CloseHandle(ConnectAndRead());
        // MdUnmapImage: ModuleUnload on an ordinary thread, then FreeLibrary.
        CHECK(CallProbe(probe, "ProbeUnload") == kOutcomeStopped);
        CHECK(PipeGone());
        FreeLibrary(probe);
        CHECK(GetModuleHandleW(probe_name.c_str()) == nullptr);
        ExitProcess(kExitClean);
    }

    if (name == L"unload-stalled") {
        const HMODULE probe = StartProbe(probe_path, kShapeStalled);
        CloseHandle(ConnectAndRead());
        CHECK(CallProbe(probe, "ProbeUnload") == kOutcomePinned);
        FreeLibrary(probe);
        // Still mapped: the publisher is still running the probe's code.
        CHECK(GetModuleHandleW(probe_name.c_str()) == probe);
        ExitProcess(kExitClean);
    }

    if (name == L"unload-from-dllmain") {
        const HMODULE host = LoadLibraryW(host_path.c_str());
        CHECK(host != nullptr);
        const auto start = reinterpret_cast<HostStartFunction>(GetProcAddress(host, "HostStart"));
        CHECK(start != nullptr);
        CHECK(start(probe_path.c_str()) == 0);
        CloseHandle(ConnectAndRead());
        // The framework's own unload: the host's DLL_PROCESS_DETACH runs the
        // probe's ModuleUnload and FreeLibrary with the loader lock held.
        const ULONGLONG started = GetTickCount64();
        FreeLibrary(host);
        const ULONGLONG took = GetTickCount64() - started;
        const HMODULE probe = GetModuleHandleW(probe_name.c_str());
        if (probe) {
            CHECK(CallProbe(probe, "ProbeLastUnload") == kOutcomePinned);
            std::printf("     the publisher could not exit under the loader lock; "
                "the probe was pinned after %llu ms\n", took);
        } else {
            std::printf("     the publisher exited under the loader lock; "
                "the probe was unloaded after %llu ms\n", took);
        }
        std::fflush(stdout);
        ExitProcess(kExitClean);
    }

    std::fprintf(stderr, "     unknown case %ls\n", std::wstring(name).c_str());
    std::fflush(stderr);
    End(kExitCheckFailed);
}

void MakeInheritable(const DWORD which) {
    const HANDLE handle = GetStdHandle(which);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        static_cast<void>(SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
    }
}

// Runs one case in a child and returns its exit code, or kExitHung.
[[nodiscard]] DWORD RunChild(
    const std::wstring& self,
    const Case& test_case,
    const std::wstring& probe_path,
    const std::wstring& host_path,
    ULONGLONG& took_ms) {
    std::wstring command = L"\"" + self + L"\" --case " + test_case.name + L" \"" + probe_path +
        L"\" \"" + host_path + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    std::fflush(stdout);
    std::fflush(stderr);
    const ULONGLONG started = GetTickCount64();
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
            &startup, &process)) {
        took_ms = 0U;
        return kExitCheckFailed;
    }
    CloseHandle(process.hThread);
    DWORD code = kExitHung;
    if (WaitForSingleObject(process.hProcess, kCaseTimeoutMs) == WAIT_OBJECT_0) {
        static_cast<void>(GetExitCodeProcess(process.hProcess, &code));
    } else {
        TerminateProcess(process.hProcess, kExitHung);
        static_cast<void>(WaitForSingleObject(process.hProcess, 5000));
    }
    took_ms = GetTickCount64() - started;
    CloseHandle(process.hProcess);
    return code;
}

} // namespace

int wmain(const int argument_count, wchar_t* arguments[]) {
    if (argument_count == 5 && std::wstring_view(arguments[1]) == L"--case") {
        RunCase(arguments[2], WithBackslashes(arguments[3]), WithBackslashes(arguments[4]));
    }
    if (argument_count != 3) {
        std::fprintf(stderr, "usage: hsot_exit_teardown_smoke <probe.dll> <host.dll>\n");
        return 2;
    }
    const std::wstring probe_path = WithBackslashes(arguments[1]);
    const std::wstring host_path = WithBackslashes(arguments[2]);
    wchar_t self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0U) {
        return 2;
    }
    // Children inherit the error mode and the standard handles.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    MakeInheritable(STD_INPUT_HANDLE);
    MakeInheritable(STD_OUTPUT_HANDLE);
    MakeInheritable(STD_ERROR_HANDLE);

    int failures = 0;
    for (const auto& test_case : kCases) {
        ULONGLONG took_ms{};
        const DWORD code = RunChild(self, test_case, probe_path, host_path, took_ms);
        const bool passed = code == test_case.expected_exit || code == test_case.also_accepted;
        std::printf("%s %-20ls exit 0x%lx, expected 0x%x, %llu ms: %s\n",
            passed ? "ok  " : "FAIL", test_case.name, code, test_case.expected_exit, took_ms,
            test_case.what);
        std::fflush(stdout);
        if (!passed) {
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
