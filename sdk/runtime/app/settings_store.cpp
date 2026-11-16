#include "app/settings_store.h"
#include "app/app.h"
#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace app {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

class PublicationLock {
public:
    explicit PublicationLock(const fs::path& path) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
#ifdef _WIN32
        file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not open the settings lock.");
        OVERLAPPED region{};
        while (!LockFileEx(file_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &region)) {
            if (GetLastError() != ERROR_LOCK_VIOLATION || std::chrono::steady_clock::now() >= deadline) {
                CloseHandle(file_);
                throw std::runtime_error("Could not lock the settings file.");
            }
            std::this_thread::sleep_for(20ms);
        }
#else
        file_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (file_ < 0) throw std::runtime_error("Could not open the settings lock.");
        while (flock(file_, LOCK_EX | LOCK_NB) != 0) {
            if ((errno != EWOULDBLOCK && errno != EINTR) || std::chrono::steady_clock::now() >= deadline) {
                close(file_);
                throw std::runtime_error("Could not lock the settings file.");
            }
            std::this_thread::sleep_for(20ms);
        }
#endif
    }
    PublicationLock(const PublicationLock&) = delete;
    ~PublicationLock() {
#ifdef _WIN32
        CloseHandle(file_);
#else
        close(file_);
#endif
    }
private:
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
#else
    int file_ = -1;
#endif
};

void Publish(const fs::path& path, const std::string& text) {
    auto temp = path;
#ifdef _WIN32
    static std::atomic<uint32_t> serial{};
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 32; ++attempt) {
        temp = path;
        temp += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(serial.fetch_add(1));
        file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not create the temporary settings file.");
    DWORD written = 0;
    const bool saved = WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr) &&
                       written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    const bool published = saved && MoveFileExW(temp.c_str(), path.c_str(),
                                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    temp += ".tmp.XXXXXX";
    std::string name = temp.native();
    const int fd = mkstemp(name.data());
    if (fd < 0) throw std::runtime_error("Could not create the temporary settings file.");
    temp = name;
    FILE* file = fdopen(fd, "wb");
    bool saved = false;
    if (file) {
        saved = std::fwrite(text.data(), 1, text.size(), file) == text.size() &&
                std::fflush(file) == 0 && fsync(fd) == 0;
        saved = std::fclose(file) == 0 && saved;
    } else {
        close(fd);
    }
    const bool published = saved && std::rename(temp.c_str(), path.c_str()) == 0;
#endif
    if (!published) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        throw std::runtime_error("Could not publish the settings file.");
    }
}

bool SameNode(const toml::node& a, const toml::node& b) {
    return a.visit([&](const auto& value) {
        const auto* other = b.as<std::decay_t<decltype(value)>>();
        return other && *other == value;
    });
}

void Merge(toml::table& table, const toml::table& edits) {
    for (const auto& [key, value] : edits) {
        auto* target = table[key].as_table();
        const auto* nested = value.as_table();
        if (target && nested) Merge(*target, *nested);
        else table.insert_or_assign(key, value);
    }
}

} // namespace

void LoadOcclusionSettings(Settings& settings, const toml::table& table) {
    if (table["ao"].is_boolean()) {
        settings.ao = table["ao"].value_or(false) ? 2u : 0u;
    } else {
        settings.ao = uint32_t(std::clamp<int64_t>(table["ao"].value_or(int64_t(settings.ao)), 0, 2));
    }
    const auto default_quality = int64_t(settings.ao ? settings.ao : settings.ao_quality);
    settings.ao_method = uint32_t(std::clamp<int64_t>(table["ao_method"].value_or(int64_t(settings.ao_method)), 0, 1));
    settings.ao_quality = uint32_t(std::clamp<int64_t>(table["ao_quality"].value_or(default_quality), 1, 2));
    if (settings.ao) settings.ao = settings.ao_quality;
}

