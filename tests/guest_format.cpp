#include "kernel/guest_format.h"

#include <initializer_list>
#include <iostream>
#include <stdexcept>

bool Check(std::string_view format, std::initializer_list<uint32_t> words, std::string_view expected) {
    auto next = words.begin();
    const auto result = kernel::FormatGuestText(format, [&] {
        if (next == words.end()) throw std::runtime_error("read beyond guest arguments");
        return *next++;
    }, [](uint32_t address, size_t count) {
        if (address != 1) throw std::runtime_error("unexpected guest string pointer");
        return std::string(std::string_view("hello").substr(0, count));
    });
    if (result == expected && next == words.end()) return true;
    std::cerr << format << " produced [" << result << "], expected [" << expected << "]\n";
    return false;
}

int main() {
    constexpr uint64_t value = std::bit_cast<uint64_t>(1.25);
    const auto low = uint32_t(value);
    const auto high = uint32_t(value >> 32);
    return !(Check("%*d %u", {5, 42, 9}, "   42 9") &&
             Check("%0*.*f %x", {8, 2, low, high, 31}, "00001.25 1f") &&
             Check("[%*.*s] %d", {uint32_t(-6), 3, 1, 5}, "[hel   ] 5") &&
             Check("[%.*s] [%.*s]", {0, 0, uint32_t(-1), 1}, "[] [hello]") &&
             Check("%12I64d %#I64x %hd", {0xFFFFFFFEu, 0xFFFFFFFFu, 0, 1, 0xFFFFu},
                   "          -2 0x100000000 -1") &&
             Check("%% %p %c", {0x1234, 'Q'}, "% 1234 Q") &&
             Check("%.*f", {uint32_t(-1), low, high}, "1.250000") &&
             Check("%n %d", {}, "%n %d"));
}
