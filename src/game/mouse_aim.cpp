#include "game_addresses.h"
// Direct mouse aim for the gameplay camera.
//
// The camera object is [[0x360E00] + 0xAC]. It owns one look controller per
// camera mode in an array at +0x114, indexed by the mode byte at +0x19; the
// active controller is cached at +0x174. The shared per-frame camera update
// sub_0002E200 reads that active controller and calls its virtual methods, so
// injecting the mouse there covers the camera modes that accumulate the globals:
// on foot (mode 5), precision aim / crosshair (modes 0/6/11), drive-by, sniper
// and the other mission cameras, not just the four modes the PC fix names. The
// unarmed and transition controllers can replace the yaw before building the
// view. Keep the requested heading for this update and restore it immediately
// before their shared orientation builder reads it.
//
// Some camera paths (precision aim among them) run a look controller's own
// input step without the shared update. Those controllers are hooked directly
// as a fallback; they reuse the same request and put it back after their target
// pass, except while a lock-on target is active, where the game's snap wins.
//
// The look controllers accumulate a yaw and a pitch in 0x348B20 (yaw) and
// 0x348A80 (pitch); the camera update turns those globals into the final
// orientation later in the frame.
//
// Mouse motion is already sampled on the host, but folding it into the pad's
// right stick loses most of it: the guest reads the stick as a signed byte, so
// small movements quantise away and the stick's dead zone throws away the rest.
// With direct aim the host no longer feeds the mouse into the stick; instead
// the shared camera update adds the raw count delta straight to the two
// accumulators before the active controller runs, so the camera keeps its own
// clamps and smoothing.
//
// The feel follows the PC version's WidescreenFix (RawInput.ixx): raw counts
// are scaled by fMouseSens (4.0) * RawInputMouse (1.0) / 1000 = 0.004 rad per
// count, with the same scale on both axes. Its direct camera path also allows
// -89..+60 degrees of pitch on foot and -89..+10 degrees in a vehicle while
// the mouse is active (PC convention, positive up; the game's accumulator is
// positive looking down, so the on-foot clamp below is mirrored), and the
// controllers' own -40..+60 limit is widened for that one update and restored
// afterwards. Direct aim stops feeding the stick for 5 s after the last
// sample, matching the PC fix's idle fallback.
//
// The profile's camera auto-center also has to be held off while the mouse is
// in use. It pulls the pitch back toward the controller's rest angle whenever
// the right stick is idle, and direct aim keeps the stick centred, so without
// that it would erase each mouse adjustment on the same frame. The setting is
// restored after the update; when a controller takes over the input, the
// game's centering returns with it.
//
// The on-foot controller also writes its rest pitch over 0x348A80 before the
// shared orientation builder runs, independently of the auto-center setting.
// The mouse therefore keeps its own accumulated pitch and restores it at that
// builder; reading the global back each frame would only ever apply one frame
// of vertical movement.
//
// In a vehicle the active controller (sub_00024040) recomputes its chase yaw
// every frame instead of accumulating the stick into the globals, and then
// calls sub_0001FD40 to build the camera orientation. That path therefore
// applies the mouse to the globals between the controller's computation and
// sub_0001FD40, and holds the resulting absolute yaw/pitch while the mouse is
// active, which is what the PC fix does. When the mouse goes idle the game's
// own chase camera comes back.
//
// TCNYC_MOUSE_STICK=1 restores the old right-stick path.
//
// The pad's own right-stick response is replaced here as well. The look
// controllers read the two signed stick bytes through sub_0001F030 and then
// cube them; while one of those controllers runs, that accessor returns a
// pre-shaped sample from common/controller_look.h (one radial dead zone, a
// quadratic curve and the optional aim scale). Original leaves the accessor
// untouched. The mouse's direct path and keyboard/mouse fallback are excluded.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "common/controller_look.h"
#include "core/input.h"
#include "core/log.h"
#include "level_generation.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kGame = TC_ADDR(0x360E00);   // [0x360E00] -> game/session object
constexpr uint32_t kCamera = 0xAC;     // game + 0xAC -> gameplay camera
constexpr uint32_t kOwner = 0x34;      // controller + 0x34 -> camera
constexpr uint32_t kActive = 0x174;    // camera + 0x174 -> active controller
constexpr uint32_t kTarget = 0x17C;    // camera + 0x17C -> followed pawn
constexpr uint32_t kActiveFlag = 0x18C;
constexpr uint32_t kMode = 0x19;       // camera + 0x19 -> camera mode
constexpr uint32_t kYaw = TC_ADDR(0x348B20);    // radians, accumulated by the controller
constexpr uint32_t kPitch = TC_ADDR(0x348A80);  // radians
// Profile setting: while the right stick is idle, the look controller blends
// the pitch back toward its rest angle. Set by the game's camera settings
// update; read only by the shared look-input routine sub_000205A0.
constexpr uint32_t kAutoCenter = TC_ADDR(0x348C24);
constexpr uint32_t kPlayer = TC_ADDR(0x37D284); // [0x37D284] -> local player pawn
constexpr uint32_t kLockTarget = 0xB4; // player + 0xB4 -> locked pawn
constexpr uint32_t kInvertX = 0xFEF;   // game + 0xFEF
constexpr uint32_t kInvertY = 0xFEE;   // game + 0xFEE
// The input manager holds the pad axes as signed bytes at +0x70. The look
// controllers read them through sub_0001F030, which normalises by 128.
constexpr uint32_t kInputState = TC_ADDR(0x4204EC); // [0x4204EC] -> input state
constexpr uint32_t kStickAxes = 0x70;               // state + 0x70/0x71: right stick x/y

