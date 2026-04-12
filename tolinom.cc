#include <dlfcn.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <pthread.h>
#include "NickelHook.h"

// Script button tracking for pthread-based click detection
#define MAX_SCRIPT_BTNS 8
struct ScriptEntry {
    void *btn;
    const char *cmd; // shell command to run
};
static ScriptEntry g_scriptBtns[MAX_SCRIPT_BTNS];
static int g_scriptBtnCount = 0;
static pthread_t g_pollThread;
static bool g_pollRunning = false;

// Panel auto-refresh: set when the Tolino Hacks dialog is being exec()'d,
// cleared when it returns. The poll thread samples wifi/ssh/tunnel state
// periodically and, if anything changes while the dialog is open, queues
// QDialog::accept() on the main thread so the panel's for-loop rebuilds
// it with fresh data. Users no longer see a stale "SSH connectable" row
// after the WiFi drops.
static void * volatile g_panelDlg = nullptr;
static char g_panelIp[32] = {0};
static int  g_panelSsh   = 0;
static int  g_panelTun   = 0;

struct QWidget;

static FILE *dbg=nullptr;
static void dbglog(const char *fmt,...){
    if(!dbg)dbg=fopen("/mnt/onboard/tolinom-debug.txt","a");
    if(!dbg)return;
    va_list ap;va_start(ap,fmt);vfprintf(dbg,fmt,ap);va_end(ap);
    fputc('\n',dbg);fflush(dbg);
}

