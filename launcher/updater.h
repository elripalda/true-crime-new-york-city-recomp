#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace launcher {

// Finds, downloads and installs a newer GitHub release beside the running executable.
// Uses the system's curl and tar, then applies the verified package after the launcher exits.
class Updater {
public:
    enum class State { Idle, Checking, Available, Downloading, Staging, Ready, Failed };

    ~Updater();
    void Check();     // background; quietly back to Idle when offline or already current
    // Makes an already-staged release visible even when update checks are off.
    bool AdoptStaged();
    void Download();  // background: download, verify and stage the available release
    void Cancel();
    // Starts the staged installer. The caller then exits so the game files can be replaced.
    bool Relaunch();
    // Runs the staged installer or recovers a prior update before game startup.
    static std::optional<int> HandleStartup(std::string_view argument);
    // Keep the installed files stable until the game has exited.
    static std::shared_ptr<void> HoldGameFiles();

    State state() const { return state_.load(); }
    float progress() const { return progress_.load(); }  // 0 to 1 while downloading
    std::string latest() const;                          // e.g. "0.9.3-beta.4"
    std::string error() const;
    static const char *Current();  // this build's version
    static constexpr char kReleasesPage[] = "https://github.com/roz1077/true-crime-new-york-city-recomp/releases";

private:
    void Run(void (Updater::*step)());
    bool UseStagedRelease(const std::filesystem::path& root);
    void DoCheck();
    void DoDownload();
    void Fail(std::string message);

    std::atomic<State> state_{State::Idle};
    std::atomic<float> progress_{0};
    std::atomic<bool> cancel_{false};
    mutable std::mutex mutex_;
    std::string latest_, error_, archive_url_, checksum_url_;
    uint64_t size_ = 0;
    std::thread thread_;
};

// true when version a is older than b ("0.9.2-beta.3" < "0.9.2-beta.4" < "0.9.2" < "0.9.3-beta.1")
bool OlderVersion(const std::string &a, const std::string &b);

} // namespace launcher