constexpr uint8_t kModeVehicle = 13;

// The PC WidescreenFix takes raw counts straight to camera angles:
//     scale = fMouseSens (4.0) * RawInputMouse (1.0) / 1000 = 0.004 rad/count
// and applies the same scale to both axes.
constexpr float kRadiansPerPixel = 0.004f;
constexpr float kPi = 3.14159265358979f;

// 0x348A80 is positive looking down: sub_0001FD40 negates it when it builds
// the orientation, and the look controllers subtract a stick-up input from it.
// The PC fix's limits are quoted in its own positive-up convention, so the
// on-foot pair is mirrored here (60 up / 89 down). The vehicle controller
// already uses positive down, so its pair needs no mirroring.
constexpr float kOnFootPitchMin = -60.0f * kPi / 180.0f;  // view up
constexpr float kOnFootPitchMax = 89.0f * kPi / 180.0f;   // view down
constexpr float kVehiclePitchMin = -10.0f * kPi / 180.0f; // view up
constexpr float kVehiclePitchMax = 89.0f * kPi / 180.0f;  // view down
// Just above the game's right-stick dead zone: enough for its aim assist to
// run, small enough not to move the view when no target is under the crosshair.
constexpr int16_t kLockStick = 1200;

uint32_t Rd(uint8_t *base, uint32_t va) { return mem::IsCommitted(va) ? X86_MEM32(va) : 0; }
uint8_t Rd8(uint8_t *base, uint32_t va) { return mem::IsCommitted(va) ? X86_MEM8(va) : 0; }
float RdF(uint8_t *base, uint32_t va) {
    float f = 0;
    if (mem::IsCommitted(va)) std::memcpy(&f, base + va, sizeof(f));
    return f;
}

