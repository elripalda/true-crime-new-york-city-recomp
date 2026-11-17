#include "game_addresses.h"
#include "core/dev_env.h"
// Extra Music Player songs from the custom music folder.
//
// The song manager (0x3735FC) keeps {tag root, SongsArray node, song count} and
// every per-song read goes through the array node: child i (0x218CB0), then a
// field by name hash (0x218A90). After the manager initialises (0xB89F0) its
// array is swapped for a copy with one more child per extra song. Each new
// child is a copy of the first song's node with its name, band, file number,
// price and initiallyAvailable replaced. The names are hashes the text lookup
// (0x260710) answers with the file's name. The player's song table has 128
// slots (player+0x1058, 4 bytes each); extra songs are marked owned in it
// before the Music Player builds its list (0xB8740), which covers old saves.
//
// Tag nodes: u16 count, u16, then 16-byte entries {type hash, name hash,
// u8 flags, u8, u16 count, value}. Flags bit 1 makes the value an offset from
// the entry to a child node.

#include "custom_music_decoder.h"

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"
#include "app/app.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace game {
namespace {

constexpr uint32_t kSongManager = TCNYC_PAL_EU ? 0x00373DCCu : TCNYC_PAL ? 0x373ECCu : 0x3735FCu;
constexpr uint32_t kPlayerPointer = TCNYC_PAL_EU ? 0x003615BCu : TCNYC_PAL ? 0x3616BCu : 0x360E00u;
constexpr uint32_t kFirstTrack = 6000;
constexpr uint32_t kOriginalLastTrack = 6092;
constexpr uint32_t kSongSlots = 128;      // the player's song table
constexpr uint32_t kOriginalSongs = 93;   // 6000..6092
constexpr uint32_t kMaxExtras = kSongSlots - kOriginalSongs;
constexpr uint32_t kTextHashBase = 0xC0570000u;  // name 2i, band 2i+1
constexpr uint32_t kNodeHashBase = 0xC0580000u;

constexpr uint32_t kSongName = 0xFCBABE45, kBandName = 0x5B348E7D, kFileNumber = 0xB18746C9,
                   kInitiallyAvailable = 0x6F5B46E0, kPrice = 0xCAC822D9, kGenre = 0x835033F8;
constexpr uint32_t kCustomGenre = 5;  // a sixth Music Player tab after the game's five

uint8_t R8(uint32_t va) { return *static_cast<uint8_t*>(kernel::HostPtr(va)); }
void W8(uint32_t va, uint8_t v) { *static_cast<uint8_t*>(kernel::HostPtr(va)) = v; }

struct Extra {
    fs::path path;
    std::string name, band;
    uint32_t name_va = 0, band_va = 0;  // guest copies of the text, made on first use
};

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string Utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool IsOriginalTrackName(const std::string& stem) {
    if (stem.empty() || stem.size() > 4 || !std::all_of(stem.begin(), stem.end(), ::isdigit)) return false;
    const unsigned long n = std::strtoul(stem.c_str(), nullptr, 10);
    return n >= kFirstTrack && n <= kOriginalLastTrack;
}

// The game's font is plain ASCII: anything else becomes '?'.
std::string Printable(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = s[i];
        if (c < 0x80) {
            out += c == '_' ? ' ' : char(c);
        } else if ((c & 0xC0) != 0x80) {
            out += '?';
        }
    }
    return out;
}

