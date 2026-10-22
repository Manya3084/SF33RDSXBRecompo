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
#include <pthread.h>
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

static void log_to_logcat(void)
{
    int p[2];
    pthread_t t;
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
    SDL_Quit();
    return 0;
}
