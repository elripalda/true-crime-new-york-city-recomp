// Phase 2 city tile store: an N×N shadow of the game's fixed 3×3 arrays.
//
// The game's arrays stay authoritative for now; this store is populated from them so Phase 3 can make it the
// source and mirror the centre 3×3 back. Enabled with XBE_CITY_GRID=3|5|7|9. With the variable unset nothing
// is allocated and the hooks are no-ops.
#pragma once

#include <cstdint>

struct X86Context;

namespace game::city_grid {

// manager is the tile manager's `this` (city + 0xD0), captured at 0x13D880.
void Init(uint8_t *base, uint32_t manager);
// Phase 3 probe: load one outer tile into the store slot and cell using the game's state machine.
void TestOuter(uint8_t *base, X86Context &ctx, uint32_t city, uint32_t manager);
// Phase 3b: load the 16 outer tiles around the centre and draw their cells after the game's nine.
void DriveRing(uint8_t *base, X86Context &ctx, uint32_t city, uint32_t manager);
void DrawRing(uint8_t *base, X86Context &ctx, uint32_t camera);
// The game is about to move a centre cell; unload the store cells before it can free a shared object.
void OnGameMove(uint8_t *base, X86Context &ctx, uint32_t city, uint32_t game_object, int32_t target_x,
                int32_t target_y);
// Called from the draw tick; copies the game's nine slots into the shadow at most every 100 ms.
void MaybeSync(uint8_t *base);

bool Ready();
int N();
uint32_t Base();
uint32_t Slots();
uint32_t Cells();
uint32_t Requests();
uint32_t Table();
bool RingCell(int idx);
bool InInnerDetailZone(uint8_t *base, uint32_t slot);
bool Ring();
bool OuterDetailAvailable();

} // namespace game::city_grid
