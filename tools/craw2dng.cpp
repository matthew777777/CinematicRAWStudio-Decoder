// SPDX-License-Identifier: Apache-2.0
//
// craw2dng in.craw out_dir [--frames FIRST-LAST] [--no-audio] [--threads N]
//
// Writes a CinemaDNG sequence DaVinci Resolve imports as a clip:
//   out_dir/<name>/<name>_000000.dng ... and out_dir/<name>/<name>.wav
// Lossless: DNG pixels are the decoded CFA samples, metadata from META and
// each frame's JSON (lens shading becomes a DNG GainMap opcode).
#include "common/Clip.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: craw2dng in.craw out_dir [--frames FIRST-LAST] [--no-audio] [--threads N]\n");
        return 2;
    }
    const fs::path in = argv[1];
    size_t first = 0, last = SIZE_MAX;
    bool audio = true;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-audio") audio = false;
        else if (a == "--frames" && i + 1 < argc) {
            const std::string r = argv[++i];
            const auto dash = r.find('-');
            first = std::stoul(r.substr(0, dash));
            last = dash == std::string::npos ? first : std::stoul(r.substr(dash + 1));
        } else if (a == "--threads" && i + 1 < argc) threads = std::stoul(argv[++i]);
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    try {
        const craw::tools::Clip clip(in.string());
        const std::string name = in.stem().string();
        const fs::path dir = fs::path(argv[2]) / name;
        fs::create_directories(dir);
        last = std::min(last, clip.frameCount() ? clip.frameCount() - 1 : 0);
        const auto t0 = std::chrono::steady_clock::now();
        size_t written = 0, failed = 0;
        for (size_t i = first; clip.frameCount() && i <= last; ++i) {
            try {
                const auto dng = clip.frameDng(i, 0, threads);
                char file[32];
                std::snprintf(file, sizeof file, "_%06zu.dng", i);
                std::FILE* f = std::fopen((dir / (name + file)).string().c_str(), "wb");
                if (!f || std::fwrite(dng.data(), 1, dng.size(), f) != dng.size()) {
                    std::fprintf(stderr, "write failed: frame %zu\n", i);
                    if (f) std::fclose(f);
                    return 1;
                }
                std::fclose(f);
                ++written;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "frame %zu skipped: %s\n", i, e.what());
                ++failed;
            }
        }
        if (audio && clip.audio().format != 0) {
            std::vector<uint8_t> wav(clip.wavSize());
            clip.readWav(0, wav.data(), wav.size());
            std::FILE* f = std::fopen((dir / (name + ".wav")).string().c_str(), "wb");
            if (!f || std::fwrite(wav.data(), 1, wav.size(), f) != wav.size()) return 1;
            std::fclose(f);
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("{\"tool\":\"craw2dng\",\"out\":\"%s\",\"frames\":%zu,\"failed\":%zu,\"audio\":%s,\"seconds\":%.2f}\n",
                    dir.string().c_str(), written, failed,
                    audio && clip.audio().format ? "true" : "false", s);
        return failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "craw2dng: %s\n", e.what());
        return 1;
    }
}
