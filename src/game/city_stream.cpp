#include "game_addresses.h"
// City streaming probe (Phase 0 of docs/city-streaming-plan.md).
//
// Nothing here changes game behaviour. With XBE_TRACE_CITY=1 it logs, for the nine fixed city tile slots:
//   - each load request made through 0x1378F0 (grid x/y, size, stream handle);
//   - every change of a slot's grid position, loaded flag or handle, with the physical-memory use at the time;
//   - a five-second summary of the nine slots and the physical/virtual/system memory use.
// The city object is [0x37F8FC]; its slots start at +0xE0, stride 0x8B0 (x at +0x04, y at +0x08, stream handle
// at +0x0C, size at +0x28, loaded flag at +0x50). 0x13D880 initialises them; the draw loop at 0x13F1B0 calls
// 0x13C4C0 nine times a frame, which is used as the frame tick for sampling.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "core/log.h"
#include "game/city_grid.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kCityPtr = TC_ADDR(0x37F8FC);
constexpr uint32_t kSlots = 0xE0, kSlotStride = 0x8B0, kSlotCount = 9;
constexpr uint32_t kX = 0x04, kY = 0x08, kHandle = 0x0C, kSize = 0x28, kLoaded = 0x50;

bool Trace() {
    static const bool on = std::getenv("XBE_TRACE_CITY") != nullptr;
    return on;
}

uint64_t PhysicalUsed();
uint64_t PhysicalFree();

// Phase 1 call trace: counts the selected outside-streamer functions and records the first caller. One line
// per order of magnitude keeps the log short while showing whether a pass runs per frame, per recentre, or
// only on a specific game path.
struct TraceEntry {
    uint32_t addr;
    std::atomic<uint32_t> count{0}, first{0}, bucket{0};
};

TraceEntry g_trace[] = {
    {TC_ADDR(0x146CA0)}, {TC_ADDR(0x146CC8)}, {TC_ADDR(0x146D66)}, {TC_ADDR(0x146FC0)}, {TC_ADDR(0x146FE8)}, {TC_ADDR(0x1472E0)}, {TC_ADDR(0x147308)},
    {TC_ADDR(0x137600)}, {TC_ADDR(0x13D750)}, {TC_ADDR(0x13D350)},
    {TC_ADDR(0x13A1B0)}, {TC_ADDR(0x13A480)}, {TC_ADDR(0x13FBE0)}, {TC_ADDR(0x1400DA)}, {TC_ADDR(0x145350)}, {TC_ADDR(0x145AC6)},
    {TC_ADDR(0x13CF40)}, {TC_ADDR(0x13E7F0)}, {TC_ADDR(0x13E91D)}, {TC_ADDR(0x13EB08)}, {TC_ADDR(0x13F840)},
    {TC_ADDR(0x138B90)}, {TC_ADDR(0x13ABB0)}, {TC_ADDR(0x13A850)}, {TC_ADDR(0x1420A0)}, {TC_ADDR(0x14A240)},
    {TC_ADDR(0x13CB50)}, {TC_ADDR(0x144250)}, {TC_ADDR(0x14F460)}, {TC_ADDR(0x142C90)},
};

// Step 1 diagnostic: who holds which tile object, and do two cells ever hold the same one?
struct DiagCell {
    uint32_t addr = 0;
    int32_t x = 0, y = 0;
    uint32_t state = 0, loaded = 0, obj = 0;
    bool valid = false;
};
DiagCell g_diag[96];
struct DiagPair {
    uint32_t a = 0, b = 0;
    bool valid = false;
};
DiagPair g_diag_pairs[256];
uint64_t g_diag_same = 0, g_diag_diff = 0;
std::chrono::steady_clock::time_point g_diag_last;

const char *DiagRole(uint32_t object, uint32_t city, uint32_t manager) {
    if (object >= manager + 0x10 && object < manager + 0x10 + 9u * 0x8B0) return "game-slot";
    if (object >= city + 0x4FA4 && object < city + 0x4FA4 + 9u * 0x9C) return "game-cell";
    if (game::city_grid::Ready()) {
        const uint32_t count = uint32_t(game::city_grid::N()) * uint32_t(game::city_grid::N());
        const uint32_t slots = game::city_grid::Slots(), cells = game::city_grid::Cells();
        if (object >= slots && object < slots + count * 0x8B0) return "store-slot";
        if (object >= cells && object < cells + count * 0x9C) return "store-cell";
    }
    return "other";
}

void DiagTransition(uint8_t *base, uint32_t object, uint32_t city, uint32_t manager, int32_t target_x,
                    int32_t target_y, uint32_t caller) {
    const char *role = DiagRole(object, city, manager);
    if (role[0] == 'o') return;
    const bool cell = std::strcmp(role, "game-cell") == 0 || std::strcmp(role, "store-cell") == 0;
    const int32_t x = int32_t(X86_MEM32(object + 0x04)), y = int32_t(X86_MEM32(object + 0x08));
    const uint32_t state = X86_MEM32(object + 0x24);
    const uint32_t loaded = cell ? (X86_MEM8(object + 0x88) ? 1u : 0u) : X86_MEM32(object + 0x50);
    const uint32_t obj = cell ? X86_MEM32(object + 0x80) : X86_MEM32(object + 0x30);

    DiagCell *slot = nullptr, *free_slot = nullptr;
    for (DiagCell &e : g_diag) {
        if (e.valid && e.addr == object) {
            slot = &e;
            break;
        }
        if (!e.valid && !free_slot) free_slot = &e;
    }
    if (!slot) slot = free_slot;
    if (!slot) return;
    const bool changed = !slot->valid || slot->x != x || slot->y != y || slot->state != state ||
                         slot->loaded != loaded || slot->obj != obj;
    if (!changed) return;
    LOGR("diag: %s %08X tile %d,%d target %d,%d state %u loaded %u obj %08X caller %08X", role, object, x, y,
         target_x, target_y, state, loaded, obj, caller);
    *slot = {object, x, y, state, loaded, obj, true};
}

bool DiagNewPair(uint32_t a, uint32_t b) {
    for (DiagPair &p : g_diag_pairs) {
        if (p.valid && p.a == a && p.b == b) return false;
        if (!p.valid && !p.a) {
            p = {a, b, true};
            return true;
        }
    }
    return false;
}

