// Streams what the game is doing to a computer over UDP, and answers memory
// read requests. A crash on the console leaves no report, but whatever was
// already sent is safe on the computer. See switch2/debug_listener.py.
#ifdef __SWITCH__
#include "Switch2Debug.h"

#include <switch.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" char _start[] __attribute__((visibility("hidden")));

namespace {
constexpr int kConsolePort = 4242;
constexpr int kComputerPort = 4243;
constexpr size_t kMaxRead = 1024;

int sSocket = -1;
sockaddr_in sComputer = {};
std::atomic<const char*> sPhase[2] = { { "none" }, { "none" } };
std::atomic<uint32_t> sPhaseCount[2] = { { 0 }, { 0 } };
bool sVerbose = false;

// Profile. Each zone is only timed from one thread; the atomics let the main
// thread read and reset the audio thread's zone.
const char* const kZoneNames[SW2P_ZONE_COUNT] = {
    "game", "update", "build", "audiowait", "events", "guistart", "rdp",  "draw",  "upload",
    "shader", "fbcopy", "fbread", "guiend",  "flush",  "pace",     "swap", "audio",
    "resource", "fileread", "teximport",
};
struct ProfZone {
    uint64_t start = 0;
    std::atomic<uint64_t> total{ 0 };
    std::atomic<uint64_t> longest{ 0 };
    std::atomic<uint32_t> calls{ 0 };
};
bool sProfiling = false;
ProfZone sZones[SW2P_ZONE_COUNT];
std::atomic<uint32_t> sCounters[SW2C_COUNT];
std::atomic<int> sScene{ -1 };
uint32_t sTicks = 0;
uint32_t sPresents = 0;
uint32_t sLatePresents = 0;
uint32_t sReportNumber = 0;
uint64_t sLongestPresent = 0;
uint64_t sLastPresent = 0;
uint64_t sLastReport = 0;

uint64_t TicksToMicros(uint64_t ticks) {
    return ticks * 1000000 / armGetSystemTickFreq();
}

constexpr int kMaxBootMarks = 32;
const char* sBootNames[kMaxBootMarks];
uint64_t sBootTicks[kMaxBootMarks];
int sBootCount = 0;
bool sBootSent = false;

void Send(const char* data, size_t size);

void SendBootMarks(uint64_t now) {
    sBootSent = true;
    if (sBootCount == 0) {
        return;
    }
    char line[1200];
    int pos = snprintf(line, sizeof(line), "B boot total=%lu", TicksToMicros(now - sBootTicks[0]) / 1000);
    for (int i = 1; i < sBootCount && pos < static_cast<int>(sizeof(line)) - 48; i++) {
        pos += snprintf(line + pos, sizeof(line) - pos, " %s=%lu", sBootNames[i],
                        TicksToMicros(sBootTicks[i] - sBootTicks[i - 1]) / 1000);
    }
    pos += snprintf(line + pos, sizeof(line) - pos, " first-frame=%lu",
                    TicksToMicros(now - sBootTicks[sBootCount - 1]) / 1000);
    Send(line, pos);
}

void Send(const char* data, size_t size) {
    if (sSocket >= 0) {
        sendto(sSocket, data, size, 0, reinterpret_cast<sockaddr*>(&sComputer), sizeof(sComputer));
    }
}

void AnswerRead(uintptr_t address, size_t size) {
    static char reply[64 + kMaxRead * 2];
    if (size > kMaxRead) {
        size = kMaxRead;
    }
    MemoryInfo info = {};
    u32 pageInfo = 0;
    bool readable = R_SUCCEEDED(svcQueryMemory(&info, &pageInfo, address)) && (info.perm & Perm_R) != 0 &&
                    info.type != MemType_Unmapped && address + size <= info.addr + info.size;
    if (!readable) {
        Send(reply, snprintf(reply, sizeof(reply), "E 0x%lx unreadable", address));
        return;
    }
    int pos = snprintf(reply, sizeof(reply), "M 0x%lx ", address);
    const auto* bytes = reinterpret_cast<const uint8_t*>(address);
    for (size_t i = 0; i < size; i++) {
        pos += snprintf(reply + pos, 3, "%02x", bytes[i]);
    }
    Send(reply, pos);
}

} // namespace

