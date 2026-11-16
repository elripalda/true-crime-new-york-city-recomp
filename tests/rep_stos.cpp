#include "cpu/x86_ops.h"

#include <array>

int main() {
    for (uint32_t width : {1u, 2u, 4u}) {
        for (uint32_t count = 0; count <= 9; ++count) {
            for (bool backwards : {false, true}) {
                std::array<uint8_t, 96> actual;
                actual.fill(0xCC);
                auto expected = actual;
                X86Context ctx{};
                ctx.eax = 0x1234B678;
                ctx.ecx = count;
                ctx.edi = 43;  // unaligned, with space for either direction
                ctx.df = backwards;
                const int32_t stride = backwards ? -int32_t(width) : int32_t(width);
                uint32_t address = ctx.edi;
                for (uint32_t i = 0; i < count; ++i) {
                    for (uint32_t byte = 0; byte < width; ++byte)
                        expected[address + byte] = uint8_t(ctx.eax >> (byte * 8));
                    address += stride;
                }
                x86_rep_stos(ctx, actual.data(), width);
                if (actual != expected) return int(width);
                if (ctx.ecx != 0 || ctx.edi != address || ctx.eax != 0x1234B678 || ctx.df != backwards)
                    return int(width) + 10;
            }
        }
    }
    return 0;
}
