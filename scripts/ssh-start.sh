#!/bin/sh
# Start SSH and display credentials.
# Password is derived from the device serial number — unique per device,
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
    # Ensure authorized_keys is in the right place
    if [ -f "$ADDS/authorized_keys" ]; then
        mkdir -p /home/root/.ssh
        cp "$ADDS/authorized_keys" /home/root/.ssh/authorized_keys
        chmod 700 /home/root/.ssh
        chmod 600 /home/root/.ssh/authorized_keys
    fi
fi

if [ "$AUTH" = "password" ]; then
    # Derive password from device serial (stable, unique, recoverable)
    # Sources tried in order: /mnt/onboard/.kobo/version, machine-id, hostname
    SERIAL=$(head -1 /mnt/onboard/.kobo/version 2>/dev/null)
    [ -z "$SERIAL" ] && SERIAL=$(cat /etc/machine-id 2>/dev/null)
    [ -z "$SERIAL" ] && SERIAL=$(hostname)
    PW=$(echo -n "tolino-hacks-ssh:${SERIAL}" | md5sum | head -c 12)
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

# Restrict to local networks
iptables -D INPUT -p tcp --dport 2222 -j DROP 2>/dev/null
iptables -D INPUT -p tcp --dport 2222 -s 192.168.0.0/16 -j ACCEPT 2>/dev/null
iptables -D INPUT -p tcp --dport 2222 -s 10.0.0.0/8 -j ACCEPT 2>/dev/null
iptables -D INPUT -p tcp --dport 2222 -s 172.16.0.0/12 -j ACCEPT 2>/dev/null
iptables -A INPUT -p tcp --dport 2222 -s 192.168.0.0/16 -j ACCEPT
iptables -A INPUT -p tcp --dport 2222 -s 10.0.0.0/8 -j ACCEPT
iptables -A INPUT -p tcp --dport 2222 -s 172.16.0.0/12 -j ACCEPT
iptables -A INPUT -p tcp --dport 2222 -j DROP

# Get device IP
IP=$(ip -4 addr show wlan0 2>/dev/null | grep -o 'inet [0-9.]*' | cut -d' ' -f2)
[ -z "$IP" ] && IP="<no wifi>"

# Write status for the dialog
if [ "$AUTH" = "password" ]; then
    echo "root@${IP}:2222 pw:${PW}" > "$STATUS"
else
    echo "root@${IP}:2222 (key auth)" > "$STATUS"
fi
