#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
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
#if TCNYC_DEV_TOOLS
    skip = _wgetenv(L"XBE_DEV_SKIP_LAUNCHER") != nullptr;
#endif
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--skip-launcher") == 0 || std::wcscmp(argv[i], L"--relaunched") == 0) skip = true;
    }
    if (argv) LocalFree(argv);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::filesystem::path location;
    if (!skip && !launcher::Run(location)) return 0;
    app::SetStartupGameLocation(std::move(location));
    return RecompMain(instance, previous, args, show);
}
