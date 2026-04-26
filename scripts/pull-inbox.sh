#!/bin/sh
# Pull all files staged in the chuzel.net inbox directly over HTTPS.
# No reverse SSH tunnel needed — just WiFi. Writes the list of pulled
# filenames to /tmp/tolinom-inbox-pulled so the panel can ask the user
# whether to delete them from the server.
#
# BusyBox wget on Kobo doesn't do SNI so TLS fails against modern
# servers. We use python3 (which ships with the firmware) for HTTPS.

set -u

ENV="/mnt/onboard/.adds/.env"
[ -f "$ENV" ] && . "$ENV"

if [ -n "${INBOX_URL:-}" ]; then
    BASE="$INBOX_URL"
elif [ -n "${TUNNEL_MCP_URL:-}" ]; then
    BASE=$(echo "$TUNNEL_MCP_URL" | sed 's|/mcp$||')
else
    echo "no INBOX_URL or TUNNEL_MCP_URL in .env" >&2
    exit 1
fi

PW="${INBOX_PASSWORD:-${UPLOAD_PASSWORD:-${MCP_CLIENT_SECRET:-}}}"
[ -z "$PW" ] && { echo "no INBOX_PASSWORD in .env" >&2; exit 1; }

OUT="/tmp/tolinom-inbox-pulled"
: > "$OUT"

export BASE PW OUT
python3 - <<'PY'
import os, json, ssl, hashlib, urllib.request, urllib.parse, sys

base = os.environ["BASE"].rstrip("/")
pw   = os.environ["PW"]
out  = os.environ["OUT"]
ctx  = ssl.create_default_context()

def req(path, method="GET"):
    r = urllib.request.Request(base + path, method=method,
                               headers={"x-tolino-password": pw})
    return urllib.request.urlopen(r, context=ctx, timeout=30)

def file_md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()

# List
try:
    with req("/inbox/list") as r:
        data = json.loads(r.read())
except Exception as e:
    print("list failed:", e, file=sys.stderr)
    sys.exit(2)

files = data.get("files", [])
if not files:
    try: os.remove(out)
    except OSError: pass
    print("inbox empty")
    sys.exit(0)

pulled = []      # newly downloaded — user will be asked to keep/delete
identical = []   # server copy matches local file — tagged for deletion too
for f in files:
    name = f["name"]
    remote_md5 = f.get("md5", "")
    dest = "/mnt/onboard/" + name

    # Skip if already present with matching md5 — no download, no count,
    # but offer to clear it from the server in the confirmation dialog.
    if remote_md5 and os.path.isfile(dest) and file_md5(dest) == remote_md5:
        identical.append(name)
        continue

    encoded = urllib.parse.quote(name, safe="")
    try:
        with req("/inbox/fetch/" + encoded) as r, open(dest, "wb") as w:
            while True:
                chunk = r.read(65536)
                if not chunk: break
                w.write(chunk)
        pulled.append(name)
    except Exception as e:
        print(f"failed {name}:", e, file=sys.stderr)
        try: os.remove(dest)
        except OSError: pass

# Flag file lists NEW pulls for the keep/delete dialog. If the dialog's
# delete button is chosen, we also clean up the identical-duplicates so
# the inbox doesn't keep showing stale entries.
with open(out, "w") as f:
    for n in pulled:
        f.write(n + "\n")

# A second file tracks identical-dupes. clear-inbox.sh deletes these too
# when the user chooses "Delete from server".
dupes_path = "/tmp/tolinom-inbox-dupes"
if identical:
    with open(dupes_path, "w") as f:
        for n in identical:
            f.write(n + "\n")
else:
    try: os.remove(dupes_path)
    except OSError: pass

if not pulled:
    try: os.remove(out)
    except OSError: pass

print(f"pulled {len(pulled)} file(s), {len(identical)} already present")
PY

# Trigger library refresh
touch /tmp/tolinom-sync-request
