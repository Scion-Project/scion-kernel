#!/bin/bash
# Rust 1.98.0 (host aarch64-apple-darwin, target aarch64-apple-ios) for metal2vulkan's bridge, kept under $W/rust.
set -e
W=${W:-$PWD/build/metal27}
export CARGO_HOME=$W/rust/cargo RUSTUP_HOME=$W/rust/rustup
curl -sSf https://static.rust-lang.org/rustup/dist/aarch64-apple-darwin/rustup-init -o /tmp/rustup-init-m27 && chmod +x /tmp/rustup-init-m27
/tmp/rustup-init-m27 -y --no-modify-path --profile minimal --default-toolchain 1.98.0 -t aarch64-apple-ios
$CARGO_HOME/bin/rustc --version; echo RUST-DONE
