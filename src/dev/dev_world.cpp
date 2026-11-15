// Developer-only vehicle spawner and weather override. Built only with TCNYC_DEV_TOOLS.
//
// Vehicles: as the script native CreateVehicleAtPointName (0x186B10 -> 0x17CE00): the model comes from
// game.0x81CC0(0x1C8A0(name hash)) (0x1C8A0 searches the sorted {hash, id} table at 0x2E4110, 89 types
// named in AssetlistsDev/Vehicle.def), and game.0x828F0(position*, model, 0, 0, 0, 0) creates it. The point
// position is replaced by the player's (0x21EC70) moved a few metres along x.
//
// Weather: the natives SetWeatherValue (0x17EB10 -> 0x14C0D0, 0..1), SetPartFogDensity (0x17EC00 ->
// 0x14C150, 0..1) and SetRainType (0x17EC70 -> 0x14BD60, 0..2). While the override is on they are applied
// every frame and the auto weather flag (0x3A7860, IsAutoWeatherEnabled reads it inverted) is set.

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>


#include "imgui.h"
#include "core/log.h"
#include "dev/dev.h"
#include "game/game_addresses.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {

constexpr uint32_t kPlayer = TC_ADDR(0x37D284);
constexpr uint32_t kGame = TC_ADDR(0x360E00);
constexpr uint32_t kGetPosition = TC_ADDR(0x21EC70);
constexpr uint32_t kModelId = TC_ADDR(0x1C8A0);
constexpr uint32_t kModelOf = TC_ADDR(0x81CC0);
constexpr uint32_t kCreateVehicle = TC_ADDR(0x828F0);
// A requester (the script one is game+0x1B4) has 8 vehicle requests at +0x208, 0x14 apart, state at +4
// (0 free, 1 waiting for a slot, 2 loading, 3/4 loaded). 0x1CE10(request, type id, 5, 0) puts a type into one
// of the 16 model slots, loading it if needed (as 0xD76DE does for a police car); 0x1E370 advances the
// requests and returns true when all are loaded.
constexpr uint32_t kRequestVehicle = TC_ADDR(0x1CE10);
constexpr uint32_t kRequests = 0x208, kRequestStride = 0x14, kRequestCount = 8;
constexpr uint32_t kStreamDone = TC_ADDR(0x1E370);  // thiscall on game+0x1B4 -> bool: CheckAssetsLoaded
constexpr uint32_t kSetWeather = TC_ADDR(0x14C0D0);
constexpr uint32_t kSetFog = TC_ADDR(0x14C150);
constexpr uint32_t kSetRainType = TC_ADDR(0x14BD60);
constexpr uint32_t kAutoWeatherOff = TC_ADDR(0x3A7860);

