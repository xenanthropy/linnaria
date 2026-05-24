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

    // Target latency budgets (the engine's mobile-era sample rates push these
    // into hundreds of ms when we count in fixed bytes -- e.g. 22 kHz mono
    // makes a 16 KiB chunk last 370 ms, which compounds into 2 s of delay
    // when stacked behind PulseAudio's own buffering. Always derive byte
    // counts from these via bytes_per_second.)
    inline constexpr uint32_t CHUNK_MS         = 50;   // engine's per-write chunk
    inline constexpr uint32_t QUEUE_LIMIT_MS   = 100;  // throttle threshold

    // Returned from AudioTrack.getMinBufferSize(III)I. The engine uses this as
    // the byte-array size and the per-write chunk, so it directly controls
    // input latency. Computed from the engine's requested format so the time
    // budget is consistent across sample rates.
    inline int GetMinBufferSize(int sampleRate, int channels, int bits) {
        int bps = sampleRate * channels * (bits / 8);
        int bytes = static_cast<int>((static_cast<uint64_t>(bps) * CHUNK_MS) / 1000);
        if (bytes < 1024) bytes = 1024; // floor: avoid silly-small chunks
        // Round up to a multiple of 4 for cleanliness (frame size is at most 4)
        bytes = (bytes + 3) & ~3;
        return bytes;
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

        // Throttle by TIME, not bytes -- at 22 kHz mono / 16-bit, the same
        // 64 KiB byte threshold would correspond to ~1.5 s, on top of which
        // PulseAudio's own buffers pile up to ~2 s of delay before sounds
        // play. Cap our queue at QUEUE_LIMIT_MS so total latency stays in
        // the ~150 ms range regardless of sample rate.
        const Uint32 max_queued = (bps * QUEUE_LIMIT_MS) / 1000;
        while (SDL_GetQueuedAudioSize(dev) > max_queued) {
            SDL_Delay(2);
        }

        SDL_QueueAudio(dev, data, static_cast<Uint32>(length));
    }

}
