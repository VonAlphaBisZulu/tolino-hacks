#!/bin/sh
# driver.sh — Full setup payload for Tolino/Kobo firmware v5
#
# This is a self-contained update.tar: all binaries and scripts are embedded.
# Just copy update.tar to the device root via USB, eject, and reboot.
#
# SSH access (port 2222, local network only):
#   - Default credentials: root / tolinopass42
#   - If .adds/authorized_keys exists, key auth is used instead
#   - Only accepts connections from local networks (192.168.x, 10.x, 172.16-31.x)
#
# To customize, place a .env file at /mnt/onboard/.adds/.env before applying.
# See .env.example for available settings.

ARCHIVE="$1"; STAGE="$2"

case $STAGE in
  stage1)

    # --- 0. Enable devmode (switch from release to dev branch) ---
    if [ -f /etc/rootfs-branch ]; then
        sed -i 's#release/#dev/#' /etc/rootfs-branch
    elif [ -f /usr/local/Kobo/branch ]; then
        sed -i 's#release/#dev/#' /usr/local/Kobo/branch
    fi

    # --- 1. Extract embedded .adds/ from update.tar ---
    if [ -n "$ARCHIVE" ] && [ -f "$ARCHIVE" ]; then
        tar xf "$ARCHIVE" -C /mnt/onboard/ .adds/ 2>/dev/null || true
        chmod +x /mnt/onboard/.adds/dropbearmulti 2>/dev/null
        chmod +x /mnt/onboard/.adds/*.sh 2>/dev/null
    fi

    # --- 2. Load config ---
    SSH_PASSWORD="tolinopass42"
    [ -f /mnt/onboard/.adds/.env ] && . /mnt/onboard/.adds/.env

    # --- 3. Set root password (used when no authorized_keys) ---
    if [ -n "$SSH_PASSWORD" ] && command -v python3 >/dev/null 2>&1; then
        HASH=$(python3 -c "import crypt; print(crypt.crypt('$SSH_PASSWORD', crypt.mksalt(crypt.METHOD_SHA256)))")
        sed -i "s|^root:[^:]*:|root:$HASH:|" /etc/shadow
    fi

    # --- 4. Set up authorized_keys ---
    if [ -f /mnt/onboard/.adds/authorized_keys ]; then
        mkdir -p /home/root/.ssh
        cp /mnt/onboard/.adds/authorized_keys /home/root/.ssh/authorized_keys
        chmod 700 /home/root/.ssh
        chmod 600 /home/root/.ssh/authorized_keys
    fi

    # --- 5. Init script: SSH server (local network only) ---
    cat > /etc/init.d/S99custom << 'INITEOF'
#!/bin/sh
DROPBEAR="/mnt/onboard/.adds/dropbearmulti"
case "$1" in
    start)
        [ -f "$DROPBEAR" ] || exit 0

        # Restore authorized_keys from persistent storage
        if [ -f /mnt/onboard/.adds/authorized_keys ]; then
            mkdir -p /home/root/.ssh
            cp /mnt/onboard/.adds/authorized_keys /home/root/.ssh/authorized_keys
            chmod 700 /home/root/.ssh
            chmod 600 /home/root/.ssh/authorized_keys
        fi

        # Generate host keys if needed
        [ -f /etc/dropbear_rsa_host_key ] || $DROPBEAR dropbearkey -t rsa -f /etc/dropbear_rsa_host_key 2>/dev/null
        [ -f /etc/dropbear_ed25519_host_key ] || $DROPBEAR dropbearkey -t ed25519 -f /etc/dropbear_ed25519_host_key 2>/dev/null

        # Determine auth mode
        EXTRA_ARGS=""
        if [ -f /home/root/.ssh/authorized_keys ]; then
            # Key auth: disable password login
            EXTRA_ARGS="-s"
        fi

        # Start dropbear
        $DROPBEAR dropbear \
            -r /etc/dropbear_rsa_host_key \
            -r /etc/dropbear_ed25519_host_key \
            -p 2222 $EXTRA_ARGS &

        # Restrict SSH to local networks only
        iptables -D INPUT -p tcp --dport 2222 -j DROP 2>/dev/null
        iptables -D INPUT -p tcp --dport 2222 -s 192.168.0.0/16 -j ACCEPT 2>/dev/null
        iptables -D INPUT -p tcp --dport 2222 -s 10.0.0.0/8 -j ACCEPT 2>/dev/null
        iptables -D INPUT -p tcp --dport 2222 -s 172.16.0.0/12 -j ACCEPT 2>/dev/null
        iptables -A INPUT -p tcp --dport 2222 -s 192.168.0.0/16 -j ACCEPT
        iptables -A INPUT -p tcp --dport 2222 -s 10.0.0.0/8 -j ACCEPT
        iptables -A INPUT -p tcp --dport 2222 -s 172.16.0.0/12 -j ACCEPT
        iptables -A INPUT -p tcp --dport 2222 -j DROP
        ;;
    stop)
        killall dropbearmulti 2>/dev/null
        iptables -D INPUT -p tcp --dport 2222 -j DROP 2>/dev/null
        iptables -D INPUT -p tcp --dport 2222 -s 192.168.0.0/16 -j ACCEPT 2>/dev/null
        iptables -D INPUT -p tcp --dport 2222 -s 10.0.0.0/8 -j ACCEPT 2>/dev/null
        iptables -D INPUT -p tcp --dport 2222 -s 172.16.0.0/12 -j ACCEPT 2>/dev/null
        ;;
esac
INITEOF
    chmod +x /etc/init.d/S99custom
    ln -sf ../init.d/S99custom /etc/rc5.d/S99custom

    # --- 6. LD_PRELOAD injection in rc.local ---
    if ! grep -q 'tolinom' /etc/rc.local; then
        sed -i 's|/usr/local/Kobo/nickel -platform kobo|if [ -f /mnt/onboard/.adds/libtolinom.so ] \&\& [ ! -f /mnt/onboard/.adds/tolinom.disabled ]; then\n    export LD_PRELOAD=/mnt/onboard/.adds/libtolinom.so\nfi\n/usr/local/Kobo/nickel -platform kobo|' /etc/rc.local
    fi

    # --- 7. Generate SSH host keys ---
    if [ -f /mnt/onboard/.adds/dropbearmulti ]; then
        [ -f /etc/dropbear_rsa_host_key ] || /mnt/onboard/.adds/dropbearmulti dropbearkey -t rsa -f /etc/dropbear_rsa_host_key 2>/dev/null
        [ -f /etc/dropbear_ed25519_host_key ] || /mnt/onboard/.adds/dropbearmulti dropbearkey -t ed25519 -f /etc/dropbear_ed25519_host_key 2>/dev/null
    fi

    sync
    exit 1  # don't reboot into recovery
    ;;
  stage2)
    set_boot_part root
    ;;
esac
