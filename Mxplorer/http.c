#include "mxplorer/http.h"
#include "net.h"
#include "stdlib.h"
#include "string.h"

#define HTTP_HEADER_MAX 8192u
#define HTTP_RESPONSE_MAX (MX_HTTP_BODY_MAX + HTTP_HEADER_MAX)
#define HTTP_IDLE_TIMEOUT_MS 5000
#define HTTP_TOTAL_TIMEOUT_MS 20000
#define HTTP_MEDIA_TIMEOUT_MS 120000u
#define HTTP_MAX_REDIRECTS 5u
#define HTTP_REDIRECT_TIMEOUT_MS 2000u
#define HTTP_URL_MAX 2048u
#define HTTP_REDIRECT_RESULT 2

/* Raw response: headers and body. Grows for large downloads (media) and
 * shrinks back to the page size afterwards. */
static char* response;
static size_t response_cap;

static int response_reserve(size_t body_capacity) {
    size_t need = body_capacity + HTTP_HEADER_MAX;
    if (response_cap >= need) return 1;
    free(response);
    response = (char*)malloc(need);
    response_cap = response ? need : 0;
    return response != NULL;
}

static void response_trim(void) {
    if (response_cap <= HTTP_RESPONSE_MAX) return;
    free(response);
    response = NULL;
    response_cap = 0;
}

static const char* too_large(size_t body_capacity) {
    return body_capacity > MX_HTTP_BODY_MAX ? "This file is larger than the media limit."
                                            : "This page is larger than 2 MiB.";
}

static void set_error(char* error, size_t capacity, const char* message) {
    if (capacity) strlcpy_safe(error, message, capacity);
}

static char lower_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static int equal_nocase(const char* a, const char* b, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) return 0;
        if (!a[i] || !b[i]) return a[i] == b[i];
    }
    return 1;
}

static int append_text(char* destination, size_t capacity, size_t* used,
                       const char* text) {
    size_t length = strlen(text);
    if (length >= capacity - *used) return 0;
    memcpy(destination + *used, text, length);
    *used += length;
    destination[*used] = '\0';
    return 1;
}

static int append_port(char* destination, size_t capacity, size_t* used,
                       uint16_t port) {
    char digits[5];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + port % 10u);
        port = (uint16_t)(port / 10u);
    } while (port && count < sizeof(digits));
    if (count + 1 >= capacity - *used) return 0;
    destination[(*used)++] = ':';
    while (count) destination[(*used)++] = digits[--count];
    destination[*used] = '\0';
    return 1;
}

static int parse_url(const char* url, char* host, size_t host_capacity,
                     char* path, size_t path_capacity, uint16_t* port,
                     int* use_tls, char* error, size_t error_capacity) {
    static const char prefix[] = "http://";
    static const char tls_prefix[] = "https://";
    if (!strncmp(url, prefix, sizeof(prefix) - 1)) {
        *use_tls = 0;
        *port = 80;
        url += sizeof(prefix) - 1;
    } else if (!strncmp(url, tls_prefix, sizeof(tls_prefix) - 1)) {
        *use_tls = 1;
        *port = 443;
        url += sizeof(tls_prefix) - 1;
    } else {
        set_error(error, error_capacity,
                  "Only HTTP and explicitly enabled unsafe HTTPS are supported.");
        return 0;
    }
    const char* authority = url;
    const char* path_start = strchr(authority, '/');
    const char* query_start = strchr(authority, '?');
    const char* authority_end = path_start;
    if (!authority_end || (query_start && query_start < authority_end))
        authority_end = query_start;
    if (!authority_end) authority_end = authority + strlen(authority);
    if (authority_end == authority || strchr(authority, '@') ||
        strchr(authority, '[')) {
        set_error(error, error_capacity, "Unsupported or invalid HTTP address.");
        return 0;
    }

    const char* colon = NULL;
    for (const char* c = authority; c < authority_end; c++)
        if (*c == ':') colon = c;
    const char* host_end = colon ? colon : authority_end;
    size_t host_length = (size_t)(host_end - authority);
    if (!host_length || host_length >= host_capacity) {
        set_error(error, error_capacity, "HTTP host is empty or too long.");
        return 0;
    }
    memcpy(host, authority, host_length);
    host[host_length] = '\0';
    if (colon) {
        unsigned value = 0;
        if (colon + 1 == authority_end) {
            set_error(error, error_capacity, "HTTP port is missing.");
            return 0;
        }
        for (const char* c = colon + 1; c < authority_end; c++) {
            if (*c < '0' || *c > '9') {
                set_error(error, error_capacity, "HTTP port is invalid.");
                return 0;
            }
            value = value * 10u + (unsigned)(*c - '0');
            if (value > 65535u) break;
        }
        if (value == 0 || value > 65535u) {
            set_error(error, error_capacity, "HTTP port is outside 1-65535.");
            return 0;
        }
        *port = (uint16_t)value;
    }

    size_t path_length = strlen(authority_end);
    if (!path_length || *authority_end == '?') {
        if (path_capacity < path_length + 2) {
            set_error(error, error_capacity, "HTTP path is too long.");
            return 0;
        }
        path[0] = '/';
        memcpy(path + 1, authority_end, path_length);
        path[path_length + 1] = '\0';
    } else {
        if (path_length >= path_capacity) {
            set_error(error, error_capacity, "HTTP path is too long.");
            return 0;
        }
        memcpy(path, authority_end, path_length);
        path[path_length] = '\0';
    }
    return 1;
}

