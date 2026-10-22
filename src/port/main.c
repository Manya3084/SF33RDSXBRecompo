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
        /* stderr's pipe may not reach the file before the process ends */
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

static void run_report_begin(void)
{
    char base[4096];
    char path[4200];
    char marker[4200];
    int fd;
    if (sf_exe_dir(base, sizeof base) != 0) {
        return;
    }
    snprintf(path, sizeof path, "%s/data", base);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/data/last_run.log", base);
    snprintf(marker, sizeof marker, "%s/data/run.marker", base);
    if (access(marker, F_OK) == 0 && (fd = open(path, O_RDONLY)) >= 0) {
        /* the last run's log (its end, if it is long) to share, and for the
         * dialog its last lines, newest first, a crash's own lines first */
        off_t end = lseek(fd, 0, SEEK_END);
        off_t from = end > (off_t)sizeof s_report_full - 1 ? end - (off_t)sizeof s_report_full + 1 : 0;
        ssize_t n;
        char *crash;
        char *lines[16];
        int ln = 0;
        char *p;
        lseek(fd, from, SEEK_SET);
        n = read(fd, s_report_full, sizeof s_report_full - 1);
        s_report_full[n > 0 ? n : 0] = 0;
        close(fd);
        crash = strstr(s_report_full, "crash: ");
        if (crash) {
            /* the lines just before the crash (a message saying why), the
             * crash, and its stack less the reporter's own frames */
            static char work[sizeof s_report_full];
            char *save = NULL;
            char *before[4];
            int nb = 0;
            int frames = 0;
            int seen = 0;
            memcpy(work, s_report_full, sizeof work);
            s_report[0] = 0;
            for (p = strtok_r(work, "\n", &save); p; p = strtok_r(NULL, "\n", &save)) {
                size_t at = strlen(s_report);
                if (!seen && strncmp(p, "crash: ", 7) != 0) {
                    if (strstr(p, "no shim")) {
                        continue;
                    }
                    if (nb == 4) {
                        memmove(before, before + 1, sizeof before - sizeof before[0]);
                        nb--;
                    }
                    before[nb++] = p;
                    continue;
                }
                if (!seen) {
                    int k;
                    seen = 1;
                    for (k = 0; k < nb; k++) {
                        at = strlen(s_report);
                        snprintf(s_report + at, sizeof s_report - at, "%s\n", before[k]);
                    }
                    at = strlen(s_report);
                    snprintf(s_report + at, sizeof s_report - at, "%s\n", p);
                    continue;
                }
                if (strncmp(p, "  #", 3) != 0 || strstr(p, "android_backtrace") || strstr(p, "on_crash") ||
                    strstr(p, "sigreturn") || strstr(p, "(abort+") || strstr(p, "raise+") || frames >= 14) {
                    continue;
                }
                frames++;
                snprintf(s_report + at, sizeof s_report - at, "%s\n", p);
            }
        } else {
            static char work[sizeof s_report_full];
            memcpy(work, s_report_full, sizeof work);
            char *save = NULL;
            for (p = strtok_r(work, "\n", &save); p; p = strtok_r(NULL, "\n", &save)) {
                if (ln == 16) {
                    memmove(lines, lines + 1, sizeof lines - sizeof lines[0]);
                    ln--;
                }
                lines[ln++] = p;
            }
            s_report[0] = 0;
            while (ln > 0) {
                size_t at = strlen(s_report);
                snprintf(s_report + at, sizeof s_report - at, "%s\n", lines[--ln]);
            }
        }
        if (!s_report[0]) {
            snprintf(s_report, sizeof s_report, "(the log was empty)");
        }
    }
    s_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        close(fd);
    }
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

/* A clean end: no report next time. */
static void run_report_end(void)
{
    char base[4096];
    char marker[4200];
    if (sf_exe_dir(base, sizeof base) == 0) {
        snprintf(marker, sizeof marker, "%s/data/run.marker", base);
        unlink(marker);
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
            } else if (ev.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
                sf_sound_pause(0);
            }
        }
        if (!sf_port_quitting()) {
            sf_game_frame();
        }
    }
    /* the sound first, so nothing plays on while the window goes */
    sf_sound_shutdown();
    sf_video_shutdown();
    sf_jobs_shutdown();
#ifdef __ANDROID__
    run_report_end();
#endif
    SDL_Quit();
    return 0;
}
