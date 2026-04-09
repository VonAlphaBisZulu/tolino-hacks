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

static void show_scripts_dialog(void *moreview) {
    dbglog("show_scripts_dialog");
    if (!fn_dlgCtor || !fn_new || !fn_setTitle || !fn_show) {
        dbglog("missing dialog syms");
        return;
    }

    // Create ConfirmationDialog (32KB: unknown real size, overallocate to be safe)
    void *dlg = fn_new(32768);
    if (!dlg) return;
    fn_dlgCtor(dlg, (void*)moreview);

    // Configure dialog
    STACK_QS(title, u"Scripts", 7);
    fn_setTitle(dlg, &title);
    if (fn_showClose) fn_showClose(dlg, true);

    // Use ConfirmationDialog's built-in accept button for each script
    // Show sequential "Run X?" dialogs using exec() (blocking)
    typedef void (*setText_t)(void*, const void*);
    typedef void (*setAccBtnText_t)(void*, const void*);
    typedef void (*setRejVisible_t)(void*, bool);
    typedef int  (*exec_t)(void*);
    typedef void (*dlgDtor_t)(void*);

    static setText_t      fn_setText     = nullptr;
    static setAccBtnText_t fn_setAccBtn  = nullptr;
    static setRejVisible_t fn_setRejVis  = nullptr;
    static exec_t         fn_exec        = nullptr;

    if (!fn_setText)    fn_setText    = (setText_t)     dlsym(RTLD_DEFAULT, "_ZN18ConfirmationDialog7setTextERK7QString");
    if (!fn_setAccBtn)  fn_setAccBtn = (setAccBtnText_t)dlsym(RTLD_DEFAULT, "_ZN18ConfirmationDialog19setAcceptButtonTextERK7QString");
    if (!fn_setRejVis)  fn_setRejVis = (setRejVisible_t)dlsym(RTLD_DEFAULT, "_ZN18ConfirmationDialog22setRejectButtonVisibleEb");
    if (!fn_exec)       fn_exec      = (exec_t)        dlsym(RTLD_DEFAULT, "_ZN7QDialog4execEv");

    dbglog("setText=%p setAccBtn=%p setRejVis=%p exec=%p", fn_setText, fn_setAccBtn, fn_setRejVis, fn_exec);

    struct { const char16_t *label; long len; const char *cmd;
             const char16_t *desc; long dlen; } scripts[] = {
        { u"Cloud Connect",   13, "/mnt/onboard/.adds/cloud-connect.sh &",
          u"Start SSH and tunnel to chuzel.net", 34 },
        { u"Cloud Disconnect",16, "/mnt/onboard/.adds/cloud-disconnect.sh &",
          u"Stop the tunnel to chuzel.net", 29 },
        { u"Cloud Status",    12, "/mnt/onboard/.adds/cloud-status.sh",
          u"Check SSH and tunnel status", 27 },
        { u"FAZ Download",    12, "/mnt/onboard/.adds/faz-download.sh &",
          u"Download the latest FAZ epub", 28 },
        { u"System Info",     11, "/mnt/onboard/.adds/sysinfo.sh &",
          u"Save system information to sysinfo.txt", 38 },
        { u"Refresh Library", 15, "__BUILTIN_REFRESH__",
          u"Rescan books via PlugWorkflowManager", 36 },
    };
    int nscripts = 6;

    if (fn_setText && fn_setAccBtn && fn_exec) {
        for (int i = 0; i < nscripts; i++) {
            void *d = fn_new(32768);
            if (!d) continue;
            fn_dlgCtor(d, (void*)moreview);

            FakeQString t = {nullptr, (char16_t*)scripts[i].label, scripts[i].len};
            fn_setTitle(d, &t);
            FakeQString desc = {nullptr, (char16_t*)scripts[i].desc, scripts[i].dlen};
            fn_setText(d, &desc);
            STACK_QS(runText, u"Run", 3);
            fn_setAccBtn(d, &runText);
            if (fn_showClose) fn_showClose(d, true);
            if (fn_setRejVis) fn_setRejVis(d, false);

            dbglog("showing dialog for script[%d]", i);
            int result = fn_exec(d);
            dbglog("exec returned %d", result);

            if (result == 1) { // QDialog::Accepted
                dbglog("running: %s", scripts[i].cmd);
                if (strcmp(scripts[i].cmd, "__BUILTIN_REFRESH__") == 0) {
                    do_library_sync();
                } else {
                    system(scripts[i].cmd);
                }

                // For Cloud Status, show result in a follow-up dialog
                if (i == 2) { // Cloud Status
                    // Read status file
                    FILE *sf = fopen("/mnt/onboard/cloud-status.txt", "r");
                    if (sf) {
                        char sbuf[256] = {0};
                        fgets(sbuf, sizeof(sbuf), sf);
                        fclose(sf);
                        // Convert to char16_t
                        char16_t ubuf[256];
                        int slen = 0;
                        for (int j = 0; sbuf[j] && sbuf[j] != '\n' && j < 255; j++) {
                            ubuf[j] = (char16_t)sbuf[j];
                            slen++;
                        }
                        void *sd = fn_new(32768);
                        if (sd) {
                            fn_dlgCtor(sd, (void*)moreview);
                            STACK_QS(stitle, u"Cloud Status", 12);
                            fn_setTitle(sd, &stitle);
                            FakeQString stext = {nullptr, ubuf, slen};
                            fn_setText(sd, &stext);
                            STACK_QS(okText, u"OK", 2);
                            fn_setAccBtn(sd, &okText);
                            if (fn_showClose) fn_showClose(sd, true);
                            if (fn_setRejVis) fn_setRejVis(sd, false);
                            fn_exec(sd);
                        }
                    }
                }
                break;
            }
            // If closed/rejected, show next option
        }
    }
    dbglog("dialog sequence done");
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

    // Create Scripts button (32KB: we don't know the real object size, overallocate to be safe)
    STACK_QS(label, u"Scripts", 7);
    void *btn = fn_new(32768);
    if (!btn) return;
    fn_btnCtor(btn, &label, widget);

    // Style to match other MoreView buttons
    if (fn_setFlat) fn_setFlat(btn, true);
    if (fn_setSS) {
        STACK_QS(ss, u"QPushButton { font-size: 13pt; text-align: left; padding: 15px 20px; border: none; border-bottom: 1px solid #c0c0c0; background: transparent; }", 149);
        fn_setSS(btn, &ss);
    }

    fn_addWidget(layout, btn, 0, 0);

    // Make checkable (used as flag to detect our button in betaFeatures hook)
    if (fn_setCheckable) fn_setCheckable(btn, true);

    // Connect clicked(bool) -> setChecked(bool) to track state
    FakeConnection conn = {nullptr};
    fn_connect(&conn, btn, "2clicked(bool)", btn, "1setChecked(bool)", 0);

    // Connect clicked() -> MoreView::betaFeatures() signal (signal-to-signal relay)
    // betaFeatures() is a SIGNAL on MoreView (prefix "2"), not a SLOT (prefix "1")
    FakeConnection conn2 = {nullptr};
    fn_connect(&conn2, btn, "2clicked()", widget, "2betaFeatures()", 0);
    dbglog("btn=%p sig-to-sig betaFeatures on %p: %p", btn, widget, conn2.d_ptr);

    g_scriptsBtn = btn;
}

