#!/bin/sh
killall dropbearmulti 2>/dev/null

# Release wake-lock and allow PowerManager::suspend to run again
rm -f /tmp/tolinom-keepawake
if [ -f /sys/power/wake_unlock ]; then
    echo "tolino-hacks-ssh" > /sys/power/wake_unlock 2>/dev/null
fi

echo "SSH stopped" > /mnt/onboard/cloud-status.txt
