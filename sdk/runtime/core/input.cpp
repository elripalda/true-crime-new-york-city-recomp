// controllers through SDL: Xbox, PlayStation and Switch pads read into the console controller's report
#include "core/input.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <iterator>
#include <mutex>
#include <utility>

#define NOMINMAX
#ifdef _WIN32
#include <windows.h>
#endif
#include <SDL3/SDL.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"

namespace input {

uint32_t g_mouse_sensitivity_percent = 100;
uint32_t g_mouse_vertical_sensitivity_percent = 100;
uint32_t g_controller_aim_response = 0;
uint32_t g_controller_aim_sensitivity_percent = 100;
// PC defaults: R reload, E use/action, G climb (the port's own action), and no
// configurable heavy key because Left Alt and mouse side button 2 are the defaults.
uint32_t g_reload_key = 'R', g_heavy_attack_key = 0, g_use_key = 'E', g_climb_key = 'G';
bool g_direct_mouse_aim = true;
uint32_t g_prompt_override = 0;
// A small right-stick sample used only while a lock-on target is active; the
// game's aim assist reads the stick and snaps the crosshair onto the target.
std::atomic<int16_t> g_lock_stick_x{0}, g_lock_stick_y{0};
// when the camera hooks last set it: the in-car camera and cutscenes skip those hooks, so a
// sample they stopped refreshing must not keep turning the view
std::atomic<uint64_t> g_lock_stick_at{0};
constexpr uint64_t kLockStickMs = 100;

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
bool g_sdl = false;
SDL_Gamepad* g_pads[4] = {};
std::atomic<PromptStyle> g_prompt_style{PromptStyle::Original};
std::atomic<bool> g_keyboard_mouse_active{false};
// the last poll's right stick came from the mouse fallback, not a controller
std::atomic<bool> g_mouse_look_stick_active{false};
// the player is in a vehicle: the keyboard prompts show the pedal keys
std::atomic<bool> g_keyboard_driving{false};
// until the camera takes a direct sample the mouse keeps the old stick path,
// so an unsupported camera mode never loses aiming
std::atomic<bool> g_direct_confirmed{false};
std::atomic<uint64_t> g_direct_at{0};      // last frame the direct controller ran
std::atomic<uint64_t> g_direct_used_at{0}; // last time the camera took a sample
// the Graphics menu (or another overlay) is using the pointer
std::atomic<bool> g_ui_active{false};
// port 0 is holding the left trigger (aim / lock-on): the right or middle mouse button, or a pad's trigger
std::atomic<bool> g_lock_held{false};

// direct aim is live only while its controller is actually updating; a
// different camera mode falls back to the right stick instead of losing the
// mouse for the rest of the session
bool DirectAimLive(uint64_t now) {
    // a short window: the camera calls this every frame, so the pad fallback
    // should take over quickly when a transition camera stops updating
    return g_direct_mouse_aim && g_direct_confirmed.load(std::memory_order_relaxed) &&
           now - g_direct_at.load(std::memory_order_relaxed) < 100;
}

// Valve's handheld (DMI product "Jupiter" for the LCD model, "Galileo" for the OLED one): its
// built-in controller reaches the game through Steam Input as a generic virtual pad. Other Valve
// hardware has its own product names and keeps the pad's own prompts.
bool RunningOnSteamDeck() {
#ifdef _WIN32
    return false;
#else
    static const bool deck = [] {
        auto read = [](const char* path) {
            std::ifstream file(path);
            std::string line;
            std::getline(file, line);
            return line;
        };
        if (read("/sys/devices/virtual/dmi/id/sys_vendor").rfind("Valve", 0) != 0) return false;
        const std::string product = read("/sys/devices/virtual/dmi/id/product_name");
        return product == "Jupiter" || product == "Galileo";
    }();
    return deck;
#endif
}

void UpdatePromptType() {
    // Steam Input presents a PlayStation pad as a virtual Xbox pad: the real type is
    // then the virtual one, while the mapped type names the physical controller.
    const SDL_GamepadType real = g_pads[0] ? SDL_GetRealGamepadType(g_pads[0]) : SDL_GAMEPAD_TYPE_UNKNOWN;
    const SDL_GamepadType mapped = g_pads[0] ? SDL_GetGamepadType(g_pads[0]) : SDL_GAMEPAD_TYPE_UNKNOWN;
    auto playstation = [](SDL_GamepadType t) {
        return t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
    };
    auto xbox = [](SDL_GamepadType t) { return t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE; };
    // the Deck's own controller: Valve 28DE:1205 when read directly, "Steam Deck" by name
    const char* name = g_pads[0] ? SDL_GetGamepadName(g_pads[0]) : nullptr;
    const bool deck_pad = g_pads[0] && ((SDL_GetGamepadVendor(g_pads[0]) == 0x28DE &&
                                         SDL_GetGamepadProduct(g_pads[0]) == 0x1205) ||
                                        (name && std::strstr(name, "Steam Deck")));
    // a PlayStation pad behind Steam Input can report Xbox types; its name still says what it is
    auto contains = [&](const char* text) { return name && std::strstr(name, text); };
    const bool playstation_name = contains("PS3") || contains("PS4") || contains("PS5") || contains("DualSense") ||
                                  contains("DualShock") || contains("Sony");
    PromptStyle style = PromptStyle::Original;
    if (playstation(real) || playstation(mapped) || playstation_name)
        style = PromptStyle::PlayStation;
    else if (deck_pad || (g_pads[0] && RunningOnSteamDeck() && !xbox(mapped)))
        style = PromptStyle::SteamDeck;
    else if (xbox(real) || xbox(mapped))
        style = PromptStyle::Xbox;
    // Elsewhere (a ROG Ally or Legion Go on SteamOS, a desktop with Steam Input) a pad Steam presents
    // without a type of its own is the handheld's or a generic Xbox-layout controller: Xbox prompts,
    // not the original console glyphs.
    else if (g_pads[0] && SDL_GetGamepadSteamHandle(g_pads[0]) != 0)
        style = PromptStyle::Xbox;
    constexpr PromptStyle kOverrides[] = {PromptStyle::Original, PromptStyle::Original, PromptStyle::Xbox,
                                          PromptStyle::PlayStation, PromptStyle::SteamDeck};
    if (g_prompt_override > 0 && g_prompt_override < 5) style = kOverrides[g_prompt_override];
    // also called while a pad is in use, so only a change is logged
    static std::string last_logged;
    const std::string what = std::string(name ? name : "") + char('0' + int(style));
    g_prompt_style.store(style, std::memory_order_relaxed);
    if (g_pads[0] && what != last_logged && (last_logged = what, true))
        LOGR("input: controller 1 prompts: %s %04X:%04X, real type %d, mapped type %d, steam handle %llu, style %d",
             name ? name : "", SDL_GetGamepadVendor(g_pads[0]), SDL_GetGamepadProduct(g_pads[0]), int(real),
             int(mapped), (unsigned long long)SDL_GetGamepadSteamHandle(g_pads[0]), int(style));
}

// Read relative mouse motion from the window thread and hold the sample for a
// short interval because the guest can poll controller state several times.
std::mutex g_mouse_lock;
int g_mouse_x = 0, g_mouse_y = 0;
uint64_t g_mouse_sample_at = 0;
int16_t g_mouse_rx = 0, g_mouse_ry = 0;
// last fallback stick value, held briefly so a mouse flick is not a one-frame
// pulse to the transition cameras
int16_t g_hold_rx = 0, g_hold_ry = 0;
uint64_t g_hold_at = 0;
// mouse wheel notches waiting to be turned into D-pad taps (weapon select / camera)
int g_wheel_steps = 0;
// one wheel notch is a press then a release, so the game cannot merge two into
// a held D-pad direction
int g_wheel_phase = 0;
// raw counts waiting for the camera, taken straight from WM_INPUT so a fast
// flick is not limited by the right-stick path's small clamp; the camera clears
// them once per frame
float g_aim_x = 0, g_aim_y = 0;
uint64_t g_aim_at = 0; // last raw mouse event
// movement made while no camera is updating (zone load, car exit). It is
// released over the first camera frames after the transition instead of being
// dropped, matching a pad that is still deflected when control returns.
float g_pending_aim_x = 0, g_pending_aim_y = 0;
constexpr float kPendingAimLimit = 150.0f;
constexpr float kPendingAimStep = 20.0f;

// A camera can stop just after its heartbeat. Counts already queued during
// that 100 ms grace period belong to the transition too, not the discard path.
// Caller holds g_mouse_lock.
void DeferMouseAim() {
    g_pending_aim_x = std::clamp(g_pending_aim_x + g_aim_x, -kPendingAimLimit, kPendingAimLimit);
    g_pending_aim_y = std::clamp(g_pending_aim_y + g_aim_y, -kPendingAimLimit, kPendingAimLimit);
    g_aim_x = g_aim_y = 0;
}

// the clock the camera and the mouse sample use
uint64_t NowMs() {
#ifdef _WIN32
    return GetTickCount64();
#else
    return SDL_GetTicks();
#endif
}

// keyboard and mouse state: XInput-style virtual keys on Windows, SDL scancodes
// elsewhere; both feed the same PC mapping below
#ifdef _WIN32
bool HeldCode(int code) { return (GetAsyncKeyState(code) & 0x8000) != 0; }
constexpr int kKeyUp = VK_UP, kKeyDown = VK_DOWN, kKeyLeft = VK_LEFT, kKeyRight = VK_RIGHT;
constexpr int kKeyEscape = VK_ESCAPE, kKeyTab = VK_TAB, kKeyReturn = VK_RETURN, kKeyBack = VK_BACK;
constexpr int kKeySpace = VK_SPACE, kKeyShift = VK_SHIFT, kKeyRShift = VK_RSHIFT, kKeyLCtrl = VK_LCONTROL,
              kKeyRCtrl = VK_RCONTROL, kKeyLAlt = VK_LMENU, kKeyRAlt = VK_RMENU;
#else
bool HeldCode(int code) {
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    return keys && code >= 0 && code < count && keys[code];
}
constexpr int kKeyUp = SDL_SCANCODE_UP, kKeyDown = SDL_SCANCODE_DOWN, kKeyLeft = SDL_SCANCODE_LEFT,
              kKeyRight = SDL_SCANCODE_RIGHT;
constexpr int kKeyEscape = SDL_SCANCODE_ESCAPE, kKeyTab = SDL_SCANCODE_TAB, kKeyReturn = SDL_SCANCODE_RETURN,
              kKeyBack = SDL_SCANCODE_BACKSPACE, kKeySpace = SDL_SCANCODE_SPACE, kKeyShift = SDL_SCANCODE_LSHIFT,
              kKeyRShift = SDL_SCANCODE_RSHIFT, kKeyLCtrl = SDL_SCANCODE_LCTRL, kKeyRCtrl = SDL_SCANCODE_RCTRL,
              kKeyLAlt = SDL_SCANCODE_LALT, kKeyRAlt = SDL_SCANCODE_RALT;
#endif

// 0 left, 1 middle, 2 right, 3 the first side button, 4 the second
bool MouseHeld(int button) {
#ifdef _WIN32
    constexpr int keys[] = {VK_LBUTTON, VK_MBUTTON, VK_RBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    return HeldCode(keys[button]);
#else
    const SDL_MouseButtonFlags state = SDL_GetMouseState(nullptr, nullptr);
    constexpr SDL_MouseButtonFlags masks[] = {SDL_BUTTON_LMASK, SDL_BUTTON_MMASK, SDL_BUTTON_RMASK,
                                              SDL_BUTTON_X1MASK, SDL_BUTTON_X2MASK};
    return (state & masks[button]) != 0;
#endif
}

// An SDL keycode as the platform's key state code. Letters and digits stay
// layout-aware, as the old HeldChar lookups were.
int KeyCode(uint32_t key) {
#ifdef _WIN32
    if (key >= 'a' && key <= 'z') return int(key - 'a' + 'A');
    if ((key >= '0' && key <= '9') || key == SDLK_RETURN || key == SDLK_ESCAPE || key == SDLK_BACKSPACE ||
        key == SDLK_TAB || key == SDLK_SPACE)
        return int(key);  // the same values as VK_RETURN, VK_ESCAPE, VK_BACK, VK_TAB, VK_SPACE
    if (key >= SDLK_F1 && key <= SDLK_F12) return VK_F1 + int(key - SDLK_F1);
    switch (key) {
    case SDLK_UP: return VK_UP;
    case SDLK_DOWN: return VK_DOWN;
    case SDLK_LEFT: return VK_LEFT;
    case SDLK_RIGHT: return VK_RIGHT;
    case SDLK_LCTRL: return VK_LCONTROL;
    case SDLK_RCTRL: return VK_RCONTROL;
    case SDLK_LSHIFT: return VK_LSHIFT;
    case SDLK_RSHIFT: return VK_RSHIFT;
    case SDLK_LALT: return VK_LMENU;
    case SDLK_RALT: return VK_RMENU;
    case SDLK_CAPSLOCK: return VK_CAPITAL;
    case SDLK_INSERT: return VK_INSERT;
    case SDLK_DELETE: return VK_DELETE;
    case SDLK_HOME: return VK_HOME;
    case SDLK_END: return VK_END;
    case SDLK_PAGEUP: return VK_PRIOR;
    case SDLK_PAGEDOWN: return VK_NEXT;
    case SDLK_KP_0: return VK_NUMPAD0;
    case SDLK_KP_1: return VK_NUMPAD1;
    case SDLK_KP_2: return VK_NUMPAD2;
    case SDLK_KP_3: return VK_NUMPAD3;
    case SDLK_KP_4: return VK_NUMPAD4;
    case SDLK_KP_5: return VK_NUMPAD5;
    case SDLK_KP_6: return VK_NUMPAD6;
    case SDLK_KP_7: return VK_NUMPAD7;
    case SDLK_KP_8: return VK_NUMPAD8;
    case SDLK_KP_9: return VK_NUMPAD9;
    case SDLK_KP_PERIOD: return VK_DECIMAL;
    case SDLK_KP_PLUS: return VK_ADD;
    case SDLK_KP_MINUS: return VK_SUBTRACT;
    case SDLK_KP_MULTIPLY: return VK_MULTIPLY;
    case SDLK_KP_DIVIDE: return VK_DIVIDE;
    default:
        if (key < 0x20 || key > 0xFFFF) return 0;
        const SHORT mapped = VkKeyScanExW(wchar_t(key), GetKeyboardLayout(0));
        return mapped == -1 ? 0 : mapped & 0xFF;
    }
#else
    return int(SDL_GetScancodeFromKey(SDL_Keycode(key), nullptr));
#endif
}

bool KeyHeld(uint32_t key) {
    if (!key) return false;
    if (key & kMouseKey) return MouseHeld(int(key & 0xFF));
    const int code = KeyCode(key);
    return code && HeldCode(code);
}

uint32_t LetterBinding(uint32_t value, char fallback) {
    const char letter = LetterKey(value, fallback);
    return letter ? uint32_t(letter - 'A' + 'a') : 0;
}

// Today's layout. The four letter keys in settings.toml still apply.
void DefaultKeyBindings(KeyBinding (&out)[kKeyBindingCount]) {
    using enum PadInput;
    const uint32_t lower_ls = uint32_t(kLeftStickClickKey - 'A' + 'a');
    const KeyBinding table[kKeyBindingCount] = {
        {MoveUp, Context::Any, {'w'}},
        {MoveDown, Context::Any, {'s'}},
        {MoveLeft, Context::Any, {'a'}},
        {MoveRight, Context::Any, {'d'}},
        {A, Context::OnFoot, {SDLK_LCTRL, SDLK_RCTRL, MouseKey(3)}},           // light attack
        {B, Context::OnFoot, {'v'}},                                           // grapple / human shield
        {X, Context::OnFoot, {SDLK_LALT, SDLK_RALT, MouseKey(4), LetterBinding(g_heavy_attack_key, 0)}},
        {Y, Context::OnFoot, {SDLK_SPACE, LetterBinding(g_climb_key, 'G')}},  // jump / climb
        {A, Context::Driving, {'w'}},                                          // accelerate
        {B, Context::Driving, {SDLK_SPACE}},                                   // handbrake
        {X, Context::Driving, {'s'}},                                          // brake / reverse
        {Y, Context::Driving, {}},
        {Black, Context::Any, {LetterBinding(g_reload_key, 'R')}},             // reload / block
        {White, Context::Any, {LetterBinding(g_use_key, 'E')}},                // use / enter vehicle
        {LT, Context::Any, {MouseKey(2), MouseKey(1)}},                        // aim / lock on
        {RT, Context::Any, {MouseKey(0)}},                                     // fire
        {Back, Context::Any, {'q', SDLK_TAB}},                                 // badge / horn / siren
        {LS, Context::Any, {lower_ls}},
        {RS, Context::Any, {SDLK_LSHIFT, SDLK_RSHIFT}},
        {Start, Context::Any, {}},                                             // Escape, fixed
    };
    std::copy(std::begin(table), std::end(table), out);
}

}  // namespace

void DefaultPadBindings(PadBinding (&out)[kPadBindingCount]) {
    using enum PadInput;
    const PadBinding table[kPadBindingCount] = {
        {Up, SDL_GAMEPAD_BUTTON_DPAD_UP},       {Down, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {Left, SDL_GAMEPAD_BUTTON_DPAD_LEFT},   {Right, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {Start, SDL_GAMEPAD_BUTTON_START},      {Back, SDL_GAMEPAD_BUTTON_BACK},
        {LS, SDL_GAMEPAD_BUTTON_LEFT_STICK},    {RS, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {A, SDL_GAMEPAD_BUTTON_SOUTH},          {B, SDL_GAMEPAD_BUTTON_EAST},
        {X, SDL_GAMEPAD_BUTTON_WEST},           {Y, SDL_GAMEPAD_BUTTON_NORTH},
        {Black, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {White, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {LT, kPadAxis | SDL_GAMEPAD_AXIS_LEFT_TRIGGER}, {RT, kPadAxis | SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
    };
    std::copy(std::begin(table), std::end(table), out);
}

namespace {

// Sets a pad input on the report: digital inputs as a bit, analog ones at full press
// (or the trigger's own depth when value is below 255).
void PressPad(Pad& pad, PadInput target, uint8_t value) {
    const int index = int(target);
    if (index < 8) {
        if (value > 32) pad.buttons |= uint16_t(1 << index);
    } else if (index < 16) {
        pad.analog[index - 8] = std::max(pad.analog[index - 8], value);
    }
}

bool WindowFocused() {
#ifdef _WIN32
    const auto window = static_cast<HWND>(app::Window());
    return window && GetForegroundWindow() == window;
#else
    const auto window = static_cast<SDL_Window*>(app::Window());
    return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
#endif
}

bool ReadKeyboardMouse(Pad& pad) {
    static std::atomic<bool> had_focus{true};
    const bool focused = WindowFocused();
    if (had_focus.exchange(focused, std::memory_order_relaxed) != focused)
        LOGR("input: game window %s focus", focused ? "regained" : "lost");
    if (!focused) {
        std::lock_guard lock(g_mouse_lock);
        g_mouse_x = g_mouse_y = g_mouse_rx = g_mouse_ry = 0;
        g_hold_rx = g_hold_ry = 0;
        g_aim_x = g_aim_y = 0;
        g_pending_aim_x = g_pending_aim_y = 0;
        g_aim_at = 0;
        g_direct_used_at.store(0, std::memory_order_relaxed);
        return false;
    }
    auto held = [](int key) { return HeldCode(key); };
    const bool ui = g_ui_active.load(std::memory_order_relaxed);
    // Keyboard movement is digital, and a diagonal is normalized to the same
    // length as a straight push (full / sqrt(2)), like the PC's keyboard input.
    // A real controller keeps its own analog values in Poll().
    const bool driving = app::kGame.keyboard_driving && app::kGame.keyboard_driving();
    g_keyboard_driving.store(driving, std::memory_order_relaxed);
    KeyBinding bindings[kKeyBindingCount];
    KeyBindings(bindings);
    bool move[4] = {};
    for (const auto& binding : bindings) {
        if (binding.context == (driving ? Context::OnFoot : Context::Driving)) continue;
        if (std::none_of(std::begin(binding.keys), std::end(binding.keys), KeyHeld)) continue;
        if (binding.target >= PadInput::MoveUp) move[int(binding.target) - int(PadInput::MoveUp)] = true;
        else PressPad(pad, binding.target, 255);
    }
    // Keyboard movement is digital, and a diagonal is normalized to the same
    // length as a straight push (full / sqrt(2)), like the PC's keyboard input.
    // A real controller keeps its own analog values in Poll().
    const int move_x = int(move[3]) - int(move[2]);
    const int move_y = int(move[0]) - int(move[1]);
    const int move_scale = move_x && move_y ? 23170 : 32767;
    pad.lx = int16_t(move_x * move_scale);
    pad.ly = int16_t(move_y * move_scale);
    // Steering uses only the horizontal axis. The forward/back keys are pedals
    // here, so a held one must not apply the on-foot diagonal scale.
    if (driving) {
        pad.ly = 0;
        pad.lx = int16_t(move_x * 32767);
    }
    // Fixed menu keys, always on so no binding can lock out the menus. When an
    // ImGui overlay owns the keyboard they already reach it as keys: do not
    // also mirror them into the pad, or the menu would step twice.
    if (!ui) {
        const int menu[] = {kKeyUp, kKeyDown, kKeyLeft, kKeyRight, kKeyEscape};
        for (int i = 0; i < 5; ++i)
            if (held(menu[i])) pad.buttons |= uint16_t(1 << i);
        if (held(kKeyReturn)) PressPad(pad, PadInput::A, 255);  // menu confirmation
        if (held(kKeyBack)) PressPad(pad, PadInput::B, 255);    // menu cancel
    }
    bool mouse_active;
    {
        std::lock_guard lock(g_mouse_lock);
        const uint64_t now = NowMs();
        const bool ui = g_ui_active.load(std::memory_order_relaxed);
        const bool direct = DirectAimLive(now);
        static bool was_direct = false;
        if (direct != was_direct && !ui) {
            LOGR("input: mouse aim %s", direct ? "direct" : "right stick (direct camera not updating)");
            was_direct = direct;
        }
        if (now - g_mouse_sample_at >= 16) {
            g_mouse_sample_at = now;
            // The guest's look controllers cube the stick, so a linear mouse
            // sample (a few counts per frame) lands near zero and the camera
            // appears dead until the mouse is thrown a long way. Shape the
            // sample with the inverse curve and a small floor so a normal move
            // produces the same stick deflection a pad does, including while a
            // cutscene or a zone transition owns the camera.
            const float gain = float(std::clamp(g_mouse_sensitivity_percent, 10u, 400u)) * 0.01f;
            auto shape = [](int counts, float gain) {
                // ignore one-count sensor jitter, then keep every real move
                // above the guest's stick dead zone
                if (counts > -2 && counts < 2) return int16_t(0);
                const float t = std::min(std::fabs(float(counts)) / 100.0f, 1.0f);
                const float s = std::min(std::max(std::cbrt(t), 0.45f), 1.0f) * gain;
                return int16_t(std::clamp((counts < 0 ? -s : s) * 32767.0f, -32767.0f, 32767.0f));
            };
            g_mouse_rx = shape(g_mouse_x, gain);
            g_mouse_ry = shape(-g_mouse_y, gain * float(std::clamp(g_mouse_vertical_sensitivity_percent, 10u, 400u)) * 0.01f);
            g_mouse_x = g_mouse_y = 0;
        }
        if (ui) g_aim_x = g_aim_y = 0;
        else if (!direct && g_direct_mouse_aim) DeferMouseAim();
        // A pad holds its deflection for as long as the stick is pushed, while
        // a mouse flick only produces one 16 ms sample. Hold the fallback stick
        // briefly so a quick move gives the transition camera the same
        // sustained input a controller would.
        if (g_mouse_rx || g_mouse_ry) {
            g_hold_rx = g_mouse_rx;
            g_hold_ry = g_mouse_ry;
            g_hold_at = now;
        } else if (direct || ui || now - g_hold_at > 150) {
            g_hold_rx = g_hold_ry = 0;
        }
        // Even a one-count direct sample must select mouse input. The stick
        // fallback's jitter threshold must not keep a connected pad in charge.
        mouse_active = g_mouse_rx || g_mouse_ry || g_hold_rx || g_hold_ry || g_aim_x || g_aim_y ||
                       (g_direct_mouse_aim && g_direct_confirmed.load(std::memory_order_relaxed) &&
                        g_aim_at && now - g_aim_at < 150);
        // the camera applies the pixels itself while direct aim is live; a lock
        // target still needs a small stick sample for the game's aim assist
        if (ui) {
            pad.rx = pad.ry = 0;
        } else if (direct) {
            const bool fresh = now - g_lock_stick_at.load(std::memory_order_relaxed) < kLockStickMs;
            pad.rx = fresh ? g_lock_stick_x.load(std::memory_order_relaxed) : 0;
            pad.ry = fresh ? g_lock_stick_y.load(std::memory_order_relaxed) : 0;
        } else {
            pad.rx = g_hold_rx;
            pad.ry = g_hold_ry;
        }
        // The PC wheel changes weapon on foot and camera in a vehicle; the pad
        // reaches both through the D-pad. Alternate press and release polls so
        // several notches are several taps, not one held direction.
        if (g_wheel_phase) {
            g_wheel_phase = 0;
        } else if (g_wheel_steps > 0) {
            pad.buttons |= uint16_t(1 << 0);
            g_wheel_steps--;
            g_wheel_phase = 1;
        } else if (g_wheel_steps < 0) {
            pad.buttons |= uint16_t(1 << 1);
            g_wheel_steps++;
            g_wheel_phase = 1;
        }
    }
    const bool active = pad.lx || pad.ly || mouse_active || pad.buttons ||
                        std::any_of(std::begin(pad.analog), std::end(pad.analog), [](uint8_t value) { return value != 0; });
    return active;
}

}  // namespace

namespace {

// the settings file's names for the keyboard table's rows, in DefaultKeyBindings order
constexpr const char* kActionNames[kKeyBindingCount] = {
    "move_forward", "move_back", "move_left", "move_right", "light_attack", "grapple", "heavy_attack", "jump",
    "drive_accelerate", "drive_handbrake", "drive_brake", "drive_y", "reload", "use", "aim", "fire",
    "back", "left_stick_click", "right_stick_click", "start"};
constexpr const char* kPadInputNames[kPadBindingCount] = {
    "Up", "Down", "Left", "Right", "Start", "Back", "LS", "RS", "A", "B", "X", "Y", "Black", "White", "LT", "RT"};
constexpr const char* kMouseNames[] = {"MouseLeft", "MouseMiddle", "MouseRight", "Mouse4", "Mouse5"};

std::mutex g_bindings_lock;
std::atomic<uint32_t> g_binding_generation{1};
bool g_custom_bindings = false;  // [controls] was read; otherwise the defaults follow the letter keys
KeyBinding g_keys[kKeyBindingCount];
PadBinding g_pad[kPadBindingCount];

std::string KeyName(uint32_t key) {
    if (key & kMouseKey) return (key & 0xFF) < 5 ? kMouseNames[key & 0xFF] : "";
    return SDL_GetKeyName(SDL_Keycode(key));
}

uint32_t KeyFromName(const std::string& name) {
    for (uint32_t i = 0; i < 5; ++i)
        if (name == kMouseNames[i]) return MouseKey(int(i));
    return uint32_t(SDL_GetKeyFromName(name.c_str()));  // SDLK_UNKNOWN (0) when not a key
}

std::string PadSourceName(int source) {
    const char* name = source & kPadAxis ? SDL_GetGamepadStringForAxis(SDL_GamepadAxis(source & 0xFF))
                                         : SDL_GetGamepadStringForButton(SDL_GamepadButton(source));
    return name ? name : "";
}

int PadSourceFromName(const std::string& name) {
    const auto button = SDL_GetGamepadButtonFromString(name.c_str());
    if (button != SDL_GAMEPAD_BUTTON_INVALID) return int(button);
    const auto axis = SDL_GetGamepadAxisFromString(name.c_str());
    return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? kPadAxis | int(axis) : -1;
}

}  // namespace

void KeyBindings(KeyBinding (&out)[kKeyBindingCount]) {
    std::lock_guard lock(g_bindings_lock);
    if (g_custom_bindings) std::copy(std::begin(g_keys), std::end(g_keys), out);
    else DefaultKeyBindings(out);
}

void PadBindings(PadBinding (&out)[kPadBindingCount]) {
    std::lock_guard lock(g_bindings_lock);
    if (g_custom_bindings) std::copy(std::begin(g_pad), std::end(g_pad), out);
    else DefaultPadBindings(out);
}

bool CanBindKey(uint32_t key) {
    if (key & kMouseKey) return key >= MouseKey(0) && key <= MouseKey(4);
    // Return, Backspace and the arrows always drive menus. Binding one made the
    // game action and the menu fire from the same press.
    if (key == SDLK_RETURN || key == SDLK_BACKSPACE || key == SDLK_UP || key == SDLK_DOWN || key == SDLK_LEFT ||
        key == SDLK_RIGHT)
        return false;
    return key && KeyCode(key);
}

void LoadBindings(const void* toml_table) {
    const auto& t = *static_cast<const toml::table*>(toml_table);
    // The old letter keys (before [controls]) are integers: 'R' is 82.
    const std::pair<const char*, uint32_t*> old_keys[] = {
        {"reload_key", &g_reload_key}, {"heavy_attack_key", &g_heavy_attack_key},
        {"use_key", &g_use_key}, {"climb_key", &g_climb_key}};
    for (const auto& [key, value] : old_keys) *value = t[key].value_or(*value);
    std::lock_guard lock(g_bindings_lock);
    DefaultKeyBindings(g_keys);
    DefaultPadBindings(g_pad);
    g_binding_generation.fetch_add(1, std::memory_order_relaxed);
    const auto* controls = t["controls"].as_table();
    g_custom_bindings = controls != nullptr;
    if (!controls) return;
    if (const auto* keyboard = (*controls)["keyboard"].as_table()) {
        for (int i = 0; i < kKeyBindingCount - 1; ++i) {  // Start stays on Escape
            const auto* keys = (*keyboard)[kActionNames[i]].as_array();
            if (!keys) continue;
            auto& binding = g_keys[i];
            std::fill(std::begin(binding.keys), std::end(binding.keys), 0u);
            int slot = 0;
            for (const auto& node : *keys) {
                const std::string name = node.value_or(std::string{});
                const uint32_t key = name.empty() ? 0 : KeyFromName(name);
                if (!CanBindKey(key)) LOGR("input: unsupported key '%s' for %s ignored", name.c_str(), kActionNames[i]);
                else if (slot < 4 && std::find(binding.keys, binding.keys + slot, key) == binding.keys + slot)
                    binding.keys[slot++] = key;
            }
        }
    }
    if (const auto* controller = (*controls)["controller"].as_table()) {
        for (int i = 0; i < kPadBindingCount; ++i) {
            const auto name = (*controller)[kPadInputNames[i]].value<std::string>();
            if (!name) continue;
            const int source = PadSourceFromName(*name);
            if (source >= 0) g_pad[i].source = source;
            else LOGR("input: unknown controller input '%s' for %s ignored", name->c_str(), kPadInputNames[i]);
        }
    }
    // A hand-edited file can bind one physical input to two actions. Keep the
    // first and give the duplicate its default (or another unused input).
    PadBinding defaults[kPadBindingCount];
    DefaultPadBindings(defaults);
    std::vector<int> used;
    for (int i = 0; i < kPadBindingCount; ++i) {
        const int source = g_pad[i].source;
        if (std::find(used.begin(), used.end(), source) == used.end()) {
            used.push_back(source);
            continue;
        }
        int fallback = defaults[i].source;
        if (std::find(used.begin(), used.end(), fallback) != used.end()) {
            fallback = -1;
            for (const auto& d : defaults)
                if (std::find(used.begin(), used.end(), d.source) == used.end()) {
                    fallback = d.source;
                    break;
                }
        }
        if (fallback >= 0) {
            g_pad[i].source = fallback;
            used.push_back(fallback);
        }
    }
}

void DefaultKeys(KeyBinding (&out)[kKeyBindingCount]) { DefaultKeyBindings(out); }

void SetBindings(const KeyBinding (&keys)[kKeyBindingCount], const PadBinding (&pad)[kPadBindingCount]) {
    std::lock_guard lock(g_bindings_lock);
    std::copy(std::begin(keys), std::end(keys), g_keys);
    std::copy(std::begin(pad), std::end(pad), g_pad);
    g_custom_bindings = true;
    g_binding_generation.fetch_add(1, std::memory_order_relaxed);
}

void KeyDisplayName(uint32_t key, char (&out)[32]) {
    constexpr const char* mouse[] = {"Mouse left", "Mouse middle", "Mouse right", "Mouse 4", "Mouse 5"};
    const char* name = !key ? "" : key & kMouseKey ? ((key & 0xFF) < 5 ? mouse[key & 0xFF] : "?")
                                                   : SDL_GetKeyName(SDL_Keycode(key));
    std::snprintf(out, sizeof(out), "%s", name);
}

void PadSourceDisplayName(int source, char (&out)[32]) {
    const char* name = "?";
    if (source & kPadAxis) {
        name = (source & 0xFF) == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "Left trigger" : "Right trigger";
    } else {
        switch (source) {
        case SDL_GAMEPAD_BUTTON_SOUTH: name = "A / Cross"; break;
        case SDL_GAMEPAD_BUTTON_EAST: name = "B / Circle"; break;
        case SDL_GAMEPAD_BUTTON_WEST: name = "X / Square"; break;
        case SDL_GAMEPAD_BUTTON_NORTH: name = "Y / Triangle"; break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: name = "LB / L1"; break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: name = "RB / R1"; break;
        case SDL_GAMEPAD_BUTTON_BACK: name = "View / Share"; break;
        case SDL_GAMEPAD_BUTTON_START: name = "Menu / Options"; break;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: name = "Left stick click"; break;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: name = "Right stick click"; break;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: name = "D-pad up"; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: name = "D-pad down"; break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: name = "D-pad left"; break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: name = "D-pad right"; break;
        default:
            if (const char* sdl = SDL_GetGamepadStringForButton(SDL_GamepadButton(source))) name = sdl;
        }
    }
    std::snprintf(out, sizeof(out), "%s", name);
}

int HeldPadSource() {
    std::lock_guard lock(g_lock);
    SDL_Gamepad* pad = g_pads[0];
    if (!pad) return -1;
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button)
        if (button != SDL_GAMEPAD_BUTTON_GUIDE && SDL_GetGamepadButton(pad, SDL_GamepadButton(button))) return button;
    for (auto axis : {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER})
        if (SDL_GetGamepadAxis(pad, axis) > 20000) return kPadAxis | int(axis);
    return -1;
}

uint32_t BindingGeneration() { return g_binding_generation.load(std::memory_order_relaxed); }

bool PadRemapped() {
    PadBinding pad[kPadBindingCount], defaults[kPadBindingCount];
    PadBindings(pad);
    DefaultPadBindings(defaults);
    for (int i = 0; i < kPadBindingCount; ++i)
        if (pad[i].target != defaults[i].target || pad[i].source != defaults[i].source) return true;
    return false;
}

namespace {

// a short keycap label the 3x5 prompt font can draw
std::string ShortKeyLabel(uint32_t key) {
    if (key & kMouseKey) {
        constexpr const char* mouse[] = {"LMB", "MMB", "RMB", "M4", "M5"};
        return (key & 0xFF) < 5 ? mouse[key & 0xFF] : "";
    }
    if (key >= 'a' && key <= 'z') return std::string(1, char(key - 'a' + 'A'));
    if (key >= '0' && key <= '9') return std::string(1, char(key));
    if (key >= SDLK_F1 && key <= SDLK_F12) return "F" + std::to_string(key - SDLK_F1 + 1);
    switch (key) {
    case SDLK_SPACE: return "SPC";
    case SDLK_LCTRL: case SDLK_RCTRL: return "CTL";
    case SDLK_LALT: case SDLK_RALT: return "ALT";
    case SDLK_LSHIFT: case SDLK_RSHIFT: return "SHF";
    case SDLK_TAB: return "TAB";
    case SDLK_RETURN: return "ENT";
    case SDLK_ESCAPE: return "ESC";
    case SDLK_BACKSPACE: return "BKS";
    case SDLK_UP: return "UP";
    case SDLK_DOWN: return "DN";
    case SDLK_LEFT: return "LFT";
    case SDLK_RIGHT: return "RGT";
    default: break;
    }
    // anything else: the first letters of its name, as far as the font has them
    std::string label;
    for (const char* c = SDL_GetKeyName(SDL_Keycode(key)); *c && label.size() < 3; ++c)
        if (std::isalnum(static_cast<unsigned char>(*c))) label += char(std::toupper(static_cast<unsigned char>(*c)));
    return label.empty() ? "?" : label;
}

// the first key bound to a target in the context, 0 when none
uint32_t FirstKey(const KeyBinding (&bindings)[kKeyBindingCount], PadInput target, bool driving) {
    for (const auto& binding : bindings) {
        if (binding.target != target || binding.context == (driving ? Context::OnFoot : Context::Driving)) continue;
        for (uint32_t key : binding.keys)
            if (key) return key;
    }
    return 0;
}

}  // namespace

void PromptLabel(PadInput target, bool driving, char (&out)[8]) {
    KeyBinding bindings[kKeyBindingCount];
    KeyBindings(bindings);
    std::string label;
    if (target == PadInput::Start) {
        label = "ESC";
    } else if (target == PadInput::MoveUp) {
        // single-character movement keys read as one word, like WASD; anything else as MOV
        for (PadInput move : {PadInput::MoveUp, PadInput::MoveLeft, PadInput::MoveDown, PadInput::MoveRight}) {
            const std::string key = ShortKeyLabel(FirstKey(bindings, move, driving));
            label = key.size() == 1 && label != "MOV" ? label + key : "MOV";
        }
    } else {
        const uint32_t key = FirstKey(bindings, target, driving);
        label = key ? ShortKeyLabel(key) : "-";
    }
    std::snprintf(out, sizeof(out), "%.4s", label.c_str());
}

void SaveBindings(void* toml_table) {
    auto& t = *static_cast<toml::table*>(toml_table);
    KeyBinding keys[kKeyBindingCount];
    PadBinding pad[kPadBindingCount];
    KeyBindings(keys);
    PadBindings(pad);
    toml::table keyboard, controller;
    for (int i = 0; i < kKeyBindingCount - 1; ++i) {
        toml::array names;
        for (uint32_t key : keys[i].keys)
            if (key) names.push_back(KeyName(key));
        keyboard.insert(kActionNames[i], std::move(names));
    }
    for (int i = 0; i < kPadBindingCount; ++i) controller.insert(kPadInputNames[i], PadSourceName(pad[i].source));
    t.insert_or_assign("controls", toml::table{{"keyboard", std::move(keyboard)}, {"controller", std::move(controller)}});
    // [controls] replaces the old letter keys
    for (const char* old : {"reload_key", "heavy_attack_key", "use_key", "climb_key"}) t.erase(old);
}

namespace {

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    if (slot == g_pads) UpdatePromptType();
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        if (i == 0) UpdatePromptType();
        LOGI("input: controller %d unplugged", i + 1);
    }
}

// Linux consumes SDL events on the window thread. Windows uses a native message
// loop and updates gamepads here at most once per millisecond, under g_lock.
void UpdatePads() {
#ifdef _WIN32
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    SDL_UpdateGamepads();  // safe from guest threads that poll for input
    // State is read directly from the pads. Drain unused button, axis and other
    // events as well, or they can fill SDL's queue and crowd out hot-plug notices.
    SDL_Event events[32];
    int count;
    while ((count = SDL_PeepEvents(events, int(std::size(events)), SDL_GETEVENT,
                                  SDL_EVENT_FIRST, SDL_EVENT_LAST)) > 0) {
        for (int i = 0; i < count; ++i) {
            const SDL_Event& event = events[i];
            if (event.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(event.gdevice.which);
            else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(event.gdevice.which);
        }
    }
#endif
}

}  // namespace

// Window thread, before the first SDL_Init: these hints shape the joystick subsystem.
void SetHints() {
    static std::once_flag once;
    std::call_once(once, [] {
        SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
        SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    });
}

void GamepadAdded(int32_t id) {
    std::lock_guard lock(g_lock);
    AddPad(SDL_JoystickID(id));
}

void GamepadRemoved(int32_t id) {
    std::lock_guard lock(g_lock);
    RemovePad(SDL_JoystickID(id));
}

bool Connected(int port) {
    std::lock_guard lock(g_lock);
    UpdatePads();
    // The title only polls a controller port that it has seen connect, and keyboard
    // and scripted input are merged into port 0 by Poll. Report port 0 present so a
    // keyboard-only host (or a scripted run without a pad) works on every platform.
    if (port == 0) return true;
    return port >= 0 && port < 4 && g_pads[port];
}

PromptStyle ControllerPromptStyle() {
    if (g_keyboard_mouse_active.load(std::memory_order_relaxed))
        return g_keyboard_driving.load(std::memory_order_relaxed) ? PromptStyle::KeyboardDriving
                                                                  : PromptStyle::KeyboardMouse;
    return g_prompt_style.load(std::memory_order_relaxed);
}

bool LockOnHeld() { return g_lock_held.load(std::memory_order_relaxed); }

bool KeyboardMouseActive() { return g_keyboard_mouse_active.load(std::memory_order_relaxed); }

bool MouseLookStickActive() { return g_mouse_look_stick_active.load(std::memory_order_relaxed); }

void SetLockOnStick(int16_t x, int16_t y) {
    g_lock_stick_x.store(x, std::memory_order_relaxed);
    g_lock_stick_y.store(y, std::memory_order_relaxed);
    g_lock_stick_at.store(NowMs(), std::memory_order_relaxed);
}

// relative motion and wheel notches from the window thread; the Windows path
// takes them from WM_INPUT, the SDL path from MOUSE_MOTION / MOUSE_WHEEL events
void ApplyMouseDelta(int dx, int dy, int wheel) {
    // the menu moves the pointer, not the camera: do not queue that motion
    if (g_ui_active.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mouse_lock);
    if (wheel) g_wheel_steps = std::clamp(g_wheel_steps + wheel, -8, 8);
    // the stick path keeps its small clamp
    g_mouse_x = std::clamp(g_mouse_x + dx, -100, 100);
    g_mouse_y = std::clamp(g_mouse_y + dy, -100, 100);
    const uint64_t now = NowMs();
    g_aim_at = now;
    if (DirectAimLive(now)) {
        // direct aim takes the full raw delta, like the PC fix's raw input path
        g_aim_x = std::clamp(g_aim_x + float(dx), -10000.0f, 10000.0f);
        g_aim_y = std::clamp(g_aim_y + float(dy), -10000.0f, 10000.0f);
    } else if (g_direct_mouse_aim && g_direct_confirmed.load(std::memory_order_relaxed)) {
        // no camera is updating: keep a bounded amount so the view catches up
        // when the transition ends instead of dropping the movement
        g_aim_x += float(dx);
        g_aim_y += float(dy);
        DeferMouseAim();
    }
}

#ifdef _WIN32
void RegisterMouse(void* window) {
    const RAWINPUTDEVICE mouse{0x01, 0x02, 0, static_cast<HWND>(window)};
    if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) LOGE("input: could not register raw mouse");
}

void ReadMouse(void* raw_input) {
    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(static_cast<HRAWINPUT>(raw_input), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) !=
            sizeof(input) ||
        input.header.dwType != RIM_TYPEMOUSE || (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
        return;
    int wheel = 0;
    if (input.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
        wheel = int16_t(input.data.mouse.usButtonData) > 0 ? 1 : -1;
    ApplyMouseDelta(int(input.data.mouse.lLastX), int(input.data.mouse.lLastY), wheel);
}
#else
void RegisterMouse(void*) {}
void ReadMouse(void*) {}

void ReadMouseMotion(float dx, float dy) {
    ApplyMouseDelta(int(std::lround(dx)), int(std::lround(dy)), 0);
}

void ReadMouseWheel(float steps) {
    const int n = int(std::lround(steps));
    if (n) ApplyMouseDelta(0, 0, n > 0 ? 1 : -1);
}
#endif

void UseDirectMouseAim() {
    g_direct_at.store(NowMs(), std::memory_order_relaxed);
    g_direct_confirmed.store(true, std::memory_order_relaxed);
}

void SetUiActive(bool active) {
    g_ui_active.store(active, std::memory_order_relaxed);
    if (!active) return;
    std::lock_guard lock(g_mouse_lock);
    g_mouse_x = g_mouse_y = g_mouse_rx = g_mouse_ry = 0;
    g_hold_rx = g_hold_ry = 0;

    g_aim_x = g_aim_y = 0; // the menu moved the pointer, not the camera
    g_pending_aim_x = g_pending_aim_y = 0;
    g_aim_at = 0;
    g_direct_used_at.store(0, std::memory_order_relaxed);
}

bool UiActive() { return g_ui_active.load(std::memory_order_relaxed); }

bool ConsumeMouseAim(float& dx, float& dy) {
    std::lock_guard lock(g_mouse_lock);
    if (g_ui_active.load(std::memory_order_relaxed) || !DirectAimLive(NowMs())) {
        g_aim_x = g_aim_y = 0;
        return false;
    }
    // If no pad poll ran during the stall, preserve the grace-period counts
    // here as well. Release them through the same bounded transition buffer.
    if (NowMs() - g_aim_at > 200) DeferMouseAim();

    float px = 0, py = 0;
    if (g_pending_aim_x != 0 || g_pending_aim_y != 0) {
        px = std::clamp(g_pending_aim_x, -kPendingAimStep, kPendingAimStep);
        py = std::clamp(g_pending_aim_y, -kPendingAimStep, kPendingAimStep);
        g_pending_aim_x -= px;
        g_pending_aim_y -= py;
    }
    if (g_aim_x == 0 && g_aim_y == 0 && px == 0 && py == 0) return false;
    dx = g_aim_x + px;
    dy = g_aim_y + py;
    g_aim_x = g_aim_y = 0;
    g_direct_used_at.store(NowMs(), std::memory_order_relaxed);
    return true;
}

bool MouseMovedRecently() {
    std::lock_guard lock(g_mouse_lock);
    return !g_ui_active.load(std::memory_order_relaxed) && g_aim_at && NowMs() - g_aim_at < 100;
}

bool DirectAimRecent() {
    const uint64_t used = g_direct_used_at.load(std::memory_order_relaxed);
    return used && NowMs() - used < 5000;
}

void Init() {
    if (g_sdl) return;  // started already for the game code window
    // the old right-stick path stays available as a fallback
    if (std::getenv("TCNYC_MOUSE_STICK")) g_direct_mouse_aim = false;
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices. Hosts call SetHints before their first SDL_Init.
    SetHints();
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL failed to start, no controllers: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    g_sdl = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

Pad Poll(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        UpdatePads();
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) {
            p.connected = true;
            PadBinding bindings[kPadBindingCount];
            PadBindings(bindings);
            for (const auto& binding : bindings) {
                // 0 to 32767 into 0 to 255
                const uint8_t value =
                    binding.source & kPadAxis
                        ? uint8_t(std::max<int>(SDL_GetGamepadAxis(pad, SDL_GamepadAxis(binding.source & 0xFF)), 0) >> 7)
                        : (SDL_GetGamepadButton(pad, SDL_GamepadButton(binding.source)) ? 255 : 0);
                if (value) PressPad(p, binding.target, value);
            }
            auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
            // SDL's sticks have y down
            // not plain negation: -32768 has no positive
            p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
            p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
            p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
            p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
        }
    }
    bool from_keyboard_mouse = false;
    if (port == 0) {
        Pad keyboard_mouse;
        const bool active = ReadKeyboardMouse(keyboard_mouse);
        const bool ui = g_ui_active.load(std::memory_order_relaxed);
        const bool controller_active = p.buttons || std::abs(p.lx) > 8000 || std::abs(p.ly) > 8000 ||
                                       std::abs(p.rx) > 8000 || std::abs(p.ry) > 8000 ||
                                       std::any_of(std::begin(p.analog), std::end(p.analog),
                                                   [](uint8_t value) { return value > 32; });
        if (active || !p.connected) g_keyboard_mouse_active.store(true, std::memory_order_relaxed);
        else if (controller_active) {
            g_keyboard_mouse_active.store(false, std::memory_order_relaxed);
            // Steam Input re-routes its virtual pads as controllers come and go, so the pad behind
            // port 0 can change without an add or remove: look at it again while it is in use
            static uint64_t prompt_checked_at = 0;
            const uint64_t now = SDL_GetTicks();
            if (now - prompt_checked_at >= 1000) {
                prompt_checked_at = now;
                std::lock_guard pads(g_lock);
                UpdatePromptType();
            }
            // Relinquish held camera angles immediately. The next mouse
            // sample starts from the controller's current heading.
            std::lock_guard lock(g_mouse_lock);
            g_direct_used_at.store(0, std::memory_order_relaxed);
            g_aim_x = g_aim_y = 0; // a flick before the handoff must not turn once
            g_pending_aim_x = g_pending_aim_y = 0;
        }
        // While an ImGui overlay owns the keyboard, do not merge the keyboard
        // pad into port 0: its movement and button mappings would reach
        // NavBits as extra menu presses. ImGui gets the keys directly, and the
        // real controller (if any) stays the pad source.
        if (!ui && (active || !p.connected)) {
            keyboard_mouse.connected = true;
            p = keyboard_mouse;
            from_keyboard_mouse = true;
        } else if (ui) {
            // Keep port 0 present as before. With no controller the pad is
            // already neutral; keyboard overlay controls never enter it.
            p.connected = true;
        }
    }
    // the left trigger aims and locks on; the mouse aim leaves the heading to the game only while it is held
    if (port == 0) g_lock_held.store(p.analog[6] > 32, std::memory_order_relaxed);
    // A background window keeps receiving controller state through SDL, but it
    // must not steer, attack or open the menu. Scripted and replayed input is
    // applied by GameInfo::pad just below, after this gate.
    if (!WindowFocused()) {
        const bool connected = p.connected;
        p = Pad{};
        p.connected = connected;
    }
    if (port == 0)
        g_mouse_look_stick_active.store(from_keyboard_mouse && (p.rx || p.ry), std::memory_order_relaxed);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

uint16_t HeldOnAnyController() {
    constexpr std::pair<SDL_GamepadButton, uint16_t> kFaceButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000},  {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
    std::lock_guard lock(g_lock);
    UpdatePads();
    uint16_t held = 0;
    for (SDL_Gamepad* pad : g_pads)
        for (auto [button, mask] : kFaceButtons)
            if (pad && SDL_GetGamepadButton(pad, button)) held |= mask;
    return held;
}

// the motors run until set again. A zero duration sends an effect once and never
// refreshes it, which loses rumble on pads that time an effect out (handheld
// haptics, DualSense, Switch); the longest SDL duration makes SDL resend the
// current motors every two seconds until the title sends zero.
constexpr uint32_t kRumbleDurationMs = 0xFFFF;

void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, kRumbleDurationMs);
}

}  // namespace input
