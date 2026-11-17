// Guest addresses and recompiled function names for the image this build targets.
// The USA build uses the values in the source; the PAL build maps them to the
// European XBE's layout in pal_addresses.h.
#pragma once
#include <cstdint>

#ifndef TCNYC_PAL
#define TCNYC_PAL 0
#endif

#ifndef TCNYC_PAL_EU
#define TCNYC_PAL_EU 0
#endif

#if TCNYC_PAL_EU
#include "pal_eu_addresses.h"
#elif TCNYC_PAL
#include "pal_addresses.h"
#else
constexpr uint32_t TC_ADDR(uint32_t usa) { return usa; }
#endif
