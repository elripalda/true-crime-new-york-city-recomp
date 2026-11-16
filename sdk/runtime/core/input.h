#pragma once
#include <cstdint>

namespace input {

// Bindings: every keyboard key, mouse button and controller input reaches the
// game as one of these console pad inputs. The button prompts show whatever is
// bound to the pad input each glyph stands for. See docs/rebinding-plan.md.
enum class PadInput : uint8_t {
    Up, Down, Left, Right, Start, Back, LS, RS,  // Pad::buttons bits 0-7
    A, B, X, Y, Black, White, LT, RT,            // Pad::analog 0-7
    MoveUp, MoveDown, MoveLeft, MoveRight,       // keyboard only: the left stick
    Count
};
// keyboard layouts: on foot and in a vehicle; menu keys (Enter, Backspace,
// arrows, Escape) are fixed and always on, so no binding can lock out the menus
enum class Context : uint8_t { Any, OnFoot, Driving };
// a key is an SDL keycode (layout-aware letters, as before), or a mouse button
inline constexpr uint32_t kMouseKey = 0x10000000;  // | 0 left, 1 middle, 2 right, 3 side 1, 4 side 2
inline constexpr uint32_t MouseKey(int button) { return kMouseKey | uint32_t(button); }
struct KeyBinding {
    PadInput target;
    Context context;
    uint32_t keys[4];  // any of them held presses the target; 0 unused
};
// a controller source: an SDL_GamepadButton, or kPadAxis | SDL_GamepadAxis (the triggers)
inline constexpr int kPadAxis = 0x100;
struct PadBinding {
    PadInput target;  // Up..RT
    int source;
};
inline constexpr int kKeyBindingCount = 20;
inline constexpr int kPadBindingCount = 16;
void KeyBindings(KeyBinding (&out)[kKeyBindingCount]);
// settings.toml [controls]: keyboard actions by name, controller inputs by SDL name;
// without it, today's layout with the old reload/heavy/use/climb letter keys
void DefaultPadBindings(PadBinding (&out)[kPadBindingCount]);
void DefaultKeys(KeyBinding (&out)[kKeyBindingCount]);  // today's layout with the letter-key settings
// replaces both tables (the launcher's Controls tab); SaveBindings then writes them
void SetBindings(const KeyBinding (&keys)[kKeyBindingCount], const PadBinding (&pad)[kPadBindingCount]);
// changes whenever the bindings do, so the renderers re-bake the prompt glyphs
uint32_t BindingGeneration();
// display names for the menus
void KeyDisplayName(uint32_t key, char (&out)[32]);
void PadSourceDisplayName(int source, char (&out)[32]);
// the first physical input held on controller 1 (a PadBinding source), -1 when none
int HeldPadSource();
// true when any controller input is moved from its default
bool PadRemapped();
// the keyboard prompt for a pad input in the current context: up to 4 characters, "-" when unbound;
// MoveUp gives the movement keys (e.g. "WASD")
void PromptLabel(PadInput target, bool driving, char (&out)[8]);
bool CanBindKey(uint32_t key);
void LoadBindings(const void* toml_table);
void SaveBindings(void* toml_table);
void PadBindings(PadBinding (&out)[kPadBindingCount]);

// the console controller's report, analog buttons are 0-255
struct Pad {
    // compare reports by value; padding is not part of the state
    bool operator==(const Pad&) const = default;
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

// KeyboardDriving: keyboard prompts with the in-vehicle pedal keys
enum class PromptStyle { Original, PlayStation, Xbox, KeyboardMouse, KeyboardDriving, SteamDeck };
PromptStyle ControllerPromptStyle();
// true while keyboard/mouse is the input the game is following; a pad takes
// over as soon as it is used
// port 0 holds the left trigger (aim / lock-on)
bool LockOnHeld();
bool KeyboardMouseActive();
// While a lock-on target is active, the game's own aim assist needs a small
// right-stick deflection to run. Direct mouse aim otherwise leaves the stick
// centred, which skips that pass.
void SetLockOnStick(int16_t x, int16_t y);
// keyboard keys that stand in for the two stick clicks, shared by input and
// the keyboard-mouse prompt glyphs
inline constexpr char kLeftStickClickKey = 'C';
inline constexpr char kRightStickClickKey = 'V';
inline char LetterKey(uint32_t value, char fallback) {
    return value >= 'A' && value <= 'Z' ? char(value) : fallback;
}
extern uint32_t g_reload_key, g_heavy_attack_key, g_use_key, g_climb_key;
extern uint32_t g_mouse_sensitivity_percent;
// optional separate vertical scale for direct aim; 100 matches the PC raw path
extern uint32_t g_mouse_vertical_sensitivity_percent;
// gameplay camera right-stick response: 0 refined (radial dead zone + quadratic),
// 1 original Xbox (per-axis dead zone + cubic)
extern uint32_t g_controller_aim_response;
// applied after the refined curve; 100 is the original full-stick turn speed
extern uint32_t g_controller_aim_sensitivity_percent;
// when set, mouse motion is not folded into the right stick: the gameplay
// camera reads the same pixels as an angle, see src/game/mouse_aim.cpp
extern bool g_direct_mouse_aim;
// settings.toml prompt_style: 0 automatic, 1 original, 2 Xbox, 3 PlayStation, 4 Steam Deck
extern uint32_t g_prompt_override;
// pixels of mouse motion since the last call, false when nothing is pending;
// the angle applies to the gameplay camera instead of the 8-bit stick
bool ConsumeMouseAim(float& dx, float& dy);
// heartbeat from the direct-aim camera controller, per frame; while it is
// fresh the mouse does not also feed the right stick
void UseDirectMouseAim();
// a direct sample was applied recently (the PC fix stops its direct path after 5 s idle)
bool DirectAimRecent();
// the right stick the game is reading comes from the mouse fallback, not a pad
bool MouseLookStickActive();
// the mouse moved within the last 100 ms and no overlay owns the pointer
bool MouseMovedRecently();
// an overlay (the Graphics menu) owns the pointer: no look input, no direct aim
void SetUiActive(bool active);
bool UiActive();
void RegisterMouse(void* window);
void ReadMouse(void* raw_input);
// SDL hosts: relative motion and wheel notches from the event loop
void ReadMouseMotion(float dx, float dy);
void ReadMouseWheel(float steps);
// before the host's first SDL_Init: joystick backend hints
void SetHints();
// window thread: hot-plug notices from the host's event loop
void GamepadAdded(int32_t id);
void GamepadRemoved(int32_t id);
void Init();  // window thread, before boot; the game code window starts it earlier
bool Connected(int port);
uint16_t HeldOnAnyController();  // face buttons as XINPUT_GAMEPAD_A/B/X/Y masks, GameInfo::pad left out
// the controller on that port, then GameInfo::pad
Pad Poll(int port);
void Rumble(int port, uint16_t left, uint16_t right);

}  // namespace input
