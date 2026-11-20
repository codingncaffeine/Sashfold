#!/bin/bash
# Reads the browser's accessibility tree over a real AT-SPI2 registry, with
# no window and never on the desktop's buses. Run it on a session bus of its
# own:
#   XDG_RUNTIME_DIR=<empty dir> dbus-run-session -- tools/atspi-session.sh \
#       <sashfold binary> <script.txt> <work dir> [tools/atspi-reader.py options]
# It starts an accessibility bus (dbus-daemon with at-spi2-core's
# accessibility.conf, on an abstract socket) and the registry on it, runs the
# browser with --script and its own profile in the work dir, and runs the
# reader (the system's libatspi, through Python's gi) against it. The script
# opens the bridge with `accessibility-serve <ms> <file>`, naming
# <work dir>/stop-serving as the file: the reader creates it when it is done
# (and <work dir>/go-1, with --go-file, once its event listeners are in).
# Every process started here is stopped by its own PID.
set -u
case "${DBUS_SESSION_BUS_ADDRESS:-}" in
    "" | */run/user/*) echo "atspi-session: run it under dbus-run-session, never on the desktop's bus" >&2; exit 99 ;;
esac
browser_binary=$(realpath "$1")
script=$(realpath "$2")
work=$(realpath "$3")
shift 3
here=$(dirname "$(realpath "$0")")
mkdir -p "$work/profile"
exec 7> "$work/a11y-address"
dbus-daemon --config-file=/usr/share/defaults/at-spi2/accessibility.conf --address="unix:abstract=sashfold-atspi-$$-$RANDOM" \
    --nofork --print-address=7 > "$work/a11y-bus.log" 2>&1 &
bus=$!
address=""
for _ in $(seq 1 50); do
    address=$(head -1 "$work/a11y-address" 2>/dev/null)
    [ -n "$address" ] && break
    sleep 0.1
done
if [ -z "$address" ]; then
    kill $bus; wait $bus
    echo "atspi-session: the accessibility bus did not start" >&2
    exit 2
fi
export AT_SPI_BUS_ADDRESS=$address
/usr/lib/at-spi2-registryd > "$work/registry.log" 2>&1 &
registry=$!
for _ in $(seq 1 50); do
    busctl --address="$address" status org.a11y.atspi.Registry > /dev/null 2>&1 && break
    sleep 0.1
done
rm -f "$work/stop-serving" "$work/go-1"
timeout 300 "$browser_binary" --script "$script" --profile "$work/profile" > "$work/browser.log" 2>&1 &
browser=$!
python3 -I "$here/atspi-reader.py" --stop-file "$work/stop-serving" "$@"
status=$?
touch "$work/stop-serving"
wait $browser
echo "browser exit: $?"
kill $registry 2>/dev/null; wait $registry 2>/dev/null
kill $bus 2>/dev/null; wait $bus 2>/dev/null
exit $status