void DiagDuplicates(uint8_t *base, uint32_t city) {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_diag_last < std::chrono::milliseconds(500)) return;
    g_diag_last = now;

    uint32_t addrs[64], objs[64], groups[64];
    int32_t xs[64], ys[64];
    const char *roles[64];
    int n = 0;
    for (int i = 0; i < 9; ++i) {
        const uint32_t cell = city + 0x4FA4 + uint32_t(i) * 0x9C;
        const uint32_t obj = X86_MEM32(cell + 0x80);
        if (!obj) continue;
        addrs[n] = cell;
        objs[n] = obj;
        groups[n] = X86_MEM32(cell + 0x38);
        xs[n] = int32_t(X86_MEM32(cell + 0x04));
        ys[n] = int32_t(X86_MEM32(cell + 0x08));
        roles[n] = "game";
        ++n;
    }
    if (game::city_grid::Ready()) {
        const int count = game::city_grid::N() * game::city_grid::N();
        const uint32_t cells = game::city_grid::Cells();
        for (int i = 0; i < count && n < 64; ++i) {
            if (!game::city_grid::RingCell(i)) continue;
            const uint32_t cell = cells + uint32_t(i) * 0x9C;
            const uint32_t obj = X86_MEM32(cell + 0x80);
            if (!obj) continue;
            addrs[n] = cell;
            objs[n] = obj;
            groups[n] = X86_MEM32(cell + 0x38);
            xs[n] = int32_t(X86_MEM32(cell + 0x04));
            ys[n] = int32_t(X86_MEM32(cell + 0x08));
            roles[n] = "store";
            ++n;
        }
    }
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (objs[i] != objs[j] && groups[i] != groups[j]) continue;
            if (groups[i] == groups[j] && groups[i] && DiagNewPair(addrs[i] ^ 0x80000000u, addrs[j])) {
                LOGR("diag: SHARED group %08X %s %08X tile %d,%d and %s %08X tile %d,%d", groups[i], roles[i],
                     addrs[i], xs[i], ys[i], roles[j], addrs[j], xs[j], ys[j]);
            }
            if (objs[i] != objs[j]) continue;
            const bool same = xs[i] == xs[j] && ys[i] == ys[j];
            if (DiagNewPair(addrs[i], addrs[j])) {
                if (same) ++g_diag_same; else ++g_diag_diff;
                LOGR("diag: DUPLICATE obj %08X %s %08X tile %d,%d and %s %08X tile %d,%d same_tile %d",
                     objs[i], roles[i], addrs[i], xs[i], ys[i], roles[j], addrs[j], xs[j], ys[j], same);
            }
        }
    }

    static auto last_summary = now;
    if (now - last_summary >= std::chrono::seconds(5)) {
        last_summary = now;
        int loaded = 0;
        if (game::city_grid::Ready()) {
            const int count = game::city_grid::N() * game::city_grid::N();
            const uint32_t cells = game::city_grid::Cells();
            for (int i = 0; i < count; ++i)
                if (game::city_grid::RingCell(i) && X86_MEM8(cells + uint32_t(i) * 0x9C + 0x88)) ++loaded;
        }
        LOGR("diag: unique duplicate pairs same_tile %llu different_tile %llu, ring %d/%d loaded, physical %llu/%llu KB",
             static_cast<unsigned long long>(g_diag_same), static_cast<unsigned long long>(g_diag_diff),
             loaded, game::city_grid::Ready() ? game::city_grid::N() * game::city_grid::N() : 0,
             static_cast<unsigned long long>(PhysicalUsed() / 1024),
             static_cast<unsigned long long>(PhysicalFree() / 1024));
    }
}

// Effect-list diagnostic: 0x13EF50 walks the list whose head sits at [0x2E7C5C] and calls each node's
// [+0x0C]. Walk the same list safely (committed pages, 4-byte alignment) and report the first node that is
// not a valid list entry. Logging is edge-triggered so the list can be watched every call without flooding.
// Node guard: effect-list nodes are mostly static objects registered once by the initialisers at 0x2730xx
// (0x2EA780.., 0x386660, 0x3866E0), so their callbacks never legitimately change. Snapshot the static node
// blocks and every node's callback on first sight; report the first change with the bytes just before it,
// so the overflowing neighbour can be identified.
} // namespace
void NodeGuard(uint8_t *base, const char *tag) {
    static const bool enabled = std::getenv("XBE_CITY_NODEGUARD") != nullptr;
    if (!enabled) return;
    // Static nodes registered by the 0x2730xx initialisers; guard their arg (+8) and callback (+0xC).
    static const uint32_t nodes[] = {TC_ADDR(0x2EA780), TC_ADDR(0x2EA794), TC_ADDR(0x2EA7B8), TC_ADDR(0x2EA7CC), TC_ADDR(0x2EA7E0), TC_ADDR(0x2EA7F4), TC_ADDR(0x2EA808),
                                     TC_ADDR(0x2EA81C), TC_ADDR(0x2EA840), TC_ADDR(0x2EA854), TC_ADDR(0x2EA868), TC_ADDR(0x2EA87C), TC_ADDR(0x2EA890), TC_ADDR(0x2EA8A4),
                                     TC_ADDR(0x2EA8B8), TC_ADDR(0x2EA8CC), TC_ADDR(0x2EA8E0), TC_ADDR(0x2EA8F4), TC_ADDR(0x2EA908), TC_ADDR(0x2EA920), TC_ADDR(0x2EA934),
                                     TC_ADDR(0x2EA980), TC_ADDR(0x2EA9B8), TC_ADDR(0x2EA9CC), TC_ADDR(0x2EA9F0)};
    static std::vector<uint32_t> snap;
    static std::unordered_map<uint32_t, uint32_t> cbs;
    static bool armed = false, fired = false;
    if (fired) return;
    if (!armed) {
        for (uint32_t n : nodes) {
            snap.push_back(X86_MEM32(n + 8));
            snap.push_back(X86_MEM32(n + 0xC));
        }
        armed = true;
        LOGR("nodeguard armed (%s)", tag);
        return;
    }
    size_t i = 0;
    for (uint32_t n : nodes) {
        for (uint32_t a = n + 8; a <= n + 0xC; a += 4, ++i) {
            const uint32_t now = X86_MEM32(a), was = snap[i];
            if (now == was) continue;
            fired = true;
            LOGR("nodeguard %s STATIC %08X: %08X -> %08X", tag, a, was, now);
            for (uint32_t d = (a - 0x40u) & ~3u; d < a + 0x10u; d += 0x10u)
                LOGR("nodeguard   %08X: %08X %08X %08X %08X", d, X86_MEM32(d), X86_MEM32(d + 4), X86_MEM32(d + 8),
                     X86_MEM32(d + 12));
            return;
        }
    }
    const uint32_t sentinel = X86_MEM32(TC_ADDR(0x2E7C5Cu));
    if (sentinel < 0x10000u || (sentinel & 3u) || !mem::IsCommitted(sentinel)) return;
    uint32_t node = X86_MEM32(sentinel);
    for (int n = 0; node && n < 512; ++n) {
        if (node < 0x10000u || (node & 3u) || !mem::IsCommitted(node + 0x10u)) {
            fired = true;
            LOGR("nodeguard %s WILD next %08X at #%d", tag, node, n);
            return;
        }
        const uint32_t cb = X86_MEM32(node + 0x0C);
        auto [it, inserted] = cbs.emplace(node, cb);
        if (!inserted && it->second != cb) {
            fired = true;
            LOGR("nodeguard %s NODE %08X cb %08X -> %08X (arg %08X key %08X)", tag, node, it->second, cb,
                 X86_MEM32(node + 8), X86_MEM32(node + 0x10));
            for (uint32_t d = (node - 0x40u) & ~3u; d < node + 0x20u; d += 0x10u)
                LOGR("nodeguard   %08X: %08X %08X %08X %08X", d, X86_MEM32(d), X86_MEM32(d + 4), X86_MEM32(d + 8),
                     X86_MEM32(d + 12));
            return;
        }
        node = X86_MEM32(node);
    }
}

namespace {
void DiagEffectLists(uint8_t *base, const char *tag, bool always) {
    NodeGuard(base, tag);
    static const bool enabled = std::getenv("XBE_CITY_RING_LISTDIAG") != nullptr;
    if (!enabled) return;
    const auto now = std::chrono::steady_clock::now();
    static auto last = now;
    static int last_count = -1, last_bad = -1;
    const uint32_t sentinel = X86_MEM32(TC_ADDR(0x2E7C5Cu));
    uint32_t node = 0, prev = 0, bad_node = 0, bad_cb = 0, bad_prev = 0;
    int count = 0, bad = 0;
    bool corrupt = false;
    if (sentinel < 0x10000u || sentinel >= 0x7FFE0000u || (sentinel & 3u) || !mem::IsCommitted(sentinel)) {
        corrupt = true;
        bad_node = sentinel;
    } else {
        node = X86_MEM32(sentinel);
        prev = sentinel;
    }
    while (!corrupt && node && count < 512) {
        if (node < 0x10000u || node >= 0x7FFE0000u || (node & 3u) || !mem::IsCommitted(node + 0x10u)) {
            corrupt = true;
            bad_node = node;
            bad_prev = prev;
            break;
        }
        const uint32_t cb = X86_MEM32(node + 0x0C);
        if (cb != 0u && (cb < 0x1000u || cb >= TC_ADDR(0x00400000u))) {
            ++bad;
            if (!bad_node) {
                bad_node = node;
                bad_cb = cb;
                bad_prev = prev;
            }
        }
        prev = node;
        node = X86_MEM32(node);
        ++count;
    }
    if (corrupt || bad || count != last_count || always) {
        if (corrupt)
            LOGR("effwatch %s CORRUPT next %08X prev %08X after %d nodes", tag, bad_node, bad_prev, count);
        else if (bad)
            LOGR("effwatch %s BADCB node %08X cb %08X prev %08X after %d nodes", tag, bad_node, bad_cb,
                 bad_prev, count);
        else if (always || now - last >= std::chrono::seconds(5))
            LOGR("effwatch %s ok %d nodes", tag, count);
        last_count = count;
        last_bad = bad;
        last = now;
    }
}

// Draw-time guard removed: skipping a cell in 0x13C4C0 leaves the camera/fade state half-updated and causes
// worse failures later. Diagnostics stay read-only.

void TraceHit(int i, X86Context &ctx, uint8_t *base) {
    TraceEntry &e = g_trace[i];
    const uint32_t n = e.count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
        e.first.store(X86_MEM32(ctx.esp), std::memory_order_relaxed);
        LOGR("city: trace %08X first from %08X", e.addr, X86_MEM32(ctx.esp));
    }
    const uint32_t bucket = n < 10 ? n : n < 100 ? 10 : n < 1000 ? 100 : n < 10000 ? 1000
                            : n < 100000 ? 10000 : n < 1000000 ? 100000 : 1000000;
    if (bucket != e.bucket.exchange(bucket, std::memory_order_relaxed))
        LOGR("city: trace %08X calls %u (first ret %08X)", e.addr, bucket,
             e.first.load(std::memory_order_relaxed));
}