extern "C" void Switch2Debug_Start(void) {
    FILE* file = fopen("debug-host.txt", "r");
    if (file == nullptr) {
        return;
    }
    char host[64] = {};
    bool haveHost = fscanf(file, "%63s", host) == 1;
    fclose(file);
    if (!haveHost) {
        return;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(kConsolePort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    sComputer.sin_family = AF_INET;
    sComputer.sin_port = htons(kComputerPort);
    if (inet_aton(host, &sComputer.sin_addr) == 0) {
        close(fd);
        return;
    }
    sSocket = fd;
    FILE* verbose = fopen("debug-verbose.txt", "r");
    if (verbose != nullptr) {
        sVerbose = true;
        fclose(verbose);
    }
    sProfiling = true;
    char hello[96];
    Send(hello, snprintf(hello, sizeof(hello), "S started, image 0x%lx", reinterpret_cast<uintptr_t>(_start)));
}

extern "C" bool Switch2Debug_Enabled(void) {
    return sSocket >= 0;
}

extern "C" void Switch2Debug_Log(const char* line) {
    Send(line, strlen(line));
}

extern "C" void Switch2Debug_Phase(int thread, const char* name) {
    if (sSocket < 0 || !sVerbose || sPhase[thread].exchange(name) == name) {
        return;
    }
    char line[96];
    uint32_t count = sPhaseCount[thread].fetch_add(1);
    Send(line, snprintf(line, sizeof(line), "P %s %s %u", thread == 0 ? "audio" : "main", name, count));
}

extern "C" bool Switch2Debug_Verbose(void) {
    return sSocket >= 0 && sVerbose;
}

extern "C" void Switch2Debug_BootMark(const char* name) {
    if (sBootCount < kMaxBootMarks) {
        sBootNames[sBootCount] = name;
        sBootTicks[sBootCount++] = armGetSystemTick();
    }
}

extern "C" unsigned long Switch2Debug_Micros(void) {
    return TicksToMicros(armGetSystemTick());
}

extern "C" void Switch2Prof_Begin(int zone) {
    if (sProfiling) {
        sZones[zone].start = armGetSystemTick();
    }
}

extern "C" void Switch2Prof_End(int zone) {
    if (!sProfiling || sZones[zone].start == 0) {
        return;
    }
    ProfZone& z = sZones[zone];
    const uint64_t elapsed = armGetSystemTick() - z.start;
    z.start = 0;
    z.total.fetch_add(elapsed, std::memory_order_relaxed);
    z.calls.fetch_add(1, std::memory_order_relaxed);
    if (elapsed > z.longest.load(std::memory_order_relaxed)) {
        z.longest.store(elapsed, std::memory_order_relaxed);
    }
}

extern "C" void Switch2Prof_Count(int counter, unsigned int amount) {
    if (sProfiling) {
        sCounters[counter].fetch_add(amount, std::memory_order_relaxed);
    }
}

namespace {
thread_local int sAnyDepth[SW2P_ZONE_COUNT] = {};
}

extern "C" unsigned long long Switch2Prof_EnterAny(int zone) {
    if (!sProfiling || sAnyDepth[zone]++ > 0) {
        return 0;
    }
    return armGetSystemTick();
}

extern "C" void Switch2Prof_LeaveAny(int zone, unsigned long long start) {
    if (!sProfiling) {
        return;
    }
    sAnyDepth[zone]--;
    if (start == 0) {
        return;
    }
    ProfZone& z = sZones[zone];
    const uint64_t elapsed = armGetSystemTick() - start;
    z.total.fetch_add(elapsed, std::memory_order_relaxed);
    z.calls.fetch_add(1, std::memory_order_relaxed);
    uint64_t longest = z.longest.load(std::memory_order_relaxed);
    while (elapsed > longest && !z.longest.compare_exchange_weak(longest, elapsed, std::memory_order_relaxed)) {
    }
}

extern "C" void Switch2Prof_SetScene(int scene) {
    if (sProfiling) {
        sScene.store(scene, std::memory_order_relaxed);
    }
}

extern "C" void Switch2Prof_Tick(void) {
    if (sProfiling) {
        sTicks++;
    }
}

extern "C" void Switch2Prof_Present(int targetFps) {
    if (!sProfiling) {
        return;
    }
    const uint64_t now = armGetSystemTick();
    const uint64_t freq = armGetSystemTickFreq();
    if (!sBootSent) {
        SendBootMarks(now);
    }
    if (sLastReport == 0) {
        sLastReport = now;
        sLastPresent = now;
        return;
    }
    const uint64_t interval = now - sLastPresent;
    sLastPresent = now;
    sPresents++;
    sLongestPresent = interval > sLongestPresent ? interval : sLongestPresent;
    // Late = took more than one and a half frames of the target rate.
    if (targetFps > 0 && interval * targetFps * 2 > freq * 3) {
        sLatePresents++;
    }
    if (now - sLastReport < freq) {
        return;
    }

    char line[1300];
    int pos = snprintf(line, sizeof(line),
                       "T %u us=%lu scene=%d target=%d frames=%u ticks=%u late=%u longest=%lu draws=%u tris=%u "
                       "uploads=%u uploadkb=%u shaders=%u fbbinds=%u resources=%u resbytes=%u",
                       sReportNumber++, TicksToMicros(now - sLastReport), sScene.load(std::memory_order_relaxed),
                       targetFps, sPresents, sTicks, sLatePresents, TicksToMicros(sLongestPresent),
                       sCounters[SW2C_DRAWS].exchange(0), sCounters[SW2C_TRIS].exchange(0),
                       sCounters[SW2C_UPLOADS].exchange(0), sCounters[SW2C_UPLOAD_KB].exchange(0),
                       sCounters[SW2C_SHADERS].exchange(0), sCounters[SW2C_FB_BINDS].exchange(0),
                       sCounters[SW2C_RESOURCES].exchange(0), sCounters[SW2C_RESOURCE_BYTES].exchange(0));
    for (int i = 0; i < SW2P_ZONE_COUNT && pos < static_cast<int>(sizeof(line)) - 64; i++) {
        ProfZone& z = sZones[i];
        const uint64_t total = z.total.exchange(0, std::memory_order_relaxed);
        const uint64_t longest = z.longest.exchange(0, std::memory_order_relaxed);
        const uint32_t calls = z.calls.exchange(0, std::memory_order_relaxed);
        pos += snprintf(line + pos, sizeof(line) - pos, " %s=%lu:%lu:%u", kZoneNames[i], TicksToMicros(total),
                        TicksToMicros(longest), calls);
    }
    Send(line, pos);
    sTicks = 0;
    sPresents = 0;
    sLatePresents = 0;
    sLongestPresent = 0;
    sLastReport = armGetSystemTick();
    sLastPresent = sLastReport;
}

// Answers one pending memory read. Called by the audio thread once per update.
extern "C" void Switch2Debug_Poll(void) {
    // Off for now: both launch crashes of the debug builds stopped around this
    // recvfrom. The one-way stream does not need it.
    static constexpr bool kReceiveEnabled = false;
    if (sSocket < 0 || !kReceiveEnabled) {
        return;
    }
    char request[64];
    sockaddr_in from = {};
    socklen_t fromSize = sizeof(from);
    ssize_t got = recvfrom(sSocket, request, sizeof(request) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromSize);
    if (got <= 0) {
        return;
    }
    request[got] = '\0';
    unsigned long address = 0;
    unsigned int size = 0;
    // R = offset in the ELF, A = absolute address.
    if (sscanf(request, "R %lx %u", &address, &size) == 2) {
        AnswerRead(reinterpret_cast<uintptr_t>(_start) + address, size);
    } else if (sscanf(request, "A %lx %u", &address, &size) == 2) {
        AnswerRead(address, size);
    }
}
#endif
