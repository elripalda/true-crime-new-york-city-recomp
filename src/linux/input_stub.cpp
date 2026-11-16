// Linux/headless stand-ins for the mouse-aim half of core/input.cpp.
// The native SDL3 host supplies the real keyboard/mouse state; the harness
// only needs the symbols the game's camera hooks reference.
#ifdef __linux__

#include <cstdint>

#include "core/input.h"

namespace input {

uint32_t g_mouse_sensitivity_percent = 100;
uint32_t g_mouse_vertical_sensitivity_percent = 100;
uint32_t g_controller_aim_response = 0;
uint32_t g_controller_aim_sensitivity_percent = 100;
uint32_t g_reload_key = 'R', g_heavy_attack_key = 0, g_use_key = 'E', g_climb_key = 'G';
bool g_direct_mouse_aim = true;

void UseDirectMouseAim() {}

bool ConsumeMouseAim(float&, float&) {
    return false;
}

bool DirectAimRecent() {
    return false;
}

bool MouseMovedRecently() { return false; }

bool MouseLookStickActive() { return false; }

bool CanBindKey(uint32_t) { return false; }
void LoadBindings(const void*) {}
void SaveBindings(void*) {}
uint32_t BindingGeneration() { return 1; }
void KeyDisplayName(uint32_t, char (&out)[32]) { out[0] = 0; }
void PadSourceDisplayName(int, char (&out)[32]) { out[0] = 0; }
int HeldPadSource() { return -1; }
bool PadRemapped() { return false; }
void PromptLabel(PadInput, bool, char (&out)[8]) { out[0] = '-', out[1] = 0; }
void KeyBindings(KeyBinding (&)[kKeyBindingCount]) {}
void PadBindings(PadBinding (&)[kPadBindingCount]) {}
void DefaultPadBindings(PadBinding (&)[kPadBindingCount]) {}

bool KeyboardMouseActive() {
    return false;
}

} // namespace input

#endif
