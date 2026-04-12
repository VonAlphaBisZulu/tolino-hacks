#!/bin/sh
# Start SSH and reverse tunnel to your server.
# Configure values in /mnt/onboard/.adds/.env (see .env.example)

# Defaults (overridden by .env)
TUNNEL_USER="user"
TUNNEL_HOST="yourserver.com"
TUNNEL_KEY="/mnt/onboard/.adds/tolino_client_key"
TUNNEL_PORT="2223"

# Load config
[ -f /mnt/onboard/.adds/.env ] && . /mnt/onboard/.adds/.env

DROPBEAR="/mnt/onboard/.adds/dropbearmulti"

# Start dropbear if not running.
# dropbearmulti is a multi-call binary: server runs as "dropbearmulti dropbear",
# client as "dropbearmulti dbclient". pidof sees only argv[0] so distinguish
# by full command line with pgrep -f.
if ! pgrep -f '[d]ropbearmulti dropbear' >/dev/null 2>&1; then
    $DROPBEAR dropbear \
        -r /etc/dropbear_rsa_host_key \
        -r /etc/dropbear_ed25519_host_key \
        -p 2222 &
    sleep 1
fi

# Start tunnel if not running
if ! pgrep -f '[d]ropbearmulti dbclient' >/dev/null 2>&1; then
    $DROPBEAR dbclient \
        -y -i "$TUNNEL_KEY" \
        -N -f -R "${TUNNEL_PORT}:localhost:2222" \
        "${TUNNEL_USER}@${TUNNEL_HOST}" 2>/dev/null
fi
