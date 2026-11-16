#include <iostream>
#include <stdexcept>

#include "release_json.h"
#include "updater.h"

int main() {
    using launcher::OlderVersion;
    bool ok = OlderVersion("0.9.2-beta.9", "v0.9.2-beta.10") &&
              OlderVersion("0.9.2-beta.10", "0.9.2") &&
              !OlderVersion("0.9.2+build.1", "0.9.2+build.2") &&
              !OlderVersion("x1.2.3", "2.0.0") &&
              !OlderVersion("1.2.3", "999999999999999999999.2.3") &&
              !OlderVersion("1.2.3-beta.01", "1.2.3");
    const auto release = launcher::ReleaseJsonParser(R"([
      {"assets":[{"size":123,"uploader":{"name":"person"},"name":"example\u002ezip"}],
       "prerelease":true,"tag_name":"v1.2.3-beta.1"}
    ])").Parse();
    const auto& entry = release.elements.at(0);
    const auto& asset = entry["assets"].elements.at(0);
    ok = ok && entry["prerelease"].True() && asset["name"].String() == "example.zip" && asset["size"].Size() == 123;
    for (const auto invalid : {"{", "{\"duplicate\":0,\"duplicate\":1}", "[01]", "[1e]", "\"\\uD800\""}) {
        try {
            launcher::ReleaseJsonParser(invalid).Parse();
            ok = false;
        } catch (const std::runtime_error&) {
        }
    }
    if (!ok) std::cerr << "Release metadata checks failed.\n";
    return ok ? 0 : 1;
}
