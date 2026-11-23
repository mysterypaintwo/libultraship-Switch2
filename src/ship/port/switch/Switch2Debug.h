#pragma once

#ifndef __cplusplus
#include <stdbool.h>
#endif

// Timing zones of the once-per-second profile (P lines are off unless
// debug-verbose.txt is also there). Zones can nest; each one holds its own time
// including anything inside it. Keep in step with kZoneNames in the .cpp.
enum Switch2ProfZone {
    SW2P_GAME,        // whole game state update, including the two below
    SW2P_PLAY_UPDATE, // gameplay logic
    SW2P_PLAY_DRAW,   // building the display list
    SW2P_AUDIO_WAIT,  // main thread waiting for the audio thread
    SW2P_EVENTS,      // window and input events
    SW2P_GUI_START,   // ImGui frame setup and menus
    SW2P_RDP,         // running the display list, including draw/upload/shader/fb
    SW2P_DRAW,        // glDraw and vertex upload
    SW2P_UPLOAD,      // texture upload
    SW2P_SHADER,      // compiling a new shader
    SW2P_FB_COPY,     // framebuffer copies
    SW2P_FB_READ,     // reading pixels or depth back to the CPU
    SW2P_GUI_END,     // drawing the game and ImGui to the screen
    SW2P_GL_FLUSH,    // glFlush
    SW2P_PACE,        // frame limiter sleep
    SW2P_SWAP,        // present and vsync wait
    SW2P_AUDIO,       // audio thread work
    SW2P_RESOURCE,    // loading an asset that was not cached, on any thread
    SW2P_FILE_READ,   // reading and unpacking a file from the archive, on any thread
    SW2P_TEX_IMPORT,  // decoding and uploading a texture
    SW2P_ZONE_COUNT
};

enum Switch2ProfCounter { SW2C_DRAWS, SW2C_TRIS, SW2C_UPLOADS, SW2C_UPLOAD_KB, SW2C_SHADERS, SW2C_FB_BINDS, SW2C_RESOURCES, SW2C_RESOURCE_BYTES, SW2C_COUNT };

// Network debugging for console builds. Off unless debug-host.txt (the IP of a
// computer running switch2/debug_listener.py) is next to the NRO.
#ifdef __SWITCH__
#ifdef __cplusplus
extern "C" {
#endif
void Switch2Debug_Start(void);
// True when debug-host.txt turned the stream on.
bool Switch2Debug_Enabled(void);
void Switch2Debug_Log(const char* line);
// thread: 0 = audio, 1 = main. name must be a string literal.
void Switch2Debug_Phase(int thread, const char* name);
void Switch2Debug_Poll(void);
// True when debug-verbose.txt also asks for phase markers and the audio dump.
// They send many packets a frame, so they stay off while profiling.
bool Switch2Debug_Verbose(void);
// Remembers how long boot took up to this step (name must be a string literal).
// Sent in one packet with the first presented frame.
void Switch2Debug_BootMark(const char* name);
unsigned long Switch2Debug_Micros(void);
void Switch2Prof_Begin(int zone);
void Switch2Prof_End(int zone);
void Switch2Prof_Count(int counter, unsigned int amount);
// For zones used by several threads. Nested calls on one thread count once.
unsigned long long Switch2Prof_EnterAny(int zone);
void Switch2Prof_LeaveAny(int zone, unsigned long long start);
void Switch2Prof_SetScene(int scene);
// One game logic tick started.
void Switch2Prof_Tick(void);
// One frame was presented. Sends the summary once a second.
void Switch2Prof_Present(int targetFps);
#ifdef __cplusplus
}
struct Switch2ProfScope {
    int zone;
    explicit Switch2ProfScope(int z) : zone(z) {
        Switch2Prof_Begin(zone);
    }
    ~Switch2ProfScope() {
        Switch2Prof_End(zone);
    }
    Switch2ProfScope(const Switch2ProfScope&) = delete;
    Switch2ProfScope& operator=(const Switch2ProfScope&) = delete;
};
#define SWITCH2_PROF(zone) Switch2ProfScope switch2ProfScope_##zone(zone)
struct Switch2ProfAnyScope {
    int zone;
    unsigned long long start;
    explicit Switch2ProfAnyScope(int z) : zone(z), start(Switch2Prof_EnterAny(z)) {
    }
    ~Switch2ProfAnyScope() {
        Switch2Prof_LeaveAny(zone, start);
    }
    Switch2ProfAnyScope(const Switch2ProfAnyScope&) = delete;
    Switch2ProfAnyScope& operator=(const Switch2ProfAnyScope&) = delete;
};
#define SWITCH2_PROF_ANY(zone) Switch2ProfAnyScope switch2ProfScope_##zone(zone)
#endif
#else
#define Switch2Debug_Start()
#define Switch2Debug_Enabled() 0
#define Switch2Debug_Log(line)
#define Switch2Debug_Phase(thread, name)
#define Switch2Debug_Poll()
#define Switch2Debug_Verbose() 0
#define Switch2Debug_BootMark(name)
#define Switch2Prof_Begin(zone)
#define Switch2Prof_End(zone)
#define Switch2Prof_Count(counter, amount)
#define Switch2Prof_SetScene(scene)
#define Switch2Prof_Tick()
#define Switch2Prof_Present(targetFps)
#define SWITCH2_PROF(zone)
#define SWITCH2_PROF_ANY(zone)
#endif
