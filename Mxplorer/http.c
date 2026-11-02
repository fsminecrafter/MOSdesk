#include "mxplorer/http.h"
#include "net.h"
#include "stdlib.h"
#include "string.h"

#define HTTP_HEADER_MAX 8192u
#define HTTP_RESPONSE_MAX (MX_HTTP_BODY_MAX + HTTP_HEADER_MAX)
#define HTTP_IDLE_TIMEOUT_MS 5000
#define HTTP_TOTAL_TIMEOUT_MS 20000

static char response[HTTP_RESPONSE_MAX];

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
    return use_tls ? mos_tls_send(connection, data, length)
                   : mos_tcp_send(connection, data, length);
}

static long transport_recv(int use_tls, long connection, void* data,
                           uint32_t length) {
    return use_tls ? mos_tls_recv(connection, data, length)
                   : mos_tcp_recv(connection, data, length);
}

static void transport_close(int use_tls, long connection) {
    if (use_tls)
        mos_tls_close(connection);
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

static int parse_status(const char* response_data) {
    const char* space = strchr(response_data, ' ');
    if (!space || space[1] < '0' || space[1] > '9' ||
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

int mx_http_get(const char* url, char* body, size_t body_capacity,
                size_t* body_length, int allow_insecure_tls,
                char* error, size_t error_capacity) {
    char host[128];
    char path[512];
    char request[1024];
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

    uint32_t ip = mos_net_lookup(host, 5000);
    if (!ip) {
        set_error(error, error_capacity, "DNS lookup failed for this host.");
        return 0;
    }
    long connection = use_tls ? mos_tls_connect(ip, port, 5000)
                              : mos_tcp_connect(ip, port, 5000);
    if (connection <= 0) {
        set_error(error, error_capacity,
                  use_tls ? "Could not start the unsafe TLS transport."
                          : "Could not connect to the HTTP server.");
        return 0;
    }
    if (use_tls && !mos_tls_wait_established(connection, 10000)) {
        transport_close(use_tls, connection);
        set_error(error, error_capacity, "Unsafe TLS handshake failed.");
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
        append_text(request, sizeof(request), &request_length,
            "\r\nUser-Agent: Mxplorer/0.1\r\n"
            "Accept: text/html,text/plain;q=0.8\r\n"
            "Connection: close\r\n\r\n");
    if (!request_ok ||
        !transport_send_all(use_tls, connection, request, request_length)) {
        transport_close(use_tls, connection);
        set_error(error, error_capacity, "Could not send the HTTP request.");
        return 0;
    }

    long start = mos_uptime();
    long last_data = start;
    int receive_failed = 0;
    while (response_length < sizeof(response)) {
        long count = transport_recv(use_tls, connection,
            response + response_length,
            (uint32_t)(sizeof(response) - response_length));
        if (count < 0) {
            if (count == (long)SYS_ERR_NOTFOUND) break;
            receive_failed = 1;
            break;
        }
        if (count == 0) {
            long now = mos_uptime();
            if (now - last_data > HTTP_IDLE_TIMEOUT_MS ||
                now - start > HTTP_TOTAL_TIMEOUT_MS)
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
    int status = parse_status(response);
    if (status < 200 || status >= 300) {
        set_error(error, error_capacity,
                  status >= 300 && status < 400
                      ? "Redirects are not supported yet."
                      : "HTTP server returned an error status.");
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
            set_error(error, error_capacity, "HTTP page exceeds the 96 KiB limit.");
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
            set_error(error, error_capacity, "HTTP page exceeds the 96 KiB limit.");
            return 0;
        }
        memcpy(body, response_body, available);
        body[available] = '\0';
        *body_length = available;
    }
    if (error_capacity) error[0] = '\0';
    return 1;
}