struct Slot {
    int32_t x = 0, y = 0;
    uint32_t handle = 0, size = 0, loaded = 0;
};

Slot ReadSlot(uint8_t *base, uint32_t city, int i) {
    const uint32_t slot = city + kSlots + uint32_t(i) * kSlotStride;
    Slot s;
    s.x = int32_t(X86_MEM32(slot + kX));
    s.y = int32_t(X86_MEM32(slot + kY));
    s.handle = X86_MEM32(slot + kHandle);
    s.size = X86_MEM32(slot + kSize);
    s.loaded = X86_MEM8(slot + kLoaded) ? 1u : 0u;
    return s;
}

bool Same(const Slot &a, const Slot &b) {
    return a.x == b.x && a.y == b.y && a.handle == b.handle && a.size == b.size && a.loaded == b.loaded;
}

uint64_t PhysicalUsed() { return uint64_t(mem::Query().contiguous_used) * mem::kPage; }
uint64_t PhysicalFree() {
    const uint64_t usable = mem::kPhysicalSize - mem::kKernelDataSize;
    const uint64_t used = PhysicalUsed();
    return used >= usable ? 0 : usable - used;
}

struct State {
    Slot slots[kSlotCount];
    bool have = false;
    uint64_t changes = 0, loads = 0, unloads = 0;
    uint64_t peak_used = 0, low_free = 0;
    uint64_t last_used = 0;
    int loaded = 0;
    std::chrono::steady_clock::time_point checked, summary, changed;
};

State g_state;

// Simulates the physical cost of the outer ring: hold XBE_CITY_RESERVE_KB of the physical window from 45 s into
// the run, after the city is up, so the game's own streaming has to live with the same reduced headroom that a
// 5Ã—5 or 7Ã—7 grid would leave. Purely a measurement: the block is never used.
uint32_t g_reserved[64];
int g_reserved_n = 0;

void MaybeReserve() {
    static const uint32_t kb = [] {
        const char *value = std::getenv("XBE_CITY_RESERVE_KB");
        return value ? uint32_t(std::atoi(value)) : 0u;
    }();
    static const uint32_t blocks = [] {
        const char *value = std::getenv("XBE_CITY_RESERVE_BLOCKS");
        return value ? std::max(1u, uint32_t(std::atoi(value))) : 1u;
    }();
    static bool done = false;
    static const auto start = std::chrono::steady_clock::now();
    if (!kb || done) return;
    if (std::chrono::steady_clock::now() - start < std::chrono::seconds(45)) return;
    done = true;
    const uint32_t each = kb * 1024 / blocks;
    for (uint32_t i = 0; i < blocks && g_reserved_n < 64; ++i) {
        const uint32_t at = mem::AllocContiguous(each, 0, mem::kPhysicalSize - 1, mem::kPage);
        if (!at) {
            LOGR("city: outer-ring simulation could not reserve block %u/%u (%u KB); %llu KB physical in use", i + 1,
                 blocks, each / 1024, static_cast<unsigned long long>(PhysicalUsed() / 1024));
            break;
        }
        g_reserved[g_reserved_n++] = at;
    }
    LOGR("city: outer-ring simulation reserved %d/%u blocks, %u KB total; %llu KB physical in use", g_reserved_n,
         blocks, uint32_t(g_reserved_n) * each / 1024,
         static_cast<unsigned long long>(PhysicalUsed() / 1024));
}

void LogSlots(uint8_t *base, uint32_t city) {
    char text[512];
    int at = 0;
    for (int i = 0; i < kSlotCount && at < int(sizeof text); ++i) {
        const Slot s = ReadSlot(base, city, i);
        at += std::snprintf(text + at, sizeof text - size_t(at), "%s%d:(%d,%d)%s%08X", i ? " " : "",
                            i, s.x, s.y, s.loaded ? "L" : "-", s.handle);
    }
    LOGR("city: slots %s", text);
}

void Sample(uint8_t *base, bool force) {
    const uint32_t city = X86_MEM32(kCityPtr);
    if (!city) return;
    MaybeReserve();
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - g_state.checked < std::chrono::milliseconds(50)) return;
    g_state.checked = now;

    const uint64_t used = PhysicalUsed();
    const uint64_t free = PhysicalFree();
    g_state.peak_used = used > g_state.peak_used ? used : g_state.peak_used;
    if (!g_state.low_free || free < g_state.low_free) g_state.low_free = free;

    Slot cur[kSlotCount];
    for (int i = 0; i < kSlotCount; ++i) cur[i] = ReadSlot(base, city, i);

    if (!g_state.have) {
        for (int i = 0; i < kSlotCount; ++i) g_state.slots[i] = cur[i];
        g_state.have = true;
        g_state.last_used = used;
        g_state.summary = now;
        LOGR("city: probe on, city %08X, physical %llu KB used, %llu KB free", city,
             static_cast<unsigned long long>(used / 1024), static_cast<unsigned long long>(free / 1024));
        LogSlots(base, city);
        return;
    }

    int loaded = 0;
    for (int i = 0; i < kSlotCount; ++i) loaded += cur[i].loaded ? 1 : 0;
    const int64_t delta = int64_t(used) - int64_t(g_state.last_used);
    const int64_t quiet_ms = g_state.changed.time_since_epoch().count()
                                 ? std::chrono::duration_cast<std::chrono::milliseconds>(now - g_state.changed).count()
                                 : -1;
    int transitions = 0;
    for (int i = 0; i < kSlotCount; ++i)
        if (!Same(g_state.slots[i], cur[i])) ++transitions;

    for (int i = 0; i < kSlotCount; ++i) {
        if (Same(g_state.slots[i], cur[i])) continue;
        const Slot &old = g_state.slots[i];
        if (cur[i].loaded && !old.loaded) ++g_state.loads;
        if (!cur[i].loaded && old.loaded) ++g_state.unloads;
        ++g_state.changes;
        LOGR("city: slot %d at %08X (%d,%d %s %08X size %u) -> (%d,%d %s %08X size %u); loaded %d -> %d, %d "
             "transitions in %lld ms, physical %llu KB (%+lld KB), free %llu KB",
             i, city + kSlots + uint32_t(i) * kSlotStride,
             old.x, old.y, old.loaded ? "loaded" : "empty", old.handle, old.size, cur[i].x, cur[i].y,
             cur[i].loaded ? "loaded" : "empty", cur[i].handle, cur[i].size, g_state.loaded, loaded, transitions,
             static_cast<long long>(quiet_ms), static_cast<unsigned long long>(used / 1024),
             static_cast<long long>(delta / 1024), static_cast<unsigned long long>(free / 1024));
    }
    g_state.last_used = used;
    if (transitions) g_state.changed = now;
    g_state.loaded = loaded;

    if (now - g_state.summary >= std::chrono::seconds(5)) {
        g_state.summary = now;
        const mem::Stats q = mem::Query();
        LOGR("city: 5 s: %llu changes (%llu loaded, %llu emptied), %d tiles loaded now, city %08X, physical "
             "%llu/%llu KB used, peak %llu KB, low free %llu KB, virtual %llu KB, system %llu KB",
             static_cast<unsigned long long>(g_state.changes), static_cast<unsigned long long>(g_state.loads),
             static_cast<unsigned long long>(g_state.unloads), loaded, city,
             static_cast<unsigned long long>(used / 1024),
             static_cast<unsigned long long>((mem::kPhysicalSize - mem::kKernelDataSize) / 1024),
             static_cast<unsigned long long>(g_state.peak_used / 1024),
             static_cast<unsigned long long>(g_state.low_free / 1024),
             static_cast<unsigned long long>(q.virtual_committed) * mem::kPage / 1024,
             static_cast<unsigned long long>(q.system_used) * mem::kPage / 1024);
        LogSlots(base, city);
    }

    for (int i = 0; i < kSlotCount; ++i) g_state.slots[i] = cur[i];
}

} // namespace

