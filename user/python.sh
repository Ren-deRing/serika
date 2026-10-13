#!/bin/sh
# the host's CPython, to run on serika: the program, libpython, the standard library with its bytecode, and the extension modules
# with the libraries they load that the initrd does not have. all under /usr: that is the disk. python finds its library from where
# its program is (/usr/bin/python3 -> /usr/lib64/python3.N), the same paths as on the host
set -e

root=$1

exe=$(readlink -f "$(command -v python3)")
name=$(basename "$exe")
version=${name#python}
std=$(python3 -c 'import sysconfig; print(sysconfig.get_path("stdlib"))')
dynload=$std/lib-dynload

mkdir -p "$root/usr/bin" "$root/usr/lib64"

cp -L "$exe" "$root/usr/bin/$name"
ln -sf "$name" "$root/usr/bin/python3"
ln -sf "$name" "$root/usr/bin/python"

cp -L "/usr/lib64/libpython$version.so.1.0" "$root/usr/lib64/"

# the library: sources and the bytecode that goes with them (without it every import is compiled again, on serika). not the
# optimized bytecode, the tests, ensurepip, or tkinter, idle and turtle, which need tk and X
dest=$root$std
mkdir -p "$dest"

tar -C "$std" -cf - \
    --exclude='*.opt-1.pyc' --exclude='*.opt-2.pyc' \
    --exclude='./test' --exclude='./idlelib' --exclude='./tkinter' --exclude='./turtledemo' --exclude='./turtle.py' \
    --exclude='./ensurepip' --exclude='./site-packages' --exclude='./lib-dynload/_tkinter*' \
    . | tar -C "$dest" -xf -

mkdir -p "$dest/site-packages"

# the shared libraries they load, except the ones the initrd has and the one that was just copied
mods=$(ls "$dynload"/*.so | grep -v '/_tkinter')

for lib in $(ldd "$exe" $mods 2>/dev/null | awk '/=> \//{print $3}' | sort -u); do
    case $(basename "$lib") in
        ld-linux-x86-64.so.2|libc.so.6|libm.so.6|libgcc_s.so.1|libpython*) continue ;;
    esac

    cp -L "$lib" "$root/usr/lib64/$(basename "$lib")"
done