std::vector<Extra> Scan() {
    std::vector<Extra> extras;
    const fs::path folder = MusicFolder();
    if (folder.empty()) return extras;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        const std::string ext = Upper(Utf8(p.extension()));
        if (ext != ".MP3" && ext != ".OGG" && ext != ".WMA" && ext != ".FLAC" && ext != ".WAV") continue;
        if (IsOriginalTrackName(Utf8(p.stem()))) continue;
        std::error_code type_ec;
        if (!fs::is_regular_file(p, type_ec)) continue;
        Extra e;
        e.path = p;
        // "Band - Title" when the file is named that way
        const std::string stem = Printable(Utf8(p.stem()));
        if (const size_t dash = stem.find(" - "); dash != std::string::npos) {
            e.band = stem.substr(0, dash);
            e.name = stem.substr(dash + 3);
        } else {
            e.band = "Custom Music";
            e.name = stem;
        }
        extras.push_back(std::move(e));
    }
    std::sort(extras.begin(), extras.end(), [](const Extra& a, const Extra& b) { return a.path < b.path; });
    if (extras.size() > kMaxExtras) {
        LOGW("radio: %zu extra songs found; the Music Player holds %u, the rest are skipped", extras.size(),
             kMaxExtras);
        extras.resize(kMaxExtras);
    }
    if (!extras.empty()) LOGR("radio: %zu extra songs in the custom music folder", extras.size());
    return extras;
}

// Read once: the song list is built when the game starts.
std::vector<Extra>& Extras() {
    static std::vector<Extra> extras = Scan();
    return extras;
}

std::mutex g_text_lock;
std::atomic<uint32_t> g_added{0};  // extra songs actually added to the manager; read by the streamer thread

// The extended list, built once per source array: the manager initialises again on every game load and
// reuses the same tag data, so later inits re-point it at this copy instead of allocating another.
struct BuiltList {
    uint32_t source = 0, copy = 0, total = 0, added = 0;
} g_list;

uint32_t GuestString(const std::string& s) {
    const uint32_t va = mem::AllocSystem(uint32_t(s.size() + 1));
    if (va) std::memcpy(kernel::HostPtr(va), s.c_str(), s.size() + 1);
    return va;
}

uint32_t NodeSize(uint32_t node) { return 4 + uint32_t(kernel::R16(node)) * 16; }

// Copies entry src to dst, keeping a child offset pointing at the same node.
void CopyEntry(uint32_t dst, uint32_t src) {
    std::memcpy(kernel::HostPtr(dst), kernel::HostPtr(src), 16);
    if (R8(src + 8) & 2) kernel::W32(dst + 12, src + kernel::R32(src + 12) - dst);
}

uint32_t BuildSong(uint32_t templ, uint32_t index, uint32_t track) {
    const uint32_t size = NodeSize(templ);
    const uint32_t node = mem::AllocSystem(size);
    if (!node) return 0;
    std::memcpy(kernel::HostPtr(node), kernel::HostPtr(templ), 4);
    for (uint32_t e = 4; e < size; e += 16) {
        CopyEntry(node + e, templ + e);
        if (R8(node + e + 8) & 1) continue;  // an array, not a single value
        switch (kernel::R32(node + e + 4)) {
            case kSongName: kernel::W32(node + e + 12, kTextHashBase + index * 2); break;
            case kBandName: kernel::W32(node + e + 12, kTextHashBase + index * 2 + 1); break;
            case kFileNumber: kernel::W32(node + e + 12, track); break;
            case kInitiallyAvailable: kernel::W32(node + e + 12, 1); break;
            case kPrice: kernel::W32(node + e + 12, 0); break;
            case kGenre: kernel::W32(node + e + 12, kCustomGenre); break;
        }
    }
    return node;
}

