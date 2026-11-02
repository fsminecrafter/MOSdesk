#ifndef MXPLORER_HTTP_H
#define MXPLORER_HTTP_H

#include <stddef.h>

#define MX_HTTP_BODY_MAX (96u * 1024u)

int mx_http_get(const char* url, char* body, size_t body_capacity,
                size_t* body_length, int allow_insecure_tls,
                char* error, size_t error_capacity);

#endif