// 0x13D880: initialises the three nine-slot arrays. Sample once the slots exist.
X86_EXTERN_FUNC(__imp__sub_0013D880);
X86_FUNC(sub_0013D880) {
    const uint32_t manager = ctx.ecx;  // the tile manager's this; the original changes ecx
    __imp__sub_0013D880(ctx, base);
    game::city_grid::Init(base, manager);
    if (!Trace()) return;
    g_state = {};
    Sample(base, true);
}

// 0x1378F0 and 0x138070 build a tile record's .XB2 and .g names and issue its stream requests. ecx is the record.
void LogRequest(X86Context &ctx, uint8_t *base, const char *which, void (*original)(X86Context &, uint8_t *)) {
    const uint32_t tile = ctx.ecx;
    if (!tile) return original(ctx, base);
    const uint32_t ret = X86_MEM32(ctx.esp);
    const int32_t x = int32_t(X86_MEM32(tile + kX)), y = int32_t(X86_MEM32(tile + kY));
    const uint32_t size = X86_MEM32(tile + kSize);
    original(ctx, base);
    LOGR("city: %s from %08X tile %08X grid %d,%d size %u -> handle %08X", which, ret, tile, x, y, size,
         X86_MEM32(tile + kHandle));
}

X86_EXTERN_FUNC(__imp__sub_001378F0);
X86_FUNC(sub_001378F0) {
    if (!Trace()) return __imp__sub_001378F0(ctx, base);
    LogRequest(ctx, base, "XB2", __imp__sub_001378F0);
}

X86_EXTERN_FUNC(__imp__sub_00138070);
X86_FUNC(sub_00138070) {
    if (!Trace()) return __imp__sub_00138070(ctx, base);
    LogRequest(ctx, base, ".g", __imp__sub_00138070);
}

// 0x142CE0 -> 0x13F090: the city draw pass. Run the ring driver here, once per pass, before any cell is
// drawn. Driving from inside 0x13C4C0 (per cell) can leave a frame half-drawn when a tile loads or unloads.
// First argument (render context) the game passed to 0x13C4C0 in the current city draw pass. The ring must
// draw with exactly this: 0x142CE0's ecx is not it, and on some passes points at a small static object.
uint32_t g_game_cell_ctx = 0;

// Move the 16-entry light table (0x349380, count 0x3493CC) to a 256-entry guest array. grid_patch routes all
// nine table sites through xbe_light_table (exported by the game code DLL); lights are addressed by index
// only, so copying the entries and switching the base on the game thread is safe.
// Move the 300-entry visible instance list (city + 0x5AD0, see grid_patch.h kInstSites) to a 4096-entry guest
// array so the outer ring can add its tiles. The list is rebuilt every frame, so switching at the city draw
// entry is safe; the entries already there are copied for that frame.
void RelocateInstTable(uint32_t city) {
    static uint32_t *count_ref = nullptr;
    if (count_ref) {  // insert/evict reach the count through the list base (XBE_CNT); keep it on this city
        *count_ref = city + 0x6430;
        return;
    }
    static bool done = false;
    if (done || !city) return;
    done = true;
#ifdef _WIN32
    HMODULE m = GetModuleHandleW(L"TrueCrimeNYC.dll");
    auto *table = m ? reinterpret_cast<uint32_t *>(GetProcAddress(m, "xbe_inst_table")) : nullptr;
    auto *cap = m ? reinterpret_cast<uint32_t *>(GetProcAddress(m, "xbe_inst_cap")) : nullptr;
    auto *count = m ? reinterpret_cast<uint32_t *>(GetProcAddress(m, "xbe_inst_count")) : nullptr;
    if (!table || !cap || !count) {
        LOGR("instances: xbe_inst_table not exported; list stays at 300");
        return;
    }
    constexpr uint32_t kEntries = 4096;
    uint32_t at = 0, size = kEntries * 8;
    if (mem::AllocateVirtual(at, size, mem::kMemReserve | mem::kMemCommit | mem::kMemTopDown, 0x04) != 0 || !at) {
        LOGR("instances: could not allocate the larger list");
        return;
    }
    uint8_t *base = mem::Base();
    std::memset(base + at, 0, size);
    std::memcpy(base + at, base + city + 0x5AD0, 300 * 8);
    *count = city + 0x6430;
    count_ref = count;
    *table = at;
    *cap = kEntries;
    LOGR("instances: visible list moved to %08X (%u entries)", at, kEntries);
#endif
}

