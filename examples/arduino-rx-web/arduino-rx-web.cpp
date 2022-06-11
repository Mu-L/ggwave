#include "ggwave-common.h"

#include "ggwave/ggwave.h"

#ifdef __EMSCRIPTEN__
#include "build_timestamp.h"
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#include <SDL.h>
#include <SDL_opengl.h>

#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <functional>

namespace {

std::string g_defaultCaptureDeviceName = "";

SDL_AudioDeviceID g_devIdInp = 0;
SDL_AudioDeviceID g_devIdOut = 0;

SDL_AudioSpec g_obtainedSpecInp;
SDL_AudioSpec g_obtainedSpecOut;

GGWave *g_ggWave = nullptr;

}

static std::function<bool()> g_doInit;
static std::function<void(int, int)> g_setWindowSize;
static std::function<bool()> g_mainUpdate;

void mainUpdate(void *) {
    g_mainUpdate();
}

// JS interface

extern "C" {
    EMSCRIPTEN_KEEPALIVE
        int sendData(int textLength, const char * text, int protocolId, int volume) {
            g_ggWave->init(textLength, text, GGWave::TxProtocolId(protocolId), volume);
            return 0;
        }

    EMSCRIPTEN_KEEPALIVE
        int getText(char * text) {
            std::copy(g_ggWave->rxData().begin(), g_ggWave->rxData().end(), text);
            return 0;
        }

    EMSCRIPTEN_KEEPALIVE
        float sampleRate()        { return g_ggWave->sampleRateInp(); }

    EMSCRIPTEN_KEEPALIVE
        int framesToRecord()      { return g_ggWave->rxFramesToRecord(); }

    EMSCRIPTEN_KEEPALIVE
        int framesLeftToRecord()  { return g_ggWave->rxFramesLeftToRecord(); }

    EMSCRIPTEN_KEEPALIVE
        int framesToAnalyze()     { return g_ggWave->rxFramesToAnalyze(); }

    EMSCRIPTEN_KEEPALIVE
        int framesLeftToAnalyze() { return g_ggWave->rxFramesLeftToAnalyze(); }

    EMSCRIPTEN_KEEPALIVE
        int hasDeviceOutput()     { return g_devIdOut; }

    EMSCRIPTEN_KEEPALIVE
        int hasDeviceCapture()    { return g_devIdInp; }

    EMSCRIPTEN_KEEPALIVE
        int doInit()              { return g_doInit(); }
}

void GGWave_setDefaultCaptureDeviceName(std::string name) {
    g_defaultCaptureDeviceName = std::move(name);
}

