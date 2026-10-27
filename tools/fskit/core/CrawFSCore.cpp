// SPDX-License-Identifier: Apache-2.0
#include "CrawFSCore.h"

#include "../../common/Clip.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct CrawFsClip {
    std::unique_ptr<craw::tools::Clip> clip;
    uint32_t pad = 0;
    uint64_t dngSize = 0;
    int64_t mtime = 0;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());

    // LRU of built DNGs; `inFlight` frames are being built by some thread.
    std::mutex mutex;
    std::condition_variable changed;
    std::list<std::pair<uint64_t, std::shared_ptr<const std::vector<uint8_t>>>> cache;
    std::vector<uint64_t> inFlight;
    std::deque<uint64_t> prefetchQueue;
    bool stopping = false;
    std::thread prefetcher;

    static constexpr size_t kCacheFrames = 12;
    static constexpr uint64_t kPrefetch = 4;

    std::shared_ptr<const std::vector<uint8_t>> lookupLocked(uint64_t f) {
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->first == f) {
                cache.splice(cache.begin(), cache, it);
                return cache.front().second;
            }
        return nullptr;
    }

    // Cached DNG for frame `f`, building it (or waiting for the thread that is).
    std::shared_ptr<const std::vector<uint8_t>> frame(uint64_t f) {
        {
            std::unique_lock lock(mutex);
            for (;;) {
                if (auto hit = lookupLocked(f)) return hit;
                if (std::find(inFlight.begin(), inFlight.end(), f) == inFlight.end()) break;
                changed.wait(lock);
            }
            inFlight.push_back(f);
        }
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        try {
            bytes = std::make_shared<const std::vector<uint8_t>>(clip->frameDng(f, pad, threads));
        } catch (...) {
            std::lock_guard lock(mutex);
            inFlight.erase(std::find(inFlight.begin(), inFlight.end(), f));
            changed.notify_all();
            throw;
        }
        std::lock_guard lock(mutex);
        inFlight.erase(std::find(inFlight.begin(), inFlight.end(), f));
        cache.emplace_front(f, bytes);
        if (cache.size() > kCacheFrames) cache.pop_back();
        changed.notify_all();
        return bytes;
    }

    void schedulePrefetch(uint64_t after) {
        std::lock_guard lock(mutex);
        prefetchQueue.clear();  // playback moved: only the newest window matters
        for (uint64_t k = 1; k <= kPrefetch && after + k < clip->frameCount(); ++k) prefetchQueue.push_back(after + k);
        changed.notify_all();
    }

    void prefetchLoop() {
        for (;;) {
            uint64_t f;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stopping || !prefetchQueue.empty(); });
                if (stopping) return;
                f = prefetchQueue.front();
                prefetchQueue.pop_front();
                if (lookupLocked(f) || std::find(inFlight.begin(), inFlight.end(), f) != inFlight.end()) continue;
            }
            try { frame(f); } catch (...) {}
        }
    }
};

extern "C" {

CrawFsClip* crawfs_open(const char* path, char* err, size_t errLen) {
    try {
        auto c = std::make_unique<CrawFsClip>();
        c->clip = std::make_unique<craw::tools::Clip>(path);
        c->clip->setPackedDng(true);  // fewer bytes through the filesystem
        if (c->clip->frameCount() == 0) throw std::runtime_error("take has no frames");
        const auto [w, h] = c->clip->imageSize();
        const auto probe = c->clip->frameDng(0, 0, c->threads);
        const uint64_t header = probe.size() - c->clip->dngPixelBytes();
        // Fixed header room: first frame's header + 1 MiB for metadata that varies per frame.
        c->pad = static_cast<uint32_t>((header + (1u << 20) + 4095) / 4096 * 4096);
        c->dngSize = c->clip->paddedDngSize(c->pad);
        struct stat st{};
        if (::stat(path, &st) == 0) c->mtime = st.st_mtime;
        c->prefetcher = std::thread([p = c.get()] { p->prefetchLoop(); });
        return c.release();
    } catch (const std::exception& e) {
        if (err && errLen) std::snprintf(err, errLen, "%s", e.what());
        return nullptr;
    }
}

void crawfs_close(CrawFsClip* c) {
    if (!c) return;
    {
        std::lock_guard lock(c->mutex);
        c->stopping = true;
        c->changed.notify_all();
    }
    if (c->prefetcher.joinable()) c->prefetcher.join();
    delete c;
}

uint64_t crawfs_frame_count(const CrawFsClip* c) { return c ? c->clip->frameCount() : 0; }
uint64_t crawfs_dng_size(const CrawFsClip* c) { return c ? c->dngSize : 0; }
int64_t crawfs_mtime(const CrawFsClip* c) { return c ? c->mtime : 0; }
uint64_t crawfs_wav_size(const CrawFsClip* c) { return c ? c->clip->wavSize() : 0; }

int64_t crawfs_read_dng(CrawFsClip* c, uint64_t f, uint64_t offset, void* dst, uint64_t len) {
    if (!c || !dst) return -EINVAL;
    if (f >= c->clip->frameCount()) return -ENOENT;
    if (offset >= c->dngSize) return 0;
    try {
        const auto bytes = c->frame(f);
        if (offset == 0) c->schedulePrefetch(f);
        const uint64_t n = std::min<uint64_t>(len, bytes->size() - offset);
        std::memcpy(dst, bytes->data() + offset, n);
        return static_cast<int64_t>(n);
    } catch (...) {
        return -EIO;
    }
}

int64_t crawfs_read_wav(CrawFsClip* c, uint64_t offset, void* dst, uint64_t len) {
    if (!c || !dst) return -EINVAL;
    const uint64_t total = c->clip->wavSize();
    if (offset >= total) return 0;
    return static_cast<int64_t>(c->clip->readWav(offset, static_cast<uint8_t*>(dst), std::min<uint64_t>(len, total - offset)));
}

}  // extern "C"