void RelocateLightTable() {
    static bool done = false;
    if (done) return;
    done = true;
#ifdef _WIN32
    HMODULE m = GetModuleHandleW(L"TrueCrimeNYC.dll");
    auto *table = m ? reinterpret_cast<uint32_t *>(GetProcAddress(m, "xbe_light_table")) : nullptr;
    if (!table) {
        LOGR("lights: xbe_light_table not exported; table stays at 16");
        return;
    }
    uint32_t at = 0, size = 0x1000;
    if (mem::AllocateVirtual(at, size, mem::kMemReserve | mem::kMemCommit | mem::kMemTopDown, 0x04) != 0 || !at) {
        LOGR("lights: could not allocate the larger table");
        return;
    }
    uint8_t *base = mem::Base();
    std::memset(base + at, 0, size);
    for (uint32_t i = 0; i < 16; ++i) X86_MEM32(at + 4 + i * 4) = X86_MEM32(TC_ADDR(0x349380u) + i * 4);
    *table = at + 4;  // keep one spare word below for the [base-4] read
    LOGR("lights: table moved to %08X (1023 entries)", at + 4);
#endif
}
extern int g_ring_draw_idx;
X86_EXTERN_FUNC(__imp__sub_00142CE0);
X86_FUNC(sub_00142CE0) {
    static const bool diag = std::getenv("XBE_CITY_DIAG") != nullptr;
    const uint32_t city = X86_MEM32(kCityPtr);
    if (city && game::city_grid::Ring()) {
        if (!game::city_grid::Ready()) game::city_grid::Init(base, city + 0xD0);
        game::city_grid::DriveRing(base, ctx, city, city + 0xD0);
        if (diag) DiagDuplicates(base, city);
        DiagEffectLists(base, "drive", false);
    }
    if (city && game::city_grid::Ring()) RelocateLightTable();
    if (city && game::city_grid::Ring()) RelocateInstTable(city);
    { void InstallTileDrawProbe(); InstallTileDrawProbe(); }
    if (std::getenv("XBE_CITY_DRAWPROBE") && city && game::city_grid::Ready()) {
        // Scene list at city + 0x6B48 (walked by 0x144250 -> 0x13F400): who owns its nodes?
        static auto last = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
            last = std::chrono::steady_clock::now();
            auto owner = [&](uint32_t node) -> int {
                for (uint32_t i = 0; i < 9; ++i) {
                    const uint32_t b = X86_MEM32(city + 0xE0 + i * 0x8B0 + 0x30);
                    if (b && node >= b && node < b + 0x400000u) return 0;
                }
                const int n = game::city_grid::N();
                for (int i = 0; i < n * n; ++i) {
                    const uint32_t b = X86_MEM32(game::city_grid::Slots() + uint32_t(i) * 0x8B0u + 0x30);
                    if (b && node >= b && node < b + 0x400000u) return 1;
                }
                return 2;
            };
            int counts[3] = {}, total = 0;
            const uint32_t head = X86_MEM32(city + 0x6B48);
            for (uint32_t e = head; e && total < 100000; e = X86_MEM32(e), ++total) {
                if (total && e == head) break;
                if (!mem::IsCommitted(e + 8)) break;
                counts[owner(X86_MEM32(e + 8))]++;
            }
            {
                static uint32_t peak = 0;
                const uint32_t cnt = X86_MEM32(city + 0x6430);
                peak = cnt > peak ? cnt : peak;
                LOGR("drawprobe: visible instances %u (peak %u of 300)", cnt, peak);
            }
            LOGR("drawprobe: scene list %d nodes: game slots %d, ring slots %d, other %d", total, counts[0], counts[1], counts[2]);
        }
    }
    { extern bool g_ring_instances_done; g_ring_instances_done = false; }
    g_game_cell_ctx = 0;
    __imp__sub_00142CE0(ctx, base);
    {   // Light table watch: count at 0x3493CC, count at 0x3493CC (no bounds check on add).
        static uint32_t high = 0;
        const uint32_t cnt = X86_MEM32(TC_ADDR(0x3493CCu));
        if (cnt > high && cnt < 0x1000u) {
            high = cnt;
            LOGR("lights high-water %u", cnt);
        }
    }
    NodeGuard(base, "gamedraw");
    static const bool alternate = std::getenv("XBE_CITY_RING_ALTERNATE") != nullptr;  // A/B captures
    static uint32_t passes = 0;
    const bool skip_alt = alternate && (++passes & 1u);
    if (alternate && (passes % 600u) < 2u) LOGR("ring alternate: pass %u ring %s", passes, skip_alt ? "off" : "on");
    // The ring normally draws inside the distant pass (0x13F1B0 hook below), with the game's own render states.
    // XBE_CITY_RING_DRAW_LATE=1 keeps the old draw here, after the pass (states already reset: see-through).
    static const bool late = std::getenv("XBE_CITY_RING_DRAW_LATE") != nullptr;
    if (late && city && game::city_grid::Ring() && !skip_alt && std::getenv("XBE_CITY_RING_NODRAW") == nullptr)
        if (g_game_cell_ctx) game::city_grid::DrawRing(base, ctx, g_game_cell_ctx);
    if (city && game::city_grid::Ring()) DiagEffectLists(base, "drawdone", false);
}

// 0x14B390: the scene submit pass; it calls the effect manager 0x13EF50 at 0x14B5AE. Watch the list at its
// entry so a node corrupted earlier in the pass is reported before the manager dereferences it.
X86_EXTERN_FUNC(__imp__sub_0014B390);
X86_FUNC(sub_0014B390) {
    DiagEffectLists(base, "submit", false);
    __imp__sub_0014B390(ctx, base);
}

// 0x13EF50: the effect manager update. Walk the list immediately before it runs.
X86_EXTERN_FUNC(__imp__sub_0013EF50);
X86_FUNC(sub_0013EF50) {
    DiagEffectLists(base, "update", false);
    __imp__sub_0013EF50(ctx, base);
}

// 0x13C4C0: one draw cell, called nine times a frame by 0x13F1B0. Use it as a cheap frame tick.
X86_EXTERN_FUNC(__imp__sub_0013C4C0);
X86_FUNC(sub_0013C4C0) {
    if (g_ring_draw_idx < 0) g_game_cell_ctx = X86_MEM32(ctx.esp + 4);
    static thread_local int index = 0;
    static thread_local auto last_call = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_call > std::chrono::milliseconds(100)) index = 0;
    last_call = now;

    if (index == 0) {
        if (!game::city_grid::Ready()) {
            const uint32_t city = X86_MEM32(kCityPtr);
            if (city) game::city_grid::Init(base, city + 0xD0);
        }
        // Read-only store housekeeping and the gated single-cell test, independent of the ring.
        game::city_grid::MaybeSync(base);
        const uint32_t city = X86_MEM32(kCityPtr);
        if (city) game::city_grid::TestOuter(base, ctx, city, city + 0xD0);
        // DrawRing is not called: the full ring crashes over longer runs (shared objects).
        if (Trace()) Sample(base, false);
    }
    if (Trace()) {
        static int cells = 0;
        if (cells < 27) {
            ++cells;
            const uint32_t cell = ctx.ecx;
            LOGR("city: draw cell %08X xy %d,%d loaded %u node %u obj %08X", cell,
                 int32_t(X86_MEM32(cell + 0x04)), int32_t(X86_MEM32(cell + 0x08)), X86_MEM8(cell + 0x88),
                 X86_MEM32(cell + 0x90), X86_MEM32(cell + 0x80));
        }
    }
    __imp__sub_0013C4C0(ctx, base);
    if (++index >= 9) index = 0;
}

