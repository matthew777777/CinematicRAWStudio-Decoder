// SPDX-License-Identifier: Apache-2.0
#include "common/Clip.h"
#include "../src/Crc32c.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

using Bytes = std::vector<uint8_t>;
static void check(bool ok) { if (!ok) throw std::runtime_error("WAV regression"); }
static void le(Bytes& b, uint64_t v, int n) { for (int i=0;i<n;++i) b.push_back(uint8_t(v >> (8*i))); }
static uint32_t u32(const Bytes& b, size_t p) { return b[p] | uint32_t(b[p+1])<<8 | uint32_t(b[p+2])<<16 | uint32_t(b[p+3])<<24; }
static void chunk(Bytes& b, const char* tag, const Bytes& payload) {
    b.insert(b.end(), tag, tag+4); le(b,0,4); le(b,payload.size(),8);
    b.insert(b.end(),payload.begin(),payload.end()); le(b,craw::detail::crc32c(payload.data(),payload.size()),4);
}
static void test(int fmt, const Bytes& source, const Bytes& expect, int sourceBytes) {
    Bytes file{'C','R','A','W',0,0,2,0,0,0,0,0,0,0,0,0};
    chunk(file,"META",Bytes{'{','}'});
    // Two chunks exercise reads crossing an audio part boundary.
    const size_t split = sourceBytes;
    for (auto range : {std::pair<size_t,size_t>{0,split}, {split,source.size()}}) {
        Bytes body; le(body,0,8); le(body,fmt,1); le(body,1,1); le(body,0,2);
        le(body,48000,4); le(body,(range.second-range.first)/sourceBytes,4);
        body.insert(body.end(),source.begin()+range.first,source.begin()+range.second);
        chunk(file,"AUDI",body);
    }
    const auto path = std::filesystem::temp_directory_path()/ ("craw_wav_test_"+std::to_string(fmt)+".craw");
    { std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(file.data()),file.size()); }
    const craw::tools::Clip clip(path.string());
    Bytes wav(clip.wavSize()); check(clip.readWav(0,wav.data(),wav.size())==wav.size());
    check(u32(wav,4)==wav.size()-8);
    check(wav[20]==1 && wav[21]==0); // Integer PCM, including float source.
    check(wav[34]==(fmt==1 ? 16 : 24));
    check(std::memcmp(wav.data()+36,"bext",4)==0 && u32(wav,40)==602);
    for (size_t i=44+338;i<44+346;++i) check(wav[i]==0); // TimeReference.
    const size_t data = 654;
    check(std::memcmp(wav.data()+data-8,"data",4)==0);
    check(u32(wav,data-4)==expect.size());
    check(std::equal(expect.begin(),expect.end(),wav.begin()+data));
    check(wav.size()==data+expect.size()+(expect.size()&1));
    if (expect.size()&1) check(wav.back()==0);
    for (size_t step : {1u,2u,7u}) {
        Bytes partial(wav.size());
        for (size_t p=0;p<wav.size();p+=step) {
            const size_t n=std::min(step,wav.size()-p);
            check(clip.readWav(p,partial.data()+p,n)==n);
        }
        check(partial==wav);
    }
    uint8_t byte; check(clip.readWav(wav.size(),&byte,1)==0);
    std::filesystem::remove(path);
}
int main() {
    test(1,Bytes{0,128,0,0,255,127},Bytes{0,128,0,0,255,127},2);
    test(3,Bytes{0,0,128,0,0,0,255,255,127},Bytes{0,0,128,0,0,0,255,255,127},3);
    const float values[]{-2,-1,0,0.5,1,2,std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()};
    Bytes source(sizeof values); std::memcpy(source.data(),values,sizeof values);
    Bytes expected;
    for (int32_t v : {-8388607,-8388607,0,4194304,8388607,8388607,0,8388607,-8388607})
        le(expected,static_cast<uint32_t>(v),3);
    test(2,source,expected,4);
}
