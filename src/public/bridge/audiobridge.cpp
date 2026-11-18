#include "public/bridge/audiobridge.h"
#include "Context.h"
#include "audio/Audio.h"
#ifdef __SWITCH__
#include "audio/SDLAudioPlayer.h"

// SwitchImpl.cpp: true once after the app got the focus back.
extern "C" bool LusSwitch_TakeAudioReopenRequest();
#endif

extern "C" {

int32_t AudioPlayerBuffered() {
    auto audio = Ship::Context::GetInstance()->GetAudio()->GetAudioPlayer();
    if (audio == nullptr) {
        return 0;
    }

    if (!audio->IsInitialized()) {
        return 0;
    }

    return audio->Buffered();
}

int32_t AudioPlayerGetDesiredBuffered() {
    auto audio = Ship::Context::GetInstance()->GetAudio()->GetAudioPlayer();
    if (audio == nullptr) {
        return 0;
    }

    if (!audio->IsInitialized()) {
        return 0;
    }

    return audio->GetDesiredBuffered();
}

AudioChannelsSetting GetAudioChannels() {
    auto audio = Ship::Context::GetInstance()->GetAudio()->GetAudioPlayer();

    if (audio == nullptr) {
        return audioStereo;
    }

    return audio->GetAudioChannels();
}

int32_t GetNumAudioChannels() {
    switch (GetAudioChannels()) {
        case audioSurround51:
            return 6;
        default:
            return 2;
    }
}

void AudioPlayerPlayFrame(const uint8_t* buf, size_t len) {
    auto audio = Ship::Context::GetInstance()->GetAudio()->GetAudioPlayer();
#ifdef __SWITCH__
    // Re-open the device here, on the audio thread, so that nothing plays
    // into it while it is closed.
    if (LusSwitch_TakeAudioReopenRequest()) {
        if (auto sdl = std::dynamic_pointer_cast<Ship::SDLAudioPlayer>(audio)) {
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