uint32_t MsaaSamplesFromTable(const toml::table& table, uint32_t fallback) {
    const auto msaa = table["msaa"];
    if (!msaa) return fallback;
    // A boolean is the legacy `msaa = true` for 4x, not the integer 1.
    if (msaa.is_boolean()) return msaa.value_or(false) ? 4u : 0u;
    const auto samples = msaa.value_or(int64_t(0));
    return samples == 2 || samples == 4 || samples == 8 ? uint32_t(samples) : 0u;
}

toml::table ReadSettingsTable(const fs::path& path) {
    if (!fs::exists(path)) return {};
    std::ifstream in;
    // A reader can catch the file while another process replaces it; retry a
    // sharing violation briefly instead of failing the load.
    for (int attempt = 0; attempt < 4 && !in.is_open(); ++attempt) {
        in.open(path, std::ios::binary);
        if (!in.is_open()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!in.is_open()) throw std::runtime_error("Could not read the settings file.");
    return toml::parse(in);
}

toml::table ChangedSettings(const toml::table& previous, const toml::table& current) {
    toml::table changes;
    for (const auto& [key, value] : current) {
        const auto* old = previous.get(key);
        if (!old) changes.insert(key, value);
        else if (old->is_table() && value.is_table()) {
            auto nested = ChangedSettings(*old->as_table(), *value.as_table());
            if (!nested.empty()) changes.insert(key, std::move(nested));
        } else if (!SameNode(*old, value)) {
            changes.insert(key, value);
        }
    }
    return changes;
}

void MergeSettingsTable(const fs::path& path, const toml::table& edits, const toml::table* full) {
    if (edits.empty()) return;
    const auto absolute = fs::absolute(path);
    fs::create_directories(absolute.parent_path());
    auto lock_path = fs::weakly_canonical(absolute.parent_path()) / absolute.filename();
    lock_path += ".lock";
    const PublicationLock lock(lock_path);
    toml::table table;
    try {
        table = ReadSettingsTable(absolute);
    } catch (const toml::parse_error&) {
        const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count();
        auto aside = absolute;
        aside += ".corrupt-" + std::to_string(stamp);
        std::error_code ec;
        fs::rename(absolute, aside, ec);
        if (ec) throw std::runtime_error("The settings file does not parse and could not be moved aside.");
        if (full) table = *full;
    }
    Merge(table, edits);
    if (edits.contains("controls"))
        for (const auto* old : {"reload_key", "heavy_attack_key", "use_key", "climb_key"}) table.erase(old);
    std::ostringstream text;
    text << table << '\n';
    Publish(absolute, text.str());
}

namespace {

// One publisher thread, jobs in order. The UI thread only builds the table and
// queues it; waiting for another process to release the settings lock there
// froze the frame and held UiLock.
struct SettingsPublisher {
    struct Job {
        fs::path path;
        toml::table edits;
        toml::table full;
    };
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> jobs;
    bool busy = false;
    bool stop = false;
    std::thread worker;

    SettingsPublisher() : worker([this] { Run(); }) {}
    ~SettingsPublisher() {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void Run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return stop || !jobs.empty(); });
                if (stop && jobs.empty()) return;
                job = std::move(jobs.front());
                jobs.pop_front();
                busy = true;
            }
            try {
                MergeSettingsTable(job.path, job.edits, &job.full);
            } catch (const std::exception& failure) {
                LOGE("Settings: %s", failure.what());
            }
            {
                std::lock_guard lock(mutex);
                busy = false;
            }
            cv.notify_all();
        }
    }

    void Push(Job job) {
        {
            std::lock_guard lock(mutex);
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
    }

    void Flush() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return jobs.empty() && !busy; });
    }
};

SettingsPublisher& Publisher() {
    static SettingsPublisher publisher;
    return publisher;
}

}  // namespace

void PublishSettingsAsync(fs::path path, toml::table edits, toml::table full) {
    if (edits.empty()) return;
    Publisher().Push({std::move(path), std::move(edits), std::move(full)});
}

void FlushSettings() { Publisher().Flush(); }

} // namespace app
