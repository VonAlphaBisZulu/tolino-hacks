#!/bin/sh
killall dropbearmulti 2>/dev/null
iptables -D INPUT -p tcp --dport 2222 -j DROP 2>/dev/null
iptables -D INPUT -p tcp --dport 2222 -s 192.168.0.0/16 -j ACCEPT 2>/dev/null
iptables -D INPUT -p tcp --dport 2222 -s 10.0.0.0/8 -j ACCEPT 2>/dev/null
iptables -D INPUT -p tcp --dport 2222 -s 172.16.0.0/12 -j ACCEPT 2>/dev/null
echo "SSH stopped" > /mnt/onboard/cloud-status.txt