bool GGWave_init(
        const int playbackId,
        const int captureId,
        const int payloadLength,
        const float sampleRateOffset,
        const bool useDSS) {

    if (g_devIdInp && g_devIdOut) {
        return false;
    }

    if (g_devIdInp == 0 && g_devIdOut == 0) {
        SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);

        if (SDL_Init(SDL_INIT_AUDIO) < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL: %s\n", SDL_GetError());
            return (1);
        }

        SDL_SetHintWithPriority(SDL_HINT_AUDIO_RESAMPLING_MODE, "medium", SDL_HINT_OVERRIDE);

        {
            int nDevices = SDL_GetNumAudioDevices(SDL_FALSE);
            printf("Found %d playback devices:\n", nDevices);
            for (int i = 0; i < nDevices; i++) {
                printf("    - Playback device #%d: '%s'\n", i, SDL_GetAudioDeviceName(i, SDL_FALSE));
            }
        }
        {
            int nDevices = SDL_GetNumAudioDevices(SDL_TRUE);
            printf("Found %d capture devices:\n", nDevices);
            for (int i = 0; i < nDevices; i++) {
                printf("    - Capture device #%d: '%s'\n", i, SDL_GetAudioDeviceName(i, SDL_TRUE));
            }
        }
    }

    bool reinit = false;

    if (g_devIdOut == 0) {
        printf("Initializing playback ...\n");

        SDL_AudioSpec playbackSpec;
        SDL_zero(playbackSpec);

        playbackSpec.freq = GGWave::kDefaultSampleRate + sampleRateOffset;
        playbackSpec.format = AUDIO_S16SYS;
        playbackSpec.channels = 1;
        playbackSpec.samples = 16*1024;
        playbackSpec.callback = NULL;

        SDL_zero(g_obtainedSpecOut);

        if (playbackId >= 0) {
            printf("Attempt to open playback device %d : '%s' ...\n", playbackId, SDL_GetAudioDeviceName(playbackId, SDL_FALSE));
            g_devIdOut = SDL_OpenAudioDevice(SDL_GetAudioDeviceName(playbackId, SDL_FALSE), SDL_FALSE, &playbackSpec, &g_obtainedSpecOut, 0);
        } else {
            printf("Attempt to open default playback device ...\n");
            g_devIdOut = SDL_OpenAudioDevice(NULL, SDL_FALSE, &playbackSpec, &g_obtainedSpecOut, 0);
        }

        if (!g_devIdOut) {
            printf("Couldn't open an audio device for playback: %s!\n", SDL_GetError());
            g_devIdOut = 0;
        } else {
            printf("Obtained spec for output device (SDL Id = %d):\n", g_devIdOut);
            printf("    - Sample rate:       %d (required: %d)\n", g_obtainedSpecOut.freq, playbackSpec.freq);
            printf("    - Format:            %d (required: %d)\n", g_obtainedSpecOut.format, playbackSpec.format);
            printf("    - Channels:          %d (required: %d)\n", g_obtainedSpecOut.channels, playbackSpec.channels);
            printf("    - Samples per frame: %d (required: %d)\n", g_obtainedSpecOut.samples, playbackSpec.samples);

            if (g_obtainedSpecOut.format != playbackSpec.format ||
                g_obtainedSpecOut.channels != playbackSpec.channels ||
                g_obtainedSpecOut.samples != playbackSpec.samples) {
                g_devIdOut = 0;
                SDL_CloseAudio();
                fprintf(stderr, "Failed to initialize playback SDL_OpenAudio!");

                return false;
            }

            reinit = true;
        }
    }

    if (g_devIdInp == 0) {
        SDL_AudioSpec captureSpec;
        captureSpec = g_obtainedSpecOut;
        captureSpec.freq = GGWave::kDefaultSampleRate + sampleRateOffset;
        captureSpec.format = AUDIO_F32SYS;
        captureSpec.samples = 1024;

        SDL_zero(g_obtainedSpecInp);

        if (captureId >= 0) {
            printf("Attempt to open capture device %d : '%s' ...\n", captureId, SDL_GetAudioDeviceName(captureId, SDL_TRUE));
            g_devIdInp = SDL_OpenAudioDevice(SDL_GetAudioDeviceName(captureId, SDL_TRUE), SDL_TRUE, &captureSpec, &g_obtainedSpecInp, 0);
        } else {
            printf("Attempt to open default capture device ...\n");
            g_devIdInp = SDL_OpenAudioDevice(g_defaultCaptureDeviceName.empty() ? nullptr : g_defaultCaptureDeviceName.c_str(),
                                            SDL_TRUE, &captureSpec, &g_obtainedSpecInp, 0);
        }
        if (!g_devIdInp) {
            printf("Couldn't open an audio device for capture: %s!\n", SDL_GetError());
            g_devIdInp = 0;
        } else {
            printf("Obtained spec for input device (SDL Id = %d):\n", g_devIdInp);
            printf("    - Sample rate:       %d\n", g_obtainedSpecInp.freq);
            printf("    - Format:            %d (required: %d)\n", g_obtainedSpecInp.format, captureSpec.format);
            printf("    - Channels:          %d (required: %d)\n", g_obtainedSpecInp.channels, captureSpec.channels);
            printf("    - Samples per frame: %d\n", g_obtainedSpecInp.samples);

            reinit = true;
        }
    }

    GGWave::SampleFormat sampleFormatInp = GGWAVE_SAMPLE_FORMAT_UNDEFINED;
    GGWave::SampleFormat sampleFormatOut = GGWAVE_SAMPLE_FORMAT_UNDEFINED;

    switch (g_obtainedSpecInp.format) {
        case AUDIO_U8:      sampleFormatInp = GGWAVE_SAMPLE_FORMAT_U8;  break;
        case AUDIO_S8:      sampleFormatInp = GGWAVE_SAMPLE_FORMAT_I8;  break;
        case AUDIO_U16SYS:  sampleFormatInp = GGWAVE_SAMPLE_FORMAT_U16; break;
        case AUDIO_S16SYS:  sampleFormatInp = GGWAVE_SAMPLE_FORMAT_I16; break;
        case AUDIO_S32SYS:  sampleFormatInp = GGWAVE_SAMPLE_FORMAT_F32; break;
        case AUDIO_F32SYS:  sampleFormatInp = GGWAVE_SAMPLE_FORMAT_F32; break;
    }

    switch (g_obtainedSpecOut.format) {
        case AUDIO_U8:      sampleFormatOut = GGWAVE_SAMPLE_FORMAT_U8;  break;
        case AUDIO_S8:      sampleFormatOut = GGWAVE_SAMPLE_FORMAT_I8;  break;
        case AUDIO_U16SYS:  sampleFormatOut = GGWAVE_SAMPLE_FORMAT_U16; break;
        case AUDIO_S16SYS:  sampleFormatOut = GGWAVE_SAMPLE_FORMAT_I16; break;
        case AUDIO_S32SYS:  sampleFormatOut = GGWAVE_SAMPLE_FORMAT_F32; break;
        case AUDIO_F32SYS:  sampleFormatOut = GGWAVE_SAMPLE_FORMAT_F32; break;
            break;
    }

    if (reinit) {
        if (g_ggWave) delete g_ggWave;

        GGWave::OperatingMode mode = GGWAVE_OPERATING_MODE_RX_AND_TX;
        if (useDSS) mode |= GGWAVE_OPERATING_MODE_USE_DSS;

        g_ggWave = new GGWave({
            payloadLength,
            (float) g_obtainedSpecInp.freq,
            (float) g_obtainedSpecOut.freq,
            GGWave::kDefaultSampleRate,
            512,
            GGWave::kDefaultSoundMarkerThreshold,
            sampleFormatInp,
            sampleFormatOut,
            mode,
        });
    }

    return true;
}

