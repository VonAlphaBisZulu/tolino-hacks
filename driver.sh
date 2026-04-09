#!/bin/sh
# driver.sh — Full setup payload for Tolino/Kobo firmware v5
#
# This is a self-contained update.tar: all binaries and scripts are embedded.
# Just copy update.tar to the device root via USB, eject, and reboot.
#
# After reboot, go to: Mehr > Scripts > SSH Start
# The dialog will display your auto-generated credentials:
#   ssh -p 2222 root@<device-ip> pw: <random-password>
#
# Password is derived from the device serial — unique per device, stable.
# To use key auth instead, place your public key as .adds/authorized_keys
# on the device via USB before applying the update.

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

    # --- 2. Set up authorized_keys (if pre-staged) ---
    if [ -f /mnt/onboard/.adds/authorized_keys ]; then
        mkdir -p /home/root/.ssh
        cp /mnt/onboard/.adds/authorized_keys /home/root/.ssh/authorized_keys
        chmod 700 /home/root/.ssh
        chmod 600 /home/root/.ssh/authorized_keys
    fi

    # --- 3. Init script: start SSH on boot ---
    cat > /etc/init.d/S99custom << 'INITEOF'
#!/bin/sh
case "$1" in
    start)
        [ -f /mnt/onboard/.adds/ssh-start.sh ] && /mnt/onboard/.adds/ssh-start.sh
        ;;
    stop)
        [ -f /mnt/onboard/.adds/ssh-stop.sh ] && /mnt/onboard/.adds/ssh-stop.sh
        ;;
esac
INITEOF
    chmod +x /etc/init.d/S99custom
    ln -sf ../init.d/S99custom /etc/rc5.d/S99custom

    # --- 4. LD_PRELOAD injection in rc.local ---
    if ! grep -q 'tolinom' /etc/rc.local; then
        sed -i 's|/usr/local/Kobo/nickel -platform kobo|if [ -f /mnt/onboard/.adds/libtolinom.so ] \&\& [ ! -f /mnt/onboard/.adds/tolinom.disabled ]; then\n    export LD_PRELOAD=/mnt/onboard/.adds/libtolinom.so\nfi\n/usr/local/Kobo/nickel -platform kobo|' /etc/rc.local
    fi

    # --- 5. Generate SSH host keys ---
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
