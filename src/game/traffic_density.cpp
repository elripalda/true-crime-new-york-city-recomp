// Traffic density: how many ambient cars the game keeps.
//
// Road spawn points (message handler 0x18C80) add a car while the point's own count ([point+0x82]) is below
// its cap ([point+0x84]) and the world's ambient car count ([0x360E00]+0xC0) is below the budget 0x14120
// returns: the area's value at [0x360E00]+0x2AB8, or 4 while 0x173B90 holds (compared at 0x18DDD and 0x18EBB).
// The Traffic density setting scales that budget by 1.5x to 5x.

#include <atomic>
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
    static int32_t last = -1;
    if (budget != last) {
        last = budget;
        const uint32_t game = X86_MEM32(0x360E00);
        LOGR("traffic: car budget %d -> %d (cars now %d)", budget, scaled, game ? int32_t(X86_MEM32(game + 0xC0)) : -1);
    }
    ctx.eax = uint32_t(scaled);
}

// The road spawn point's own cap ([point+0x84], int16, compared with its count [point+0x82] at 0x18DC7 and
// 0x18E2B) stopped traffic at ~16 cars against a budget of 30 at 3x, so it is scaled the same way for the
// duration of the point's handler.
X86_EXTERN_FUNC(__imp__sub_00018C80);
X86_FUNC(sub_00018C80) {
    const uint32_t level = std::atomic_ref(app::Get().traffic_density).load(std::memory_order_relaxed);
    const uint32_t point = ctx.ecx;
    if (level == 0 || level > 5 || !point) return __imp__sub_00018C80(ctx, base);
    const int16_t cap = int16_t(X86_MEM16(point + 0x84));
    const int32_t scaled = game::choices::TrafficScale(cap, level);
    X86_MEM16(point + 0x84) = uint16_t(int16_t(scaled > 0x7FFF ? 0x7FFF : scaled));
    __imp__sub_00018C80(ctx, base);
    if (int16_t(X86_MEM16(point + 0x84)) == int16_t(scaled)) X86_MEM16(point + 0x84) = uint16_t(cap);
}