static long transport_send(int use_tls, long connection, const void* data,
                           uint32_t length) {
    return use_tls ? mos_tls13_send(connection, data, length)
                   : mos_tcp_send(connection, data, length);
}

static long transport_recv(int use_tls, long connection, void* data,
                           uint32_t length, uint32_t timeout_ms) {
    return use_tls ? mos_tls13_recv(connection, data, length, timeout_ms)
                   : mos_tcp_recv(connection, data, length);
}

static void transport_close(int use_tls, long connection) {
    if (use_tls)
        mos_tls13_close(connection);
    else
        mos_tcp_close(connection);
}

static int transport_send_all(int use_tls, long connection, const char* data,
                              size_t length) {
    size_t sent = 0;
    while (sent < length) {
        size_t remaining = length - sent;
        uint32_t chunk = remaining > 4096u ? 4096u : (uint32_t)remaining;
        long count = transport_send(use_tls, connection, data + sent, chunk);
        if (count <= 0 || (size_t)count > remaining) return 0;
        sent += (size_t)count;
    }
    return 1;
}

static char* find_header_end(char* data, size_t length) {
    for (size_t i = 0; i + 3 < length; i++)
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n')
            return data + i;
    return NULL;
}

static int parse_status(const char* response_data, size_t length) {
    const char* line_end = response_data + length;
    const char* space = NULL;
    for (const char* p = response_data; p < line_end && *p != '\r' && *p != '\n'; p++) {
        if (*p == ' ') {
            space = p;
            break;
        }
    }
    if (!space || line_end - space < 4 || space[1] < '0' || space[1] > '9' ||
        space[2] < '0' || space[2] > '9' ||
        space[3] < '0' || space[3] > '9')
        return 0;
    return (space[1] - '0') * 100 + (space[2] - '0') * 10 +
           space[3] - '0';
}

static void parse_headers(char* headers, size_t length, size_t* content_length,
                          int* has_content_length, int* is_chunked) {
    size_t pos = 0;
    while (pos < length && !(headers[pos] == '\r' && pos + 1 < length &&
                             headers[pos + 1] == '\n'))
        pos++;
    pos += 2;
    while (pos + 1 < length) {
        size_t end = pos;
        while (end + 1 < length &&
               !(headers[end] == '\r' && headers[end + 1] == '\n'))
            end++;
        if (end == pos) break;
        const char* line = headers + pos;
        size_t line_length = end - pos;
        const char* colon = NULL;
        for (size_t i = 0; i < line_length; i++) {
            if (line[i] == ':') {
                colon = line + i;
                break;
            }
        }
        if (colon) {
            size_t key_length = (size_t)(colon - line);
            const char* value = colon + 1;
            size_t value_length = line_length - key_length - 1;
            while (value_length && (*value == ' ' || *value == '\t')) {
                value++;
                value_length--;
            }
            if (key_length == 14 &&
                equal_nocase(line, "content-length", key_length)) {
                size_t length_value = 0;
                int valid = value_length != 0;
                for (size_t i = 0; i < value_length; i++) {
                    if (value[i] < '0' || value[i] > '9') {
                        valid = 0;
                        break;
                    }
                    length_value = length_value * 10u +
                                   (size_t)(value[i] - '0');
                    if (length_value > MX_HTTP_BODY_MAX) {
                        valid = 0;
                        break;
                    }
                }
                if (valid) {
                    *content_length = length_value;
                    *has_content_length = 1;
                }
            } else if (key_length == 17 &&
                       equal_nocase(line, "transfer-encoding", key_length) &&
                       value_length >= 7 &&
                       equal_nocase(value + value_length - 7, "chunked", 7)) {
                *is_chunked = 1;
            }
        }
        pos = end + 2;
    }
}

