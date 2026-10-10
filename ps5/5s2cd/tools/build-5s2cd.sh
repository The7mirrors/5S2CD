#!/bin/bash
# Build 5S2CD's eboot.bin with the public RADV driver, the way PS5SX2 documents it (ps5/docs/build-dependencies.md,
# "Successful RADV build attempt"). Run build-deps.sh once first. Output: ps5/build-radv/app/eboot.bin
#   HOST_TOOLS   prefix of the host SPIRV-LLVM-Translator / SPIRV-Tools / glslang builds (optional)
#   PS5SX2_DEPS  the dependency folder (default: <repo>/deps)
set -e
cd "$(dirname "$0")/../../.."
export PATH=${HOST_TOOLS:+$HOST_TOOLS/bin:}/usr/lib/llvm-18/bin:$PATH
export PS5SX2_DEPS="${PS5SX2_DEPS:-$PWD/deps}"
export PS5_PAYLOAD_SDK="$PS5SX2_DEPS/sdk285"
export PS5_RADV_ARCHIVE="$PS5SX2_DEPS/PS5_Vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
export PS5_RADV_PLATFORM="$PS5SX2_DEPS/PS5_Vulkan/.deps/native/ps5-payload-sdk/target/lib/libps5platform.a"
export PS5_RADV_BUILTINS=$(ls /usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.builtins-x86_64.a)
export PS5_RADV_REV=0b2d6d1a61d9
export LLD=ld.lld NM=llvm-nm HOST_CXX=clang++
export LLD_EXTRA='--no-dynamic-linker -z nodynamic-undefined-weak'
TAG=${1:-5s2cd}
echo "== compile"
make -C ps5/coreorbis -f Makefile.vk -j"$(nproc)"
echo "== chain the NFS and memstream fclose/fflush wrappers"
PATCHED=$PS5SX2_DEPS/libps5platform-chained.a
cp "$PS5_RADV_PLATFORM" "$PATCHED"
llvm-objcopy --redefine-sym __wrap_fflush=__mem_wrap_fflush --redefine-sym __wrap_fclose=__mem_wrap_fclose "$PATCHED"
export PS5_RADV_PLATFORM="$PATCHED"
NFSO=ps5/coreorbis/obj-vk/shim/orbis-shims/OrbisNfs.o
llvm-nm "$NFSO" | grep -q __mem_wrap_fflush ||
	llvm-objcopy --redefine-sym __real_fflush=__mem_wrap_fflush --redefine-sym __real_fclose=__mem_wrap_fclose "$NFSO"
echo "== link"
ORBIS_BUILD_TAG=$TAG ORBIS_TEST_BUILD=0 bash ps5/coreorbis/link-radv.sh
ls -la ps5/build-radv/app/eboot.bin