GGWave *& GGWave_instance() { return g_ggWave; }

bool GGWave_mainLoop() {
    if (g_devIdInp == 0 && g_devIdOut == 0) {
        return false;
    }

    if (g_ggWave->txHasData() == false) {
        SDL_PauseAudioDevice(g_devIdOut, SDL_FALSE);

        static auto tLastNoData = std::chrono::high_resolution_clock::now();
        auto tNow = std::chrono::high_resolution_clock::now();

        if ((int) SDL_GetQueuedAudioSize(g_devIdOut) < g_ggWave->samplesPerFrame()*g_ggWave->sampleSizeOut()) {
            SDL_PauseAudioDevice(g_devIdInp, SDL_FALSE);
            const int nHave = (int) SDL_GetQueuedAudioSize(g_devIdInp);
            const int nNeed = g_ggWave->samplesPerFrame()*g_ggWave->sampleSizeInp();
            if (::getTime_ms(tLastNoData, tNow) > 500.0f && nHave >= nNeed) {
                static std::vector<uint8_t> dataInp(nNeed);
                SDL_DequeueAudio(g_devIdInp, dataInp.data(), nNeed);

                if (g_ggWave->decode(dataInp.data(), dataInp.size()) == false) {
                    fprintf(stderr, "Warning: failed to decode input data!\n");
                } else {
                    GGWave::TxRxData rxData;
                    int n = g_ggWave->rxTakeData(rxData);
                    if (n > 0) {
                        std::time_t timestamp = std::time(nullptr);
                        std::string tstr = std::asctime(std::localtime(&timestamp));
                        tstr.back() = 0;
                        printf("[%s] Received: '%s'\n", tstr.c_str(), rxData.data());
                    }
                }

                if (nHave > 32*nNeed) {
                    fprintf(stderr, "Warning: slow processing, clearing queued audio buffer of %d bytes ...", SDL_GetQueuedAudioSize(g_devIdInp));
                    SDL_ClearQueuedAudio(g_devIdInp);
                }
            } else {
                SDL_ClearQueuedAudio(g_devIdInp);
            }
        } else {
            tLastNoData = tNow;
        }
    } else {
        SDL_PauseAudioDevice(g_devIdOut, SDL_TRUE);
        SDL_PauseAudioDevice(g_devIdInp, SDL_TRUE);

        const auto nBytes = g_ggWave->encode();
        SDL_QueueAudio(g_devIdOut, g_ggWave->txWaveform(), nBytes);
    }

    return true;
}

