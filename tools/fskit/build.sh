#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Builds and registers the crawfs FSKit module (macOS 26+) without Xcode:
# Command Line Tools swiftc/clang++ + codesign.
#   tools/fskit/build.sh            # build to build-fskit/, sign ad hoc, register
#   SIGN_ID="Developer ID Application: ..." tools/fskit/build.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${OUT:-$REPO/build-fskit}"
SIGN_ID="${SIGN_ID:--}"
TARGET="$(uname -m)-apple-macos26.0"
APPEX="$OUT/CrawFS.app/Contents/Extensions/CrawFSExtension.appex"

rm -rf "$OUT" && mkdir -p "$OUT/obj" "$APPEX/Contents/MacOS" "$OUT/CrawFS.app/Contents/MacOS"

echo "== C++ core"
for src in "$REPO/src/Decoder.cpp" "$REPO/src/Reader.cpp" "$REPO/tools/common/Clip.cpp" "$HERE/core/CrawFSCore.cpp"; do
    clang++ -std=c++20 -O3 -DNDEBUG -target "$TARGET" -I"$REPO/include" -c "$src" -o "$OUT/obj/$(basename "$src" .cpp).o"
done
ar rcs "$OUT/libcrawfscore.a" "$OUT"/obj/*.o

echo "== Swift extension"
swiftc -parse-as-library -swift-version 5 -O -target "$TARGET" \
    -I "$HERE/core" "$HERE"/Sources/*.swift "$OUT/libcrawfscore.a" -lc++ \
    -framework FSKit -o "$APPEX/Contents/MacOS/CrawFSExtension"
cp "$HERE/Info.plist" "$APPEX/Contents/Info.plist"

# Minimal host app: FSKit modules are enabled in System Settings from their app.
cat > "$OUT/host.swift" <<'SWIFT'
import Foundation
print("Cinematic RAW Studio crawfs: enable it in System Settings > General > Login Items & Extensions > File System Extensions, then: mount -F -t crawfs take.craw /path/to/mountpoint")
SWIFT
swiftc -O -target "$TARGET" "$OUT/host.swift" -o "$OUT/CrawFS.app/Contents/MacOS/CrawFS"
cat > "$OUT/CrawFS.app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>CrawFS</string>
<key>CFBundleIdentifier</key><string>org.cinematicrawstudio.crawfs</string>
<key>CFBundleName</key><string>CrawFS</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.2.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>26.0</string>
<key>LSUIElement</key><true/>
</dict></plist>
PLIST

echo "== sign ($SIGN_ID)"
codesign --force --sign "$SIGN_ID" --entitlements "$HERE/CrawFSExtension.entitlements" --options runtime "$APPEX"
codesign --force --sign "$SIGN_ID" --options runtime "$OUT/CrawFS.app"
codesign --verify --deep --strict "$OUT/CrawFS.app"

if [[ "${REGISTER:-1}" == 1 ]]; then
    echo "== register"
    pluginkit -a "$APPEX"
    pluginkit -m -v -i org.cinematicrawstudio.crawfs.extension || true
fi
echo "built $OUT/CrawFS.app"
