# crawfs — FSKit module (macOS 26+)

Mounts a `.CRAW` take as a CinemaDNG folder without FUSE (same approach as
mcrawfs: FSKit unary file system on an `FSPathURLResource`):

```
mount -F -t crawfs ~/Movies/IMG_..._VID.craw ~/CRAW/take
# -> take_000000.dng ... take.wav ; drag the folder into DaVinci Resolve
umount ~/CRAW/take
```

DNG/WAV content is identical to `craw2dng` (lossless CFA, colour metadata,
GainMap lens shading, CinemaDNG timecode); frames are cached with read-ahead.

## Build and enable

FSKit requires the `com.apple.developer.fskit.fsmodule` entitlement, which
macOS honours only for code signed by an Apple-issued identity with a
provisioning profile. Ad hoc builds register but crash/are refused at launch.

1. In the Apple Developer portal (free or paid team): create an App ID such as
   `org.example.crawfs.extension` with the **FSKit Module** capability, a
   macOS development provisioning profile for it, and install your
   "Apple Development" certificate in the login keychain
   (`security find-identity -v -p codesigning` must list it).
2. Build and install to `~/Applications/CrawFS.app`:
   ```
   SIGN_ID="Apple Development: Your Name (XXXXXXXXXX)" \
   PROFILE=~/Downloads/crawfs.provisionprofile \
   BUNDLE_ID=org.example.crawfs.extension  tools/fskit/build.sh
   ```
3. Enable once: System Settings → General → Login Items & Extensions →
   File System Extensions → CrawFS. Re-registering (a new signature) resets
   this switch.

Without macOS 26 or signing, use `craw2dng` (export) or `craw_mount` (FUSE).
