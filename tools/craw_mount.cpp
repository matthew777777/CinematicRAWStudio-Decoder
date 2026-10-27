// SPDX-License-Identifier: Apache-2.0
//
// craw_mount MOUNTPOINT clip1.craw [clip2.craw ...] [--flat] [-f] [fuse options]
//
// --flat (single take): the DNGs and WAV sit directly in MOUNTPOINT instead of
// MOUNTPOINT/<name>/.
//
// Read-only virtual filesystem that presents every .CRAW take as a CinemaDNG
// folder, so DaVinci Resolve (or any CinemaDNG reader) opens it directly:
//   MOUNTPOINT/<name>/<name>_000000.dng ... <name>.wav
// DNGs are decoded on demand (lossless). All frames of a clip have the same
// file size: pixel data starts at a fixed padded offset, so directory
// listings need no decoding. Unmount with `umount MOUNTPOINT`.
#include "common/Clip.h"

#include <fuse.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <ctime>

// libfuse / fuse-t use struct stat and off_t; WinFsp's FUSE layer has its own types.
#if defined(_WIN32)
using craw_stat_t = struct fuse_stat;
using craw_off_t = fuse_off_t;
#ifndef O_ACCMODE
#define O_ACCMODE 3
#endif
#define CRAW_SET_MTIME(st, t) ((st)->st_mtim.tv_sec = (t))
#else
using craw_stat_t = struct stat;
using craw_off_t = off_t;
#define CRAW_SET_MTIME(st, t) ((st)->st_mtime = (t))
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#include <deque>
#include <atomic>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct MountedClip {
    std::string name;
    time_t mtime = 0;
    std::unique_ptr<craw::tools::Clip> clip;
    uint32_t pad = 0;
    uint64_t dngSize = 0;
    // Last frame read, for playback prefetch (heap-held: atomics don't move).
    struct Playback { std::atomic<long> lastFrame{-2}; };
    std::unique_ptr<Playback> play = std::make_unique<Playback>();
};

struct CachedFrame {
    const MountedClip* clip;
    size_t index;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
};

std::vector<MountedClip> gClips;
bool gFlat = false;
std::mutex gCacheMutex;
std::list<CachedFrame> gCache;  // most recent first
// Cache by memory, not frame count: scrubbing revisits frames, and a
// back-and-forth scrub should hit decoded DNGs. --cache-mb overrides.
uint64_t gCacheBytes = 2048ull << 20;
uint64_t gCachedBytes = 0;
constexpr size_t kPrefetch = 6;  // frames decoded ahead of sequential playback
std::condition_variable gCacheReady;
std::vector<std::pair<const MountedClip*, size_t>> gInFlight;
// Decode on the performance cores only: on Apple silicon, spreading one
// frame over the efficiency cores too is slower (threads wait for the
// slowest band).
unsigned performanceCores() {
#if defined(__APPLE__)
    int n = 0;
    size_t len = sizeof n;
    if (sysctlbyname("hw.perflevel0.physicalcpu", &n, &len, nullptr, 0) == 0 && n > 0) return static_cast<unsigned>(n);
#endif
    return std::max(1u, std::thread::hardware_concurrency());
}
const unsigned kThreads = performanceCores();
// Prefetch: a small fixed pool takes jobs from a queue that each new
// playback position replaces, so stale frames are never decoded and an
// on-demand decode (Resolve waiting) keeps most of the cores.
constexpr unsigned kPrefetchWorkers = 2;
std::mutex gQueueMutex;
std::condition_variable gQueueReady;
std::deque<std::pair<const MountedClip*, size_t>> gQueue;

struct Node {
    const MountedClip* clip = nullptr;
    enum { None, Root, Dir, Dng, Wav } kind = None;
    size_t frame = 0;
};

Node resolve(const char* path) {
    Node n;
    std::string p = path;
    if (gFlat) {
        // One take at the root: "/<name>/..." becomes "/...".
        if (p == "/") { n.clip = &gClips.front(); n.kind = Node::Dir; return n; }
        p = "/" + gClips.front().name + p;
    }
    if (p == "/") { n.kind = Node::Root; return n; }
    if (p.front() == '/') p.erase(0, 1);
    const auto slash = p.find('/');
    const std::string dir = p.substr(0, slash);
    for (const auto& c : gClips) {
        if (c.name != dir) continue;
        n.clip = &c;
        if (slash == std::string::npos) { n.kind = Node::Dir; return n; }
        const std::string file = p.substr(slash + 1);
        if (file == c.name + ".wav" && c.clip->audio().format != 0) { n.kind = Node::Wav; return n; }
        const std::string prefix = c.name + "_";
        if (file.size() == prefix.size() + 10 && file.compare(0, prefix.size(), prefix) == 0 &&
            file.compare(file.size() - 4, 4, ".dng") == 0) {
            const std::string digits = file.substr(prefix.size(), 6);
            if (std::all_of(digits.begin(), digits.end(), ::isdigit)) {
                n.frame = std::stoul(digits);
                if (n.frame < c.clip->frameCount()) n.kind = Node::Dng;
            }
        }
        return n;
    }
    return n;
}

