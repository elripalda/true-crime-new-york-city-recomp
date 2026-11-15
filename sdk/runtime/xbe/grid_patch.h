#pragma once
// City-cell grid remainder fix, shared by the CMake generation step (tools/grid_patch) and the
// first-launch compiler (runtime/app/game_code.cpp), so both paths produce identical code.
//
// The grid routines divide (coordinate + 1000) with signed idiv and use the remainder as a 3x3
// bucket. A negative coordinate gives a negative remainder; the index becomes -1 and the 16-byte
// record is written over the end of cell slot 8 (+0x70/+0x74), corrupting its pointers and crashing
// the renderer. Only the divisions at the guest addresses below feed that index; make their
// remainder the mathematical modulo by adding the divisor when it is negative. Non-negative
// dividends are unchanged and the quotient is discarded in these routines.
//
// The sites are explicit. Before changing anything the patcher verifies, for every occurrence, the
// guest address, the idiv operand, the divisor load and the divide-error trap in the generated
// block. It also requires the exact number of occurrences, so a recompiler change that moves or
// splits these routines fails the build instead of patching an unrelated division.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace xbe {

// Guest idiv instructions whose remainder feeds the 3x3 cell index. The recompiler emits
// continuation entries for some functions, so the same address appears in more than one host
// function; `occurrences` is the count across the configured partitions for this title.
struct GridSite {
    uint32_t address;
    const char* operand;  // idiv operand, which must also be the divisor loaded into d
    uint32_t occurrences;
};

#if defined(TCNYC_PAL) && TCNYC_PAL
inline constexpr GridSite kGridSites[] = {
    {0x0013945D, "ecx", 1},
    {0x00139474, "edi", 1},
    {0x0013FAA2, "ecx", 1},
    {0x0013FAB9, "edi", 1},
    {0x00147420, "esi", 3},
    {0x0014746D, "ecx", 3},
    {0x00147743, "esi", 2},
    {0x0014778D, "ecx", 2},
    {0x00147A63, "edi", 2},
    {0x00147AAD, "ecx", 2},
    {0x0014ACC1, "ecx", 1},
    {0x0014ACE8, "esi", 1},
};
#else
inline constexpr GridSite kGridSites[] = {
    {0x00138BCD, "ecx", 1},
    {0x00138BE4, "edi", 1},
    {0x0013F212, "ecx", 1},
    {0x0013F229, "edi", 1},
    {0x00146D30, "esi", 3},
    {0x00146D7D, "ecx", 3},
    {0x00147053, "esi", 2},
    {0x0014709D, "ecx", 2},
    {0x00147373, "edi", 2},
    {0x001473BD, "ecx", 2},
    {0x0014A5D1, "ecx", 1},
    {0x0014A5F8, "esi", 1},
};
#endif
inline constexpr uint32_t kGridSiteCount = sizeof(kGridSites) / sizeof(kGridSites[0]);
inline constexpr uint32_t kGridExpectedOccurrences = 20;

// 0x13E7F0 builds the visible-portal list in a stack frame: sub esp, 0x204, list at esp+0x190, so only
// 0x204-0x190 = 0x74 bytes (29 entries) before it runs into the saved registers/return address. The 5x5
// ring (16 extra nodes) fits; the 7x7 ring (40) overflows it and crashes in an indirect call. Enlarge the
// frame; all locals are esp-relative, so the extra room is safe.
struct FrameSite {
    uint32_t address;
    uint32_t from;
    uint32_t to;
};
#if defined(TCNYC_PAL) && TCNYC_PAL
inline constexpr FrameSite kFrameSites[] = {{0x0013F086, 0x204, 0x804}};
#else
inline constexpr FrameSite kFrameSites[] = {{0x0013E7F6, 0x204, 0x1004}};
#endif
inline constexpr uint32_t kFrameSiteCount = sizeof(kFrameSites) / sizeof(kFrameSites[0]);
inline constexpr uint32_t kFrameExpectedOccurrences = 1;

