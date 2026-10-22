/* Work the game thread hands off: disc reads, music and sound-effect
 * decoding, the first start's import. A few worker threads take jobs in
 * the order they were given.
 *
 * The game keeps its own time (one frame each 1/60 s, and every result it
 * reads lands on the frame it lands on with no threads), so a job never
 * decides when something happens: the game thread asks for a job's result
 * at the point it needs it, and waits there if the job is not done. A job
 * still queued at that point runs on the game thread at once.
 *
 * SF_THREADS=N sets the workers (0 runs every job on the thread that asks
 * for its result, as the program did before it had workers); the default
 * is one fewer than the processor's cores, two at most, as the game
 * thread, the render thread and the audio device's own thread have the
 * rest. */

#include "port/jobs.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>

#define WORKERS_MAX 4

enum { JOB_IDLE, JOB_QUEUED, JOB_RUNNING, JOB_DONE };

static SDL_Mutex *s_lock;
static SDL_Condition *s_wake; /* a job was queued, or the pool stops */
static SDL_Condition *s_done; /* a job finished */
static SfJob *s_head;
static SfJob *s_tail;
static SDL_Thread *s_thread[WORKERS_MAX];
static int s_workers;
static int s_stop;

static void run(SfJob *j)
{
    j->fn(j->arg);
    SDL_LockMutex(s_lock);
    j->state = JOB_DONE;
    SDL_BroadcastCondition(s_done);
    SDL_UnlockMutex(s_lock);
}

static int worker(void *unused)
{
    (void)unused;
    SDL_LockMutex(s_lock);
    for (;;) {
        SfJob *j;
        while (!s_head && !s_stop) {
            SDL_WaitCondition(s_wake, s_lock);
        }
        if (s_stop) {
            break;
        }
        j = s_head;
        s_head = j->next;
        if (!s_head) {
            s_tail = NULL;
        }
        j->state = JOB_RUNNING;
        SDL_UnlockMutex(s_lock);
        run(j);
        SDL_LockMutex(s_lock);
    }
    SDL_UnlockMutex(s_lock);
    return 0;
}

void sf_jobs_init(void)
{
    const char *env = getenv("SF_THREADS");
    int n;
    int i;
    if (s_lock) {
        return;
    }
    s_lock = SDL_CreateMutex();
    s_wake = SDL_CreateCondition();
    s_done = SDL_CreateCondition();
    if (env) {
        n = atoi(env);
    } else {
        n = SDL_GetNumLogicalCPUCores() - 1;
        n = n > 2 ? 2 : n;
    }
    n = n < 0 ? 0 : n > WORKERS_MAX ? WORKERS_MAX : n;
    for (i = 0; i < n; i++) {
        s_thread[s_workers] = SDL_CreateThread(worker, "sf3 worker", NULL);
        if (s_thread[s_workers]) {
            s_workers++;
        }
    }
}

int sf_jobs_workers(void)
{
    return s_workers;
}

void sf_job_start(SfJob *j, void (*fn)(void *), void *arg)
{
    if (!s_lock) {
        sf_jobs_init();
    }
    j->fn = fn;
    j->arg = arg;
    j->next = NULL;
    if (s_workers == 0) {
        /* no workers: it runs when its result is asked for */
        j->state = JOB_QUEUED;
        return;
    }
    SDL_LockMutex(s_lock);
    j->state = JOB_QUEUED;
    if (s_tail) {
        s_tail->next = j;
    } else {
        s_head = j;
    }
    s_tail = j;
    SDL_SignalCondition(s_wake);
    SDL_UnlockMutex(s_lock);
}

/* Takes a job off the queue if it is still on it; 1 when it was. */
static int unqueue(SfJob *j)
{
    SfJob **p;
    SfJob *prev = NULL;
    for (p = &s_head; *p; prev = *p, p = &(*p)->next) {
        if (*p == j) {
            *p = j->next;
            if (s_tail == j) {
                s_tail = prev;
            }
            j->next = NULL;
            return 1;
        }
    }
    return 0;
}

void sf_job_wait(SfJob *j)
{
    if (j->state == JOB_IDLE) {
        return;
    }
    if (s_workers == 0) {
        if (j->state == JOB_QUEUED) {
            j->state = JOB_RUNNING;
            j->fn(j->arg);
            j->state = JOB_DONE;
        }
        return;
    }
    SDL_LockMutex(s_lock);
    if (j->state == JOB_QUEUED && unqueue(j)) {
        /* nobody has it yet: this thread does it now */
        j->state = JOB_RUNNING;
        SDL_UnlockMutex(s_lock);
        run(j);
        return;
    }
    while (j->state != JOB_DONE) {
        SDL_WaitCondition(s_done, s_lock);
    }
    SDL_UnlockMutex(s_lock);
}

int sf_job_done(SfJob *j)
{
    int done;
    if (j->state == JOB_IDLE) {
        return 1;
    }
    if (s_workers == 0) {
        return j->state == JOB_DONE;
    }
    SDL_LockMutex(s_lock);
    done = j->state == JOB_DONE;
    SDL_UnlockMutex(s_lock);
    return done;
}

void sf_job_reset(SfJob *j)
{
    sf_job_wait(j);
    j->state = JOB_IDLE;
}

/* A job's own progress: the job publishes with sf_job_progress, a waiter
 * waits for a condition on it with sf_job_wait_for. */
void sf_job_progress(void)
{
    if (s_workers == 0) {
        return;
    }
    SDL_LockMutex(s_lock);
    SDL_BroadcastCondition(s_done);
    SDL_UnlockMutex(s_lock);
}

void sf_job_wait_for(SfJob *j, int (*ready)(void *), void *arg)
{
    if (ready(arg) || j->state == JOB_IDLE) {
        return;
    }
    if (s_workers == 0) {
        sf_job_wait(j);
        return;
    }
    SDL_LockMutex(s_lock);
    if (j->state == JOB_QUEUED && unqueue(j)) {
        j->state = JOB_RUNNING;
        SDL_UnlockMutex(s_lock);
        run(j);
        return;
    }
    while (j->state != JOB_DONE && !ready(arg)) {
        SDL_WaitCondition(s_done, s_lock);
    }
    SDL_UnlockMutex(s_lock);
}

void sf_jobs_shutdown(void)
{
    int i;
    if (!s_lock) {
        return;
    }
    SDL_LockMutex(s_lock);
    s_stop = 1;
    SDL_BroadcastCondition(s_wake);
    SDL_UnlockMutex(s_lock);
    for (i = 0; i < s_workers; i++) {
        SDL_WaitThread(s_thread[i], NULL);
    }
    s_workers = 0;
}
