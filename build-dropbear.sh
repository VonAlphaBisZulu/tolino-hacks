#!/bin/bash
# Cross-compile Dropbear SSH for ARM
# Requires: arm-linux-gnueabihf-gcc
#
# Builds with password auth enabled (dynamically linked against device libc).
# If authorized_keys exists on device, key auth is preferred.
set -e

VERSION="2024.86"
URL="https://matt.ucc.asn.au/dropbear/releases/dropbear-${VERSION}.tar.bz2"

echo "=== Building Dropbear ${VERSION} for ARM ==="

WORKDIR=$(mktemp -d)
trap "rm -rf $WORKDIR" EXIT
cd "$WORKDIR"

wget -q "$URL"
tar xjf "dropbear-${VERSION}.tar.bz2"
cd "dropbear-${VERSION}"

./configure --host=arm-linux-gnueabihf --disable-zlib --disable-lastlog \
    --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
    CC=arm-linux-gnueabihf-gcc CFLAGS="-Os" \
    2>&1 | tail -3

make PROGRAMS="dropbear dbclient dropbearkey scp" MULTI=1 -j$(nproc) \
    2>&1 | tail -3

arm-linux-gnueabihf-strip dropbearmulti

# Output
if [ -n "$1" ]; then
    cp dropbearmulti "$1"
    echo "Saved to $1"
else
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    cp dropbearmulti "$SCRIPT_DIR/dropbearmulti"
    echo "Saved to $SCRIPT_DIR/dropbearmulti"
fi

ls -lh dropbearmulti
echo "=== Done ==="
