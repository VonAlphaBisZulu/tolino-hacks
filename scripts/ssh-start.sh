#!/bin/sh
# Start SSH and display credentials.
# Password is derived from device serial + MAC — unique per device,
# stable across reboots, recoverable without the device displaying it.
# If authorized_keys exists, key auth is used instead (no password).

ADDS="/mnt/onboard/.adds"
DROPBEAR="$ADDS/dropbearmulti"
STATUS="/mnt/onboard/cloud-status.txt"

[ -f "$DROPBEAR" ] || { echo "dropbearmulti not found" > "$STATUS"; exit 1; }

# Determine auth mode
AUTH="password"
if [ -f /home/root/.ssh/authorized_keys ] || [ -f "$ADDS/authorized_keys" ]; then
    AUTH="key"
    if [ -f "$ADDS/authorized_keys" ]; then
        mkdir -p /home/root/.ssh
        cp "$ADDS/authorized_keys" /home/root/.ssh/authorized_keys
        chmod 700 /home/root/.ssh
        chmod 600 /home/root/.ssh/authorized_keys
    fi
fi

if [ "$AUTH" = "password" ]; then
    # Derive password from device serial + MAC (stable, unique)
    SERIAL=$(cat /mnt/onboard/.kobo/version 2>/dev/null | cut -d',' -f1)
    MAC=$(cat /sys/class/net/wlan0/address 2>/dev/null)
    PW=$(echo -n "tolino-hacks-ssh:${SERIAL}:${MAC}" | md5sum | cut -c1-12)
    echo "root:$PW" | chpasswd 2>/dev/null
fi

# Start dropbear if not running
if ! pidof dropbearmulti >/dev/null 2>&1; then
    [ -f /etc/dropbear_rsa_host_key ] || $DROPBEAR dropbearkey -t rsa -f /etc/dropbear_rsa_host_key 2>/dev/null
    [ -f /etc/dropbear_ed25519_host_key ] || $DROPBEAR dropbearkey -t ed25519 -f /etc/dropbear_ed25519_host_key 2>/dev/null

    EXTRA=""
    [ "$AUTH" = "key" ] && EXTRA="-s"

    $DROPBEAR dropbear \
        -r /etc/dropbear_rsa_host_key \
        -r /etc/dropbear_ed25519_host_key \
        -p 2222 $EXTRA &
fi

# Get device IP
IP=$(ip -4 addr show wlan0 2>/dev/null | grep -o 'inet [0-9.]*' | cut -d' ' -f2)
[ -z "$IP" ] && IP="<no wifi>"

# Write status for the dialog
if [ "$AUTH" = "password" ]; then
    echo "root@${IP}:2222 pw:${PW}" > "$STATUS"
else
    echo "root@${IP}:2222 (key auth)" > "$STATUS"
fi