constexpr const char *kVehicles[] = {
    "BEATER_BIGCAR_77", "BEATER_BLITZER", "BEATER_COUNTYSQUARE_77", "BEATER_GUIDO", "BEATER_LAMINA_92",
    "BEATER_PALOMINO", "BEATER_PIRAHNA", "BEATER_SIDESPLITTER_83", "BUS_CITY", "BUS_TOUR", "COUPE_CIVIL",
    "COUPE_CIVIL_UPGRADED", "COUPE_DOOBIE7", "COUPE_GEE250", "COUPE_GUIDO", "COUPE_MACHO", "COUPE_SKYLIME",
    "COUPE_SKYLIME_UPGRADED", "COUPE_SUPER", "COUPE_SUPER_UPGRADED", "MINIVAN_BBBOX", "MINIVAN_CONVOY",
    "MINIVAN_HENNA", "MOTORCYCLE_9960", "MOTORCYCLE_BONEVILLE", "MOTORCYCLE_CHIP", "MOTORCYCLE_CHOPPER",
    "MOTORCYCLE_CRUISER", "MUSCLECAR_BLITZER", "MUSCLECAR_BLITZER_UPGRADED", "MUSCLECAR_PIRAHNA",
    "MUSCLECAR_PIRAHNA_UPGRADED", "NICK_3HUNDRED", "NICK_3HUNDRED_UPGRADED", "NICK_COQUETE",
    "NICK_COQUETE_UPGRADED", "NICK_GRANDTOURISM", "NICK_NOX", "NICK_NOX_UPGRADED", "NICK_PALOMINO",
    "NICK_PALOMINO_UPGRADED", "NICK_SENIORX", "NICK_SENIORX_UPGRADED", "REDMAN_BEATER_BLITZER",
    "REDMAN_BEATER_GUIDO", "ROADSTER_MISTER2", "ROADSTER_MISTER2_UPGRADED", "SEDAN_BIGCAR_77", "SEDAN_CANARY",
    "SEDAN_CARNAGE", "SEDAN_CROWNWIG", "SEDAN_FREDSCAB", "SEDAN_HICLASS", "SEDAN_LAMINA_92", "SEDAN_LIMO",
    "SEDAN_POLICE", "SEDAN_POLICE_TRAINING", "SEDAN_SEDAN80", "SEDAN_SIDESPLITTER_83", "SEDAN_SKIND",
    "SEDAN_TAXI", "SUV_4WD5", "SUV_BAJAJUMPER", "SUV_BAJAJUMPER_POLICE", "SUV_FUNNER", "SUV_GRANDPECHANGA",
    "SUV_URBAN", "SWAGON_COUNTRYSQUARE_77", "SWAGON_VOLE", "TRUCK_ARMOREDCAR", "TRUCK_FIREENGINE",
    "TRUCK_FREDUPX", "TRUCK_PICKUP", "TRUCK_PICKUP_BIKE", "TRUCK_PICKUP_BOAT", "TRUCK_PICKUP_GARDEN",
    "TRUCK_PICKUP_LADDER", "TRUCK_PICKUP_SHELL", "TRUCK_RAMP", "TRUCK_RAMP_BOX", "TRUCK_RAMP_FIXED",
    "TRUCK_T260", "TRUCK_T260_GARBAGE", "TRUCK_T260_UTILITY", "TRUCK_T262", "TRUCK_T262_TANKER",
    "TRUCK_T262_WOOD", "VAN_AMBULANCE", "VAN_SAVANT",
};
constexpr int kVehicleCount = int(sizeof(kVehicles) / sizeof(kVehicles[0]));

// vehicle model slots: 16 at game+0x2660, 0x14 apart; word +6 indexes the 0x74-byte model table at 0x37493C,
// whose word +0x60 is the vehicle type id (the value 0x1C8A0 finds for a name hash). 0x81CC0 only finds
// types in these slots, so only loaded types can be created.
constexpr uint32_t kSlots = 0x2660, kSlotStride = 0x14, kSlotCount = 16;
constexpr uint32_t kModelTable = TC_ADDR(0x37493C), kModelStride = 0x74;
constexpr uint32_t kTypeTable = TC_ADDR(0x2E4110);  // {pairs*, count}, pairs {name hash, type id}
std::atomic<uint32_t> g_loaded[kSlotCount];        // type ids of loaded slots (0 = empty), for the menu

std::mutex g_lock;
std::vector<uint32_t> g_spawns;  // name hashes
std::atomic<float> g_distance{6.0f};

std::atomic<bool> g_weather_on{false}, g_weather_was_on{false};
std::atomic<float> g_weather{0.0f}, g_fog{0.0f};
std::atomic<int> g_rain_type{0};
uint8_t g_auto_saved = 0;

bool Readable(uint32_t va, uint32_t size = 4) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}

uint32_t CallMethod(X86Context &ctx, uint32_t fn, uint32_t self, std::initializer_list<uint32_t> args) {
    const uint32_t ecx = ctx.ecx;
    ctx.ecx = self;
    const uint32_t r = kernel::CallGuest(ctx, fn, args);
    ctx.ecx = ecx;
    return r;
}

