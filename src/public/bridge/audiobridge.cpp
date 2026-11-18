#include "public/bridge/audiobridge.h"
#include "Context.h"
#include "audio/Audio.h"
#ifdef __SWITCH__
#include "audio/SDLAudioPlayer.h"

// SwitchImpl.cpp: true once after the app got the focus back.
extern "C" bool LusSwitch_TakeAudioReopenRequest();
#endif

extern "C" {

int32_t AudioPlayerBuffered(void) {
    auto audio = LUS::Context::GetInstance()->GetAudio()->GetAudioPlayer();
    if (audio == nullptr) {
        return 0;
    }

    if (!audio->IsInitialized()) {
        return 0;
    }

    return audio->Buffered();
}

int32_t AudioPlayerGetDesiredBuffered(void) {
    auto audio = LUS::Context::GetInstance()->GetAudio()->GetAudioPlayer();
    if (audio == nullptr) {
        return 0;
    }

    if (!audio->IsInitialized()) {
        return 0;
    }

    return audio->GetDesiredBuffered();
}

void AudioPlayerPlayFrame(const uint8_t* buf, size_t len) {
    auto audio = LUS::Context::GetInstance()->GetAudio()->GetAudioPlayer();
#ifdef __SWITCH__
    // Re-open the device here, on the audio thread, so that nothing plays
    // into it while it is closed.
    if (LusSwitch_TakeAudioReopenRequest()) {
        if (auto sdl = std::dynamic_pointer_cast<LUS::SDLAudioPlayer>(audio)) {
            sdl->Reopen();
        }
    }
#endif
    if (audio == nullptr) {
        return;
    }

    if (!audio->IsInitialized()) {
        return;
    }

    audio->Play(buf, len);
}
}
