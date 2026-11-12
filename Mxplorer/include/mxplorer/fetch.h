#ifndef MXPLORER_FETCH_H
#define MXPLORER_FETCH_H
#include <stddef.h>
#include "mxplorer/http.h"

/* Downloads on worker threads (fetch.c).
 *
 * The page engine runs on the main thread; the network runs here.
 * MX_FETCH_WORKERS threads each take the oldest queued request and run it
 * through the disk cache (mx_cache_get), so several files download at
 * once and the window keeps answering while they do. Everything is
 * polled from the main thread; nothing here calls back into it.
 *
 * Requests belong to the page being loaded: mx_fetch_cancel_all() (a new
 * page) drops what is queued, and what is still running is thrown away
 * when it finishes. */

#define MX_FETCH_WORKERS 4

typedef struct {
    int ok;                     /* the download (or the saved copy) worked */
    char* body;                 /* malloc()ed, NUL-terminated; the caller frees it */
    size_t length;
    mx_http_info_t info;
    int how;                    /* MX_CACHE_* */
    char error[160];
    long queued_ms;             /* when it was requested */
} mx_fetch_result_t;

/* Starts the workers; returns how many run (0: requests run inline). */
int mx_fetch_start(void);

/* Queues `url` and returns its id (> 0), or 0 when the queue is full.
 * The same address already queued for this page returns that request.
 * `media` requests (pictures, videos) get the larger media size limit and
 * run one at a time. */
int mx_fetch_request(const char* url, int media, int allow_insecure_tls);

/* The id of this page's request for `url`, 0 when there is none. */
int mx_fetch_find(const char* url);

/* 1 when request `id` is done: *out is filled and the request is gone.
 * 0 while it runs, -1 for an unknown id. */
int mx_fetch_poll(int id, mx_fetch_result_t* out);

/* Waits for request `id`, calling `idle` (may be NULL) about every 50 ms
 * and giving up when `deadline_ms` (mx_now_ms() time, 0 = none) passes.
 * Returns 1 with *out filled, 0 on timeout (the request is abandoned). */
int mx_fetch_wait(int id, mx_fetch_result_t* out, long deadline_ms, void (*idle)(void));

/* Forgets every request of the current page. */
void mx_fetch_cancel_all(void);

/* Requests queued or running (for the status line). */
int mx_fetch_pending(void);

#endif
