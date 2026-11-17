#pragma once

#include <filesystem>
#include <string_view>

namespace app {

enum class GameRegion { Unknown, Ntsc, Pal, PalEu };

// The PAL game executables ship in this subfolder of the install; the launcher in the install folder starts them.
inline constexpr char kRegionsFolder[] = "regions";
inline std::filesystem::path InstallRootFor(const std::filesystem::path& exe_folder) {
    return exe_folder.filename() == kRegionsFolder ? exe_folder.parent_path() : exe_folder;
}
constexpr bool IsPal(GameRegion region) { return region == GameRegion::Pal || region == GameRegion::PalEu; }

// The Xbox executables Beta 4 supports. They are public identifiers, not game
// data, so a launcher built without the PAL images beside it can still name the
// disc a player picks.
inline constexpr std::string_view kNtscImageSha1 = "0ca011f37a49a6f372012c4a5103951ff955af66";
inline constexpr std::string_view kPalImageSha1 = "bd30774d7048a9129b1776e626c75480658d84d8";
inline constexpr std::string_view kPalEuImageSha1 = "569ddfb481d929f55a4e37f0322b4f6f59baadff";

constexpr GameRegion RegionForImageSha1(std::string_view sha1) {
    if (sha1 == kNtscImageSha1) return GameRegion::Ntsc;
    if (sha1 == kPalImageSha1) return GameRegion::Pal;
    if (sha1 == kPalEuImageSha1) return GameRegion::PalEu;
    return GameRegion::Unknown;
}

constexpr GameRegion BuildRegion() {
#if TCNYC_PAL_EU
    return GameRegion::PalEu;
#elif TCNYC_PAL
    return GameRegion::Pal;
#else
    return GameRegion::Ntsc;
#endif
}

// The NTSC launcher can hand either PAL disc to its PAL executable. A PAL build
// only carries its own game code, so it may only start its own disc.
constexpr bool CanLaunchRegion(GameRegion own, GameRegion target) {
    return target != GameRegion::Unknown && (own == GameRegion::Ntsc || own == target);
}

struct GameSource {
    std::filesystem::path path;
    GameRegion region = GameRegion::Unknown;
};
GameSource DetectGameSource(const std::filesystem::path& path, bool prefer_pal = false);

} // namespace app
