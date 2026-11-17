#include "app/game_source.h"

int main() {
    using app::GameRegion;
    using app::CanLaunchRegion;
    using app::RegionForImageSha1;

    // The hashes are the supported Xbox executables named by the launcher.
    if (RegionForImageSha1(app::kNtscImageSha1) != GameRegion::Ntsc) return 1;
    if (RegionForImageSha1(app::kPalImageSha1) != GameRegion::Pal) return 2;
    if (RegionForImageSha1(app::kPalEuImageSha1) != GameRegion::PalEu) return 3;
    if (RegionForImageSha1("0123456789abcdef0123456789abcdef01234567") != GameRegion::Unknown) return 4;

    // The NTSC launcher can start either PAL executable; each PAL build stays
    // with the disc whose code it contains.
    if (!CanLaunchRegion(GameRegion::Ntsc, GameRegion::Ntsc)) return 5;
    if (!CanLaunchRegion(GameRegion::Ntsc, GameRegion::Pal)) return 6;
    if (!CanLaunchRegion(GameRegion::Ntsc, GameRegion::PalEu)) return 7;
    if (!CanLaunchRegion(GameRegion::Pal, GameRegion::Pal)) return 8;
    if (CanLaunchRegion(GameRegion::Pal, GameRegion::Ntsc)) return 9;
    if (CanLaunchRegion(GameRegion::Pal, GameRegion::PalEu)) return 10;
    if (!CanLaunchRegion(GameRegion::PalEu, GameRegion::PalEu)) return 11;
    if (CanLaunchRegion(GameRegion::PalEu, GameRegion::Pal)) return 12;
    if (CanLaunchRegion(GameRegion::Ntsc, GameRegion::Unknown)) return 13;

    return 0;
}
