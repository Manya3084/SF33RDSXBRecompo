#ifndef SF_PORT_JOBS_H
#define SF_PORT_JOBS_H

/* Work for the worker threads (src/port/jobs.c). A job is the caller's
 * own struct, zeroed before first use, and must not move while it runs. */

typedef struct SfJob {
    void (*fn)(void *);
    void *arg;
    int state;
    struct SfJob *next;
} SfJob;

void sf_jobs_init(void);
void sf_jobs_shutdown(void);

/* The worker threads running: 0 when every job runs on the thread that
 * waits for it. */
int sf_jobs_workers(void);

/* Queue fn(arg). A job must be done (or never started) before it is
 * started again. */
void sf_job_start(SfJob *j, void (*fn)(void *), void *arg);

/* Until the job is done; a job no worker has taken yet runs here. */
void sf_job_wait(SfJob *j);
int sf_job_done(SfJob *j);

/* Waits, then marks the job as never started. */
void sf_job_reset(SfJob *j);

/* For a job that publishes part of its result as it goes: it calls
 * sf_job_progress after each part; sf_job_wait_for waits until ready(arg)
 * or the job is done. ready is read under the pool's lock. */
void sf_job_progress(void);
void sf_job_wait_for(SfJob *j, int (*ready)(void *), void *arg);

#endif