// Cached DNG for frame i; decodes on miss (waiting for an in-flight prefetch
// of the same frame instead of decoding twice).
std::shared_ptr<const std::vector<uint8_t>> frameBytes(const MountedClip& c, size_t i, unsigned threads = kThreads) {
    {
        std::unique_lock lock(gCacheMutex);
        for (;;) {
            for (auto it = gCache.begin(); it != gCache.end(); ++it)
                if (it->clip == &c && it->index == i) {
                    gCache.splice(gCache.begin(), gCache, it);
                    return gCache.front().bytes;
                }
            const auto key = std::make_pair(&c, i);
            if (std::find(gInFlight.begin(), gInFlight.end(), key) == gInFlight.end()) {
                gInFlight.push_back(key);
                break;
            }
            gCacheReady.wait(lock);
        }
    }
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    try {
        bytes = std::make_shared<const std::vector<uint8_t>>(c.clip->frameDng(i, c.pad, threads));
    } catch (...) {
        std::lock_guard lock(gCacheMutex);
        gInFlight.erase(std::find(gInFlight.begin(), gInFlight.end(), std::make_pair(&c, i)));
        gCacheReady.notify_all();
        throw;
    }
    std::lock_guard lock(gCacheMutex);
    gInFlight.erase(std::find(gInFlight.begin(), gInFlight.end(), std::make_pair(&c, i)));
    gCache.push_front({&c, i, bytes});
    gCachedBytes += bytes->size();
    while (gCache.size() > 1 && gCachedBytes > gCacheBytes) {
        gCachedBytes -= gCache.back().bytes->size();
        gCache.pop_back();
    }
    gCacheReady.notify_all();
    return bytes;
}

bool cachedOrInFlight(const MountedClip& c, size_t f) {
    std::lock_guard lock(gCacheMutex);
    const auto key = std::make_pair(&c, f);
    return std::find(gInFlight.begin(), gInFlight.end(), key) != gInFlight.end() ||
           std::any_of(gCache.begin(), gCache.end(), [&](const CachedFrame& e) { return e.clip == &c && e.index == f; });
}

void prefetchWorker() {
    // Prefetch decodes share the cores: on-demand reads must not wait behind them.
    const unsigned threads = std::max(1u, kThreads / kPrefetchWorkers);
    for (;;) {
        std::pair<const MountedClip*, size_t> job;
        {
            std::unique_lock lock(gQueueMutex);
            gQueueReady.wait(lock, [] { return !gQueue.empty(); });
            job = gQueue.front();
            gQueue.pop_front();
        }
        if (cachedOrInFlight(*job.first, job.second)) continue;
        try { frameBytes(*job.first, job.second, threads); } catch (...) {}
    }
}

// Readers such as Resolve walk a sequence in order (forwards when playing,
// backwards in reverse shuttle): decode the next frames in that direction.
// A jump (scrub) prefetches nothing, so the requested frame gets the CPU.
void notePlayback(const MountedClip& c, size_t i) {
    const long cur = static_cast<long>(i), last = c.play->lastFrame.exchange(cur);
    if (cur == last) return;  // further reads of the same frame
    const long step = cur - last;
    if (step != 1 && step != -1) {
        std::lock_guard lock(gQueueMutex);
        gQueue.clear();  // scrubbing: drop the stale plan
        return;
    }
    std::vector<std::pair<const MountedClip*, size_t>> plan;
    for (long k = 1; k <= static_cast<long>(kPrefetch); ++k) {
        const long f = cur + k * step;
        if (f < 0 || f >= static_cast<long>(c.clip->frameCount())) break;
        plan.emplace_back(&c, static_cast<size_t>(f));
    }
    {
        std::lock_guard lock(gQueueMutex);
        gQueue.assign(plan.begin(), plan.end());
    }
    gQueueReady.notify_all();
}

int doGetattr(const char* path, craw_stat_t* st
#if FUSE_USE_VERSION >= 30
              , struct fuse_file_info*
#endif
) {
    std::memset(st, 0, sizeof *st);
    const Node n = resolve(path);
    switch (n.kind) {
        case Node::Root:
        case Node::Dir:
            if (n.clip) CRAW_SET_MTIME(st, n.clip->mtime);
            st->st_mode = S_IFDIR | 0555;
            st->st_nlink = 2;
            return 0;
        case Node::Dng:
            st->st_mode = S_IFREG | 0444;
            st->st_nlink = 1;
            st->st_size = static_cast<craw_off_t>(n.clip->dngSize);
            CRAW_SET_MTIME(st, n.clip->mtime);
            return 0;
        case Node::Wav:
            st->st_mode = S_IFREG | 0444;
            st->st_nlink = 1;
            st->st_size = static_cast<craw_off_t>(n.clip->clip->wavSize());
            CRAW_SET_MTIME(st, n.clip->mtime);
            return 0;
        default:
            return -ENOENT;
    }
}

