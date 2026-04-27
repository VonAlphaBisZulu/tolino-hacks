#include <dlfcn.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
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

// --- EPDC ioctl tracer ---
// Logs every ioctl that nickel sends to a framebuffer fd. We track which
// fds map to fb* via an open() hook; for those, we dump (request, region,
// waveform, flags) to a separate trace file. Goal: discover what waveform
// modes nickel uses for the keyboard vs the browser vs other widgets,
// so we can replicate them in our QWebEngineView.
//
// To enable: touch /mnt/onboard/.adds/tolinom-epdc-trace before booting.
// Disable by removing the file (we re-check on every ioctl — cheap).
static FILE *epdc_log = nullptr;
static int   epdc_fbfds[8] = {-1,-1,-1,-1,-1,-1,-1,-1};

static bool epdc_tracing() {
    return access("/mnt/onboard/.adds/tolinom-epdc-trace", F_OK) == 0;
}

static void epdc_logf(const char *fmt, ...) {
    if (!epdc_log) epdc_log = fopen("/mnt/onboard/tolinom-epdc.log", "a");
    if (!epdc_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(epdc_log, fmt, ap); va_end(ap);
    fputc('\n', epdc_log); fflush(epdc_log);
}

// Cache: fd → (is_fb_yes, is_fb_no, unknown). nickel opens /dev/fb0 before
// our preload lib is loaded, so we can't catch the open() call. Instead,
// the first time we see ioctl on an unknown fd, readlink(/proc/self/fd/N)
// to discover what it points at and remember.
static signed char fdkind[1024]; // 0=unknown, 1=fb, -1=not-fb

static bool is_fb_fd(int fd) {
    if (fd < 0 || fd >= 1024) return false;
    if (fdkind[fd] == 1) return true;
    if (fdkind[fd] == -1) return false;
    char link[64], target[128];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) { fdkind[fd] = -1; return false; }
    target[n] = 0;
    bool fb = (strncmp(target, "/dev/fb", 7) == 0);
    fdkind[fd] = fb ? 1 : -1;
    return fb;
}
static void register_fb_fd(int fd) { if (fd >= 0 && fd < 1024) fdkind[fd] = 1; }
static void unregister_fb_fd(int fd) { if (fd >= 0 && fd < 1024) fdkind[fd] = 0; }

// Layout of the Linux/MXC mxcfb_update_data struct used by Kobo's EPDC.
// First fields are stable across versions: rect (x,y,w,h) then waveform_mode,
// update_mode, update_marker, temp, flags. We only read these.
struct EpdcRect { unsigned int x, y, w, h; };
struct EpdcUpdateData {
    EpdcRect     update_region;
    unsigned int waveform_mode;
    unsigned int update_mode;
    unsigned int update_marker;
    int          temp;
    unsigned int flags;
    // (more fields after, ignored)
};

// Symbolic names for the well-known waveforms — Kobo uses these on top of
// the base mxc_epdc set. Order matches the driver enum.
static const char *waveform_name(unsigned int wf) {
    switch (wf) {
        case 0: return "INIT";
        case 1: return "DU";
        case 2: return "GC16";
        case 3: return "GC4";
        case 4: return "A2";
        case 5: return "GL16";
        case 6: return "REAGL";
        case 7: return "REAGLD";
        case 257: return "AUTO"; // Kobo-specific MXCFB_WAVEFORM_MODE_AUTO
        default: return "?";
    }
}

