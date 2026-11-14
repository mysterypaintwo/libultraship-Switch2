#ifdef __SWITCH__
#include "SwitchImpl.h"
#include <switch.h>
#include <SDL2/SDL.h>
#include "SwitchPerformanceProfiles.h"
#include "public/bridge/consolevariablebridge.h"
#include <spdlog/spdlog.h>
#include "Context.h"
#include "audio/Audio.h"

#include <ImGui/imgui_internal.h>

// The Switch 2 draws the UI at 1080p in handheld mode too, where the Switch 1
// drew it at 720p. A 1.5x font keeps the menus the same size on screen.
#ifdef __SWITCH2__
#define SWITCH_FONT_SIZE 36.0f
#else
#define SWITCH_FONT_SIZE 24.0f
#endif

#define DOCKED_MODE 1
#define HANDHELD_MODE 0

static AppletHookCookie applet_hook_cookie;
static bool isRunning = true;
static bool hasFocus = true;
static bool isShowingVirtualKeyboard = true;

void DetectAppletMode();

static void on_applet_hook(AppletHookType hook, void* param);

void LUS::Switch::Init(SwitchPhase phase) {
    switch (phase) {
        case PreInitPhase:
            DetectAppletMode();
            break;
        case PostInitPhase:
            appletInitializeGamePlayRecording();
#ifdef DEBUG
            socketInitializeDefault();
            nxlinkStdio();
#endif
            appletSetGamePlayRecordingState(true);
            appletHook(&applet_hook_cookie, on_applet_hook, NULL);
            appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
#ifndef __SWITCH2__
            if (!hosversionBefore(8, 0, 0)) {
                clkrstInitialize();
            }
#endif
            break;
    }
}

void LUS::Switch::Exit() {
#ifdef DEBUG
    socketExit();
#endif
#ifndef __SWITCH2__
    clkrstExit();
#endif
    appletSetGamePlayRecordingState(false);
}

void LUS::Switch::ImGuiSetupFont(ImFontAtlas* fonts) {
    plInitialize(PlServiceType_User);
    static PlFontData stdFontData, extFontData;

    PlFontData fonts_std;
    PlFontData fonts_ext;

    plGetSharedFontByType(&fonts_std, PlSharedFontType_Standard);
    plGetSharedFontByType(&fonts_ext, PlSharedFontType_NintendoExt);

    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;

    strcpy(config.Name, "Nintendo Standard");
    fonts->AddFontFromMemoryTTF(fonts_std.address, fonts_std.size, SWITCH_FONT_SIZE, &config, fonts->GetGlyphRangesCyrillic());

    strcpy(config.Name, "Nintendo Ext");
    static const ImWchar ranges[] = {
        0xE000, 0xE06B, 0xE070, 0xE07E, 0xE080, 0xE099, 0xE0A0, 0xE0BA, 0xE0C0, 0xE0D6, 0xE0E0, 0xE0F5, 0xE100,
        0xE105, 0xE110, 0xE116, 0xE121, 0xE12C, 0xE130, 0xE13C, 0xE140, 0xE14D, 0xE150, 0xE153, 0,
    };

    fonts->AddFontFromMemoryTTF(fonts_ext.address, fonts_ext.size, SWITCH_FONT_SIZE, &config, ranges);
    fonts->Build();

    plExit();
}

void LUS::Switch::ImGuiProcessEvent(bool wantsTextInput) {
    ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetActiveID());

    if (wantsTextInput) {
        if (!isShowingVirtualKeyboard) {
            state->ClearText();

            isShowingVirtualKeyboard = true;
            SDL_StartTextInput();
        }
    } else {
        if (isShowingVirtualKeyboard) {
            isShowingVirtualKeyboard = false;
            SDL_StopTextInput();
        }
    }
}

bool LUS::Switch::IsRunning() {
    return isRunning;
}

void LUS::Switch::Quit() {
    isRunning = false;
}

void LUS::Switch::ApplyFrameRate(int fps, bool force) {
#ifdef __SWITCH2__
    static int appliedFps = -1;
    if (fps == appliedFps && !force) {
        return;
    }
    appliedFps = fps;
    // 0 presents every refresh. Without this call the window presents at the
    // swap interval / 60 s, so it never goes above 60 FPS, even at 120 Hz.
    u32 at120 = (fps <= 0 || fps >= 120) ? 0 : fps;
    u32 at60 = (fps <= 0 || fps >= 60) ? 0 : fps;
    Result rc = nwindowSetFrameRates(nwindowGetDefault(), at120, at60, true);
    if (R_FAILED(rc)) {
        SPDLOG_WARN("nwindowSetFrameRates({}, {}) failed: 0x{:x}", at120, at60, rc);
    }
#endif
}

uint32_t LUS::Switch::GetRefreshRate() {
#ifdef __SWITCH2__
    // The FPS cap reads this every frame, so query the display once. On a
    // 60 Hz TV, vsync still holds the game to 60 FPS.
    static uint32_t sRefreshRate = 0;
    if (sRefreshRate != 0) {
        return sRefreshRate;
    }
    Vi2Display* display = nullptr;
    Vi2DisplayModeCapability caps = {};
    if (R_SUCCEEDED(nwindowGetVi2Display(nwindowGetDefault(), &display)) && display != nullptr &&
        R_SUCCEEDED(vi2QueryDisplayModeCapability(display, &caps))) {
        // Bit 1 is 120 Hz, bit 0 is 60 Hz.
        sRefreshRate = ((caps.refresh_rates | caps.refresh_rates_secondary) & BIT(1)) ? 120 : 60;
        return sRefreshRate;
    }
    // No display yet (before the window opens): ask again next time.
    return 60;
#endif
    return 60;
}

