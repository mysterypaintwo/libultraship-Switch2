#include "ship/audio/Audio.h"

#ifdef __APPLE__
#include "ship/audio/CoreAudioAudioPlayer.h"
#endif

#include "ship/Context.h"
#include "ship/config/Config.h"
#include "ship/controller/controldeck/ControlDeck.h"

namespace Ship {

Audio::~Audio() {
    SPDLOG_TRACE("destruct audio");
}

void Audio::InitAudioPlayer() {
    // Close the old device before opening the new one. The audio thread may
    // still hold the old player for a moment; Close() waits for its Play().
    std::shared_ptr<AudioPlayer> oldPlayer;
    {
        std::lock_guard<std::mutex> lock(mAudioPlayerMutex);
        oldPlayer = std::move(mAudioPlayer);
    }
    if (oldPlayer != nullptr) {
        oldPlayer->Close();
        oldPlayer = nullptr;
    }

    std::shared_ptr<AudioPlayer> player;
    switch (GetCurrentAudioBackend()) {
#ifdef _WIN32
        case AudioBackend::WASAPI:
            player = std::make_shared<WasapiAudioPlayer>(this->mAudioSettings);
            break;
#endif
#ifdef __APPLE__
        case AudioBackend::COREAUDIO:
            player = std::make_shared<CoreAudioAudioPlayer>(this->mAudioSettings);
            break;
#endif
        case AudioBackend::SDL:
            player = std::make_shared<SDLAudioPlayer>(this->mAudioSettings);
            break;
        default:
            player = std::make_shared<NullAudioPlayer>(this->mAudioSettings);
            break;
    }

    if (player && !player->Init()) {
        // Failed to initialize system audio player.
        // Fallback to Null if the native system player does not work.
        if (GetCurrentAudioBackend() != AudioBackend::NUL) {
            SetCurrentAudioBackend(AudioBackend::NUL);
            return;
        }
    }

    std::lock_guard<std::mutex> lock(mAudioPlayerMutex);
    mAudioPlayer = std::move(player);
}

void Audio::Init() {
    mConfig = Context::GetRawInstance()->GetConfig();

    mAvailableAudioBackends = std::make_shared<std::vector<AudioBackend>>();
#ifdef _WIN32
    mAvailableAudioBackends->push_back(AudioBackend::WASAPI);
#endif
#ifdef __APPLE__
    mAvailableAudioBackends->push_back(AudioBackend::COREAUDIO);
#endif
    mAvailableAudioBackends->push_back(AudioBackend::SDL);
    mAvailableAudioBackends->push_back(AudioBackend::NUL);

    SetCurrentAudioBackend(GetSavedAudioBackend());
    SetAudioChannels(GetSavedAudioChannelsSetting());
}

std::shared_ptr<AudioPlayer> Audio::GetAudioPlayer() {
    std::lock_guard<std::mutex> lock(mAudioPlayerMutex);
    return mAudioPlayer;
}

AudioBackend Audio::GetCurrentAudioBackend() {
    return mAudioBackend;
}

AudioBackend Audio::GetSavedAudioBackend() {
    std::string backendName = mConfig->GetString("Window.AudioBackend");
    if (backendName == "wasapi") {
        return AudioBackend::WASAPI;
    }

    // Migrate pulse player in config to sdl
    if (backendName == "pulse") {
        mConfig->SetString("Window.AudioBackend", "sdl");
        mConfig->Save();
        return AudioBackend::SDL;
    }

    if (backendName == "coreaudio") {
        return AudioBackend::COREAUDIO;
    }

    if (backendName == "sdl") {
        return AudioBackend::SDL;
    }

    if (backendName == "null") {
        return AudioBackend::NUL;
    }

    SPDLOG_TRACE("Could not find AudioBackend matching value from config file ({}). Returning default AudioBackend.",
                 backendName);
#ifdef _WIN32
    return AudioBackend::WASAPI;
#endif

#ifdef __APPLE__
    return AudioBackend::COREAUDIO;
#endif

    return AudioBackend::SDL;
}

void Audio::SetCurrentAudioBackend(AudioBackend backend) {
    mAudioBackend = backend;

    switch (backend) {
        case AudioBackend::WASAPI:
            mConfig->SetString("Window.AudioBackend", "wasapi");
            break;
        case AudioBackend::COREAUDIO:
            mConfig->SetString("Window.AudioBackend", "coreaudio");
            break;
        case AudioBackend::SDL:
            mConfig->SetString("Window.AudioBackend", "sdl");
            break;
        case AudioBackend::NUL:
            mConfig->SetString("Window.AudioBackend", "null");
            break;
        default:
            mConfig->SetString("Window.AudioBackend", "");
    }
    mConfig->Save();

    InitAudioPlayer();
}

std::shared_ptr<std::vector<AudioBackend>> Audio::GetAvailableAudioBackends() {
    return mAvailableAudioBackends;
}

void Audio::SetAudioChannels(AudioChannelsSetting channels) {
    if (mAudioSettings.ChannelSetting != channels) {
        mAudioSettings.ChannelSetting = channels;
        // Reinitialize the existing audio player with the new channel configuration
        if (auto player = GetAudioPlayer()) {
            player->SetAudioChannels(channels);
        }
    }
}

AudioChannelsSetting Audio::GetAudioChannels() const {
    return mAudioSettings.ChannelSetting;
}

AudioChannelsSetting Audio::GetSavedAudioChannelsSetting() {
    int32_t channelsSetting =
        mConfig->GetInt("CVars." CVAR_AUDIO_CHANNELS_SETTING, static_cast<int32_t>(AudioChannelsSetting::audioMax));
    switch (channelsSetting) {
        case AudioChannelsSetting::audioMatrix51:
            return AudioChannelsSetting::audioMatrix51;
        case AudioChannelsSetting::audioRaw51:
            return AudioChannelsSetting::audioRaw51;
        case AudioChannelsSetting::audioStereo:
        case AudioChannelsSetting::audioMax:
        default:
            return AudioChannelsSetting::audioStereo;
    }
}

} // namespace Ship
