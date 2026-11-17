#pragma once

#include "app/game_source.h"

namespace launcher {

// Returns false when the player closes the launcher without starting the game.
bool Run(app::GameSource& source);
bool LaunchPal(const app::GameSource& source);
bool PalInstalled(app::GameRegion region);

} // namespace launcher
