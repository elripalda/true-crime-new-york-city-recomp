#pragma once

#include <filesystem>

namespace launcher {

// Returns false when the player closes the launcher without starting the game.
bool Run(std::filesystem::path &location);

} // namespace launcher
