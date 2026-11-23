#include "ship/controller/controldevice/controller/mapping/sdl/SDLButtonToAnyMapping.h"

#include "ship/utils/StringHelper.h"
#include "ship/window/gui/IconsFontAwesome4.h"

namespace Ship {
SDLButtonToAnyMapping::SDLButtonToAnyMapping(int32_t sdlControllerButton)
    : ControllerInputMapping(PhysicalDeviceType::SDLGamepad) {
    mControllerButton = static_cast<SDL_GameControllerButton>(sdlControllerButton);
}

SDLButtonToAnyMapping::~SDLButtonToAnyMapping() {
}

std::string SDLButtonToAnyMapping::GetPhysicalInputName() {
#ifdef __SWITCH__
    // SDL's names are positional (Xbox layout): SDL A is the bottom button, which
    // is B on a Nintendo controller. Show what is printed on the controller.
    switch (mControllerButton) {
        case SDL_CONTROLLER_BUTTON_A:
            return "B";
        case SDL_CONTROLLER_BUTTON_B:
            return "A";
        case SDL_CONTROLLER_BUTTON_X:
            return "Y";
        case SDL_CONTROLLER_BUTTON_Y:
            return "X";
        case SDL_CONTROLLER_BUTTON_BACK:
            return "-";
        case SDL_CONTROLLER_BUTTON_GUIDE:
            return "Home";
        case SDL_CONTROLLER_BUTTON_START:
            return "+";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            return "L Stick";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            return "R Stick";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return "L";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return "R";
        case SDL_CONTROLLER_BUTTON_MISC1:
            return "Capture";
        default:
            break;
    }
#endif
    switch (mControllerButton) {
        case SDL_CONTROLLER_BUTTON_A:
            return "A";
        case SDL_CONTROLLER_BUTTON_B:
            return "B";
        case SDL_CONTROLLER_BUTTON_X:
            return "X";
        case SDL_CONTROLLER_BUTTON_Y:
            return "Y";
        case SDL_CONTROLLER_BUTTON_BACK:
            return "View";
        case SDL_CONTROLLER_BUTTON_GUIDE:
            return "Xbox";
        case SDL_CONTROLLER_BUTTON_START:
            return StringHelper::Sprintf("%s", ICON_FA_BARS);
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            return "LS";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            return "RS";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return "LB";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return "RB";
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return StringHelper::Sprintf("D-Pad %s", ICON_FA_ARROW_UP);
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return StringHelper::Sprintf("D-Pad %s", ICON_FA_ARROW_DOWN);
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return StringHelper::Sprintf("D-Pad %s", ICON_FA_ARROW_LEFT);
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return StringHelper::Sprintf("D-Pad %s", ICON_FA_ARROW_RIGHT);
        case SDL_CONTROLLER_BUTTON_MISC1:
            return "Share"; /* Xbox Series X share button, PS5 microphone button, Nintendo Switch Pro capture button,
                               Amazon Luna microphone button */
        case SDL_CONTROLLER_BUTTON_PADDLE1:
            return "P1";
        case SDL_CONTROLLER_BUTTON_PADDLE2:
            return "P2";
        case SDL_CONTROLLER_BUTTON_PADDLE3:
            return "P3";
        case SDL_CONTROLLER_BUTTON_PADDLE4:
            return "P4";
        default:
            break;
    }

    return GetGenericButtonName();
}

std::string SDLButtonToAnyMapping::GetGenericButtonName() {
    return StringHelper::Sprintf("B%d", mControllerButton);
}

std::string SDLButtonToAnyMapping::GetPhysicalDeviceName() {
    return "SDL Gamepad";
}
} // namespace Ship
