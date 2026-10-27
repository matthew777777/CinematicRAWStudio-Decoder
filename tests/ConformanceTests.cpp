// SPDX-License-Identifier: Apache-2.0
//
// Decodes every golden vector from CinematicRAWStudio-Spec/vectors and checks
// the samples match; then flips bytes to check corrupt input is rejected
// cleanly (never crashes, never returns a wrong frame silently).
#include <craw/Decoder.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::vector<uint8_t> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char** argv) {
    const fs::path dir = argc > 1 ? argv[1] : CRAW_VECTORS_DIR;
    const auto mb = slurp(dir / "manifest.json");
    const std::string manifest(mb.begin(), mb.end());
    std::regex nameRe(R"re("name": "([^"]+)")re");
    int failures = 0, cases = 0;
    for (auto it = std::sregex_iterator(manifest.begin(), manifest.end(), nameRe);
         it != std::sregex_iterator(); ++it, ++cases) {
        const std::string name = (*it)[1];
        const auto payload = slurp(dir / (name + ".payload"));
        const auto rawBytes = slurp(dir / (name + ".raw"));
        std::vector<uint16_t> expect(rawBytes.size() / 2), out;
        for (size_t i = 0; i < expect.size(); ++i) expect[i] = rawBytes[2 * i] | rawBytes[2 * i + 1] << 8;
        bool ok = true;
        for (unsigned threads : {1u, 4u}) {
            try {
                craw::decodeFrame(payload.data(), payload.size(), out, threads);
                ok &= out == expect;
            } catch (const std::exception& e) {
                std::printf("  %s\n", e.what());
                ok = false;
            }
        }
        // Every single-byte corruption must throw (CRC or structural check).
        size_t silent = 0;
        for (size_t i = 0; i < payload.size(); i += std::max<size_t>(1, payload.size() / 512)) {
            auto bad = payload;
            bad[i] ^= 0x5A;
            try {
                craw::decodeFrame(bad.data(), bad.size(), out, 1);
                if (out != expect) ++silent;
            } catch (const craw::DecodeError&) {
            }
        }
        // Truncations must throw.
        for (size_t cut : {size_t{0}, size_t{10}, payload.size() / 2, payload.size() - 1}) {
            try {
                craw::decodeFrame(payload.data(), cut, out, 1);
                ++silent;
            } catch (const craw::DecodeError&) {
            }
        }
        ok &= silent == 0;
        std::printf("%-6s %s\n", ok ? "PASS" : "FAIL", name.c_str());
        failures += !ok;
    }
    std::printf("%d vectors, %d failures\n", cases, failures);
    return failures || cases == 0;
}