bool GGWave_deinit() {
    if (g_devIdInp == 0 && g_devIdOut == 0) {
        return false;
    }

    delete g_ggWave;
    g_ggWave = nullptr;

    SDL_PauseAudioDevice(g_devIdInp, 1);
    SDL_CloseAudioDevice(g_devIdInp);
    SDL_PauseAudioDevice(g_devIdOut, 1);
    SDL_CloseAudioDevice(g_devIdOut);

    g_devIdInp = 0;
    g_devIdOut = 0;

    return true;
}

int main(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
    printf("Build time: %s\n", BUILD_TIMESTAMP);
    printf("Press the Init button to start\n");

    if (argv[1]) {
        GGWave_setDefaultCaptureDeviceName(argv[1]);
    }
#else
    printf("Usage: %s [-cN] [-lN]\n", argv[0]);
    printf("    -cN - select capture device N\n");
    printf("    -lN - fixed payload length of size N, N in [1, %d]\n", GGWave::kMaxLengthFixed);
    printf("\n");
#endif

    GGWave::Protocols::rx() = { {
        { "[R2T2] Normal",      64,  9, 1, 2, true, },
        { "[R2T2] Fast",        64,  6, 1, 2, true, },
        { "[R2T2] Fastest",     64,  3, 1, 2, true, },
        { "[R2T2] Low Normal",  16,  9, 1, 2, true, },
        { "[R2T2] Low Fast",    16,  6, 1, 2, true, },
        { "[R2T2] Low Fastest", 16,  3, 1, 2, true, },
    } };

    const auto argm = parseCmdArguments(argc, argv);
    const int captureId = argm.count("c") == 0 ? 0 : std::stoi(argm.at("c"));
    const int payloadLength = argm.count("l") == 0 ? 16 : std::stoi(argm.at("l"));

    bool isInitialized = false;

    g_doInit = [&]() {
        if (GGWave_init(0, captureId, payloadLength, 0, true) == false) {
            fprintf(stderr, "Failed to initialize GGWave\n");
            return false;
        }

        isInitialized = true;
        printf("Listening for payload with length = %d bytes ..\n", payloadLength);

        return true;
    };

    g_mainUpdate = [&]() {
        if (isInitialized == false) {
            return true;
        }

        GGWave_mainLoop();

        return true;
    };

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(mainUpdate, NULL, 60, true);
#else
    if (g_doInit() == false) {
        printf("Error: failed to initialize audio\n");
        return -2;
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (g_mainUpdate() == false) break;
    }

    GGWave_deinit();

    // Cleanup
    SDL_CloseAudio();
    SDL_Quit();
#endif

    return 0;
}
