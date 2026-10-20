#!/bin/sh
# Sustained NBD read test for the card module (builder example).
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH LC_ALL=C

mkdir -p /dev /proc /sys /tmp /run
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /dev/pts /run /tmp
mount -t devpts devpts /dev/pts 2>/dev/null

exec 0</dev/console
exec 1>/dev/console
exec 2>/dev/console

echo "nnpi-stress: init"
hostname nnpi-stress
ip link set lo up 2>/dev/null

insmod /lib/modules/sphcs.ko mem_thr=25000 disable_p2p=1
echo "nnpi-stress: insmod rc=$?"
i=0
while [ ! -e /sys/class/net/sphcschan0 ] && [ $i -lt 300 ]; do
    sleep 1
    i=$((i + 1))
done
ip link set sphcschan0 up 2>/dev/null
echo "nnpi-stress: net up after ${i}s"

/usr/bin/rawtun 10809 sphcschan0 >/dev/null 2>&1 &
sleep 1

n=0
while [ $n -lt 300 ]; do
    if timeout 20 nbd-client -b 4096 127.0.0.1 10809 /dev/nbd0; then
        break
    fi
    n=$((n + 1))
    echo "nnpi-stress: nbd retry $n"
    sleep 3
done

if [ ! -b /dev/nbd0 ]; then
    echo "nnpi-stress: no nbd device"
    while :; do sleep 3600; done
fi

echo "nnpi-stress: reading 15 GiB"
dd if=/dev/nbd0 of=/dev/null bs=1M count=15360 2>/tmp/dd.log
rc=$?
tail -2 /tmp/dd.log
echo "nnpi-stress: dd rc=$rc"

printf "NNPISTRESS DONE rc=%s\n" "$rc" | dd of=/dev/nbd0 bs=1 seek=17178820608 conv=fsync 2>/dev/null
echo "nnpi-stress: marker written"

while :; do sleep 3600; done