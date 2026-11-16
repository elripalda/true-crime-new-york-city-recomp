// One conversion from ImGui's named keys to SDL keycodes. The in-game Controls
// editor uses this, and the launcher receives the same SDL keycodes from its
// events, so both bind the same set of keys. No ImGui headers: the values are
// ImGuiKey enum numbers, so the mapping stays testable and platform-neutral.
#pragma once
#include <cstdint>

namespace common::keys {

// ImGuiKey enum values (imgui.h, 1.91). Keep in sync with the enum.
enum ImGuiKeyValue : int {
    kImGuiKey_None = 0,
    kImGuiKey_Tab = 512,
    kImGuiKey_LeftArrow,
    kImGuiKey_RightArrow,
    kImGuiKey_UpArrow,
    kImGuiKey_DownArrow,
    kImGuiKey_PageUp,
    kImGuiKey_PageDown,
    kImGuiKey_Home,
    kImGuiKey_End,
    kImGuiKey_Insert,
    kImGuiKey_Delete,
    kImGuiKey_Backspace,
    kImGuiKey_Space,
    kImGuiKey_Enter,
    kImGuiKey_Escape,
    kImGuiKey_LeftCtrl,
    kImGuiKey_LeftShift,
    kImGuiKey_LeftAlt,
    kImGuiKey_LeftSuper,
    kImGuiKey_RightCtrl,
    kImGuiKey_RightShift,
    kImGuiKey_RightAlt,
    kImGuiKey_RightSuper,
    kImGuiKey_Menu,
    kImGuiKey_0,
    kImGuiKey_1,
    kImGuiKey_2,
    kImGuiKey_3,
    kImGuiKey_4,
    kImGuiKey_5,
    kImGuiKey_6,
    kImGuiKey_7,
    kImGuiKey_8,
    kImGuiKey_9,
    kImGuiKey_A,
    kImGuiKey_B,
    kImGuiKey_C,
    kImGuiKey_D,
    kImGuiKey_E,
    kImGuiKey_F,
    kImGuiKey_G,
    kImGuiKey_H,
    kImGuiKey_I,
    kImGuiKey_J,
    kImGuiKey_K,
    kImGuiKey_L,
    kImGuiKey_M,
    kImGuiKey_N,
    kImGuiKey_O,
    kImGuiKey_P,
    kImGuiKey_Q,
    kImGuiKey_R,
    kImGuiKey_S,
    kImGuiKey_T,
    kImGuiKey_U,
    kImGuiKey_V,
    kImGuiKey_W,
    kImGuiKey_X,
    kImGuiKey_Y,
    kImGuiKey_Z,
    kImGuiKey_F1,
    kImGuiKey_F2,
    kImGuiKey_F3,
    kImGuiKey_F4,
    kImGuiKey_F5,
    kImGuiKey_F6,
    kImGuiKey_F7,
    kImGuiKey_F8,
    kImGuiKey_F9,
    kImGuiKey_F10,
    kImGuiKey_F11,
    kImGuiKey_F12,
    kImGuiKey_F13,
    kImGuiKey_F14,
    kImGuiKey_F15,
    kImGuiKey_F16,
    kImGuiKey_F17,
    kImGuiKey_F18,
    kImGuiKey_F19,
    kImGuiKey_F20,
    kImGuiKey_F21,
    kImGuiKey_F22,
    kImGuiKey_F23,
    kImGuiKey_F24,
    kImGuiKey_Apostrophe,
    kImGuiKey_Comma,
    kImGuiKey_Minus,
    kImGuiKey_Period,
    kImGuiKey_Slash,
    kImGuiKey_Semicolon,
    kImGuiKey_Equal,
    kImGuiKey_LeftBracket,
    kImGuiKey_Backslash,
    kImGuiKey_RightBracket,
    kImGuiKey_Grave,
    kImGuiKey_CapsLock,
    kImGuiKey_ScrollLock,
    kImGuiKey_NumLock,
    kImGuiKey_PrintScreen,
    kImGuiKey_Pause,
    kImGuiKey_Keypad0,
    kImGuiKey_Keypad1,
    kImGuiKey_Keypad2,
    kImGuiKey_Keypad3,
    kImGuiKey_Keypad4,
    kImGuiKey_Keypad5,
    kImGuiKey_Keypad6,
    kImGuiKey_Keypad7,
    kImGuiKey_Keypad8,
    kImGuiKey_Keypad9,
    kImGuiKey_KeypadDecimal,
    kImGuiKey_KeypadDivide,
    kImGuiKey_KeypadMultiply,
    kImGuiKey_KeypadSubtract,
    kImGuiKey_KeypadAdd,
    kImGuiKey_KeypadEnter,
    kImGuiKey_KeypadEqual,
    kImGuiKey_AppBack,
    kImGuiKey_AppForward,
    kImGuiKey_Oem102,
};

inline constexpr uint32_t SdlKeycode(int key) {
    if (key >= kImGuiKey_A && key <= kImGuiKey_Z) return uint32_t('a' + (key - kImGuiKey_A));
    if (key >= kImGuiKey_0 && key <= kImGuiKey_9) return uint32_t('0' + (key - kImGuiKey_0));
    if (key >= kImGuiKey_F1 && key <= kImGuiKey_F24)
        return 0x4000003Au + uint32_t(key - kImGuiKey_F1);  // SDLK_F1..SDLK_F24
    switch (key) {
    case kImGuiKey_Tab: return '\t';
    case kImGuiKey_LeftArrow: return 0x40000050u;   // SDLK_LEFT
    case kImGuiKey_RightArrow: return 0x4000004Fu;  // SDLK_RIGHT
    case kImGuiKey_UpArrow: return 0x40000052u;     // SDLK_UP
    case kImGuiKey_DownArrow: return 0x40000051u;   // SDLK_DOWN
    case kImGuiKey_PageUp: return 0x4000004Bu;
    case kImGuiKey_PageDown: return 0x4000004Eu;
    case kImGuiKey_Home: return 0x4000004Au;
    case kImGuiKey_End: return 0x4000004Du;
    case kImGuiKey_Insert: return 0x40000049u;
    case kImGuiKey_Delete: return 0x7Fu;
    case kImGuiKey_Backspace: return 0x08u;
    case kImGuiKey_Space: return 0x20u;
    case kImGuiKey_Enter: return 0x0Du;
    case kImGuiKey_Escape: return 0x1Bu;
    case kImGuiKey_LeftCtrl: return 0x400000E0u;
    case kImGuiKey_LeftShift: return 0x400000E1u;
    case kImGuiKey_LeftAlt: return 0x400000E2u;
    case kImGuiKey_LeftSuper: return 0x400000E3u;
    case kImGuiKey_RightCtrl: return 0x400000E4u;
    case kImGuiKey_RightShift: return 0x400000E5u;
    case kImGuiKey_RightAlt: return 0x400000E6u;
    case kImGuiKey_RightSuper: return 0x400000E7u;
    case kImGuiKey_Menu: return 0x40000065u;
    case kImGuiKey_Apostrophe: return '\'';
    case kImGuiKey_Comma: return ',';
    case kImGuiKey_Minus: return '-';
    case kImGuiKey_Period: return '.';
    case kImGuiKey_Slash: return '/';
    case kImGuiKey_Semicolon: return ';';
    case kImGuiKey_Equal: return '=';
    case kImGuiKey_LeftBracket: return '[';
    case kImGuiKey_Backslash: return '\\';
    case kImGuiKey_RightBracket: return ']';
    case kImGuiKey_Grave: return '`';
    case kImGuiKey_CapsLock: return 0x40000039u;
    case kImGuiKey_ScrollLock: return 0x40000047u;
    case kImGuiKey_NumLock: return 0x40000053u;
    case kImGuiKey_PrintScreen: return 0x40000046u;
    case kImGuiKey_Pause: return 0x40000048u;
    case kImGuiKey_Keypad0: return 0x40000062u;
    case kImGuiKey_Keypad1: return 0x4000005Fu;
    case kImGuiKey_Keypad2: return 0x40000060u;
    case kImGuiKey_Keypad3: return 0x40000061u;
    case kImGuiKey_Keypad4: return 0x4000005Cu;
    case kImGuiKey_Keypad5: return 0x4000005Du;
    case kImGuiKey_Keypad6: return 0x4000005Eu;
    case kImGuiKey_Keypad7: return 0x40000059u;
    case kImGuiKey_Keypad8: return 0x4000005Au;
    case kImGuiKey_Keypad9: return 0x4000005Bu;
    case kImGuiKey_KeypadDecimal: return 0x40000063u;
    case kImGuiKey_KeypadDivide: return 0x40000054u;
    case kImGuiKey_KeypadMultiply: return 0x40000055u;
    case kImGuiKey_KeypadSubtract: return 0x40000056u;
    case kImGuiKey_KeypadAdd: return 0x40000057u;
    case kImGuiKey_KeypadEnter: return 0x40000058u;
    case kImGuiKey_KeypadEqual: return 0x40000067u;
    default: return 0;
    }
}

}  // namespace common::keys
