#include "game_addresses.h"
// Car paint drawn as one blended mesh: keep its depth writes.
//
// This is a bug in the original Xbox release, not in the port: the game's own draw turns the depth writes off.
//
// The mesh draw 0x23DC60 sends car bodies through its paint path (0x23E6CE..0x23E800): one of the paint pixel
// shaders from the table at 0x4E6B00, blend ONE, SRC_ALPHA (the blend cache at 0x4C6380 holds 0x10302), output
// alpha (1 - material alpha * texture alpha) * (1 - reflection). Paint materials carry blend type 0xC0
// ([material] & 0xC0), which the routine draws as translucent: before the draw at 0x23F2F6, 0x23F2CC turns depth
// writes off.
//
// Rusted beaters (BEATER_GUIDO, BEATER_PALOMINO, ...) paint their whole body with such a material: the rust
// texture, alpha 241-255, with material opacity 255 ([material+7]). Close up the body is split into a dozen parts
// that the translucent list sorts back to front. The distant model is one mesh of about a thousand vertices drawn
// without culling, so without depth writes its far side and interior, drawn later in the same mesh, paint over the
// panels nearest the camera: the car looks partly see-through at some distances. Clean cars (COUPE_GUIDO) draw an
// opaque base coat with depth writes first, so the coat this call draws over it already sits behind correct depth.
//
// For that one call, a paint material at full opacity keeps depth writes on; texture alpha still blends as
// before. Glass and other see-through paint (opacity below 255) is left as the game draws it.
// XBE_CAR_PAINT_DEPTH=0 restores the game's behaviour for comparison.

#include <cstdint>
#include <cstdlib>

#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using XbeSimpleStateFilterFn = uint32_t (*)(X86Context &, uint32_t method, uint32_t value);
XbeSimpleStateFilterFn &XbeSimpleStateFilter();  // xdk/d3d8.cpp

namespace {

constexpr uint32_t kDepthMask = 0x35C;                          // NV097_SET_DEPTH_MASK
constexpr uint32_t kTranslucentDepthOff = TC_ADDR(0x23F2D8);    // return of 0x23F2D3, the call 0x23F2CE sets up
constexpr uint32_t kBlendCache = TC_ADDR(0x4C6380);             // 0x23DC60's cached (source << 16 | destination)
constexpr uint32_t kPaintBlend = 0x10302;                       // ONE, SRC_ALPHA: the paint path at 0x23E7A2

uint32_t KeepPaintDepth(X86Context &ctx, uint32_t method, uint32_t value) {
    if (method != kDepthMask || value != 0) return value;
    uint8_t *base = mem::Base();
    if (X86_MEM32(ctx.esp) != kTranslucentDepthOff) return value;
    // 0x23DC60 keeps the material (mesh part + 0x0C, from 0x23E0FD) in edi from there to the draw
    const uint32_t material = ctx.edi;
    if (X86_MEM32(kBlendCache) != kPaintBlend) return value;
    if (!mem::IsCommitted(material) || !mem::IsCommitted(material + 7)) return value;
    if (X86_MEM8(material + 7) != 0xFF) return value;  // glass and tinted paint stay see-through
    return 1;
}

const bool g_installed = [] {
    if (const char *e = std::getenv("XBE_CAR_PAINT_DEPTH"); e && e[0] == '0') return false;
    ::XbeSimpleStateFilter() = KeepPaintDepth;
    return true;
}();

} // namespace
