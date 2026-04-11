#!/bin/sh
# Force WiFi re-association. Useful after nickel has dropped the radio
# (e.g. during a soft-sleep cycle) and it hasn't come back on its own.
# Runs best-effort: tries several mechanisms in order, stops once one
# restores a route.

log() { echo "$(date): $1" >> /mnt/onboard/wifi-reconnect.log; }

log "reconnect requested"

# 1. Try to nudge wpa_supplicant directly
wpa_cli -i wlan0 reassociate 2>/dev/null

# 2. Cycle the interface if it's down or has no IP
IP=$(ip -4 addr show wlan0 2>/dev/null | grep -o 'inet [0-9.]*' | cut -d' ' -f2)
if [ -z "$IP" ]; then
    log "no IP — cycling wlan0"
    ifconfig wlan0 down 2>/dev/null
    sleep 1
    ifconfig wlan0 up 2>/dev/null
    # Let wpa_supplicant re-associate
    wpa_cli -i wlan0 reconnect 2>/dev/null
    sleep 2
    # Kick DHCP
    udhcpc -q -n -i wlan0 >/dev/null 2>&1
fi

# 3. Final state
IP=$(ip -4 addr show wlan0 2>/dev/null | grep -o 'inet [0-9.]*' | cut -d' ' -f2)
log "finished, ip=${IP:-none}"
