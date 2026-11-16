#include "common/controller_look.h"

#include <cmath>

namespace {

using namespace game::controller_look;

bool Near(float a, float b, float epsilon = 1e-5f) { return std::fabs(a - b) <= epsilon; }

bool NearMagnitude(Stick v, float expected) { return Near(Magnitude(v), expected, 2e-5f); }

bool SameDirection(Stick actual, Stick expected) {
    const float am = Magnitude(actual), em = Magnitude(expected);
    if (am <= 0 || em <= 0) return am <= 0 && em <= 0;
    return Near(actual.x / am, expected.x / em) && Near(actual.y / am, expected.y / em);
}

bool CheckEqualDeflection(GameCurve curve) {
    constexpr float kDeflections[] = {0.20f, 0.30f, 0.60f, 0.85f, 1.0f};
    constexpr float kDiagonal = 0.70710678118f;
    for (const float deflection : kDeflections) {
        const Stick horizontal{deflection, 0};
        const Stick diagonal{deflection * kDiagonal, deflection * kDiagonal};
        const Stick horizontal_out = GuestOutput(Feed(horizontal, curve), curve);
        const Stick diagonal_out = GuestOutput(Feed(diagonal, curve), curve);
        // Same deflection means the same combined look input, whichever way
        // the stick is pushed.
        if (!NearMagnitude(horizontal_out, Magnitude(Desired(horizontal)))) return false;
        if (!NearMagnitude(diagonal_out, Magnitude(horizontal_out))) return false;
        // The curve keeps the stick's direction.
        if (!SameDirection(horizontal_out, horizontal) || !SameDirection(diagonal_out, diagonal)) return false;
    }
    return true;
}

} // namespace

int main() {
    // One dead zone for the whole stick, then the rest of the travel fills the range.
    if (Magnitude(Desired({0.10f, 0})) != 0 || Magnitude(Desired({0.149f, 0})) != 0 ||
        Magnitude(Desired({0.15f, 0})) != 0 || Magnitude(Desired({0.16f, 0})) <= 0)
        return 1;
    if (!NearMagnitude(Desired({1.0f, 0}), 1.0f)) return 2;

    // 100% starts at the same full-stick output as the original cubic.
    for (const GameCurve curve : {GameCurve::RadialCubic, GameCurve::PerAxisCubic}) {
        if (!NearMagnitude(GuestOutput(Feed({1.0f, 0}, curve), curve), 1.0f)) return 3;
        if (!CheckEqualDeflection(curve)) return 4;
        // The slider scales the curve's output, not the raw stick.
        if (!NearMagnitude(GuestOutput(Feed({1.0f, 0}, curve, kRadialDeadzone, 2.0f), curve), 2.0f)) return 5;
        if (!NearMagnitude(GuestOutput(Feed({0.6f, 0}, curve, kRadialDeadzone, 1.5f), curve),
                           Magnitude(Desired({0.6f, 0}, kRadialDeadzone, 1.5f))))
            return 6;
        // Below the dead zone neither routine should receive a sample.
        const Stick quiet = Feed({0.12f, 0.05f}, curve);
        if (quiet.x != 0 || quiet.y != 0) return 7;
    }
    return 0;
}
