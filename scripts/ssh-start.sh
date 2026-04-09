#!/bin/sh
/etc/init.d/S99custom start
echo "SSH started on port 2222" > /mnt/onboard/cloud-status.txt
