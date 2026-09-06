#!/usr/bin/env bash
# Puts Sashfold on a Linux desktop for the current user: the icon into the
# hicolor theme, a launcher into the applications menu (pointing at the
# built binary, or at `sashfold` on PATH with --path), and optionally a
# shortcut on the Desktop. Nothing needs root; nothing leaves ~/.local
# except the Desktop shortcut you ask for.
#
#   bash tools/install-desktop-linux.sh [build-gcc/sashfold] [--desktop] [--path]
#
# The window's app_id is "sashfold", which is how the desktop matches a
# running window to this launcher's name and icon.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
binary="$here/build-gcc/sashfold"
want_desktop=0
use_path=0
for arg in "$@"; do
    case "$arg" in
        --desktop) want_desktop=1 ;;
        --path) use_path=1 ;;
        *) binary="$(cd "$(dirname "$arg")" && pwd)/$(basename "$arg")" ;;
    esac
done
if [ "$use_path" -eq 0 ] && [ ! -x "$binary" ]; then
    echo "install-desktop-linux: no executable at $binary (build first, or pass its path)" >&2
    exit 1
fi

data="${XDG_DATA_HOME:-$HOME/.local/share}"
icons="$data/icons/hicolor"
apps="$data/applications"
mkdir -p "$apps"
source_icon="$here/assets/icon.png"

# The icon at every size the theme spec names, squared on a transparent
# ground; without ImageMagick the largest size gets the original and the
# desktop scales it.
sizes="16 22 24 32 48 64 128 256 512"
if command -v magick > /dev/null 2>&1; then
    for size in $sizes; do
        mkdir -p "$icons/${size}x${size}/apps"
        magick "$source_icon" -background none -gravity center -extent "$(magick "$source_icon" -format '%[fx:max(w,h)]x%[fx:max(w,h)]' info:)" \
            -resize "${size}x${size}" "$icons/${size}x${size}/apps/sashfold.png"
    done
    echo "icon: $(echo $sizes | wc -w) sizes under $icons"
else
    mkdir -p "$icons/512x512/apps"
    cp "$source_icon" "$icons/512x512/apps/sashfold.png"
    echo "icon: assets/icon.png copied to $icons/512x512/apps (install ImageMagick for every size)"
fi

if [ "$use_path" -eq 1 ]; then
    exec_line="sashfold"
else
    exec_line="$binary"
fi
# The launcher, with the executable's path filled in.
sed -e "s|^Exec=sashfold %u|Exec=$exec_line %u|" -e "s|^Exec=sashfold\$|Exec=$exec_line|" \
    "$here/packaging/linux/sashfold.desktop" > "$apps/sashfold.desktop"
chmod +x "$apps/sashfold.desktop"
echo "launcher: $apps/sashfold.desktop -> $exec_line"

if [ "$want_desktop" -eq 1 ]; then
    desktop_dir="$(xdg-user-dir DESKTOP 2> /dev/null || echo "$HOME/Desktop")"
    mkdir -p "$desktop_dir"
    cp "$apps/sashfold.desktop" "$desktop_dir/Sashfold.desktop"
    chmod +x "$desktop_dir/Sashfold.desktop"
    echo "shortcut: $desktop_dir/Sashfold.desktop"
fi

# Tell the desktop.
command -v update-desktop-database > /dev/null 2>&1 && update-desktop-database "$apps" 2> /dev/null || true
command -v gtk-update-icon-cache > /dev/null 2>&1 && gtk-update-icon-cache -q -t "$icons" 2> /dev/null || true
command -v kbuildsycoca6 > /dev/null 2>&1 && kbuildsycoca6 > /dev/null 2>&1 || true
echo "done: Sashfold is in the applications menu"
