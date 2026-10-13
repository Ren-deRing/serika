#!/bin/sh
# glmark2, the opengl es 2 benchmark, built for wayland from the source in deps/ (fetched here if it is not there yet).
# it draws with whatever the compositor's clients draw with: llvmpipe. its scenes (textures, models, shaders) are
# under /usr/share/glmark2, which is the disk. what it loads is added by wayland.sh's loop at the end: this is after it
set -e

root=$1
# make has the kernel's compiler and flags in the environment: this is a program for the host's glibc
unset CC LD CFLAGS CPPFLAGS LDFLAGS

src=${DEPS_DIR:?DEPS_DIR is made by the top Makefile}/glmark2
build=${BUILD_DIR:?BUILD_DIR is made by the top Makefile}/glmark2

[ -d "$src" ] || git clone -q --depth 1 https://github.com/glmark2/glmark2 "$src"

# a directory a failed setup left is no build: it starts again
[ -e "$build/build.ninja" ] || rm -rf "$build"
[ -d "$build" ] || meson setup "$build" "$src" -Dflavors=wayland-glesv2 --prefix=/usr -Ddata-path=/usr/share/glmark2 >/dev/null

ninja -C "$build" >/dev/null
DESTDIR=$root ninja -C "$build" install >/dev/null

# no manual on the disk
rm -rf "$root/usr/share/man"

# the libraries it needs that the disk and the initrd do not have yet
for lib in $(ldd "$root/usr/bin/glmark2-es2-wayland" | awk '/=> \//{print $3}'); do
    case $(basename "$lib") in
        ld-linux-x86-64.so.2|libc.so.6|libm.so.6|libgcc_s.so.1) continue ;;
    esac

    [ -e "$root/usr/lib64/$(basename "$lib")" ] || cp -L "$lib" "$root/usr/lib64/$(basename "$lib")"
done
