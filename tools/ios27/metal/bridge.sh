#!/bin/bash
# metal2vulkan's bridge for iOS (libnative_metal_bridge.a), with Starbite's patches, through its build-m2v-bridge.sh.
#   SB=<Starbite tree> DEVELOPER_DIR=<Xcode 16.4> tools/ios27/metal/bridge.sh   (run rust.sh first)
W=${W:-$PWD/build/metal27}
export CARGO_HOME=$W/rust/cargo RUSTUP_HOME=$W/rust/rustup
export PATH=$CARGO_HOME/bin:$PATH DEVELOPER_DIR=${DEVELOPER_DIR:?set DEVELOPER_DIR to an Xcode}
export CARGO_BUILD_JOBS=4
cd "${SB:?set SB to the Starbite tree}" && nice -n 10 sh tools/metal2/build-m2v-bridge.sh ios; echo "BRIDGE-EXIT $?"
