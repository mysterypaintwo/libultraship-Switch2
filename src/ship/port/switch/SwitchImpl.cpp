#ifdef __SWITCH__
#include "SwitchImpl.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <pthread.h>
#include <switch.h>
#include <SDL2/SDL.h>
#include "SwitchPerformanceProfiles.h"
#include "libultraship/bridge/consolevariablebridge.h"
#include <spdlog/spdlog.h>
#include "ship/Context.h"
#include "ship/audio/Audio.h"
#include "ship/utils/StringHelper.h"

#include <imgui_internal.h>

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
// Written by the applet hook, read by the audio thread (SDLAudioPlayer.cpp).
static std::atomic<bool> hasFocus = true;
static bool isShowingVirtualKeyboard = false;

// pl (shared system fonts) stays open while ImGui can read the fonts.
static bool sPlInitialized = false;

static SwkbdConfig keyboard;
static char kbBuffer[256] = { 0 };

HidsysUniquePadId uniquePadIds[8];

void DetectAppletMode();

// Thread stacks. libnx/libounce give a thread 128 KiB of stack when the
// program does not ask for a size, and std::thread never asks. Desktop threads
// get 512 KiB (macOS) to 8 MiB (Linux), and the game's audio thread, the
// resource loader and the decoders were written for that. A thread that runs
// past its stack here writes over other memory without faulting, and the
// program crashes later somewhere else. The link wraps pthread_create
// (-Wl,--wrap=pthread_create, src/CMakeLists.txt) to give such threads 1 MiB.
extern "C" int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg);

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) {
    constexpr size_t kDefaultStackSize = 1024 * 1024;
    size_t stackSize = 0;
    if (attr != nullptr && (pthread_attr_getstacksize(attr, &stackSize) != 0 || stackSize != 0)) {
        return __real_pthread_create(thread, attr, start, arg);
    }
    pthread_attr_t sized;
    if (attr != nullptr) {
        sized = *attr;
    } else {
        pthread_attr_init(&sized);
    }
    pthread_attr_setstacksize(&sized, kDefaultStackSize);
    int result = __real_pthread_create(thread, &sized, start, arg);
    if (attr == nullptr) {
        pthread_attr_destroy(&sized);
    }
    return result;
}

extern "C" bool LusSwitch_HasFocus() {
    return hasFocus;
}

static void on_applet_hook(AppletHookType hook, void* param);

namespace SwitchCrashReport {
void Install();
}

void Ship::Switch::Init(SwitchPhase phase) {
    switch (phase) {
        case PreInitPhase:
            // Write logs/crash-*.txt when the program crashes (SwitchCrashReport.cpp).
            SwitchCrashReport::Install();
            DetectAppletMode();
            socketInitializeDefault();
#ifdef DEBUG
            nxlinkStdio();
#endif
            break;
        case PostInitPhase:
            appletInitializeGamePlayRecording();
            appletSetGamePlayRecordingState(true);
            appletHook(&applet_hook_cookie, on_applet_hook, NULL);
            appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
#ifndef __SWITCH2__
            if (!hosversionBefore(8, 0, 0)) {
                clkrstInitialize();
            }
#endif
            hidsysInitialize();
            padConfigureInput(8, HidNpadStyleSet_NpadStandard);
            s32 total = 0; // unused
            hidsysGetUniquePadIds(uniquePadIds, 8, &total);
            break;
    }
}

void Ship::Switch::Exit() {
    if (sPlInitialized) {
        plExit();
        sPlInitialized = false;
    }
    socketExit();
#ifndef __SWITCH2__
    clkrstExit();
#endif
    appletSetGamePlayRecordingState(false);
}

void Ship::Switch::ImGuiSetupFont(ImFontAtlas* fonts) {
    if (!sPlInitialized) {
        plInitialize(PlServiceType_User);
    }
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

    // No plExit() here: the atlas keeps pointers into the shared font memory
    // (FontDataOwnedByAtlas = false), and plExit() unmaps it. The atlas is
    // built again whenever a font is added later (the port adds its own fonts
    // after this), and that read the unmapped memory. Exit() closes pl.
    sPlInitialized = true;
}

