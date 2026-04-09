# Tolino/Kobo Firmware v5 (Qt6) — Root Access, SSH, Menu Injection & Library Sync

> Tested on Tolino Shine Color (= Kobo Clara Color), firmware v5, Qt 6.5.2, kernel 4.9.77, armv7l.
> Should work on any Kobo/Tolino device running firmware v5 with minor adjustments.

## Table of Contents
1. [Overview](#overview)
2. [Prerequisites](#prerequisites)
3. [Step 1: Enable Devmode](#step-1-enable-devmode)
4. [Step 2: Get Shell Access](#step-2-get-shell-access)
5. [Step 3: Install Dropbear SSH](#step-3-install-dropbear-ssh)
6. [Step 4: Build the Hook Library (tolinom)](#step-4-build-the-hook-library-tolinom)
7. [Step 5: Install the Hook Library](#step-5-install-the-hook-library)
8. [Step 6: Shell Scripts](#step-6-shell-scripts)
9. [Step 7: One-Shot Recovery (update.tar)](#step-7-one-shot-recovery-updatetar)
10. [Key Technical Findings for v5](#key-technical-findings-for-v5)
11. [Library Sync (PlugWorkflowManager)](#library-sync-plugworkflowmanager)
12. [File Transfer without SFTP](#file-transfer-without-sftp)
13. [Qt5 vs Qt6 Comparison](#qt5-vs-qt6-comparison)
14. [Optional: MCP Server for Remote Access](#optional-mcp-server-for-remote-access)

---

## Overview

Firmware v5 switched from Qt5 to Qt6, breaking existing tools like NickelMenu. This guide documents a working approach to:

- Get root SSH access over WiFi (Dropbear, 719KB static binary)
- Inject custom menu items into the native Tolino/Kobo UI
- Trigger library rescans programmatically (`PlugWorkflowManager::sync()`)
- Transfer files without SFTP (`cat | ssh` piping)
- Optionally expose the device via MCP for AI-assisted management

**Architecture:**
```
LD_PRELOAD → libtolinom.so (NickelHook PLT hooking)
  ├── Hooks Ui_MoreView::setupUi → injects "Scripts" button
  ├── Hooks MoreView::betaFeatures → intercepts button clicks
  ├── Poll thread → watches /tmp/tolinom-sync-request trigger file
  └── dlsym → all Qt6 interaction, no headers needed
```

---

## Prerequisites

- **Host**: Linux or WSL with `arm-linux-gnueabihf-gcc` and `arm-linux-gnueabihf-g++`
- **Device**: Tolino Shine Color (or Kobo Clara Color) with firmware v5
- **Network**: Device and host on same WiFi network

```bash
# Ubuntu/WSL
sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
```

---

## Step 1: Enable Devmode

Use the EnableDevmode update.tar from [notmarek/KoboTolinoFindings](https://github.com/notmarek/KoboTolinoFindings):

1. Download `update.tar` from the EnableDevmode folder
2. Connect device via USB
3. Copy `update.tar` to the root of the device storage
4. Eject and reboot — the device will process the update

> **Note**: `KoboRoot.tgz` does NOT work on Tolino firmware. You must use `update.tar` with a `driver.sh` payload.

---

## Step 2: Get Shell Access

The initial shell uses a Python3 TCP server (Python 3.10 is pre-installed on the device).

### Create the init script

Create `update.tar` containing `driver.sh` with this content:

```bash
#!/bin/sh

# Create init script for TCP shell
cat > /etc/init.d/S99custom << 'INITSCRIPT'
#!/bin/sh
case "$1" in
    start)
        # TCP shell (fallback access)
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
    stop) killall python3 2>/dev/null ;;
esac
INITSCRIPT
chmod +x /etc/init.d/S99custom
ln -sf /etc/init.d/S99custom /etc/rc5.d/S99custom
```

After reboot, connect via: `nc <device-ip> 4444`

---

## Step 3: Install Dropbear SSH

### Cross-compile Dropbear (static, ~719KB)

```bash
wget https://matt.ucc.asn.au/dropbear/releases/dropbear-2024.86.tar.bz2
tar xjf dropbear-2024.86.tar.bz2 && cd dropbear-2024.86

# Disable password auth (use key auth only)
cat > localoptions.h << 'EOF'
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_CLI_PASSWORD_AUTH 0
EOF

./configure --host=arm-linux-gnueabihf --disable-zlib --disable-lastlog \
    --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
    CC=arm-linux-gnueabihf-gcc LDFLAGS="-static" CFLAGS="-Os"

make PROGRAMS="dropbear dbclient dropbearkey scp" MULTI=1 STATIC=1 -j$(nproc)
arm-linux-gnueabihf-strip dropbearmulti
# Result: ~719KB static ARM binary
```

### Install on device

Copy `dropbearmulti` to the device as `/mnt/onboard/.adds/dropbearmulti`.

> **Important**: `/mnt/onboard/.adds/` is on the FAT32 user partition and persists across reboots. The root filesystem `/etc/` also persists, but `/usr/local/bin/` does NOT.

> **Important**: FAT32 does not support symlinks. Use the multi-call binary directly: `/mnt/onboard/.adds/dropbearmulti dropbear ...`

### Generate host keys and set up authorized_keys

```bash
# On the device (via TCP shell):
/mnt/onboard/.adds/dropbearmulti dropbearkey -t rsa -f /etc/dropbear_rsa_host_key
/mnt/onboard/.adds/dropbearmulti dropbearkey -t ed25519 -f /etc/dropbear_ed25519_host_key

# Set up SSH key auth
# Note: home directory is /home/root (NOT /root) per /etc/passwd
mkdir -p /home/root/.ssh
echo "your-ssh-public-key-here" > /home/root/.ssh/authorized_keys
chmod 700 /home/root/.ssh
chmod 600 /home/root/.ssh/authorized_keys

# Persist authorized_keys on user partition (restored on boot)
cp /home/root/.ssh/authorized_keys /mnt/onboard/.adds/authorized_keys

# Start dropbear
/mnt/onboard/.adds/dropbearmulti dropbear \
    -r /etc/dropbear_rsa_host_key \
    -r /etc/dropbear_ed25519_host_key \
    -p 2222 &
```

### Update init script to auto-start SSH

Update `/etc/init.d/S99custom` to start dropbear on boot:

```bash
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
        # TCP shell (fallback)
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
```

### Connect via SSH

```bash
ssh -p 2222 -i ~/.ssh/id_rsa root@<device-ip>
```

> **Note**: Dropbear survives device sleep. WiFi drops during sleep but reconnects on wake, and dropbear resumes accepting connections.

---

## Step 4: Build the Hook Library (tolinom)

### Source: `tolinom.cc`

See the full source in the [tolinom.cc section below](#tolinom-source).

### Build script: `build-tolinom.sh`

```bash
#!/bin/bash
set -e
cd /tmp && rm -rf tolinom && mkdir tolinom && cd tolinom

cp /path/to/tolinom.cc .
wget -q https://raw.githubusercontent.com/pgaskin/NickelHook/master/nh.c
wget -q https://raw.githubusercontent.com/pgaskin/NickelHook/master/NickelHook.h

# Critical: patch NickelHook to accept STB_WEAK symbols (Qt6 uses weak binding)
sed -i 's/ELFW(ST_BIND)(sym->st_info) == STB_GLOBAL/( ELFW(ST_BIND)(sym->st_info) == STB_GLOBAL || ELFW(ST_BIND)(sym->st_info) == STB_WEAK )/' nh.c

# First pass: compile to discover mangled symbol names
arm-linux-gnueabihf-gcc -c -fPIC -Os nh.c -o nh.o 2>/dev/null
arm-linux-gnueabihf-g++ -c -fPIC -Os -Wno-unused-result -Wno-unused-variable -Wno-unused-local-typedefs tolinom.cc -o tolinom.o
arm-linux-gnueabihf-g++ -shared -o libtolinom.so tolinom.o nh.o -ldl

SETUP=$(nm -D libtolinom.so | grep nm_hook_setupUi | awk '{print $3}')
BETA=$(nm -D libtolinom.so | grep nm_hook_betaFeatures | awk '{print $3}')

if [ -z "$SETUP" ] || [ -z "$BETA" ]; then
    echo "ERROR: symbols not found"; exit 1
fi

# Second pass: patch placeholder names and rebuild
sed -i "s|PLACEHOLDER_SETUP|$SETUP|" tolinom.cc
sed -i "s|PLACEHOLDER_BETA|$BETA|" tolinom.cc
rm -f *.o libtolinom.so

arm-linux-gnueabihf-gcc -c -fPIC -Os nh.c -o nh.o 2>/dev/null
arm-linux-gnueabihf-g++ -c -fPIC -Os -Wno-unused-result -Wno-unused-variable -Wno-unused-local-typedefs tolinom.cc -o tolinom.o
arm-linux-gnueabihf-g++ -shared -o libtolinom.so tolinom.o nh.o -ldl
arm-linux-gnueabihf-strip libtolinom.so
```

> **Why two-pass?** NickelHook's `sym_new` field needs the exact mangled C++ symbol name of the hook function. The name includes the encoded function name length, so we compile first to discover it via `nm -D`, then patch and rebuild.

> **Why compile nh.c separately?** NickelHook is C code. Compiling it as C++ (with g++) causes errors from C++ stricter type checking (goto over initialization, implicit casts). Compile nh.c with gcc and tolinom.cc with g++, then link together.

---

## Step 5: Install the Hook Library

### Deploy

```bash
# No SFTP on the device — use cat piping
cat libtolinom.so | ssh -p 2222 root@<device-ip> "cat > /mnt/onboard/.adds/libtolinom.so"
```

### Enable LD_PRELOAD

Add to `/etc/rc.local` (before the `nickel` launch line):

```bash
if [ -f /mnt/onboard/.adds/libtolinom.so ] && [ ! -f /mnt/onboard/.adds/tolinom.disabled ]; then
    export LD_PRELOAD=/mnt/onboard/.adds/libtolinom.so
fi
/usr/local/Kobo/nickel -platform kobo &
```

> **Safety**: Create `/mnt/onboard/.adds/tolinom.disabled` via USB to disable the hook if it causes boot loops. The 15-second failsafe delay in NickelHook also helps.

### Reboot

```bash
ssh -p 2222 root@<device-ip> "reboot"
```

After reboot, a "Scripts" button appears in the Mehr/More menu.

---

## Step 6: Shell Scripts

Place these in `/mnt/onboard/.adds/`:

### cloud-connect.sh
```bash
#!/bin/sh
if ! pidof dropbearmulti >/dev/null 2>&1; then
    /mnt/onboard/.adds/dropbearmulti dropbear \
        -r /etc/dropbear_rsa_host_key \
        -r /etc/dropbear_ed25519_host_key \
        -p 2222 &
    sleep 1
fi
if ! pidof dbclient >/dev/null 2>&1; then
    /mnt/onboard/.adds/dropbearmulti dbclient \
        -y -i /mnt/onboard/.adds/tolino_client_key \
        -N -f -R 2223:localhost:2222 \
        user@yourserver.com 2>/dev/null
fi
```

### cloud-disconnect.sh
```bash
#!/bin/sh
killall dbclient 2>/dev/null
```

### cloud-status.sh
```bash
#!/bin/sh
STATUS=""
if pidof dropbearmulti >/dev/null 2>&1; then
    STATUS="SSH: running"
else
    STATUS="SSH: stopped"
fi
if pidof dbclient >/dev/null 2>&1; then
    STATUS="$STATUS | Tunnel: active"
else
    STATUS="$STATUS | Tunnel: down"
fi
echo "$STATUS" > /mnt/onboard/cloud-status.txt
```

### sysinfo.sh
```bash
#!/bin/sh
(uname -a; echo "---"; free -m; echo "---"; df -h; echo "---"; uptime) > /mnt/onboard/sysinfo.txt
```

---

## Step 7: One-Shot Recovery (update.tar)

For factory reset recovery, create an `update.tar` that installs everything in one shot:

```bash
#!/bin/sh
# driver.sh — full setup payload

# 1. Init script (SSH + TCP shell)
cat > /etc/init.d/S99custom << 'EOF'
# ... (full init script from Step 3)
EOF
chmod +x /etc/init.d/S99custom
ln -sf /etc/init.d/S99custom /etc/rc5.d/S99custom

# 2. LD_PRELOAD injection in rc.local
# Patch rc.local to add LD_PRELOAD before nickel launch
sed -i '/\/usr\/local\/Kobo\/nickel/i \
if [ -f /mnt/onboard/.adds/libtolinom.so ] && [ ! -f /mnt/onboard/.adds/tolinom.disabled ]; then\n    export LD_PRELOAD=/mnt/onboard/.adds/libtolinom.so\nfi' /etc/rc.local

# 3. Generate SSH host keys
/mnt/onboard/.adds/dropbearmulti dropbearkey -t rsa -f /etc/dropbear_rsa_host_key 2>/dev/null
/mnt/onboard/.adds/dropbearmulti dropbearkey -t ed25519 -f /etc/dropbear_ed25519_host_key 2>/dev/null

# 4. Set up authorized_keys
mkdir -p /home/root/.ssh
cp /mnt/onboard/.adds/authorized_keys /home/root/.ssh/authorized_keys 2>/dev/null
chmod 700 /home/root/.ssh
chmod 600 /home/root/.ssh/authorized_keys 2>/dev/null
```

**Pre-stage on USB before factory reset:**
- `/mnt/onboard/.adds/dropbearmulti` — Dropbear SSH (static ARM binary)
- `/mnt/onboard/.adds/libtolinom.so` — Hook library
- `/mnt/onboard/.adds/authorized_keys` — Your SSH public key
- `/mnt/onboard/.adds/cloud-*.sh` — Shell scripts
- `update.tar` containing `driver.sh` — The setup payload

---

## Key Technical Findings for v5

### 1. NickelHook STB_WEAK fix
Qt6 nickel uses `STB_WEAK` binding for `Ui_*` classes. NickelHook only accepts `STB_GLOBAL`. One-line fix:
```c
// In nh.c, change:
ELFW(ST_BIND)(sym->st_info) == STB_GLOBAL
// To:
( ELFW(ST_BIND)(sym->st_info) == STB_GLOBAL || ELFW(ST_BIND)(sym->st_info) == STB_WEAK )
```

### 2. No Qt headers needed
All Qt6 interaction via `dlsym()`. Key pattern:
```cpp
struct FakeQString { void *d; char16_t *ptr; long size; };
// d=nullptr works for static strings in Qt6
```

### 3. ARM EABI hidden return parameter
`QObject::connect()` returns `QMetaObject::Connection` (non-trivial struct). On ARM EABI, this uses a hidden first parameter:
```cpp
struct FakeConnection { void *d_ptr; };
typedef void (*connect_t)(FakeConnection*, const void*, const char*,
                          const void*, const char*, int);
```

### 4. Signal-to-signal relay
Route button clicks through existing signals:
```cpp
fn_connect(&conn, btn, "2clicked()", widget, "2betaFeatures()", 0);
// "2" prefix = SIGNAL, "1" prefix = SLOT
```

### 5. ConfirmationDialog for user interaction
Custom QPushButtons inside dialogs don't receive taps (Tolino gesture system). Use `ConfirmationDialog` with `QDialog::exec()` for blocking sequential dialogs instead.

---

## Library Sync (PlugWorkflowManager)

The proper way to trigger a library rescan on firmware v5:

```cpp
// In-process (from hook library):
typedef void* (*getInstance_t)();
typedef void  (*sync_t)(void*);
getInstance_t fn_gi = (getInstance_t)dlsym(RTLD_DEFAULT,
    "_ZN19PlugWorkflowManager14sharedInstanceEv");
sync_t fn_sy = (sync_t)dlsym(RTLD_DEFAULT,
    "_ZN19PlugWorkflowManager4syncEv");
void *mgr = fn_gi();
if (mgr) fn_sy(mgr);
```

**Remote trigger** (from SSH/MCP): create `/tmp/tolinom-sync-request` — the poll thread picks it up within 150ms and calls sync:
```bash
ssh -p 2222 root@<device-ip> "touch /tmp/tolinom-sync-request"
```

> **Note**: `/tmp/nickel-hardware-status` is a FIFO. Writing `usb plug add` triggers the USB dialog. Writing `usb plug remove` alone does NOT trigger a rescan. `PlugWorkflowManager::sync()` is the reliable method.

---

## File Transfer without SFTP

Dropbear's multi-call SCP binary requires `sftp-server` on the remote, which is not installed on the Tolino. Use pipe-based transfer instead:

```bash
# Upload to device
cat local-file.epub | ssh -p 2222 root@<device-ip> "cat > /mnt/onboard/book.epub"

# Download from device
ssh -p 2222 root@<device-ip> "cat /mnt/onboard/book.epub" > local-file.epub
```

---

## Qt5 vs Qt6 Comparison

| Aspect | Qt5 (FW4) | Qt6 (FW5) |
|--------|-----------|-----------|
| NickelMenu | Works | Broken (PLT symbols changed) |
| Symbol binding | STB_GLOBAL | STB_WEAK for Ui_* classes |
| createMenuTextItem | In PLT | NOT in PLT |
| Hookable entry point | createMenuTextItem | Ui_MoreView::setupUi |
| QString ABI | QStringData* d_ptr | { void* d, char16_t* ptr, long size } |
| connect() return | Same | Same (ARM EABI hidden param) |
| KoboRoot.tgz | Works on Kobo | Does NOT work on Tolino |

---

## Optional: MCP Server for Remote Access

An MCP (Model Context Protocol) server can bridge the Tolino to Claude AI or any MCP client, allowing voice/text commands like "download today's newspaper to my e-reader."

**Architecture:**
```
claude.ai/phone --> https://tolino.yourserver.com/mcp (OAuth + MCP)
               --> Apache reverse proxy (port 443)
               --> MCP server (Python FastMCP, port 8900)
               --> reverse SSH tunnel (port 2223)
               --> Tolino dropbear (port 2222)
```

**Components:**
- Reverse SSH tunnel from Tolino to your server (`dbclient -R`)
- Python MCP server with tools: status, shell, fetch_url, write_file, upload, download, refresh_library
- OAuth 2.0 for authentication
- Apache/nginx reverse proxy with Let's Encrypt SSL

See the [tolino-mcp repository](https://github.com/youruser/tolino-mcp) for the server source and setup instructions.

---

## tolinom source

The complete `tolinom.cc` source is maintained alongside this guide. Key entry points:

- `nm_hook_setupUi` — Injects "Scripts" button into the Mehr/More menu
- `nm_hook_betaFeatures` — Intercepts button clicks via signal relay
- `show_scripts_dialog` — Sequential ConfirmationDialogs for script selection
- `do_library_sync` — Calls `PlugWorkflowManager::sync()` via dlsym
- `poll_thread` — Watches `/tmp/tolinom-sync-request` for remote sync triggers
