#!/bin/bash
# Cross-compile Dropbear SSH for ARM (static, ~719KB)
# Requires: arm-linux-gnueabihf-gcc
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

# Key-based auth only (no password auth, avoids crypt() dependency)
cat > localoptions.h << 'EOF'
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_CLI_PASSWORD_AUTH 0
EOF

./configure --host=arm-linux-gnueabihf --disable-zlib --disable-lastlog \
    --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
    CC=arm-linux-gnueabihf-gcc LDFLAGS="-static" CFLAGS="-Os" \
    2>&1 | tail -3

make PROGRAMS="dropbear dbclient dropbearkey scp" MULTI=1 STATIC=1 -j$(nproc) \
    2>&1 | tail -3

arm-linux-gnueabihf-strip dropbearmulti

DEST="${OLDPWD:-$(pwd)}"
# Try to copy back to the original working directory
if [ -n "$1" ]; then
    cp dropbearmulti "$1"
    echo "Saved to $1"
else
    cp dropbearmulti "$DEST/../dropbearmulti" 2>/dev/null || cp dropbearmulti /tmp/dropbearmulti
    echo "Saved to $DEST/../dropbearmulti (or /tmp/dropbearmulti)"
fi

ls -lh dropbearmulti
echo "=== Done ==="
