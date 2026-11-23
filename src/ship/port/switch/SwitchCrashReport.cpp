// Crash reports for the Switch and Switch 2 builds.
//
// The console closes a homebrew program that crashes without saying where.
// This file writes logs/crash-<time>.txt (next to the other logs) when:
// - the CPU faults (data abort, bad PC, ...): libnx calls
//   __libnx_exception_handler with the registers;
// - C++ ends the program: an exception nobody caught (std::terminate);
// - abort() (SIGABRT, or __syscall_abort for libsysbase's abort) or a failed
//   assert() (with its message), in our code or in a library (GPU driver,
//   SDL, ...);
// - exit() is called while nobody asked the program to quit (exit is wrapped
//   with -Wl,--wrap=exit, src/CMakeLists.txt).
//
// Addresses are written as offsets into the program image (PC - __start__), so
// they can be looked up in the build's .elf:
//   aarch64-none-elf-addr2line -Cfipe <name>.elf 0x<offset> ...
// The stack scan lists every word on the stack that points into the program's
// code: return addresses of the calls that led to the crash (and some stale
// ones), newest first.
#ifdef __SWITCH__

#include <switch.h>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cxxabi.h>
#include <exception>
#include <sys/stat.h>
#include <typeinfo>

extern "C" {
// Set by the linker script: the start of the program image and the end of its code.
// __start__ is an absolute 0 in the linker script, not the load address, so
// use _start, the first instruction of the image.
extern char _start[] __attribute__((visibility("hidden")));
extern char __rodata_start[];

// The program's threads name themselves with LusSwitch_SetThreadName, so the
// report says which one crashed (main, audio, ...).
static thread_local const char* tThreadName = nullptr;
static const char* sBuildInfo = "";

// Set when the program was asked to quit (quit button, HOME menu close, ...),
// so that the exit() that follows does not write a report.
static volatile bool sQuitRequested = false;
static const char* sQuitReason = nullptr;

void LusSwitch_NoteQuit(const char* reason) {
    if (!sQuitRequested) {
        sQuitReason = reason;
    }
    sQuitRequested = true;
}

void LusSwitch_SetThreadName(const char* name) {
    tThreadName = name;
}

// Build version and commit, written at the top of each report. The string must
// stay valid for the whole run.
void LusSwitch_SetBuildInfo(const char* info) {
    sBuildInfo = info != nullptr ? info : "";
}

// The handler runs on this stack (libnx's default is only 1 KiB).
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
// libnx drops an exception when the process reports a debugger, unless this is
// set. Forcing it on from boot stopped the game launching in one test build
// (not proven the cause), so only the test button turns it on for now.
extern u32 __nx_exception_ignoredebug;
}

namespace {

char sReport[0x4000];
size_t sReportLen = 0;
volatile bool sReportWritten = false;

void Append(const char* fmt, ...) {
    if (sReportLen >= sizeof(sReport)) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(sReport + sReportLen, sizeof(sReport) - sReportLen, fmt, args);
    va_end(args);
    if (n > 0) {
        sReportLen += static_cast<size_t>(n);
        if (sReportLen > sizeof(sReport)) {
            sReportLen = sizeof(sReport);
        }
    }
}

uintptr_t ImageStart() {
    return reinterpret_cast<uintptr_t>(_start);
}

bool IsCode(uintptr_t address) {
    return address >= ImageStart() && address < reinterpret_cast<uintptr_t>(__rodata_start);
}

// An address in the program's code as "0x<offset>", anything else as "abs 0x<address>".
void AppendAddress(const char* name, uintptr_t address) {
    if (IsCode(address)) {
        Append("%-4s 0x%lx\n", name, address - ImageStart());
    } else {
        Append("%-4s abs 0x%lx\n", name, address);
    }
}

// Lists the words on the stack from sp to the end of the stack's memory region
// that point into the program's code.
void AppendStackScan(uintptr_t sp) {
    MemoryInfo info = {};
    u32 pageInfo = 0;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, sp))) {
        Append("stack: not readable\n");
        return;
    }
    uintptr_t end = info.addr + info.size;
    // Enough for the newest calls; a full scan of an 8 MiB stack is too slow.
    if (end - sp > 0x10000) {
        end = sp + 0x10000;
    }
    Append("\nstack scan (offsets of return addresses, newest first):\n");
    int found = 0;
    for (uintptr_t p = sp & ~static_cast<uintptr_t>(7); p + 8 <= end && found < 64; p += 8) {
        uintptr_t value = *reinterpret_cast<const uintptr_t*>(p);
        if (IsCode(value)) {
            Append("0x%lx\n", value - ImageStart());
            found++;
        }
    }
}

void AppendHeader(const char* what) {
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmNow);
    Append("Crash report, %s\n%s\n", stamp, what);
    u64 threadId = 0;
    svcGetThreadId(&threadId, CUR_THREAD_HANDLE);
    Append("thread: %s (id %lu)\n", tThreadName != nullptr ? tThreadName : "unnamed", threadId);
    if (sBuildInfo[0] != '\0') {
        Append("build: %s\n", sBuildInfo);
    }
    Append("image 0x%lx (offsets below are from here; look them up in the .elf)\n\n", ImageStart());
}

