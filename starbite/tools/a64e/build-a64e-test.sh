#!/usr/bin/env bash
# Static arm64e test binary (no libc, no dyld) -> build/a64e/a64e-test
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/a64e
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
xcrun -sdk iphoneos clang -arch arm64e -mios-version-min=18.0 -O1 -ffreestanding -fno-builtin \
	-fno-stack-protector -nostdlib -static -Wl,-e,__start -o build/a64e/a64e-test tools/a64e/a64e-test.c
codesign -s - -f -i com.starbite.a64e-test build/a64e/a64e-test
otool -hv build/a64e/a64e-test | tail -1