void Ship::Switch::CreateKeyboard() {
    Result rc = swkbdCreate(&keyboard, 0);
    if (R_FAILED(rc)) {
        SPDLOG_ERROR("Failed to create keyboard: {}", rc);
    } else {
        swkbdConfigMakePresetDefault(&keyboard);
    }
}

void Ship::Switch::ImGuiProcessEvent(bool wantsTextInput) {
    ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetActiveID());
    ImGuiIO& io = ImGui::GetIO();
    if (wantsTextInput) {
        if (!isShowingVirtualKeyboard) {
            // WantTextInput comes from the last frame: the widget that wanted
            // text may be gone, and then there is no state.
            if (state == nullptr) {
                return;
            }
            state->ClearText();
            isShowingVirtualKeyboard = true;
            memset(kbBuffer, 0, sizeof(kbBuffer));
            swkbdShow(&keyboard, kbBuffer, sizeof(kbBuffer));
            io.SetAppAcceptingEvents(true);
            io.AddInputCharactersUTF8(kbBuffer);
        }
    } else if (isShowingVirtualKeyboard) {
        isShowingVirtualKeyboard = false;
    }
}

bool Ship::Switch::IsRunning() {
    return isRunning;
}

void Ship::Switch::Quit() {
    SPDLOG_INFO("Quit requested by the program");
    LusSwitch_NoteQuit("Ship::Switch::Quit");
    isRunning = false;
}