// Qt6 QString: { QArrayData* d, char16_t* ptr, qsizetype size }
struct FakeQString { void *d; char16_t *ptr; long size; };
#define STACK_QS(v, lit, n) \
    static const char16_t v##data[] = lit; \
    FakeQString v = {nullptr,(char16_t*)v##data,n};

// ARM EABI: non-trivial return types use hidden first parameter
struct FakeConnection { void *d_ptr; };

// Function pointer types
typedef void  (*setupUi_t)(void*, QWidget*);
typedef void  (*void_fn_t)(void*);                          // generic this->method()
typedef void  (*ctor_t)(void*, const void*, QWidget*);      // QPushButton(this, text, parent)
typedef void  (*addWidget_t)(void*, void*, int, int);       // QBoxLayout::addWidget
typedef void* (*layout_t)(QWidget*);                        // QWidget::layout()
typedef void* (*op_new_t)(unsigned int);                    // operator new
typedef void  (*setBool_t)(void*, bool);                    // generic setBool method
typedef void  (*setSS_t)(void*, const void*);               // QWidget::setStyleSheet(QString)
typedef void  (*setQS_t)(void*, const void*);               // generic setText/setTitle(QString)
typedef void  (*addW_t)(void*, void*);                      // addWidget(QWidget*)
typedef void  (*connect_t)(FakeConnection*, const void*, const char*,
                           const void*, const char*, int);
typedef bool  (*isChecked_t)(const void*);                  // QAbstractButton::isChecked()
typedef void  (*dlgCtor_t)(void*, void*);                   // ConfirmationDialog(QWidget*)
typedef void  (*show_t)(void*);                             // QWidget::show()

// Original function pointers (set by NickelHook)
static setupUi_t orig_setupUi = nullptr;
static void_fn_t orig_betaFeatures = nullptr;
static void_fn_t orig_suspend = nullptr;
typedef void (*suspendQS_t)(void*, const void*);
static suspendQS_t orig_suspendDevice = nullptr;
static suspendQS_t orig_suspendDeviceWithAlarm = nullptr;

// Resolved Qt symbols
static ctor_t     fn_btnCtor    = nullptr;
static addWidget_t fn_addWidget = nullptr;
static layout_t   fn_layout    = nullptr;
static op_new_t   fn_new       = nullptr;
static setBool_t  fn_setFlat   = nullptr;
static setSS_t    fn_setSS     = nullptr;
static setBool_t  fn_setObjName = nullptr;
static connect_t  fn_connect   = nullptr;
static setBool_t  fn_setCheckable = nullptr;
static isChecked_t fn_isChecked = nullptr;
static dlgCtor_t  fn_dlgCtor   = nullptr;
static setQS_t   fn_setTitle   = nullptr;
static setBool_t  fn_showClose = nullptr;
static addW_t    fn_dlgAddWidget = nullptr;
static show_t    fn_show       = nullptr;
static setQS_t  fn_setObjNameQS = nullptr;

// Global state
static void *g_scriptsBtn = nullptr;

static void resolve_syms() {
    if (fn_btnCtor) return;
    fn_btnCtor     = (ctor_t)    dlsym(RTLD_DEFAULT, "_ZN11QPushButtonC1ERK7QStringP7QWidget");
    fn_layout      = (layout_t)  dlsym(RTLD_DEFAULT, "_ZNK7QWidget6layoutEv");
    fn_addWidget   = (addWidget_t)dlsym(RTLD_DEFAULT, "_ZN10QBoxLayout9addWidgetEP7QWidgeti6QFlagsIN2Qt13AlignmentFlagEE");
    fn_new         = (op_new_t)  dlsym(RTLD_DEFAULT, "_Znwj");
    fn_setFlat     = (setBool_t) dlsym(RTLD_DEFAULT, "_ZN11QPushButton7setFlatEb");
    fn_setSS       = (setSS_t)   dlsym(RTLD_DEFAULT, "_ZN7QWidget13setStyleSheetERK7QString");
    fn_connect     = (connect_t) dlsym(RTLD_DEFAULT, "_ZN7QObject7connectEPKS_PKcS1_S3_N2Qt14ConnectionTypeE");
    fn_setCheckable= (setBool_t) dlsym(RTLD_DEFAULT, "_ZN15QAbstractButton12setCheckableEb");
    fn_isChecked   = (isChecked_t)dlsym(RTLD_DEFAULT, "_ZNK15QAbstractButton9isCheckedEv");
    fn_setObjNameQS= (setQS_t)  dlsym(RTLD_DEFAULT, "_ZN7QObject13setObjectNameERK7QString");

    // ConfirmationDialog symbols
    fn_dlgCtor     = (dlgCtor_t) dlsym(RTLD_DEFAULT, "_ZN18ConfirmationDialogC1EP7QWidget");
    fn_setTitle    = (setQS_t)   dlsym(RTLD_DEFAULT, "_ZN18ConfirmationDialog8setTitleERK7QString");
    fn_showClose   = (setBool_t) dlsym(RTLD_DEFAULT, "_ZN18ConfirmationDialog15showCloseButtonEb");
    fn_dlgAddWidget= (addW_t)   dlsym(RTLD_DEFAULT, "_ZN18ConfirmationDialog9addWidgetEP7QWidget");
    fn_show        = (show_t)   dlsym(RTLD_DEFAULT, "_ZN7QWidget4showEv");

    dbglog("syms resolved: dlgCtor=%p setTitle=%p showClose=%p dlgAdd=%p show=%p isChk=%p",
        fn_dlgCtor, fn_setTitle, fn_showClose, fn_dlgAddWidget, fn_show, fn_isChecked);
}

static void do_library_sync();

// Helper: convert ASCII string to char16_t buffer, return length.
// Newlines are preserved — the dialog renders them as line breaks.
static int ascii_to_u16(const char *src, char16_t *dst, int maxlen) {
    int i = 0;
    for (; src[i] && i < maxlen - 1; i++)
        dst[i] = (char16_t)src[i];
    return i;
}

// Helper: run command and capture first line of output
static int run_capture(const char *cmd, char *buf, int buflen) {
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    buf[0] = 0;
    fgets(buf, buflen, p);
    pclose(p);
    // strip trailing newline
    int n = strlen(buf);
    if (n > 0 && buf[n-1] == '\n') buf[--n] = 0;
    return n;
}

static void show_panel(void *parent) {
    dbglog("show_panel");
    if (!fn_dlgCtor || !fn_new || !fn_setTitle) {
        dbglog("missing dialog syms");
        return;
    }

    // Resolve symbols
    typedef void (*setText_t)(void*, const void*);
    typedef void (*setAccBtnText_t)(void*, const void*);
    typedef void (*setRejVisible_t)(void*, bool);
    typedef int  (*exec_t)(void*);

    static setText_t       fn_setText    = nullptr;
    static setAccBtnText_t fn_setAccBtn = nullptr;
    static setRejVisible_t fn_setRejVis = nullptr;
    static exec_t          fn_exec      = nullptr;

    if (!fn_setText)    fn_setText    = (setText_t)         dlsym(RTLD_DEFAULT, "_ZN18ConfirmationDialog7setTextERK7QString");
    if (!fn_setAccBtn)  fn_setAccBtn = (setAccBtnText_t)   dlsym(RTLD_DEFAULT, "_ZN18ConfirmationDialog19setAcceptButtonTextERK7QString");
    if (!fn_setRejVis)  fn_setRejVis = (setRejVisible_t)   dlsym(RTLD_DEFAULT, "_ZN18ConfirmationDialog22setRejectButtonVisibleEb");
    if (!fn_exec)       fn_exec      = (exec_t)            dlsym(RTLD_DEFAULT, "_ZN7QDialog4execEv");

    if (!fn_exec || !fn_dlgAddWidget) {
        dbglog("missing panel syms: exec=%p addW=%p", fn_exec, fn_dlgAddWidget);
        return;
    }

    for (;;) {
        // --- Read current state ---
        // dbclient runs via the dropbearmulti multi-call binary, so argv[0]
        // is "dropbearmulti" and pidof dbclient never matches. Detect both
        // the server and the tunnel client by full command line match.
        // The `[d]` character-class trick stops pgrep from matching the
        // parent shell that system() spawns (its cmdline contains the
        // literal pattern string).
        bool ssh_on = (system(
            "pgrep -f '[d]ropbearmulti dropbear' >/dev/null 2>&1") == 0);
        bool tunnel_on = (system(
            "pgrep -f '[d]ropbearmulti dbclient' >/dev/null 2>&1") == 0);

        // Always read the IP so we can detect WiFi dropping mid-dialog.
        char ip[32] = {0};
        run_capture("ip -4 addr show wlan0 2>/dev/null | grep -o 'inet [0-9.]*' | cut -d' ' -f2", ip, sizeof(ip));

        // Snapshot for the poll thread auto-refresh watchdog
        strncpy(g_panelIp, ip, sizeof(g_panelIp) - 1);
        g_panelIp[sizeof(g_panelIp) - 1] = 0;
        g_panelSsh = ssh_on ? 1 : 0;
        g_panelTun = tunnel_on ? 1 : 0;

        // Resolve SSH connection details (shown in the sub-label under the SSH row)
        char sshInfo[256] = {0};
        if (ssh_on) {
            char pw[32] = {0};
            run_capture(
                "SERIAL=$(cat /mnt/onboard/.kobo/version 2>/dev/null | cut -d',' -f1);"
                "MAC=$(cat /sys/class/net/wlan0/address 2>/dev/null);"
                "printf '%s' \"tolino-hacks-ssh:${SERIAL}:${MAC}\" | md5sum | cut -c1-12",
                pw, sizeof(pw));
            dbglog("panel: ip='%s' pw='%s' ssh_on=%d", ip, pw, ssh_on);
            if (ip[0] && pw[0])
                snprintf(sshInfo, sizeof(sshInfo),
                    "ssh -p 2222 root@%s\nPassword: %s\nWiFi + wake-lock kept alive",
                    ip, pw);
            else if (ip[0])
                snprintf(sshInfo, sizeof(sshInfo),
                    "ssh -p 2222 root@%s\nWiFi + wake-lock kept alive", ip);
            else
                snprintf(sshInfo, sizeof(sshInfo), "on (no WiFi)");
        } else {
            snprintf(sshInfo, sizeof(sshInfo), "off");
        }

        // Tunnel sub-label: when active, surface the server details read from
        // .env so the user can see which endpoint the tunnel is using and how
        // an MCP client would reach it. TUNNEL_MCP_URL is optional.
        char tunInfoBuf[256] = {0};
        if (tunnel_on) {
            char host[64] = {0};
            char port[16] = {0};
            char mcp[128] = {0};
            run_capture(". /mnt/onboard/.adds/.env 2>/dev/null; printf '%s' \"${TUNNEL_HOST}\"", host, sizeof(host));
            run_capture(". /mnt/onboard/.adds/.env 2>/dev/null; printf '%s' \"${TUNNEL_PORT:-2223}\"", port, sizeof(port));
            run_capture(". /mnt/onboard/.adds/.env 2>/dev/null; printf '%s' \"${TUNNEL_MCP_URL}\"", mcp, sizeof(mcp));
            if (host[0] && mcp[0])
                snprintf(tunInfoBuf, sizeof(tunInfoBuf),
                    "%s:%s -> localhost:2222\nMCP: %s", host, port, mcp);
            else if (host[0])
                snprintf(tunInfoBuf, sizeof(tunInfoBuf),
                    "%s:%s -> localhost:2222", host, port);
            else
                snprintf(tunInfoBuf, sizeof(tunInfoBuf), "connected");
        } else {
            snprintf(tunInfoBuf, sizeof(tunInfoBuf), "off");
        }
        const char *tunInfo = tunInfoBuf;

        // --- Build dialog ---
        void *dlg = fn_new(32768);
        if (!dlg) break;
        fn_dlgCtor(dlg, parent);
        STACK_QS(title, u"Tolino Hacks", 12);
        fn_setTitle(dlg, &title);
        if (fn_showClose) fn_showClose(dlg, true);
        if (fn_setRejVis) fn_setRejVis(dlg, false);

        // --- Toggle rows using native MenuTextItem (same widget as Mehr menu) ---
        typedef void (*menuItemCtor_t)(void*, void*, bool, bool);
        typedef void (*menuItemSetText_t)(void*, const void*);
        typedef void (*menuItemRegGestures_t)(void*);
        static menuItemCtor_t fn_miCtor = nullptr;
        static menuItemSetText_t fn_miSetText = nullptr;
        static menuItemRegGestures_t fn_miRegGest = nullptr;
        static setRejVisible_t fn_setAccVis = nullptr;
        if (!fn_miCtor)    fn_miCtor    = (menuItemCtor_t)dlsym(RTLD_DEFAULT, "_ZN12MenuTextItemC1EP7QWidgetbb");
        if (!fn_miSetText) fn_miSetText = (menuItemSetText_t)dlsym(RTLD_DEFAULT, "_ZN12MenuTextItem7setTextERK7QString");
        if (!fn_miRegGest) fn_miRegGest = (menuItemRegGestures_t)dlsym(RTLD_DEFAULT, "_ZN12MenuTextItem22registerForTapGesturesEv");
        if (!fn_setAccVis) fn_setAccVis = (setRejVisible_t)dlsym(RTLD_DEFAULT, "_ZN18ConfirmationDialog22setAcceptButtonVisibleEb");
        if (!fn_miCtor) { dbglog("no MenuTextItem"); break; }

        // Style override: remove italic/bold quirks, keep native layout
        STACK_QS(miSS, u"*{font-style:normal;}", 21);

        auto makeMenuRow = [&](const char16_t *text, int len, bool checkable, bool checked) -> void* {
            void *w = fn_new(32768);
            fn_miCtor(w, nullptr, checkable, checked);
            FakeQString qs = {nullptr, (char16_t*)text, len};
            fn_miSetText(w, &qs);
            if (fn_setSS) fn_setSS(w, &miSS);
            fn_dlgAddWidget(dlg, w);
            // Register gestures AFTER the widget is parented into the dialog,
            // otherwise the first tap is swallowed by focus/gesture bootstrap.
            if (fn_miRegGest) fn_miRegGest(w);
            return w;
        };

        // Sub-label rendered as a non-interactive TouchLabel under each row.
        typedef void (*touchLblCtor_t)(void*, const void*, void*, int);
        typedef void (*touchLblInit_t)(void*);
        static touchLblCtor_t fn_touchCtor = nullptr;
        static touchLblInit_t fn_touchInit = nullptr;
        if (!fn_touchCtor) fn_touchCtor = (touchLblCtor_t)dlsym(RTLD_DEFAULT, "_ZN10TouchLabelC1ERK7QStringP7QWidget6QFlagsIN2Qt10WindowTypeEE");
        if (!fn_touchInit) fn_touchInit = (touchLblInit_t)dlsym(RTLD_DEFAULT, "_ZN10TouchLabel10initializeEv");

        STACK_QS(subSS,
            u"QLabel{font-size:9pt;font-weight:300;color:#333;"
             "padding:4px 48px 14px 48px;}",
            76);

        // Sub-label text buffers must outlive fn_exec(dlg), so they live in the
        // loop iteration scope rather than inside the lambda (where stack reuse
        // would leave Qt reading garbage and rendering it as CJK).
        char16_t sshInfoU[256];
        char16_t tunInfoU[256];
        int sshInfoLen = ascii_to_u16(sshInfo, sshInfoU, 256);
        int tunInfoLen = ascii_to_u16(tunInfo, tunInfoU, 256);
        FakeQString sshInfoQS = {nullptr, sshInfoU, sshInfoLen};
        FakeQString tunInfoQS = {nullptr, tunInfoU, tunInfoLen};

        auto makeSubLabel = [&](const FakeQString *qs) {
            if (!fn_touchCtor || qs->size == 0) return;
            void *w = fn_new(32768);
            fn_touchCtor(w, qs, nullptr, 0);
            if (fn_touchInit) fn_touchInit(w);
            if (fn_setSS) fn_setSS(w, &subSS);
            fn_dlgAddWidget(dlg, w);
        };

        // Prefix toggle rows with a Unicode checkbox glyph so the state is obvious.
        // ☑ = U+2611, ☐ = U+2610. Action rows get a refresh glyph.
        void *sshTouch = makeMenuRow(
            ssh_on ? u"\u2611  SSH Server" : u"\u2610  SSH Server", 13, false, false);
        makeSubLabel(&sshInfoQS);

#ifndef TOLINOM_PUBLIC
        // Reverse tunnel is a personal-build feature — it needs a server the
        // user controls. Public builds hide the row entirely.
        void *tunTouch = makeMenuRow(
            tunnel_on ? u"\u2611  Reverse Tunnel" : u"\u2610  Reverse Tunnel", 17, false, false);
        makeSubLabel(&tunInfoQS);
#else
        void *tunTouch = nullptr;
        (void)tunnel_on; (void)tunInfoQS;
#endif

        void *refreshTouch = makeMenuRow(
            u"\u21BB  Refresh Library", 18, false, false);

        // WiFi reconnect action (useful after nickel drops the radio on sleep).
        void *wifiTouch = makeMenuRow(
            u"\u21BB  Reconnect WiFi", 17, false, false);

        // Hidden trackers — tap toggles tracker AND closes dialog.
        // Slots for ssh / tunnel / refresh / wifi; the tunnel slot is nullptr
        // in public builds and is simply skipped.
        STACK_QS(emptyStr, u"", 0);
        void *trackers[4] = {nullptr, nullptr, nullptr, nullptr};
        void *touches[4]  = {sshTouch, tunTouch, refreshTouch, wifiTouch};
        for (int i = 0; i < 4; i++) {
            if (!touches[i]) continue;
            trackers[i] = fn_new(32768);
            fn_btnCtor(trackers[i], &emptyStr, nullptr);
            fn_setCheckable(trackers[i], true);
            FakeConnection ct = {nullptr};
            fn_connect(&ct, touches[i], "2tapped(bool)", trackers[i], "1toggle()", 0);
            FakeConnection ca = {nullptr};
            fn_connect(&ca, touches[i], "2tapped(bool)", dlg, "1accept()", 0);
        }

        // Hide the accept button entirely (tapping rows auto-closes)
        if (fn_setAccVis) fn_setAccVis(dlg, false);

        // --- Show and wait ---
        // Expose dlg to the poll thread so it can auto-close on state change.
        g_panelDlg = dlg;
        dbglog("panel: showing (ssh=%d tunnel=%d)", ssh_on, tunnel_on);
        int result = fn_exec(dlg);
        g_panelDlg = nullptr;
        dbglog("panel: exec returned %d", result);

        if (result != 1) break; // closed

        // --- Apply: tracker checked = user tapped it (toggle from prior state) ---
        if (trackers[0] && fn_isChecked(trackers[0])) {
            if (ssh_on) {
                dbglog("panel: stopping SSH");
                system("/mnt/onboard/.adds/ssh-stop.sh");
            } else {
                dbglog("panel: starting SSH");
                system("/mnt/onboard/.adds/ssh-start.sh");
            }
        }
        if (trackers[1] && fn_isChecked(trackers[1])) {
            if (tunnel_on) {
                dbglog("panel: stopping tunnel");
                system("/mnt/onboard/.adds/cloud-disconnect.sh");
            } else {
                dbglog("panel: starting tunnel");
                system("/mnt/onboard/.adds/cloud-connect.sh &");
            }
        }
        if (trackers[2] && fn_isChecked(trackers[2])) {
            dbglog("panel: refreshing library");
            do_library_sync();
        }
        if (trackers[3] && fn_isChecked(trackers[3])) {
            dbglog("panel: reconnecting wifi");
            system("/mnt/onboard/.adds/wifi-reconnect.sh &");
        }

        // Loop back to show updated status
    }
    dbglog("panel closed");
}

// Hook: Ui_MoreView::setupUi - creates the Scripts button
__attribute__((visibility("default")))
void nm_hook_setupUi(void *_this, QWidget *widget) {
    dbglog("setupUi called");
    orig_setupUi(_this, widget);
    resolve_syms();

    if (!fn_btnCtor || !fn_layout || !fn_addWidget || !fn_new || !fn_connect) return;

    void *layout = fn_layout(widget);
    if (!layout) { dbglog("no layout"); return; }

    // Ui_MoreView struct (from uic):
    // [0] moreViewLayout  [1] moreContainer  [2] moreContainerLayout
    // [3..17] buttons (activity, articles, settings, help, betaFeatures, etc.)
    void **ui = (void**)_this;
    void *containerLayout = ui[2];
    dbglog("containerLayout=%p", containerLayout);

    // Create MenuTextItem — the native Kobo menu button widget
    typedef void (*menuItemCtor_t)(void*, void*, bool, bool);
    typedef void (*menuItemSetText_t)(void*, const void*);
    typedef void (*menuItemRegGestures_t)(void*);
    menuItemCtor_t fn_miCtor = (menuItemCtor_t)dlsym(RTLD_DEFAULT, "_ZN12MenuTextItemC1EP7QWidgetbb");
    menuItemSetText_t fn_miSetText = (menuItemSetText_t)dlsym(RTLD_DEFAULT, "_ZN12MenuTextItem7setTextERK7QString");
    menuItemRegGestures_t fn_miRegGest = (menuItemRegGestures_t)dlsym(RTLD_DEFAULT, "_ZN12MenuTextItem22registerForTapGesturesEv");

    if (!fn_miCtor) {
        dbglog("no MenuTextItem constructor");
        return;
    }

    void *btn = fn_new(32768);
    if (!btn) return;
    // MenuTextItem(QWidget* parent, bool checkable, bool checked)
    fn_miCtor(btn, (void*)ui[1], false, false);
    STACK_QS(label, u"Tolino Hacks", 12);
    fn_miSetText(btn, &label);
    if (fn_miRegGest) fn_miRegGest(btn);

    // Force non-italic font (MenuTextItem defaults to italic for some states)
    if (fn_setSS) {
        STACK_QS(css, u"*{font-style:normal;font-weight:normal;}", 41);
        fn_setSS(btn, &css);
    }

    // Append to moreContainerLayout (bottom of the list), wrapped in a
    // little spacing so it's visually separated from the native items.
    typedef void (*addSpacing_t)(void*, int);
    addSpacing_t fn_addSpacing = (addSpacing_t)dlsym(RTLD_DEFAULT, "_ZN10QBoxLayout10addSpacingEi");

    // QFrame separator below the button
    typedef void (*qframeCtor_t)(void*, void*, int);
    typedef void (*setFrameShape_t)(void*, int);
    qframeCtor_t fn_frameCtor = (qframeCtor_t)dlsym(RTLD_DEFAULT, "_ZN6QFrameC1EP7QWidget6QFlagsIN2Qt10WindowTypeEE");
    setFrameShape_t fn_setShape = (setFrameShape_t)dlsym(RTLD_DEFAULT, "_ZN6QFrame13setFrameShapeENS_5ShapeE");

    void *targetLayout = (containerLayout && fn_addWidget) ? containerLayout : layout;
    if (fn_addSpacing) fn_addSpacing(targetLayout, 18);
    fn_addWidget(targetLayout, btn, 0, 0);
    if (fn_addSpacing) fn_addSpacing(targetLayout, 18);

    if (fn_frameCtor && fn_setShape) {
        typedef void (*setFrameShadow_t2)(void*, int);
        typedef void (*setLineWidth_t)(void*, int);
        setFrameShadow_t2 fn_setShadow = (setFrameShadow_t2)dlsym(RTLD_DEFAULT, "_ZN6QFrame14setFrameShadowENS_6ShadowE");
        setLineWidth_t    fn_setLW     = (setLineWidth_t)   dlsym(RTLD_DEFAULT, "_ZN6QFrame12setLineWidthEi");
        void *sep = fn_new(2048);
        if (sep) {
            fn_frameCtor(sep, (void*)ui[1], 0);
            fn_setShape(sep, 4); // QFrame::HLine
            if (fn_setShadow) fn_setShadow(sep, 16); // QFrame::Plain
            if (fn_setLW)     fn_setLW(sep, 2);
            if (fn_setSS) {
                STACK_QS(sepCss, u"QFrame{color:#888;margin:0 24px;}", 33);
                fn_setSS(sep, &sepCss);
            }
            fn_addWidget(targetLayout, sep, 0, 0);
        }
    }

    // Hidden QPushButton shim — bridges MenuTextItem::tapped to betaFeatures signal
    // (Same pattern NickelMenu uses)
    STACK_QS(emptyStr, u"", 0);
    void *shim = fn_new(32768);
    fn_btnCtor(shim, &emptyStr, widget);
    fn_setCheckable(shim, true);

    // MenuTextItem::tapped(bool) -> shim::toggle()
    FakeConnection conn = {nullptr};
    fn_connect(&conn, btn, "2tapped(bool)", shim, "1toggle()", 0);

    // shim::toggled(bool) -> MoreView::betaFeatures() signal
    FakeConnection conn2 = {nullptr};
    fn_connect(&conn2, shim, "2toggled(bool)", widget, "2betaFeatures()", 0);
    dbglog("MenuTextItem=%p shim=%p betaFeatures on %p", btn, shim, widget);

    g_scriptsBtn = shim;  // track the shim, not the MenuTextItem
}

// Hook: MoreView::betaFeatures - intercept when Scripts button triggers it
__attribute__((visibility("default")))
void nm_hook_betaFeatures(void *_this) {
    dbglog("betaFeatures called, this=%p scriptsBtn=%p", _this, g_scriptsBtn);

    // Check if triggered by our Scripts button
    if (g_scriptsBtn && fn_isChecked && fn_isChecked(g_scriptsBtn)) {
        dbglog("triggered by Scripts button!");
        // Uncheck the shim — but block signals first so resetting
        // doesn't fire toggled(false) → betaFeatures() → real Beta dialog
        typedef bool (*blockSig_t)(void*, bool);
        typedef void (*setChecked_t)(void*, bool);
        blockSig_t fn_block = (blockSig_t)dlsym(RTLD_DEFAULT, "_ZN7QObject12blockSignalsEb");
        setChecked_t fn_sc = (setChecked_t)dlsym(RTLD_DEFAULT, "_ZN15QAbstractButton10setCheckedEb");
        bool prev = false;
        if (fn_block) prev = fn_block(g_scriptsBtn, true);
        if (fn_sc) fn_sc(g_scriptsBtn, false);
        if (fn_block) fn_block(g_scriptsBtn, prev);

        // Show our custom dialog
        show_panel(_this);
        return;
    }

    // Not our button - call original betaFeatures
    dbglog("calling original betaFeatures");
    orig_betaFeatures(_this);
}

// Helper: reset the PowerManager idle timer so nickel's next idle-check
// starts over instead of immediately re-requesting sleep.
static void pm_poke(void *_this) {
    typedef void (*upd_t)(void*);
    static upd_t fn_upd = nullptr;
    if (!fn_upd) fn_upd = (upd_t)dlsym(RTLD_DEFAULT,
        "_ZN12PowerManager14updateLastUsedEv");
    if (fn_upd && _this) fn_upd(_this);
}

static bool keepawake_active() {
    return access("/tmp/tolinom-keepawake", F_OK) == 0;
}

// Hooks: PowerManager sleep entry points. Multiple variants exist on this
// firmware and we don't know which one nickel actually hits, so we block
// them all when the keepawake flag is set. Only log when actually blocking
// — logging every passthrough would grow the debug file indefinitely.
__attribute__((visibility("default")))
void nm_hook_suspend(void *_this) {
    if (keepawake_active()) {
        dbglog("PM::suspend() blocked");
        pm_poke(_this);
        return;
    }
    if (orig_suspend) orig_suspend(_this);
}

__attribute__((visibility("default")))
void nm_hook_suspendDevice(void *_this, const void *reason) {
    if (keepawake_active()) {
        dbglog("PM::suspendDevice() blocked");
        pm_poke(_this);
        return;
    }
    if (orig_suspendDevice) orig_suspendDevice(_this, reason);
}

__attribute__((visibility("default")))
void nm_hook_suspendDeviceWithAlarm(void *_this, const void *reason) {
    if (keepawake_active()) {
        dbglog("PM::suspendDeviceWithAlarm() blocked");
        pm_poke(_this);
        return;
    }
    if (orig_suspendDeviceWithAlarm) orig_suspendDeviceWithAlarm(_this, reason);
}

// Trigger file: MCP or shell scripts can create this to request a library sync
#define SYNC_TRIGGER "/tmp/tolinom-sync-request"

static volatile int g_syncInProgress = 0;

// Direct sync call — only safe from the main (Qt/nickel) thread.
static void do_library_sync() {
    if (__sync_lock_test_and_set(&g_syncInProgress, 1)) {
        dbglog("sync already in progress, skipping");
        return;
    }
    typedef void* (*getInstance_t)();
    typedef void  (*sync_t)(void*);
    static getInstance_t fn_gi = nullptr;
    static sync_t fn_sy = nullptr;
    if (!fn_gi) fn_gi = (getInstance_t)dlsym(RTLD_DEFAULT,
        "_ZN19PlugWorkflowManager14sharedInstanceEv");
    if (!fn_sy) fn_sy = (sync_t)dlsym(RTLD_DEFAULT,
        "_ZN19PlugWorkflowManager4syncEv");
    if (fn_gi && fn_sy) {
        void *mgr = fn_gi();
        if (mgr) {
            dbglog("triggering PlugWorkflowManager::sync()");
            fn_sy(mgr);
        }
    }
    __sync_lock_release(&g_syncInProgress);
}

// Queue sync to run on the main thread via QTimer::singleShot.
// Safe to call from any thread. Falls back to direct call if sync()
// isn't a registered Qt slot (which prints a warning but doesn't crash).
static void do_library_sync_queued() {
    if (__sync_fetch_and_add(&g_syncInProgress, 0)) {
        dbglog("sync already in progress, skipping queued request");
        return;
    }
    // QTimer::singleShot(int msec, const QObject *receiver, const char *member)
    typedef void (*singleShot_t)(int, const void*, const char*);
    static singleShot_t fn_ss = nullptr;
    if (!fn_ss) fn_ss = (singleShot_t)dlsym(RTLD_DEFAULT,
        "_ZN6QTimer10singleShotEiPK7QObjectPKc");

    typedef void* (*getInstance_t)();
    static getInstance_t fn_gi = nullptr;
    if (!fn_gi) fn_gi = (getInstance_t)dlsym(RTLD_DEFAULT,
        "_ZN19PlugWorkflowManager14sharedInstanceEv");

    if (fn_ss && fn_gi) {
        void *mgr = fn_gi();
        if (mgr) {
            // "1sync()" = SLOT(sync()) — queues on receiver's thread (main thread)
            dbglog("queuing sync via QTimer::singleShot on main thread");
            fn_ss(0, mgr, "1sync()");
            return;
        }
    }
    // Fallback: direct call (if QTimer or PlugWorkflowManager not available)
    dbglog("WARNING: QTimer::singleShot unavailable, direct sync call from poll thread");
    do_library_sync();
}

// Poll thread: checks button states and watches for sync trigger file
static void *poll_thread(void *) {
    typedef void (*setChecked_t)(void*, bool);
    setChecked_t fn_sc = nullptr;
    int panel_watchdog_counter = 0;
    while (g_pollRunning) {
        usleep(150000); // 150ms
        // Check for sync trigger from MCP/shell (always, even before syms resolved)
        // Use queued version — marshals to main thread via QTimer::singleShot
        if (access(SYNC_TRIGGER, F_OK) == 0) {
            unlink(SYNC_TRIGGER);
            do_library_sync_queued();
        }

        // Panel auto-refresh watchdog. Every ~3s while the panel is visible,
        // resample wifi/ssh/tunnel state. If anything changed since the panel
        // snapshot, queue dlg->accept() on the main thread and clear g_panelDlg
        // so we don't fire twice. The panel's for-loop then re-reads state and
        // rebuilds the dialog with fresh info.
        if (g_panelDlg && ++panel_watchdog_counter >= 20) {
            panel_watchdog_counter = 0;
            char ip_now[32] = {0};
            run_capture(
                "ip -4 addr show wlan0 2>/dev/null | grep -o 'inet [0-9.]*' | cut -d' ' -f2",
                ip_now, sizeof(ip_now));
            int ssh_now = (system("pgrep -f '[d]ropbearmulti dropbear' >/dev/null 2>&1") == 0) ? 1 : 0;
            int tun_now = (system("pgrep -f '[d]ropbearmulti dbclient' >/dev/null 2>&1") == 0) ? 1 : 0;
            if (ssh_now != g_panelSsh || tun_now != g_panelTun
                || strncmp(ip_now, g_panelIp, sizeof(g_panelIp)) != 0) {
                void *dlg = g_panelDlg;
                g_panelDlg = nullptr;
                typedef void (*singleShot_t)(int, const void*, const char*);
                static singleShot_t fn_ss = nullptr;
                if (!fn_ss) fn_ss = (singleShot_t)dlsym(RTLD_DEFAULT,
                    "_ZN6QTimer10singleShotEiPK7QObjectPKc");
                if (fn_ss && dlg) {
                    dbglog("panel: state changed (ip='%s'->'%s' ssh=%d->%d tun=%d->%d), refreshing",
                        g_panelIp, ip_now, g_panelSsh, ssh_now, g_panelTun, tun_now);
                    fn_ss(0, dlg, "1accept()");
                }
            }
        }

        if (!fn_isChecked) continue;
        if (!fn_sc) fn_sc = (setChecked_t)dlsym(RTLD_DEFAULT, "_ZN15QAbstractButton10setCheckedEb");
        for (int i = 0; i < g_scriptBtnCount; i++) {
            if (g_scriptBtns[i].btn && fn_isChecked(g_scriptBtns[i].btn)) {
                dbglog("poll: btn[%d] clicked, running: %s", i, g_scriptBtns[i].cmd);
                if (fn_sc) fn_sc(g_scriptBtns[i].btn, false);
                system(g_scriptBtns[i].cmd);
            }
        }
    }
    return nullptr;
}

static int nm_init(){
    dbglog("init v6 (panel UI)");
    if(!access("/mnt/onboard/.adds/tolinom.disabled",F_OK)){dbglog("disabled");return 1;}
    dbglog("orig_setupUi=%p orig_beta=%p orig_susp=%p orig_suspDev=%p orig_suspDevAl=%p",
        orig_setupUi, orig_betaFeatures, orig_suspend,
        orig_suspendDevice, orig_suspendDeviceWithAlarm);

    // Start polling thread
    g_pollRunning = true;
    pthread_create(&g_pollThread, nullptr, poll_thread, nullptr);
    dbglog("poll thread started");
    return 0;
}

static struct nh_info info={.name="TolinoM",.desc="Panel UI v6",.failsafe_delay=15};
static struct nh_hook hooks[]={
    {
        .sym="_ZN11Ui_MoreView7setupUiEP7QWidget",
        .sym_new="PLACEHOLDER_SETUP",
        .lib="libnickel.so.1.0.0",
        .out=(void**)&orig_setupUi,
        .optional=true,
    },
    {
        .sym="_ZN8MoreView12betaFeaturesEv",
        .sym_new="PLACEHOLDER_BETA",
        .lib="libnickel.so.1.0.0",
        .out=(void**)&orig_betaFeatures,
        .optional=true,
    },
    {
        .sym="_ZN12PowerManager7suspendEv",
        .sym_new="PLACEHOLDER_SUSPEND",
        .lib="libnickel.so.1.0.0",
        .out=(void**)&orig_suspend,
        .optional=true,
    },
    {
        .sym="_ZN12PowerManager13suspendDeviceERK7QString",
        .sym_new="PLACEHOLDER_SUSPDEV",
        .lib="libnickel.so.1.0.0",
        .out=(void**)&orig_suspendDevice,
        .optional=true,
    },
    {
        .sym="_ZN12PowerManager22suspendDeviceWithAlarmERK7QString",
        .sym_new="PLACEHOLDER_SUSPDEVAL",
        .lib="libnickel.so.1.0.0",
        .out=(void**)&orig_suspendDeviceWithAlarm,
        .optional=true,
    },
    {0}
};
NickelHook(.init=&nm_init,.info=&info,.hook=hooks,)
