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

# Ensure cross-compile libcrypt is available (needed for password auth)
EXTRA_CFLAGS=""
EXTRA_LDFLAGS=""
echo '#include <crypt.h>' > test_crypt.c
echo 'int main(){ crypt("",""); return 0; }' >> test_crypt.c
if ! arm-linux-gnueabihf-gcc -static test_crypt.c -lcrypt -o /dev/null 2>/dev/null; then
    echo "Fetching armhf libcrypt..."
    SYSROOT="$WORKDIR/armhf-sysroot"
    mkdir -p "$SYSROOT"
    # Download armhf packages directly from Ubuntu ports (avoids multiarch)
    # Try multiple versions (22.04=4.4.27-1, 24.04=4.4.36-4)
    for V in 4.4.27-1 4.4.33-2 4.4.36-4; do
        [ -f crypt-dev.deb ] || wget -q "http://ports.ubuntu.com/pool/main/libx/libxcrypt/libcrypt-dev_${V}_armhf.deb" -O crypt-dev.deb 2>/dev/null || rm -f crypt-dev.deb
        [ -f crypt1.deb ] || wget -q "http://ports.ubuntu.com/pool/main/libx/libxcrypt/libcrypt1_${V}_armhf.deb" -O crypt1.deb 2>/dev/null || rm -f crypt1.deb
    done
    dpkg -x crypt-dev.deb "$SYSROOT/" 2>/dev/null || true
    dpkg -x crypt1.deb "$SYSROOT/" 2>/dev/null || true
    # Point compiler at the extracted headers and libraries
    if [ -d "$SYSROOT/usr/include" ]; then
        EXTRA_CFLAGS="-I$SYSROOT/usr/include"
        EXTRA_LDFLAGS="-L$SYSROOT/usr/lib/arm-linux-gnueabihf"
    fi
fi
rm -f test_crypt.c

# Final check: if we still can't link crypt, disable password auth
HAS_PASSWD=1
echo '#include <crypt.h>' > test_crypt.c
echo 'int main(){ crypt("",""); return 0; }' >> test_crypt.c
if ! arm-linux-gnueabihf-gcc -static $EXTRA_CFLAGS $EXTRA_LDFLAGS test_crypt.c -lcrypt -o /dev/null 2>/dev/null; then
    echo "WARNING: libcrypt unavailable — password auth disabled (key-only)"
    HAS_PASSWD=0
fi
rm -f test_crypt.c

wget -q "$URL"
tar xjf "dropbear-${VERSION}.tar.bz2"
cd "dropbear-${VERSION}"

if [ "$HAS_PASSWD" = "0" ]; then
    cat > localoptions.h << 'EOF'
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_CLI_PASSWORD_AUTH 0
EOF
fi

./configure --host=arm-linux-gnueabihf --disable-zlib --disable-lastlog \
    --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
    CC=arm-linux-gnueabihf-gcc LDFLAGS="-static $EXTRA_LDFLAGS" CFLAGS="-Os $EXTRA_CFLAGS" \
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
[ "$HAS_PASSWD" = "1" ] && echo "=== Done (password auth ENABLED) ===" || echo "=== Done (key-only, no password auth) ==="