uint64_t NowMs() {
    static const auto start = std::chrono::steady_clock::now();
    return uint64_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

// The gameplay camera, when this specific look controller belongs to it and it
// has a pawn to follow.  The original methods bail out on the same two fields,
// so injecting there would move angles no camera reads.
uint32_t GameplayCamera(uint8_t *base, uint32_t controller) {
    if (!controller || !mem::IsCommitted(controller + kOwner)) return 0;
    const uint32_t owner = X86_MEM32(controller + kOwner);
    const uint32_t game = Rd(base, kGame);
    if (!game || !mem::IsCommitted(game + kCamera)) return 0;
    const uint32_t camera = X86_MEM32(game + kCamera);
    if (!camera || owner != camera) return 0;
    if (!Rd(base, camera + kTarget) || !Rd8(base, camera + kActiveFlag)) return 0;
    return camera;
}

// The shared camera object itself (not a look controller): the per-frame update
// asks it for the active controller and runs that controller's virtual methods.
// A zone load or a car exit clears the followed target for a moment, so the
// target cannot be part of this test: the direct path has to stay live through
// that window or the mouse drops to the pad and its horizontal dead zone.
bool IsGameplayCamera(uint8_t *base, uint32_t camera) {
    const uint32_t game = Rd(base, kGame);
    if (!camera || !game || camera != Rd(base, game + kCamera)) return false;
    return Rd8(base, camera + kActiveFlag) != 0;
}

// The gameplay camera for a look controller that is active right now. Unlike
// GameplayCamera, a followed target is not required: the precision-aim look
// step still accumulates the mouse while a transition has cleared the target.
uint32_t ActiveGameplayCamera(uint8_t *base, uint32_t controller) {
    if (!controller || !mem::IsCommitted(controller + kOwner)) return 0;
    const uint32_t camera = X86_MEM32(controller + kOwner);
    const uint32_t game = Rd(base, kGame);
    if (!camera || !game || camera != Rd(base, game + kCamera)) return 0;
    if (Rd(base, camera + kActive) != controller || !Rd8(base, camera + kActiveFlag)) return 0;
    return camera;
}

// Scoped to a shared camera update: other cameras and unrelated calls to the
// orientation builder must keep their own angles. Thread-local because guest
// threads can build other cameras while the gameplay update is running.
struct CameraAim {
    uint32_t camera = 0;
    uint32_t controller = 0;
    float yaw = 0;
    float pitch = 0;
    bool active = false;   // the mouse sampled recently (yaw handoff)
    bool mouse_aim = false; // keyboard/mouse owns the aim (pitch hold)
    bool valid = false;
    uint64_t at = 0;       // when this request was made
};
thread_local CameraAim g_camera_aim;

// Pitch accumulated by the mouse while keyboard/mouse owns the on-foot camera.
// The controller's target pass writes its rest angle over 0x348A80 before the
// orientation builder runs, so reading that global back each frame would only
// ever apply one frame of mouse movement. Keep our own angle instead, seeded
// from the game's pitch once, and use it for the view and the update. It is
// seeded again whenever the camera, its controller or the mode changes (a level
// load builds new ones), or when a pad takes over. A pause or cutscene keeps it,
// so the view comes back where the player left it.
struct AimHold {
    uint32_t camera = 0, controller = 0;
    uint8_t mode = 0;
    float pitch = 0;
    bool held = false;
    uint32_t generation = 0;  // game::LevelGeneration() when the hold started
};
thread_local AimHold g_aim_hold;

// Adds the pending raw mouse delta to the camera angles.  The caller injects
// before the controller runs and widens that controller's own pitch clamp, so
// the controller does not clamp the result back to its stock -40..+60 range.
// Returns the pitch the view should use and whether the mouse owns the aim.
float ApplyAim(uint8_t *base, uint32_t camera, uint32_t controller, uint8_t mode, float pitch_min, float pitch_max,
               bool &mouse_aim) {
    // tells input this camera is still the one updating; when it stops, input
    // falls back to the right stick for the new camera mode
    input::UseDirectMouseAim();
    float dx = 0, dy = 0;
    const bool sampled = input::ConsumeMouseAim(dx, dy);
    // a level load or world reset ends the hold: the camera and its controller are new objects
    const uint32_t generation = game::LevelGeneration();
    if (g_aim_hold.generation != generation) g_aim_hold.held = false;
    // The hold starts when the mouse moves and lasts while keyboard/mouse stays the active device. A
    // keyboard-only player never starts it, so the game's own pitch and auto-centre stay in charge.
    mouse_aim = input::DirectAimRecent() || (input::KeyboardMouseActive() && g_aim_hold.held);
    if (!mouse_aim) {
        g_aim_hold.held = false;
        return RdF(base, kPitch);
    }
    if (!g_aim_hold.held || g_aim_hold.camera != camera || g_aim_hold.controller != controller ||
        g_aim_hold.mode != mode || g_aim_hold.generation != generation) {
        // Start from the game's current angle, then keep the mouse's own so the
        // controller's per-frame rest-pitch reset cannot undo the accumulation.
        g_aim_hold = {camera, controller, mode, RdF(base, kPitch), true, generation};
    }

    const float percent = float(std::clamp(input::g_mouse_sensitivity_percent, 10u, 400u)) * 0.01f;
    const float vertical = float(std::clamp(input::g_mouse_vertical_sensitivity_percent, 10u, 400u)) * 0.01f;
    const float yaw_scale = kRadiansPerPixel * percent;
    const float pitch_scale = kRadiansPerPixel * percent * vertical;
    const uint32_t game = Rd(base, kGame);
    const float yaw_sign = Rd8(base, game + kInvertX) ? -1.0f : 1.0f;
    // 0x348A80 is positive looking down, and SDL/WM_INPUT report raw Y
    // positive when the mouse moves down, so the default adds dy. Invert-Y
    // flips that. Playing with the stock setting must not invert the view.
    const float pitch_sign = Rd8(base, game + kInvertY) ? -1.0f : 1.0f;

    float yaw = RdF(base, kYaw);
    if (sampled) {
        yaw += dx * yaw_scale * yaw_sign;
        g_aim_hold.pitch = std::clamp(g_aim_hold.pitch + dy * pitch_scale * pitch_sign, pitch_min, pitch_max);
    }
    std::memcpy(base + kYaw, &yaw, sizeof(yaw));
    std::memcpy(base + kPitch, &g_aim_hold.pitch, sizeof(g_aim_hold.pitch));
    return g_aim_hold.pitch;
}

// A request made by the shared camera update is consumed by the controller it
// is about to run. The window is short: it only has to cover that one update,
// and it keeps an early-returned controller from restoring a stale heading.
bool AimRequested(uint32_t controller) {
    return g_camera_aim.valid && g_camera_aim.controller == controller &&
           NowMs() - g_camera_aim.at < 100;
}

// True while the game has a lock-on target. Its own camera pass then owns the
// heading: restoring the mouse here would leave the crosshair beside the target
// instead of snapping onto it. Locking on needs the left trigger held; the
// player's target field alone is set outside lock-on too (after a level load it
// is never clear), which kept the mouse from turning the camera's intro.
bool LockTargetActive(uint8_t *base) {
    if (!input::LockOnHeld()) return false;
    const uint32_t player = Rd(base, kPlayer);
    if (!player || !mem::IsCommitted(player + kLockTarget)) return false;
    return Rd(base, player + kLockTarget) != 0;
}

// Put the heading the mouse asked for back after a controller's own target or
// transition pass. Only the values already requested by this update are
// written, so this cannot consume a delta twice.
bool RestoreRequestedAim(uint8_t *base, uint32_t controller) {
    if (!input::g_direct_mouse_aim || !AimRequested(controller)) return false;
    if (LockTargetActive(base)) return false;
    if (g_camera_aim.active) std::memcpy(base + kYaw, &g_camera_aim.yaw, sizeof(g_camera_aim.yaw));
    if (g_camera_aim.mouse_aim) std::memcpy(base + kPitch, &g_camera_aim.pitch, sizeof(g_camera_aim.pitch));
    return true;
}

// Widen a look controller's pitch clamp for one update, so the controller's own
// clamp does not undo the PC range.  The stock fields are restored by the
// caller before it returns, keeping the gamepad path original.
bool WidenPitch(uint8_t *base, uint32_t controller, float min, float max, uint32_t &old_min, uint32_t &old_max) {
    if (!mem::IsCommitted(controller + 0x20) || !mem::IsCommitted(controller + 0x24)) return false;
    old_min = X86_MEM32(controller + 0x20);
    old_max = X86_MEM32(controller + 0x24);
    std::memcpy(base + controller + 0x20, &min, sizeof(min));
    std::memcpy(base + controller + 0x24, &max, sizeof(max));
    return true;
}

void RestorePitch(uint8_t *base, uint32_t controller, uint32_t old_min, uint32_t old_max) {
    std::memcpy(base + controller + 0x20, &old_min, sizeof(old_min));
    std::memcpy(base + controller + 0x24, &old_max, sizeof(old_max));
}

namespace cl = game::controller_look;
using cl::GameCurve;

// Which cubic the look controller about to run applies to its stick sample.
// The response mapping is only active inside a gameplay look controller, so
// movement, menus and the mouse's fallback stick keep their original feel.
struct LookMapping {
    bool active = false;
    GameCurve curve = GameCurve::RadialCubic;
};
thread_local LookMapping t_look_mapping;

struct ScopedLookMapping {
    LookMapping previous;
    ScopedLookMapping(GameCurve curve, bool gameplay) : previous(t_look_mapping) {
        const uint32_t response =
            std::atomic_ref(input::g_controller_aim_response).load(std::memory_order_relaxed);
        const bool mouse_stick = input::MouseLookStickActive();
        t_look_mapping = {gameplay && response == 0 && !mouse_stick, curve};
        // LOGR: release and dev builds both define NDEBUG, which drops info lines.
        static std::atomic<int> active_logged{0}, skipped_logged{0};
        if (t_look_mapping.active) {
            if (active_logged.fetch_add(1, std::memory_order_relaxed) < 1)
                LOGR("controller look: right-stick curve active (%s cubic)",
                     curve == GameCurve::RadialCubic ? "radial" : "per-axis");
        } else if (skipped_logged.fetch_add(1, std::memory_order_relaxed) < 3) {
            LOGR("controller look: right-stick curve skipped (gameplay %d, response %u, mouse stick %d)",
                 int(gameplay), response, int(mouse_stick));
        }
    }
    ~ScopedLookMapping() { t_look_mapping = previous; }
    ScopedLookMapping(const ScopedLookMapping &) = delete;
    ScopedLookMapping &operator=(const ScopedLookMapping &) = delete;
};

// A look step's own pitch change, kept against a recompute from a pinned camera.
//
// Several camera updates run a look step (the stick moves 0x348A80) and then
// recompute the pitch from where the camera actually is: sub_0001FCA0,
// sub_000225B0 and sub_0002CFC0 set 0x348A80 to -atan2(height, distance) from
// the camera position at 0x348A60 to its look point at 0x3489E0. Looking hard
// up swings the camera down behind the player until the ground stops it, so the
// recomputed pitch is pinned. Pushing the stick back down then moves the pitch
// in the look step, the recompute puts the pinned value straight back, and the
// camera position only creeps after it: the view sticks at the top for seconds
// (in the L2 view until L2 is released). When the stick moved the pitch back
// toward the middle of the controller's range and the recompute would pull it
// away again, keep the look step's pitch. Pushing further into the pin, and
// every frame without stick movement, is unchanged.
struct LookStep {
    bool valid = false;
    float before = 0, after = 0, middle = 0;
    uint64_t at_us = 0;  // when the look step ran
    uint32_t kept = 0;   // recomputes overridden since
};
thread_local LookStep t_look_step;
thread_local int t_camera_update_depth = 0;

uint64_t NowUs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

// One camera update (the shared per-frame update or a controller's own). A look
// step's record lasts until the outermost update ends.
struct CameraUpdateScope {
    CameraUpdateScope() {
        if (t_camera_update_depth++ == 0) t_look_step = {};
    }
    ~CameraUpdateScope() {
        if (--t_camera_update_depth == 0) t_look_step = {};
    }
    CameraUpdateScope(const CameraUpdateScope &) = delete;
    CameraUpdateScope &operator=(const CameraUpdateScope &) = delete;
};

void RecordLookStep(uint8_t *base, uint32_t controller, float before) {
    if (!mem::IsCommitted(controller + 0x20) || !mem::IsCommitted(controller + 0x24)) return;
    t_look_step.valid = true;
    t_look_step.before = before;
    t_look_step.after = RdF(base, kPitch);
    t_look_step.middle = (RdF(base, controller + 0x20) + RdF(base, controller + 0x24)) * 0.5f;
    t_look_step.at_us = NowUs();
    t_look_step.kept = 0;
}

// After a pitch recompute. Outside a hooked camera update the record is only
// trusted for the same update (a few milliseconds), never a later frame's.
void KeepStickPitch(uint8_t *base) {
    if (!t_look_step.valid) return;
    if (t_camera_update_depth == 0 && NowUs() - t_look_step.at_us > 4000) {
        t_look_step = {};
        return;
    }
    const float delta = t_look_step.after - t_look_step.before;
    const float pitch = RdF(base, kPitch);
    const bool undoes_stick = (pitch - t_look_step.after) * delta < 0;
    const bool toward_middle =
        std::fabs(t_look_step.after - t_look_step.middle) < std::fabs(t_look_step.before - t_look_step.middle);
    if (delta != 0 && undoes_stick && toward_middle) {
        std::memcpy(base + kPitch, &t_look_step.after, sizeof(t_look_step.after));
        t_look_step.kept++;
    }
}

// The pre-shaped value to return for one right-stick axis. Read both axes so
// the dead zone and curve stay radial; the guest then applies its own cubic.
float MappedStickAxis(uint8_t *base, uint32_t axis) {
    const uint32_t state = mem::IsCommitted(kInputState) ? X86_MEM32(kInputState) : 0;
    if (!state || !mem::IsCommitted(state + kStickAxes + 1)) return 0;
    const float x = float(int8_t(X86_MEM8(state + kStickAxes))) / cl::kStickFullScale;
    const float y = float(int8_t(X86_MEM8(state + kStickAxes + 1))) / cl::kStickFullScale;
    const uint32_t percent = std::clamp(
        std::atomic_ref(input::g_controller_aim_sensitivity_percent).load(std::memory_order_relaxed),
        cl::kMinSensitivityPercent, cl::kMaxSensitivityPercent);
    const cl::Stick shaped =
        cl::Feed({x, y}, t_look_mapping.curve, cl::kRadialDeadzone, float(percent) * 0.01f);
    return axis == 0 ? shaped.x : shaped.y;
}

// sub_0001F030 is the look controllers' stick accessor. While a mapped
// controller is running, replace axes 0 and 1 with the shaped sample before
// its cubic. Any other caller and the Original response go through unchanged.
X86_EXTERN_FUNC(__imp__sub_0001F030);
X86_FUNC(sub_0001F030) {
    if (t_look_mapping.active) {
        const uint32_t axis = X86_MEM32(ctx.esp + 4);
        if (axis == 0 || axis == 1) {
            x87_push(ctx, double(MappedStickAxis(base, axis)));
            ctx.esp += 4;  // the original ends in a plain ret; its callers remove the two arguments
            return;
        }
    }
    __imp__sub_0001F030(ctx, base);
}

using GuestFunc = void (*)(X86Context &, uint8_t *);

// Some camera paths run a look controller's own input step without going
// through sub_0002E200; precision aim is one of them. Apply the same mouse aim
// there when the shared update did not, but never consume the delta twice.
// The requested heading is put back after the controller's own target pass,
// except while a lock-on target is active, where the game's snap wins.
void RunControllerAim(X86Context &ctx, uint8_t *base, GuestFunc original, GameCurve curve) {
    const uint32_t controller = ctx.ecx;
    const uint32_t camera = ActiveGameplayCamera(base, controller);
    // The response mapping follows this controller's own stick read. It is
    // active for gameplay pad look only, never for the mouse's fallback stick
    // or another camera that shares the controller method.
    ScopedLookMapping mapping(curve, camera != 0);
    const bool requested = AimRequested(controller);
    if (!camera || !input::g_direct_mouse_aim || Rd8(base, camera + kMode) == kModeVehicle) {
        const float before = RdF(base, kPitch);
        original(ctx, base);
        RecordLookStep(base, controller, before);
        return;
    }

    uint32_t old_min = 0, old_max = 0;
    bool mouse_aim = false;
    bool widen = false;
    uint8_t auto_center = 0;
    const CameraAim previous = g_camera_aim;
    if (!requested) {
        const uint8_t mode = Rd8(base, camera + kMode);
        const float pitch = ApplyAim(base, camera, controller, mode, kOnFootPitchMin, kOnFootPitchMax, mouse_aim);
        input::SetLockOnStick(mouse_aim && LockTargetActive(base) ? kLockStick : 0, 0);
        const bool recent = input::DirectAimRecent();
        g_camera_aim = {camera, controller, RdF(base, kYaw), pitch, recent, mouse_aim, true, NowMs()};
        widen = mouse_aim &&
                WidenPitch(base, controller, kOnFootPitchMin, kOnFootPitchMax, old_min, old_max);
        auto_center = Rd8(base, kAutoCenter);
        if (mouse_aim) base[kAutoCenter] = 0;
    }

    const float before = RdF(base, kPitch);
    original(ctx, base);
    RecordLookStep(base, controller, before);
    RestoreRequestedAim(base, controller);

    if (!requested) {
        if (mouse_aim) base[kAutoCenter] = auto_center;
        if (widen) RestorePitch(base, controller, old_min, old_max);
        g_camera_aim = previous;
    }
}

} // namespace

