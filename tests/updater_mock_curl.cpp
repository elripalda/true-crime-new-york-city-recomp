#include <cstdlib>
#include <filesystem>
#include <string_view>

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    const wchar_t* output = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"-o") {
            output = argv[i + 1];
            break;
        }
    }
    if (!output) return 2;
    const std::wstring_view url(argv[argc - 1]);
    const wchar_t* input = _wgetenv(url.find(L"api.github.com") != std::wstring_view::npos
                                   ? L"TCNYC_FIXTURE_RELEASE"
                                   : url.ends_with(L".sha256") ? L"TCNYC_FIXTURE_CHECKSUM" : L"TCNYC_FIXTURE_ARCHIVE");
    if (!input) return 2;
    std::error_code ec;
    return std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing, ec) ? 0 : 2;
}
