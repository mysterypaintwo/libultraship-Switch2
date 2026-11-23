#include "ship/audio/SDLAudioPlayer.h"
#include <spdlog/spdlog.h>
#ifdef __SWITCH__
#include <algorithm>
#include "ship/port/switch/Switch2Debug.h"
#endif

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
#ifdef __SWITCH__
    if (mResampler != nullptr) {
        SDL_FreeAudioStream(mResampler);
        mResampler = nullptr;
    }
#endif
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
#ifdef __SWITCH__
    // Open the device at the audio renderer's own rate (SDL sets it up at
    // 48 kHz) and resample the game's audio here. At any other rate SDL gives
    // the renderer a voice at that rate and leaves the resampling to it, and
    // on Switch 2 that played at the wrong speed and pitch, with gaps.
    // Zelda64Recomp does the same: one 48 kHz device, resampled on our side.
    want.freq = kSwitchDeviceRate;
#endif
    want.format = AUDIO_S16SYS;
    want.channels = mNumChannels;
    want.samples = this->GetSampleLength();
    want.callback = NULL;

    mDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (mDevice == 0) {
        SPDLOG_ERROR("SDL_OpenAudio error: {}", SDL_GetError());
        return false;
    }

#ifdef __SWITCH__
    mDeviceRate = want.freq;
    mResampler = SDL_NewAudioStream(AUDIO_S16SYS, mNumChannels, this->GetSampleRate(), AUDIO_S16SYS, mNumChannels,
                                    mDeviceRate);
    if (mResampler == nullptr) {
        SPDLOG_ERROR("SDL_NewAudioStream error: {}", SDL_GetError());
        SDL_CloseAudioDevice(mDevice);
        mDevice = 0;
        return false;
    }
    mStats = {};
    mStats.startTicks = SDL_GetTicks();
    SPDLOG_INFO("SDL Audio initialized: {} channels, game {} Hz, device {} Hz (driver {}, have {} Hz, {} samples)",
                mNumChannels, this->GetSampleRate(), mDeviceRate, SDL_GetCurrentAudioDriver(), have.freq,
                have.samples);
#else
    SPDLOG_INFO("SDL Audio initialized: {} channels, {} Hz", mNumChannels, this->GetSampleRate());
#endif

    SDL_PauseAudioDevice(mDevice, 0);
    return true;
}

int SDLAudioPlayer::Buffered() {
    std::lock_guard<std::recursive_mutex> lock(DeviceMutex());
    if (mDevice == 0) {
        return 0;
    }
    int frames = SDL_GetQueuedAudioSize(mDevice) / (sizeof(int16_t) * mNumChannels);
#ifdef __SWITCH__
    // The game counts in its own frames (32 kHz); the queue holds 48 kHz ones.
    frames = static_cast<int>(static_cast<int64_t>(frames) * this->GetSampleRate() / mDeviceRate);
#endif
    return frames;
}

#ifdef __SWITCH__
// The console's audio renderer can stop taking samples, for example after
// sleep (https://github.com/HarbourMasters/Shipwright/issues/3317). The old fix
// closed and re-opened the device every time the app got the focus, at boot
// too; tearing down the audio renderer that often crashed on Switch 2. Now
// the device is re-opened only when it has really stopped: samples are
// queued, the app has the focus, and the queue has not gone down for 2 s.
// (Zelda64Recomp keeps one device open the whole time, the same idea.)
bool SDLAudioPlayer::ReopenIfStalled(uint32_t queued) {
    const uint32_t now = SDL_GetTicks();
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

// Every 5 s, logs how fast the game makes audio and how fast the device takes
// it. Both should be the same number of seconds of audio: if the device takes
// more than 48000 frames a second, the console plays too fast (high pitch) and
// the queue runs dry (gaps).
void SDLAudioPlayer::LogStats(uint32_t queuedBytes) {
    if (!Switch2Debug_Enabled()) {
        return;
    }
    const uint32_t now = SDL_GetTicks();
    const uint32_t elapsed = now - mStats.startTicks;
    if (elapsed < 5000) {
        return;
    }
    const uint32_t frameBytes = sizeof(int16_t) * mNumChannels;
    const double seconds = elapsed / 1000.0;
    SPDLOG_INFO("Audio: game made {:.0f} Hz (expect {}), device took {:.0f} Hz (expect {}), queued {} ms, "
                "lowest {} ms, longest gap between plays {} ms, {} plays, {} underruns, {} dropped",
                mStats.gameFrames / seconds, this->GetSampleRate(), mStats.drainedBytes / frameBytes / seconds,
                mDeviceRate, queuedBytes / frameBytes * 1000 / mDeviceRate,
                mStats.minQueuedBytes == UINT32_MAX ? 0 : mStats.minQueuedBytes / frameBytes * 1000 / mDeviceRate,
                mStats.maxPlayGapMs, mStats.plays, mStats.underruns,
                mStats.droppedPlays);
    const uint32_t lastPlay = mStats.lastPlayTicks;
    mStats = {};
    mStats.startTicks = now;
    mStats.lastPlayTicks = lastPlay;
}
#endif

void SDLAudioPlayer::DoPlay(const uint8_t* buf, size_t len) {
    std::lock_guard<std::recursive_mutex> lock(DeviceMutex());
    if (mDevice == 0) {
        return;
    }
#ifdef __SWITCH__
    uint32_t queued = SDL_GetQueuedAudioSize(mDevice);
    if (queued < mQueuedAfterLastPlay) {
        mStats.drainedBytes += mQueuedAfterLastPlay - queued;
    }
    if (queued == 0 && mStats.plays + mStats.underruns > 0) {
        mStats.underruns++;
    }
    mStats.minQueuedBytes = std::min(mStats.minQueuedBytes, queued);
    const uint32_t playTicks = SDL_GetTicks();
    if (mStats.lastPlayTicks != 0) {
        mStats.maxPlayGapMs = std::max(mStats.maxPlayGapMs, playTicks - mStats.lastPlayTicks);
    }
    mStats.lastPlayTicks = playTicks;
    mStats.plays++;
    mStats.gameFrames += len / (sizeof(int16_t) * mNumChannels);
    if (ReopenIfStalled(queued) || mDevice == 0) {
        return;
    }
    LogStats(queued);

    if (Buffered() >= 6000) {
        // Don't fill the audio buffer too much in case this happens
        mStats.droppedPlays++;
        mQueuedAfterLastPlay = queued;
        return;
    }
    if (SDL_AudioStreamPut(mResampler, buf, static_cast<int>(len)) != 0) {
        SPDLOG_ERROR("SDL_AudioStreamPut error: {}", SDL_GetError());
        return;
    }
    int available = SDL_AudioStreamAvailable(mResampler);
    if (available > 0) {
        if (mResampled.size() < static_cast<size_t>(available)) {
            mResampled.resize(available);
        }
        int got = SDL_AudioStreamGet(mResampler, mResampled.data(), available);
        if (got > 0) {
            SDL_QueueAudio(mDevice, mResampled.data(), got);
        }
    }
    mQueuedAfterLastPlay = SDL_GetQueuedAudioSize(mDevice);
#else
    if (Buffered() < 6000) {
        // Don't fill the audio buffer too much in case this happens
        SDL_QueueAudio(mDevice, buf, len);
    }
#endif
}

} // namespace Ship