extern "C" int open(const char *pathname, int flags, ...) {
    static int (*real_open)(const char *, int, ...) = nullptr;
    if (!real_open) real_open = (int(*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
    int fd;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        fd = real_open(pathname, flags, mode);
    } else {
        fd = real_open(pathname, flags);
    }
    if (fd >= 0 && pathname && strncmp(pathname, "/dev/fb", 7) == 0) {
        register_fb_fd(fd);
        if (epdc_tracing()) epdc_logf("OPEN fb fd=%d path=%s", fd, pathname);
    }
    return fd;
}

extern "C" int close(int fd) {
    static int (*real_close)(int) = nullptr;
    if (!real_close) real_close = (int(*)(int))dlsym(RTLD_NEXT, "close");
    if (is_fb_fd(fd)) unregister_fb_fd(fd);
    return real_close(fd);
}

// Set when our QDialog (Apps) is visible, so we only rewrite waveforms
// during app interaction — not during normal Tolino UI use.
static volatile int g_app_active = 0;
// Per-app refresh policy: 0 = fast (force PARTIAL, monochrome, no flash),
// 1 = color (leave PARTIAL/FULL alone — keeps color but flashes). Apps opt
// into color via manifest.json: {"refresh":"color"}.
static volatile int g_app_color_mode = 0;
static bool epdc_fast_mode() {
    return access("/mnt/onboard/.adds/tolinom-epdc-fast", F_OK) == 0;
}

// 0x4024462e = MXCFB_SEND_UPDATE on this driver (verified via trace).
// Type='F' (0x46), nr=0x2e, write, size=36 bytes (matches our struct).
#define KOBO_MXCFB_SEND_UPDATE 0x4024462eUL

// Send a full-screen GC16 FULL update bypassing our own override. Used to
// scrub residual ghost from DU partial paints when our app dialog closes,
// otherwise nickel's incremental Mehr-menu repaints don't fully redraw
// thin lines like the separators around our buttons.
static void epdc_full_refresh() {
    static int (*real_ioctl)(int, unsigned long, ...) = nullptr;
    if (!real_ioctl) real_ioctl = (int(*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    int fd = -1;
    for (int i = 0; i < 1024; i++) if (fdkind[i] == 1) { fd = i; break; }
    if (fd < 0 || !real_ioctl) return;
    EpdcUpdateData u;
    memset(&u, 0, sizeof(u));
    u.update_region.x = 0;
    u.update_region.y = 0;
    u.update_region.w = 1072;
    u.update_region.h = 1448;
    u.waveform_mode = 2;     // GC16
    u.update_mode   = 1;     // FULL — drives the cleanup waveform cycle
    u.update_marker = 0;
    u.temp = -1;             // TEMP_USE_AMBIENT
    u.flags = 0;
    real_ioctl(fd, KOBO_MXCFB_SEND_UPDATE, &u);
}

extern "C" int ioctl(int fd, unsigned long request, ...) {
    static int (*real_ioctl)(int, unsigned long, ...) = nullptr;
    if (!real_ioctl) real_ioctl = (int(*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    va_list ap; va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (is_fb_fd(fd) && request == KOBO_MXCFB_SEND_UPDATE && arg) {
        EpdcUpdateData *u = (EpdcUpdateData *)arg;

        // While our app dialog is on screen, downgrade heavy waveforms
        // (GC16, REAGL) to DU so partial paints don't trigger the
        // multi-flash full refresh. INIT and AUTO are left alone — INIT
        // is only sent rarely (first paint) and AUTO is already adaptive.
        if (g_app_active && !g_app_color_mode && epdc_fast_mode()) {
            unsigned int orig_wf   = u->waveform_mode;
            unsigned int orig_mode = u->update_mode;
            // Fast/monochrome path: forcing PARTIAL kills the flash but
            // also kills color rendering on Kaleido 3 (the FULL waveform
            // cycle is what drives subpixel grey levels precisely enough
            // to show color through the RGB filter). Apps that need color
            // set "refresh":"color" in manifest.json and skip this whole
            // block.
            if (u->update_mode == 1) u->update_mode = 0;
            (void)orig_wf;
            if (epdc_tracing() && (orig_wf != u->waveform_mode || orig_mode != u->update_mode))
                epdc_logf("REWRITE wf %u(%s)->%u(%s) mode %u->%u region=(%u,%u %ux%u)",
                    orig_wf, waveform_name(orig_wf),
                    u->waveform_mode, waveform_name(u->waveform_mode),
                    orig_mode, u->update_mode,
                    u->update_region.x, u->update_region.y,
                    u->update_region.w, u->update_region.h);
        }

        if (epdc_tracing()) {
            epdc_logf("UPDATE fd=%d region=(%u,%u %ux%u) wf=%u(%s) mode=%u flags=0x%x app=%d",
                fd,
                u->update_region.x, u->update_region.y,
                u->update_region.w, u->update_region.h,
                u->waveform_mode, waveform_name(u->waveform_mode),
                u->update_mode, u->flags, g_app_active);
        }
    }

    return real_ioctl(fd, request, arg);
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
static void *g_appsBtn    = nullptr;

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

// QUrl on the stack — d_ptr starts null, the C1Ev ctor allocates the
// internal data. We never destruct it (small leak, scope-bounded use).
struct FakeQUrl { void *d; };

// QRect is x1,y1,x2,y2 (inclusive). All header-only inline ctors so we
// build it ourselves.
struct FakeQRect { int x1, y1, x2, y2; };

// Show a single web app full-screen in a frameless QDialog hosting a
// QWebEngineView. The HTML inside is responsible for its own chrome
// (header, X close, etc.). Closing happens when the page calls
// window.close() — we intercept windowCloseRequested and accept().
static void show_webapp(void *parent, const char *index_path,
                        const char *display_name) {
    // Resolve symbols lazily so that even devices without QtWebEngine in
    // memory degrade gracefully (the panel just won't open the app).
    typedef void (*viewCtor_t)(void*, void*);
    typedef void (*viewSetUrl_t)(void*, const FakeQUrl*);
    typedef void (*urlCtor_t)(FakeQUrl*);
    typedef void (*urlSetUrl_t)(FakeQUrl*, const void*, int);
    typedef void (*resizeFn_t)(void*, int, int);
    static viewCtor_t   fn_viewCtor   = nullptr;
    static viewSetUrl_t fn_viewSetUrl = nullptr;
    static urlCtor_t    fn_urlCtor    = nullptr;
    static urlSetUrl_t  fn_urlSetUrl  = nullptr;
    static resizeFn_t   fn_resize     = nullptr;
    if (!fn_viewCtor)   fn_viewCtor   = (viewCtor_t)  dlsym(RTLD_DEFAULT, "_ZN14QWebEngineViewC1EP7QWidget");
    if (!fn_viewSetUrl) fn_viewSetUrl = (viewSetUrl_t)dlsym(RTLD_DEFAULT, "_ZN14QWebEngineView6setUrlERK4QUrl");
    if (!fn_urlCtor)    fn_urlCtor    = (urlCtor_t)   dlsym(RTLD_DEFAULT, "_ZN4QUrlC1Ev");
    if (!fn_urlSetUrl)  fn_urlSetUrl  = (urlSetUrl_t) dlsym(RTLD_DEFAULT, "_ZN4QUrl6setUrlERK7QStringNS_11ParsingModeE");
    if (!fn_resize)     fn_resize     = (resizeFn_t)  dlsym(RTLD_DEFAULT, "_ZN7QWidget6resizeEii");
    if (!fn_viewCtor || !fn_viewSetUrl || !fn_urlCtor || !fn_urlSetUrl) {
        dbglog("apps: QtWebEngine symbols missing");
        return;
    }

    typedef int  (*exec_t)(void*);
    typedef void (*qDialogCtor_t)(void*, void*, int);
    typedef void (*qVBoxCtor_t)(void*, void*);
    typedef void (*layAddW_t)(void*, void*);
    typedef void (*layMargins_t)(void*, int, int, int, int);
    typedef void (*setGeo_t)(void*, const FakeQRect*);
    static exec_t        fn_exec_local = nullptr;
    static qDialogCtor_t fn_qDlgCtor   = nullptr;
    static qVBoxCtor_t   fn_vboxCtor   = nullptr;
    static layAddW_t     fn_layAddW    = nullptr;
    static layMargins_t  fn_layMargins = nullptr;
    static setGeo_t      fn_setGeo     = nullptr;
    if (!fn_exec_local) fn_exec_local = (exec_t)dlsym(RTLD_DEFAULT, "_ZN7QDialog4execEv");
    if (!fn_qDlgCtor)   fn_qDlgCtor   = (qDialogCtor_t)dlsym(RTLD_DEFAULT, "_ZN7QDialogC1EP7QWidget6QFlagsIN2Qt10WindowTypeEE");
    if (!fn_vboxCtor)   fn_vboxCtor   = (qVBoxCtor_t)dlsym(RTLD_DEFAULT, "_ZN11QVBoxLayoutC1EP7QWidget");
    // QBoxLayout::addWidget(QWidget*) (no stretch/align overload) — actually
    // we'll reuse the existing fn_addWidget which is the 4-arg overload.
    if (!fn_layMargins) fn_layMargins = (layMargins_t)dlsym(RTLD_DEFAULT, "_ZN7QLayout18setContentsMarginsEiiii");
    if (!fn_setGeo)     fn_setGeo     = (setGeo_t)dlsym(RTLD_DEFAULT, "_ZN7QWidget11setGeometryERK5QRect");
    if (!fn_qDlgCtor || !fn_vboxCtor || !fn_exec_local) {
        dbglog("apps: QDialog/QVBox symbols missing");
        return;
    }

    // Build the file:// URL
    char urlBuf[512];
    snprintf(urlBuf, sizeof(urlBuf), "file://%s", index_path);
    char16_t urlU[512];
    int urlLen = ascii_to_u16(urlBuf, urlU, 512);
    FakeQString urlQS = {nullptr, urlU, urlLen};

    FakeQUrl url = {nullptr};
    fn_urlCtor(&url);
    fn_urlSetUrl(&url, &urlQS, 0);

    (void)display_name;

    // Frameless modal QDialog (no title bar, no chrome, no padding).
    // Qt::FramelessWindowHint = 0x00000800.
    void *dlg = fn_new(32768);
    if (!dlg) return;
    fn_qDlgCtor(dlg, parent, 0x00000800);

    // Force application-level modality so input is fully captured and the
    // very first tap inside the dialog reaches the WebView (without it,
    // the first event sometimes only activates the window).
    typedef void (*setMod_t)(void*, int);
    static setMod_t fn_setMod = nullptr;
    if (!fn_setMod) fn_setMod = (setMod_t)dlsym(RTLD_DEFAULT,
        "_ZN7QWidget17setWindowModalityEN2Qt15WindowModalityE");
    // NonModal (0): the dialog stays open via exec()'s event loop but
    // does NOT grab input, so taps in the status-bar gutter actually
    // reach nickel's status bar widgets (WiFi icon, etc.) instead of
    // being absorbed by our modal.
    if (fn_setMod) fn_setMod(dlg, 0); // Qt::NonModal

    // Make sure the view accepts touch events directly, not just synthetic
    // mouse events converted from touches (the conversion adds a focus-grab
    // hop). WidgetAttribute::WA_AcceptTouchEvents = 64 in Qt 6.
    typedef void (*setAttr_t)(void*, int, bool);
    static setAttr_t fn_setAttr = nullptr;
    if (!fn_setAttr) fn_setAttr = (setAttr_t)dlsym(RTLD_DEFAULT,
        "_ZN7QWidget12setAttributeEN2Qt15WidgetAttributeEb");

    // Try nickel's BaseDialogWebView for HTTPS-capable WebView. Skip the
    // QVBoxLayout entirely on this path — wrapper sets its own geometry
    // as a child of dlg. Survival logging at each step so we can see which
    // call crashes if this attempt also fails.
    typedef void  (*basedlgwv_ctor_t)(void*, void*);
    typedef void* (*getwv_t)(void*);
    static basedlgwv_ctor_t fn_bdwvCtor = nullptr;
    static getwv_t          fn_getwv    = nullptr;
    if (!fn_bdwvCtor) fn_bdwvCtor = (basedlgwv_ctor_t)dlsym(RTLD_DEFAULT, "_ZN17BaseDialogWebViewC1EP7QWidget");
    if (!fn_getwv)    fn_getwv    = (getwv_t)dlsym(RTLD_DEFAULT, "_ZN17BaseDialogWebView10getWebViewEv");

    void *layout  = nullptr;
    void *wrapper = nullptr;
    void *view    = nullptr;
    bool tryBdwv  = (fn_bdwvCtor && fn_getwv);
    // Opt-out file: if /mnt/onboard/.adds/tolinom-no-bdwv exists, skip and
    // use raw QWebEngineView. Lets you recover without re-flashing if a
    // nickel update breaks the BaseDialogWebView path.
    if (access("/mnt/onboard/.adds/tolinom-no-bdwv", F_OK) == 0) tryBdwv = false;

    if (tryBdwv) {
        dbglog("apps: about to fn_new(262144) for BaseDialogWebView");
        wrapper = fn_new(262144);
        dbglog("apps: wrapper allocated at %p; calling ctor with dlg=%p", wrapper, dlg);
        if (wrapper) {
            fn_bdwvCtor(wrapper, (QWidget*)dlg);
            dbglog("apps: BaseDialogWebView ctor returned");
            // Inner view may be lazy-created on first show(). Show wrapper
            // first, then probe.
            typedef void (*show_t)(void*);
            static show_t fn_show2 = nullptr;
            if (!fn_show2) fn_show2 = (show_t)dlsym(RTLD_DEFAULT, "_ZN7QWidget4showEv");
            if (fn_show2) { fn_show2(wrapper); dbglog("apps: wrapper shown"); }
            view = fn_getwv(wrapper);
            dbglog("apps: getWebView returned %p", view);
        }
    }
    if (!view) {
        // Fallback: raw QWebEngineView with our own QVBoxLayout
        layout = fn_new(2048);
        if (layout) fn_vboxCtor(layout, dlg);
        if (layout && fn_layMargins) fn_layMargins(layout, 0, 0, 0, 0);
        view = fn_new(131072);
        if (!view) return;
        fn_viewCtor(view, nullptr);
        wrapper = view;
        dbglog("apps: fallback raw QWebEngineView=%p", view);
    }

    // Disable Chromium context menu — long-press on touch fires it,
    // and "Open in new window" creates orphan top-level windows.
    typedef void (*setCtxPolicy_t)(void*, int);
    static setCtxPolicy_t fn_setCtxPolicy = nullptr;
    if (!fn_setCtxPolicy) fn_setCtxPolicy = (setCtxPolicy_t)dlsym(RTLD_DEFAULT,
        "_ZN7QWidget20setContextMenuPolicyEN2Qt17ContextMenuPolicyE");
    if (fn_setCtxPolicy) fn_setCtxPolicy(view, 0); // Qt::NoContextMenu

    // Accept touches directly (avoids the touch->mouse synthetic conversion
    // that costs us the first tap). 64 = Qt::WA_AcceptTouchEvents in Qt 6.
    if (fn_setAttr) {
        fn_setAttr(view, 64, true);
        fn_setAttr(dlg,  64, true);
    }

    // Allow our file:// launcher (and apps) to load https:// content.
    // Chromium blocks this by default — file:// is treated as a unique
    // origin and remote URLs need an opt-in attribute on the page's
    // settings. The same attribute is set by nickel's beta browser.
    typedef void* (*pageSettings_t)(const void*);
    typedef void  (*setSettingsAttr_t)(void*, int, bool);
    static pageSettings_t    fn_pageSettings = nullptr;
    static setSettingsAttr_t fn_settingsAttr = nullptr;
    if (!fn_pageSettings) fn_pageSettings = (pageSettings_t)dlsym(RTLD_DEFAULT,
        "_ZNK14QWebEnginePage8settingsEv");
    if (!fn_settingsAttr) fn_settingsAttr = (setSettingsAttr_t)dlsym(RTLD_DEFAULT,
        "_ZN18QWebEngineSettings12setAttributeENS_12WebAttributeEb");
    typedef void* (*pageGetter2_t)(const void*);
    static pageGetter2_t fn_pageOf = nullptr;
    if (!fn_pageOf) fn_pageOf = (pageGetter2_t)dlsym(RTLD_DEFAULT,
        "_ZNK14QWebEngineView4pageEv");
    if (fn_pageOf && fn_pageSettings && fn_settingsAttr) {
        void *page = fn_pageOf(view);
        if (page) {
            void *settings = fn_pageSettings(page);
            if (settings) {
                // QWebEngineSettings::WebAttribute enum values (Qt 6.5):
                //   6 = LocalContentCanAccessRemoteUrls
                //   9 = LocalContentCanAccessFileUrls
                //  22 = AllowRunningInsecureContent
                fn_settingsAttr(settings, 6,  true);
                fn_settingsAttr(settings, 9,  true);
                fn_settingsAttr(settings, 22, true);
                dbglog("apps: enabled remote/file/insecure access on page");
            }
        }
    }

    fn_viewSetUrl(view, &url);

    // Layout-managed: add wrapper. Bdwv path: set geometry directly.
    if (layout && fn_addWidget) {
        fn_addWidget(layout, wrapper, 1, 0);
    } else if (fn_setGeo && wrapper) {
        FakeQRect wgeo = {0, 0, 1071, 1447};
        fn_setGeo(wrapper, &wgeo);
        typedef void (*show_t)(void*);
        static show_t fn_show = nullptr;
        if (!fn_show) fn_show = (show_t)dlsym(RTLD_DEFAULT, "_ZN7QWidget4showEv");
        if (fn_show) fn_show(wrapper);
    }

    // Wire JS window.close() to dlg.accept() via QWebEnginePage signal.
    typedef void* (*pageGetter_t)(const void*);
    static pageGetter_t fn_page = nullptr;
    if (!fn_page) fn_page = (pageGetter_t)dlsym(RTLD_DEFAULT, "_ZNK14QWebEngineView4pageEv");
    if (fn_page && fn_connect) {
        void *page = fn_page(view);
        if (page) {
            FakeConnection wc = {nullptr};
            fn_connect(&wc, page, "2windowCloseRequested()", dlg, "1accept()", 0);
        }
    }

    // Native overlay close button — Chromium's window.close() is unreliable
    // for non-script-opened windows, so we provide a Qt-side X that always
    // works. Positioned absolute top-right above the WebView; raise()
    // pulls it on top of the layout-managed view.
    void *closeBtn = fn_new(8192);
    if (closeBtn && fn_btnCtor) {
        STACK_QS(xText, u"✕", 1); // ✕ multiplication X
        fn_btnCtor(closeBtn, &xText, (QWidget*)dlg);
        if (fn_setSS) {
            STACK_QS(xCss,
                u"QPushButton{background:#fff;border:2px solid #000;"
                 "border-radius:30px;font-size:36px;font-weight:300;"
                 "color:#000;}"
                 "QPushButton:pressed{background:#000;color:#fff;}",
                157);
            fn_setSS(closeBtn, &xCss);
        }
        if (fn_resize) fn_resize(closeBtn, 60, 60);
        // setGeometry on the button via QRect to position top-right
        FakeQRect btnGeo = {1072 - 76, 16, 1072 - 17, 75};
        if (fn_setGeo) fn_setGeo(closeBtn, &btnGeo);
        // Raise above the WebView so taps hit the button, not the page
        typedef void (*raise_t)(void*);
        static raise_t fn_raise = nullptr;
        if (!fn_raise) fn_raise = (raise_t)dlsym(RTLD_DEFAULT, "_ZN7QWidget5raiseEv");
        if (fn_raise) fn_raise(closeBtn);
        // Wire clicked → dlg.accept()
        FakeConnection cb = {nullptr};
        fn_connect(&cb, closeBtn, "2clicked(bool)", dlg, "1accept()", 0);
        dbglog("apps: native close button wired");
    }

    // Position and size: full e-paper display, top-left corner. QRect uses
    // inclusive (x1,y1,x2,y2) so right/bottom are size-1.
    // Leave a top gutter so nickel's status bar (battery, WiFi, time) stays
    // visible above our dialog. The user can tap the WiFi indicator to
    // troubleshoot connectivity without exiting the app and losing state.
    // Status-bar height is roughly 88 px on this firmware.
    const int SCREEN_W   = 1072;
    const int SCREEN_H   = 1448;
    const int TOP_GUTTER = 96;
    FakeQRect geo = {0, TOP_GUTTER, SCREEN_W - 1, SCREEN_H - 1};
    if (fn_setGeo) fn_setGeo(dlg, &geo);

    // Make sure the WebView has keyboard/touch focus the moment the dialog
    // opens — otherwise the first tap on a tile only activates the window
    // and doesn't reach the page. setFocus is a QWidget slot; queue it via
    // QTimer::singleShot so it fires once the event loop in exec() is
    // running.
    typedef void (*singleShot_t)(int, const void*, const char*);
    static singleShot_t fn_ss2 = nullptr;
    if (!fn_ss2) fn_ss2 = (singleShot_t)dlsym(RTLD_DEFAULT,
        "_ZN6QTimer10singleShotEiPK7QObjectPKc");
    typedef void (*activate_t)(void*);
    static activate_t fn_activate = nullptr;
    if (!fn_activate) fn_activate = (activate_t)dlsym(RTLD_DEFAULT,
        "_ZN7QWidget14activateWindowEv");
    if (fn_activate) fn_activate(dlg);
    if (fn_ss2) {
        fn_ss2(0, dlg,  "1activateWindow()");
        fn_ss2(0, view, "1setFocus()");
    }

    // Per-app refresh policy from manifest.json next to index.html.
    // Default fast/monochrome (no flash). Apps that genuinely need color
    // fidelity (e.g. photos, image-rich crosswords) declare
    // {"refresh":"color"} in their manifest. PARTIAL preserves existing
    // color pixels just fine for vivid hues; pastels fade either way
    // due to Kaleido 3's limited color depth.
    g_app_color_mode = 0;
    {
        const char *slash = strrchr(index_path, '/');
        if (slash) {
            int len = slash - index_path;
            char manifest[320];
            if (len < (int)sizeof(manifest) - 32) {
                memcpy(manifest, index_path, len);
                strcpy(manifest + len, "/manifest.json");
                FILE *mf = fopen(manifest, "r");
                if (mf) {
                    char buf[1024]; size_t n = fread(buf, 1, sizeof(buf)-1, mf); fclose(mf);
                    buf[n] = 0;
                    if (strstr(buf, "\"refresh\"") && strstr(buf, "\"color\"")) {
                        g_app_color_mode = 1;
                        dbglog("apps: manifest -> color refresh");
                    }
                }
            }
        }
    }

    dbglog("apps: launching %s -> %s color=%d", display_name, urlBuf, g_app_color_mode);
    g_app_active = 1;
    fn_exec_local(dlg);
    g_app_active = 0;
    g_app_color_mode = 0;
    // Tell nickel to repaint the Mehr menu (the parent widget) so its
    // pixels are fresh in the framebuffer; then a GC16 cleanup refresh
    // pushes them out cleanly. Without the update() the EPDC refresh just
    // re-displays the stale ghost pixels Qt never overwrote.
    typedef void (*upd_t)(void*);
    static upd_t fn_update  = nullptr;
    static upd_t fn_repaint = nullptr;
    if (!fn_update)  fn_update  = (upd_t)dlsym(RTLD_DEFAULT, "_ZN7QWidget6updateEv");
    if (!fn_repaint) fn_repaint = (upd_t)dlsym(RTLD_DEFAULT, "_ZN7QWidget7repaintEv");
    if (fn_update && parent)  fn_update(parent);
    if (fn_repaint && parent) fn_repaint(parent);
    if (epdc_fast_mode()) epdc_full_refresh();
    dbglog("apps: closed %s", display_name);
}

// Embed-mode launch via nickel's MainWindowController::pushView. Returns
// true if the embed path succeeded (the view is now part of nickel's
// navigation stack and the status bar / nav footer stay visible). False
// means we should fall back to the floating QDialog launch.
//
// Opt-in via /mnt/onboard/.adds/tolinom-embed; when removed, the dialog
// path is used so a bad nickel update can't strand the launcher.
static bool try_show_launcher_embedded(const char *idx_path) {
    if (access("/mnt/onboard/.adds/tolinom-embed", F_OK) != 0) return false;

    typedef void* (*mwcInst_t)();
    typedef void  (*pushView_t)(void*, void*);
    static mwcInst_t  fn_mwcInst  = nullptr;
    static pushView_t fn_pushView = nullptr;
    if (!fn_mwcInst)  fn_mwcInst  = (mwcInst_t) dlsym(RTLD_DEFAULT, "_ZN20MainWindowController14sharedInstanceEv");
    if (!fn_pushView) fn_pushView = (pushView_t)dlsym(RTLD_DEFAULT, "_ZN20MainWindowController8pushViewEP7QWidget");
    if (!fn_mwcInst || !fn_pushView) return false;

    void *mwc = fn_mwcInst();
    if (!mwc) return false;

    // QWebEngineView setup, same security flags as show_webapp.
    typedef void  (*viewCtor_t)(void*, void*);
    typedef void  (*viewSetUrl_t)(void*, const FakeQUrl*);
    typedef void  (*urlCtor_t)(FakeQUrl*);
    typedef void  (*urlSetUrl_t)(FakeQUrl*, const void*, int);
    static viewCtor_t   fn_vCtor  = nullptr;
    static viewSetUrl_t fn_vUrl   = nullptr;
    static urlCtor_t    fn_uCtor  = nullptr;
    static urlSetUrl_t  fn_uSetU  = nullptr;
    if (!fn_vCtor) fn_vCtor = (viewCtor_t)  dlsym(RTLD_DEFAULT, "_ZN14QWebEngineViewC1EP7QWidget");
    if (!fn_vUrl)  fn_vUrl  = (viewSetUrl_t)dlsym(RTLD_DEFAULT, "_ZN14QWebEngineView6setUrlERK4QUrl");
    if (!fn_uCtor) fn_uCtor = (urlCtor_t)   dlsym(RTLD_DEFAULT, "_ZN4QUrlC1Ev");
    if (!fn_uSetU) fn_uSetU = (urlSetUrl_t) dlsym(RTLD_DEFAULT, "_ZN4QUrl6setUrlERK7QStringNS_11ParsingModeE");
    if (!fn_vCtor || !fn_vUrl || !fn_uCtor || !fn_uSetU) return false;

    char urlBuf[512];
    snprintf(urlBuf, sizeof(urlBuf), "file://%s", idx_path);
    char16_t urlU[512];
    int urlLen = ascii_to_u16(urlBuf, urlU, 512);
    FakeQString urlQS = {nullptr, urlU, urlLen};
    FakeQUrl url = {nullptr};
    fn_uCtor(&url);
    fn_uSetU(&url, &urlQS, 0);

    // Build the view fresh — overallocate, no parent (pushView reparents).
    void *view = fn_new(131072);
    if (!view) return false;
    fn_vCtor(view, nullptr);

    // Web settings: allow file:// to fetch https:// (the same fix that
    // unlocked the iframe to 20minutes / Keesing).
    typedef void* (*pageOf_t)(const void*);
    typedef void* (*settingsOf_t)(const void*);
    typedef void  (*setAttr_t)(void*, int, bool);
    static pageOf_t     fn_pageOf  = nullptr;
    static settingsOf_t fn_settsOf = nullptr;
    static setAttr_t    fn_settAtt = nullptr;
    if (!fn_pageOf)  fn_pageOf  = (pageOf_t)    dlsym(RTLD_DEFAULT, "_ZNK14QWebEngineView4pageEv");
    if (!fn_settsOf) fn_settsOf = (settingsOf_t)dlsym(RTLD_DEFAULT, "_ZNK14QWebEnginePage8settingsEv");
    if (!fn_settAtt) fn_settAtt = (setAttr_t)   dlsym(RTLD_DEFAULT, "_ZN18QWebEngineSettings12setAttributeENS_12WebAttributeEb");
    if (fn_pageOf && fn_settsOf && fn_settAtt) {
        void *page = fn_pageOf(view);
        if (page) {
            void *settings = fn_settsOf(page);
            if (settings) {
                fn_settAtt(settings, 6,  true);
                fn_settAtt(settings, 9,  true);
                fn_settAtt(settings, 22, true);
            }
        }
    }

    fn_vUrl(view, &url);

    // Disable Chromium context menu
    typedef void (*setCtx_t)(void*, int);
    static setCtx_t fn_setCtx = nullptr;
    if (!fn_setCtx) fn_setCtx = (setCtx_t)dlsym(RTLD_DEFAULT,
        "_ZN7QWidget20setContextMenuPolicyEN2Qt17ContextMenuPolicyE");
    if (fn_setCtx) fn_setCtx(view, 0);

    dbglog("apps: pushView(view=%p) on MWC=%p", view, mwc);
    g_app_active = 1;
    fn_pushView(mwc, view);
    return true;
}

// Scan /mnt/onboard/.adds/apps/<name>/index.html, write a manifest
// to apps/launcher/apps.json, then open the launcher web app. The
// launcher renders an Android-style icon grid of the available apps.
// All UI logic lives in HTML/CSS/JS; this C++ side just feeds it data.
static void show_apps_launcher(void *parent) {
    const char *appsDir = "/mnt/onboard/.adds/apps";
    const char *launcherIdx = "/mnt/onboard/.adds/apps/launcher/index.html";

    // Build apps.json next to the launcher so it can fetch it relative.
    // Each entry: {"name": "...", "path": "file://...", "icon": "..."}
    // The launcher links window.location to path on tap.
    FILE *jf = fopen("/mnt/onboard/.adds/apps/launcher/apps.json", "w");
    if (!jf) {
        // Try to mkdir launcher first — if missing, we can't show anything
        mkdir("/mnt/onboard/.adds/apps/launcher", 0755);
        jf = fopen("/mnt/onboard/.adds/apps/launcher/apps.json", "w");
        if (!jf) { dbglog("apps: cannot write apps.json"); return; }
    }
    fputs("[", jf);
    int n = 0;
    DIR *d = opendir(appsDir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            if (strcmp(de->d_name, "launcher") == 0) continue;
            char idx[320];
            snprintf(idx, sizeof(idx), "%s/%s/index.html", appsDir, de->d_name);
            struct stat st;
            if (stat(idx, &st) != 0) continue;
            // Optional icon at apps/<name>/icon.png or icon.svg
            char iconRel[64] = "";
            char iconPath[320];
            snprintf(iconPath, sizeof(iconPath), "%s/%s/icon.svg", appsDir, de->d_name);
            if (stat(iconPath, &st) == 0) {
                snprintf(iconRel, sizeof(iconRel), "../%s/icon.svg", de->d_name);
            } else {
                snprintf(iconPath, sizeof(iconPath), "%s/%s/icon.png", appsDir, de->d_name);
                if (stat(iconPath, &st) == 0)
                    snprintf(iconRel, sizeof(iconRel), "../%s/icon.png", de->d_name);
            }
            if (n++) fputs(",", jf);
            // Note: app names with quotes/backslashes will break JSON. We
            // don't expect any in this controlled directory.
            fprintf(jf, "{\"name\":\"%s\",\"path\":\"file://%s\",\"icon\":\"%s\"}",
                    de->d_name, idx, iconRel);
        }
        closedir(d);
    }
    fputs("]", jf);
    fclose(jf);

    // If there's no launcher app, fall back to opening the first app
    // directly so the user gets *something* useful.
    struct stat lst;
    if (stat(launcherIdx, &lst) != 0) {
        dbglog("apps: launcher not installed, looking for any app");
        DIR *d2 = opendir(appsDir);
        if (!d2) return;
        struct dirent *de;
        while ((de = readdir(d2))) {
            if (de->d_name[0] == '.') continue;
            if (strcmp(de->d_name, "launcher") == 0) continue;
            char idx[320];
            snprintf(idx, sizeof(idx), "%s/%s/index.html", appsDir, de->d_name);
            if (stat(idx, &lst) == 0) {
                closedir(d2);
                show_webapp(parent, idx, de->d_name);
                return;
            }
        }
        closedir(d2);
        return;
    }

    // Try the embedded view path first (nickel's MainWindowController::
    // pushView). If the user hasn't opted in via the flag file, falls back
    // to the floating QDialog launch.
    if (try_show_launcher_embedded(launcherIdx)) {
        dbglog("apps: launcher embedded into nickel nav stack");
        return;
    }
    show_webapp(parent, launcherIdx, "Apps");
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

        // Always read the IP so we can detect WiFi dropping mid-dialog.
        char ip[32] = {0};
        run_capture("ip -4 addr show wlan0 2>/dev/null | grep -o 'inet [0-9.]*' | cut -d' ' -f2", ip, sizeof(ip));

        // Tunnel requires both the dbclient process AND active WiFi — if WiFi
        // is down the TCP connection is dead even if the process lingers.
        bool tunnel_on = ip[0] && (system(
            "pgrep -f '[d]ropbearmulti dbclient' >/dev/null 2>&1") == 0);

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

        // Pull Inbox — pulls files staged on chuzel.net over HTTPS (no tunnel
        // needed). Label reflects the number of files waiting if we can see it.
        // Files already present locally with matching md5 are skipped silently
        // and don't contribute to the badge count.
        int inbox_count = 0;
        {
            char buf[16] = {0};
            // BusyBox wget can't SNI, so use python3 for the HTTPS query.
            // Also filter out files already on-device with matching md5.
            run_capture(
                "/mnt/onboard/.adds/inbox-count.sh 2>/dev/null",
                buf, sizeof(buf));
            inbox_count = atoi(buf);
        }

        // Build the row label as char16_t directly — the glyph would be
        // corrupted if we went through ascii_to_u16 (which treats each UTF-8
        // byte as a separate codepoint). U+25BC is a basic triangle.
        char16_t pullLabelU[64];
        int pullLabelLen = 0;
        const char16_t pullBase[] = u"\u25BC  Pull Inbox";
        const int pullBaseLen = (int)(sizeof(pullBase)/sizeof(char16_t)) - 1;
        for (int i = 0; i < pullBaseLen; i++) pullLabelU[pullLabelLen++] = pullBase[i];
        if (inbox_count > 0) {
            char countStr[16];
            snprintf(countStr, sizeof(countStr), " (%d)", inbox_count);
            for (int i = 0; countStr[i] && pullLabelLen < 63; i++)
                pullLabelU[pullLabelLen++] = (char16_t)countStr[i];
        }
        void *pullTouch = makeMenuRow(pullLabelU, pullLabelLen, false, false);

        void *refreshTouch = makeMenuRow(
            u"\u21BB  Refresh Library", 18, false, false);

        // WiFi reconnect action (useful after nickel drops the radio on sleep).
        void *wifiTouch = makeMenuRow(
            u"\u21BB  Reconnect WiFi", 17, false, false);

        // (Apps row was removed — Apps is now its own Mehr-menu entry)

        // Hidden trackers — tap toggles tracker AND closes dialog.
        // Slots for ssh / tunnel / pull / refresh / wifi; the tunnel slot is
        // nullptr in public builds and is simply skipped.
        STACK_QS(emptyStr, u"", 0);
        void *trackers[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
        void *touches[5]  = {sshTouch, tunTouch, pullTouch, refreshTouch, wifiTouch};
        for (int i = 0; i < 5; i++) {
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
            dbglog("panel: pulling inbox");
            system("/mnt/onboard/.adds/pull-inbox.sh >/tmp/tolinom-pull-out 2>&1");
            do_library_sync();
            // If anything was pulled, ask whether to keep it on the server.
            if (access("/tmp/tolinom-inbox-pulled", F_OK) == 0) {
                // Build body listing the pulled filenames
                char body[512] = "Pulled:\n";
                FILE *lf = fopen("/tmp/tolinom-inbox-pulled", "r");
                if (lf) {
                    char line[128];
                    int used = strlen(body);
                    while (fgets(line, sizeof(line), lf) && used < (int)sizeof(body) - 2) {
                        int n = strlen(line);
                        if (n > 0 && line[n-1] == '\n') line[--n] = 0;
                        int w = snprintf(body + used, sizeof(body) - used, "  %s\n", line);
                        if (w < 0 || used + w >= (int)sizeof(body)) break;
                        used += w;
                    }
                    fclose(lf);
                }
                char16_t bodyU[512];
                int bodyLen = ascii_to_u16(body, bodyU, 512);

                void *cd = fn_new(32768);
                if (cd) {
                    fn_dlgCtor(cd, parent);
                    STACK_QS(ctitle, u"Downloaded from inbox", 21);
                    fn_setTitle(cd, &ctitle);
                    FakeQString bodyQS = {nullptr, bodyU, bodyLen};
                    fn_setText(cd, &bodyQS);
                    if (fn_showClose) fn_showClose(cd, true);
                    if (fn_setRejVis) fn_setRejVis(cd, true);
                    if (fn_setAccVis) fn_setAccVis(cd, true);
                    STACK_QS(keepTxt, u"Keep on server", 14);
                    fn_setAccBtn(cd, &keepTxt);
                    // Reject button label via reflection: same setter pattern
                    typedef void (*setRejText_t)(void*, const void*);
                    static setRejText_t fn_setRejBtn = nullptr;
                    if (!fn_setRejBtn) fn_setRejBtn = (setRejText_t)dlsym(RTLD_DEFAULT,
                        "_ZN18ConfirmationDialog19setRejectButtonTextERK7QString");
                    STACK_QS(delTxt, u"Delete from server", 18);
                    if (fn_setRejBtn) fn_setRejBtn(cd, &delTxt);

                    int cr = fn_exec(cd);
                    dbglog("inbox-confirm: result=%d", cr);
                    if (cr == 0) {
                        // Rejected — user chose to delete from server
                        system("/mnt/onboard/.adds/clear-inbox.sh /tmp/tolinom-inbox-pulled");
                    } else {
                        // Accepted or dismissed — keep files, drop the list
                        unlink("/tmp/tolinom-inbox-pulled");
                    }
                }
            }
        }
        if (trackers[3] && fn_isChecked(trackers[3])) {
            dbglog("panel: refreshing library");
            do_library_sync();
        }
        if (trackers[4] && fn_isChecked(trackers[4])) {
            dbglog("panel: reconnecting wifi");
            system("/mnt/onboard/.adds/wifi-reconnect.sh &");
        }
        // (Apps row removed — Apps lives as its own Mehr-menu entry now)

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

    // Native MenuTextItem can host an icon to the left of the text — same
    // slot used by Activity / Settings / Hilfe etc. Adds visual parity and
    // shapes the row to native height.
    typedef void (*setLeftIcon_t)(void*, const void*);
    typedef void (*pixmapCtor_t)(void*, const void*, const char*, int);
    setLeftIcon_t fn_setLeftIcon = (setLeftIcon_t)dlsym(RTLD_DEFAULT, "_ZN12MenuTextItem16setLeftIconImageERK7QPixmap");
    pixmapCtor_t  fn_pixCtor     = (pixmapCtor_t)dlsym(RTLD_DEFAULT, "_ZN7QPixmapC1ERK7QStringPKc6QFlagsIN2Qt19ImageConversionFlagEE");

    auto setIconFor = [&](void *button, const char *path) {
        if (!fn_setLeftIcon || !fn_pixCtor) return;
        // QPixmap inherits QPaintDevice (vtable + a couple of fields) so it
        // is bigger than a plain pointer; we need an actual heap buffer or
        // the ctor will scribble past the end of our struct. Overallocate
        // generously (64 bytes is a comfortable upper bound). We leak it,
        // but setupUi only fires once per Mehr open.
        void *pix = fn_new(64);
        if (!pix) return;
        memset(pix, 0, 64);
        char16_t pathU[128];
        int pathLen = ascii_to_u16(path, pathU, 128);
        FakeQString pathQS = {nullptr, pathU, pathLen};
        fn_pixCtor(pix, &pathQS, nullptr, 0);
        fn_setLeftIcon(button, pix);
    };

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
    setIconFor(btn, "/mnt/onboard/.adds/icons/tolino-hacks.png");
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

    // Second Mehr-menu entry: "Apps" — bypasses the Tolino Hacks panel
    // and opens the HTML app launcher directly.
    void *apbtn = fn_new(32768);
    if (apbtn) {
        fn_miCtor(apbtn, (void*)ui[1], false, false);
        STACK_QS(aplabel, u"Apps", 4);
        fn_miSetText(apbtn, &aplabel);
        setIconFor(apbtn, "/mnt/onboard/.adds/icons/apps.png");
        if (fn_miRegGest) fn_miRegGest(apbtn);
        if (fn_setSS) {
            STACK_QS(apcss, u"*{font-style:normal;font-weight:normal;}", 41);
            fn_setSS(apbtn, &apcss);
        }
        // Match Tolino Hacks: vertical breathing room above and below
        if (fn_addSpacing) fn_addSpacing(targetLayout, 18);
        fn_addWidget(targetLayout, apbtn, 0, 0);
        if (fn_addSpacing) fn_addSpacing(targetLayout, 18);

        if (fn_frameCtor && fn_setShape) {
            typedef void (*setFrameShadow_t3)(void*, int);
            typedef void (*setLineWidth_t2)(void*, int);
            setFrameShadow_t3 fn_setShadow2 = (setFrameShadow_t3)dlsym(RTLD_DEFAULT, "_ZN6QFrame14setFrameShadowENS_6ShadowE");
            setLineWidth_t2   fn_setLW2     = (setLineWidth_t2)   dlsym(RTLD_DEFAULT, "_ZN6QFrame12setLineWidthEi");
            void *sep2 = fn_new(2048);
            if (sep2) {
                fn_frameCtor(sep2, (void*)ui[1], 0);
                fn_setShape(sep2, 4);
                if (fn_setShadow2) fn_setShadow2(sep2, 16);
                if (fn_setLW2)     fn_setLW2(sep2, 2);
                if (fn_setSS) {
                    STACK_QS(sep2Css, u"QFrame{color:#888;margin:0 24px;}", 33);
                    fn_setSS(sep2, &sep2Css);
                }
                fn_addWidget(targetLayout, sep2, 0, 0);
            }
        }

        void *apshim = fn_new(32768);
        fn_btnCtor(apshim, &emptyStr, widget);
        fn_setCheckable(apshim, true);
        FakeConnection apc1 = {nullptr};
        fn_connect(&apc1, apbtn, "2tapped(bool)", apshim, "1toggle()", 0);
        FakeConnection apc2 = {nullptr};
        fn_connect(&apc2, apshim, "2toggled(bool)", widget, "2betaFeatures()", 0);
        g_appsBtn = apshim;
    }
}

// Hook: MoreView::betaFeatures - intercept when Scripts button triggers it
__attribute__((visibility("default")))
void nm_hook_betaFeatures(void *_this) {
    dbglog("betaFeatures called, this=%p scripts=%p apps=%p",
        _this, g_scriptsBtn, g_appsBtn);

    typedef bool (*blockSig_t)(void*, bool);
    typedef void (*setChecked_t)(void*, bool);
    static blockSig_t fn_block = nullptr;
    static setChecked_t fn_sc = nullptr;
    if (!fn_block) fn_block = (blockSig_t)dlsym(RTLD_DEFAULT, "_ZN7QObject12blockSignalsEb");
    if (!fn_sc)    fn_sc    = (setChecked_t)dlsym(RTLD_DEFAULT, "_ZN15QAbstractButton10setCheckedEb");

    auto reset_shim = [&](void *shim) {
        // Uncheck without firing toggled(false) → betaFeatures recursion
        bool prev = false;
        if (fn_block) prev = fn_block(shim, true);
        if (fn_sc) fn_sc(shim, false);
        if (fn_block) fn_block(shim, prev);
    };

    if (g_scriptsBtn && fn_isChecked && fn_isChecked(g_scriptsBtn)) {
        dbglog("triggered by Tolino Hacks button");
        reset_shim(g_scriptsBtn);
        show_panel(_this);
        return;
    }
    if (g_appsBtn && fn_isChecked && fn_isChecked(g_appsBtn)) {
        dbglog("triggered by Apps button");
        reset_shim(g_appsBtn);
        show_apps_launcher(_this);
        return;
    }

    // Not our button — call original betaFeatures
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
            int tun_now = (ip_now[0] && system("pgrep -f '[d]ropbearmulti dbclient' >/dev/null 2>&1") == 0) ? 1 : 0;
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
