#!/bin/bash
# SPDX-License-Identifier: MIT-0
# build-sileo.sh [OUT] : Sileo.app built from the project's fork of Sileo (github.com/Scion-Project/Sileo, the Sileo Team's
# work under its own BSD-4-Clause licence, with the Scion project's changes), at the commit pinned below, with Xcode on a
# Mac. prepare.sh runs it when SILEO_FROM=fork and puts the result in the prefix in place of the Procursus package's app.
# Why a build of our own: Sileo 2.5.1 becomes root only by a spawn into persona 99 and, when the kernel refuses that,
# leaves the install at "Queued" for ever without a word; the fork falls back to the bundle's giveMeRoot and reports a
# failed root spawn (docs/ios27/SILEO.md, "Stuck at Queued").
# What it does, as the fork's own Makefile "stage" step does it, with Apple's tools instead of ldid and dpkg-deb:
#   1. the fork at SILEO_COMMIT (a shallow fetch of that one commit; the commit is checked), its Swift packages at the
#      revisions its Package.resolved names;
#   2. one fix in a dependency's checkout: elihwyma/Alderis declares ColorPickerInnerViewController.tab, which collides
#      with UIViewController.tab of the iOS 18 and later SDKs (upstream hbang/Alderis renamed it colorPickerTab);
#   3. xcodebuild, Release, arm64, deployment target 16.5, no signing;
#   4. the app as the .deb has it: no _CodeSignature, no Frameworks (the system has the Swift libraries), stripped; the
#      binary records SDK 16.5 (vtool): iOS 27's UIKit refuses to start an app without scenes that says it was built
#      with the iOS 27 SDK; giveMeRoot built from the fork's giveMeRoot.c; both signed ad hoc with the fork's
#      entitlements files; giveMeRoot 4755.
# Not reproducible byte for byte (the compiler, the SDK); OUT/SOURCE says what it was built from and OUT/SHA256SUMS what
# came out. Needs Xcode (tested: 27.2) and the network. No root. Nothing of it goes into this repository.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=${1:-${JB_ROOT:-$REPO/jb}/sileo-fork}
SILEO_GIT=https://github.com/Scion-Project/Sileo.git
SILEO_COMMIT=426b5529818bbdce5de3e5e47697a39a62073995     # branch agent/scion-sileo/root-fallback (Scion-Project/Sileo PR 1)
W=$OUT/work
die() { echo "build-sileo.sh: $*" >&2; exit 1; }
command -v xcodebuild >/dev/null || die "no xcodebuild (Xcode)"

mkdir -p "$W"
S=$W/src
if [[ ! -d $S/.git ]]; then
	git init -q "$S"
	git -C "$S" remote add origin "$SILEO_GIT"
fi
if [[ $(git -C "$S" rev-parse -q --verify HEAD 2>/dev/null || true) != "$SILEO_COMMIT" ]]; then
	git -C "$S" fetch -q --depth 1 origin "$SILEO_COMMIT"
	git -C "$S" checkout -q --detach FETCH_HEAD
fi
[[ $(git -C "$S" rev-parse HEAD) == "$SILEO_COMMIT" ]] || die "the checkout is not $SILEO_COMMIT"

XB=(xcodebuild -project "$S/Sileo.xcodeproj" -scheme Sileo -configuration Release -arch arm64 -sdk iphoneos
	-derivedDataPath "$W/dd" -clonedSourcePackagesDirPath "$W/spm")
"${XB[@]}" -resolvePackageDependencies > "$W/resolve.log" 2>&1 || { tail -20 "$W/resolve.log" >&2; die "package resolution failed ($W/resolve.log)"; }
AL=$W/spm/checkouts/Alderis/Alderis/ColorPickerInnerViewController.swift
if [[ -f $AL ]] && grep -q 'var tab: ColorPickerTab' "$AL"; then
	sed -i '' -e 's/var tab: ColorPickerTab/var pickerTab: ColorPickerTab/' -e 's/^\([[:space:]]*\)tab = configuration.initialTab/\1pickerTab = configuration.initialTab/' "$AL"
	grep -q 'pickerTab = configuration.initialTab' "$AL" || die "Alderis changed: the tab fix no longer applies"
fi
"${XB[@]}" -disableAutomaticPackageResolution CODE_SIGNING_ALLOWED=NO PRODUCT_BUNDLE_IDENTIFIER=org.coolstar.SileoStore \
	DISPLAY_NAME=Sileo ALWAYS_EMBED_SWIFT_STANDARD_LIBRARIES=NO IPHONEOS_DEPLOYMENT_TARGET=16.5 RUN_CLANG_STATIC_ANALYZER=NO \
	build > "$W/build.log" 2>&1 || { grep -E 'error:' "$W/build.log" | sort -u | head -20 >&2; die "xcodebuild failed ($W/build.log)"; }

A=$OUT/Sileo.app
rm -rf "$A"
cp -R "$W/dd/Build/Products/Release-iphoneos/Sileo.app" "$A"
rm -rf "$A/_CodeSignature" "$A/Frameworks"
xcrun strip "$A/Sileo"
xcrun vtool -set-build-version ios 16.5 16.5 -replace -output "$A/Sileo.vt" "$A/Sileo"
mv -f "$A/Sileo.vt" "$A/Sileo"
xcrun -sdk iphoneos clang -arch arm64 -mios-version-min=11.0 -O2 -Wall -o "$A/giveMeRoot" "$S/giveMeRoot/giveMeRoot.c"
codesign -f -s - --entitlements "$S/Sileo/Entitlements.entitlements" "$A/Sileo" 2>/dev/null
codesign -f -s - --entitlements "$S/giveMeRoot/Entitlements.plist" "$A/giveMeRoot" 2>/dev/null
chmod 4755 "$A/giveMeRoot"
# plain arm64 only (60-sileo.sh checks the whole prefix for it too)
for f in Sileo giveMeRoot; do
	[[ $(lipo -archs "$A/$f") == arm64 ]] || die "$f is not plain arm64"
done
otool -l "$A/Sileo" | grep -A4 LC_BUILD_VERSION | grep -q 'sdk 16.5' || die "the SDK version was not set"
codesign -d --entitlements - "$A/Sileo" 2>/dev/null | grep -q 'com.apple.private.persona-mgmt' || die "Sileo lost its entitlements"
printf '%s %s\nxcode %s\n' "$SILEO_GIT" "$SILEO_COMMIT" "$(xcodebuild -version | tr '\n' ' ')" > "$OUT/SOURCE"
(cd "$A" && shasum -a 256 Sileo giveMeRoot) > "$OUT/SHA256SUMS"
echo "build-sileo.sh: $A ($(du -sh "$A" | cut -f1)) from Scion-Project/Sileo ${SILEO_COMMIT:0:12}"
