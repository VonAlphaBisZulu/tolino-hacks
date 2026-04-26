#!/bin/sh
# Delete files listed in /tmp/tolinom-inbox-pulled from the chuzel.net
# inbox. Called by the Tolino Hacks panel when the user confirms they
# want staged files removed from the server after a successful pull.
#
# Uses python3 because BusyBox wget has no DELETE method.

set -u

ENV="/mnt/onboard/.adds/.env"
[ -f "$ENV" ] && . "$ENV"

if [ -n "${INBOX_URL:-}" ]; then
    BASE="$INBOX_URL"
elif [ -n "${TUNNEL_MCP_URL:-}" ]; then
    BASE=$(echo "$TUNNEL_MCP_URL" | sed 's|/mcp$||')
else
    exit 1
fi

PW="${INBOX_PASSWORD:-${UPLOAD_PASSWORD:-${MCP_CLIENT_SECRET:-}}}"
[ -z "$PW" ] && exit 1

LIST="${1:-/tmp/tolinom-inbox-pulled}"
DUPES="/tmp/tolinom-inbox-dupes"

# Combine both lists so identical-on-device files also get cleaned from
# the server when the user picks "Delete from server".
MERGED=$(mktemp)
[ -f "$LIST"  ] && cat "$LIST"  >> "$MERGED"
[ -f "$DUPES" ] && cat "$DUPES" >> "$MERGED"
[ -s "$MERGED" ] || { rm -f "$MERGED" "$LIST" "$DUPES"; exit 0; }

export BASE PW MERGED
python3 - <<'PY'
import os, urllib.request, urllib.parse, ssl
base = os.environ["BASE"]
pw   = os.environ["PW"]
lst  = os.environ["MERGED"]
ctx  = ssl.create_default_context()
seen = set()
with open(lst) as f:
    for line in f:
        name = line.strip()
        if not name or name in seen: continue
        seen.add(name)
        url = base + "/inbox/item/" + urllib.parse.quote(name, safe="")
        req = urllib.request.Request(url, method="DELETE",
                                     headers={"x-tolino-password": pw})
        try:
            urllib.request.urlopen(req, context=ctx, timeout=10).read()
        except Exception as e:
            print("delete failed for", name, ":", e)
PY

rm -f "$MERGED" "$LIST" "$DUPES"
