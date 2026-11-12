/* The host renderer fetches with curl (render.c); the cache's network
 * calls are not used there. */
#include "mxplorer/http.h"
#include <string.h>
/* (validators are passed to mx_http_get_ex now) */
int mx_http_get(const char* url, char* body, size_t body_capacity, size_t* body_length,
                int allow_insecure_tls, char* error, size_t error_capacity, mx_http_info_t* info) {
    (void)url; (void)body; (void)body_capacity; (void)allow_insecure_tls;
    if (body_length) *body_length = 0;
    if (info) memset(info, 0, sizeof(*info));
    if (error_capacity) strncpy(error, "no network in the host renderer", error_capacity - 1);
    return 0;
}

int mx_http_get_ex(const char* url, char* body, size_t body_capacity, size_t* body_length,
                   int allow_insecure_tls, char* error, size_t error_capacity, mx_http_info_t* info,
                   const char* etag, const char* last_modified) {
    (void)etag; (void)last_modified;
    return mx_http_get(url, body, body_capacity, body_length, allow_insecure_tls, error,
                       error_capacity, info);
}
