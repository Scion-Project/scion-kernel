# source this (from bash) before building XNU on Linux
# Everything is found relative to this file. THEOS (default ~/theos) provides the cctools and the
# Darwin SDK; XNU_SRC is the kernel tree (xnu/apply.sh creates it).
DARWIN_REDWOOD=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export DARWIN_REDWOOD
export XNU_HOST_OS=$(uname)
# On macOS there is no theos: xnu-deps/setup.sh lays out xnu-deps/theos-mac like one (links to the
# cctools of Xcode and to the iPhoneOS 16.5 SDK of github.com/theos/sdks).
if [ "$XNU_HOST_OS" = Darwin ]; then
	export THEOS=${THEOS:-$DARWIN_REDWOOD/xnu-deps/theos-mac}
else
	export THEOS=${THEOS:-$HOME/theos}
fi
# the kernel tree and SDK are the OS profile's (profiles/*.conf; ios-16.5 unless OS_PROFILE or .os-profile says otherwise)
_scion_profile() { (cd "$DARWIN_REDWOOD" && OS_PROFILE_QUIET=1 python3 tools/os-profile.py get "$1" 2>/dev/null) || echo "$2"; }
export XNU_SRC=${XNU_SRC:-$DARWIN_REDWOOD/$(_scion_profile src xnu-src/xnu-8796.121.2)}
export XNUDEPS=$DARWIN_REDWOOD/xnu-deps
export PATH=$XNUDEPS/host/bin:$PATH
export XNU_ARCH_STRING=${XNU_ARCH_STRING:-$(_scion_profile arch arm64)}
# xnu-12377's build reads its own version (the Darwin release: kern.osrelease, "Darwin Kernel Version ...") from the SDK's
# System.kext, which a public SDK lacks: give it the profile's `darwin` (25.5.0 for ios-26)
if [ "$(_scion_profile base xnu-8796)" != xnu-8796 ]; then
	export RC_DARWIN_KERNEL_VERSION=${RC_DARWIN_KERNEL_VERSION:-$(_scion_profile darwin 0.0.0)}
	# and it calls /usr/bin/xcrun by its path, where xnu-8796 called xcrun: ours (xnu-deps/host/bin) answers with our SDK
	export XNU_MAKE_ARGS="XCRUN=xcrun"
	# DriverKit's .iig files: on macOS Apple's iig from Xcode (xnu-deps/host/bin/iig hands over to it); elsewhere the
	# headers it generates, ready-made in the macOS 26.1 SDK that xnu-deps/setup.sh fetches for this base
	if [ "$XNU_HOST_OS" = Darwin ] && _iig=$(/usr/bin/xcrun -f iig 2>/dev/null); then
		export XNU_IIG=$_iig
	else
		export XNU_IIG_HEADERS=$XNUDEPS/MacOSX26.1.sdk/System/Library/Frameworks/Kernel.framework/Headers
	fi
fi
export XNU_SDK_VERSION=${XNU_SDK_VERSION:-$(_scion_profile sdk 16.5)}
export XNU_SDK_PATH=${XNU_SDK_PATH:-$XNUDEPS/sdk/iPhoneOS$XNU_SDK_VERSION.xnu.sdk}
export XNU_PLATFORM_PATH=$XNUDEPS/sdk/Platforms/iPhoneOS.platform
