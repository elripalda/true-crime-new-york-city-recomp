// Right-stick response for the gameplay camera.
//
// The guest's look controllers read the right stick as two signed bytes and
// normalise them by 128. They then cube the result: two controllers use the
// stick's magnitude (radial cubic) and one uses each axis separately. That
// makes a diagonal push weaker than a straight push at the same deflection and
// leaves a large per-axis dead zone before the curve.
//
// This file holds the host-side replacement: one radial dead zone, a quadratic
// magnitude curve that keeps the stick's direction, and an optional aim scale.
// `Feed` returns the values to hand back to the guest, pre-shaped so that the
// controller's own cubic produces `Desired` before the game's yaw/pitch
// multipliers. Keeping the guest curve untouched preserves its smoothing,
// inversion, pitch limits, aim assist and timing.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace game::controller_look {

// The guest accessor 0x1F030 divides the signed stick byte by 128.
inline constexpr float kStickFullScale = 128.0f;
// One dead zone for the whole stick, as a fraction of full deflection.
inline constexpr float kRadialDeadzone = 0.15f;
inline constexpr uint32_t kMinSensitivityPercent = 100;
inline constexpr uint32_t kMaxSensitivityPercent = 400;

enum class GameCurve {
    RadialCubic,  // sub_000205A0 / sub_00029A40: direction * magnitude^3
    PerAxisCubic, // sub_00025910: x^3 and y^3 independently
};

struct Stick {
    float x = 0, y = 0;
};

inline float Magnitude(Stick value) { return std::sqrt(value.x * value.x + value.y * value.y); }

// The look input the player should get: a radial dead zone, the remaining
// travel rescaled to 0..1, then a quadratic curve on the magnitude. Direction
// is unchanged, so equal deflections produce equal input in every direction.
inline Stick Desired(Stick raw, float deadzone = kRadialDeadzone, float sensitivity = 1.0f) {
    const float magnitude = Magnitude(raw);
    if (magnitude <= deadzone) return {};
    const float travel = std::min((magnitude - deadzone) / (1.0f - deadzone), 1.0f);
    const float curve = travel * travel;
    const float scale = curve * sensitivity / magnitude;
    return {raw.x * scale, raw.y * scale};
}

// Pre-shape a raw sample for the guest during its own cubic. The guest then
// sees the same look input as `Desired`, without the host having to duplicate
// any of the controller's per-frame state.
inline Stick Feed(Stick raw, GameCurve curve, float deadzone = kRadialDeadzone, float sensitivity = 1.0f) {
    const Stick desired = Desired(raw, deadzone, sensitivity);
    const float magnitude = Magnitude(desired);
    if (magnitude <= 0) return {};
    if (curve == GameCurve::RadialCubic) {
        // The guest turns any input vector v into v/m * m^3. Give it a vector
        // with the same direction and cbrt(wanted magnitude).
        const float shaped = std::cbrt(magnitude);
        return {desired.x / magnitude * shaped, desired.y / magnitude * shaped};
    }
    // The guest cubes each axis. Give it cbrt(axis) so each axis comes back
    // as the wanted component; this preserves the combined direction.
    return {std::cbrt(desired.x), std::cbrt(desired.y)};
}

// The guest's curve, useful to tests and to keep the comments above checked.
inline Stick GuestOutput(Stick fed, GameCurve curve) {
    const float magnitude = Magnitude(fed);
    if (curve == GameCurve::RadialCubic) {
        if (magnitude <= 0) return {};
        const float scale = magnitude * magnitude;
        return {fed.x * scale, fed.y * scale};
    }
    return {fed.x * fed.x * fed.x, fed.y * fed.y * fed.y};
}

} // namespace game::controller_look
