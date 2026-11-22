#include "ship/audio/SDLAudioPlayer.h"
#include <spdlog/spdlog.h>

#ifdef __SWITCH__
// SwitchImpl.cpp: false while the HOME menu or another applet has the focus.
extern "C" bool LusSwitch_HasFocus();
#endif

namespace Ship {

SDLAudioPlayer::~SDLAudioPlayer() {
    SPDLOG_TRACE("destruct SDL audio player");
    DoClose();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void SDLAudioPlayer::DoClose() {
    if (mDevice != 0) {
        // Pause playback first
        SDL_PauseAudioDevice(mDevice, 1);
        // Clear any queued audio to prevent glitches when reopening
        SDL_ClearQueuedAudio(mDevice);
        SDL_CloseAudioDevice(mDevice);
        mDevice = 0;
    }
}

bool SDLAudioPlayer::DoInit() {
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        SPDLOG_ERROR("SDL init error: {}", SDL_GetError());
        return false;
    }

    // Always open with the correct number of output channels
    mNumChannels = this->GetNumOutputChannels();

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = this->GetSampleRate();
    want.format = AUDIO_S16SYS;
    want.channels = mNumChannels;
    want.samples = this->GetSampleLength();
    want.callback = NULL;

    mDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (mDevice == 0) {
        SPDLOG_ERROR("SDL_OpenAudio error: {}", SDL_GetError());
        return false;
    }

    SPDLOG_INFO("SDL Audio initialized: {} channels, {} Hz", mNumChannels, this->GetSampleRate());

    SDL_PauseAudioDevice(mDevice, 0);
    return true;
}

int SDLAudioPlayer::Buffered() {
    std::lock_guard<std::recursive_mutex> lock(DeviceMutex());
    if (mDevice == 0) {
        return 0;
    }
    return SDL_GetQueuedAudioSize(mDevice) / (sizeof(int16_t) * mNumChannels);
}

#ifdef __SWITCH__
// The console's audio renderer can stop taking samples, for example after
// sleep (https://github.com/HarbourMasters/Shipwright/issues/3317). The old fix
// closed and re-opened the device every time the app got the focus, at boot
// too; tearing down the audio renderer that often crashed on Switch 2. Now
// the device is re-opened only when it has really stopped: samples are
// queued, the app has the focus, and the queue has not gone down for 2 s.
// (Zelda64Recomp keeps one device open the whole time, the same idea.)
bool SDLAudioPlayer::ReopenIfStalled() {
    const uint32_t now = SDL_GetTicks();
    const uint32_t queued = SDL_GetQueuedAudioSize(mDevice);
    if (queued < mQueuedAfterLastPlay || queued == 0 || !LusSwitch_HasFocus() || mLastDrainTicks == 0) {
        mLastDrainTicks = now;
        return false;
    }
    if (now - mLastDrainTicks < 2000 || now - mLastReopenTicks < 10000) {
        return false;
    }
    SPDLOG_WARN("SDL audio device stopped playing ({} bytes queued for {} ms): re-opening it", queued,
                now - mLastDrainTicks);
    mLastReopenTicks = now;
    mLastDrainTicks = now;
    mQueuedAfterLastPlay = 0;
    DoClose();
    if (!DoInit()) {
        SPDLOG_ERROR("Could not re-open the SDL audio device");
        mDevice = 0;
    }
    return true;
}
#endif

void SDLAudioPlayer::DoPlay(const uint8_t* buf, size_t len) {
    std::lock_guard<std::recursive_mutex> lock(DeviceMutex());
    if (mDevice == 0) {
        return;
    }
#ifdef __SWITCH__
    ReopenIfStalled();
    if (mDevice == 0) {
        return;
    }
#endif
    if (Buffered() < 6000) {
        // Don't fill the audio buffer too much in case this happens
        SDL_QueueAudio(mDevice, buf, len);
    }
#ifdef __SWITCH__
    mQueuedAfterLastPlay = SDL_GetQueuedAudioSize(mDevice);
#endif
}

} // namespace Ship