// Phase 1 trace hooks. Each counts calls and records the first caller; the original body is untouched.
#define TRACE_HOOK(addr, index)                                                                       \
    X86_EXTERN_FUNC(__imp__sub_##addr);                                                               \
    X86_FUNC(sub_##addr) {                                                                            \
        if (Trace()) TraceHit(index, ctx, base);                                                      \
        __imp__sub_##addr(ctx, base);                                                                 \
    }

TRACE_HOOK(00146CA0, 0)
TRACE_HOOK(00146CC8, 1)
TRACE_HOOK(00146D66, 2)
TRACE_HOOK(00146FC0, 3)
TRACE_HOOK(00146FE8, 4)
TRACE_HOOK(001472E0, 5)
TRACE_HOOK(00147308, 6)

TRACE_HOOK(0013D750, 8)
TRACE_HOOK(0013D350, 9)
TRACE_HOOK(0013A1B0, 10)
TRACE_HOOK(0013A480, 11)
TRACE_HOOK(0013FBE0, 12)
TRACE_HOOK(001400DA, 13)
TRACE_HOOK(00145350, 14)
TRACE_HOOK(00145AC6, 15)
TRACE_HOOK(0013CF40, 16)
// 0x13E7F0 (trace index 17) is hooked below with the portal trace.
TRACE_HOOK(0013E91D, 18)
TRACE_HOOK(0013EB08, 19)
#if defined(TCNYC_PAL) && TCNYC_PAL
// The PAL body for this hook is the one the USA 0x13F840 function became.
TRACE_HOOK(001400D0, 20)
#else
TRACE_HOOK(0013F840, 20)
#endif
TRACE_HOOK(00138B90, 21)
TRACE_HOOK(0013ABB0, 22)
TRACE_HOOK(0013A850, 23)
TRACE_HOOK(001420A0, 24)
TRACE_HOOK(0014A240, 25)
TRACE_HOOK(0013CB50, 26)
TRACE_HOOK(00144250, 27)
TRACE_HOOK(0014F460, 28)
TRACE_HOOK(00142C90, 29)

// Phase 3 probe: the per-slot state machine's argument order.
X86_EXTERN_FUNC(__imp__sub_00137600);
X86_FUNC(sub_00137600) {
    static const bool freeze_game = std::getenv("XBE_CITY_FREEZE_GAME") != nullptr;
    static const auto freeze_start = std::chrono::steady_clock::now();
    const uint32_t object = ctx.ecx;
    if (freeze_game && std::chrono::steady_clock::now() - freeze_start > std::chrono::seconds(5)) {
        const uint32_t city = X86_MEM32(kCityPtr);
        if (city) {
            const uint32_t manager = city + 0xD0;
            const bool game_slot = object >= manager + 0x10 && object < manager + 0x10 + 9u * 0x8B0;
            const bool game_cell = object >= city + 0x4FA4 && object < city + 0x4FA4 + 9u * 0x9C;
            if (game_slot || game_cell) {
                ctx.eax = 0;
                ctx.esp += 16;  // return address + three arguments (ret 0xC)
                return;
            }
        }
    }
    // One owner per tile: when a game cell changes, drop any ring cell for the same tile before the game
    // unloads or reuses the object. XBE_CITY_RING_NOUNLOAD disables it for debugging.
    static const bool ring_unload = std::getenv("XBE_CITY_RING_NOUNLOAD") == nullptr;
    if (ring_unload && !freeze_game) {
        const uint32_t city = X86_MEM32(kCityPtr);
        if (city) {
            const uint32_t manager = city + 0xD0;
            const bool game_slot = object >= manager + 0x10 && object < manager + 0x10 + 9u * 0x8B0;
            const bool game_cell = object >= city + 0x4FA4 && object < city + 0x4FA4 + 9u * 0x9C;
            if (game_slot || game_cell) {
                const int32_t nx = int32_t(X86_MEM32(ctx.esp + 8)), ny = int32_t(X86_MEM32(ctx.esp + 0xC));
                const int32_t ox = int32_t(X86_MEM32(object + 4)), oy = int32_t(X86_MEM32(object + 8));
                if (nx != ox || ny != oy) game::city_grid::OnGameMove(base, ctx, city, object, nx, ny);
            }
        }
    }
    static const bool diag = std::getenv("XBE_CITY_DIAG") != nullptr;
    if (diag) {
        const uint32_t city = X86_MEM32(kCityPtr);
        if (city)
            DiagTransition(base, object, city, city + 0xD0, int32_t(X86_MEM32(ctx.esp + 8)),
                           int32_t(X86_MEM32(ctx.esp + 0xC)), X86_MEM32(ctx.esp));
    }

    const uint32_t grid_slots = game::city_grid::Slots(), grid_cells = game::city_grid::Cells();
    const uint32_t grid_count = game::city_grid::Ready()
                                    ? uint32_t(game::city_grid::N()) * uint32_t(game::city_grid::N())
                                    : 0;
    if (Trace() && grid_count && object >= grid_slots && object < grid_slots + grid_count * 0x8B0)
        LOGR("city: state store slot %08X xy %d,%d state %u flag %u args %08X %08X %08X", object,
             int32_t(X86_MEM32(object + 4)), int32_t(X86_MEM32(object + 8)), X86_MEM32(object + 0x24),
             X86_MEM32(object + 0x2C), X86_MEM32(ctx.esp + 4), X86_MEM32(ctx.esp + 8),
             X86_MEM32(ctx.esp + 0xC));
    if (Trace() && grid_count && object >= grid_cells && object < grid_cells + grid_count * 0x9C)
        LOGR("city: state store cell %08X xy %d,%d state %u flag %u args %08X %08X %08X", object,
             int32_t(X86_MEM32(object + 4)), int32_t(X86_MEM32(object + 8)), X86_MEM32(object + 0x24),
             X86_MEM32(object + 0x2C), X86_MEM32(ctx.esp + 4), X86_MEM32(ctx.esp + 8),
             X86_MEM32(ctx.esp + 0xC));
    if (Trace()) {
        static int n = 0;
        if (n < 12) {
            ++n;
            const uint32_t slot = ctx.ecx;
            LOGR("city: state slot %08X slotxy %d,%d args %08X %08X %08X", slot,
                 int32_t(X86_MEM32(slot + 4)), int32_t(X86_MEM32(slot + 8)), X86_MEM32(ctx.esp + 4),
                 X86_MEM32(ctx.esp + 8), X86_MEM32(ctx.esp + 0xC));
        }
        TraceHit(7, ctx, base);
    }
    __imp__sub_00137600(ctx, base);
}

// Effect-list insert. Log inserts made while a ring cell is being drawn (DrawRing sets g_ring_draw_idx).
int g_ring_draw_idx = -1;
X86_EXTERN_FUNC(__imp__sub_0013EFB0);
X86_FUNC(sub_0013EFB0) {
    static int logged = 0;
    if (g_ring_draw_idx >= 0 && logged < 40 && std::getenv("XBE_CITY_NODEGUARD")) {
        ++logged;
        LOGR("effinsert ring %d node %08X linked %08X cb %08X key %08X ret %08X", g_ring_draw_idx, ctx.ecx,
             X86_MEM32(ctx.ecx), X86_MEM32(ctx.ecx + 0xC), X86_MEM32(ctx.ecx + 0x10), X86_MEM32(ctx.esp));
    }
    __imp__sub_0013EFB0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_0013B460);
X86_FUNC(sub_0013B460) {
    const uint32_t arg0 = X86_MEM32(ctx.esp + 4), self = ctx.ecx, ret = X86_MEM32(ctx.esp);
    __imp__sub_0013B460(ctx, base);
    if (g_ring_draw_idx >= 0) {
        char tag[96];
        std::snprintf(tag, sizeof tag, "after 0013B460 ring %d ecx %08X arg0 %08X ret %08X", g_ring_draw_idx, self, arg0, ret);
        NodeGuard(base, tag);
    }
}
X86_EXTERN_FUNC(__imp__sub_0021C6D0);
X86_FUNC(sub_0021C6D0) {
    const uint32_t arg0 = X86_MEM32(ctx.esp + 4), self = ctx.ecx, ret = X86_MEM32(ctx.esp);
    __imp__sub_0021C6D0(ctx, base);
    if (g_ring_draw_idx >= 0) {
        char tag[96];
        std::snprintf(tag, sizeof tag, "after 0021C6D0 ring %d ecx %08X arg0 %08X ret %08X", g_ring_draw_idx, self, arg0, ret);
        NodeGuard(base, tag);
    }
}
X86_EXTERN_FUNC(__imp__sub_0024C6E0);
X86_FUNC(sub_0024C6E0) {
    const uint32_t arg0 = X86_MEM32(ctx.esp + 4), self = ctx.ecx, ret = X86_MEM32(ctx.esp);
    __imp__sub_0024C6E0(ctx, base);
    if (g_ring_draw_idx >= 0) {
        char tag[96];
        std::snprintf(tag, sizeof tag, "after 0024C6E0 ring %d ecx %08X arg0 %08X ret %08X", g_ring_draw_idx, self, arg0, ret);
        NodeGuard(base, tag);
    }
}
X86_EXTERN_FUNC(__imp__sub_0024CEC0);
X86_FUNC(sub_0024CEC0) {
    const uint32_t arg0 = X86_MEM32(ctx.esp + 4), self = ctx.ecx, ret = X86_MEM32(ctx.esp);
    __imp__sub_0024CEC0(ctx, base);
    if (g_ring_draw_idx >= 0) {
        char tag[96];
        std::snprintf(tag, sizeof tag, "after 0024CEC0 ring %d ecx %08X arg0 %08X ret %08X", g_ring_draw_idx, self, arg0, ret);
        NodeGuard(base, tag);
    }
}
X86_EXTERN_FUNC(__imp__sub_00245EF0);
X86_FUNC(sub_00245EF0) {
    if (ctx.ecx >= 0x2E0000u && ctx.ecx < 0x390000u) {
        static int n = 0;
        if (n++ < 4)
            LOGR("badthis 245EF0 ecx %08X ret %08X ring %d esi %08X edi %08X ebx %08X ebp %08X [esp+4] %08X", ctx.ecx,
                 X86_MEM32(ctx.esp), g_ring_draw_idx, ctx.esi, ctx.edi, ctx.ebx, ctx.ebp, X86_MEM32(ctx.esp + 4));
    }
    __imp__sub_00245EF0(ctx, base);
}

// XBE_TRACE_HEAP: pair the release 0x21FC00 with the game heap's own free. 0x268B0F is operator new, 0x268A85 is
// free (block header at ptr-4). Every block remembers the caller chain (ebp frames) that allocated and freed it;
// when 0x21FC00 is handed an object inside a freed block, both chains are logged before the game touches it.
namespace heaptrace {
struct Rec {
    uint32_t size = 0;
    uint32_t alloc[5] = {};
    uint32_t freed[5] = {};
    bool live = true;
};
std::map<uint32_t, Rec> g_blocks;
std::map<uint32_t, std::array<uint32_t, 5>> *g_released = nullptr;
std::mutex g_lock;
bool On() {
    static const bool on = std::getenv("XBE_TRACE_HEAP") != nullptr;
    return on;
}
void Chain(uint8_t *base, const X86Context &ctx, uint32_t out[5]) {
    out[0] = X86_MEM32(ctx.esp);
    uint32_t ebp = ctx.ebp;
    for (int i = 1; i < 5; ++i) {
        if (ebp < 0x10000u || (ebp & 3u) || !mem::IsCommitted(ebp) || !mem::IsCommitted(ebp + 7)) break;
        out[i] = X86_MEM32(ebp + 4);
        const uint32_t next = X86_MEM32(ebp);
        if (next <= ebp) break;
        ebp = next;
    }
}
void Log(const char *what, uint32_t at, const Rec &r) {
    LOGR("heaptrace %s block %08X size %X alloc %08X<%08X<%08X<%08X<%08X free %08X<%08X<%08X<%08X<%08X", what, at,
         r.size, r.alloc[0], r.alloc[1], r.alloc[2], r.alloc[3], r.alloc[4], r.freed[0], r.freed[1], r.freed[2],
         r.freed[3], r.freed[4]);
}
} // namespace heaptrace

X86_EXTERN_FUNC(__imp__sub_00269405);
X86_FUNC(sub_00269405) {
    static const bool release_trace = std::getenv("XBE_TRACE_RELEASE") != nullptr;
    if (!heaptrace::On() && !release_trace) return __imp__sub_00269405(ctx, base);
    heaptrace::Rec r;
    r.size = X86_MEM32(ctx.esp + 4);
    if (heaptrace::On()) heaptrace::Chain(base, ctx, r.alloc);
    __imp__sub_00269405(ctx, base);
    if (!ctx.eax) return;
    std::lock_guard<std::mutex> lock(heaptrace::g_lock);
    if (heaptrace::g_released) {  // a new allocation here: earlier releases of objects inside it no longer count
        auto &m = *heaptrace::g_released;
        m.erase(m.lower_bound(ctx.eax), m.lower_bound(ctx.eax + r.size));
    }
    if (heaptrace::On()) heaptrace::g_blocks[ctx.eax] = r;
}

X86_EXTERN_FUNC(__imp__sub_002688F9);
X86_FUNC(sub_002688F9) {
    if (heaptrace::On()) {
        const uint32_t ptr = X86_MEM32(ctx.esp + 4);
        std::lock_guard<std::mutex> lock(heaptrace::g_lock);
        auto it = heaptrace::g_blocks.find(ptr);
        if (it != heaptrace::g_blocks.end()) {
            if (!it->second.live) heaptrace::Log("DOUBLE-FREE", ptr, it->second);
            it->second.live = false;
            heaptrace::Chain(base, ctx, it->second.freed);
        }
    }
    __imp__sub_002688F9(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0021FC00);
X86_FUNC(sub_0021FC00) {
    if (std::getenv("XBE_TRACE_RELEASE")) {
        // Release twice without a new allocation at that address in between = double release; log both.
        static std::map<uint32_t, std::array<uint32_t, 5>> released;
        std::array<uint32_t, 5> chain{};
        heaptrace::Chain(base, ctx, chain.data());
        std::lock_guard<std::mutex> g(heaptrace::g_lock);
        heaptrace::g_released = &released;
        auto [it, fresh] = released.emplace(ctx.ecx, chain);
        if (!fresh) {
            static int n = 0;
            if (n++ < 20) {
                const auto &a = it->second;
                LOGR("reltrace DOUBLE RELEASE obj %08X cb %08X refs %04X first %08X<%08X<%08X<%08X<%08X now "
                     "%08X<%08X<%08X<%08X<%08X",
                     ctx.ecx, X86_MEM32(ctx.ecx + 0x10), X86_MEM16(ctx.ecx + 0x82), a[0], a[1], a[2], a[3], a[4],
                     chain[0], chain[1], chain[2], chain[3], chain[4]);
            }
            it->second = chain;
        }
    }
    if (heaptrace::On()) {
        const uint32_t obj = ctx.ecx;
        std::lock_guard<std::mutex> lock(heaptrace::g_lock);
        auto it = heaptrace::g_blocks.upper_bound(obj);
        if (it != heaptrace::g_blocks.begin()) {
            --it;
            if (!it->second.live && obj < it->first + it->second.size) {
                static int n = 0;
                if (n++ < 40) {
                    LOGR("heaptrace RELEASE OF FREED obj %08X (+%X in block) from %08X", obj, obj - it->first,
                         X86_MEM32(ctx.esp));
                    heaptrace::Log("freed", it->first, it->second);
                }
            }
        }
    }
    __imp__sub_0021FC00(ctx, base);
}

// XBE_CITY_SLOTDRAW_TRACE: does the portal pass reach ring slots? 0x13D100 is the per-slot setup it calls;
// count calls on game slots (city + 0xE0..) and on store slots (allocated top-down, >= 0x70000000).
X86_EXTERN_FUNC(__imp__sub_0013D100);
X86_FUNC(sub_0013D100) {
    static const bool on = std::getenv("XBE_CITY_SLOTDRAW_TRACE") != nullptr;
    if (on) {
        static uint32_t game_calls = 0, store_calls = 0;
        static auto last = std::chrono::steady_clock::now();
        (ctx.ecx >= 0x70000000u ? store_calls : game_calls)++;
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(5)) {
            last = now;
            LOGR("slotdraw: 0x13D100 on game slots %u, store slots %u (ret %08X)", game_calls, store_calls,
                 X86_MEM32(ctx.esp));
            // Which slot field leads to this object? Search the nine game slots (city + 0xE0, 0x8B0 each) for
            // the pointer, and one level deeper through each slot pointer field.
            const uint32_t obj = ctx.ecx, city = X86_MEM32(kCityPtr);
            int hits = 0;
            for (uint32_t i = 0; city && i < 9 && hits < 4; ++i) {
                const uint32_t slot = city + 0xE0 + i * 0x8B0;
                for (uint32_t off = 0; off < 0x8B0 && hits < 4; off += 4) {
                    const uint32_t v = X86_MEM32(slot + off);
                    if (v == obj) {
                        LOGR("slotdraw: obj %08X = slot %u +%X", obj, i, off);
                        ++hits;
                    } else if (v >= 0x10000u && (v & 3u) == 0 && mem::IsCommitted(v) && mem::IsCommitted(v + 0x3FC)) {
                        for (uint32_t o2 = 0; o2 < 0x400 && hits < 4; o2 += 4)
                            if (X86_MEM32(v + o2) == obj) {
                                LOGR("slotdraw: obj %08X = [slot %u +%X] +%X", obj, i, off, o2);
                                ++hits;
                            }
                    }
                }
            }
            if (!hits) LOGR("slotdraw: obj %08X (vt %08X) not found in game slots", obj, X86_MEM32(obj));
        }
    }
    __imp__sub_0013D100(ctx, base);
}

// XBE_CITY_DRAWPROBE: which guest code draws tile (.g) geometry? A draw whose vertex buffer header lies inside a
// game slot's .g block (slot + 0x30, the next 4 MB) is tile geometry; log its distinct caller chains.
using XbeDrawProbeFn = void (*)(X86Context &, uint32_t vertex_buffer);
XbeDrawProbeFn &XbeDrawProbe();
namespace {
void TileDrawProbe(X86Context &ctx, uint32_t vb) {
    uint8_t *base = mem::Base();
    const uint32_t city = X86_MEM32(kCityPtr);
    if (!city || !vb) return;
    for (uint32_t i = 0; i < 9; ++i) {
        const uint32_t block = X86_MEM32(city + 0xE0 + i * 0x8B0 + 0x30);
        if (!block || vb < block || vb >= block + 0x400000u) continue;
        uint32_t chain[10] = {};
        chain[0] = X86_MEM32(ctx.esp);
        for (uint32_t ebp = ctx.ebp, n = 1; n < 10; ++n) {
            if (ebp < 0x10000u || (ebp & 3u) || !mem::IsCommitted(ebp) || !mem::IsCommitted(ebp + 7)) break;
            chain[n] = X86_MEM32(ebp + 4);
            const uint32_t next = X86_MEM32(ebp);
            if (next <= ebp) break;
            ebp = next;
        }
        const uint32_t key = chain[1] ^ (chain[3] << 1) ^ (chain[5] << 2);
        static std::vector<uint32_t> seen;
        static std::mutex lock;
        std::lock_guard<std::mutex> g(lock);
        if (seen.size() >= 16 || std::find(seen.begin(), seen.end(), key) != seen.end()) return;
        seen.push_back(key);
        LOGR("drawprobe: tile draw (slot %u, vb %08X) from %08X<%08X<%08X<%08X<%08X<%08X<%08X<%08X<%08X<%08X", i, vb,
             chain[0], chain[1], chain[2], chain[3], chain[4], chain[5], chain[6], chain[7], chain[8], chain[9]);
        return;
    }
}
} // namespace
void InstallTileDrawProbe() {
    static const bool on = std::getenv("XBE_CITY_DRAWPROBE") != nullptr;
    if (on && !::XbeDrawProbe()) ::XbeDrawProbe() = TileDrawProbe;
}

bool g_ring_instances_done = false;

// Ring instances: 0x14A240 calls 0x148D10(this = its visibility context, slot root [slot + 0x50], planes,
// plane count, 0, 0) to add a slot's visible instances to the visible list (moved and enlarged, see
// RelocateInstTable). After the game's own call (return 0x14A73B), run it again once per frame with the same
// context for each loaded ring slot, so their full-detail instances are drawn. XBE_CITY_RING_INSTANCES=1.
X86_EXTERN_FUNC(__imp__sub_00148D10);
X86_FUNC(sub_00148D10) {
    static const bool on = std::getenv("XBE_CITY_RING_INSTANCES") != nullptr;
    const uint32_t ret = X86_MEM32(ctx.esp);
    const uint32_t self = ctx.ecx, planes = X86_MEM32(ctx.esp + 8), nplanes = X86_MEM32(ctx.esp + 12);
    __imp__sub_00148D10(ctx, base);
    if (std::getenv("XBE_CITY_INSTCTX")) {
        static auto t = std::chrono::steady_clock::now();
        static uint32_t seen[8] = {}, nseen = 0;
        const uint32_t owner = X86_MEM32(self + 0xA54);
        bool known = false;
        for (uint32_t k = 0; k < nseen; ++k) known |= seen[k] == owner;
        if (!known && nseen < 8) {
            seen[nseen++] = owner;
            LOGR("instctx: list owner %08X (city %08X) ret %08X count %u", owner, X86_MEM32(kCityPtr), ret,
                 X86_MEM32(owner + 0x6430));
        }
        if (std::chrono::steady_clock::now() - t > std::chrono::seconds(5)) {
            t = std::chrono::steady_clock::now();
            LOGR("instctx: owner %08X count after walk %u", owner, X86_MEM32(owner + 0x6430));
        }
    }
    if (!on || ret != 0x0014A73Bu || !game::city_grid::Ring() || !game::city_grid::Ready()) return;
    const uint32_t city = X86_MEM32(kCityPtr);
    if (g_ring_instances_done) return;  // once per frame (reset at the city draw entry)
    g_ring_instances_done = true;
    const uint32_t before = X86_MEM32(city + 0x6430);
    (void)nplanes;
    const int n = game::city_grid::N();
    const uint32_t saved_ecx = ctx.ecx, saved_eax = ctx.eax;
    int slots = 0;
    for (int i = 0; i < n * n; ++i) {
        if (!game::city_grid::RingCell(i)) continue;
        const uint32_t root = X86_MEM32(game::city_grid::Slots() + uint32_t(i) * 0x8B0u + 0x50);
        if (!root) continue;
        ctx.ecx = self;
        kernel::CallGuest(ctx, 0x148D10, {root, planes, 0, 0, 0});  // no portal planes: camera tests only
        if (++slots >= 8) break;                                     // bounded while testing
    }
    ctx.ecx = saved_ecx;
    ctx.eax = saved_eax;
    static auto last = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
        last = std::chrono::steady_clock::now();
        LOGR("ring instances: %d ring slots walked, visible list %u -> %u", slots, before, X86_MEM32(city + 0x6430));
    }
}


