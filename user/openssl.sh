#!/bin/sh
# the host's openssl, which busybox's wget runs to speak https ("openssl s_client"), with the libraries it loads that the initrd
# does not have. all of it is under /usr: that is the disk. the libraries go in /usr/lib64, which ld.so searches after /lib64
set -e

root=$1

mkdir -p "$root/usr/bin" "$root/usr/lib64"
cp -L /usr/bin/openssl "$root/usr/bin/openssl"

for lib in $(ldd /usr/bin/openssl 2>/dev/null | awk '/=> \//{print $3}' | sort -u); do
    case $(basename "$lib") in
        ld-linux-x86-64.so.2|libc.so.6|libm.so.6|libgcc_s.so.1) continue ;;
    esac

    cp -L "$lib" "$root/usr/lib64/$(basename "$lib")"
done
