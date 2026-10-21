#!/bin/sh
# VHDX command mailbox. Block 4194064 holds "NNPIREQ <seq>" plus the command
# text; block 4194065 returns "NNPIRES <seq> ... rc=<rc>" plus the output tail.
# The host writes and reads the attached disk directly, so the mailbox needs
# no extra transport and does not share the NBD data path.
#
# Block numbers are relative to the 16 GiB store used here (4 KiB blocks);
# they sit in the last MiB, outside the ext4 filesystem.
TAIL=$(cat /run/nnpi-store-dev 2>/dev/null || echo /dev/nbd0)
CTL_BLK=4194064
RES_BLK=4194065
LAST=/run/nnpi-mailbox.last
[ -f $LAST ] || echo 0 > $LAST
while :; do
    # O_DIRECT bypasses the block-device page cache; fall back to flushing it
    # when the NBD device rejects direct reads.
    if ! dd if=$TAIL bs=4096 skip=$CTL_BLK count=1 iflag=direct of=/tmp/ctl.raw 2>/dev/null; then
        blockdev --flushbufs $TAIL 2>/dev/null
        dd if=$TAIL bs=4096 skip=$CTL_BLK count=1 of=/tmp/ctl.raw 2>/dev/null
    fi
    tr -d '\000' < /tmp/ctl.raw > /tmp/ctl
    first=$(head -n 1 /tmp/ctl)
    case "$first" in
        NNPIREQ\ *)
            seq=${first#NNPIREQ }
            last=$(cat $LAST 2>/dev/null)
            if [ -n "$seq" ] && [ "$seq" != "0" ] && [ "$seq" != "$last" ]; then
                sed -n '2,$p' /tmp/ctl > /tmp/cmd.sh
                {
                    printf 'NNPIRES %s start %s\n' "$seq" "$(date -u +%FT%TZ)"
                    timeout 1800 sh /tmp/cmd.sh 2>&1
                    printf 'NNPIRES %s rc=%s end %s\n' "$seq" "$?" "$(date -u +%FT%TZ)"
                } > /tmp/res 2>&1
                tail -c 4096 /tmp/res > /tmp/res.t
                dd if=/tmp/res.t of=$TAIL bs=4096 seek=$RES_BLK conv=sync,fsync 2>/dev/null
                echo "$seq" > $LAST
                printf 'NNPIDONE %s\n' "$seq" > /tmp/ctl.done
                dd if=/tmp/ctl.done of=$TAIL bs=4096 seek=$CTL_BLK conv=sync,fsync 2>/dev/null
            fi
            ;;
    esac
    sleep 2
done
