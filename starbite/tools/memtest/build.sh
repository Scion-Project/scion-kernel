#!/usr/bin/env bash
# Build tools/memtest/memtest.c as a static arm64 iOS executable (no libSystem,
# no dyld) -> build/memtest/launchd.memtest, ad-hoc signed. Install it as
# /usr/appleinternal/sbin/launchd.memtest and boot with launchdsuffix=memtest.
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
mkdir -p build/memtest
xcrun --sdk iphoneos clang -target arm64-apple-ios18.0 -isysroot "$SDK" \
    -O1 -ffreestanding -fno-builtin -fno-stack-protector -fno-stack-check \
    -mno-outline-atomics -static -nostdlib -Wl,-e,__start \
    -o build/memtest/launchd.memtest tools/memtest/memtest.c
codesign -f -s - build/memtest/launchd.memtest
file build/memtest/launchd.memtest
otool -l build/memtest/launchd.memtest | grep -E "cmd LC_(UNIXTHREAD|MAIN|LOAD_DYLINKER|LOAD_DYLIB)" || true
