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

## DaVinci Resolve workflow

Resolve imports CinemaDNG natively. Two lossless ways to get there:

**Mount (no conversion, no extra disk):** requires [fuse-t](https://www.fuse-t.org)
(kext-free FUSE for macOS) or libfuse on Linux.

```
mkdir -p ~/CRAW
./build/craw_mount ~/CRAW /path/to/IMG_..._VID.craw [more.craw ...]   # -f to stay in foreground
# In Resolve: Media page -> ~/CRAW/<clip>/ -> drag the folder in (DNG sequence + WAV)
umount ~/CRAW
```

Each take appears as `<name>/<name>_000000.dng ... <name>.wav`; DNGs are
decoded on demand with read-ahead (about 24 fps sequential for 4080x2040 on an
M-series Mac).

**Export:** `./build/craw2dng clip.craw out_dir [--frames 0-99] [--no-audio]`
writes the same files to disk.

DNG contents: CFA pixels bit-exact to the recording, black/white levels,
ColorMatrix/ForwardMatrix/CameraCalibration 1+2 with illuminants,
AsShotNeutral, ISO and exposure time, CinemaDNG FrameRate + TimeCodes, and the
lens-shading/vignetting map as DNG OpcodeList2 GainMap (one per CFA phase).
Audio: WAV in the recorded format (32-bit float or 16-bit PCM).
