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

## Easy install: Mac, Linux and Windows

Install the complete toolset once with the
[shared installer](https://github.com/matthew777777/CinematicRAWStudio-Spec/blob/main/GETTING_STARTED.md).
It fetches the sibling repos and dependencies automatically.

Mac/Linux:

```sh
curl -fsSL https://raw.githubusercontent.com/matthew777777/CinematicRAWStudio-Spec/main/scripts/install.sh | bash
```

Windows PowerShell:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/matthew777777/CinematicRAWStudio-Spec/main/scripts/install.ps1)))
```

Open a new terminal, then use `craw` for the editor,
`craw mount "take.craw"` for Resolve, `craw unmount` to close mounts,
or `craw export "take.craw"` to save CinemaDNG and sound.


## DaVinci Resolve workflow

End users: see **GETTING_STARTED.md** in CinematicRAWStudio-Spec — the
installer sets up one-click mounting (`CRAW Mount.app`, `craw-mount`).

### craw_mount (FUSE: fuse-t on macOS, libfuse 2 on Linux, WinFsp on Windows)

```
craw_mount MOUNTPOINT take.craw --flat [-f]        # one take, files at the root
craw_mount MOUNTPOINT a.craw b.craw [-f]           # several takes, one folder each
umount MOUNTPOINT                                  # (Linux: fusermount -u)
```
- Read-only; every take shows `<name>_NNNNNN.dng` and `<name>.wav`.
- DNGs are built on demand with an LRU cache and 4-frame read-ahead; every
  frame of a take has the same size (pixel data at a fixed padded offset), so
  listings need no decoding.
- Without `-f` it daemonises; `-f` keeps it in the foreground (logs to stderr).

### craw2dng

```
craw2dng take.craw OUT_DIR [--frames FIRST-LAST] [--no-audio] [--threads N]
```
Writes `OUT_DIR/<name>/<name>_NNNNNN.dng` and `<name>.wav`.

### DNG contents (both tools)

CFA pixels bit-exact to the recording · CFAPattern · BlackLevel (2x2, dynamic
per frame when recorded) · WhiteLevel · ColorMatrix/ForwardMatrix/
CameraCalibration 1+2 with illuminants · AsShotNeutral · ISO · ExposureTime ·
CinemaDNG FrameRate + TimeCodes · lens-shading map as OpcodeList2 GainMap
(one per CFA phase) · orientation only from an explicit capture `orientation`
key (landscape video is never rotated by the sensor mount angle). Metadata is
read from the SPEC.md META layout or the flat RawLens keys.

### FSKit module (macOS 26+, no FUSE)

`tools/fskit` — `mount -F -t crawfs take.craw MOUNTPOINT`. Requires an Apple
Development certificate and provisioning profile; see `tools/fskit/README.md`.

### Verified (macOS)

8 real 4080x2040 frames: DNG pixels byte-identical to the source both exported
and mounted; LibRaw (`dcraw_emu`) develops them; exiftool shows all tags;
float32 stereo WAV reads correctly; sequential reads ~42 ms/frame through the
mount.

## Recording codec and audio compatibility

Both CRAW-Live (codec 2) and CRAW-Fast (codec 1) decode to the original RAW
samples. Payloads without a frame CRC rely on the container chunk CRC32C.
WAV exports use integer PCM, retaining 16/24-bit source precision and
converting float to 24-bit. They include Broadcast WAV timecode and RIFF
padding for odd-length audio. NaN float samples become silence; infinities
clip to full scale. `ctest --test-dir build-release --output-on-failure`
checks conformance and WAV output, including arbitrary partial reads used
by the virtual mount.

In Resolve, place the video and WAV at the same timeline start. See the
[sibling getting-started guide](../CinematicRAWStudio-Spec/GETTING_STARTED.md).
