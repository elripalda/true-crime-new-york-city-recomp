// Rules for the aim rows added to the native Options > Controller tab
// (native_graphics.cpp): which rows are the game's, cursor movement, value
// changes and row text. No runtime dependencies, so a test can compile it.
//
// The tab's five native rows are ROTARY RADAR, VIBRATION, VERT CAM, HORIZ CAM
// and CONTROLLER INFO. Four rows follow them: the right-stick response and
// speed, and the mouse speeds. The tab's frame holds five rows, so the slots
// show a window of the nine that scrolls with the cursor, as on the Display tab.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <span>

#include "common/graphics_choices.h"

namespace game::controller_rows {

inline constexpr uint32_t kNativeRows = 5;  // the game's own rows, logical rows 0 to 4
inline constexpr uint32_t kShownRows = 5;   // slots in the tab's frame
inline constexpr size_t kTextBytes = 64;
inline constexpr size_t kMaxTextChars = 19;  // as the Display tab's rows

// dispatcher input bits (event 9), as display_rows.h
inline constexpr uint32_t kUp = 0x01, kDown = 0x02, kLeft = 0x04, kRight = 0x08, kBack = 0x20, kAccept = 0x100,
                          kCancel = 0x200;

enum class Option { Response, StickSpeed, MouseSpeed, MouseVertical, Count };
inline constexpr uint32_t kRowCount = kNativeRows + uint32_t(Option::Count);

struct Values {
    uint32_t response = 0;  // controller_aim_response: 0 Refined, 1 Original
    uint32_t stick = 100;   // controller_aim_sensitivity_percent
    uint32_t mouse = 100;   // mouse_sensitivity_percent
    uint32_t vertical = 100;  // mouse_vertical_sensitivity_percent
};

inline bool Native(uint32_t row) { return row < kNativeRows; }
inline Option OptionAt(uint32_t row) { return row >= kNativeRows && row < kRowCount ? Option(row - kNativeRows) : Option::Count; }

// stick speed only applies to the refined response
inline bool Enabled(const Values &v, Option option) { return option != Option::StickSpeed || v.response == 0; }

// the first logical row shown in the top slot, keeping row visible
inline uint32_t Scroll(uint32_t row, uint32_t first) {
    if (row < first) return row;
    if (row >= first + kShownRows) return row + 1 - kShownRows;
    return first;
}

inline uint32_t Clamped(uint32_t value, uint32_t low, uint32_t high) { return std::clamp(value, low, high); }

inline void Text(const Values &v, Option option, char (&text)[kTextBytes]) {
    const auto speed = [&](const char *label, uint32_t percent) {
        std::snprintf(text, kTextBytes, "%s %u.%02uX", label, percent / 100, percent % 100);
    };
    switch (option) {
    case Option::Response:
        std::snprintf(text, kTextBytes, "STICK AIM %s", v.response ? "ORIGINAL" : "REFINED");
        break;
    case Option::StickSpeed:
        speed("STICK SPEED", Clamped(v.stick, choices::kControllerAimSensitivities[0],
                                     choices::kControllerAimSensitivities[std::size(choices::kControllerAimSensitivities) - 1]));
        break;
    case Option::MouseSpeed:
        speed("MOUSE SPEED",
              Clamped(v.mouse, choices::kMinMouseSensitivityPercent, choices::kMaxMouseSensitivityPercent));
        break;
    case Option::MouseVertical:
        speed("MOUSE VERT",
              Clamped(v.vertical, choices::kMinMouseSensitivityPercent, choices::kMaxMouseSensitivityPercent));
        break;
    default:
        text[0] = 0;
        break;
    }
}

// one step left (-1) or right (+1); false if nothing changed
inline bool Change(Values &v, Option option, int direction) {
    if (!Enabled(v, option)) return false;
    const auto step = [&](uint32_t &value, std::span<const uint32_t> list, uint32_t low, uint32_t high) {
        const uint32_t current = Clamped(value, low, high);
        const uint32_t next = choices::Step(list, current, direction);
        const bool changed = next != value;
        value = next;
        return changed;
    };
    switch (option) {
    case Option::Response:  // two choices: any step toggles
        v.response = v.response ? 0 : 1;
        return true;
    case Option::StickSpeed:
        return step(v.stick, choices::kControllerAimSensitivities, choices::kControllerAimSensitivities[0],
                    choices::kControllerAimSensitivities[std::size(choices::kControllerAimSensitivities) - 1]);
    case Option::MouseSpeed:
        return step(v.mouse, choices::kMouseSensitivities, choices::kMinMouseSensitivityPercent,
                    choices::kMaxMouseSensitivityPercent);
    case Option::MouseVertical:
        return step(v.vertical, choices::kMouseSensitivities, choices::kMinMouseSensitivityPercent,
                    choices::kMaxMouseSensitivityPercent);
    default:
        return false;
    }
}

// What one input does on the row list. handled: the port moved the cursor or
// changed a value itself. Otherwise the game's dispatcher takes the input with
// the tab's row field set to the logical row: a native row's own action, or Up
// on the first row (the cursor goes to the tab bar).
struct Step {
    bool handled = false;
    uint32_t row = 0;
    int change = 0;  // -1 or +1 on an added row
    bool leave = false;  // Back/Cancel: the game leaves the row list, from any row
};

inline Step Input(uint32_t row, uint32_t input) {
    Step step;
    step.row = row;
    if (input & (kBack | kCancel)) {
        step.leave = true;  // the game's own Back, whatever row the cursor is on
        return step;
    }
    if (input & kUp) {
        if (row == 0) return step;  // to the tab bar, as the game does
        step.handled = true;
        step.row = row - 1;
        return step;
    }
    if (input & kDown) {
        step.handled = true;
        step.row = std::min(row + 1, kRowCount - 1);
        return step;
    }
    if (Native(row)) return step;  // the game's own row action
    step.handled = true;  // every other input on an added row stays with the port
    if (input & kLeft) step.change = -1;
    else if (input & (kRight | kAccept)) step.change = 1;
    return step;
}

} // namespace game::controller_rows
