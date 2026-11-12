#ifndef MXPLORER_HTTP_H
#define MXPLORER_HTTP_H

#include <stddef.h>

#define MX_HTTP_BODY_MAX (2u * 1024u * 1024u)
/* .img and .vid downloads may be larger than pages. */
#define MX_HTTP_MEDIA_MAX (32u * 1024u * 1024u)

typedef struct {
    int status;                /* HTTP status, 0 when no response */
    char content_type[64];
    char final_url[2048];      /* after redirects */
    int redirects;
    long elapsed_ms;
    size_t header_bytes;
    /* for the cache (cache.c) */
    char etag[128];
    char last_modified[64];
    char cache_control[128];
} mx_http_info_t;

/* GET `url` into `body`. Error pages (4xx/5xx) are returned as success with
 * info->status set. `info` may be NULL. Safe to call from several threads
 * at once: every request has its own state. */
int mx_http_get(const char* url, char* body, size_t body_capacity,
                size_t* body_length, int allow_insecure_tls,
                char* error, size_t error_capacity, mx_http_info_t* info);

/* mx_http_get() as a conditional request: asks the server whether its copy
 * changed (If-None-Match / If-Modified-Since). Either validator may be NULL
 * or empty. A server with nothing new answers status 304 and no body. */
int mx_http_get_ex(const char* url, char* body, size_t body_capacity,
                   size_t* body_length, int allow_insecure_tls,
                   char* error, size_t error_capacity, mx_http_info_t* info,
                   const char* etag, const char* last_modified);

#endif
