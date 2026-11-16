#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace kernel {

// read_word consumes one 32-bit guest stack slot. Only validated format
// specifiers reach the host CRT, with a host argument of the matching type.
template <typename ReadWord, typename ReadString>
std::string FormatGuestText(std::string_view format, ReadWord read_word, ReadString read_string) {
    constexpr int kFieldLimit = 511;  // existing per-conversion output buffer
    constexpr int kStringLimit = 256;
    constexpr std::string_view kFlags = "-+ #0";
    std::string out;
    size_t pos = 0;
    auto digits = [&] {
        int value = 0;
        while (pos < format.size() && format[pos] >= '0' && format[pos] <= '9')
            value = std::min(kFieldLimit, value * 10 + format[pos++] - '0');
        return value;
    };
    auto read_wide = [&] {
        const uint32_t low = read_word();
        return uint64_t(low) | (uint64_t(read_word()) << 32);
    };

    while (pos < format.size()) {
        const size_t start = pos++;
        if (format[start] != '%' || (pos < format.size() && format[pos] == '%')) {
            out += format[start];
            if (format[start] == '%') ++pos;
            continue;
        }

        const size_t flags_start = pos;
        while (pos < format.size() && kFlags.find(format[pos]) != std::string_view::npos) ++pos;
        std::string flags(format.substr(flags_start, pos - flags_start));
        const bool width_arg = pos < format.size() && format[pos] == '*';
        int width = 0;
        if (width_arg) ++pos;
        else width = digits();
        int precision = -1;
        bool precision_arg = false;
        if (pos < format.size() && format[pos] == '.') {
            ++pos;
            precision_arg = pos < format.size() && format[pos] == '*';
            if (precision_arg) ++pos;
            else precision = digits();
        }

        std::string_view length;
        for (const auto candidate : {std::string_view("I64"), std::string_view("I32"), std::string_view("ll"),
                                     std::string_view("hh"), std::string_view("l"), std::string_view("h")}) {
            if (!format.substr(pos).starts_with(candidate)) continue;
            length = candidate;
            pos += length.size();
            break;
        }
        const char conversion = pos < format.size() ? format[pos++] : '\0';
        const bool integer = conversion && std::string_view("diouxX").find(conversion) != std::string_view::npos;
        const bool floating = conversion && std::string_view("fegFEG").find(conversion) != std::string_view::npos;
        const bool string = conversion == 's';
        const bool character = conversion == 'c';
        const bool pointer = conversion == 'p';
        if (!(integer || (floating && (length.empty() || length == "l")) ||
              ((string || character || pointer) && length.empty()))) {
            // The argument layout of an unsupported directive is unknown.
            out += format.substr(start);
            break;
        }

        if (width_arg) {
            const int64_t arg = std::bit_cast<int32_t>(read_word());
            if (arg < 0 && flags.find('-') == std::string::npos) flags += '-';
            width = int(std::min<int64_t>(kFieldLimit, arg < 0 ? -arg : arg));
        }
        if (precision_arg)
            precision = std::clamp(std::bit_cast<int32_t>(read_word()), -1, kFieldLimit);

        const bool wide = integer && (length == "ll" || length == "I64");
        const bool signed_integer = conversion == 'd' || conversion == 'i';
        std::erase_if(flags, [&](char flag) {
            if (flag == '-') return false;
            if (flag == '0') return string || character;
            if (flag == '#') return !(floating || pointer || conversion == 'o' || conversion == 'x' || conversion == 'X');
            return !(signed_integer || floating);
        });
        std::string spec = "%" + flags;
        if (width) spec += std::to_string(width);
        if (precision >= 0 && !character) spec += "." + std::to_string(precision);
        if (wide) spec += "ll";
        else if (integer && (length == "h" || length == "hh")) spec += length;
        spec += pointer ? 'x' : conversion;  // guest pointers are 32-bit values

        auto append = [&](auto value) {
            char buffer[kFieldLimit + 1];
            const int count = std::snprintf(buffer, sizeof(buffer), spec.c_str(), value);
            if (count > 0) out.append(buffer, std::min(count, kFieldLimit));
        };
        if (string) {
            const auto address = read_word();
            const auto text = precision == 0 ? std::string{} :
                read_string(address, size_t(precision < 0 ? kStringLimit : std::min(precision, kStringLimit)));
            append(text.c_str());
        } else if (floating) {
            append(std::bit_cast<double>(read_wide()));
        } else if (wide) {
            const uint64_t value = read_wide();
            if (signed_integer) append(static_cast<long long>(std::bit_cast<int64_t>(value)));
            else append(static_cast<unsigned long long>(value));
        } else {
            const uint32_t value = read_word();
            if (signed_integer || character) append(int(std::bit_cast<int32_t>(value)));
            else append(static_cast<unsigned int>(value));
        }
    }
    return out;
}

}  // namespace kernel
