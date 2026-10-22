/* The native entry. The game itself starts in sf_game_init. */

#include "host/compat.h"
#include "port/jobs.h"
#include "port/port.h"

#include <SDL3/SDL.h>

#if !defined(_WIN32) && !defined(__ANDROID__)
#define SF_BACKTRACE 1 /* glibc's; Android's own crash report has one */
#include <execinfo.h>
#endif
#ifdef __ANDROID__
#include <SDL3/SDL_main.h> /* SDL's activity calls SDL_main */
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unwind.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "host/paths.h"

/* A crash writes where it happened to stderr and data/crash.log. */
static char s_crash_path[4096 + 32];

#ifdef __ANDROID__
/* This run's log (data/last_run.log), which a crash also writes to. */
static int s_log_fd = -1;

typedef struct Unwound {
    void *pc[32];
    int n;
} Unwound;

static _Unwind_Reason_Code unwind_one(struct _Unwind_Context *ctx, void *arg)
{
    Unwound *u = arg;
    void *pc = (void *)_Unwind_GetIP(ctx);
    if (pc && u->n < 32) {
        u->pc[u->n++] = pc;
    }
    return u->n < 32 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

/* The crash's stack, each frame as library+offset (function+offset). */
static void android_backtrace(int fd)
{
    Unwound u;
    int i;
    u.n = 0;
    _Unwind_Backtrace(unwind_one, &u);
    for (i = 0; i < u.n; i++) {
        Dl_info info;
        char line[512];
        int len;
        if (dladdr(u.pc[i], &info) && info.dli_fname) {
            const char *lib = strrchr(info.dli_fname, '/');
            lib = lib ? lib + 1 : info.dli_fname;
            len = snprintf(line, sizeof line, "  #%02d %s+0x%lx (%s+0x%lx)\n", i, lib,
                           (unsigned long)((char *)u.pc[i] - (char *)info.dli_fbase), info.dli_sname ? info.dli_sname : "?",
                           info.dli_saddr ? (unsigned long)((char *)u.pc[i] - (char *)info.dli_saddr) : 0ul);
        } else {
            len = snprintf(line, sizeof line, "  #%02d %p\n", i, u.pc[i]);
        }
        (void)!write(fd, line, (size_t)len);
    }
}
#endif

static void on_crash(int sig)
{
#ifdef SF_BACKTRACE
    void *frames[64];
    int n = backtrace(frames, 64);
#endif
    char head[128];
    int fd;
    int len = snprintf(head, sizeof head, "crash: signal %d (%s) at game frame %d\n", sig, strsignal(sig), sf_port_frame);
    (void)!write(2, head, (size_t)len);
#ifdef SF_BACKTRACE
    backtrace_symbols_fd(frames, n, 2);
#endif
#ifdef __ANDROID__
    if (s_log_fd >= 0) {
        /* stderr's pipe may not reach the file before the process ends;
         * a new line first, in case the pipe's last line is half written */
        (void)!write(s_log_fd, "\n", 1);
        (void)!write(s_log_fd, head, (size_t)len);
        android_backtrace(s_log_fd);
        fsync(s_log_fd);
    }
#endif
    fd = open(s_crash_path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fd >= 0) {
        (void)!write(fd, head, (size_t)len);
#ifdef SF_BACKTRACE
        backtrace_symbols_fd(frames, n, fd);
#endif
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void crash_handler(void)
{
    char base[4096];
    if (sf_exe_dir(base, sizeof base) == 0) {
        snprintf(s_crash_path, sizeof s_crash_path, "%s/data/crash.log", base);
    }
    signal(SIGSEGV, on_crash);
    signal(SIGABRT, on_crash);
    signal(SIGFPE, on_crash);
    signal(SIGILL, on_crash);
#ifdef SIGBUS
    signal(SIGBUS, on_crash);
#endif
}

#ifdef __ANDROID__
/* Android has no terminal: stderr (every message the port writes) goes to
 * the system log, tag sf3, a line at a time (adb logcat -s sf3). */
static void *log_pump(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[1024];
    size_t have = 0;
    for (;;) {
        ssize_t n = read(fd, buf + have, sizeof buf - 1 - have);
        char *line;
        char *nl;
        if (n <= 0) {
            break;
        }
        have += (size_t)n;
        buf[have] = 0;
        line = buf;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = 0;
            __android_log_write(ANDROID_LOG_INFO, "sf3", line);
            if (s_log_fd >= 0) {
                *nl = '\n';
                (void)!write(s_log_fd, line, (size_t)(nl - line + 1));
            }
            line = nl + 1;
        }
        have = strlen(line);
        if (have == sizeof buf - 1) {
            __android_log_write(ANDROID_LOG_INFO, "sf3", line);
            have = 0;
        } else {
            memmove(buf, line, have);
        }
    }
    return NULL;
}

/* The phone has no terminal to read: each run's log is kept in
 * data/last_run.log, and a run that did not end cleanly (a crash, or a
 * stop on an error) is shown, from the end of its log, when the game
 * next starts. data/run.marker is there while a run is. */
static char s_report[3072];
static char s_report_full[65536];

/* data/run.marker holds the running process's id while it runs in the
 * foreground; going to the background, where Android may end it, or a
 * clean quit takes it away. */
static char s_marker[4200];
static int s_prev_pid;
static int s_fatal_seen; /* the system log has a fatal line of that run */

static void run_report_mark(int on)
{
    if (!s_marker[0]) {
        return;
    }
    if (on) {
        int fd = open(s_marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            char pid[32];
            int n = snprintf(pid, sizeof pid, "%d", (int)getpid());
            (void)!write(fd, pid, (size_t)n);
            close(fd);
        }
    } else {
        unlink(s_marker);
    }
}

/* A line of the report: the stack's frames less the reporter's own. */
static int keep_frame(const char *p)
{
    return !strstr(p, "android_backtrace") && !strstr(p, "on_crash") && !strstr(p, "sigreturn") &&
           !strstr(p, "(abort+") && !strstr(p, "(raise+");
}

static void report_add(const char *line)
{
    size_t at = strlen(s_report);
    snprintf(s_report + at, sizeof s_report - at, "%s\n", line);
}

/* What Android's own C library said as the process stopped (an abort's
 * reason: FORTIFY, fdsan, an uncaught C++ exception) goes to the system
 * log, not to stderr; an app may read its own lines there. */
static void read_system_log(void)
{
    FILE *f = popen("logcat -d -v brief -t 800 '*:W' 2>/dev/null", "r");
    char line[512];
    size_t at = strlen(s_report_full);
    static char fatal[3][512]; /* the newest three */
    int nf = 0;
    int k;
    if (!f) {
        return;
    }
    at += (size_t)snprintf(s_report_full + at, sizeof s_report_full - at, "\n--- system log ---\n");
    while (fgets(line, sizeof line, f)) {
        if (at < sizeof s_report_full - 1) {
            at += (size_t)snprintf(s_report_full + at, sizeof s_report_full - at, "%s", line);
            if (at > sizeof s_report_full - 1) {
                at = sizeof s_report_full - 1;
            }
        }
        char tag[32];
        snprintf(tag, sizeof tag, "(%5d)", s_prev_pid);
        if (s_prev_pid && strstr(line, tag) &&
            (strncmp(line, "F/", 2) == 0 || strstr(line, "FORTIFY") || strstr(line, "fdsan") ||
             strstr(line, "Abort message") || strstr(line, "terminating"))) {
            line[strcspn(line, "\n")] = 0;
            if (nf == 3) {
                memmove(fatal[0], fatal[1], sizeof fatal - sizeof fatal[0]);
                nf--;
            }
            snprintf(fatal[nf++], sizeof fatal[0], "%s", line);
            s_fatal_seen = 1;
        }
    }
    pclose(f);
    for (k = nf - 1; k >= 0; k--) {
        report_add(fatal[k]);
    }
}

static void run_report_begin(void)
{
    char base[4096];
    char path[4200];
    char prev[4200];
    char marker[4200];
    int stopped;
    int fd;
    if (sf_exe_dir(base, sizeof base) != 0) {
        return;
    }
    snprintf(path, sizeof path, "%s/data", base);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/data/last_run.log", base);
    snprintf(prev, sizeof prev, "%s/data/prev_run.log", base);
    snprintf(marker, sizeof marker, "%s/data/run.marker", base);
    stopped = 0;
    s_prev_pid = 0;
    if ((fd = open(marker, O_RDONLY)) >= 0) {
        char pid[32];
        ssize_t n = read(fd, pid, sizeof pid - 1);
        close(fd);
        pid[n > 0 ? n : 0] = 0;
        s_prev_pid = atoi(pid);
        stopped = 1;
    }
    /* the last run's log moves aside before this run's starts */
    rename(path, prev);
    if (stopped && (fd = open(prev, O_RDONLY)) >= 0) {
        off_t end = lseek(fd, 0, SEEK_END);
        off_t from = end > (off_t)sizeof s_report_full / 2 ? end - (off_t)sizeof s_report_full / 2 : 0;
        ssize_t n;
        lseek(fd, from, SEEK_SET);
        n = read(fd, s_report_full, sizeof s_report_full / 2);
        s_report_full[n > 0 ? n : 0] = 0;
        close(fd);
    }
    if (stopped) {
        static char work[sizeof s_report_full];
        char *save = NULL;
        char *p;
        char *crash = strstr(s_report_full, "crash: ");
        s_report[0] = 0;
        read_system_log(); /* its fatal lines first */
        memcpy(work, s_report_full, sizeof work);
        if (crash) {
            /* the crash and its stack */
            int frames = 0;
            int in = 0;
            for (p = strtok_r(work + (crash - s_report_full), "\n", &save); p; p = strtok_r(NULL, "\n", &save)) {
                if (strstr(p, "--- system log ---")) {
                    break;
                }
                if (!in) {
                    report_add(p);
                    in = 1;
                } else if (strncmp(p, "  #", 3) == 0 && keep_frame(p) && frames < 14) {
                    report_add(p);
                    frames++;
                }
            }
        } else {
            /* no crash: the log's last lines, newest first */
            char *lines[14];
            int ln = 0;
            for (p = strtok_r(work, "\n", &save); p; p = strtok_r(NULL, "\n", &save)) {
                if (strstr(p, "--- system log ---")) {
                    break;
                }
                if (strstr(p, "no shim")) {
                    continue;
                }
                if (ln == 14) {
                    memmove(lines, lines + 1, sizeof lines - sizeof lines[0]);
                    ln--;
                }
                lines[ln++] = p;
            }
            while (ln > 0) {
                report_add(lines[--ln]);
            }
        }
        {
            /* eight lines at most: the dialog's buttons stay on the screen */
            char *q = s_report;
            int k;
            for (k = 0; k < 8 && q && *q; k++) {
                q = strchr(q, '\n');
                q = q ? q + 1 : NULL;
            }
            if (q) {
                *q = 0;
            }
        }
        if (!crash && !strstr(s_report_full, "stopped: ") && !s_fatal_seen) {
            /* it was closed, or ended by Android in the background: no stop */
            s_report[0] = 0;
            s_report_full[0] = 0;
        }
    }
    s_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    snprintf(s_marker, sizeof s_marker, "%s", marker);
    run_report_mark(1);
}

/* SF3Activity.copyText: the text on the clipboard, to paste anywhere. */
static void copy_text(const char *text)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    jclass cls;
    jmethodID m;
    jstring s;
    if (!env || !act) {
        return;
    }
    cls = (*env)->GetObjectClass(env, act);
    m = (*env)->GetMethodID(env, cls, "copyText", "(Ljava/lang/String;)V");
    s = (*env)->NewStringUTF(env, text);
    if (m && s) {
        (*env)->CallVoidMethod(env, act, m, s);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, s);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}

static void run_report_show(void)
{
    SDL_MessageBoxButtonData buttons[2];
    SDL_MessageBoxData box;
    int pressed = 0;
    if (!s_report[0]) {
        return;
    }
    memset(buttons, 0, sizeof buttons);
    buttons[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
    buttons[0].buttonID = 0;
    buttons[0].text = "OK";
    buttons[1].buttonID = 1;
    buttons[1].text = "Copy log";
    memset(&box, 0, sizeof box);
    box.flags = SDL_MESSAGEBOX_WARNING;
    box.title = "The last run stopped";
    box.message = s_report;
    box.numbuttons = 2;
    box.buttons = buttons;
    if (SDL_ShowMessageBox(&box, &pressed) && pressed == 1) {
        copy_text(s_report_full);
    }
}


static void log_to_logcat(void)
{
    int p[2];
    pthread_t t;
    run_report_begin();
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (pipe(p) != 0) {
        return;
    }
    dup2(p[1], 2);
    close(p[1]);
    pthread_create(&t, NULL, log_pump, (void *)(intptr_t)p[0]);
    pthread_detach(t);
}
#endif

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#ifdef __ANDROID__
    log_to_logcat();
#endif
    {
        int cli = sf_setup_cli(argc, argv);
        if (cli >= 0) {
            return cli;
        }
    }
    crash_handler();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (getenv("SF_HEADLESS") && getenv("SF_RENDERER") && strcmp(getenv("SF_RENDERER"), "opengl") == 0) {
        /* a test of the OpenGL screen: nothing on the desktop */
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    }
#ifdef __ANDROID__
    /* landscape, held there (not turned by the sensor) until SCREEN ADJUST's
     * ORIENTATION says otherwise (src/port/display.c): SDL would otherwise
     * let a resizable window turn with the phone */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
#ifdef __ANDROID__
    run_report_show();
    fprintf(stderr, "android: API %d\n", SDL_GetAndroidSDKVersion());
#endif
    sf_game_init();
    while (!sf_port_quitting()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING) {
                sf_port_quit();
            } else if (ev.type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
                sf_sound_pause(1); /* Android: SDL holds this thread until the app is back */
#ifdef __ANDROID__
                run_report_mark(0); /* Android may end it there: not a stop */
#endif
            } else if (ev.type == SDL_EVENT_TEXT_INPUT) {
                sf_net_page_text(ev.text.text); /* the NETWORK page's address */
            } else if (ev.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
                sf_sound_pause(0);
#ifdef __ANDROID__
                run_report_mark(1);
#endif
            }
        }
        if (!sf_port_quitting()) {
            sf_frame();
        }
    }
    /* the sound first, so nothing plays on while the window goes */
    sf_sound_shutdown();
    sf_video_shutdown();
    sf_jobs_shutdown();
#ifdef __ANDROID__
    run_report_mark(0);
#endif
    SDL_Quit();
    return 0;
}
