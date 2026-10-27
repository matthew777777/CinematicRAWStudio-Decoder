#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Builds and installs the crawfs FSKit module (macOS 26+) without Xcode:
# Command Line Tools swiftc/clang++ + codesign.
#
# FSKit modules need the restricted entitlement com.apple.developer.fskit.fsmodule,
# which macOS only honours with an Apple-issued identity + provisioning profile
# (an Apple Development certificate from a free or paid team works; create the
# App ID with the "FSKit Module" capability). Ad hoc builds compile and register
# but cannot run.
#   SIGN_ID="Apple Development: Name (XXXXXXXXXX)" \
#   PROFILE=~/Downloads/crawfs.provisionprofile \
#   BUNDLE_ID=org.example.crawfs.extension  tools/fskit/build.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${OUT:-$REPO/build-fskit}"
SIGN_ID="${SIGN_ID:--}"
BUNDLE_ID="${BUNDLE_ID:-org.cinematicrawstudio.crawfs.extension}"
PROFILE="${PROFILE:-}"
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
# Keys Xcode adds to every bundle; ExtensionFoundation reads the platform keys at launch.
SDK_VER="$(xcrun --show-sdk-version)"; SDK_BUILD="$(xcrun --show-sdk-build-version)"
for plist in "$APPEX/Contents/Info.plist"; do
    plutil -insert CFBundleSupportedPlatforms -json '["MacOSX"]' "$plist"
    plutil -insert DTPlatformName -string macosx "$plist"
    plutil -insert DTPlatformVersion -string "$SDK_VER" "$plist"
    plutil -insert DTSDKName -string "macosx$SDK_VER" "$plist"
    plutil -insert DTSDKBuild -string "$SDK_BUILD" "$plist"
    plutil -insert DTPlatformBuild -string "$SDK_BUILD" "$plist"
    plutil -insert DTCompiler -string com.apple.compilers.llvm.clang.1_0 "$plist"
    plutil -insert BuildMachineOSBuild -string "$(sw_vers -buildVersion)" "$plist"
done

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

plutil -replace CFBundleIdentifier -string "$BUNDLE_ID" "$APPEX/Contents/Info.plist"
plutil -replace CFBundleIdentifier -string "${BUNDLE_ID%.extension}" "$OUT/CrawFS.app/Contents/Info.plist"

echo "== sign ($SIGN_ID)"
ENT="$OUT/entitlements.plist"
cp "$HERE/CrawFSExtension.entitlements" "$ENT"
if [[ -n "$PROFILE" ]]; then
    cp "$PROFILE" "$APPEX/Contents/embedded.provisionprofile"
    TEAM="$(security cms -D -i "$PROFILE" | plutil -extract TeamIdentifier.0 raw -)"
    plutil -insert com.apple.developer.fskit.fsmodule -bool true "$ENT"
    plutil -insert com.apple.application-identifier -string "$TEAM.$BUNDLE_ID" "$ENT"
    plutil -insert com.apple.developer.team-identifier -string "$TEAM" "$ENT"
elif [[ "$SIGN_ID" == "-" ]]; then
    echo "warning: ad hoc build: FSKit will not run it (needs SIGN_ID + PROFILE, see header)" >&2
fi
codesign --force --sign "$SIGN_ID" --entitlements "$ENT" --options runtime "$APPEX"
codesign --force --sign "$SIGN_ID" --options runtime "$OUT/CrawFS.app"
codesign --verify --deep --strict "$OUT/CrawFS.app"

if [[ "${INSTALL:-1}" == 1 ]]; then
    # Updating ~/Applications/CrawFS.app in place keeps the user's enablement;
    # a first install must be enabled once in System Settings > General >
    # Login Items & Extensions > File System Extensions.
    DEST="$HOME/Applications/CrawFS.app"
    first=0; [[ -d "$DEST" ]] || first=1
    mkdir -p "$HOME/Applications"
    rsync -a --delete "$OUT/CrawFS.app/" "$DEST/"
    if [[ $first == 1 ]]; then
        /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f -R "$DEST"
        pluginkit -a "$DEST/Contents/Extensions/CrawFSExtension.appex"
        echo "Enable crawfs in System Settings > General > Login Items & Extensions > File System Extensions"
        open "x-apple.systempreferences:com.apple.LoginItems-Settings.extension" || true
    fi
    echo "installed $DEST"
fi
echo "built $OUT/CrawFS.app"
