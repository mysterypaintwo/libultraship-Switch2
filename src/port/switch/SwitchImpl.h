#pragma once

#include <cstdint>
#include <string>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui.h>
#include "SwitchPerformanceProfiles.h"

namespace Ship {
enum SwitchProfiles { MAXIMUM, HIGH, BOOST, STOCK, POWERSAVINGM1, POWERSAVINGM2, POWERSAVINGM3 };

enum SwitchPhase { PreInitPhase, PostInitPhase };

class Switch {
  public:
    static void Init(SwitchPhase phase);
    static void Exit();
    static void ImGuiSetupFont(ImFontAtlas* fonts);
    static void ImGuiProcessEvent(bool wantsTextInput);
    static bool IsRunning();
    // Asks to stop the main loop, as the HOME menu's close does. The app then
    // shuts down and returns to the homebrew menu.
    static void Quit();
    // Switch 2: sets the present rate for the target FPS. Call it again after
    // every swap interval change, which resets the rate to 60 Hz.
    static void ApplyFrameRate(int fps, bool force = false);
    // The display's highest refresh rate (120 on the Switch 2 screen).
    static uint32_t GetRefreshRate();
    static void GetDisplaySize(int* width, int* height);
    static void ApplyOverclock();
    static void ThrowMissingOTR(std::string OTRPath);
    static void PrintErrorMessageToScreen(const char* str, ...);
};
}; // namespace Ship