void AddExtras(uint32_t manager) {
    const std::vector<Extra>& extras = Extras();
    const uint32_t array = kernel::R32(manager + 4);
    const uint32_t count = kernel::R32(manager + 8);
    if (array == g_list.copy) {  // already extended
        g_added = g_list.added;
        return;
    }
    // A later init that does not extend the list must not leave the previous
    // load's count behind: MarkOwned would mark original songs as owned.
    g_added = 0;
    if (extras.empty() || !array || !count) return;
    if (array == g_list.source && g_list.copy) {
        kernel::W32(manager + 4, g_list.copy);
        kernel::W32(manager + 8, g_list.total);
        g_added = g_list.added;
        return;
    }
    if (count != kernel::R16(array) || count >= kSongSlots) return;
    if (count != kOriginalSongs) LOGW("radio: song list has %u songs, expected %u", count, kOriginalSongs);
    const uint32_t first = array + 4;
    if (!(R8(first + 8) & 2)) return;
    const uint32_t templ = first + kernel::R32(first + 12);

    const uint32_t added = std::min<uint32_t>(uint32_t(extras.size()), kSongSlots - count);
    const uint32_t total = count + added;
    const uint32_t copy = mem::AllocSystem(4 + total * 16);
    if (!copy) return;
    std::memcpy(kernel::HostPtr(copy), kernel::HostPtr(array), 4);
    kernel::W16(copy, uint16_t(total));
    for (uint32_t i = 0; i < count; ++i) CopyEntry(copy + 4 + i * 16, array + 4 + i * 16);
    std::vector<uint32_t> songs;
    for (uint32_t i = 0; i < added; ++i) {
        const uint32_t entry = copy + 4 + (count + i) * 16;
        const uint32_t song = BuildSong(templ, i, kOriginalLastTrack + 1 + i);
        if (!song) {
            for (const uint32_t built : songs) mem::FreeSystem(built);
            mem::FreeSystem(copy);
            LOGW("radio: out of memory building the extra song list");
            return;
        }
        songs.push_back(song);
        std::memcpy(kernel::HostPtr(entry), kernel::HostPtr(first), 16);
        kernel::W32(entry + 4, kNodeHashBase + i);
        kernel::W32(entry + 12, song - entry);
    }
    if (g_list.copy) LOGW("radio: song list source moved; building it again");
    g_list = {array, copy, total, added};
    kernel::W32(manager + 4, copy);
    kernel::W32(manager + 8, total);
    g_added = added;
    LOGR("radio: Music Player list extended to %u songs", total);
}

// Extra songs start owned, whatever an older save recorded for their slots.
void MarkOwned() {
    const uint32_t player = kernel::R32(kPlayerPointer);
    const uint32_t count = kernel::R32(kSongManager + 8);
    if (!player || !g_added || count > kSongSlots || count < g_added) return;
    for (uint32_t i = count - g_added; i < count; ++i) {
        const uint32_t slot = player + 0x1058 + i * 4;
        if (R8(slot + 2)) continue;
        W8(slot, 3);
        W8(slot + 1, 3);
        W8(slot + 2, 1);
        W8(slot + 3, 0xFF);
    }
}

} // namespace

fs::path MusicFolder() {
    fs::path folder;
    if (const std::string chosen = app::Snapshot().music_folder; !chosen.empty())
        folder = fs::path(reinterpret_cast<const char8_t*>(chosen.c_str()));
#if TCNYC_DEV_TOOLS
#ifdef _WIN32
    else if (const wchar_t* test = DevWEnv(L"TCNYC_TEST_RADIO_FOLDER"))
        folder = test;
#else
    else if (const char* test = DevEnv("TCNYC_TEST_RADIO_FOLDER"))
        folder = test;
#endif
#endif
    // the default: music\ beside the executable, made on first use so it is easy to find
    if (folder.empty()) {
        folder = app::ExeDir() / "music";
        std::error_code ec;
        fs::create_directories(folder, ec);
    }
    if (folder.is_relative()) folder = app::ExeDir() / folder;
    return folder;
}

// Only songs that made it into the game's list are valid tracks.
fs::path ExtraSongPath(uint32_t track) {
    if (track <= kOriginalLastTrack || track - kOriginalLastTrack - 1 >= g_added.load()) return {};
    return Extras()[track - kOriginalLastTrack - 1].path;
}

uint32_t LastMusicTrack() { return kOriginalLastTrack + g_added.load(); }

} // namespace game

