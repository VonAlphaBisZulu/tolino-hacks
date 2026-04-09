#!/bin/bash
# Package everything into an update.tar for one-shot installation.
# Run this after building libtolinom.so with build.sh.
#
# Usage: ./package-update.sh [path-to-ssh-pubkey]
#
# The update.tar goes on the device root.
# Pre-stage .adds/ files on USB before applying the update.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Tolino Hacks Packaging ==="

# Check for required files
if [ ! -f "$SCRIPT_DIR/libtolinom.so" ]; then
    echo "ERROR: libtolinom.so not found. Run build.sh first."
    exit 1
fi

if [ ! -f "$SCRIPT_DIR/dropbearmulti" ]; then
    echo "WARNING: dropbearmulti not found."
    echo "  Build it with: ./build-dropbear.sh"
    echo "  Or download a pre-built ARM static binary."
fi

# SSH public key
SSH_PUBKEY=""
if [ -n "$1" ]; then
    SSH_PUBKEY="$1"
elif [ -f ~/.ssh/id_rsa.pub ]; then
    SSH_PUBKEY=~/.ssh/id_rsa.pub
elif [ -f ~/.ssh/id_ed25519.pub ]; then
    SSH_PUBKEY=~/.ssh/id_ed25519.pub
fi

if [ -n "$SSH_PUBKEY" ] && [ -f "$SSH_PUBKEY" ]; then
    echo "Using SSH key: $SSH_PUBKEY"
else
    echo "WARNING: No SSH public key found."
    echo "  SSH login won't work without authorized_keys."
    echo "  Pass your pubkey path as argument: ./package-update.sh ~/.ssh/id_rsa.pub"
fi

# Create staging directory
STAGING=$(mktemp -d)
trap "rm -rf $STAGING" EXIT

# 1. Create update.tar with driver.sh
cp "$SCRIPT_DIR/driver.sh" "$STAGING/driver.sh"
(cd "$STAGING" && tar cf "$SCRIPT_DIR/update.tar" driver.sh)
echo "Created update.tar"

# 2. Prepare .adds/ directory (user copies this to device via USB)
ADDS="$STAGING/adds"
mkdir -p "$ADDS"

cp "$SCRIPT_DIR/libtolinom.so" "$ADDS/"
[ -f "$SCRIPT_DIR/dropbearmulti" ] && cp "$SCRIPT_DIR/dropbearmulti" "$ADDS/"
cp "$SCRIPT_DIR/scripts/"*.sh "$ADDS/" 2>/dev/null
[ -n "$SSH_PUBKEY" ] && [ -f "$SSH_PUBKEY" ] && cp "$SSH_PUBKEY" "$ADDS/authorized_keys"

# Create .adds.zip for easy transfer
(cd "$STAGING" && mv adds .adds && zip -r "$SCRIPT_DIR/dot-adds.zip" .adds/)
echo "Created dot-adds.zip"

echo ""
echo "=== Installation ==="
echo "1. Connect device via USB"
echo "2. Copy update.tar to the root of the device"
echo "3. Extract dot-adds.zip to the root of the device"
echo "   (creates .adds/ with all binaries and scripts)"
echo "4. Eject and reboot"
echo ""
echo "After reboot:"
echo "  - SSH: ssh -p 2222 root@<device-ip>"
echo "  - TCP shell: nc <device-ip> 4444"
echo "  - Menu: Mehr > Scripts"