void LUS::Switch::GetDisplaySize(int* width, int* height) {
#ifdef __SWITCH2__
    // The Switch 2 handheld screen is 1080p, and 1080p is a safe docked size.
    *width = 1920;
    *height = 1080;
    return;
#endif
    switch (appletGetOperationMode()) {
        case DOCKED_MODE:
            *width = 1920;
            *height = 1080;
            break;
        case HANDHELD_MODE:
            *width = 1280;
            *height = 720;
            break;
    }
}

void LUS::Switch::ApplyOverclock(void) {
#ifdef __SWITCH2__
    // The clock table is for the Switch 1 CPU. Switch 2 keeps its own clocks.
    return;
#endif
    SwitchProfiles perfMode = (SwitchProfiles)CVarGetInteger("gSwitchPerfMode", (int)LUS::MAXIMUM);

    if (perfMode >= 0 && perfMode <= LUS::POWERSAVINGM3) {
        if (hosversionBefore(8, 0, 0)) {
            pcvSetClockRate(PcvModule_CpuBus, SWITCH_CPU_SPEEDS_VALUES[perfMode]);
        } else {
            ClkrstSession session = { 0 };
            clkrstOpenSession(&session, PcvModuleId_CpuBus, 3);
            clkrstSetClockRate(&session, SWITCH_CPU_SPEEDS_VALUES[perfMode]);
            clkrstCloseSession(&session);
        }
    }
}

void LUS::Switch::PrintErrorMessageToScreen(const char* str, ...) {
    consoleInit(NULL);
    srand(time(0));

    va_list args;
    va_start(args, str);
    vprintf(str, args);
    va_end(args);

    while (appletMainLoop()) {
        consoleUpdate(NULL);
    }

    consoleExit(NULL);
}

static void on_applet_hook(AppletHookType hook, void* param) {
    AppletFocusState focus_state;

    /* Exit request */
    switch (hook) {
        case AppletHookType_OnExitRequest:
            isRunning = false;
            break;

            /* Focus state*/
        case AppletHookType_OnFocusState:
            focus_state = appletGetFocusState();
            hasFocus = focus_state == AppletFocusState_InFocus;

            if (!hasFocus) {
#ifndef __SWITCH2__
                if (hosversionBefore(8, 0, 0)) {
                    pcvSetClockRate(PcvModule_CpuBus, SWITCH_CPU_SPEEDS_VALUES[LUS::STOCK]);
                } else {
                    ClkrstSession session = { 0 };
                    clkrstOpenSession(&session, PcvModuleId_CpuBus, 3);
                    clkrstSetClockRate(&session, SWITCH_CPU_SPEEDS_VALUES[LUS::STOCK]);
                    clkrstCloseSession(&session);
                }
#endif
            } else {
                LUS::Switch::ApplyOverclock();
                // reinitialize audio subsystem to fix audio problems after resuming from sleep
                // see https://github.com/HarbourMasters/Shipwright/issues/3317
                SPDLOG_INFO("restarting SDL audio system to work around audio problems on resume");
                LUS::Context::GetInstance()->GetAudio()->SetAudioBackend(LUS::AudioBackend::SDL);
            }

            break;

            /* Performance mode */
        case AppletHookType_OnPerformanceMode:
            LUS::Switch::ApplyOverclock();
            break;
        default:
            break;
    }
}

const char* RandomTexts[] = {
    "Psst, don't forget to blame Melon",
    "Potsanity when?",
    "Why are you acting so random?",
    "Enough! My ship sails in the morning",
    "Do you want 2 or 7 of those?",
    "Lamp oil, rope, bombs you want it, it's yours my friend as long as you have enough rupees",
    "You can build it yourself",
    "Descargar para android",
    "Made with <3 by the Harbour Masters!",
    "They say that Kenix is not a developer",
    "Squadala we're off",
    "They say one once saw an equals not get set equals",
    "This is the port all true gamers dock at"
    "Enhancements? Times Savers? Cheats? You want them? They're yours my friend!",
    "They say you gotta have the BIIIIG salad",
    "They say Louis stopped working on the imports so he can focus on the exports",
    "They say ZAPD is good software",
};

void DetectAppletMode() {
    AppletType at = appletGetAppletType();
    if (at == AppletType_Application || at == AppletType_SystemApplication)
        return;

    LUS::Switch::PrintErrorMessageToScreen("\x1b[2;2HYou've launched the Ship while in Applet mode."
                                           "\x1b[4;2HPlease relaunch while in full-memory mode."
                                           "\x1b[5;2HHold R when opening any game to enter HBMenu."
                                           "\x1b[44;2H%s.",
                                           RandomTexts[rand() % 25]);
}

void LUS::Switch::ThrowMissingOTR(std::string OTRPath) {
    LUS::Switch::PrintErrorMessageToScreen("\x1b[2;2HYou've launched the Ship without the OTR file."
                                           "\x1b[4;2HPlease relaunch making sure %s exists."
                                           "\x1b[44;2H%s.",
                                           OTRPath.c_str(), RandomTexts[rand() % 25]);
}
#endif
