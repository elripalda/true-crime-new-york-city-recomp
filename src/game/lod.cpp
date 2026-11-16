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

X86_EXTERN_FUNC(__imp__sub_0021E1A0);

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
constexpr uint32_t kActiveCarVtable = TC_ADDR(0x2D5A7C);
constexpr uint32_t kTrafficCarVtable = TC_ADDR(0x2D9654);
// Vehicle lamps. Before each body draw (0x13F585, 0x13F7A0) the dynamic-object draw sets material bit 0x8000 (lamps
// lit) when the low 16 bits of [object+0x70] are below the night amount at 0x363D44, which climbs from 0 to 0xFFFF
// through dusk and falls through dawn, so lamps come on object by object. [object+0x70] is the object's X position,
// a float: its low bits change with every movement, so a moving car's lamps switch on and off every frame through
// dusk and dawn. Distant cars drawn by the city instance draw get the bit from 0x136E70 the same way. The draws here
// compare the night amount against a fixed number per object instead.
constexpr uint32_t kNightAmount = TC_ADDR(0x363D44);
constexpr uint32_t kMaterialFlags = TC_ADDR(0x3F54CC);
constexpr uint16_t kNightLights = 0x8000;
thread_local uint32_t t_lamp_object = 0;  // the object of the body draw running on this thread, 0 outside one

bool AllowedType(uint32_t vtable) {
    static uint32_t list[16];
    static int n = -1;
    if (n < 0) {
        n = 0;
        if (const char *env = std::getenv("XBE_VEHICLE_LOD"); !env) {
            list[n++] = kActiveCarVtable;
            list[n++] = kTrafficCarVtable;
        } else {
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

// a fixed 16-bit threshold for an object (its address, mixed), in place of the bits of its position
uint16_t LampThreshold(uint32_t object) { return uint16_t((object * 0x9E3779B9u) >> 16); }

void ApplyStableLamps(uint8_t *base) {
    if (!t_lamp_object) return;
    const uint16_t lights = LampThreshold(t_lamp_object) < X86_MEM16(kNightAmount) ? kNightLights : 0;
    X86_MEM16(kMaterialFlags) = (X86_MEM16(kMaterialFlags) & ~kNightLights) | lights;
}

void DrawWithStableLamps(X86Context &ctx, uint8_t *base, uint32_t object) {
    const uint32_t outer = t_lamp_object;
    t_lamp_object = object;
    ApplyStableLamps(base);
    // The stable mask is deliberately the one restored: the part and glass draws
    // that follow read the same material flags, and restoring the guest's original
    // would let their position bits switch the lamps back off mid-car.
    const uint16_t flags = X86_MEM16(kMaterialFlags);
    __imp__sub_0021E1A0(ctx, base);
    t_lamp_object = outer;
    X86_MEM16(kMaterialFlags) = flags;
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

X86_EXTERN_FUNC(__imp__sub_0009DDB0);
X86_FUNC(sub_0009DDB0) {
    // Message 16 with a render context sets up the car material before the mesh walk.
    // The guest can replace the entire mask here, including the night bit set at draw entry.
    const bool setup_draw = X86_MEM32(ctx.esp + 4) == 16 && X86_MEM32(ctx.esp + 8) != 0;
    __imp__sub_0009DDB0(ctx, base);
    if (setup_draw) ApplyStableLamps(base);
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
X86_FUNC(sub_0021E1A0) {
    const uint32_t ret0 = X86_MEM32(ctx.esp);
    const bool body = ret0 == TC_ADDR(0x13F5CA) || ret0 == TC_ADDR(0x13F7C2);
    // The distant instance draw passes the car object too. Read it only after the
    // caller check: [esp+8] is not a valid object pointer on every other path.
    const bool distant = ret0 == TC_ADDR(0x1388AF);
    uint32_t obj = 0, vt = 0;
    if ((body || distant) && mem::IsCommitted(ctx.esp + 8)) {
        obj = X86_MEM32(ctx.esp + 8);
        if (obj && mem::IsCommitted(obj)) vt = X86_MEM32(obj);
    }
    const bool was = t_allowed;
    const bool allowed = (body || distant) && AllowedType(vt);
    if (body) {
        t_allowed = allowed;
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
    // The callers that set the lamp bit from a moving value: the two body draws of the dynamic-object draw, and the
    // city instance draw 0x1387F0 for an instance carrying an object (distant traffic cars, model 0x2DA270), whose
    // bit comes from 0x136E70's test of the instance's +8 against the same night amount. Gate on the car type so a
    // non-car at one of these sites does not get a synthetic lamp threshold.
    if (allowed)
        DrawWithStableLamps(ctx, base, obj);
    else
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
    static const bool trace = std::getenv("XBE_INSTANCE_TRACE") != nullptr;
    if (ret == TC_ADDR(0x00144670) && trace) {
        static uint32_t calls = 0, small = 0;
        ++calls;
        if (X87_ST(0) < 1.0) ++small;
        if ((calls & 0xFFFF) == 0) LOGR("instance cull: %u calls, last size %.3f, %u under 1.0", calls, X87_ST(0), small);
    }
    if (factor != 1.0 && ret == TC_ADDR(0x00144670)) X87_ST(0) *= factor;
}
