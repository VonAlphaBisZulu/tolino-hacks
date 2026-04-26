#!/bin/sh
# Count staged inbox files that are NOT already present on-device with
# the same md5. Used by the Tolino Hacks panel to show the "Pull Inbox (N)"
# badge. Exits 0 with a number on stdout (or 0 on any failure so the panel
# just shows "Pull Inbox" without a count).

ENV="/mnt/onboard/.adds/.env"
[ -f "$ENV" ] && . "$ENV"

if [ -n "${INBOX_URL:-}" ]; then
    BASE="$INBOX_URL"
elif [ -n "${TUNNEL_MCP_URL:-}" ]; then
    BASE=$(echo "$TUNNEL_MCP_URL" | sed 's|/mcp$||')
else
    echo 0; exit 0
fi

PW="${INBOX_PASSWORD:-${UPLOAD_PASSWORD:-${MCP_CLIENT_SECRET:-}}}"
[ -z "$PW" ] && { echo 0; exit 0; }

export BASE PW
python3 - 2>/dev/null <<'PY' || echo 0
import os, json, ssl, hashlib, urllib.request as u
base = os.environ["BASE"].rstrip("/")
pw   = os.environ["PW"]
req = u.Request(base + "/inbox/list",
                headers={"x-tolino-password": pw})
data = json.loads(u.urlopen(req, context=ssl.create_default_context(), timeout=4).read())
count = 0
for f in data.get("files", []):
    local = "/mnt/onboard/" + f["name"]
    remote_md5 = f.get("md5", "")
    if os.path.isfile(local) and remote_md5:
        h = hashlib.md5()
        with open(local, "rb") as fh:
            for chunk in iter(lambda: fh.read(65536), b""):
                h.update(chunk)
        if h.hexdigest() == remote_md5:
            continue    # already on device, identical
    count += 1
print(count)
PY
