#pragma once

#include <filesystem>

namespace app {

enum class GameRegion { Unknown, Ntsc, Pal, PalEu };
constexpr bool IsPal(GameRegion region) { return region == GameRegion::Pal || region == GameRegion::PalEu; }

struct GameSource {
    std::filesystem::path path;
    GameRegion region = GameRegion::Unknown;
};
GameSource DetectGameSource(const std::filesystem::path& path, bool prefer_pal = false);

} // namespace app