void Ship::Switch::ApplyFrameRate(int fps, bool force) {
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

uint32_t Ship::Switch::GetRefreshRate() {
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

void Ship::Switch::GetDisplaySize(int* width, int* height) {
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

void Ship::Switch::ApplyOverclock(void) {
#ifdef __SWITCH2__
    // The clock table is for the Switch 1 CPU. Switch 2 keeps its own clocks.
    return;
#endif
    SwitchProfiles perfMode = (SwitchProfiles)CVarGetInteger(CVAR_SWITCH_PERF_MODE, (int)Ship::MAXIMUM);

    if (perfMode >= 0 && perfMode <= Ship::POWERSAVINGM3) {
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

char* Ship::Switch::GetControllerUUID(int controller) {
    HidsysUniquePadSerialNumber serial;
    hidsysGetUniquePadSerialNumber(uniquePadIds[controller], &serial);
    char* cuid = serial.serial_number;
    return SDL_strdup(strlen(cuid) >= 14 && cuid[0] == 'X' && cuid[1] == 'C'
                          ? cuid
                          : StringHelper::Sprintf("CID%d0000000000", controller).c_str());
}

static void on_applet_hook(AppletHookType hook, void* param) {
    AppletFocusState focus_state;

    /* Exit request */
    switch (hook) {
        case AppletHookType_OnExitRequest:
            SPDLOG_INFO("The system asked the program to quit (HOME menu close or power off)");
            LusSwitch_NoteQuit("system exit request");
            isRunning = false;
            break;

            /* Focus state*/
        case AppletHookType_OnFocusState:
            focus_state = appletGetFocusState();
            hasFocus = focus_state == AppletFocusState_InFocus;
            SPDLOG_INFO("Focus state: {}", (int)focus_state);

            if (!hasFocus) {
#ifndef __SWITCH2__
                if (hosversionBefore(8, 0, 0)) {
                    pcvSetClockRate(PcvModule_CpuBus, SWITCH_CPU_SPEEDS_VALUES[Ship::STOCK]);
                } else {
                    ClkrstSession session = { 0 };
                    clkrstOpenSession(&session, PcvModuleId_CpuBus, 3);
                    clkrstSetClockRate(&session, SWITCH_CPU_SPEEDS_VALUES[Ship::STOCK]);
                    clkrstCloseSession(&session);
                }
#endif
            } else {
                Ship::Switch::ApplyOverclock();
                // Audio that stopped after sleep (Shipwright issue #3317) is
                // fixed by SDLAudioPlayer's stall watchdog, which re-opens the
                // device only if it really stopped. Re-opening it at every
                // focus change (at boot too) crashed on Switch 2.
            }

            break;

        case AppletHookType_OnResume:
            Ship::Switch::ApplyOverclock();
            break;

            /* Performance mode */
        case AppletHookType_OnPerformanceMode:
            Ship::Switch::ApplyOverclock();
            break;
        default:
            break;
    }
}

void Ship::Switch::ShowErrorApplet(const char *format, ...) {
    ErrorSystemConfig errorConfig = {};

    // Error applet can display up to 2048 bytes
    char messageBuffer[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(messageBuffer, sizeof(messageBuffer), format, args);
    va_end(args);

    const Result rc = errorSystemCreate(&errorConfig, messageBuffer, nullptr);

    if (R_SUCCEEDED(rc)) {
        errorSystemSetResult(&errorConfig, MAKERESULT(400, 1)); // module id, error code
        errorSystemShow(&errorConfig);
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
    "This is the port all true gamers dock at",
    "Enhancements? Times Savers? Cheats? You want them? They're yours my friend!",
    "They say you gotta have the BIIIIG salad",
    "They say Louis stopped working on the imports so he can focus on the exports",
    "They say ZAPD is good software",
};

void DetectAppletMode() {
    AppletType at = appletGetAppletType();
    if (at == AppletType_Application || at == AppletType_SystemApplication)
        return;

    Ship::Switch::ShowErrorApplet("You've launched the Ship while in Applet mode.\n"
                                  "Please relaunch while in full-memory mode.\n"
                                  "Hold R when opening any game to enter HBMenu.\n\n"
                                  "%s.",
                                  RandomTexts[rand() % (sizeof(RandomTexts) / sizeof(RandomTexts[0]))]);
    exit(1);
}

void Ship::Switch::ShowSetupErrorAndExit(const char* title, const std::string& body) {
    consoleInit(NULL);
    printf("\x1b[2;2H%s\n\n", title);
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string::npos) {
            end = body.size();
        }
        printf(" %s\n", body.substr(start, end - start).c_str());
        start = end + 1;
    }
    printf("\n Press A to return to the Homebrew Menu.\n");

    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_A) {
            break;
        }
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    LusSwitch_NoteQuit("setup screen");
    exit(0);
}

void Ship::Switch::RequireGameFiles(const char* gameName, const char* folder, const std::vector<std::string>& required,
                                    const std::vector<std::string>& anyOf, const char* help) {
    std::string missing;
    for (const auto& file : required) {
        if (!std::filesystem::exists(file)) {
            missing += "  - " + file + "\n";
        }
    }
    if (!anyOf.empty()) {
        bool found = false;
        std::string names;
        for (const auto& file : anyOf) {
            found = found || std::filesystem::exists(file);
            names += (names.empty() ? "" : " or ") + file;
        }
        if (!found) {
            missing += "  - " + names + "\n";
        }
    }
    if (missing.empty()) {
        return;
    }
    ShowSetupErrorAndExit((std::string(gameName) + " is not set up yet").c_str(),
                          "These files are missing from " + std::string(folder) + ":\n\n" + missing + "\n" + help);
}

void Ship::Switch::ThrowMissingOTR(std::string otrPath) {
    Ship::Switch::ShowErrorApplet("You've launched the Ship without an OTR/O2R file.\n"
                                        "Please relaunch making sure %s exists.\n\n"
                                        "%s.",
                                        otrPath.c_str(), RandomTexts[rand() % (sizeof(RandomTexts) / sizeof(RandomTexts[0]))]);
    exit(2);
}
#endif