// Every camera mode. The camera update reads the active look controller and
// calls its virtual methods; injecting here, before those methods, covers the
// on-foot, precision aim, crosshair, drive-by and mission cameras in one place.
// The vehicle path is left to sub_0001FD40 below because it rebuilds its chase
// yaw every frame. Controllers using the shared orientation builder also get
// the requested yaw there, after their own target/transition calculations.
X86_EXTERN_FUNC(__imp__sub_0002E200);
X86_FUNC(sub_0002E200) {
    CameraUpdateScope scope;
    const uint32_t camera = ctx.ecx;
    const uint32_t controller = camera ? Rd(base, camera + kActive) : 0;
    const uint8_t mode = camera ? Rd8(base, camera + kMode) : 0;

    const bool owned = controller && Rd(base, controller + kOwner) == camera;
    if (input::g_direct_mouse_aim && owned && mode != kModeVehicle && IsGameplayCamera(base, camera)) {
        uint32_t old_min = 0, old_max = 0;
        bool mouse_aim = false;
        const float pitch = ApplyAim(base, camera, controller, mode, kOnFootPitchMin, kOnFootPitchMax, mouse_aim);
        input::SetLockOnStick(mouse_aim && LockTargetActive(base) ? kLockStick : 0, 0);
        const bool recent = input::DirectAimRecent();
        const bool widen = mouse_aim &&
                           WidenPitch(base, controller, kOnFootPitchMin, kOnFootPitchMax, old_min, old_max);
        // The look controller's input step centers the pitch when the stick is
        // idle. Suppress that for this update, or it pulls the view straight
        // back to the rest angle the mouse just moved away from.
        const uint8_t auto_center = Rd8(base, kAutoCenter);
        if (mouse_aim) base[kAutoCenter] = 0;
        const CameraAim previous = g_camera_aim;
        g_camera_aim = {camera, controller, RdF(base, kYaw), pitch, recent, mouse_aim, true, NowMs()};
        __imp__sub_0002E200(ctx, base);
        g_camera_aim = previous;
        if (mouse_aim) base[kAutoCenter] = auto_center;
        if (widen) RestorePitch(base, controller, old_min, old_max);
        return;
    }
    __imp__sub_0002E200(ctx, base);
}

