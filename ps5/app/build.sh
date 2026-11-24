#!/usr/bin/env bash
# Build chiaki-ps5 as a PS5 directory title (dist/PPSA48765) on the RADV Vulkan driver.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
B=$HOME/b
root=$B/PS5_Vulkan-main
sdk_root=$root/.deps/native/ps5-payload-sdk
radv=$root/.deps/native/radv-release
archive=$radv/lib/libvulkan_radeon.ps5.a
HB=$B/pacbrew/opt/ps5-payload-sdk/target/user/homebrew
FG=$B/ps5-framegen-main
C=$B/chiaki-ng
tool=$root/build/host/ps5-native-tool
native=$root/tooling/native
work=$here/build
mkdir -p "$work/obj" "$work/gen"
cc() { PS5_PAYLOAD_SDK="$sdk_root" sh "$root/tooling/prospero-clang18" "$@"; }

glslangValidator -V --target-env vulkan1.1 --vn fs_vert "$here/shaders/fs.vert" -o "$work/gen/fs_vert.h" >/dev/null
glslangValidator -V --target-env vulkan1.1 --vn present_frag "$here/shaders/present.frag" -o "$work/gen/present_frag.h" >/dev/null

INC=(-I "$radv/include" -I "$work/gen" -I "$here" -I "$C/lib/include" -I "$HB/include" -I "$FG/src")
CXXF=(-std=c++20 -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
cc "${CXXF[@]}" "${INC[@]}" -c "$here/main.cpp" -o "$work/obj/main.o"
cc "${CXXF[@]}" "${INC[@]}" -DPS5_FRAMEGEN_VULKAN_HEADER='"vkfns.h"' -c "$FG/src/ps5_framegen.cpp" -o "$work/obj/ps5_framegen.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"

cc -std=c11 -O2 -ffunction-sections -fdata-sections -c "$here/compat_stubs.c" -o "$work/obj/compat_stubs.o"
cc -std=c11 -O2 -ffunction-sections -fdata-sections -c "$here/net_wrap.c" -o "$work/obj/net_wrap.o"
mkdir -p "$work/stubs"
stub() {
    cc -std=c11 -O2 -fPIC -c "$root/$2" -o "$work/obj/$1_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "$1.prx" -o "$work/stubs/$1.so" "$work/obj/$1_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
source "$root/tools/radv-link.sh"
radv_link_recipe "$root" "$sdk_root" "$archive"
# The platform binds getaddrinfo to a stub that always fails; use ours (IP literals).
flags=()
for f in "${radv_link_flags[@]}"; do
    [[ $f == --defsym=getaddrinfo=* || $f == --defsym=freeaddrinfo=* ]] || flags+=("$f")
done
radv_link_flags=("${flags[@]}" --defsym=getaddrinfo=__wrap_getaddrinfo --defsym=freeaddrinfo=__wrap_freeaddrinfo --defsym=getnameinfo=__ps5app_getnameinfo)
L=$HB/lib
"$sdk_root/bin/prospero-lld" --error-limit=0 "${radv_linker_script[@]}" --eh-frame-hdr @"$root/tooling/radv-weak-null.rsp" "${radv_link_flags[@]}" \
    --version-script "$here/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "$work/obj/main.o" "$work/obj/ps5_framegen.o" "$work/obj/compat_stubs.o" "$work/obj/net_wrap.o" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    "$here/libchiaki.a" \
    --start-group "$L/libavcodec.a" "$L/libswresample.a" "$L/libavutil.a" "$L/libopus.a" "$L/libssl.a" "$L/libcrypto.a" \
    "$L/libimgui.a" "$L/libx264.a" "$L/liblzma.a" "$L/libiconv.a" --end-group \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --stub "$work/stubs/libSceAgc.so" --stub "$work/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
app=$here/dist/PPSA48765
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$here/param.json" "$app/sce_sys/param.json"
cp "$here/assets/icon0.png" "$app/sce_sys/icon0.png"
cp "$root/runtime/libc.prx" "$app/sce_module/libc.prx"
"$tool" self --inspect --file "$app/eboot.bin" >/dev/null
echo "built $app ($(stat -c %s "$app/eboot.bin") bytes)"