// Hook: MoreView::betaFeatures - intercept when Scripts button triggers it
__attribute__((visibility("default")))
void nm_hook_betaFeatures(void *_this) {
    dbglog("betaFeatures called, this=%p scriptsBtn=%p", _this, g_scriptsBtn);

    // Check if triggered by our Scripts button
    if (g_scriptsBtn && fn_isChecked && fn_isChecked(g_scriptsBtn)) {
        dbglog("triggered by Scripts button!");
        // Uncheck the button
        typedef void (*setChecked_t)(void*, bool);
        setChecked_t fn_sc = (setChecked_t)dlsym(RTLD_DEFAULT, "_ZN15QAbstractButton10setCheckedEb");
        if (fn_sc) fn_sc(g_scriptsBtn, false);

        // Show our custom dialog
        show_scripts_dialog(_this);
        return;
    }

    // Not our button - call original betaFeatures
    dbglog("calling original betaFeatures");
    orig_betaFeatures(_this);
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
    while (g_pollRunning) {
        usleep(150000); // 150ms
        // Check for sync trigger from MCP/shell (always, even before syms resolved)
        // Use queued version — marshals to main thread via QTimer::singleShot
        if (access(SYNC_TRIGGER, F_OK) == 0) {
            unlink(SYNC_TRIGGER);
            do_library_sync_queued();
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
    dbglog("init v5 (thread-safe sync)");
    if(!access("/mnt/onboard/.adds/tolinom.disabled",F_OK)){dbglog("disabled");return 1;}
    dbglog("orig_setupUi=%p orig_betaFeatures=%p", orig_setupUi, orig_betaFeatures);

    // Start polling thread
    g_pollRunning = true;
    pthread_create(&g_pollThread, nullptr, poll_thread, nullptr);
    dbglog("poll thread started");
    return 0;
}

static struct nh_info info={.name="TolinoM",.desc="Scripts menu v5",.failsafe_delay=15};
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
    {0}
};
NickelHook(.init=&nm_init,.info=&info,.hook=hooks,)
