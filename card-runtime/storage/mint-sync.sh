#!/bin/sh
# Persist the RAM root back to the VHDX mounted at /mnt/vhdx.
# -A is omitted: this kernel does not support POSIX ACLs on ext4.
#
# No --delete: /mnt/vhdx doubles as card-side storage, so a file that exists
# only there must never be erased by the writeback. Deleting a file in RAM
# therefore does not delete it from the VHDX; remove it from the mount too.
#
# Large immutable image trees are excluded so one pass stays short over the
# slow NBD link. They are supplied by the boot image, not by this mirror.
mountpoint -q /mnt/vhdx || exit 0
exec 9>/run/mint-sync.lock
flock -n 9 || exit 0
rsync -aHX \
  --exclude=/proc --exclude=/sys --exclude=/dev --exclude=/run \
  --exclude=/tmp --exclude=/mnt --exclude=/media --exclude=/lost+found \
  --exclude=/usr/share/doc --exclude=/usr/share/man --exclude=/usr/share/gnome \
  --exclude=/usr/share/locale --exclude=/usr/share/icons --exclude=/usr/share/fonts \
  --exclude=/var/cache/apt --exclude=/root/.cache \
  / /mnt/vhdx/ >>/var/log/mint-sync.log 2>&1
rc=$?
# Flush only this filesystem: a global sync can block on the dead raw NBD.
# The rsync status is what systemd should see, so report it after the flush.
timeout 600 sync -f /mnt/vhdx
exit $rc
