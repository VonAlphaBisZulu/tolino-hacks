#!/bin/sh
# dbclient runs via dropbearmulti — kill by full command line, not argv[0]
pkill -f 'dropbearmulti dbclient' 2>/dev/null
echo "Tunnel disconnected"
