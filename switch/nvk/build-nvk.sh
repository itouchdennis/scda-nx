#!/bin/bash
# mesa-switch (NVK for Horizon) on Linux without Docker: the steps of build-switch.sh / build-unified.sh, Vulkan only.
# Needs: devkitA64, meson, ninja, clang/llvm/libclc/spirv-llvm-translator, rust + rust-aarch64-gnu, bindgen, cbindgen.
#   MESA=/path/to/mesa-switch switch/nvk/build-nvk.sh
# Result: $OUT/opt/devkitpro/portlibs/switch (lib/libvulkan.a, include/vulkan), where switch/build.sh looks for it
# (default OUT build/nvk-sdk). The meson cross and native files and the bindgen wrapper go to build/nvk-work.
set -euo pipefail
SCRIPT="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT/../.." && pwd)"
MESA="${MESA:-$REPO/../mesa-switch}"
OUT="${OUT:-$REPO/build/nvk-sdk}"
HERE="$REPO/build/nvk-work"
mkdir -p "$HERE/bin"
cp "$SCRIPT/bin/rustc" "$HERE/bin/rustc"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export LC_ALL=C TZ=UTC ZERO_AR_DATE=1
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$MESA" show -s --format=%ct HEAD)}"
test -d /usr/lib/rustlib/aarch64-unknown-linux-gnu || { echo "install rust-aarch64-gnu (pacman -S rust-aarch64-gnu)"; exit 2; }
for lib in libexpat.a libz.a libzstd.a; do
    test -f /opt/devkitpro/portlibs/switch/lib/$lib ||
        { echo "missing $lib: sudo pacman -S switch-libexpat switch-zlib switch-libzstd"; exit 2; }
done

# the in-tree bindgen wrapper (atomic shim, devkitA64 headers) beside our rustc
cp "$MESA/bindgen-switch-wrapper.sh" "$HERE/bin/bindgen"
cp "$MESA/bindgen-atomic-shim.h" "$HERE/bin/"
chmod +x "$HERE/bin/bindgen"

cat > "$HERE/cross.txt" <<CROSS
[binaries]
c = '$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc'
cpp = '$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++'
ar = '$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc-ar'
nm = '$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm'
strip = '$DEVKITPRO/devkitA64/bin/aarch64-none-elf-strip'
pkg-config = '$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config'
rust = '$HERE/bin/rustc'
bindgen = '$HERE/bin/bindgen'

[built-in options]
c_args = ['-D__SWITCH__','-DHAVE_TIMESPEC_GET','-I$DEVKITPRO/portlibs/switch/include','-isystem','$DEVKITPRO/libnx/include','-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-fPIC','-ftls-model=local-exec','-ffunction-sections','-fdata-sections']
c_link_args = ['-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-fPIC','-ftls-model=local-exec','-L$DEVKITPRO/portlibs/switch/lib','-L$DEVKITPRO/libnx/lib','-lnx']
cpp_args = ['-D__SWITCH__','-DHAVE_TIMESPEC_GET','-I$DEVKITPRO/portlibs/switch/include','-isystem','$DEVKITPRO/libnx/include','-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-fPIC','-ftls-model=local-exec','-ffunction-sections','-fdata-sections']
cpp_link_args = ['-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-fPIC','-ftls-model=local-exec','-L$DEVKITPRO/portlibs/switch/lib','-L$DEVKITPRO/libnx/lib','-lnx']

[host_machine]
system = 'horizon'
cpu_family = 'aarch64'
cpu = 'cortex-a57'
endian = 'little'
CROSS

cat > "$HERE/native.txt" <<NATIVE
[binaries]
mesa_clc = '$MESA/builddir-native/src/compiler/clc/mesa_clc'
vtn_bindgen2 = '$MESA/builddir-native/src/compiler/spirv/vtn_bindgen2'
cbindgen = '$(command -v cbindgen)'
rust = '/usr/bin/rustc'
NATIVE

cd "$MESA"
echo "== host tools"
if [ ! -x builddir-native/src/compiler/clc/mesa_clc ]; then
    meson setup builddir-native --wipe -Dvulkan-drivers= -Dgallium-drivers= -Dshader-cache=disabled -Dplatforms= \
        -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dtools=[] \
        -Dllvm=enabled -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dinstall-mesa-clc=true
fi
ninja -C builddir-native src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2

echo "== cross build"
setup=(); [ -f builddir-linux/meson-private/coredata.dat ] && setup=(--reconfigure)
meson setup builddir-linux "${setup[@]}" --cross-file "$HERE/cross.txt" --native-file "$HERE/native.txt" \
    --default-library=static --prefix=/opt/devkitpro/portlibs/switch --libdir=lib \
    --buildtype=release -Doptimization=2 -Db_lto=false -Db_ndebug=true \
    -Dvulkan-drivers=nouveau -Dgallium-drivers= -Dgallium-rusticl=false -Dplatforms=switch \
    -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dvideo-codecs= \
    -Dshader-cache=enabled -Dxmlconfig=enabled -Dexpat=enabled -Dtools=[] -Dllvm=disabled \
    -Dshared-glapi=disabled -Dshared-llvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system \
    -Dcpp_rtti=false -Dbuild-tests=false
ninja -C builddir-linux -j"${JOBS:-$(nproc)}"
rm -rf "$OUT"
meson install -C builddir-linux --destdir "$OUT"
echo "== done:"
find "$OUT" -name '*.a' -exec ls -la {} \;
