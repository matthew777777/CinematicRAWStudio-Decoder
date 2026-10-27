// SPDX-License-Identifier: Apache-2.0
//
// craw_dec info   file.craw
// craw_dec verify file.craw            decode every frame, check CRCs, JSON timing
// craw_dec frames file.craw out_dir    write frame_NNNNNN.raw (uint16 LE)
#include <craw/Reader.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: craw_dec info|verify|frames file.craw [out_dir]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    try {
        craw::Reader r(argv[2]);
        if (cmd == "info") {
            std::printf("meta: %s\nframes: %zu%s\n", r.metaJson().c_str(), r.frameCount(),
                        r.recovered() ? " (recovered: no index)" : "");
            for (auto& c : r.chunks())
                std::printf("  %.4s @%llu size=%llu ts=%lld%s\n", c.fourcc.data(),
                            (unsigned long long)c.offset, (unsigned long long)c.size,
                            (long long)c.timestampNs, c.crcOk ? "" : " CRC-BAD");
            return 0;
        }
        std::vector<uint16_t> out;
        double totalMs = 0, worstMs = 0, mpix = 0;
        size_t bad = 0;
        for (size_t i = 0; i < r.frameCount(); ++i) {
            try {
                const auto rec = r.frame(i);
                const auto t0 = std::chrono::steady_clock::now();
                const auto info = craw::decodeFrame(rec.payload, rec.payloadSize, out, threads);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                totalMs += ms;
                worstMs = std::max(worstMs, ms);
                mpix += info.width * double(info.height) / 1e6;
                if (cmd == "frames") {
                    char name[64];
                    std::snprintf(name, sizeof name, "/frame_%06zu.raw", i);
                    std::FILE* f = std::fopen((std::string(argv[3]) + name).c_str(), "wb");
                    if (!f || std::fwrite(out.data(), 2, out.size(), f) != out.size()) return 1;
                    std::fclose(f);
                }
            } catch (const craw::DecodeError& e) {
                std::fprintf(stderr, "frame %zu: %s\n", i, e.what());
                ++bad;
            }
        }
        const size_t n = r.frameCount();
        std::printf("{\"tool\":\"craw_dec\",\"frames\":%zu,\"bad_frames\":%zu,\"recovered\":%s,"
                    "\"mean_ms\":%.3f,\"worst_ms\":%.3f,\"mpix_per_s\":%.1f}\n",
                    n, bad, r.recovered() ? "true" : "false", n ? totalMs / n : 0.0, worstMs,
                    totalMs > 0 ? mpix / (totalMs / 1e3) : 0.0);
        return bad ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "craw_dec: %s\n", e.what());
        return 1;
    }
}
