#pragma once

#ifndef __cplusplus
#include <stdbool.h>
#endif

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
#ifdef __cplusplus
}
#endif
#else
#define Switch2Debug_Start()
#define Switch2Debug_Enabled() 0
#define Switch2Debug_Log(line)
#define Switch2Debug_Phase(thread, name)
#define Switch2Debug_Poll()
#endif
