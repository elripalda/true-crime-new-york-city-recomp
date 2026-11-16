#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "updater.h"

int main(int argc, char** argv) {
    using launcher::Updater;
    using State = Updater::State;
    using namespace std::chrono_literals;
    const std::string_view argument = argc > 1 ? argv[1] : "";
    if (const auto handled = Updater::HandleStartup(argument)) return *handled;

    if (argument != "--exercise" && argument != "--stage") {
#ifdef _WIN32
        const auto* result = _wgetenv(L"TCNYC_FIXTURE_RESTARTED");
#else
        const auto* result = std::getenv("TCNYC_FIXTURE_RESTARTED");
#endif
        if (result) {
            std::ofstream out{std::filesystem::path(result)};
            out << TCNYC_FIXTURE_VERSION << '\n';
        }
        return 0;
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    Updater updater;
    updater.Check();
    while (updater.state() == State::Checking && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    if (updater.state() == State::Available) updater.Download();
    while ((updater.state() == State::Downloading || updater.state() == State::Staging) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    if (updater.state() != State::Ready) {
        std::cerr << updater.error() << '\n';
        return 1;
    }
    return argument == "--stage" || updater.Relaunch() ? 0 : 1;
}
