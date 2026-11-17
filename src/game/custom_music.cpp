#include "game_addresses.h"
// MP3 and OGG songs for the Music Player.
//
// The streamer opens <track>.WMA and hands the file handle to the XDK WMA
// decoder factory (0x29F66D, (descriptor, out object)); the handle is the
// second dword of the descriptor. When kernel/files.cpp substituted an MP3 or
// OGG file for that handle, the factory returns a stand-in object instead: guest
// memory whose vtable is the real decoder's, so every call through it lands in
// the hooks below. They recognise the stand-in and serve 16-bit PCM from a host
// decoder; for any other object they run the original method.
//
// The streamer only uses the decoder synchronously: Process fills the packet
// before returning, and a packet with no data ends the song.

#include "custom_music_decoder.h"

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace game {
namespace {

constexpr uint32_t kDecoderVtable = TCNYC_PAL_EU ? 0x002E005Cu : TCNYC_PAL ? 0x2E016Cu : 0x2DF9BCu;
constexpr uint32_t kDecoderSize = 0x94;  // what the factory allocates; +0x34 writes at +0x8C
constexpr uint32_t kOk = 0;
constexpr uint32_t kFail = 0x80004005;
constexpr uint32_t kNotImplemented = 0x80004001;
constexpr uint32_t kAcceptOutput = 0x02;  // XMO_STATUSF_ACCEPT_OUTPUT_DATA

struct StandIn {
    std::unique_ptr<MusicDecoder> decoder;
    uint32_t refs = 1;
    uint64_t position = 0;  // frames delivered
    std::vector<int16_t> scratch;
};

std::mutex g_lock;
std::unordered_map<uint32_t, StandIn> g_objects;  // guest object -> host decoder
// Stand-ins alive. While it is zero (no custom song open), calls on the game's own WMA decoders go
// straight to the original method without taking the lock.
std::atomic<uint32_t> g_live{0};

uint32_t Arg(const X86Context& ctx, int i) { return kernel::R32(ctx.esp + 4 + 4 * i); }

// stdcall return: pops the return address and nargs arguments
void Return(X86Context& ctx, uint32_t value, int nargs) {
    ctx.eax = value;
    ctx.esp += 4 * (nargs + 1);
}

uint32_t Create(const std::filesystem::path& path) {
    std::unique_ptr<MusicDecoder> decoder = OpenMusicDecoder(path);
    if (!decoder) return 0;
    const uint32_t object = mem::AllocSystem(kDecoderSize);
    if (!object) return 0;
    std::memset(kernel::HostPtr(object), 0, kDecoderSize);
    kernel::W32(object, kDecoderVtable);
    LOGR("radio: decoding %s on the host (%u Hz, %u channels, %.1f s)", reinterpret_cast<const char*>(path.filename().u8string().c_str()),
         decoder->rate, decoder->channels, decoder->rate ? double(decoder->frames) / decoder->rate : 0.0);
    std::lock_guard<std::mutex> lock(g_lock);
    g_objects[object].decoder = std::move(decoder);
    g_live.fetch_add(1, std::memory_order_release);
    return object;
}

uint32_t BytesPerFrame(const StandIn& s) { return s.decoder->channels * 2; }

} // namespace
} // namespace game

// Every handler takes the lock, looks up `this`, and falls back to the guest
// method when the object is a real WMA decoder. With no stand-in alive it
// skips the lock and the lookup.
#define STAND_IN(addr)                                                    \
    if (game::g_live.load(std::memory_order_acquire) == 0) {              \
        __imp__sub_##addr(ctx, base);                                     \
        return;                                                           \
    }                                                                     \
    std::unique_lock<std::mutex> lock(game::g_lock);                      \
    auto found = game::g_objects.find(game::Arg(ctx, 0));                 \
    if (found == game::g_objects.end()) {                                 \
        lock.unlock();                                                    \
        __imp__sub_##addr(ctx, base);                                     \
        return;                                                           \
    }                                                                     \
    game::StandIn& self = found->second;                                  \
    (void)self

// factory (descriptor, out object)
X86_EXTERN_FUNC(__imp__sub_0029F66D);
X86_FUNC(sub_0029F66D) {
    const uint32_t descriptor = game::Arg(ctx, 0);
    const uint32_t out = game::Arg(ctx, 1);
    const std::filesystem::path path = descriptor ? kernel::HostDecodedRadioPath(kernel::R32(descriptor + 4))
                                                  : std::filesystem::path{};
    if (path.empty()) {
        __imp__sub_0029F66D(ctx, base);
        return;
    }
    const uint32_t object = game::Create(path);
    if (out) kernel::W32(out, object);
    game::Return(ctx, object ? game::kOk : game::kFail, 2);
}

// AddRef (this)
X86_EXTERN_FUNC(__imp__sub_0029F4C5);
X86_FUNC(sub_0029F4C5) {
    STAND_IN(0029F4C5);
    game::Return(ctx, ++self.refs, 1);
}

// Release (this)
X86_EXTERN_FUNC(__imp__sub_0029F637);
X86_FUNC(sub_0029F637) {
    STAND_IN(0029F637);
    const uint32_t refs = --self.refs;
    if (!refs) {
        const uint32_t object = found->first;
        game::g_objects.erase(found);
        game::g_live.fetch_sub(1, std::memory_order_release);
        mem::FreeSystem(object);
    }
    game::Return(ctx, refs, 1);
}