// song manager init (this, tag root)
X86_EXTERN_FUNC(__imp__sub_000B89F0);
X86_FUNC(sub_000B89F0) {
    const uint32_t manager = ctx.ecx;
    __imp__sub_000B89F0(ctx, base);
    game::AddExtras(manager);
}

// Music Player list build: reads the owned flags
X86_EXTERN_FUNC(__imp__sub_000B8740);
X86_FUNC(sub_000B8740) {
    game::MarkOwned();
    __imp__sub_000B8740(ctx, base);
}

// next song for the car radio: picks among owned songs, so mark the extras
// first in case the Music Player has not been opened this session
X86_EXTERN_FUNC(__imp__sub_00079A60);
X86_FUNC(sub_00079A60) {
    game::MarkOwned();
    __imp__sub_00079A60(ctx, base);
}

// Music Player tab filter (this): lists owned songs of genre this+0x2C. The
// input handler wraps the tab between 0 and 4 before calling it; a wrap is
// turned into a stop at the custom tab instead. The call made when the Music Player opens (right after
// the list build, returning to 0xC15AE) is never a wrap: the menu object is reused, so its previous tab
// would otherwise make a reopen at tab 0 after closing on tab 4 look like 4 -> 0.
namespace game {
namespace {
constexpr uint32_t kTabFilterOpenReturn = TC_ADDR(0x000C15AEu);
uint32_t g_last_tab_menu = 0, g_last_tab = 0;
bool g_on_custom_tab = false;
}
}
X86_EXTERN_FUNC(__imp__sub_000AFF30);
X86_FUNC(sub_000AFF30) {
    const uint32_t menu = ctx.ecx;
    uint32_t tab = kernel::R32(menu + 0x2C);
    const bool opening = kernel::R32(ctx.esp) == game::kTabFilterOpenReturn;
    if (game::g_added && !opening && menu == game::g_last_tab_menu) {
        const uint32_t last = game::g_last_tab;
        if ((last == 4 && tab == 0) || (last == 0 && tab == 4)) {
            tab = game::kCustomGenre;
            kernel::W32(menu + 0x2C, tab);
        }
    }
    game::g_last_tab_menu = menu;
    game::g_last_tab = tab;
    if (tab == game::kCustomGenre && !game::g_on_custom_tab) LOGR("radio: custom music tab opened");
    game::g_on_custom_tab = tab == game::kCustomGenre;
    __imp__sub_000AFF30(ctx, base);
}


// text lookup (this, hash, bool* found)
X86_EXTERN_FUNC(__imp__sub_00260710);
X86_FUNC(sub_00260710) {
    const uint32_t hash = kernel::R32(ctx.esp + 4);
    // The tab label is the text for table[tab] at 0x2E6EEC (five hashes). On the
    // custom tab the read lands on the float 1.0 stored after the table.
    if (hash == 0x3F800000u && game::g_on_custom_tab) {
        static uint32_t label = game::GuestString("CUSTOM MUSIC");
        if (const uint32_t found = kernel::R32(ctx.esp + 8)) game::W8(found, label != 0);
        ctx.eax = label;
        ctx.esp += 12;
        return;
    }
    const uint32_t index = (hash - game::kTextHashBase) / 2;
    if (hash - game::kTextHashBase >= game::kMaxExtras * 2 || index >= game::g_added) {
        __imp__sub_00260710(ctx, base);
        return;
    }
    game::Extra& e = game::Extras()[index];
    uint32_t va;
    {
        std::lock_guard<std::mutex> lock(game::g_text_lock);
        uint32_t& slot = hash & 1 ? e.band_va : e.name_va;
        if (!slot) slot = game::GuestString(hash & 1 ? e.band : e.name);
        va = slot;
    }
    if (const uint32_t found = kernel::R32(ctx.esp + 8)) game::W8(found, va != 0);
    ctx.eax = va;
    ctx.esp += 12;
}