static int is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

static int parse_location(const char* headers, size_t length,
                          char* location, size_t capacity) {
    size_t pos = 0;
    while (pos + 1 < length &&
           !(headers[pos] == '\r' && headers[pos + 1] == '\n'))
        pos++;
    pos += 2;
    while (pos + 1 < length) {
        size_t end = pos;
        while (end + 1 < length &&
               !(headers[end] == '\r' && headers[end + 1] == '\n'))
            end++;
        if (end == pos) break;
        const char* line = headers + pos;
        size_t line_length = end - pos;
        const char* colon = NULL;
        for (size_t i = 0; i < line_length; i++) {
            if (line[i] == ':') {
                colon = line + i;
                break;
            }
        }
        if (colon && (size_t)(colon - line) == 8 &&
            equal_nocase(line, "location", 8)) {
            const char* value = colon + 1;
            const char* value_end = line + line_length;
            while (value < value_end && (*value == ' ' || *value == '\t')) value++;
            while (value_end > value &&
                   (value_end[-1] == ' ' || value_end[-1] == '\t'))
                value_end--;
            size_t value_length = (size_t)(value_end - value);
            if (!value_length || value_length >= capacity) return 0;
            memcpy(location, value, value_length);
            location[value_length] = '\0';
            return 1;
        }
        pos = end + 2;
    }
    return 0;
}

static int append_slice(char* destination, size_t capacity, size_t* used,
                        const char* text, size_t length) {
    if (*used >= capacity || length >= capacity - *used) return 0;
    memcpy(destination + *used, text, length);
    *used += length;
    destination[*used] = '\0';
    return 1;
}

static int resolve_location(const char* base_url, const char* location,
                            char* resolved, size_t capacity) {
    static const char http_prefix[] = "http://";
    static const char https_prefix[] = "https://";
    const char* fragment = strchr(location, '#');
    size_t location_length = fragment ? (size_t)(fragment - location)
                                     : strlen(location);
    if (!location_length) return 0;

    if (!strncmp(location, http_prefix, sizeof(http_prefix) - 1) ||
        !strncmp(location, https_prefix, sizeof(https_prefix) - 1))
        return append_slice(resolved, capacity, &(size_t){0}, location,
                            location_length);

    int base_is_tls = !strncmp(base_url, https_prefix, sizeof(https_prefix) - 1);
    const char* scheme = base_is_tls ? "https://" : "http://";
    size_t used = 0;
    if (location[0] == '/' && location_length > 1 && location[1] == '/') {
        return append_text(resolved, capacity, &used, base_is_tls ? "https:" : "http:") &&
               append_slice(resolved, capacity, &used, location, location_length);
    }

    char host[128];
    char path[2048];
    char ignored_error[1];
    uint16_t port;
    int use_tls;
    if (!parse_url(base_url, host, sizeof(host), path, sizeof(path), &port,
                   &use_tls, ignored_error, sizeof(ignored_error)))
        return 0;
    if (!append_text(resolved, capacity, &used, scheme) ||
        !append_text(resolved, capacity, &used, host))
        return 0;
    if ((use_tls && port != 443) || (!use_tls && port != 80)) {
        if (!append_port(resolved, capacity, &used, port)) return 0;
    }

    if (location[0] == '/')
        return append_slice(resolved, capacity, &used, location, location_length);

    size_t base_path_length = strlen(path);
    for (size_t i = 0; i < base_path_length; i++) {
        if (path[i] == '?' || path[i] == '#') {
            base_path_length = i;
            break;
        }
    }
    if (location[0] == '?') {
        return append_slice(resolved, capacity, &used, path, base_path_length) &&
               append_slice(resolved, capacity, &used, location, location_length);
    }

    size_t directory_length = base_path_length;
    while (directory_length && path[directory_length - 1] != '/') directory_length--;
    return append_slice(resolved, capacity, &used, path, directory_length) &&
           append_slice(resolved, capacity, &used, location, location_length);
}

