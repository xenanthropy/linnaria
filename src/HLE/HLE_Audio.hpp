#pragma once
#include <SDL2/SDL.h>
#include <atomic>
#include <iostream>
#include <mutex>

// Audio output bridge. The engine uses Android AudioTrack via JNI; we
// intercept the AudioTrack JNI methods in JNIEmulator and route the PCM
// here. Implementation details:
//   * Queue-based: SDL_QueueAudio appends PCM to SDL's internal mixer
//     queue, the OS audio thread drains it. We don't write a callback.
//   * Throttled: AudioTrack.write() on real Android blocks when the
//     hardware buffer is full -- the engine's audio thread relies on
//     that for pacing. We mimic it by sleeping in Write() until the
//     queued depth drops below a threshold.
//   * Best-effort device open: if SDL_OpenAudioDevice fails (no audio
//     hardware / no PulseAudio / permissions), Write becomes a silent
//     no-op but still sleeps proportionally so the audio thread doesn't
//     spin.
namespace HLE::Audio {

    inline std::mutex device_mutex;
    inline SDL_AudioDeviceID device = 0;
    inline uint32_t bytes_per_second = 176400; // default 44.1kHz / 16-bit / stereo

    // Returned from AudioTrack.getMinBufferSize(III)I. Bytes. The engine
    // uses this as the byte-array size, so it's also the chunk size of
    // every Write call. 16 KiB at 44.1kHz / 16-bit / stereo is ~93 ms --
    // generous enough that occasional GC / scheduler stalls don't underrun.
    inline int GetMinBufferSize() {
        return 16384;
    }

    inline void OpenDevice(int sampleRate, int channels, int bits) {
        std::lock_guard<std::mutex> lock(device_mutex);
        if (device != 0) return; // already open; second AudioTrack reuses it

        SDL_AudioSpec want{}, have{};
        want.freq     = sampleRate;
        want.format   = (bits == 8) ? AUDIO_U8 : AUDIO_S16LSB;
        want.channels = static_cast<Uint8>(channels);
        want.samples  = 1024;
        want.callback = nullptr; // queue-based

        device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        bytes_per_second = static_cast<uint32_t>(sampleRate) * channels * (bits / 8);

        if (device == 0) {
            std::cerr << "[Audio] SDL_OpenAudioDevice failed: " << SDL_GetError()
                      << " (silent fallback)" << std::endl;
            return;
        }
        std::cout << "[Audio] Device opened: " << sampleRate << " Hz, "
                  << channels << "ch, " << bits << "-bit" << std::endl;
    }

    inline void Play() {
        std::lock_guard<std::mutex> lock(device_mutex);
        if (device) SDL_PauseAudioDevice(device, 0);
    }

    inline void Stop() {
        std::lock_guard<std::mutex> lock(device_mutex);
        if (device) SDL_PauseAudioDevice(device, 1);
    }

    inline void Close() {
        std::lock_guard<std::mutex> lock(device_mutex);
        if (device) {
            SDL_CloseAudioDevice(device);
            device = 0;
        }
    }

    inline void Write(const uint8_t* data, int length) {
        SDL_AudioDeviceID dev;
        uint32_t bps;
        {
            std::lock_guard<std::mutex> lock(device_mutex);
            dev = device;
            bps = bytes_per_second;
        }

        if (dev == 0) {
            // Silent fallback: sleep for the wall-clock duration of the
            // audio we'd have played, so the engine's audio thread doesn't
            // spin at 100% CPU.
            if (bps > 0 && length > 0) {
                uint32_t ms = (static_cast<uint32_t>(length) * 1000) / bps;
                if (ms > 0) SDL_Delay(ms);
            }
            return;
        }

        // Throttle: block until SDL's queue has drained below ~4 buffers'
        // worth. Mirrors AudioTrack.write blocking on a full hardware buffer.
        const Uint32 max_queued = 4 * 16384;
        while (SDL_GetQueuedAudioSize(dev) > max_queued) {
            SDL_Delay(2);
        }

        SDL_QueueAudio(dev, data, static_cast<Uint32>(length));
    }

}
