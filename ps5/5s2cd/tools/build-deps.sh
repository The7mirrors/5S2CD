#!/bin/bash
# Build PS5SX2's public dependencies for the RADV path (ps5/docs/build-dependencies.md) into <repo>/deps.
# Needs: PS5_PAYLOAD_SDK (the ps5-payload-dev SDK), the dependency sources checked out in deps/ as that document lists
# (PS5_Vulkan, PS5_Mesa, PS5_PayloadSDK, glslang, ...) and deps/agc-symbols (DNNDHH's libSceAgc/libSceAgcDriver
# listings). HOST_TOOLS = prefix of host builds that are newer than Ubuntu 22.04's (optional).
set -e
here=$(cd "$(dirname "$0")" && pwd)
cd "$here/../../.."
: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK to the ps5-payload-dev SDK}"
if [ -n "$HOST_TOOLS" ]; then
	export PATH=$HOST_TOOLS/bin:$PATH PKG_CONFIG_PATH=$HOST_TOOLS/lib/pkgconfig LD_LIBRARY_PATH=$HOST_TOOLS/lib
fi
export PATH=/usr/lib/llvm-18/bin:$PATH
J=$(nproc)
step() { echo; echo "===== $(date +%H:%M:%S) $*"; }
step "venv with a recent meson"
[ -x deps/build-env/bin/meson ] || { python3 -m venv deps/build-env && deps/build-env/bin/pip -q install meson==1.12.1 mako packaging pyyaml; }
step "SDK copy + AGC import stubs (link-time only, never deployed)"
[ -d deps/sdk285 ] || cp -a "$PS5_PAYLOAD_SDK" deps/sdk285
python3 "$here/gen_agc_stubs.py" deps/agc-symbols deps/sdk285/target/lib
export PS5_PAYLOAD_SDK="$PWD/deps/sdk285"
step "glslang for PS5"
cmake -S deps/glslang -B deps/glslang/build-ps5 -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
	-DCMAKE_BUILD_TYPE=Release -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF
cmake --build deps/glslang/build-ps5 -j"$J"
step "PS5_Vulkan native dependencies (SDK fork bootstrap)"
PS5_PAYLOAD_SDK_FORK="$PWD/deps/PS5_PayloadSDK" BUILD_JOBS=$J bash deps/PS5_Vulkan/tools/setup-native-dependencies.sh
step "RADV driver (Mesa)"
MESON="$PWD/deps/build-env/bin/meson" PS5_MESA_FORK="$PWD/deps/PS5_Mesa" PATH="$PWD/deps/build-env/bin:$PATH" BUILD_JOBS=$J \
	bash deps/PS5_Vulkan/tools/build-radv.sh release
step "DEPS DONE"