// Called by the skyline hook (camera.cpp) just before the skyline at 0x13F2BB, inside the distant-city pass
// 0x13F090 and right after the game's nine distant cells: the ring's cells then get exactly the same render
// states (depth, blend, fog) as the game's own. XBE_CITY_RING_DRAW_LATE=1 restores the old draw after the pass.
void DrawRingInDistantPass(uint8_t *base, X86Context &ctx) {
    static const bool late = std::getenv("XBE_CITY_RING_DRAW_LATE") != nullptr;
    static const bool nodraw = std::getenv("XBE_CITY_RING_NODRAW") != nullptr;
    const uint32_t city = X86_MEM32(kCityPtr);
    if (late || nodraw || !game::city_grid::Ring() || !g_game_cell_ctx || !city) return;
    // While one of the game's own nine tiles is still loading, its gap would show the ring's far tiles in front
    // of where nearer buildings belong; hold the ring back until the game's tiles settle (state +0x24 idle).
    static const bool always = std::getenv("XBE_CITY_RING_DRAW_ALWAYS") != nullptr;
    for (uint32_t i = 0; !always && i < 9; ++i)
        if (X86_MEM32(city + 0xE0 + i * 0x8B0 + 0x24) || X86_MEM32(city + 0x4FA4 + i * 0x9C + 0x24)) return;
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    game::city_grid::DrawRing(base, ctx, g_game_cell_ctx);
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
}

