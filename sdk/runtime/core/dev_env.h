#pragma once

#include <cstdlib>

// Developer diagnostics, traces and tuning overrides read their environment
// switches only in TCNYC_DEV_TOOLS builds. Release builds behave as if the
// variable were unset. Player-facing switches documented in README.md and
// README-LINUX.md keep using std::getenv directly.
inline const char* DevEnv(const char* name) {
#if defined(TCNYC_DEV_TOOLS) && TCNYC_DEV_TOOLS
    return std::getenv(name);
#else
    (void)name;
    return nullptr;
#endif
}

#ifdef _WIN32
inline const wchar_t* DevWEnv(const wchar_t* name) {
#if defined(TCNYC_DEV_TOOLS) && TCNYC_DEV_TOOLS
    return _wgetenv(name);
#else
    (void)name;
    return nullptr;
#endif
}
#endif
