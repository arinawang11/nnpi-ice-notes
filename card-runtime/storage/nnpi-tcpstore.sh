#!/bin/bash
# Attach the VHDX over TCP NBD once the host's net-bridge is reachable.
# The raw NBD path (nbd0) remains the boot default; this service only takes
# over after the host switches channels, which unmounts /mnt/vhdx first.
#
# nbd-client runs with -n (no fork) and stays as a child of this script:
# systemd kills the whole cgroup when the main process exits, which would
# otherwise pull the device out from under the mounted filesystem.
HOST=192.168.115.1
PORT=10810
DEV=/dev/nbd1
MNT=/mnt/vhdx
NBD_PID=

attach() {
    nbd-client -n -N default -b 4096 -L "$HOST" "$PORT" "$DEV" &
    NBD_PID=$!
    i=0
    while [ "$i" -lt 40 ]; do
        if nbd-client -c "$DEV" >/dev/null 2>&1; then
            return 0
        fi
        kill -0 "$NBD_PID" 2>/dev/null || { NBD_PID=; return 1; }
        sleep 0.5
        i=$((i + 1))
    done
    return 1
}

while :; do
    # Probe the port only while disconnected. The host server serves one
    # connection at a time, so probing with an established session times out
    # and would skip the mount step forever.
    if ! nbd-client -c "$DEV" >/dev/null 2>&1; then
        if ! timeout 2 bash -c "echo >/dev/tcp/$HOST/$PORT" 2>/dev/null; then
            sleep 5
            continue
        fi
        if ! attach; then
            [ -n "$NBD_PID" ] && kill "$NBD_PID" 2>/dev/null
            NBD_PID=
            sleep 5
            continue
        fi
    fi

    src=$(awk -v m="$MNT" '$2 == m { print $1; exit }' /proc/mounts)
    if [ -z "$src" ]; then
        if ! mount -t ext4 "$DEV" "$MNT"; then
            sleep 5
            continue
        fi
        src=$DEV
    fi
    if [ "$src" != "$DEV" ]; then
        echo "$MNT is mounted from $src; waiting for an operator to unmount it"
        sleep 10
        continue
    fi

    echo "$DEV" > /run/nnpi-store-dev
    # restart, not start: both units may already be running against the raw
    # device from boot, and their TAIL is resolved once at process start.
    systemctl restart --no-block nnpi-mailbox.service nnpi-wd.service 2>/dev/null
    systemctl start mint-sync.timer 2>/dev/null
    echo "attached $DEV at $MNT"

    if [ -n "$NBD_PID" ]; then
        wait "$NBD_PID"
        echo "nbd-client exited"
        exit 1
    fi
    sleep infinity
done
