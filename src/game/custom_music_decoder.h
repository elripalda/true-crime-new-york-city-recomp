#pragma once
// Host decoders for MP3 and OGG Vorbis radio replacements. The game only has a
// WMA decoder, so these songs are decoded here and handed to it as 16-bit PCM.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace game {

class MusicDecoder {
public:
    virtual ~MusicDecoder() = default;
    uint32_t rate = 0;
    uint32_t channels = 0;    // 1 or 2
    uint64_t frames = 0;      // total length, 0 when unknown
    // Fills up to max_frames interleaved frames; returns the count, 0 at the end.
    virtual size_t Read(int16_t* out, size_t max_frames) = 0;
    virtual bool Rewind() = 0;
};

// extra_songs.cpp: the custom music folder (empty when none), the file for an
// extra song's track number (6093 and up), and the last track number in use.
std::filesystem::path MusicFolder();
std::filesystem::path ExtraSongPath(uint32_t track);
uint32_t LastMusicTrack();

// Opens <path> by extension (.mp3 or .ogg). Returns null and logs on failure.
std::unique_ptr<MusicDecoder> OpenMusicDecoder(const std::filesystem::path& path);

} // namespace game