// XBE_CITY_PORTALTRACE: the portal pass 0x13E7F0 runs on `this` (stored at [esp+0x44]) and at 0x13ECAD reads the
// city's instance table instead of this->+0x50. Log every distinct `this` that is not the city.
X86_EXTERN_FUNC(__imp__sub_0013E7F0);
X86_FUNC(sub_0013E7F0) {
    if (Trace()) TraceHit(17, ctx, base);
    static const bool on = std::getenv("XBE_CITY_PORTALTRACE") != nullptr;
    if (on) {
        const uint32_t self = ctx.ecx, city = X86_MEM32(kCityPtr);
        static uint32_t seen[16] = {}, n = 0;
        bool known = self == city;
        for (uint32_t i = 0; i < n && !known; ++i) known = seen[i] == self;
        if (!known && n < 16) {
            seen[n++] = self;
            const int ring_n = game::city_grid::Ready() ? game::city_grid::N() : 0;
            const uint32_t store_lo = game::city_grid::Ready() ? game::city_grid::Slots() : 0;
            LOGR("portaltrace: this %08X (city %08X) vt %08X ret %08X [this+0x50] %08X [city+0x50] %08X store slots %08X..+%X",
                 self, city, X86_MEM32(self), X86_MEM32(ctx.esp), X86_MEM32(self + 0x50), X86_MEM32(city + 0x50),
                 store_lo, uint32_t(ring_n * ring_n) * 0x8B0u);
        }
    }
    __imp__sub_0013E7F0(ctx, base);
}