// GetInfo (this, XMEDIAINFO*): flags, input size, output size, lookahead
X86_EXTERN_FUNC(__imp__sub_0029F21F);
X86_FUNC(sub_0029F21F) {
    STAND_IN(0029F21F);
    if (const uint32_t info = game::Arg(ctx, 1)) {
        kernel::W32(info, 3);
        kernel::W32(info + 4, 0);
        kernel::W32(info + 8, self.decoder->channels << 12);
        kernel::W32(info + 12, 0);
    }
    game::Return(ctx, game::kOk, 2);
}

// GetStatus (this, DWORD*)
X86_EXTERN_FUNC(__imp__sub_0029F261);
X86_FUNC(sub_0029F261) {
    STAND_IN(0029F261);
    if (const uint32_t status = game::Arg(ctx, 1)) kernel::W32(status, game::kAcceptOutput);
    game::Return(ctx, game::kOk, 2);
}

// Process (this, input packet, output packet). XMEDIAPACKET: buffer, max size,
// completed size*, status*, completion event, timestamp*.
X86_EXTERN_FUNC(__imp__sub_0029F547);
X86_FUNC(sub_0029F547) {
    STAND_IN(0029F547);
    const uint32_t packet = game::Arg(ctx, 2);
    if (!packet) {
        game::Return(ctx, game::kFail, 3);
        return;
    }
    const uint32_t buffer = kernel::R32(packet);
    const uint32_t max_size = kernel::R32(packet + 4);
    const uint32_t completed = kernel::R32(packet + 8);
    const uint32_t status = kernel::R32(packet + 12);
    const uint32_t bytes_per_frame = game::BytesPerFrame(self);
    const size_t want = buffer ? max_size / bytes_per_frame : 0;
    size_t got = 0;
    if (want) {
        const size_t need = want * self.decoder->channels;
        if (self.scratch.size() < need) self.scratch.resize(need);
        // A decoder may return short reads before the end; fill the packet.
        while (got < want) {
            const size_t n = self.decoder->Read(self.scratch.data() + got * self.decoder->channels, want - got);
            if (!n) break;
            got += n;
        }
        std::memcpy(kernel::HostPtr(buffer), self.scratch.data(), got * bytes_per_frame);
    }
    self.position += got;
    if (completed) kernel::W32(completed, uint32_t(got * bytes_per_frame));
    if (status) kernel::W32(status, 0);  // XMEDIAPACKET_STATUS_SUCCESS
    if (kernel::R32(packet + 16)) LOG_FIRST(1, "radio: decoder packet has a completion event; not signalled");
    game::Return(ctx, game::kOk, 3);
}

// Discontinuity (this)
X86_EXTERN_FUNC(__imp__sub_0029F25C);
X86_FUNC(sub_0029F25C) {
    STAND_IN(0029F25C);
    game::Return(ctx, game::kOk, 1);
}

// Flush (this)
X86_EXTERN_FUNC(__imp__sub_0029F4E3);
X86_FUNC(sub_0029F4E3) {
    STAND_IN(0029F4E3);
    game::Return(ctx, game::kOk, 1);
}

// Seek (this, offset, origin, DWORD* absolute): the WMA decoder only reports
// where it is, so this does the same.
X86_EXTERN_FUNC(__imp__sub_0029F31A);
X86_FUNC(sub_0029F31A) {
    STAND_IN(0029F31A);
    if (const uint32_t absolute = game::Arg(ctx, 3))
        kernel::W32(absolute, uint32_t(self.position * game::BytesPerFrame(self)));
    game::Return(ctx, game::kOk, 4);
}

// GetLength (this, DWORD*): decoded size in bytes
X86_EXTERN_FUNC(__imp__sub_0029F380);
X86_FUNC(sub_0029F380) {
    STAND_IN(0029F380);
    if (const uint32_t length = game::Arg(ctx, 1)) {
        const uint64_t bytes = self.decoder->frames * game::BytesPerFrame(self);
        kernel::W32(length, bytes > UINT32_MAX ? UINT32_MAX : uint32_t(bytes));
    }
    game::Return(ctx, game::kOk, 2);
}

// DoWork (this): nothing is queued
X86_EXTERN_FUNC(__imp__sub_0029F14F);
X86_FUNC(sub_0029F14F) {
    STAND_IN(0029F14F);
    game::Return(ctx, game::kOk, 1);
}

// GetFileHeader (this, WMAXMOFileHeader*): version, sample rate, channels,
// duration (ms), bitrate. The streamer builds its PCM format from this.
X86_EXTERN_FUNC(__imp__sub_0029F2E0);
X86_FUNC(sub_0029F2E0) {
    STAND_IN(0029F2E0);
    if (const uint32_t header = game::Arg(ctx, 1)) {
        const game::MusicDecoder& d = *self.decoder;
        const uint64_t ms = d.rate ? d.frames * 1000 / d.rate : 0;
        kernel::W32(header, 2);
        kernel::W32(header + 4, d.rate);
        kernel::W32(header + 8, d.channels);
        kernel::W32(header + 12, ms > UINT32_MAX ? UINT32_MAX : uint32_t(ms));
        kernel::W32(header + 16, d.rate * d.channels * 16);
    }
    game::Return(ctx, game::kOk, 2);
}

// GetFileContentDescription (this, out): no title or artist tags
X86_EXTERN_FUNC(__imp__sub_0029F296);
X86_FUNC(sub_0029F296) {
    STAND_IN(0029F296);
    game::Return(ctx, game::kNotImplemented, 2);
}

#undef STAND_IN