// The dynamic light table: 16 pointers at 0x349380 with the count at 0x3493CC. 0x3493C0 onwards is other
// data, so the 17th light (reached by the 7x7 ring) lands in a foreign global and 0x61370 later reads a null
// light. Lights are addressed only by index (each stores its own at +0x94), so every site reads the base
// through xbe_light_table instead; the host moves the table to a larger array at a quiet point.
#if defined(TCNYC_PAL) && TCNYC_PAL
inline constexpr uint32_t kLightSites[] = {0x00061430, 0x000617C0, 0x000619B4, 0x00061D40, 0x00063729,
                                           0x0006374C, 0x0006375F, 0x0006376C, 0x00063783};
#else
inline constexpr uint32_t kLightSites[] = {0x00061400, 0x00061790, 0x00061984, 0x00061D10, 0x000636F9,
                                           0x0006371C, 0x0006372F, 0x0006373C, 0x00063753};
#endif
inline constexpr uint32_t kLightSiteCount = sizeof(kLightSites) / sizeof(kLightSites[0]);
inline const char kLightDecl[] = "extern \"C\" uint32_t xbe_light_table;";
#if defined(TCNYC_PAL) && TCNYC_PAL
inline const char kLightDef[] =
    "#ifdef _WIN32\n__declspec(dllexport)\n#endif\nextern \"C\" uint32_t xbe_light_table = 0x349C40u;";
inline const char kLightFrom[] = "0x349C40u";
inline const char kLightFromMinus4[] = "0x349C3Cu";
#else
inline const char kLightDef[] =
    "#ifdef _WIN32\n__declspec(dllexport)\n#endif\nextern \"C\" uint32_t xbe_light_table = 0x349380u;";
inline const char kLightFrom[] = "0x349380u";
inline const char kLightFromMinus4[] = "0x34937Cu";
#endif

// The per-frame visible city instance list: 300 entries of {float sort key, instance} at city + 0x5AD0, count
// at city + 0x6430, filled by 0x14A240/0x148A00 from the game's 3x3 tiles only and drawn by 0x144250 and
// 0x1387F0. Every site that addresses it (base +0x5AD0/+0x5AD4) or checks its 300 cap (0x12C) goes through
// XBE_INST(city) / xbe_inst_cap, so the host can move it to a larger array for the outer ring. Until it does,
// XBE_INST(city) is city + 0x5AD0 and the cap is 300: unchanged behaviour. NTSC addresses only.
#if defined(TCNYC_PAL) && TCNYC_PAL
inline constexpr uint32_t kInstSites[] = {0};
inline constexpr uint32_t kInstSiteCount = 0;
#else
inline constexpr uint32_t kInstSites[] = {0x0013882F, 0x00138D80, 0x0013F8E0, 0x00144590, 0x00148BBA, 0x00148BE7,
                                          0x00148C60, 0x00148C6C, 0x00148CA1, 0x0014A939, 0x0014A966, 0x0014AA63,
                                          0x0014AA75, 0x0014AA7B, 0x0014AAC0, 0x0014AAC5,
                                          // insert 0x1481D0 / evict 0x147720 read the count at list base + 0x960
                                          0x001481D3, 0x001481EF, 0x00148206, 0x00148210, 0x0014821D, 0x00147723,
                                          0x0014772F, 0x00147754};
inline constexpr uint32_t kInstSiteCount = sizeof(kInstSites) / sizeof(kInstSites[0]);
#endif
inline const char kInstMacro[] =
    "#define XBE_INST(c) (xbe_inst_table ? xbe_inst_table : (uint32_t)(c) + 0x5AD0u)\n"
    "#define XBE_CNT(l) (xbe_inst_table && (uint32_t)(l) == xbe_inst_table ? xbe_inst_count : (uint32_t)(l) + 0x960u)";
