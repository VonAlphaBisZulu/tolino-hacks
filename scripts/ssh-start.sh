#!/bin/sh
# Start SSH and display credentials.
# Password is derived from device serial + MAC — unique per device,
# stable across reboots, recoverable without the device displaying it.
# If authorized_keys exists, key auth is used instead (no password).

ADDS="/mnt/onboard/.adds"
DROPBEAR="$ADDS/dropbearmulti"
STATUS="/mnt/onboard/cloud-status.txt"

[ -f "$DROPBEAR" ] || { echo "dropbearmulti not found" > "$STATUS"; exit 1; }

# Stage authorized_keys if pre-deposited on onboard storage
if [ -f "$ADDS/authorized_keys" ]; then
    mkdir -p /home/root/.ssh
    cp "$ADDS/authorized_keys" /home/root/.ssh/authorized_keys
    chmod 700 /home/root/.ssh
    chmod 600 /home/root/.ssh/authorized_keys
fi

# Always derive and set the password (stable, unique per device).
# Dropbear is started with both key and password auth enabled so that
# clients that only speak password (WinSCP, etc) still work.
SERIAL=$(cat /mnt/onboard/.kobo/version 2>/dev/null | cut -d',' -f1)
MAC=$(cat /sys/class/net/wlan0/address 2>/dev/null)
PW=$(echo -n "tolino-hacks-ssh:${SERIAL}:${MAC}" | md5sum | cut -c1-12)
echo "root:$PW" | chpasswd 2>/dev/null

# Start dropbear if not running.
# dropbearmulti is the same binary used by the outgoing tunnel (dbclient),
# so match on full command line to distinguish server from client.
if ! pgrep -f 'dropbearmulti dropbear' >/dev/null 2>&1; then
    [ -f /etc/dropbear_rsa_host_key ] || $DROPBEAR dropbearkey -t rsa -f /etc/dropbear_rsa_host_key 2>/dev/null
    [ -f /etc/dropbear_ed25519_host_key ] || $DROPBEAR dropbearkey -t ed25519 -f /etc/dropbear_ed25519_host_key 2>/dev/null

    $DROPBEAR dropbear \
        -r /etc/dropbear_rsa_host_key \
        -r /etc/dropbear_ed25519_host_key \
        -p 2222 &
fi

# Tell the nickel PowerManager::suspend hook to block sleep while SSH is up.
# The hook (in libtolinom.so) checks this file and short-circuits suspend().
touch /tmp/tolinom-keepawake

# Kernel wake-lock (best effort, belt-and-braces with the PM hook)
if [ -f /sys/power/wake_lock ]; then
    echo "tolino-hacks-ssh" > /sys/power/wake_lock 2>/dev/null
fi
echo on > /sys/class/net/wlan0/power/control 2>/dev/null
iwconfig wlan0 power off 2>/dev/null

# Background keep-alive + idle watchdog.
# While SSH is running:
#   - re-assert the wake-lock
#   - disable WiFi power save
#   - ping the gateway so the association stays active
# If no active SSH connection for 20 minutes, auto-stop SSH so the
# device can sleep again. An active connection means more than one
# dropbearmulti process (the listener + at least one session).
(
    IDLE_LIMIT=60   # 60 iterations * 20s = 20 min
    idle=0
    while pgrep -f 'dropbearmulti dropbear' >/dev/null 2>&1; do
        [ -f /sys/power/wake_lock ] && echo "tolino-hacks-ssh" > /sys/power/wake_lock 2>/dev/null
        echo on > /sys/class/net/wlan0/power/control 2>/dev/null
        iwconfig wlan0 power off 2>/dev/null
        GW=$(ip route 2>/dev/null | awk '/default/{print $3; exit}')
        [ -n "$GW" ] && ping -c 1 -W 2 "$GW" >/dev/null 2>&1

        # Active session count = dropbear server processes > 1 (listener + sessions)
        n=$(pgrep -f 'dropbearmulti dropbear' 2>/dev/null | wc -l)
        if [ "$n" -gt 1 ]; then
            idle=0
        else
            idle=$((idle + 1))
        fi
        if [ "$idle" -ge "$IDLE_LIMIT" ]; then
            /mnt/onboard/.adds/ssh-stop.sh
            break
        fi
        sleep 20
    done
) >/dev/null 2>&1 &

# Get device IP
IP=$(ip -4 addr show wlan0 2>/dev/null | grep -o 'inet [0-9.]*' | cut -d' ' -f2)
[ -z "$IP" ] && IP="<no wifi>"

# Write status for the dialog — panel reads pw: from here
echo "root@${IP}:2222 pw:${PW}" > "$STATUS"
