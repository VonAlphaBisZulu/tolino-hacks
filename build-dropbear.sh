#!/bin/bash
# Cross-compile Dropbear SSH for ARM (static, with password auth)
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

# Check if cross-compile libcrypt is available for static password auth
CROSS_SYSROOT=$(arm-linux-gnueabihf-gcc -print-sysroot 2>/dev/null || echo "")
HAS_LIBCRYPT=0
if [ -n "$CROSS_SYSROOT" ]; then
    find "$CROSS_SYSROOT" -name 'libcrypt.a' 2>/dev/null | grep -q . && HAS_LIBCRYPT=1
fi
# Also check the linker directly
if [ "$HAS_LIBCRYPT" = "0" ]; then
    echo 'int main(){}' > /tmp/test_crypt.c
    arm-linux-gnueabihf-gcc -static /tmp/test_crypt.c -lcrypt -o /dev/null 2>/dev/null && HAS_LIBCRYPT=1
    rm -f /tmp/test_crypt.c
fi

if [ "$HAS_LIBCRYPT" = "1" ]; then
    echo "libcrypt found — building with password auth"
else
    echo "libcrypt NOT found — disabling password auth (key-only)"
    cat > localoptions.h << 'EOF'
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_CLI_PASSWORD_AUTH 0
EOF
fi

./configure --host=arm-linux-gnueabihf --disable-zlib --disable-lastlog \
    --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
    CC=arm-linux-gnueabihf-gcc LDFLAGS="-static" CFLAGS="-Os" \
    2>&1 | tail -3

make PROGRAMS="dropbear dbclient dropbearkey scp" MULTI=1 STATIC=1 -j$(nproc) \
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
