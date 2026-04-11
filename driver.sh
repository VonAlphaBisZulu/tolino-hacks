#!/bin/sh
# driver.sh — Full setup payload for Tolino/Kobo firmware v5
#
# This is a self-contained update.tar: all binaries and scripts are embedded.
# Just copy update.tar to the device root via USB, eject, and reboot.
#
# After reboot, go to: Mehr > Scripts > SSH Start
# The dialog will display your auto-generated credentials:
#   ssh -p 2222 root@<device-ip> pw:<password>
#
# Password is derived from the device serial — unique per device, stable.
# To use key auth instead, place your public key as .adds/authorized_keys
# on the device via USB before applying the update.

ARCHIVE="$1"; STAGE="$2"

case $STAGE in
  stage1)

    # NOTE: we deliberately do NOT flip /etc/rootfs-branch to "dev/". Kobo's
    # nickel enables a top-right debug HUD when running the dev rootfs, and
    # we get no benefit from the dev branch — /etc mods, /home/root, /mnt/
    # onboard and rc.local LD_PRELOAD all persist on the release branch too.

    # --- 1. Stage .adds/ files to /etc/tolino-hacks/ ---
    # /mnt/onboard/ may not be writable during the update process,
    # so we store files on the root filesystem first. The init script
    # copies them to /mnt/onboard/.adds/ on first boot.
    mkdir -p /etc/tolino-hacks
    SCRIPT_DIR="$(dirname "$0")"

    # Try to extract from archive
    if [ -n "$ARCHIVE" ] && [ -f "$ARCHIVE" ]; then
        tar xf "$ARCHIVE" -C /etc/tolino-hacks/ .adds/ 2>/dev/null
        # If .adds/ was extracted inside tolino-hacks, flatten it
        if [ -d /etc/tolino-hacks/.adds ]; then
            cp /etc/tolino-hacks/.adds/* /etc/tolino-hacks/ 2>/dev/null
            rm -rf /etc/tolino-hacks/.adds
        fi
    fi
    # Copy from staging directory (if updater pre-extracted)
    if [ -d "$SCRIPT_DIR/.adds" ]; then
        cp "$SCRIPT_DIR/.adds/"* /etc/tolino-hacks/ 2>/dev/null
    fi
    chmod +x /etc/tolino-hacks/dropbearmulti 2>/dev/null
    chmod +x /etc/tolino-hacks/*.sh 2>/dev/null

    # Also try writing directly to /mnt/onboard/.adds/ (may work on some devices)
    if [ -w /mnt/onboard/ ]; then
        mkdir -p /mnt/onboard/.adds
        cp /etc/tolino-hacks/* /mnt/onboard/.adds/ 2>/dev/null
        chmod +x /mnt/onboard/.adds/dropbearmulti 2>/dev/null
        chmod +x /mnt/onboard/.adds/*.sh 2>/dev/null
    fi

    # --- 2. Clean up stale keys from previous installs ---
    rm -f /etc/dropbear_rsa_host_key /etc/dropbear_ed25519_host_key
    rm -rf /home/root/.ssh

    # --- 2b. Set up authorized_keys (if pre-staged by user) ---
    if [ -f /mnt/onboard/.adds/authorized_keys ]; then
        mkdir -p /home/root/.ssh
        cp /mnt/onboard/.adds/authorized_keys /home/root/.ssh/authorized_keys
        chmod 700 /home/root/.ssh
        chmod 600 /home/root/.ssh/authorized_keys
    fi

    # --- 3. Init script: deploy and start SSH on boot ---
    cat > /etc/init.d/S99custom << 'INITEOF'
#!/bin/sh
case "$1" in
    start)
        # On first boot after update, copy staged files to .adds/.
        # cp -n skips existing files so a user's hand-deployed libtolinom.so
        # is never clobbered. Also restore the NickelHook failsafe rename
        # if the previous boot left a stale .failsafe behind.
        if [ -f /mnt/onboard/.adds/libtolinom.so.failsafe ] \
           && [ ! -f /mnt/onboard/.adds/libtolinom.so ]; then
            mv /mnt/onboard/.adds/libtolinom.so.failsafe \
               /mnt/onboard/.adds/libtolinom.so
        fi
        if [ -d /etc/tolino-hacks ] && [ -f /etc/tolino-hacks/dropbearmulti ]; then
            mkdir -p /mnt/onboard/.adds
            cp -n /etc/tolino-hacks/* /mnt/onboard/.adds/ 2>/dev/null
            chmod +x /mnt/onboard/.adds/dropbearmulti 2>/dev/null
            chmod +x /mnt/onboard/.adds/*.sh 2>/dev/null
        fi
        # SSH does NOT auto-start on boot — users would wonder why the
        # device never sleeps. Create /mnt/onboard/.adds/ssh-autostart as
        # an empty flag file to opt into auto-start for your own device.
        if [ -f /mnt/onboard/.adds/ssh-autostart ] \
           && [ -f /mnt/onboard/.adds/ssh-start.sh ]; then
            /mnt/onboard/.adds/ssh-start.sh
        fi
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
    DBEAR="/etc/tolino-hacks/dropbearmulti"
    [ -f "$DBEAR" ] || DBEAR="/mnt/onboard/.adds/dropbearmulti"
    if [ -f "$DBEAR" ]; then
        [ -f /etc/dropbear_rsa_host_key ] || "$DBEAR" dropbearkey -t rsa -f /etc/dropbear_rsa_host_key 2>/dev/null
        [ -f /etc/dropbear_ed25519_host_key ] || "$DBEAR" dropbearkey -t ed25519 -f /etc/dropbear_ed25519_host_key 2>/dev/null
    fi

    sync
    exit 1  # don't reboot into recovery
    ;;
  stage2)
    set_boot_part root
    ;;
esac
