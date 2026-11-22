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
    if (sSocket < 0 || sPhase[thread].exchange(name) == name) {
        return;
    }
    char line[96];
    uint32_t count = sPhaseCount[thread].fetch_add(1);
    Send(line, snprintf(line, sizeof(line), "P %s %s %u", thread == 0 ? "audio" : "main", name, count));
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
