#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>

#include "app/app.h"
#include "app/settings_store.h"

int main() {
    using namespace app;
    namespace fs = std::filesystem;
    const auto tag = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = fs::current_path() / ("settings-test-" + std::to_string(tag));
    const auto file = dir / "settings.toml";
    int result = 1;
    try {
        const toml::table initial{{"ao", 0}, {"external", true},
            {"controls", toml::table{{"keyboard", toml::table{{"use", toml::array{"E"}},
                                                           {"reload", toml::array{"R"}}}}}}};
        MergeSettingsTable(file, initial);
        auto edited = initial;
        edited["controls"]["keyboard"].as_table()->insert_or_assign("reload", toml::array{"Y"});
        const auto delta = ChangedSettings(initial, edited);
        auto first = std::async(std::launch::async, [&] { MergeSettingsTable(file, delta); });
        MergeSettingsTable(file, toml::table{{"ao", 2}, {"ao_method", 1},
            {"controls", toml::table{{"keyboard", toml::table{{"use", toml::array{"F"}}}}}}});
        first.get();
        const auto saved = ReadSettingsTable(file);
        const auto keyboard = saved["controls"]["keyboard"];
        if (saved["ao"].value_or(0) == 2 && saved["ao_method"].value_or(0) == 1 &&
            saved["external"].value_or(false) && keyboard["use"][0].value_or(std::string("")).compare("F") == 0 &&
            keyboard["reload"][0].value_or(std::string("")).compare("Y") == 0) result = 0;
        Settings settings;
        LoadOcclusionSettings(settings, toml::table{{"ao", true}});
        if (settings.ao != 2 || settings.ao_quality != 2) result = 1;
        LoadOcclusionSettings(settings, toml::table{{"ao", 2}, {"ao_method", 1}, {"ao_quality", 1}});
        if (settings.ao != 1 || settings.ao_method != 1 || settings.ao_quality != 1) result = 1;
        LoadOcclusionSettings(settings, toml::table{{"ao", 0}, {"ao_method", 1}, {"ao_quality", 1}});
        if (settings.ao != 0 || settings.ao_method != 1 || settings.ao_quality != 1) result = 1;
        // Legacy msaa = true means 4x; explicit counts are 2, 4 or 8, and a
        // missing key keeps the default.
        if (MsaaSamplesFromTable(toml::table{{"msaa", true}}, 8) != 4) result = 1;
        if (MsaaSamplesFromTable(toml::table{{"msaa", false}}, 8) != 0) result = 1;
        for (const auto samples : {2, 4, 8}) {
            if (MsaaSamplesFromTable(toml::table{{"msaa", samples}}, 0) != uint32_t(samples)) result = 1;
        }
        if (MsaaSamplesFromTable(toml::table{{"msaa", 3}}, 8) != 0) result = 1;
        if (MsaaSamplesFromTable(toml::table{{"msaa", 1}}, 8) != 0) result = 1;
        if (MsaaSamplesFromTable(toml::table{}, 8) != 8) result = 1;
        if (result) std::cerr << saved << '\n';

        // A file that does not parse is moved aside and rebuilt from the full settings plus the edits.
        const auto broken = dir / "broken.toml";
        { std::ofstream(broken, std::ios::binary) << "ao = [unterminated\n"; }
        const toml::table full{{"ao", 1}, {"external", true}};
        MergeSettingsTable(broken, toml::table{{"ao", 2}}, &full);
        const auto rebuilt = ReadSettingsTable(broken);
        int asides = 0;
        for (const auto& entry : fs::directory_iterator(dir))
            if (entry.path().filename().string().starts_with("broken.toml.corrupt-")) ++asides;
        if (rebuilt["ao"].value_or(0) != 2 || !rebuilt["external"].value_or(false) || asides != 1) {
            std::cerr << "corrupt recovery: " << rebuilt << " asides " << asides << '\n';
            result = 1;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    return result;
}
