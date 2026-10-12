#!/bin/sh
# the host's clang and lld, with what they need to compile and link a C program for the glibc already in the initrd.
# copied to the same paths under $1, because clang finds its parts relative to where it is
set -e

root=$1

resource=$(clang -print-resource-dir)
libgcc=$(dirname "$(gcc -print-libgcc-file-name)")

copy() {
    for f in "$@"; do
        mkdir -p "$root$(dirname "$f")"
        cp -L "$f" "$root$f"
    done
}

# the programs. binutils is for ar and the rest: lld links
progs="/usr/bin/clang-22 /usr/bin/lld /usr/bin/make /usr/bin/ar /usr/bin/ranlib /usr/bin/nm /usr/bin/objcopy /usr/bin/strip"
copy $progs

mkdir -p "$root/usr/bin" "$root/lib64"
ln -sf clang-22 "$root/usr/bin/clang"
ln -sf clang-22 "$root/usr/bin/gcc"
ln -sf clang-22 "$root/usr/bin/cc"
ln -sf lld "$root/usr/bin/ld.lld"
ln -sf lld "$root/usr/bin/ld"
ln -sf make "$root/usr/bin/gmake"

# the shared libraries they load, next to the ones the programs already have
for lib in $(ldd $progs 2>/dev/null | awk '/=> \//{print $3}' | sort -u); do
    cp -L "$lib" "$root/lib64/$(basename "$lib")"
done

# libm.so is a script that asks for this one if it is needed
cp -L /lib64/libmvec.so.1 "$root/lib64/"

# clang's own headers (stddef.h and so on)
mkdir -p "$root$resource"
cp -r "$resource/include" "$root$resource/"

# clang takes gcc's start files and runtime from where gcc is installed, and does not need gcc itself
for f in crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o libgcc.a libgcc_eh.a libgcc_s.so libgcc_s_asneeded.so; do
    copy "$libgcc/$f"
done

# glibc's headers and start files, and the linux headers they include: every file of the two packages there
files=$(rpm -ql glibc-devel kernel-headers | grep -E '^/usr/(include/|lib64/(crt1|crti|crtn|Scrt1)\.o$|lib64/(libc\.so|libc_nonshared\.a|libm\.so)$|lib64/(libpthread|libdl|librt|libutil|libanl)\.a$)')

for f in $files; do
    [ -f "$f" ] && copy "$f"
done

mkdir -p "$root/tmp"
