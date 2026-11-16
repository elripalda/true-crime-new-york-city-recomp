// MP3 (dr_mp3), FLAC (dr_flac), WAV (dr_wav) and OGG Vorbis (stb_vorbis) decoding for custom radio songs.
// Kept in its own file: stb_vorbis.c defines short macro names.
#include "custom_music_decoder.h"

#include "core/log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../../sdk/third_party/audio/dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#include "../../sdk/third_party/audio/dr_flac.h"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "../../sdk/third_party/audio/dr_wav.h"

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "../../sdk/third_party/audio/stb_vorbis.c"

namespace kernel {
FILE* OpenHostFile(const std::filesystem::path& path, const char* mode);
}

namespace game {
namespace {

// Songs are a few megabytes, so the whole file is read up front. This keeps file
// access off the audio refill path and works the same with wide paths everywhere.
// The file is sized first and read in one call. A song is held whole in memory, so anything
// past kMaxSongBytes (a long uncompressed WAV) is refused rather than risking the address space.
constexpr uint64_t kMaxSongBytes = 512ull << 20;

bool ReadAll(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || !size) return false;
    if (size > kMaxSongBytes) {
        LOGW("radio: %s is over %llu MB; skipped", reinterpret_cast<const char*>(path.filename().u8string().c_str()),
             static_cast<unsigned long long>(kMaxSongBytes >> 20));
        return false;
    }
    FILE* fp = kernel::OpenHostFile(path, "rb");
    if (!fp) return false;
    out.resize(size_t(size));
    const size_t n = std::fread(out.data(), 1, out.size(), fp);
    const bool ok = !std::ferror(fp);
    std::fclose(fp);
    out.resize(n);  // the file may have shrunk since it was sized
    return ok && !out.empty();
}

class Mp3 final : public MusicDecoder {
public:
    explicit Mp3(std::vector<uint8_t> data) : data_(std::move(data)) {}
    ~Mp3() override {
        if (open_) drmp3_uninit(&mp3_);
    }
    bool Open() {
        open_ = drmp3_init_memory(&mp3_, data_.data(), data_.size(), nullptr) != 0;
        if (!open_ || mp3_.channels < 1 || mp3_.channels > 2 || !mp3_.sampleRate) return false;
        rate = mp3_.sampleRate;
        channels = mp3_.channels;
        frames = drmp3_get_pcm_frame_count(&mp3_);  // scans the file, then rewinds
        return drmp3_seek_to_pcm_frame(&mp3_, 0) != 0;
    }
    size_t Read(int16_t* out, size_t max_frames) override {
        return size_t(drmp3_read_pcm_frames_s16(&mp3_, max_frames, out));
    }
    bool Rewind() override { return drmp3_seek_to_pcm_frame(&mp3_, 0) != 0; }

private:
    std::vector<uint8_t> data_;
    drmp3 mp3_{};
    bool open_ = false;
};

class Ogg final : public MusicDecoder {
public:
    explicit Ogg(std::vector<uint8_t> data) : data_(std::move(data)) {}
    ~Ogg() override {
        if (vorbis_) stb_vorbis_close(vorbis_);
    }
    bool Open() {
        if (data_.size() > size_t(INT32_MAX)) return false;
        int error = 0;
        vorbis_ = stb_vorbis_open_memory(data_.data(), int(data_.size()), &error, nullptr);
        if (!vorbis_) return false;
        const stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
        if (info.channels < 1 || !info.sample_rate) return false;
        rate = info.sample_rate;
        channels = info.channels == 1 ? 1 : 2;  // surround is folded down to stereo
        frames = stb_vorbis_stream_length_in_samples(vorbis_);
        return true;
    }
    size_t Read(int16_t* out, size_t max_frames) override {
        const size_t shorts = std::min<size_t>(max_frames * channels, INT32_MAX);
        return size_t(stb_vorbis_get_samples_short_interleaved(vorbis_, int(channels), out, int(shorts)));
    }
    bool Rewind() override { return stb_vorbis_seek_start(vorbis_) != 0; }

private:
    std::vector<uint8_t> data_;
    stb_vorbis* vorbis_ = nullptr;
};

// Folds interleaved s16 with `source` channels down to mono or stereo in place.
size_t Fold(int16_t* pcm, size_t frames, uint32_t source, uint32_t target) {
    if (source == target) return frames;
    for (size_t f = 0; f < frames; ++f) {
        const int16_t* in = pcm + f * source;
        int32_t left = in[0], right = source > 1 ? in[1] : in[0];
        // the remaining channels (centre, surround) go into both sides at half level
        for (uint32_t c = 2; c < source; ++c) left += in[c] / 2, right += in[c] / 2;
        left = std::clamp(left, -32768, 32767);
        right = std::clamp(right, -32768, 32767);
        if (target == 1) {
            pcm[f] = int16_t((left + right) / 2);
        } else {
            pcm[f * 2] = int16_t(left);
            pcm[f * 2 + 1] = int16_t(right);
        }
    }
    return frames;
}

class Flac final : public MusicDecoder {
public:
    explicit Flac(std::vector<uint8_t> data) : data_(std::move(data)) {}
    ~Flac() override {
        if (flac_) drflac_close(flac_);
    }
    bool Open() {
        flac_ = drflac_open_memory(data_.data(), data_.size(), nullptr);
        if (!flac_ || !flac_->channels || !flac_->sampleRate) return false;
        rate = flac_->sampleRate;
        source_ = flac_->channels;
        channels = source_ == 1 ? 1 : 2;
        frames = flac_->totalPCMFrameCount;
        return true;
    }
    size_t Read(int16_t* out, size_t max_frames) override {
        const size_t need = max_frames * source_;
        if (buffer_.size() < need) buffer_.resize(need);
        const size_t n = size_t(drflac_read_pcm_frames_s16(flac_, max_frames, buffer_.data()));
        Fold(buffer_.data(), n, source_, channels);
        std::memcpy(out, buffer_.data(), n * channels * sizeof(int16_t));
        return n;
    }
    bool Rewind() override { return drflac_seek_to_pcm_frame(flac_, 0) != 0; }

private:
    std::vector<uint8_t> data_;
    std::vector<int16_t> buffer_;
    drflac* flac_ = nullptr;
    uint32_t source_ = 0;
};

class Wav final : public MusicDecoder {
public:
    explicit Wav(std::vector<uint8_t> data) : data_(std::move(data)) {}
    ~Wav() override {
        if (open_) drwav_uninit(&wav_);
    }
    bool Open() {
        open_ = drwav_init_memory(&wav_, data_.data(), data_.size(), nullptr) != 0;
        if (!open_ || !wav_.channels || !wav_.sampleRate) return false;
        rate = wav_.sampleRate;
        source_ = wav_.channels;
        channels = source_ == 1 ? 1 : 2;
        frames = wav_.totalPCMFrameCount;
        return true;
    }
    size_t Read(int16_t* out, size_t max_frames) override {
        const size_t need = max_frames * source_;
        if (buffer_.size() < need) buffer_.resize(need);
        const size_t n = size_t(drwav_read_pcm_frames_s16(&wav_, max_frames, buffer_.data()));
        Fold(buffer_.data(), n, source_, channels);
        std::memcpy(out, buffer_.data(), n * channels * sizeof(int16_t));
        return n;
    }
    bool Rewind() override { return drwav_seek_to_pcm_frame(&wav_, 0) != 0; }

private:
    std::vector<uint8_t> data_;
    std::vector<int16_t> buffer_;
    drwav wav_{};
    bool open_ = false;
    uint32_t source_ = 0;
};

} // namespace

std::unique_ptr<MusicDecoder> OpenMusicDecoder(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::vector<uint8_t> data;
    if (!ReadAll(path, data)) {
        LOGW("radio: could not read %s", reinterpret_cast<const char*>(path.filename().u8string().c_str()));
        return nullptr;
    }
    if (ext == ".mp3") {
        auto mp3 = std::make_unique<Mp3>(std::move(data));
        if (mp3->Open()) return mp3;
    } else if (ext == ".ogg") {
        auto ogg = std::make_unique<Ogg>(std::move(data));
        if (ogg->Open()) return ogg;
    } else if (ext == ".flac") {
        auto flac = std::make_unique<Flac>(std::move(data));
        if (flac->Open()) return flac;
    } else if (ext == ".wav") {
        auto wav = std::make_unique<Wav>(std::move(data));
        if (wav->Open()) return wav;
    }
    LOGW("radio: %s is not a playable MP3, OGG Vorbis, FLAC or WAV file", reinterpret_cast<const char*>(path.filename().u8string().c_str()));
    return nullptr;
}

} // namespace game