int doReaddir(const char* path, void* buf, fuse_fill_dir_t filler, craw_off_t, struct fuse_file_info*
#if FUSE_USE_VERSION >= 30
              , enum fuse_readdir_flags
#endif
) {
    auto fill = [&](void* buffer, const char* name, const craw_stat_t* st, craw_off_t offset) {
#if FUSE_USE_VERSION >= 30
        return filler(buffer, name, st, offset, static_cast<fuse_fill_dir_flags>(0));
#else
        return filler(buffer, name, st, offset);
#endif
    };
    const Node n = resolve(path);
    if (n.kind != Node::Root && n.kind != Node::Dir) return -ENOENT;
    fill(buf, ".", nullptr, 0);
    fill(buf, "..", nullptr, 0);
    if (n.kind == Node::Root) {
        for (const auto& c : gClips) fill(buf, c.name.c_str(), nullptr, 0);
        return 0;
    }
    char file[512];
    for (size_t i = 0; i < n.clip->clip->frameCount(); ++i) {
        std::snprintf(file, sizeof file, "%s_%06zu.dng", n.clip->name.c_str(), i);
        fill(buf, file, nullptr, 0);
    }
    if (n.clip->clip->audio().format != 0) fill(buf, (n.clip->name + ".wav").c_str(), nullptr, 0);
    return 0;
}

int doOpen(const char* path, struct fuse_file_info* fi) {
    const Node n = resolve(path);
    if (n.kind != Node::Dng && n.kind != Node::Wav) return -ENOENT;
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;
    return 0;
}

int doRead(const char* path, char* out, size_t size, craw_off_t offset, struct fuse_file_info*) {
    const Node n = resolve(path);
    if (offset < 0) return -EINVAL;
    if (n.kind == Node::Wav) {
        const uint64_t total = n.clip->clip->wavSize();
        if (static_cast<uint64_t>(offset) >= total) return 0;
        const size_t len = static_cast<size_t>(std::min<uint64_t>(size, total - offset));
        return static_cast<int>(n.clip->clip->readWav(offset, reinterpret_cast<uint8_t*>(out), len));
    }
    if (n.kind != Node::Dng) return -ENOENT;
    try {
        notePlayback(*n.clip, n.frame);
        const auto bytes = frameBytes(*n.clip, n.frame);
        if (static_cast<uint64_t>(offset) >= bytes->size()) return 0;
        const size_t len = std::min(size, bytes->size() - static_cast<size_t>(offset));
        std::memcpy(out, bytes->data() + offset, len);
        return static_cast<int>(len);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "craw_mount: %s frame %zu: %s\n", n.clip->name.c_str(), n.frame, e.what());
        return -EIO;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: craw_mount MOUNTPOINT clip.craw [clip.craw ...] [--cache-mb 2048] [-f] [fuse options]\n");
        return 2;
    }
    std::vector<char*> fuseArgs{argv[0], argv[1]};
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--flat") { gFlat = true; continue; }
        if (a == "--cache-mb" && i + 1 < argc) { gCacheBytes = std::stoull(argv[++i]) << 20; continue; }
        if (!a.empty() && a[0] == '-') { fuseArgs.push_back(argv[i]); continue; }
        try {
            MountedClip c;
            c.name = fs::path(a).stem().string();
            c.clip = std::make_unique<craw::tools::Clip>(a);
            c.clip->setPackedDng(true);  // fewer bytes through FUSE
            struct stat src{};
            if (::stat(a.c_str(), &src) == 0) c.mtime = src.st_mtime;
            if (c.clip->frameCount() == 0) { std::fprintf(stderr, "skip %s: no frames\n", a.c_str()); continue; }
            // Fixed header room: first frame's header plus 1 MiB for metadata that
            // varies per frame (e.g. a larger lens-shading GainMap).
            const auto [w, h] = c.clip->imageSize();
            const auto probe = c.clip->frameDng(0, 0, kThreads);
            const uint64_t header = probe.size() - c.clip->dngPixelBytes();
            c.pad = static_cast<uint32_t>((header + (1u << 20) + 4095) / 4096 * 4096);
            c.dngSize = c.clip->paddedDngSize(c.pad);
            std::printf("%s: %zu frames %ux%u -> %s/%s/\n", a.c_str(), c.clip->frameCount(), w, h, argv[1],
                        c.name.c_str());
            gClips.push_back(std::move(c));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "skip %s: %s\n", a.c_str(), e.what());
        }
    }
    if (gClips.empty()) return 1;
    if (gFlat && gClips.size() != 1) {
        std::fprintf(stderr, "--flat needs exactly one take\n");
        return 2;
    }
    std::string volname = "-ovolname=" + (gFlat ? gClips.front().name : std::string("CRAW"));
    fuseArgs.push_back(const_cast<char*>("-o"));
    fuseArgs.push_back(const_cast<char*>("ro"));
#if defined(__APPLE__) || defined(_WIN32)
    fuseArgs.push_back(volname.data());
#endif
    for (unsigned k = 0; k < kPrefetchWorkers; ++k) std::thread(prefetchWorker).detach();
    fuse_operations ops{};
    ops.getattr = doGetattr;
    ops.readdir = doReaddir;
    ops.open = doOpen;
    ops.read = doRead;
    return fuse_main(static_cast<int>(fuseArgs.size()), fuseArgs.data(), &ops, nullptr);
}
