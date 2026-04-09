#!/bin/sh
# Start SSH and reverse tunnel to your server.
# Configure TUNNEL_USER, TUNNEL_HOST, and TUNNEL_KEY below.

TUNNEL_USER="user"
TUNNEL_HOST="yourserver.com"
TUNNEL_KEY="/mnt/onboard/.adds/tolino_client_key"
TUNNEL_PORT="2223"  # remote port on server that forwards back to us

DROPBEAR="/mnt/onboard/.adds/dropbearmulti"

# Start dropbear if not running
if ! pidof dropbearmulti >/dev/null 2>&1; then
    $DROPBEAR dropbear \
        -r /etc/dropbear_rsa_host_key \
        -r /etc/dropbear_ed25519_host_key \
        -p 2222 &
    sleep 1
fi

# Start tunnel if not running
if ! pidof dbclient >/dev/null 2>&1; then
    $DROPBEAR dbclient \
        -y -i "$TUNNEL_KEY" \
        -N -f -R "${TUNNEL_PORT}:localhost:2222" \
        "${TUNNEL_USER}@${TUNNEL_HOST}" 2>/dev/null
fi
