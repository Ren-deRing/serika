#!/bin/sh
# the host's mesa with its software renderer (llvmpipe) and what it loads, copied to the same paths under $1
set -e

root=$1

copy() {
    for f in "$@"; do
        mkdir -p "$root$(dirname "$f")"
        cp -L "$f" "$root$f"
    done
}

# what a program links, and what the loaders open by name
libs="/usr/lib64/libEGL.so.1 /usr/lib64/libGLESv2.so.2 /usr/lib64/libGLdispatch.so.0 /usr/lib64/libEGL_mesa.so.0 \
      /usr/lib64/libgbm.so.1 /usr/lib64/dri/libdril_dri.so $(ls /usr/lib64/libgallium-*.so)"
copy $libs

# glvnd finds the vendor by this file, and mesa finds the driver by the name of its link
copy /usr/share/glvnd/egl_vendor.d/50_mesa.json
ln -sf libdril_dri.so "$root/usr/lib64/dri/swrast_dri.so"
ln -sf libdril_dri.so "$root/usr/lib64/dri/kms_swrast_dri.so"

# the shared libraries they load: llvm among them
mkdir -p "$root/lib64"
for lib in $(ldd $libs 2>/dev/null | awk '/=> \//{print $3}' | sort -u); do
    cp -L "$lib" "$root/lib64/$(basename "$lib")"
done
