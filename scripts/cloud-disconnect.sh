#!/bin/sh
# dbclient runs via dropbearmulti — kill by full command line, not argv[0].
# BusyBox pkill may not support -f, so use pgrep + kill instead.
kill $(pgrep -f '[d]ropbearmulti dbclient') 2>/dev/null
echo "Tunnel disconnected"
