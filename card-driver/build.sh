#!/bin/sh
# Build the card-side sphcs module against a target kernel tree.
# Usage: KDIR=/path/to/kernel/build ION_STUB=1 ./build.sh
set -eu
KDIR="${KDIR:-/lib/modules/$(uname -r)/build}"
ION_STUB="${ION_STUB:-0}"
exec make KDIR="$KDIR" ION_STUB="$ION_STUB" "$@"