#!/bin/sh
STATUS=""
if pidof dropbearmulti >/dev/null 2>&1; then
    STATUS="SSH: running"
else
    STATUS="SSH: stopped"
fi
if pidof dbclient >/dev/null 2>&1; then
    STATUS="$STATUS | Tunnel: active"
else
    STATUS="$STATUS | Tunnel: down"
fi
echo "$STATUS" > /mnt/onboard/cloud-status.txt
