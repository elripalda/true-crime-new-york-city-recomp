#include "game_addresses.h"
// Traffic density: how many ambient cars the game keeps.
//
// Road spawn points (message handler 0x18C80) add a car while the point's own count ([point+0x82]) is below
// its cap ([point+0x84]) and the world's ambient car count ([0x360E00]+0xC0) is below the budget 0x14120
// returns: the area's value at [0x360E00]+0x2AB8, or 4 while 0x173B90 holds (compared at 0x18DDD and 0x18EBB).
// The Traffic density setting scales that budget by 1.5x to 5x.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>

#include "app/app.h"
#include "common/graphics_choices.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

X86_EXTERN_FUNC(__imp__sub_00014120);
X86_FUNC(sub_00014120) {
    __imp__sub_00014120(ctx, base);
    const uint32_t level = std::atomic_ref(app::Get().traffic_density).load(std::memory_order_relaxed);
    if (level == 0 || level > 5) return;
    const int32_t budget = int32_t(ctx.eax);
    const int32_t scaled = game::choices::TrafficScale(budget, level);
    static uint32_t ticks = 0;
    if ((++ticks & 0x3FFF) == 0) {
        const uint32_t g = X86_MEM32(TC_ADDR(0x360E00));
        LOGR("traffic: cars now %d of budget %d", g ? int32_t(X86_MEM32(g + 0xC0)) : -1, scaled);
    }
    static int32_t last = -1;
    if (budget != last) {
        last = budget;
        const uint32_t game = X86_MEM32(TC_ADDR(0x360E00));
        LOGR("traffic: car budget %d -> %d (cars now %d)", budget, scaled, game ? int32_t(X86_MEM32(game + 0xC0)) : -1);
    }
    ctx.eax = uint32_t(scaled);
}

// The road spawn point's own cap ([point+0x84], int16, compared with its count [point+0x82] at 0x18DC7 and
// 0x18E2B) stopped traffic at ~16 cars against a budget of 30 at 3x, so it is scaled too.
//
// It is scaled once, where the game sets it: in the two spawn point constructors (vtable 0x2DA270), 0x19230
// and the inline one in 0x17BE0, both of which take the cap as their first stack argument. An earlier version
// raised the cap only while the point's handler 0x18C80 ran and restored it afterwards; between calls a point
// could then hold more cars than its stored cap, the point was destroyed while cars still referred to it, and
// a later message to the freed point crashed the release path (0x21FC00 -> callback -> 0x23C310) with every
// preset at Maximum. Points created before the setting changes keep their cap until the area reloads.
namespace {
void ScaleCapArgument(X86Context &ctx, uint8_t *base) {
    const uint32_t level = std::atomic_ref(app::Get().traffic_density).load(std::memory_order_relaxed);
    if (level == 0 || level > 5) return;
    const int16_t cap = int16_t(X86_MEM16(ctx.esp + 4));
    if (cap <= 0) return;
    const int32_t scaled = game::choices::TrafficScale(cap, level);
    X86_MEM16(ctx.esp + 4) = uint16_t(int16_t(scaled > 0x7FFF ? 0x7FFF : scaled));
    static int logged = 0;
    if (logged++ < 3) LOGR("traffic: spawn point cap %d -> %d", cap, scaled);
}
} // namespace

