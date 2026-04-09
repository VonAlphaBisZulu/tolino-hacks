#!/bin/sh
/etc/init.d/S99custom stop
echo "SSH stopped" > /mnt/onboard/cloud-status.txt
