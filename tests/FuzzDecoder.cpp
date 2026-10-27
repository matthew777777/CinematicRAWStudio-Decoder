// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: cmake -DCRAW_FUZZ=ON with clang, then ./craw_fuzz_decoder corpus/
#include <craw/Decoder.h>

#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<uint16_t> out;
    try {
        const auto info = craw::probeFrame(data, size);
        if (uint64_t{info.width} * info.height > (1u << 22)) return 0;  // bound memory
        craw::decodeFrame(data, size, out, 1);
    } catch (const craw::DecodeError&) {
    }
    return 0;
}