// In vehicle.  sub_00024040 (mode 13) rebuilds its chase yaw each frame and
// calls this method to turn the yaw/pitch globals into the camera orientation.
// Injecting before this call puts the mouse angles in the same place the PC
// fix does, and holding them across frames gives the same absolute aim instead
// of a per-frame nudge that the chase camera would erase.
//
// The method is shared by every look controller, so it only touches the
// active gameplay controller. Non-vehicle updates carry a scoped requested
// heading from sub_0002E200, including unarmed and transition controllers.
X86_EXTERN_FUNC(__imp__sub_0001FD40);
X86_FUNC(sub_0001FD40) {
    static bool held = false;
    static float yaw = 0, pitch = 0;
    static uint64_t last_vehicle_at = 0;
    static uint32_t last_vehicle_camera = 0, last_vehicle_controller = 0;

    const uint32_t controller = ctx.ecx;
    // Applying this after sub_0002E200 returns is too late: this method has
    // already used the controller's replacement yaw to build the view. The same
    // update also rewrites the pitch to the controller's rest angle before
    // coming here, so put the held mouse pitch back as well.
    RestoreRequestedAim(base, controller);
    if (input::g_direct_mouse_aim) {
        const uint32_t camera = GameplayCamera(base, controller);
        if (camera && Rd(base, camera + kActive) == controller && Rd8(base, camera + kMode) != kModeVehicle)
            held = false;
        if (camera && Rd8(base, camera + kMode) == kModeVehicle && Rd(base, camera + kActive) == controller) {
            // every look controller calls this shared method, so only the
            // vehicle controller updates the hold; an unrelated call must not
            // clear it and force the chase pitch back in
            const uint64_t now = NowMs();
            if (now - last_vehicle_at > 500 || !input::DirectAimRecent() ||
                camera != last_vehicle_camera || controller != last_vehicle_controller)
                held = false;
            last_vehicle_at = now;
            last_vehicle_camera = camera;
            last_vehicle_controller = controller;
            input::UseDirectMouseAim();

            float dx = 0, dy = 0;
            if (input::ConsumeMouseAim(dx, dy)) {
                if (!held) {
                    // start from the chase angles the vehicle controller just
                    // computed, exactly like the PC fix seeding its accumulator
                    yaw = RdF(base, kYaw);
                    pitch = RdF(base, kPitch);
                    held = true;
                }
                const float percent = float(std::clamp(input::g_mouse_sensitivity_percent, 10u, 400u)) * 0.01f;
                const float vertical =
                    float(std::clamp(input::g_mouse_vertical_sensitivity_percent, 10u, 400u)) * 0.01f;
                const uint32_t game = Rd(base, kGame);
                const float yaw_sign = Rd8(base, game + kInvertX) ? -1.0f : 1.0f;
                // Here the accumulator is positive looking down as well, so
                // the default does not negate dy; invert-Y flips that.
                const float pitch_sign = Rd8(base, game + kInvertY) ? -1.0f : 1.0f;
                yaw += dx * kRadiansPerPixel * percent * yaw_sign;
                pitch = std::clamp(pitch + dy * kRadiansPerPixel * percent * vertical * pitch_sign, kVehiclePitchMin,
                                   kVehiclePitchMax);
            }

            if (held && input::DirectAimRecent()) {
                std::memcpy(base + kYaw, &yaw, sizeof(yaw));
                std::memcpy(base + kPitch, &pitch, sizeof(pitch));
            } else {
                // 5 s without a sample: let the game's chase camera take over
                held = false;
            }
        }
    }

    __imp__sub_0001FD40(ctx, base);
}

