#pragma once
#include "AudioPlayer.h"
#include <SDL2/SDL.h>
#include <vector>

namespace Ship {
class SDLAudioPlayer final : public AudioPlayer {
  public:
    SDLAudioPlayer(AudioSettings settings) : AudioPlayer(settings) {
    }
    ~SDLAudioPlayer();

    int Buffered();
    void Play(const uint8_t* buf, size_t len);

  protected:
    bool DoInit();
    void DoClose();

  private:
    SDL_AudioDeviceID mDevice = 0; ///< Handle to the opened SDL audio device.
    int32_t mNumChannels = 2;      ///< Number of output channels (2 for stereo, 6 for 5.1).
#ifdef __SWITCH__
    // The device runs at the audio renderer's rate; mResampler converts the
    // game's audio to it (see DoInit).
    static constexpr int kSwitchDeviceRate = 48000;
    int mDeviceRate = kSwitchDeviceRate;
    SDL_AudioStream* mResampler = nullptr;
    std::vector<uint8_t> mResampled;
    // Stall watchdog (see ReopenIfStalled): the queue size after the last
    // play, and when the device last took samples from the queue.
    uint32_t mQueuedAfterLastPlay = 0;
    uint32_t mLastDrainTicks = 0;
    uint32_t mLastReopenTicks = 0;
    bool ReopenIfStalled(uint32_t queued);
    // Counters for the log line that LogStats writes every 5 s.
    struct Stats {
        uint32_t startTicks = 0;
        uint64_t gameFrames = 0;
        uint64_t drainedBytes = 0;
        uint32_t plays = 0;
        uint32_t underruns = 0;
        uint32_t droppedPlays = 0;
        uint32_t minQueuedBytes = UINT32_MAX;
        uint32_t maxPlayGapMs = 0;
        uint32_t lastPlayTicks = 0;
    } mStats;
    void LogStats(uint32_t queuedBytes);
#endif
};
} // namespace Ship