static int decode_chunked(const char* source, size_t length,
                          char* body, size_t capacity, size_t* out_length) {
    size_t pos = 0;
    size_t written = 0;
    while (pos < length) {
        size_t chunk_length = 0;
        int digits = 0;
        while (pos < length && source[pos] != '\r') {
            char c = lower_ascii(source[pos++]);
            if (c == ';') {
                while (pos < length && source[pos] != '\r') pos++;
                break;
            }
            unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                             c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 99u;
            if (digit > 15u || chunk_length > (capacity - digit) / 16u)
                return 0;
            chunk_length = chunk_length * 16u + digit;
            digits++;
        }
        if (!digits || pos + 1 >= length || source[pos] != '\r' ||
            source[pos + 1] != '\n')
            return 0;
        pos += 2;
        if (!chunk_length) {
            body[written] = '\0';
            *out_length = written;
            return 1;
        }
        if (chunk_length > capacity - written ||
            chunk_length + 2 > length - pos)
            return 0;
        memcpy(body + written, source + pos, chunk_length);
        written += chunk_length;
        pos += chunk_length;
        if (source[pos] != '\r' || source[pos + 1] != '\n') return 0;
        pos += 2;
    }
    return 0;
}

static uint32_t request_time_left(long start, uint32_t timeout_ms) {
    long elapsed = mos_uptime() - start;
    if (elapsed < 0 || (uint32_t)elapsed >= timeout_ms) return 0;
    return timeout_ms - (uint32_t)elapsed;
}

static uint32_t capped_timeout(uint32_t remaining, uint32_t cap) {
    return remaining < cap ? remaining : cap;
}

static void header_value(const char* headers, size_t length, const char* name,
                         char* out, size_t capacity) {
    size_t n = strlen(name);
    if (capacity) out[0] = '\0';
    for (size_t i = 0; i + n + 1 < length; i++) {
        if ((i == 0 || headers[i - 1] == '\n') && equal_nocase(headers + i, name, n) &&
            headers[i + n] == ':') {
            size_t start = i + n + 1;
            while (start < length && (headers[start] == ' ' || headers[start] == '\t')) start++;
            size_t end = start;
            while (end < length && headers[end] != '\r' && headers[end] != '\n') end++;
            size_t count = end - start;
            if (count >= capacity) count = capacity - 1;
            memcpy(out, headers + start, count);
            out[count] = '\0';
            return;
        }
    }
}

static char pending_etag[128];
static char pending_last_modified[64];

void mx_http_set_validators(const char* etag, const char* last_modified) {
    strlcpy_safe(pending_etag, etag ? etag : "", sizeof(pending_etag));
    strlcpy_safe(pending_last_modified, last_modified ? last_modified : "",
                 sizeof(pending_last_modified));
}

