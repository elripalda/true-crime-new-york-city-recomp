#include "game_addresses.h"
// Object detail: how far meshes keep each detail level.
//
// A scene node keeps its meshes as a list at node+0x2C, highest detail first: each entry has its switch
// depth at +0x08 (0: no limit) and the next, lower-detail entry at +0x04. The selectors follow the list while
// the camera-space depth exceeds the entry's switch depth:
//   0x21C330  standalone selector, stdcall (list in ecx, depth), from 0x61A11 and 0x21E267
//   0x21CF50  scene walker, depth is the 4th argument
//   0x21DC00  scene walker, depth is the 5th argument (also the inline compare at 0x21E057)
// The two walkers recurse into child nodes with the same depth, so only calls from outside them are scaled.
//
// Vehicles are left at the game's own depth: a car only sets up its paint and textures once the game itself
// picks the detailed mesh, so forcing that mesh early draws distant parked cars untextured (black) until the
// player walks up to them. Every selector call made while a vehicle draw routine runs is left alone:
//   0x13F4C0  the car body draw: 0x21E1A0 at 0x13F5C5 and again at 0x13F7BD, both on the vehicle's node
//   0x142DB0  the vehicle part draw beside it (four 0x21E1A0 calls on the [esi+0x2C] node), in the same vehicle
//             code as 0x142CF0, which wraps 0x13F4C0
// A scope flag covers every path under them, not just one return address.
//
// The object_detail setting scales the depth those selectors see: 1 (original), 1/2, 1/4, or 0 (always the
// highest detail). Distant pedestrians are not affected: they are LowPed.XB2 stand-ins (0x1B4730, up to
// seven a frame) until the game swaps in a real pedestrian close by.

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {

float Scale() {
    // a single aligned 32-bit field the menu stores; read on the game thread many times a frame
    const uint32_t level = std::atomic_ref(app::Get().object_detail).load(std::memory_order_relaxed);
    constexpr float kScales[] = {1.0f, 0.5f, 0.25f, 0.0f};
    return kScales[level < 4 ? level : 0];
}

// set while a vehicle draw routine runs on this (the game's) thread
thread_local uint32_t t_vehicle = 0;
// Object types (vtables) that keep the setting inside the dynamic-object draw 0x13F4C0. That routine draws
// every moving object (cars, characters, spawners), so it is scoped as a whole above; 0x2D5A7C is the active
// car and paints its detailed mesh at any distance. Parked cars that have not been woken (touched) carry only
// their low-detail textures and drew bare with the detailed mesh, so other types keep the game's own depth.
// XBE_VEHICLE_LOD (comma-separated hex vtables) replaces the list, for testing other types.
thread_local bool t_allowed = false;
bool AllowedType(uint32_t vtable) {
    static uint32_t list[16];
    static int n = -1;
    if (n < 0) {
        n = 0;
        if (const char *env = std::getenv("XBE_VEHICLE_LOD"); !env) {
            list[n++] = TC_ADDR(0x2D5A7C);  // the active car
            list[n++] = TC_ADDR(0x2D9654);  // ambient traffic: checked at Maximum, no bare or black cars (2026-11-15)        } else {
            const char *p = env;
            while (*p && n < 16) {
                char *end;
                const unsigned long v = std::strtoul(p, &end, 16);
                if (end == p) break;
                list[n++] = uint32_t(v);
                p = *end ? end + 1 : end;
            }
        }
    }
    for (int i = 0; i < n; ++i)
        if (list[i] == vtable) return true;
    return false;
}

// scales the float argument at esp + offset unless the call comes from inside a walker
void ScaleDepth(X86Context &ctx, uint8_t *base, uint32_t offset) {
    const float scale = Scale();
    if (scale == 1.0f) return;
    const uint32_t ret = X86_MEM32(ctx.esp);
    // recursion from inside a walker (0x21CF50..0x21D0E0, 0x21DC00..0x21E1A0) passes an already scaled depth
    if ((ret >= TC_ADDR(0x21CF50) && ret < TC_ADDR(0x21D0E0)) || (ret >= TC_ADDR(0x21DC00) && ret < TC_ADDR(0x21E1A0))) return;
    // the standalone selector call from 0x61A11 kept the setting under a vehicle in a49f21d too
    if (t_vehicle && ret != TC_ADDR(0x61A16) && !t_allowed) return;
    float depth;
    std::memcpy(&depth, base + ctx.esp + offset, 4);
    depth *= scale;
    std::memcpy(base + ctx.esp + offset, &depth, 4);
}

} // namespace

