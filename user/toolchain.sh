#!/bin/sh
# the host's gcc, with what it needs to compile and link a C program for the glibc already in the initrd.
# copied to the same paths under $1, because gcc finds its parts relative to where it is
set -e

root=$1

cc1=$(gcc -print-prog-name=cc1)
libexec=$(dirname "$cc1")
libgcc=$(dirname "$(gcc -print-libgcc-file-name)")

copy() {
    for f in "$@"; do
        mkdir -p "$root$(dirname "$f")"
        cp -L "$f" "$root$f"
    done
}

progs="/usr/bin/gcc /usr/bin/make /usr/bin/as /usr/bin/ld.bfd $cc1 $libexec/collect2 $libexec/liblto_plugin.so $libexec/lto-wrapper"
copy $progs

mkdir -p "$root/usr/bin" "$root/lib64"
ln -sf ld.bfd "$root/usr/bin/ld"
ln -sf gcc "$root/usr/bin/cc"
ln -sf make "$root/usr/bin/gmake"

# the shared libraries they load, next to the ones the programs already have
for lib in $(ldd $progs 2>/dev/null | awk '/=> \//{print $3}' | sort -u); do
    cp -L "$lib" "$root/lib64/$(basename "$lib")"
done

# gcc's own start files and libraries, and its headers
for f in crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o libgcc.a libgcc_eh.a libgcc_s.so libgcc_s_asneeded.so libatomic.so libatomic_asneeded.so libatomic.a; do
    copy "$libgcc/$f"
done

mkdir -p "$root$libgcc"
cp -r "$libgcc/include" "$root$libgcc/"

# glibc's headers and start files, and the linux headers they include: every file of the two packages there
files=$(rpm -ql glibc-devel kernel-headers | grep -E '^/usr/(include/|lib64/(crt1|crti|crtn|Scrt1)\.o$|lib64/(libc\.so|libc_nonshared\.a|libm\.so)$)')

for f in $files; do
    [ -f "$f" ] && copy "$f"
done

# libatomic.so is a script naming the library by its full name
copy /usr/lib64/libatomic.so.1.2.0
ln -sf libatomic.so.1.2.0 "$root/usr/lib64/libatomic.so.1"

mkdir -p "$root/tmp"
