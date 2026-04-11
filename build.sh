#!/bin/bash
# Build libtolinom.so — NickelHook-based hook library for Tolino/Kobo v5
# Requires: arm-linux-gnueabihf-gcc, arm-linux-gnueabihf-g++, wget
#
# Two-pass build:
#   1. Compile with placeholder symbols, extract mangled names via nm
#   2. Patch placeholders with real names, rebuild final .so
set -e

# TOLINOM_PUBLIC=1 builds the variant shipped via the GitHub release: no
# reverse-tunnel feature. Unset (default) builds the personal variant that
# includes the tunnel. The flag is a C preprocessor define.
EXTRA_CXXFLAGS=""
if [ "${TOLINOM_PUBLIC:-0}" = "1" ]; then
    EXTRA_CXXFLAGS="-DTOLINOM_PUBLIC=1"
    echo "=== Building PUBLIC variant (no reverse tunnel) ==="
else
    echo "=== Building personal variant (reverse tunnel enabled) ==="
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKDIR=$(mktemp -d)
trap "rm -rf $WORKDIR" EXIT

cd "$WORKDIR"

# Copy source from repo
cp "$SCRIPT_DIR/tolinom.cc" .

# Download NickelHook
wget -q https://raw.githubusercontent.com/pgaskin/NickelHook/master/nh.c
wget -q https://raw.githubusercontent.com/pgaskin/NickelHook/master/NickelHook.h

# Patch NickelHook to accept STB_WEAK symbols (required for Qt6 firmware v5)
sed -i 's/ELFW(ST_BIND)(sym->st_info) == STB_GLOBAL/( ELFW(ST_BIND)(sym->st_info) == STB_GLOBAL || ELFW(ST_BIND)(sym->st_info) == STB_WEAK )/' nh.c

# Pass 1: compile to extract mangled symbol names
arm-linux-gnueabihf-gcc -c -fPIC -Os nh.c -o nh.o 2>/dev/null
arm-linux-gnueabihf-g++ -c -fPIC -Os $EXTRA_CXXFLAGS -Wno-unused-result -Wno-unused-variable -Wno-unused-local-typedefs tolinom.cc -o tolinom.o
arm-linux-gnueabihf-g++ -shared -o libtolinom.so tolinom.o nh.o -ldl

SETUP=$(nm -D libtolinom.so | grep nm_hook_setupUi | awk '{print $3}')
BETA=$(nm -D libtolinom.so | grep nm_hook_betaFeatures | awk '{print $3}')
SUSPEND=$(nm -D libtolinom.so | grep nm_hook_suspend | awk '{print $3}')
echo "SETUP=$SETUP BETA=$BETA SUSPEND=$SUSPEND"

if [ -z "$SETUP" ] || [ -z "$BETA" ] || [ -z "$SUSPEND" ]; then
    echo "ERROR: Could not find symbols"
    nm -D libtolinom.so | grep nm_hook
    exit 1
fi

# Pass 2: patch placeholders and rebuild
sed -i "s|PLACEHOLDER_SETUP|$SETUP|" tolinom.cc
sed -i "s|PLACEHOLDER_BETA|$BETA|" tolinom.cc
sed -i "s|PLACEHOLDER_SUSPEND|$SUSPEND|" tolinom.cc
rm -f *.o libtolinom.so

arm-linux-gnueabihf-gcc -c -fPIC -Os nh.c -o nh.o 2>/dev/null
arm-linux-gnueabihf-g++ -c -fPIC -Os $EXTRA_CXXFLAGS -Wno-unused-result -Wno-unused-variable -Wno-unused-local-typedefs tolinom.cc -o tolinom.o
arm-linux-gnueabihf-g++ -shared -o libtolinom.so tolinom.o nh.o -ldl
arm-linux-gnueabihf-strip libtolinom.so

# Output
ls -lh libtolinom.so
DEST="${1:-$SCRIPT_DIR}"
cp libtolinom.so "$DEST/libtolinom.so"
echo "Saved to $DEST/libtolinom.so"
