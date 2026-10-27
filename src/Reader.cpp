// SPDX-License-Identifier: Apache-2.0
#include <craw/Reader.h>

#include "Crc32c.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace craw {
namespace {

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32; }

constexpr size_t kChunkHeader = 16, kChunkTail = 4, kFramHeader = 20;

}  // namespace

Reader::Reader(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw DecodeError("craw: cannot open " + path);
    data_.assign(std::istreambuf_iterator<char>(f), {});
    const uint8_t* d = data_.data();
    const size_t n = data_.size();
    if (n < 16 || std::memcmp(d, "CRAW", 4) != 0) throw DecodeError("craw: not a CRAW file");
    if ((d[4] | d[5] << 8) != 0 || (d[6] | d[7] << 8) < 2) throw DecodeError("craw: unsupported major version");

    // Scan every chunk: cheap (headers only) and works for crashed files too.
    bool sawIndex = false;
    for (size_t pos = 16; pos + kChunkHeader + kChunkTail <= n;) {
        if (std::memcmp(d + pos, "CRAWEND", 7) == 0) break;
        const uint64_t size = rd64(d + pos + 8);
        if (size > n - pos - kChunkHeader - kChunkTail) break;  // truncated
        ChunkRef c{};
        std::memcpy(c.fourcc.data(), d + pos, 4);
        c.offset = pos;
        c.size = size;
        const uint8_t* body = d + pos + kChunkHeader;
        c.crcOk = detail::crc32c(body, size) == rd32(body + size);
        if (std::memcmp(d + pos, "FRAM", 4) == 0 && size >= kFramHeader)
            c.timestampNs = static_cast<int64_t>(rd64(body + 8));
        else if ((std::memcmp(d + pos, "AUDI", 4) == 0 || std::memcmp(d + pos, "LSHD", 4) == 0) && size >= 8)
            c.timestampNs = static_cast<int64_t>(rd64(body));
        pos += kChunkHeader + size + kChunkTail;
        if (std::memcmp(c.fourcc.data(), "INDX", 4) == 0) { sawIndex = true; break; }
        if (std::memcmp(c.fourcc.data(), "META", 4) == 0 && meta_.empty()) {
            meta_.assign(reinterpret_cast<const char*>(body), size);
            continue;
        }
        if (std::memcmp(c.fourcc.data(), "FRAM", 4) == 0) frames_.push_back(chunks_.size());
        chunks_.push_back(c);
    }
    recovered_ = !sawIndex;
}

std::string_view Reader::payload(const ChunkRef& c) const {
    return {reinterpret_cast<const char*>(data_.data() + c.offset + kChunkHeader), c.size};
}

FrameRecord Reader::frame(size_t i) const {
    const ChunkRef& c = chunks_.at(frames_.at(i));
    if (!c.crcOk) throw DecodeError("craw: FRAM chunk CRC mismatch");
    if (c.size < kFramHeader) throw DecodeError("craw: FRAM too small");
    const uint8_t* b = data_.data() + c.offset + kChunkHeader;
    const uint32_t metaLen = rd32(b + 16);
    if (metaLen > c.size - kFramHeader) throw DecodeError("craw: bad FRAM meta length");
    return {rd64(b), static_cast<int64_t>(rd64(b + 8)),
            std::string(reinterpret_cast<const char*>(b + kFramHeader), metaLen),
            b + kFramHeader + metaLen, c.size - kFramHeader - metaLen};
}

}  // namespace craw