X86_EXTERN_FUNC(__imp__sub_00019230);
X86_FUNC(sub_00019230) {
    ScaleCapArgument(ctx, base);
    __imp__sub_00019230(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00017BE0);
X86_FUNC(sub_00017BE0) {
    ScaleCapArgument(ctx, base);
    __imp__sub_00017BE0(ctx, base);
}

// XBE_TRACE_TRAFFIC_RANGE: the ambient car handler 0x44090 removes a car (returns -2) when its distance from the
// player passes [0x2E3500] (scaled) or [0x2E3504] (0x45181..0x4519B). Log those ranges, the far-flag ranges
// [0x2D3D9C]/[0x2D3DB4], and despawns per 5 s: a high count while cruising is cars vanishing in view.
X86_EXTERN_FUNC(__imp__sub_00044090);
X86_FUNC(sub_00044090) {
    // Hysteresis: cars spawn inside the despawn limits ([0x2E3500] scaled, [0x2E3504]), so cars near the edge
    // were removed and respawned constantly (50-120 despawns per 5 s with ~8 cars, even at Original: the game's
    // own flicker). Only traffic code reads these two constants and only this handler despawns, so they are
    // widened for its duration. Spawning is widened less (sub_00018C80), keeping the gap. Default 2.0, override
    // with XBE_TRAFFIC_DESPAWN_SCALE.
    static const float scale = [] {
        const char *e = std::getenv("XBE_TRAFFIC_DESPAWN_SCALE");
        const float s = e ? float(std::atof(e)) : 2.0f;
        return s > 0.0f ? s : 1.0f;
    }();
    const float near_limit = X86_MEMF32(0x2E3500), far_limit = X86_MEMF32(0x2E3504);
    if (scale != 1.0f) {
        X86_MEMF32(0x2E3500) = near_limit * scale;
        X86_MEMF32(0x2E3504) = far_limit * scale;
    }
    __imp__sub_00044090(ctx, base);
    if (scale != 1.0f) {
        X86_MEMF32(0x2E3500) = near_limit;
        X86_MEMF32(0x2E3504) = far_limit;
    }
    static const bool trace = std::getenv("XBE_TRACE_TRAFFIC_RANGE") != nullptr;
    if (!trace) return;
    static uint32_t despawns = 0;
    static auto last = std::chrono::steady_clock::now();
    if (int32_t(ctx.eax) == -2) ++despawns;
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) return;
    last = now;
    const uint32_t game = X86_MEM32(0x360E00);
    LOGR("traffic range: despawn %.1f / %.1f, far flag %.1f / %.1f, despawns %u per 5 s, cars %d",
         X86_MEMF32(0x2E3500), X86_MEMF32(0x2E3504), X86_MEMF32(0x2D3D9C), X86_MEMF32(0x2D3DB4), despawns,
         game ? int32_t(X86_MEM32(game + 0xC0)) : -1);
    despawns = 0;
}

// Spawn range: road spawn points (handler 0x18C80) only add cars within the same limits ([0x2E3500] scaled,
// [0x2E3504]), about 30 units out, so new cars popped in close. Widen them 1.5x for the handler's duration so cars
// appear farther away; the despawn limits above are widened more (2x) so a new car is not removed straight away.
// Static constants only, restored after the call. XBE_TRAFFIC_SPAWN_SCALE overrides 1.5.
X86_EXTERN_FUNC(__imp__sub_00018C80);
X86_FUNC(sub_00018C80) {
    static const float scale = [] {
        const char *e = std::getenv("XBE_TRAFFIC_SPAWN_SCALE");
        const float s = e ? float(std::atof(e)) : 1.5f;
        return s > 0.0f ? s : 1.0f;
    }();
    if (scale == 1.0f) return __imp__sub_00018C80(ctx, base);
    const float near_limit = X86_MEMF32(0x2E3500), far_limit = X86_MEMF32(0x2E3504);
    X86_MEMF32(0x2E3500) = near_limit * scale;
    X86_MEMF32(0x2E3504) = far_limit * scale;
    __imp__sub_00018C80(ctx, base);
    X86_MEMF32(0x2E3500) = near_limit;
    X86_MEMF32(0x2E3504) = far_limit;
}

// Minimum spawn distance. In 0x18C80 a road spawn point closer than the near limit [0x2E3500] always spawns a car
// (0x18E41..0x18E68: no lower bound), so cars appeared right next to the player. The distance it compares comes from
// 0x15420(&out, 0) returning to 0x18E46; when it is under the minimum, report it as beyond the far limit so that
// point skips this spawn. Off by default (original behaviour); XBE_TRAFFIC_MIN_SPAWN=<distance> to test (10-20 tried).
X86_EXTERN_FUNC(__imp__sub_00015420);
X86_FUNC(sub_00015420) {
    const uint32_t ret = X86_MEM32(ctx.esp), out = X86_MEM32(ctx.esp + 4);
    __imp__sub_00015420(ctx, base);
    if (ret != 0x00018E46u || !out) return;
    static const float min_distance = [] {
        const char *e = std::getenv("XBE_TRAFFIC_MIN_SPAWN");
        return e ? float(std::atof(e)) : 0.0f;
    }();
    if (min_distance > 0.0f && X86_MEMF32(out) < min_distance) X86_MEMF32(out) = X86_MEMF32(0x2E3504) + 1.0f;
}