// Writes the report to logs/crash-<time>.txt (the program's folder is the
// working directory). Only the first report of a run is written.
void WriteReport() {
    if (sReportWritten) {
        return;
    }
    sReportWritten = true;
    mkdir("logs", 0777);
    char path[64];
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    strftime(path, sizeof(path), "logs/crash-%Y%m%d-%H%M%S.txt", &tmNow);
    FILE* file = fopen(path, "w");
    if (file == nullptr) {
        return;
    }
    fwrite(sReport, 1, sReportLen, file);
    fclose(file);
}

const char* ExceptionName(u32 desc) {
    switch (desc) {
        case ThreadExceptionDesc_InstructionAbort:
            return "instruction abort (bad PC)";
        case ThreadExceptionDesc_MisalignedPC:
            return "misaligned PC";
        case ThreadExceptionDesc_MisalignedSP:
            return "misaligned SP";
        case ThreadExceptionDesc_SError:
            return "SError";
        case ThreadExceptionDesc_BadSVC:
            return "bad SVC";
        case ThreadExceptionDesc_Trap:
            return "trap";
        case ThreadExceptionDesc_Other:
            return "data abort or other fault";
        default:
            return "unknown exception";
    }
}

// Reports from the current stack (terminate and abort), for which there is no
// register dump.
[[noreturn]] void ReportAndExit(const char* what) {
    AppendHeader(what);
    AppendAddress("lr", reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    AppendStackScan(reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
    WriteReport();
    _Exit(1);
}

void OnTerminate() {
    char what[512] = "std::terminate: ";
    std::exception_ptr current = std::current_exception();
    if (current) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            int status = 0;
            const char* mangled = typeid(e).name();
            char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
            snprintf(what + strlen(what), sizeof(what) - strlen(what), "uncaught %s: %s",
                     status == 0 && demangled != nullptr ? demangled : mangled, e.what());
        } catch (...) {
            strncat(what, "uncaught exception of an unknown type", sizeof(what) - strlen(what) - 1);
        }
    } else {
        strncat(what, "called without an exception", sizeof(what) - strlen(what) - 1);
    }
    ReportAndExit(what);
}

void OnAbortSignal(int) {
    ReportAndExit("abort() (SIGABRT), for example from a failed assert");
}

} // namespace

extern "C" {
// abort() calls this (newlib/libsysbase), and so does a failed assert().
// libsysbase's abort() calls this. Only a weak reference points here, so it is
// marked used, or link time optimization drops it (src/CMakeLists.txt also
// adds -u). newlib's abort() raises SIGABRT instead: OnAbortSignal.
[[noreturn]] __attribute__((used)) void __syscall_abort(void) {
    ReportAndExit("abort(): a library or the game gave up (see the log and the stack below)");
}

// A failed assert(). newlib's __assert_func prints the message to stderr (lost
// on the console) and calls abort(), so it is wrapped (-Wl,--wrap=__assert_func)
// to put the message in the report.
[[noreturn]] void __wrap___assert_func(const char* file, int line, const char* func, const char* expr) {
    char what[512];
    snprintf(what, sizeof(what), "assert failed: %s\n  at %s:%d (%s)", expr != nullptr ? expr : "?",
             file != nullptr ? file : "?", line, func != nullptr ? func : "?");
    ReportAndExit(what);
}

[[noreturn]] void __real_exit(int code);

// exit() while the program was not asked to quit: write where it came from.
[[noreturn]] void __wrap_exit(int code) {
    if (!sQuitRequested || code != 0) {
        char what[160];
        snprintf(what, sizeof(what), "exit(%d) called%s%s", code,
                 sQuitRequested ? " after a quit request: " : " but nobody asked the program to quit",
                 sQuitRequested && sQuitReason != nullptr ? sQuitReason : "");
        AppendHeader(what);
        AppendAddress("lr", reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
        AppendStackScan(reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
        WriteReport();
    }
    __real_exit(code);
}
}

extern "C" void LusSwitch_EnableFaultReports(void) {
    u64 debugged = 0;
    Result rc = svcGetInfo(&debugged, InfoType_DebuggerAttached, INVALID_HANDLE, 0);
    Append("debugger attached: %lu (svcGetInfo 0x%x)\n", debugged, rc);
    __nx_exception_ignoredebug = 1;
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    char what[96];
    snprintf(what, sizeof(what), "CPU exception 0x%x: %s", ctx->error_desc, ExceptionName(ctx->error_desc));
    AppendHeader(what);
    AppendAddress("pc", ctx->pc.x);
    AppendAddress("lr", ctx->lr.x);
    Append("sp   abs 0x%lx\nfar  abs 0x%lx (the address that was read or written)\nesr  0x%x\n\n", ctx->sp.x,
           ctx->far.x, ctx->esr);
    for (int i = 0; i < 29; i++) {
        Append("x%-2d 0x%016lx%s", i, ctx->cpu_gprs[i].x, (i % 2) == 1 ? "\n" : "  ");
    }
    Append("\nfp  0x%016lx\n", ctx->fp.x);
    AppendStackScan(ctx->sp.x);
    WriteReport();
    // Let the system close the program as it would without this handler.
    svcReturnFromException(MAKERESULT(Module_Libnx, LibnxError_BadInput));
}

namespace SwitchCrashReport {
// Installs the terminate and abort handlers. The CPU exception handler is
// always on.
void Install() {
    LusSwitch_SetThreadName("main");
    std::set_terminate(OnTerminate);
    signal(SIGABRT, OnAbortSignal);
}
} // namespace SwitchCrashReport

#endif
