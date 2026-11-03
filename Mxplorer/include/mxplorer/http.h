#ifndef MXPLORER_HTTP_H
#define MXPLORER_HTTP_H

#include <stddef.h>

#define MX_HTTP_BODY_MAX (2u * 1024u * 1024u)

typedef struct {
    int status;                /* HTTP status, 0 when no response */
    char content_type[64];
    char final_url[512];       /* after redirects */
    int redirects;
    long elapsed_ms;
    size_t header_bytes;
} mx_http_info_t;

/* GET `url` into `body`. Error pages (4xx/5xx) are returned as success with
 * info->status set. `info` may be NULL. */
int mx_http_get(const char* url, char* body, size_t body_capacity,
                size_t* body_length, int allow_insecure_tls,
                char* error, size_t error_capacity, mx_http_info_t* info);

#endif
