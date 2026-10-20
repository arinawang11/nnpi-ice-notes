#!/bin/sh
# NNP-I card image init template.
# Expects /lib/modules/sphcs.ko and (from --rootfs) a shell, nbd-client and
# optionally rawtun. Mounts the persistent root over NBD and switch_roots.
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH LC_ALL=C

mkdir -p /dev /proc /sys /tmp /run
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /dev/pts /run /tmp /mnt/root
mount -t devpts devpts /dev/pts 2>/dev/null

exec 0</dev/console
exec 1>/dev/console
exec 2>/dev/console

echo "nnpi-card: init"
hostname nnpi-card
ip link set lo up 2>/dev/null

insmod /lib/modules/sphcs.ko mem_thr=25000 disable_p2p=1 || insmod sphcs.ko mem_thr=25000 disable_p2p=1
i=0
while [ ! -e /sys/class/net/sphcschan0 ] && [ $i -lt 120 ]; do
    sleep 1
    i=$((i + 1))
done
ip link set sphcschan0 up 2>/dev/null

# Optional rescue listener (needs a static listener in the rootfs).
if [ -x /usr/bin/mintsh ]; then
    /usr/bin/mintsh &
fi

# Raw tunnel if the rootfs provides rawtun; otherwise TCP NBD to the host.
if [ -x /usr/bin/rawtun ]; then
    /usr/bin/rawtun 10809 sphcschan0 >/dev/null 2>&1 &
    sleep 1
    NBD_ARGS="127.0.0.1 10809"
else
    NBD_ARGS="192.168.115.1 10810"
fi

n=0
while [ $n -lt 300 ]; do
    if timeout 20 nbd-client -b 4096 $NBD_ARGS /dev/nbd0; then
        if mount -t ext4 -o rw /dev/nbd0 /mnt/root; then
            break
        fi
        nbd-client -d /dev/nbd0 2>/dev/null
    fi
    n=$((n + 1))
    echo "nnpi-card: nbd retry $n"
    sleep 3
done

if [ -f /mnt/root/etc/os-release ]; then
    echo "nnpi-card: switching root to $(head -1 /mnt/root/etc/os-release)"
    mkdir -p /mnt/root/var/log
    ( cat /dev/kmsg >> /mnt/root/var/log/nnpi-kmsg.log ) &
    mount --move /proc /mnt/root/proc
    mount --move /sys /mnt/root/sys
    mount --move /dev /mnt/root/dev
    exec switch_root /mnt/root /sbin/init
fi

echo "nnpi-card: root unavailable, rescue shell only"
while :; do sleep 3600; done