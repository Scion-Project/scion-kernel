#!/bin/sh
# Build the native Metal bridge (tools/ios27/metal2/bridge/NativeMetalBridge.rs, from IOS-26-Hot phone/metal2: UniXNU's
# bridge over the metal2vulkan runtime, MIT) as a static library for iOS arm64.
#   tools/ios27/metal2/build-m2v-bridge.sh [ios|host]      (run from anywhere; the paths are the repository's)
# Source: build/m2v/metal2vulkan = github.com/steelbrain/metal2vulkan 720ef1ce + tools/ios27/metal2/bridge/patches
# (cloned and patched once). Rust 1.98.0 with the aarch64-apple-ios target (rustup), llvm-as from an LLVM (Homebrew's
# by default; LLVM_AS=<path> to name another). Output: <repository>/build/m2v-bridge/<target>/libnative_metal_bridge.a
# Output: build/m2v-bridge/<target>/libnative_metal_bridge.a (beside, not above, the metal2vulkan workspace)
set -e
cd "$(dirname "$0")/../../.."
ROOT=$PWD
SRC=$ROOT/build/m2v/metal2vulkan
OUT=$ROOT/build/m2v-bridge
target=${1:-ios}
mkdir -p "$OUT"
if [ ! -d "$SRC" ]; then
	git clone -q https://github.com/steelbrain/metal2vulkan "$SRC"
	git -C "$SRC" checkout -q 720ef1ce
	for p in "$ROOT"/tools/ios27/metal2/bridge/patches/*.patch; do git -C "$SRC" apply "$p"; done
fi
LLVM_AS=${LLVM_AS:-/opt/homebrew/opt/llvm/bin/llvm-as}
"$LLVM_AS" -o "$OUT/runtime_examples.air" "$SRC/crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll"
"$LLVM_AS" -o "$OUT/function_constant.air" "$SRC/crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll"
cat > "$OUT/Cargo.toml" <<EOF
[package]
name = "native-metal-bridge"
version = "0.1.0"
edition = "2024"
rust-version = "1.98"
[workspace]
[lib]
crate-type = ["staticlib"]
path = "$ROOT/tools/ios27/metal2/bridge/NativeMetalBridge.rs"
[dependencies]
metal2vulkan = { path = "$SRC/crates/metal2vulkan", features = ["ash"] }
metal2vulkan-runtime = { path = "$SRC/crates/metal2vulkan-runtime" }
libc = "0.2"
sha2 = { version = "0.10", features = ["asm"] }
[profile.release]
panic = "unwind"
debug = 1
EOF
if [ "$target" = ios ]; then triple=aarch64-apple-ios; else triple=aarch64-apple-darwin; fi
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_16.4.app/Contents/Developer}
export PATH=$HOME/.cargo/bin:/opt/homebrew/opt/rustup/bin:$PATH
NMB_FIXTURE_AIR=$OUT/runtime_examples.air NMB_FC_AIR=$OUT/function_constant.air IPHONEOS_DEPLOYMENT_TARGET=16.0 \
	MACOSX_DEPLOYMENT_TARGET=14.0 cargo +1.98.0 build --release --manifest-path "$OUT/Cargo.toml" --target $triple \
	--target-dir "$OUT/target" > "$OUT/build-$target.log" 2>&1 || { grep -E "^error" -A8 "$OUT/build-$target.log" | head -60; exit 1; }
mkdir -p "$OUT/$target"
cp "$OUT/target/$triple/release/libnative_metal_bridge.a" "$OUT/$target/"
ls -la "$OUT/$target/libnative_metal_bridge.a"
