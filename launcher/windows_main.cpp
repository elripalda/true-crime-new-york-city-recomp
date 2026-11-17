#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include "core/dev_env.h"
#include <shellapi.h>

#include <cstdlib>
#include <cwchar>
#include <utility>

#include "launcher.h"
#include "updater.h"

namespace app {
void SetStartupGameLocation(std::filesystem::path location);
}

extern "C" int WINAPI RecompMain(HINSTANCE, HINSTANCE, PWSTR, int);

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string update_argument;
    if (argc > 1) {
        const std::wstring_view first(argv[1]);
        if (first.starts_with(L"--apply-update=") || first.starts_with(L"--recover-update="))
            update_argument.assign(first.begin(), first.end());
    }
    if (const auto handled = launcher::Updater::HandleStartup(update_argument)) {
        if (argv) LocalFree(argv);
        return *handled;
    }
    bool skip = false;
    app::GameSource source;
#if TCNYC_DEV_TOOLS
    skip = DevWEnv(L"XBE_DEV_SKIP_LAUNCHER") != nullptr;
#endif
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--game-source") == 0 && i + 1 < argc) source.path = argv[++i];
        else if (std::wcscmp(argv[i], L"--skip-launcher") == 0 || std::wcscmp(argv[i], L"--relaunched") == 0) skip = true;
    }
    if (argv) LocalFree(argv);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!skip && source.path.empty() && !launcher::Run(source)) return 0;
#ifndef TCNYC_PAL
    if (source.region == app::GameRegion::Unknown) {
        const auto detected = app::DetectGameSource(source.path);
        if (detected.region != app::GameRegion::Unknown) source = detected;
    }
    if (app::IsPal(source.region)) return launcher::LaunchPal(source) ? 0 : 1;
#endif
    const auto game_files = launcher::Updater::HoldGameFiles();
    if (!game_files) return 1;
    app::SetStartupGameLocation(std::move(source.path));
    return RecompMain(instance, previous, args, show);
}