static int http_get_once(const char* url, char* body, size_t body_capacity,
                         size_t* body_length, int allow_insecure_tls,
                         uint32_t timeout_ms, char* redirect,
                         size_t redirect_capacity, char* error,
                         size_t error_capacity, mx_http_info_t* info) {
    char host[128];
    char path[2048];
    char request[3072];   /* path, validators, headers */
    uint16_t port;
    int use_tls = 0;
    size_t response_length = 0;
    if (body_length) *body_length = 0;
    if (!body || body_capacity < 2 || !body_length || !url) {
        set_error(error, error_capacity, "Invalid HTTP request buffer.");
        return 0;
    }
    if (!parse_url(url, host, sizeof(host), path, sizeof(path), &port, &use_tls,
                   error, error_capacity))
        return 0;
    if (!response_reserve(body_capacity)) {
        set_error(error, error_capacity, "Not enough memory for this download.");
        return 0;
    }
    if (use_tls && !allow_insecure_tls) {
        set_error(error, error_capacity,
                  "Unsafe HTTPS is disabled. Enable it in Settings to continue.");
        return 0;
    }
    if (!mos_net_is_up()) {
        set_error(error, error_capacity,
                  "Network is unavailable; connect a network adapter and run dhcp.");
        return 0;
    }

    long request_start = mos_uptime();
    uint32_t remaining = request_time_left(request_start, timeout_ms);
    uint32_t ip = remaining ? mos_net_lookup(host,
        capped_timeout(remaining, 5000u)) : 0;
    if (!ip) {
        set_error(error, error_capacity, "DNS lookup failed for this host.");
        return 0;
    }
    remaining = request_time_left(request_start, timeout_ms);
    uint32_t connect_timeout = capped_timeout(remaining,
                                               use_tls ? 15000u : 5000u);
    long connection = !connect_timeout ? 0 :
        use_tls ? mos_tls13_connect(ip, port, host, connect_timeout)
                : mos_tcp_connect(ip, port, connect_timeout);
    if (connection <= 0) {
        set_error(error, error_capacity,
                  use_tls ? "HTTPS TLS 1.3 handshake failed."
                          : "Could not connect to the HTTP server.");
        return 0;
    }
    size_t request_length = 0;
    int request_ok =
        append_text(request, sizeof(request), &request_length, "GET ") &&
        append_text(request, sizeof(request), &request_length, path) &&
        append_text(request, sizeof(request), &request_length, " HTTP/1.0\r\nHost: ") &&
        append_text(request, sizeof(request), &request_length, host) &&
        ((use_tls && port == 443) || (!use_tls && port == 80) ||
         append_port(request, sizeof(request), &request_length, port)) &&
        (!pending_etag[0] ||
         (append_text(request, sizeof(request), &request_length, "\r\nIf-None-Match: ") &&
          append_text(request, sizeof(request), &request_length, pending_etag))) &&
        (!pending_last_modified[0] ||
         (append_text(request, sizeof(request), &request_length, "\r\nIf-Modified-Since: ") &&
          append_text(request, sizeof(request), &request_length, pending_last_modified))) &&
        append_text(request, sizeof(request), &request_length,
            "\r\nUser-Agent: Mozilla/5.0 (Minimal-OS; x86_64) Mxplorer/0.3\r\n"
            "Accept: text/html,application/xhtml+xml,text/css,*/*;q=0.8\r\n"
            "Accept-Language: en-US,en;q=0.8\r\n"
            "Connection: close\r\n\r\n");
    if (!request_ok ||
        !transport_send_all(use_tls, connection, request, request_length)) {
        transport_close(use_tls, connection);
        set_error(error, error_capacity, "Could not send the HTTP request.");
        return 0;
    }

    long last_data = mos_uptime();
    int receive_failed = 0;
    while (response_length < response_cap) {
        remaining = request_time_left(request_start, timeout_ms);
        if (!remaining) break;
        long count = transport_recv(use_tls, connection,
            response + response_length,
            (uint32_t)(response_cap - response_length),
            capped_timeout(remaining, HTTP_IDLE_TIMEOUT_MS));
        if (count < 0) {
            if (count == (long)SYS_ERR_NOTFOUND) break;
            receive_failed = 1;
            break;
        }
        if (count == 0) {
            long now = mos_uptime();
            if (now - last_data > HTTP_IDLE_TIMEOUT_MS ||
                !request_time_left(request_start, timeout_ms))
                break;
            mos_sleep(5);
            continue;
        }
        response_length += (size_t)count;
        last_data = mos_uptime();
        if (response_length >= HTTP_HEADER_MAX &&
            !find_header_end(response, response_length)) {
            receive_failed = 1;
            break;
        }
        char* header_end = find_header_end(response, response_length);
        if (header_end) {
            if (is_redirect_status(parse_status(response, response_length)))
                break;
            size_t header_length = (size_t)(header_end - response);
            size_t content_length = 0;
            int has_content_length = 0;
            int chunked = 0;
            parse_headers(response, header_length, &content_length,
                          &has_content_length, &chunked);
            size_t received_body = response_length - header_length - 4;
            if (has_content_length && !chunked &&
                received_body >= content_length)
                break;
        }
    }
    transport_close(use_tls, connection);
    if (receive_failed) {
        set_error(error, error_capacity, "HTTP response could not be read.");
        return 0;
    }

    char* header_end = find_header_end(response, response_length);
    if (!header_end) {
        set_error(error, error_capacity, "HTTP response headers are incomplete.");
        return 0;
    }
    size_t header_length = (size_t)(header_end - response);
    int status = parse_status(response, response_length);
    if (info) {
        info->status = status;
        info->header_bytes = header_length + 4;
        header_value(response, header_length, "content-type", info->content_type,
                     sizeof(info->content_type));
        header_value(response, header_length, "etag", info->etag, sizeof(info->etag));
        header_value(response, header_length, "last-modified", info->last_modified,
                     sizeof(info->last_modified));
        header_value(response, header_length, "cache-control", info->cache_control,
                     sizeof(info->cache_control));
    }
    if (is_redirect_status(status)) {
        if (!redirect || !parse_location(response, header_length, redirect,
                                         redirect_capacity)) {
            set_error(error, error_capacity,
                      "Redirect response has no valid Location header.");
            return 0;
        }
        if (error_capacity) error[0] = '\0';
        return HTTP_REDIRECT_RESULT;
    }
    if (status < 200) {
        set_error(error, error_capacity,
                  "HTTP server returned an unexpected status.");
        return 0;
    }
    size_t declared_length = 0;
    int has_content_length = 0;
    int chunked = 0;
    parse_headers(response, header_length, &declared_length,
                  &has_content_length, &chunked);
    size_t available = response_length - header_length - 4;
    const char* response_body = header_end + 4;
    if (has_content_length && !chunked) {
        if (declared_length > body_capacity - 1) {
            set_error(error, error_capacity, too_large(body_capacity));
            return 0;
        }
        if (available < declared_length) {
            set_error(error, error_capacity, "HTTP response ended before the page was complete.");
            return 0;
        }
        available = declared_length;
    }
    if (chunked) {
        if (!decode_chunked(response_body, available, body, body_capacity,
                            body_length)) {
            set_error(error, error_capacity, "Invalid or oversized chunked HTTP response.");
            return 0;
        }
    } else {
        if (available >= body_capacity) {
            set_error(error, error_capacity, too_large(body_capacity));
            return 0;
        }
        memcpy(body, response_body, available);
        body[available] = '\0';
        *body_length = available;
    }
    if (error_capacity) error[0] = '\0';
    return 1;
}

