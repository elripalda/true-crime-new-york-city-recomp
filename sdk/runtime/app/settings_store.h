#pragma once

#include <filesystem>
#include <toml++/toml.hpp>

namespace app {

struct Settings;
void LoadOcclusionSettings(Settings& settings, const toml::table& table);
// A sample count for the legacy `msaa` key: true was 4x, false off, and an
// integer must be 2, 4 or 8. A missing key returns `fallback`.
uint32_t MsaaSamplesFromTable(const toml::table& table, uint32_t fallback);
toml::table ReadSettingsTable(const std::filesystem::path& path);
// Applies edits to the file under a cross-process lock. A file that does not parse is renamed to
// <name>.corrupt-<time> and rebuilt from `full` (the complete settings, when the caller has them) plus
// the edits, so one bad file does not make every later save fail.
void MergeSettingsTable(const std::filesystem::path& path, const toml::table& edits,
                        const toml::table* full = nullptr);
// Queues one merge+publish on the settings worker, so the UI thread never waits
// for the cross-process file lock. FlushSettings waits for the queue to drain.
void PublishSettingsAsync(std::filesystem::path path, toml::table edits, toml::table full);
void FlushSettings();
toml::table ChangedSettings(const toml::table& previous, const toml::table& current);

} // namespace app
