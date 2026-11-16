#pragma once

#include <charconv>
#include <cctype>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

// The release API supplies a bounded JSON document. Keep exact integers and
// separate arrays from objects so metadata can be selected by field name.
struct ReleaseJson {
    enum class Type { Null, Object, Array, String, Number, Boolean };
    Type type = Type::Null;
    std::string value;
    std::map<std::string, ReleaseJson, std::less<>> fields;
    std::vector<ReleaseJson> elements;

    const ReleaseJson& operator[](std::string_view name) const {
        static const ReleaseJson empty;
        const auto it = fields.find(name);
        return it != fields.end() ? it->second : empty;
    }

    std::string_view String() const { return type == Type::String ? std::string_view(value) : std::string_view{}; }

    uint64_t Size() const {
        uint64_t result = 0;
        if (type != Type::Number) return 0;
        const auto end = value.data() + value.size();
        const auto parsed = std::from_chars(value.data(), end, result);
        return parsed.ec == std::errc{} && parsed.ptr == end ? result : 0;
    }

    bool True() const { return type == Type::Boolean && value == "true"; }
};

class ReleaseJsonParser {
public:
    explicit ReleaseJsonParser(std::string_view source) : text_(source) {}

    ReleaseJson Parse() {
        auto json = Value(0);
        Space();
        if (!text_.empty()) Invalid();
        return json;
    }

private:
    std::string_view text_;

    [[noreturn]] static void Invalid() { throw std::runtime_error("Invalid release metadata."); }
    void Space() {
        while (!text_.empty() && std::string_view(" \t\r\n").find(text_.front()) != std::string_view::npos)
            text_.remove_prefix(1);
    }
    bool Take(char c) {
        Space();
        if (text_.empty() || text_.front() != c) return false;
        text_.remove_prefix(1);
        return true;
    }
    char Next() {
        if (text_.empty()) Invalid();
        const char c = text_.front();
        text_.remove_prefix(1);
        return c;
    }

    unsigned Hex() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const auto digit = std::string_view("0123456789abcdef").find(char(std::tolower(uint8_t(Next()))));
            if (digit == std::string_view::npos) Invalid();
            value = (value << 4) | unsigned(digit);
        }
        return value;
    }

    std::string String() {
        if (!Take('"')) Invalid();
        std::string out;
        for (char c = Next(); c != '"'; c = Next()) {
            if (uint8_t(c) < 0x20) Invalid();
            if (c != '\\') {
                out += c;
                continue;
            }
            c = Next();
            if (c == 'u') {
                unsigned code = Hex();
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (Next() != '\\' || Next() != 'u') Invalid();
                    const unsigned low = Hex();
                    if (low < 0xDC00 || low > 0xDFFF) Invalid();
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    Invalid();
                }
                if (code < 0x80) out += char(code);
                else {
                    if (code >= 0x10000) out += char(0xF0 | (code >> 18));
                    if (code >= 0x800) out += char((code < 0x10000 ? 0xE0 : 0x80) | ((code >> 12) & 0x3F));
                    out += char((code < 0x800 ? 0xC0 : 0x80) | ((code >> 6) & 0x3F));
                    out += char(0x80 | (code & 0x3F));
                }
                continue;
            }
            const auto escaped = std::string_view("\"\\/bfnrt").find(c);
            if (escaped == std::string_view::npos) Invalid();
            out += "\"\\/\b\f\n\r\t"[escaped];
        }
        return out;
    }

    ReleaseJson Value(unsigned depth) {
        using Type = ReleaseJson::Type;
        if (depth > 32) Invalid();
        ReleaseJson result;
        if (Take('{')) {
            result.type = Type::Object;
            if (!Take('}')) {
                do {
                    auto name = String();
                    if (!Take(':') || !result.fields.emplace(std::move(name), Value(depth + 1)).second) Invalid();
                } while (Take(','));
                if (!Take('}')) Invalid();
            }
        } else if (Take('[')) {
            result.type = Type::Array;
            if (!Take(']')) {
                do result.elements.push_back(Value(depth + 1)); while (Take(','));
                if (!Take(']')) Invalid();
            }
        } else if (!text_.empty() && text_.front() == '"') {
            result.type = Type::String;
            result.value = String();
        } else {
            const size_t length = text_.find_first_of(",]} \t\r\n");
            const auto token = text_.substr(0, length);
            if (token == "true" || token == "false") result.type = Type::Boolean;
            else if (token != "null") {
                const auto is_number = [](std::string_view value) {
                    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
                    size_t i = value.starts_with('-') ? 1 : 0;
                    if (i == value.size() || !digit(value[i])) return false;
                    if (value[i++] != '0') while (i < value.size() && digit(value[i])) ++i;
                    if (i < value.size() && value[i] == '.') {
                        if (++i == value.size() || !digit(value[i])) return false;
                        while (i < value.size() && digit(value[i])) ++i;
                    }
                    if (i < value.size() && (value[i] == 'e' || value[i] == 'E')) {
                        ++i;
                        if (i < value.size() && (value[i] == '+' || value[i] == '-')) ++i;
                        if (i == value.size() || !digit(value[i])) return false;
                        while (i < value.size() && digit(value[i])) ++i;
                    }
                    return i == value.size();
                };
                if (!is_number(token)) Invalid();
                result.type = Type::Number;
            }
            result.value = token;
            text_.remove_prefix(token.size());
        }
        return result;
    }
};

} // namespace launcher
