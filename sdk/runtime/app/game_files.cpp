// The game in assets: its files, or a disc image (.iso) with this executable's xbe on it. The xbe must be the one
// this executable was recompiled from; another release's would run its own data under this code.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // the xbe recompiled into this executable, written by CMake

namespace app {
namespace {
GameRegion settings_region = GameRegion::Unknown;
}

const char* SettingsFile() {
    if (settings_region == GameRegion::PalEu) return "settings-pal-eu.toml";
    if (settings_region == GameRegion::Pal) return "settings-pal.toml";
    return kGame.settings;
}

void SelectGameSettings(GameRegion region) {
    settings_region = region;
}

std::string HddName() {
#if TCNYC_PAL_EU
    std::string name = "hdd-pal-eu";
#elif defined(TCNYC_PAL)
    std::string name = "hdd-pal";
#else
    std::string name = "hdd";
#endif
    if (Instance() > 1) name += std::to_string(Instance());
    return name;
}


namespace {

namespace fs = std::filesystem;

#ifdef TCNYC_PAL_EU
constexpr char kPalAssets[] = "assets-pal-eu";
#else
constexpr char kPalAssets[] = "assets-pal";
#endif

// assets beside the exe or in any folder above it (the build output)
fs::path FindAssets(const char* name = "assets") {
    auto dir = ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / name, ec)) return dir / name;
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

std::string Narrow(const wchar_t* text) {
    std::string out;
    for (; *text; ++text) out += *text < 128 ? char(*text) : '?';
    return out;
}

void Alert(const wchar_t* text) {
#ifdef _WIN32
    MessageBoxW(nullptr, text, kGame.short_title, MB_ICONERROR);
#else
    LOGE("game: %s", Narrow(text).c_str());
#endif
}

std::string PathText(const fs::path& path) {
    const auto utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    constexpr uint64_t kMaxXbeBytes = 64ull << 20;
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size > kMaxXbeBytes) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.resize(size_t(size));
    return f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())).good();
}

bool IsIso(const fs::path& path) {
    auto extension = PathText(path.extension());
    std::ranges::transform(extension, extension.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return extension == ".iso";
}

std::vector<fs::path> Candidates(const fs::path& source) {
    std::error_code ec;
    if (IsIso(source) && fs::is_regular_file(source, ec)) return {source};
    if (!fs::is_directory(source, ec)) return {};
    std::vector<fs::path> result{source};
    for (fs::directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code status;
        if (IsIso(it->path()) && it->is_regular_file(status)) result.push_back(it->path());
    }
    std::sort(result.begin() + 1, result.end());
    return result;
}

bool ReadSource(const fs::path& name, std::vector<uint8_t>& xbe, std::unique_ptr<kernel::Disc>& disc) {
    std::error_code ec;
    if (fs::is_directory(name, ec)) return ReadHostFile(name / IMAGE_XBE, xbe);
    disc = std::make_unique<kernel::Disc>();
    std::string reason;
    return disc->Open(name, reason) && disc->Load(IMAGE_XBE, xbe) && disc->StillAtPath();
}

}  // namespace

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

GameSource DetectGameSource(const fs::path& location, bool prefer_pal) {
    auto candidates = Candidates(location.empty() ? FindAssets() : location);
    if (location.empty()) {
        auto pal_candidates = Candidates(FindAssets(kPalAssets));
        candidates.insert(candidates.end(), pal_candidates.begin(), pal_candidates.end());
#ifndef TCNYC_PAL
        auto eu_candidates = Candidates(FindAssets("assets-pal-eu"));
        candidates.insert(candidates.end(), eu_candidates.begin(), eu_candidates.end());
#endif
    }
    GameSource first;
    for (const auto& candidate : candidates) {
        std::vector<uint8_t> xbe;
        std::unique_ptr<kernel::Disc> disc;
        std::error_code ec;
        const auto path = fs::canonical(candidate, ec);
        if (ec || !ReadSource(path, xbe, disc)) continue;
        const auto hash = Sha1(xbe);
        GameRegion region = GameRegion::Unknown;
#if TCNYC_PAL_EU
        if (hash == kImageSha1) region = GameRegion::PalEu;
#elif defined(TCNYC_PAL)
        if (hash == kImageSha1) region = GameRegion::Pal;
#else
        if (hash == kImageSha1) region = GameRegion::Ntsc;
#ifdef TCNYC_PAL_SHA1
        if (hash == TCNYC_PAL_SHA1) region = GameRegion::Pal;
#endif
#ifdef TCNYC_PAL_EU_SHA1
        if (hash == TCNYC_PAL_EU_SHA1) region = GameRegion::PalEu;
#endif
#endif
        if (region == GameRegion::Unknown) continue;
        if (first.region == GameRegion::Unknown) first = {path, region};
        if (!location.empty() || IsPal(region) == prefer_pal) return {path, region};
    }
    return first;
}

bool FindGame(GameFiles& game, std::string* error, const fs::path& location) {
    auto candidates = Candidates(location.empty() ? FindAssets() : location);
#ifdef TCNYC_PAL
    if (location.empty()) {
        auto pal_candidates = Candidates(FindAssets(kPalAssets));
        candidates.insert(candidates.end(), pal_candidates.begin(), pal_candidates.end());
    }
#endif
    bool wrong_image = false;
    for (const auto& candidate : candidates) {
        std::vector<uint8_t> xbe;
        std::unique_ptr<kernel::Disc> disc;
        if (!ReadSource(candidate, xbe, disc)) continue;
        if (Sha1(xbe) != kImageSha1) {
            wrong_image = true;
            continue;
        }
        game = {disc ? candidate.parent_path() : candidate, std::move(xbe)};
        kernel::MountDisc(std::move(disc));
        return true;
    }

    if (wrong_image) {
        constexpr wchar_t message[] = L"This Xbox release does not match the selected game executable.";
        Alert(message);
        if (error) *error = Narrow(message);
    } else {
        Alert(kGame.missing);
        if (error) *error = Narrow(kGame.missing);
    }
    return false;
}

}  // namespace app