int mx_http_get(const char* url, char* body, size_t body_capacity,
                size_t* body_length, int allow_insecure_tls,
                char* error, size_t error_capacity, mx_http_info_t* info) {
    char current_url[HTTP_URL_MAX];
    long started = mos_uptime();
    if (info) memset(info, 0, sizeof(*info));
    if (!url || strlen(url) >= sizeof(current_url)) {
        pending_etag[0] = pending_last_modified[0] = '\0';
        if (body_length) *body_length = 0;
        set_error(error, error_capacity, "HTTP address is too long.");
        return 0;
    }
    strlcpy_safe(current_url, url, sizeof(current_url));

    unsigned redirects = 0;
    for (;;) {
        char location[HTTP_URL_MAX];
        /* large downloads (media) get more time */
        uint32_t total = body_capacity > MX_HTTP_BODY_MAX ? HTTP_MEDIA_TIMEOUT_MS
                                                          : HTTP_TOTAL_TIMEOUT_MS;
        int result = http_get_once(current_url, body, body_capacity,
                                   body_length, allow_insecure_tls,
                                   redirects ? HTTP_REDIRECT_TIMEOUT_MS : total,
                                   location, sizeof(location), error,
                                   error_capacity, info);
        if (result != HTTP_REDIRECT_RESULT) {
            pending_etag[0] = pending_last_modified[0] = '\0';
            response_trim();
            if (info) {
                strlcpy_safe(info->final_url, current_url, sizeof(info->final_url));
                info->redirects = (int)redirects;
                info->elapsed_ms = mos_uptime() - started;
            }
            return result;
        }
        /* a redirect goes elsewhere: the validators were for this address */
        pending_etag[0] = pending_last_modified[0] = '\0';
        if (redirects >= HTTP_MAX_REDIRECTS) {
            set_error(error, error_capacity, "Too many HTTP redirects (maximum 5).");
            response_trim();
            return 0;
        }
        char resolved[HTTP_URL_MAX];
        if (!resolve_location(current_url, location, resolved,
                              sizeof(resolved))) {
            set_error(error, error_capacity, "HTTP redirect URL is invalid or too long.");
            response_trim();
            return 0;
        }
        strlcpy_safe(current_url, resolved, sizeof(current_url));
        redirects++;
    }
}
