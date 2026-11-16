#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include <SDL3/SDL.h>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr uint32_t kBlockFrames = kRate / 100;
constexpr size_t kBlockSamples = size_t(kBlockFrames) * 2;
constexpr size_t kRingSamples = kBlockSamples * 6;

std::mutex g_lock;
std::condition_variable g_wake;
std::array<float, kRingSamples> g_ring{};
size_t g_read = 0, g_write = 0, g_count = 0;
bool g_suspended = false;
enum class DeviceChange { None, Added, Removed };
DeviceChange g_device_change = DeviceChange::None;

void SDLCALL Pull(void*, SDL_AudioStream* stream, int additional, int) {
    const size_t samples = size_t(std::max(0, additional)) / (2 * sizeof(float)) * 2;
    std::array<float, kBlockSamples> buffer;
    size_t missing = 0;
    for (size_t offset = 0; offset < samples;) {
        const size_t want = std::min(samples - offset, buffer.size());
        size_t take;
        {
            std::lock_guard lock(g_lock);
            take = std::min(want, g_count);
            const size_t first = std::min(take, g_ring.size() - g_read);
            std::copy_n(g_ring.data() + g_read, first, buffer.data());
            std::copy_n(g_ring.data(), take - first, buffer.data() + first);
            g_read = (g_read + take) % g_ring.size();
            g_count -= take;
        }
        g_wake.notify_one();
        std::fill(buffer.begin() + take, buffer.begin() + want, 0.0f);
        missing += want - take;
        if (!SDL_PutAudioStreamData(stream, buffer.data(), int(want * sizeof(float)))) break;
        offset += want;
    }
    if (missing) LOG_FIRST(4, "audio: mixer underrun, filling %zu frames with silence", missing / 2);
}

void Run(std::function<void(float*, uint32_t)> render) {
    SDL_AudioStream* stream = nullptr;
    bool audio_initialized = false;
    bool paused = true;
    auto retry_at = Clock::now();
    auto silent_at = retry_at;
    std::array<float, kBlockSamples> block{};
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};

    for (;;) {
        std::unique_lock lock(g_lock);
        const bool suspended = g_suspended;
        if (g_device_change != DeviceChange::None) {
            const auto change = g_device_change;
            g_device_change = DeviceChange::None;
            if (stream && change == DeviceChange::Added) continue;
            g_read = g_write = g_count = 0;
            retry_at = Clock::now();
            lock.unlock();
            if (stream) SDL_DestroyAudioStream(stream);
            stream = nullptr;
            paused = true;
            continue;
        }
        const auto now = Clock::now();
        if (!stream && !suspended && now >= retry_at) {
            lock.unlock();
            if (!audio_initialized) audio_initialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
            if (audio_initialized)
                stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Pull, nullptr);
            retry_at = now + 1s;
            if (stream) LOGI("audio: %s at %u Hz", SDL_GetCurrentAudioDriver(), kRate);
            else LOG_FIRST(4, "audio: no output device (%s), keeping the mixer clock running", SDL_GetError());
            continue;
        }
        // Prime a new or drained device before its first callback.
        if (stream && paused != suspended && (suspended || g_count >= 3 * kBlockSamples)) {
            lock.unlock();
            const bool ok = suspended ? SDL_PauseAudioStreamDevice(stream) : SDL_ResumeAudioStreamDevice(stream);
            if (ok) {
                paused = suspended;
            } else {
                LOG_FIRST(4, "audio: device stopped responding (%s)", SDL_GetError());
                SDL_DestroyAudioStream(stream);
                stream = nullptr;
                paused = true;
                retry_at = Clock::now() + 1s;
                lock.lock();
                g_read = g_write = g_count = 0;
            }
            continue;
        }
        if (suspended || (stream && g_count + block.size() > g_ring.size())) {
            g_wake.wait(lock);
            continue;
        }
        if (!stream && now < silent_at) {
            g_wake.wait_until(lock, std::min(silent_at, retry_at));
            continue;
        }

        lock.unlock();
        render(block.data(), kBlockFrames);
        // Without a device the mixer runs on its own 10 ms schedule. Advance the deadline rather than
        // restarting it from the wake-up time, so wake-up latency does not slow the game's audio clock;
        // resync only after falling far behind (a stall or a suspend).
        silent_at += 10ms;
        if (stream || Clock::now() - silent_at > 100ms) silent_at = Clock::now();
        if (!stream) continue;
        lock.lock();
        if (g_suspended || g_device_change == DeviceChange::Removed) continue;
        const size_t first = std::min(block.size(), g_ring.size() - g_write);
        std::copy_n(block.data(), first, g_ring.data() + g_write);
        std::copy_n(block.data() + first, block.size() - first, g_ring.data());
        g_write = (g_write + block.size()) % g_ring.size();
        g_count += block.size();
    }
}

} // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread(Run, std::move(render)).detach();
}

void SetSuspended(bool suspended) {
    {
        std::lock_guard lock(g_lock);
        g_suspended = suspended;
    }
    g_wake.notify_one();
}

void DeviceChanged(bool removed) {
    {
        std::lock_guard lock(g_lock);
        if (removed || g_device_change == DeviceChange::None)
            g_device_change = removed ? DeviceChange::Removed : DeviceChange::Added;
    }
    g_wake.notify_one();
}

} // namespace audio
