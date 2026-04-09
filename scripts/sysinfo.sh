#!/bin/sh
(uname -a; echo "---"; free -m; echo "---"; df -h; echo "---"; uptime) > /mnt/onboard/sysinfo.txt