// Precision aim (mode 6) and the crosshair/on-foot controllers run their own
// input step. Some camera paths do not call sub_0002E200 at all, so these are
// the fallback that keeps the mouse live there. They also put the requested
// heading back after the controller's target/lock-on pass, which would
// otherwise leave the view on the target while the mouse is moving.
X86_EXTERN_FUNC(__imp__sub_000205A0);
X86_FUNC(sub_000205A0) { RunControllerAim(ctx, base, __imp__sub_000205A0, GameCurve::RadialCubic); }

X86_EXTERN_FUNC(__imp__sub_00029A40);
X86_FUNC(sub_00029A40) { RunControllerAim(ctx, base, __imp__sub_00029A40, GameCurve::RadialCubic); }

X86_EXTERN_FUNC(__imp__sub_00025910);
X86_FUNC(sub_00025910) { RunControllerAim(ctx, base, __imp__sub_00025910, GameCurve::PerAxisCubic); }

// The look controller used while L2 is held. Its stick step matches
// sub_000205A0 (axes 0 and 1, magnitude cubed). Only the right-stick response
// and the pinned-pitch fix apply here; the mouse paths above are unchanged.
X86_EXTERN_FUNC(__imp__sub_0002A7B0);
X86_FUNC(sub_0002A7B0) {
    const uint32_t controller = ctx.ecx;
    ScopedLookMapping mapping(GameCurve::RadialCubic, ActiveGameplayCamera(base, controller) != 0);
    const float before = RdF(base, kPitch);
    __imp__sub_0002A7B0(ctx, base);
    RecordLookStep(base, controller, before);
}