inline const char kInstDecl[] = "extern \"C\" uint32_t xbe_inst_table, xbe_inst_cap, xbe_inst_count;";
inline const char kInstDef[] =
    "#ifdef _WIN32\n__declspec(dllexport)\n#endif\nextern \"C\" uint32_t xbe_inst_table = 0u;\n"
    "#ifdef _WIN32\n__declspec(dllexport)\n#endif\nextern \"C\" uint32_t xbe_inst_cap = 0x12Cu;\n"
    "#ifdef _WIN32\n__declspec(dllexport)\n#endif\nextern \"C\" uint32_t xbe_inst_count = 0u;";

// Statements appended after a guest instruction. 0x13E7F0 (portal pass) checks an instance slot in one object's
// table ([[esp+0x44]+0x50][idx], 0x13EC93) but then reads the city's table ([[0x37F8FC]+0x50][idx], 0x13ECB6)
// and dereferences it at 0x13ECB9. In the normal game both are the same object; with the outer ring they can
// differ, the city entry is null, and the pass crashed reading guest 0x30. Take the game's own skip path
// (0x13EE8A, the target of the first check) when that entry is null. NTSC only.
struct AppendSite {
    uint32_t address;
    const char* text;
};
#if defined(TCNYC_PAL) && TCNYC_PAL
inline constexpr AppendSite kAppendSites[] = {{0, ""}};
inline constexpr uint32_t kAppendSiteCount = 0;
#else
inline constexpr AppendSite kAppendSites[] = {{0, ""}};  // 0x13ECB6 null skip reverted: crashed sooner (181 s)
inline constexpr uint32_t kAppendSiteCount = 0;
#endif

// Constants replaced at a guest instruction. 0x13E7F0 also keeps a byte array of portal flags at esp+0xA0+i next to
// its pointer list at esp+0x190+i*4: only 240 bytes before the flags overwrite portal pointers. The 9x9 ring pushes
// more portals than that; a corrupted pointer then gave an object with a null table (+0x54) and a crash at
// 0x13EB1E (read of guest 0x30, ~17 min into the attract screen). With the frame at 0x1004 the flags move to
// esp+0xC00 (1028 entries) and the pointer list keeps 0x190..0xC00 (668 entries). NTSC only.
struct ReplaceSite {
    uint32_t address;
    const char* from;
    const char* to;
};
#if defined(TCNYC_PAL) && TCNYC_PAL
inline constexpr ReplaceSite kReplaceSites[] = {{0, "", ""}};
inline constexpr uint32_t kReplaceSiteCount = 0;
#else
inline constexpr ReplaceSite kReplaceSites[] = {{0x0013EA38, "0xA0u", "0xC00u"}, {0x0013EA50, "0xA0u", "0xC00u"},
                                                {0x0013EA70, "0xA0u", "0xC00u"}, {0x0013EA88, "0xA0u", "0xC00u"},
                                                {0x0013EA9F, "0xA0u", "0xC00u"}};
inline constexpr uint32_t kReplaceSiteCount = 5;
#endif

inline const ReplaceSite* ReplaceSiteAt(uint32_t address) {
    for (uint32_t i = 0; i < kReplaceSiteCount; ++i)
        if (kReplaceSites[i].address == address) return &kReplaceSites[i];
    return nullptr;
}

inline const AppendSite* AppendSiteAt(uint32_t address) {
    for (uint32_t i = 0; i < kAppendSiteCount; ++i)
        if (kAppendSites[i].address == address) return &kAppendSites[i];
    return nullptr;
}

inline bool IsInstSite(uint32_t address) {
    for (uint32_t i = 0; i < kInstSiteCount; ++i)
        if (kInstSites[i] == address) return true;
    return false;
}