uint32_t Crc32(const char *s) {  // the game's name hashes: standard CRC-32
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; ++s) {
        c ^= uint8_t(*s);
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

} // namespace

namespace dev {

void WorldFrame(X86Context &ctx) {
    // weather
    const bool on = g_weather_on;
    if (on) {
        if (!g_weather_was_on) g_auto_saved = *static_cast<uint8_t *>(kernel::HostPtr(kAutoWeatherOff));
        *static_cast<uint8_t *>(kernel::HostPtr(kAutoWeatherOff)) = 1;
        kernel::CallGuest(ctx, kSetWeather, {Bits(g_weather)});
        kernel::CallGuest(ctx, kSetFog, {Bits(g_fog)});
        kernel::CallGuest(ctx, kSetRainType, {uint32_t(g_rain_type.load())});
    } else if (g_weather_was_on) {
        *static_cast<uint8_t *>(kernel::HostPtr(kAutoWeatherOff)) = g_auto_saved;
    }
    g_weather_was_on = on;

    // vehicles: which types are loaded now
    if (Readable(kGame) && Readable(kernel::R32(kGame) + kSlots + kSlotCount * kSlotStride)) {
        const uint32_t g = kernel::R32(kGame);
        for (uint32_t i = 0; i < kSlotCount; ++i) {
            const int16_t idx = int16_t(kernel::R32(g + kSlots + i * kSlotStride + 4) >> 16);
            const uint32_t entry = kModelTable + uint32_t(idx) * kModelStride + 0x60;
            g_loaded[i] = idx >= 0 && Readable(entry) ? uint32_t(int16_t(kernel::R32(entry))) : 0;
        }
    }
    std::vector<uint32_t> spawns;
    {
        std::lock_guard guard(g_lock);
        spawns.swap(g_spawns);
    }
    static std::vector<uint32_t> pending;  // streamed, waiting for CheckAssetsLoaded
    static int wait = 0;
    if ((spawns.empty() && pending.empty()) || !Readable(kPlayer) || !Readable(kGame)) return;
    const uint32_t player = kernel::R32(kPlayer), game = kernel::R32(kGame);
    if (!Readable(player) || !Readable(game + 0x2A90)) return;
    const uint32_t streamer = kernel::R32(game + 0x1B4);
    if (!Readable(streamer)) return;
    // request each type into a model slot through a free request of the script requester (from the
    // last, which missions are least likely to use), then wait until the requester reports it loaded
    for (uint32_t hash : spawns) {
        const uint32_t id = kernel::CallGuest(ctx, kModelId, {hash});
        int k = int(kRequestCount) - 1;
        while (k >= 0 && kernel::R32(streamer + kRequests + uint32_t(k) * kRequestStride + 4) > 1) --k;
        if (k < 0) {
            LOGR("dev: no free vehicle request for %08X", hash);
            continue;
        }
        const uint32_t ok = CallMethod(ctx, kRequestVehicle, streamer, {uint32_t(k), id, 5, 0}) & 0xFF;
        LOGR("dev: requested vehicle %08X (type %u) in request %d: %s", hash, id, k, ok ? "ok" : "refused");
        pending.push_back(hash);
        wait = 0;
    }
    // drive the requests (a request waiting for a slot to be evicted is finished here), then wait until
    // every pending type is in a ready slot (0x81CC0 finds it)
    CallMethod(ctx, kStreamDone, streamer, {});
    bool ready = true;
    for (uint32_t hash : pending)
        if (int32_t(CallMethod(ctx, kModelOf, game, {kernel::CallGuest(ctx, kModelId, {hash})})) < 0) ready = false;
    if (!ready && ++wait < 900) {
        if (wait % 120 == 0)
            for (uint32_t k = 0; k < kRequestCount; ++k) {
                const uint32_t r = streamer + kRequests + k * kRequestStride;
                LOGR("dev: request %u: slot %d state %u type %u", k, int32_t(kernel::R32(r)), kernel::R32(r + 4),
                     kernel::R32(r + 0x10));
            }
        return;
    }
    if (!ready) LOGR("dev: vehicle not loaded after 15 s, trying anyway");
    spawns.swap(pending);
    pending.clear();
    static const uint32_t position = mem::AllocSystem(16, 16);
    for (uint32_t hash : spawns) {
        const uint32_t at = CallMethod(ctx, kGetPosition, player, {position});
        const uint32_t pos = at ? at : position;
        if (!Readable(pos, 16)) break;
        if (pos != position) std::memcpy(kernel::HostPtr(position), kernel::HostPtr(pos), 16);
        float x;
        std::memcpy(&x, kernel::HostPtr(position), 4);
        x += g_distance;
        std::memcpy(kernel::HostPtr(position), &x, 4);
        const uint32_t id = kernel::CallGuest(ctx, kModelId, {hash});
        const uint32_t model = CallMethod(ctx, kModelOf, game, {id});
        if (!model) {
            LOGR("dev: vehicle %08X has no model", hash);
            continue;
        }
        const uint32_t car = CallMethod(ctx, kCreateVehicle, game, {position, model, 0, 0, 0, 0});
        LOGR("dev: spawned vehicle %08X (model %08X) -> %08X", hash, model, car);
    }
}

void VehiclesWeatherUi() {
    ImGui::SeparatorText("Spawn vehicle");
    // type id -> name, from the game's table and the names' hashes
    static int name_of[256];
    static bool mapped = false;
    if (!mapped && Readable(kTypeTable, 8)) {
        for (int &n : name_of) n = -1;
        const uint32_t pairs = kernel::R32(kTypeTable), count = kernel::R32(kTypeTable + 4);
        for (uint32_t k = 0; k < count && Readable(pairs + k * 8 + 4); ++k)
            for (int i = 0; i < kVehicleCount; ++i)
                if (Crc32(kVehicles[i]) == kernel::R32(pairs + k * 8)) {
                    const uint32_t id = kernel::R32(pairs + k * 8 + 4);
                    if (id < 256) name_of[id] = i;
                }
        mapped = true;
    }
    bool loaded[kVehicleCount] = {};
    for (auto &slot : g_loaded) {
        const uint32_t id = slot.load();
        if (id && id < 256 && name_of[id] >= 0) loaded[name_of[id]] = true;
    }
    static bool only_loaded = false;
    ImGui::Checkbox("Only vehicles loaded now", &only_loaded);
    static int pick = 30;  // MUSCLECAR_PIRAHNA
    static ImGuiTextFilter filter;
    filter.Draw("Filter", 200);
    if (ImGui::BeginListBox("##vehicles", {-1, 160})) {
        for (int i = 0; i < kVehicleCount; ++i) {
            if (!filter.PassFilter(kVehicles[i]) || (only_loaded && !loaded[i])) continue;
            if (ImGui::Selectable(kVehicles[i], pick == i)) pick = i;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                std::lock_guard guard(g_lock);
                g_spawns.push_back(Crc32(kVehicles[i]));
            }
        }
        ImGui::EndListBox();
    }
    float dist = g_distance;
    if (ImGui::SliderFloat("Offset (x)", &dist, -30.0f, 30.0f, "%.0f")) g_distance = dist;
    if (ImGui::Button("Spawn") && InGameplay()) {
        std::lock_guard guard(g_lock);
        g_spawns.push_back(Crc32(kVehicles[pick]));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(or double-click; needs gameplay)");

    ImGui::SeparatorText("Weather");
    bool on = g_weather_on;
    if (ImGui::Checkbox("Override weather", &on)) g_weather_on = on;
    float w = g_weather, f = g_fog;
    int r = g_rain_type;
    if (ImGui::SliderFloat("Weather (rain)", &w, 0.0f, 1.0f)) g_weather = w;
    if (ImGui::SliderFloat("Fog", &f, 0.0f, 1.0f)) g_fog = f;
    if (ImGui::SliderInt("Rain type", &r, 0, 2)) g_rain_type = r;
}

} // namespace dev