// Another look step with the same radial cubic (axes 0 and 1, magnitude cubed,
// then the controller's own pitch clamp at +0x20/+0x24).
X86_EXTERN_FUNC(__imp__sub_00027380);
X86_FUNC(sub_00027380) {
    const uint32_t controller = ctx.ecx;
    ScopedLookMapping mapping(GameCurve::RadialCubic, ActiveGameplayCamera(base, controller) != 0);
    const float before = RdF(base, kPitch);
    __imp__sub_00027380(ctx, base);
    RecordLookStep(base, controller, before);
}

// The L2 view's update: its look step, then the recompute from the camera.
X86_EXTERN_FUNC(__imp__sub_0002AAF0);
X86_FUNC(sub_0002AAF0) {
    CameraUpdateScope scope;
    __imp__sub_0002AAF0(ctx, base);
}

// The pitch recomputes from the camera position.
X86_EXTERN_FUNC(__imp__sub_0001FCA0);
X86_FUNC(sub_0001FCA0) {
    __imp__sub_0001FCA0(ctx, base);
    KeepStickPitch(base);
}

X86_EXTERN_FUNC(__imp__sub_000225B0);
X86_FUNC(sub_000225B0) {
    __imp__sub_000225B0(ctx, base);
    KeepStickPitch(base);
}

X86_EXTERN_FUNC(__imp__sub_0002CFC0);
X86_FUNC(sub_0002CFC0) {
    __imp__sub_0002CFC0(ctx, base);
    KeepStickPitch(base);
}

// The blend from the car to the on-foot camera after leaving a vehicle (about 2.5 s). While 0x348988 is set, the
// on-foot controller's update (0x28C50) asks this routine first and, when it returns true, skips its whole look and
// orientation step, so the mouse moves the yaw global but the view ignores it. The routine ends the blend itself by
// clearing that flag and its counter at 0x348B9C; when the mouse moves, end it the same way so the mouse takes the
// camera straight away. Untouched, the game's own blend plays as before.
X86_EXTERN_FUNC(__imp__sub_00021C10);
X86_FUNC(sub_00021C10) {
    constexpr uint32_t kExitBlend = TC_ADDR(0x348988), kExitBlendCount = TC_ADDR(0x348B9C);
    if (input::g_direct_mouse_aim && base[kExitBlend] && input::MouseMovedRecently()) {
        base[kExitBlend] = 0;
        X86_MEM32(kExitBlendCount) = 0;
    }
    __imp__sub_00021C10(ctx, base);
}