// Rewrites one generated statement; false if it has none of the three expected shapes.
inline bool RewriteInstSite(std::string& code) {
    if (const size_t at = code.find("(uint32_t)(ctx.esi + 0x960u)"); at != std::string::npos) {  // count via list base
        code.replace(at, 28, "(uint32_t)(XBE_CNT(ctx.esi))");
        if (const size_t cap = code.find("(0x12Cu)"); cap != std::string::npos) code.replace(cap, 8, "(xbe_inst_cap)");
        return true;
    }
    for (const auto& [from, to] : {std::pair<std::string, std::string>{"(uint32_t)(0x5AD0u)", "(uint32_t)(XBE_INST(a) - a)"},
                                   {"(uint32_t)(0x5AD4u)", "(uint32_t)(XBE_INST(a) + 4u - a)"}}) {
        if (const size_t at = code.find(from); at != std::string::npos) {  // add reg, 0x5AD0 / 0x5AD4
            code.replace(at, from.size(), to);
            return true;
        }
    }
    if (const size_t at = code.find("(0x12Cu)"); at != std::string::npos) {  // the 300 cap
        code.replace(at, 8, "(xbe_inst_cap)");
        return true;
    }
    for (const auto& [from, to] : {std::pair<std::string, std::string>{"0x5AD0u", "0u"}, {"0x5AD4u", "4u"}}) {
        const size_t k = code.find(from);
        if (k == std::string::npos) continue;
        const size_t r = code.rfind("(uint32_t)(ctx.e", k);  // ea = (uint32_t)(ctx.REG + [ctx.IDX * 8u + ]K)
        if (r == std::string::npos) return false;
        const size_t reg = r + 11;
        code.replace(k, from.size(), to);
        code.replace(reg, 7, "XBE_INST(" + code.substr(reg, 7) + ")");
        return true;
    }
    return false;
}

inline bool IsLightSite(uint32_t address) {
    for (uint32_t site : kLightSites)
        if (site == address) return true;
    return false;
}

struct GridPatchResult {
    uint32_t files = 0;
    uint32_t occurrences = 0;
    std::string error;
    bool ok() const { return error.empty(); }
};

inline std::string ToHex(uint32_t value, int digits = 8) {
    char text[16];
    std::snprintf(text, sizeof(text), "%0*X", digits, value);
    return text;
}

inline bool ParseHex(const std::string& text, size_t at, size_t count, uint32_t& out) {
    if (at + count > text.size()) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < count; i++) {
        const char ch = text[at + i];
        const int digit = ch >= '0' && ch <= '9' ? ch - '0'
                            : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                            : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                                                     : -1;
        if (digit < 0) return false;
        value = value * 16 + uint32_t(digit);
    }
    out = value;
    return true;
}

inline const GridSite* GridSiteAt(uint32_t address) {
    for (const GridSite& site : kGridSites)
        if (site.address == address) return &site;
    return nullptr;
}

inline const FrameSite* FrameSiteAt(uint32_t address) {
    for (const FrameSite& site : kFrameSites)
        if (site.address == address) return &site;
    return nullptr;
}

