#ifndef MXPLORER_CACHE_H
#define MXPLORER_CACHE_H
#include <stddef.h>
#include <stdint.h>
#include "mxplorer/http.h"

/* Disk cache for pages and their files, in 0:/data/mxcache/.
 *
 * A copy is used without asking the server only while its Cache-Control
 * max-age lasts. After that the server is asked whether the file changed
 * (If-None-Match / If-Modified-Since): "304 Not Modified" keeps the copy,
 * anything else replaces it. So a site's new content always shows. When
 * the network is down, the last copy is shown instead of an error.
 *
 * Pictures are kept already converted to .img (mx_cache_replace), so a
 * cached page does not decode its PNGs and JPEGs again. .vid files are
 * kept as they come. The least recently used files go first when the
 * size limit is reached. */

/* How the answer of mx_cache_get() was made. */
enum {
    MX_CACHE_NETWORK = 0,      /* downloaded (and stored, when allowed) */
    MX_CACHE_FRESH,            /* the copy, still within max-age */
    MX_CACHE_REVALIDATED,      /* the server said it has not changed */
    MX_CACHE_OFFLINE           /* the copy, because the download failed */
};

#define MX_CACHE_DEFAULT_MB 64u

void mx_cache_init(void);                 /* reads the index; call once */
void mx_cache_flush(void);                /* writes the index if it changed */
void mx_cache_set_limit_mb(uint32_t mb);  /* 0 turns the cache off */
uint32_t mx_cache_limit_mb(void);
uint64_t mx_cache_used_bytes(void);
uint32_t mx_cache_count(void);
void mx_cache_clear(void);

/* mx_http_get() through the cache. *how is one of MX_CACHE_*. */
int mx_cache_get(const char* url, char* body, size_t body_capacity, size_t* body_length,
                 int allow_insecure_tls, char* error, size_t error_capacity,
                 mx_http_info_t* info, int* how);

/* Replaces the stored copy of `url` (already in the cache) with `data`,
 * keeping its validators: used to keep a picture as .img. */
void mx_cache_replace(const char* url, const void* data, size_t length);

#endif
