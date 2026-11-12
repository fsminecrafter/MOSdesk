/* Downloads on worker threads. See mxplorer/fetch.h. */
#include "mxplorer/fetch.h"
#include "mxplorer/cache.h"
#include "mxplorer/util.h"
#include "string.h"
#include "stdlib.h"
#include "minimalos.h"
#include "thread.h"

#define MAX_JOBS 96
#define FETCH_URL_MAX 2048        /* as http.c */
/* Worker stacks: mx_cache_get keeps two mx_http_info_t (~2.4 KiB each)
 * and http.c a few KiB more; everything big is on the heap. */
#define WORKER_STACK (128u * 1024u)

enum { JOB_FREE = 0, JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_ABANDONED };

typedef struct {
    int state;
    int id;
    int media;
    int tls;
    unsigned seq;               /* queue order */
    char url[FETCH_URL_MAX];
    mx_fetch_result_t result;
} job_t;

static job_t jobs[MAX_JOBS];
static mx_lock_t lock;
static int next_id = 1;
static unsigned next_seq;
static int media_running;
static mos_thread_t workers[MX_FETCH_WORKERS];
static int worker_count;

static job_t* by_id(int id) {
    if (id <= 0) return NULL;
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].state != JOB_FREE && jobs[i].id == id) return &jobs[i];
    return NULL;
}

/* Runs one request (on a worker, or inline when there are none). */
static void run(const char* url, int media, int tls, mx_fetch_result_t* r) {
    size_t capacity = media ? MX_HTTP_MEDIA_MAX : MX_HTTP_BODY_MAX;
    char* buffer = (char*)malloc(capacity);
    if (!buffer) {
        mx_copy(r->error, sizeof(r->error), "Not enough memory for this file.");
        return;
    }
    size_t received = 0;
    r->how = MX_CACHE_NETWORK;
    r->ok = mx_cache_get(url, buffer, capacity, &received, tls, r->error, sizeof(r->error),
                         &r->info, &r->how);
    if (!r->ok) {
        free(buffer);
        return;
    }
    char* fitted = (char*)realloc(buffer, received + 1);
    if (fitted) buffer = fitted;
    buffer[received] = '\0';
    r->body = buffer;
    r->length = received;
}

/* The oldest queued request this worker may take, marked running. */
static job_t* take(void) {
    job_t* best = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        job_t* j = &jobs[i];
        if (j->state != JOB_QUEUED || (j->media && media_running)) continue;
        if (!best || (int)(j->seq - best->seq) < 0) best = j;
    }
    if (best) {
        best->state = JOB_RUNNING;
        if (best->media) media_running = 1;
    }
    return best;
}

static void worker(void* arg) {
    (void)arg;
    static char urls[MX_FETCH_WORKERS][FETCH_URL_MAX];
    int slot = (int)(uintptr_t)arg;
    long idle_ms = 2;
    for (;;) {
        mx_lock(&lock);
        job_t* j = take();
        int id = 0, media = 0, tls = 0;
        if (j) {
            id = j->id;
            media = j->media;
            tls = j->tls;
            mx_copy(urls[slot], sizeof(urls[slot]), j->url);
        }
        mx_unlock(&lock);
        if (!j) {
            /* nothing to do: look again soon, less often the longer it lasts */
            mos_sleep(idle_ms);
            if (idle_ms < 20) idle_ms++;
            continue;
        }
        idle_ms = 2;

        mx_fetch_result_t r;
        memset(&r, 0, sizeof(r));
        run(urls[slot], media, tls, &r);

        mx_lock(&lock);
        if (media) media_running = 0;
        job_t* done = by_id(id);         /* the same slot unless it was given up */
        if (done && done->state == JOB_RUNNING) {
            r.queued_ms = done->result.queued_ms;
            done->result = r;
            done->state = JOB_DONE;
        } else {
            if (done) done->state = JOB_FREE;    /* abandoned while it ran */
            free(r.body);
        }
        mx_unlock(&lock);
    }
}

