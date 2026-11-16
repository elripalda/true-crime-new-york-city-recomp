#pragma once

#include <filesystem>
#include <toml++/toml.hpp>

namespace app {

struct Settings;
void LoadOcclusionSettings(Settings& settings, const toml::table& table);
toml::table ReadSettingsTable(const std::filesystem::path& path);
// Applies edits to the file under a cross-process lock. A file that does not parse is renamed to
// <name>.corrupt-<time> and rebuilt from `full` (the complete settings, when the caller has them) plus
// the edits, so one bad file does not make every later save fail.
void MergeSettingsTable(const std::filesystem::path& path, const toml::table& edits,
                        const toml::table* full = nullptr);
toml::table ChangedSettings(const toml::table& previous, const toml::table& current);

} // namespace app
