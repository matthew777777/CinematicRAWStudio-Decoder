# Cinematic RAW Studio — Decoder

C++20 clean-room decoder and container reader for `.CRAW`, written from
`../CinematicRAWStudio-Spec/SPEC.md`. Hardened for untrusted input
(bounds checks, CRC per chunk and per frame, crashed-file recovery).

```
cmake -S . -B build -G Ninja && ninja -C build
./build/craw_dec_conformance          # bit-exact + corruption rejection
./build/craw_dec info|verify|frames file.craw [out_dir]
cmake -B build-fuzz -DCRAW_FUZZ=ON -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++   # libFuzzer
scripts/build_android.sh              # Release arm64-v8a (NDK)
```
License: Apache-2.0.