int mx_fetch_start(void) {
    for (int i = 0; i < MX_FETCH_WORKERS; i++) {
        if (mos_thread_create(&workers[worker_count], worker, (void*)(uintptr_t)worker_count,
                              WORKER_STACK) != 0)
            break;
        worker_count++;
    }
    return worker_count;
}

int mx_fetch_find(const char* url) {
    int id = 0;
    mx_lock(&lock);
    for (int i = 0; i < MAX_JOBS && !id; i++) {
        job_t* j = &jobs[i];
        if ((j->state == JOB_QUEUED || j->state == JOB_RUNNING || j->state == JOB_DONE) &&
            !strcmp(j->url, url))
            id = j->id;
    }
    mx_unlock(&lock);
    return id;
}

int mx_fetch_request(const char* url, int media, int allow_insecure_tls) {
    if (!url || !*url || strlen(url) >= FETCH_URL_MAX) return 0;
    int existing = mx_fetch_find(url);
    if (existing) return existing;
    int id = 0;
    mx_lock(&lock);
    for (int i = 0; i < MAX_JOBS; i++) {
        job_t* j = &jobs[i];
        if (j->state != JOB_FREE) continue;
        memset(j, 0, sizeof(*j));
        j->id = id = next_id++;
        if (next_id <= 0) next_id = 1;
        j->media = media;
        j->tls = allow_insecure_tls;
        j->seq = next_seq++;
        mx_copy(j->url, sizeof(j->url), url);
        j->result.queued_ms = mx_now_ms();
        j->state = JOB_QUEUED;
        break;
    }
    mx_unlock(&lock);
    if (id && !worker_count) {
        /* no threads: run it here and now */
        mx_lock(&lock);
        job_t* j = by_id(id);
        j->state = JOB_RUNNING;
        mx_unlock(&lock);
        mx_fetch_result_t r;
        memset(&r, 0, sizeof(r));
        run(url, media, allow_insecure_tls, &r);
        mx_lock(&lock);
        r.queued_ms = j->result.queued_ms;
        j->result = r;
        j->state = JOB_DONE;
        mx_unlock(&lock);
    }
    return id;
}

int mx_fetch_poll(int id, mx_fetch_result_t* out) {
    int status;
    mx_lock(&lock);
    job_t* j = by_id(id);
    if (!j || j->state == JOB_ABANDONED) {
        status = -1;
    } else if (j->state != JOB_DONE) {
        status = 0;
    } else {
        *out = j->result;
        j->state = JOB_FREE;
        status = 1;
    }
    mx_unlock(&lock);
    return status;
}

static void abandon_locked(job_t* j) {
    if (j->state == JOB_RUNNING) {
        j->state = JOB_ABANDONED;       /* the worker frees it when it ends */
    } else if (j->state == JOB_QUEUED || j->state == JOB_DONE) {
        free(j->result.body);
        j->state = JOB_FREE;
    }
}

int mx_fetch_wait(int id, mx_fetch_result_t* out, long deadline_ms, void (*idle)(void)) {
    long last_idle = mx_now_ms();
    for (;;) {
        int s = mx_fetch_poll(id, out);
        if (s == 1) return 1;
        if (s < 0) return 0;
        long now = mx_now_ms();
        if (deadline_ms && now >= deadline_ms) {
            mx_lock(&lock);
            job_t* j = by_id(id);
            if (j) abandon_locked(j);
            mx_unlock(&lock);
            return 0;
        }
        if (idle && now - last_idle >= 50) {
            last_idle = now;
            idle();
        }
        mos_sleep(2);
    }
}

void mx_fetch_cancel_all(void) {
    mx_lock(&lock);
    for (int i = 0; i < MAX_JOBS; i++) abandon_locked(&jobs[i]);
    mx_unlock(&lock);
}

int mx_fetch_pending(void) {
    int n = 0;
    mx_lock(&lock);
    for (int i = 0; i < MAX_JOBS; i++)
        n += jobs[i].state == JOB_QUEUED || jobs[i].state == JOB_RUNNING;
    mx_unlock(&lock);
    return n;
}