// Applies the fix to dir/x86_recomp.0..partitions-1.cpp. Every expected partition, site and
// occurrence is required; files are replaced through a temporary file, and any mismatch is an
// error. Calling it again on already-corrected sources verifies and accepts them.
inline GridPatchResult PatchGridSources(const std::filesystem::path& dir, uint32_t partitions) {
    GridPatchResult result;
    if (!partitions) {
        result.error = "the image has no recompiled partitions";
        return result;
    }
    const std::string plain = "ctx.edx = (uint32_t)(n % d); }";
    const std::string fixed =
        "ctx.edx = (uint32_t)(n % d); if ((int32_t)ctx.edx < 0) ctx.edx += (uint32_t)d; }";
    struct Change {
        std::filesystem::path file, temp;
        std::string text;
    };
    struct Changes : std::vector<Change> {
        ~Changes() {
            for (const auto& change : *this) {
                if (change.temp.empty()) continue;
                std::error_code ignored;
                std::filesystem::remove(change.temp, ignored);
            }
        }
    } changes;
    std::vector<uint32_t> seen(kGridSiteCount, 0);
    std::vector<uint32_t> seen_frame(kFrameSiteCount, 0);
    uint32_t frame_occurrences = 0;
    uint32_t light_occurrences = 0;
    uint32_t inst_occurrences = 0;
    uint32_t append_occurrences = 0;
    uint32_t replace_occurrences = 0;
    for (uint32_t part = 0; part < partitions; part++) {
        const std::filesystem::path file = dir / ("x86_recomp." + std::to_string(part) + ".cpp");
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            result.error = "cannot read " + file.string();
            return result;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad()) {
            result.error = "read failed for " + file.string();
            return result;
        }
        in.close();
        const bool trailing_newline = !text.empty() && text.back() == '\n';
        std::vector<std::string> lines;
        {
            std::istringstream stream(text);
            std::string line;
            while (std::getline(stream, line)) lines.push_back(line);
        }
        bool changed = false;
        bool light_here = false;
        bool inst_here = false;
        for (size_t i = 0; i < lines.size(); i++) {
            const std::string& line = lines[i];
            const size_t comment = line.find("// ");
            if (comment == std::string::npos) continue;
            uint32_t address = 0;
            if (!ParseHex(line, comment + 3, 8, address)) continue;
            if (comment + 12 > line.size()) continue;
            if (IsLightSite(address)) {
                size_t target = i + 1;
                while (target < lines.size() && lines[target].find_first_not_of(" \t\r") == std::string::npos)
                    target++;
                std::string& code = lines[target < lines.size() ? target : i];
                bool hit = false;
                for (const auto& [from, to] : {std::pair<std::string, std::string>{kLightFrom, "xbe_light_table"},
                                               {kLightFromMinus4, "(xbe_light_table - 4u)"}}) {
                    const size_t at = code.find(from);
                    if (at != std::string::npos) {
                        code.replace(at, from.size(), to);
                        changed = hit = true;
                    }
                }
                if (!hit && code.find("xbe_light_table") == std::string::npos) {
                    result.error = "light site " + ToHex(address) + " does not match";
                    return result;
                }
                light_here = true;
                light_occurrences++;
                continue;
            }
            if (const ReplaceSite* site = ReplaceSiteAt(address)) {
                size_t target = i + 1;
                while (target < lines.size() && lines[target].find_first_not_of(" \t\r") == std::string::npos)
                    target++;
                if (target >= lines.size()) {
                    result.error = "replace site " + ToHex(address) + " has no statement";
                    return result;
                }
                std::string& code = lines[target];
                const std::string from = std::string("+ ") + site->from + ")";
                const std::string to = std::string("+ ") + site->to + ")";
                if (const size_t at = code.find(from); at != std::string::npos) {
                    code.replace(at, from.size(), to);
                    changed = true;
                } else if (code.find(to) == std::string::npos) {
                    result.error = "replace site " + ToHex(address) + " does not match";
                    return result;
                }
                replace_occurrences++;
                continue;
            }
            if (const AppendSite* site = AppendSiteAt(address)) {
                size_t target = i + 1;
                while (target < lines.size() && lines[target].find_first_not_of(" \t\r") == std::string::npos)
                    target++;
                if (target >= lines.size()) {
                    result.error = "append site " + ToHex(address) + " has no statement";
                    return result;
                }
                if (lines[target].find(site->text) == std::string::npos) {
                    std::string& code = lines[target];
                    const bool cr = !code.empty() && code.back() == '\r';
                    if (cr) code.pop_back();
                    code += site->text;
                    if (cr) code += '\r';
                    changed = true;
                }
                append_occurrences++;
                continue;
            }
            if (IsInstSite(address)) {
                size_t target = i + 1;
                while (target < lines.size() && lines[target].find_first_not_of(" \t\r") == std::string::npos)
                    target++;
                std::string& code = lines[target < lines.size() ? target : i];
                if (code.find("XBE_INST(") == std::string::npos && code.find("xbe_inst_cap") == std::string::npos) {
                    if (!RewriteInstSite(code)) {
                        result.error = "instance site " + ToHex(address) + " does not match";
                        return result;
                    }
                    changed = true;
                }
                inst_here = true;
                inst_occurrences++;
                continue;
            }
            if (line.compare(comment + 12, 3, "sub") == 0) {
                const FrameSite* fs = FrameSiteAt(address);
                if (!fs) continue;
                const std::string expect = "sub esp, 0x" + ToHex(fs->from, 1);
                if (line.find(expect) == std::string::npos) {
                    result.error = "guest " + ToHex(address) + " does not match '" + expect + "'";
                    return result;
                }
                size_t target = i + 1;
                while (target < lines.size() && lines[target].find_first_not_of(" \t\r") == std::string::npos)
                    target++;
                if (target == lines.size()) {
                    result.error = "the sub block after guest " + ToHex(address) + " is missing";
                    return result;
                }
                const std::string from_u = "0x" + ToHex(fs->from, 1) + "u";
                const std::string to_u = "0x" + ToHex(fs->to, 1) + "u";
                const size_t at = lines[target].find(from_u);
                if (at != std::string::npos) {
                    lines[target].replace(at, from_u.size(), to_u);
                    changed = true;
                } else if (lines[target].find(to_u) == std::string::npos) {
                    result.error = "unexpected sub frame after guest " + ToHex(address);
                    return result;
                }
                seen_frame[size_t(fs - kFrameSites)]++;
                frame_occurrences++;
                continue;
            }
            if (line.compare(comment + 12, 4, "idiv") != 0) continue;
            const GridSite* site = GridSiteAt(address);
            if (!site) continue;
            const size_t operand_at = comment + 16;
            if (operand_at >= line.size() || line[operand_at] != ' ') {
                result.error = "malformed idiv comment at guest " + ToHex(address);
                return result;
            }
            const size_t operand_end = line.find_first_of(" \t\r", operand_at + 1);
            const std::string operand =
                line.substr(operand_at + 1, operand_end == std::string::npos ? std::string::npos
                                                                             : operand_end - operand_at - 1);
            if (operand != site->operand) {
                result.error = "guest " + ToHex(address) + " divides by " + operand + ", expected " +
                               site->operand;
                return result;
            }
            size_t target = i + 1;
            while (target < lines.size() && lines[target].find_first_not_of(" \t\r") == std::string::npos) target++;
            if (target == lines.size()) {
                result.error = "the idiv block after guest " + ToHex(address) + " is missing";
                return result;
            }
            const std::string& block = lines[target];
            const std::string divisor = "int32_t d = (int32_t)ctx." + operand + ";";
            const std::string trap = "x86_trap(ctx, 0x" + ToHex(address, 1) + ", \"divide error\");";
            if (block.find("int64_t n = (int64_t)") == std::string::npos ||
                block.find(divisor) == std::string::npos || block.find(trap) == std::string::npos) {
                result.error = "guest " + ToHex(address) + " does not match the expected idiv block";
                return result;
            }
            const size_t at = block.find(plain);
            const size_t already = block.find(fixed);
            if (at != std::string::npos) {
                lines[target].replace(at, plain.size(), fixed);
                changed = true;
            } else if (already == std::string::npos) {
                result.error = "unexpected idiv remainder after guest " + ToHex(address);
                return result;
            }
            const size_t index = size_t(site - kGridSites);
            seen[index]++;
            result.occurrences++;
        }
        // Declare (or, in partition 0, define) the light table base once per file that needs it.
        const std::string want = part == 0 ? kLightDef : kLightDecl;
        if ((part == 0 || light_here) && text.find(want) == std::string::npos && !lines.empty()) {
            lines.insert(lines.begin() + 1, want);
            changed = true;
        }
        if (kInstSiteCount && (part == 0 || inst_here) && !lines.empty()) {
            const std::string inst = part == 0 ? kInstDef : kInstDecl;
            if (text.find(kInstMacro) == std::string::npos) {
                lines.insert(lines.begin() + 1, kInstMacro);
                changed = true;
            }
            if (text.find(inst) == std::string::npos) {
                lines.insert(lines.begin() + 1, inst);
                changed = true;
            }
        }
        if (changed) {
            std::string patched;
            for (size_t i = 0; i < lines.size(); i++) {
                patched += lines[i];
                if (i + 1 < lines.size() || trailing_newline) patched += '\n';
            }
            changes.push_back({file, {}, std::move(patched)});
        }
        result.files++;
    }
    for (uint32_t i = 0; i < kGridSiteCount; i++) {
        if (seen[i] != kGridSites[i].occurrences) {
            result.error = "guest " + ToHex(kGridSites[i].address) + " appears " + std::to_string(seen[i]) +
                           " time(s), expected " + std::to_string(kGridSites[i].occurrences);
            return result;
        }
    }
    if (result.occurrences != kGridExpectedOccurrences) {
        result.error = "patched " + std::to_string(result.occurrences) + " sites, expected " +
                       std::to_string(kGridExpectedOccurrences);
        return result;
    }
    for (uint32_t i = 0; i < kFrameSiteCount; i++) {
        if (seen_frame[i] != 1) {
            result.error = "frame site " + ToHex(kFrameSites[i].address) + " appears " +
                           std::to_string(seen_frame[i]) + " time(s), expected 1";
            return result;
        }
    }
    if (frame_occurrences != kFrameExpectedOccurrences) {
        result.error = "patched " + std::to_string(frame_occurrences) + " frame sites, expected " +
                       std::to_string(kFrameExpectedOccurrences);
        return result;
    }
    if (replace_occurrences < kReplaceSiteCount) {
        result.error = "patched " + std::to_string(replace_occurrences) + " replace sites, expected " +
                       std::to_string(kReplaceSiteCount);
        return result;
    }
    if (append_occurrences < kAppendSiteCount) {
        result.error = "patched " + std::to_string(append_occurrences) + " append sites, expected " +
                       std::to_string(kAppendSiteCount);
        return result;
    }
    if (inst_occurrences < kInstSiteCount) {
        result.error = "patched " + std::to_string(inst_occurrences) + " instance sites, expected at least " +
                       std::to_string(kInstSiteCount);
        return result;
    }
    if (light_occurrences < kLightSiteCount) {
        result.error = "patched " + std::to_string(light_occurrences) + " light sites, expected at least " +
                       std::to_string(kLightSiteCount);
        return result;
    }
    // Validate the whole image before staging anything. Exclusive staging files also make a
    // retry safe after interruption: a previous run's leftovers are never opened for truncation.
    static std::atomic<uint64_t> serial{0};
    for (auto& change : changes) {
        std::FILE* out = nullptr;
        for (int attempt = 0; attempt < 64 && !out; attempt++) {
            const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
            change.temp = change.file;
            change.temp += ".gridtmp." + std::to_string(tick) + "." + std::to_string(serial.fetch_add(1));
#ifdef _WIN32
            out = _wfopen(change.temp.c_str(), L"wbx");
#else
            out = std::fopen(change.temp.c_str(), "wbx");
#endif
            if (!out) change.temp.clear();  // never clean up a file owned by another run
        }
        if (!out) {
            result.error = "cannot stage " + change.file.string();
            return result;
        }
        const bool wrote = std::fwrite(change.text.data(), 1, change.text.size(), out) == change.text.size();
        const bool closed = std::fclose(out) == 0;
        if (!wrote || !closed) {
            result.error = "write failed for " + change.temp.string();
            return result;
        }
    }
    for (auto& change : changes) {
#ifdef _WIN32
        if (!MoveFileExW(change.temp.c_str(), change.file.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            result.error = "replacing " + change.file.string() + " failed: " + std::to_string(GetLastError());
            return result;
        }
#else
        std::error_code ec;
        std::filesystem::rename(change.temp, change.file, ec);
        if (ec) {
            result.error = "replacing " + change.file.string() + " failed: " + ec.message();
            return result;
        }
#endif
        change.temp.clear();
    }
    return result;
}

}  // namespace xbe
