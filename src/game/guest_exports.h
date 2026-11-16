// Variables the recompiled game defines for the hooks to tune (generated x86_recomp.0.cpp): the
// relocated instance and light tables, ring instance distances, draw radius and portal slots.
//
// Windows loads the recompiled game as TrueCrimeNYC.dll and finds them by name. Linux links the
// same generated code into the executable, so it takes their addresses directly. Callers get null
// when a variable is missing and keep the game's own behaviour.
#pragma once

#include <cstdint>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
extern "C" {
extern uint32_t xbe_inst_table, xbe_inst_cap, xbe_inst_count, xbe_light_table;
extern float xbe_draw_radius_scale, xbe_ring_instance_scale, xbe_inner_instance_scale;
extern uint32_t xbe_portal_slot_hits[2], xbe_extra_slot_count, xbe_extra_slots[128];
}
#endif

namespace game {

template <typename T>
T* GuestExport(const char* name) {
#ifdef _WIN32
    const HMODULE module = GetModuleHandleW(L"TrueCrimeNYC.dll");
    return module ? reinterpret_cast<T*>(GetProcAddress(module, name)) : nullptr;
#else
    struct Entry {
        const char* name;
        void* address;
    };
    static const Entry kExports[] = {
        {"xbe_inst_table", &xbe_inst_table},
        {"xbe_inst_cap", &xbe_inst_cap},
        {"xbe_inst_count", &xbe_inst_count},
        {"xbe_light_table", &xbe_light_table},
        {"xbe_draw_radius_scale", &xbe_draw_radius_scale},
        {"xbe_ring_instance_scale", &xbe_ring_instance_scale},
        {"xbe_inner_instance_scale", &xbe_inner_instance_scale},
        {"xbe_portal_slot_hits", xbe_portal_slot_hits},
        {"xbe_extra_slot_count", &xbe_extra_slot_count},
        {"xbe_extra_slots", xbe_extra_slots},
    };
    for (const Entry& e : kExports)
        if (std::strcmp(e.name, name) == 0) return static_cast<T*>(e.address);
    return nullptr;
#endif
}

} // namespace game
