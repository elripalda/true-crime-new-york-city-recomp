// Include the implementation to hold the real logger lock while testing the crash path.
#include "core/log.cpp"

#include <fstream>
#include <iterator>
#include <string>

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "tcnyc-crash-log-test";
    fs::remove_all(dir);
    logx::Init(dir, "crash", false);

    // Another thread holds the logger for a moment: the crash line still reaches the file.
    std::atomic<bool> holding{false};
    std::thread other([&] {
        logx::Held lock;
        holding = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
    while (!holding) std::this_thread::yield();
    logx::EnterCrashMode();
    logx::Write(logx::Level::Error, "crash line from the faulting thread");
    other.join();

    // The faulting thread already inside the logger: no deadlock, stderr only.
    {
        logx::Held lock;
        logx::Write(logx::Level::Error, "fault inside the logger");
        logx::Flush();
    }

    std::ifstream in(logx::g_file_path);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    if (text.find("crash line from the faulting thread") == std::string::npos) return 1;
    if (text.find("fault inside the logger") != std::string::npos) return 2;
    return 0;
}
