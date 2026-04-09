#!/bin/sh
# driver.sh — Full setup payload for Tolino/Kobo firmware v5
# Place inside update.tar alongside pre-staged files in .adds/
#
# Pre-stage on USB before applying update:
#   /mnt/onboard/.adds/dropbearmulti     — Dropbear SSH static binary
#   /mnt/onboard/.adds/libtolinom.so     — Hook library (from build.sh)
#   /mnt/onboard/.adds/authorized_keys   — Your SSH public key
#   /mnt/onboard/.adds/cloud-connect.sh  — (optional) tunnel script
#   /mnt/onboard/.adds/cloud-disconnect.sh
#   /mnt/onboard/.adds/cloud-status.sh
#   /mnt/onboard/.adds/sysinfo.sh

ARCHIVE="$1"; STAGE="$2"

case $STAGE in
  stage1)

    # --- 1. Init script: SSH server + TCP shell fallback ---
    cat > /etc/init.d/S99custom << 'INITEOF'
#!/bin/sh
case "$1" in
    start)
        # SSH server (dropbear)
        if [ -f /mnt/onboard/.adds/dropbearmulti ]; then
            mkdir -p /home/root/.ssh
            if [ -f /mnt/onboard/.adds/authorized_keys ]; then
                cp /mnt/onboard/.adds/authorized_keys /home/root/.ssh/authorized_keys
                chmod 700 /home/root/.ssh
                chmod 600 /home/root/.ssh/authorized_keys
            fi
            [ -f /etc/dropbear_rsa_host_key ] || /mnt/onboard/.adds/dropbearmulti dropbearkey -t rsa -f /etc/dropbear_rsa_host_key 2>/dev/null
            [ -f /etc/dropbear_ed25519_host_key ] || /mnt/onboard/.adds/dropbearmulti dropbearkey -t ed25519 -f /etc/dropbear_ed25519_host_key 2>/dev/null
            /mnt/onboard/.adds/dropbearmulti dropbear -r /etc/dropbear_rsa_host_key -r /etc/dropbear_ed25519_host_key -p 2222 &
        fi
        # TCP shell (fallback if SSH not available)
        python3 -c "
import socket, subprocess, threading
def handle(conn):
    try:
        while True:
            data = conn.recv(4096)
            if not data: break
            result = subprocess.run(data.decode().strip(), shell=True, capture_output=True, text=True, timeout=30)
            conn.sendall((result.stdout + result.stderr + '<<<END>>>\n').encode())
    except: pass
    finally: conn.close()
while True:
    try:
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(('', 4444))
        s.listen(5)
        while True:
            c, a = s.accept()
            threading.Thread(target=handle, args=(c,), daemon=True).start()
    except:
        import time; time.sleep(2)
" &
        ;;
    stop)
        killall dropbearmulti 2>/dev/null
        killall python3 2>/dev/null
        ;;
esac
INITEOF
    chmod +x /etc/init.d/S99custom
    ln -sf ../init.d/S99custom /etc/rc5.d/S99custom

    # --- 2. LD_PRELOAD injection in rc.local ---
    if ! grep -q 'tolinom' /etc/rc.local; then
        sed -i 's|/usr/local/Kobo/nickel -platform kobo|if [ -f /mnt/onboard/.adds/libtolinom.so ] \&\& [ ! -f /mnt/onboard/.adds/tolinom.disabled ]; then\n    export LD_PRELOAD=/mnt/onboard/.adds/libtolinom.so\nfi\n/usr/local/Kobo/nickel -platform kobo|' /etc/rc.local
    fi

    # --- 3. Generate SSH host keys if dropbearmulti is pre-staged ---
    if [ -f /mnt/onboard/.adds/dropbearmulti ]; then
        chmod +x /mnt/onboard/.adds/dropbearmulti
        [ -f /etc/dropbear_rsa_host_key ] || /mnt/onboard/.adds/dropbearmulti dropbearkey -t rsa -f /etc/dropbear_rsa_host_key 2>/dev/null
        [ -f /etc/dropbear_ed25519_host_key ] || /mnt/onboard/.adds/dropbearmulti dropbearkey -t ed25519 -f /etc/dropbear_ed25519_host_key 2>/dev/null
    fi

    # --- 4. Set up authorized_keys ---
    if [ -f /mnt/onboard/.adds/authorized_keys ]; then
        mkdir -p /home/root/.ssh
        cp /mnt/onboard/.adds/authorized_keys /home/root/.ssh/authorized_keys
        chmod 700 /home/root/.ssh
        chmod 600 /home/root/.ssh/authorized_keys
    fi

    # --- 5. Make scripts executable ---
    chmod +x /mnt/onboard/.adds/*.sh 2>/dev/null

    sync
    exit 1  # don't reboot into recovery
    ;;
  stage2)
    set_boot_part root
    ;;
esac