X86_EXTERN_FUNC(__imp__sub_0013F4C0);
X86_FUNC(sub_0013F4C0) {
    t_vehicle++;
    __imp__sub_0013F4C0(ctx, base);
    t_vehicle--;
}

X86_EXTERN_FUNC(__imp__sub_00142DB0);
X86_FUNC(sub_00142DB0) {
    t_vehicle++;
    __imp__sub_00142DB0(ctx, base);
    t_vehicle--;
}

X86_EXTERN_FUNC(__imp__sub_0021C330);
X86_FUNC(sub_0021C330) {
    ScaleDepth(ctx, base, 0x04);
    __imp__sub_0021C330(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0021CF50);
X86_FUNC(sub_0021CF50) {
    ScaleDepth(ctx, base, 0x10);
    __imp__sub_0021CF50(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0021DC00);
X86_FUNC(sub_0021DC00) {
    ScaleDepth(ctx, base, 0x14);
    __imp__sub_0021DC00(ctx, base);
}

// sets which object the selectors below 0x21E1A0 belong to while the dynamic-object draw runs
X86_EXTERN_FUNC(__imp__sub_0021E1A0);
X86_FUNC(sub_0021E1A0) {
    const uint32_t ret0 = X86_MEM32(ctx.esp);
    const bool was = t_allowed;
    if (ret0 == TC_ADDR(0x13F5CA) || ret0 == TC_ADDR(0x13F7C2)) {
        const uint32_t obj = X86_MEM32(ctx.esp + 8), vt = X86_MEM32(obj);
        t_allowed = AllowedType(vt);
        static const bool trace = std::getenv("XBE_TRACE_VEHICLES") != nullptr;
        if (trace) {  // vtable census of objects drawn by the dynamic-object body draw (execution plan 4.1)
            static uint32_t vts[32] = {}, counts[32] = {}, n = 0, total = 0;
            uint32_t k = 0;
            while (k < n && vts[k] != vt) ++k;
            if (k == n && n < 32) {
                vts[n++] = vt;
                LOGR("vehicles: new type vtable %08X obj %08X model %08X (ret %08X)", vt, obj, X86_MEM32(obj + 0x448), ret0);
            }
            if (k < 32) ++counts[k];
            if ((++total & 0x3FFF) == 0)
                for (uint32_t i = 0; i < n; ++i) LOGR("vehicles: vtable %08X drawn %u times", vts[i], counts[i]);
        }
    }
    __imp__sub_0021E1A0(ctx, base);
    t_allowed = was;
}

// Instance draw distance. The world render 0x144250 draws each visible city instance (the per-frame list at
// city + 0x5AD4) only when its size, returned on the x87 stack by 0x137C00, is larger than its camera depth
// times the shared constant at 0x2D2AD4 (0x144670..0x144680): a screen-size cull. Scaling the size returned to
// that one call keeps instances (building detail, street furniture) visible proportionally further.
// XBE_INSTANCE_DISTANCE=<factor> for testing.
X86_EXTERN_FUNC(__imp__sub_00137C00);
X86_FUNC(sub_00137C00) {
    static const double factor = [] {
        const char *e = std::getenv("XBE_INSTANCE_DISTANCE");
        const double f = e ? std::atof(e) : 1.0;
        return f > 0.0 ? f : 1.0;
    }();
    const uint32_t ret = X86_MEM32(ctx.esp);
    __imp__sub_00137C00(ctx, base);
    if (ret == 0x00144670 && std::getenv("XBE_INSTANCE_TRACE")) {
        static uint32_t calls = 0, small = 0;
        ++calls;
        if (X87_ST(0) < 1.0) ++small;
        if ((calls & 0xFFFF) == 0) LOGR("instance cull: %u calls, last size %.3f, %u under 1.0", calls, X87_ST(0), small);
    }
    if (factor != 1.0 && ret == 0x00144670) X87_ST(0) *= factor;
}
