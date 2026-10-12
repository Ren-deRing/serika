#!/bin/sh
# the compositor and clients from the host, and the libraries they load: all of it under usr, which is the disk
set -e

root=$1

mkdir -p "$root/usr/bin" "$root/usr/lib64"

# what libinput, xkbcommon and the cursor look for under /usr/share: the device quirks, the keymaps (xkb is
# at /usr/share/X11/xkb through a link) and the cursors of the default theme
mkdir -p "$root/usr/share/X11" "$root/usr/share/icons/default" "$root/usr/share/icons/Adwaita"
cp -r /usr/share/libinput "$root/usr/share/"
cp -r /usr/share/xkeyboard-config-2 "$root/usr/share/"
ln -sfn ../xkeyboard-config-2 "$root/usr/share/X11/xkb"
cp -r /usr/share/X11/locale "$root/usr/share/X11/" # the compose tables, which libxkbcommon finds by the locale's name
mkdir -p "$root/usr/lib/locale"
cp -r /usr/lib/locale/C.utf8 "$root/usr/lib/locale/" # foot refuses to run in a locale that is not utf-8
cp /usr/share/icons/default/index.theme "$root/usr/share/icons/default/"
cp /usr/share/icons/Adwaita/index.theme "$root/usr/share/icons/Adwaita/"
cp -r /usr/share/icons/Adwaita/cursors "$root/usr/share/icons/Adwaita/"

# a client to try it with: it lists what the compositor offers
cp -L /usr/bin/wayland-info "$root/usr/bin/wayland-info"

# and to see what libudev and libinput make of the devices
mkdir -p "$root/usr/libexec/libinput"
cp -L /usr/bin/libinput /usr/bin/udevadm "$root/usr/bin/"
cp -L /usr/libexec/libinput/libinput-list-devices /usr/libexec/libinput/libinput-debug-events "$root/usr/libexec/libinput/"

# labwc, a compositor that is not ours (wlroots 0.19, which is what the host has for it). its defaults are in the binary;
# what it draws text with is pango, so fontconfig needs a configuration and a font
cp -L /usr/bin/labwc /usr/bin/labnag /usr/bin/Xwayland "$root/usr/bin/" # labwc quits if it has no Xwayland to start for X clients
mkdir -p "$root/usr/share/fontconfig" "$root/usr/share/fonts/dejavu"
cp /usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf /usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf "$root/usr/share/fonts/dejavu/"
cat > "$root/usr/share/fontconfig/fonts.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
    <dir>/usr/share/fonts</dir>
    <cachedir>/tmp/fontconfig</cachedir>
</fontconfig>
EOF

# foot, a terminal: its configuration (found through XDG_CONFIG_DIRS), the terminfo it names itself with, a monospace font
# and the script labwc starts a terminal with (the host's looks for one and starts with #!/usr/bin/sh, which is not here)
cp -L /usr/bin/foot "$root/usr/bin/"
printf '#!/bin/sh\nexec foot "$@"\n' > "$root/usr/bin/lab-sensible-terminal"
chmod +x "$root/usr/bin/lab-sensible-terminal"
mkdir -p "$root/usr/etc/xdg/foot" "$root/usr/share/terminfo/f" "$root/usr/share/terminfo/x"
cat > "$root/usr/etc/xdg/foot/foot.ini" <<'EOF'
shell=/bin/sh
font=DejaVu Sans Mono:size=12
pad=4x4
EOF
cp -L /usr/share/terminfo/f/foot /usr/share/terminfo/f/foot+base /usr/share/terminfo/f/foot-direct "$root/usr/share/terminfo/f/"
cp -L /usr/share/terminfo/x/xterm-256color "$root/usr/share/terminfo/x/"
cp /usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf /usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono-Bold.ttf "$root/usr/share/fonts/dejavu/"

# what they need, except what the initrd has
for lib in $(ldd "$root/usr/bin/wayland-info" "$root/usr/bin/libinput" "$root/usr/bin/udevadm" "$root/usr/bin/labwc" "$root/usr/bin/labnag" "$root/usr/bin/Xwayland" "$root/usr/bin/foot" \
                 "$root/usr/libexec/libinput/libinput-list-devices" "$root/usr/libexec/libinput/libinput-debug-events" 2>/dev/null | awk '/=> \//{print $3}' | sort -u); do
    case $(basename "$lib") in
        ld-linux-x86-64.so.2|libc.so.6|libm.so.6|libgcc_s.so.1) continue ;;
    esac

    [ -e "$root/usr/lib64/$(basename "$lib")" ] || cp -L "$lib" "$root/usr/lib64/$(basename "$lib")"
done
