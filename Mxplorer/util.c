#include "mxplorer/util.h"
#include "string.h"
#include "stdlib.h"

#ifdef MX_HOST
#include <time.h>
long mx_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
#else
#include "minimalos.h"
long mx_now_ms(void) {
    return mos_uptime();
}
#endif

#define MX_ARENA_CHUNK (256u * 1024u)

void* mx_arena_alloc(mx_arena_t* arena, size_t size) {
    size = (size + 15u) & ~(size_t)15u;
    if (arena->limit && arena->total + size > arena->limit) {
        arena->failed = 1;
        return NULL;
    }
    mx_arena_chunk_t* chunk = arena->head;
    size_t header = (sizeof(mx_arena_chunk_t) + 15u) & ~(size_t)15u;
    if (!chunk || chunk->used + size > chunk->capacity) {
        size_t capacity = size > MX_ARENA_CHUNK ? size : MX_ARENA_CHUNK;
        chunk = (mx_arena_chunk_t*)malloc(header + capacity);
        if (!chunk) {
            arena->failed = 1;
            return NULL;
        }
        chunk->next = arena->head;
        chunk->used = 0;
        chunk->capacity = capacity;
        arena->head = chunk;
    }
    void* result = (char*)chunk + header + chunk->used;
    chunk->used += size;
    arena->total += size;
    return result;
}

void* mx_arena_zalloc(mx_arena_t* arena, size_t size) {
    void* result = mx_arena_alloc(arena, size);
    if (result) memset(result, 0, size);
    return result;
}

char* mx_arena_strndup(mx_arena_t* arena, const char* text, size_t length) {
    char* result = (char*)mx_arena_alloc(arena, length + 1);
    if (!result) return NULL;
    if (length) memcpy(result, text, length);
    result[length] = '\0';
    return result;
}

char* mx_arena_strdup(mx_arena_t* arena, const char* text) {
    return mx_arena_strndup(arena, text ? text : "", text ? strlen(text) : 0);
}

void mx_arena_free_all(mx_arena_t* arena) {
    mx_arena_chunk_t* chunk = arena->head;
    while (chunk) {
        mx_arena_chunk_t* next = chunk->next;
        free(chunk);
        chunk = next;
    }
    arena->head = NULL;
    arena->total = 0;
    arena->failed = 0;
}

char mx_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

int mx_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

int mx_streq_nocase(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b && mx_lower(*a) == mx_lower(*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

int mx_strneq_nocase(const char* a, const char* b, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (mx_lower(a[i]) != mx_lower(b[i])) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

int mx_starts_with_nocase(const char* text, const char* prefix) {
    while (*prefix) {
        if (mx_lower(*text++) != mx_lower(*prefix++)) return 0;
    }
    return 1;
}

void mx_copy(char* out, size_t capacity, const char* text) {
    mx_copy_n(out, capacity, text, text ? strlen(text) : 0);
}

void mx_copy_n(char* out, size_t capacity, const char* text, size_t length) {
    if (!capacity) return;
    if (length >= capacity) length = capacity - 1;
    if (length) memcpy(out, text, length);
    out[length] = '\0';
}

void mx_append(char* out, size_t capacity, const char* text) {
    size_t used = strlen(out);
    if (used < capacity) mx_copy(out + used, capacity - used, text);
}

void mx_append_int(char* out, size_t capacity, long value) {
    char digits[24];
    int count = 0;
    unsigned long magnitude = value < 0 ? (unsigned long)(-value)
                                        : (unsigned long)value;
    do {
        digits[count++] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude && count < 22);
    if (value < 0) digits[count++] = '-';
    char text[24];
    for (int i = 0; i < count; i++) text[i] = digits[count - 1 - i];
    text[count] = '\0';
    mx_append(out, capacity, text);
}

size_t mx_utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp < 0x110000) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    out[0] = '?';
    return 1;
}

const char* mx_memchr(const char* text, char c, size_t length) {
    for (size_t i = 0; i < length; i++)
        if (text[i] == c) return text + i;
    return NULL;
}

uint32_t mx_hash(const char* text, size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        hash ^= (uint8_t)mx_lower(text[i]);
        hash *= 16777619u;
    }
    return hash;
}

/* Removes "." and ".." segments from the path part of an absolute URL. */
static void url_normalize_path(char* url) {
    char* scheme = strstr(url, "://");
    if (!scheme) return;
    char* path = strchr(scheme + 3, '/');
    if (!path) return;
    char* end = path;
    while (*end && *end != '?' && *end != '#') end++;
    char tail[512];
    mx_copy(tail, sizeof(tail), end);
    const char* starts[64];
    size_t lengths[64];
    int depth = 0;
    int trailing_slash = 0;
    const char* p = path;
    while (p < end) {
        const char* segment = p + 1;
        const char* next = segment;
        while (next < end && *next != '/') next++;
        size_t length = (size_t)(next - segment);
        trailing_slash = 0;
        if (length == 1 && segment[0] == '.') {
            trailing_slash = 1;
        } else if (length == 2 && segment[0] == '.' && segment[1] == '.') {
            if (depth) depth--;
            trailing_slash = 1;
        } else if (depth < 64) {
            starts[depth] = segment;
            lengths[depth++] = length;
        }
        p = next;
    }
    char out[1024];
    size_t used = 0;
    for (int i = 0; i < depth; i++) {
        if (used + 1 + lengths[i] >= sizeof(out)) break;
        out[used++] = '/';
        memcpy(out + used, starts[i], lengths[i]);
        used += lengths[i];
    }
    if (!used || trailing_slash) out[used++] = '/';
    out[used] = '\0';
    size_t prefix = (size_t)(path - url);
    mx_copy(url + prefix, 1024 - prefix, out);
    mx_append(url, 1024, tail);
}

int mx_url_resolve(const char* base, const char* ref, char* out,
                   size_t capacity) {
    char result[1024];
    while (*ref == ' ' || *ref == '\t' || *ref == '\n') ref++;
    if (!*ref) {
        mx_copy(out, capacity, base);
        return 1;
    }
    const char* colon = strchr(ref, ':');
    const char* slash = strchr(ref, '/');
    if (colon && (!slash || colon < slash)) {
        /* absolute: scheme present */
        mx_copy(out, capacity, ref);
        return strlen(ref) < capacity;
    }
    const char* base_scheme = strstr(base, "://");
    if (!base_scheme) {
        mx_copy(out, capacity, ref);
        return 0;
    }
    if (ref[0] == '/' && ref[1] == '/') {
        mx_copy_n(result, sizeof(result), base,
                  (size_t)(base_scheme - base) + 1);
        mx_append(result, sizeof(result), ref);
    } else if (ref[0] == '/') {
        const char* host_end = base_scheme + 3;
        while (*host_end && *host_end != '/' && *host_end != '?' &&
               *host_end != '#')
            host_end++;
        mx_copy_n(result, sizeof(result), base, (size_t)(host_end - base));
        mx_append(result, sizeof(result), ref);
    } else if (ref[0] == '#' || ref[0] == '?') {
        const char* cut = base;
        while (*cut && *cut != '#' && (ref[0] == '#' || *cut != '?')) cut++;
        mx_copy_n(result, sizeof(result), base, (size_t)(cut - base));
        mx_append(result, sizeof(result), ref);
    } else {
        const char* host_end = base_scheme + 3;
        while (*host_end && *host_end != '/' && *host_end != '?' &&
               *host_end != '#')
            host_end++;
        const char* query = host_end;
        while (*query && *query != '?' && *query != '#') query++;
        const char* last_slash = NULL;
        for (const char* p = host_end; p < query; p++)
            if (*p == '/') last_slash = p;
        if (last_slash) {
            mx_copy_n(result, sizeof(result), base,
                      (size_t)(last_slash - base) + 1);
        } else {
            mx_copy_n(result, sizeof(result), base, (size_t)(host_end - base));
            mx_append(result, sizeof(result), "/");
        }
        mx_append(result, sizeof(result), ref);
    }
    url_normalize_path(result);
    mx_copy(out, capacity, result);
    return strlen(result) < capacity;
}

int mx_url_local_path(const char* url, char* out, size_t capacity) {
    if (!url || !capacity) return 0;
    const char* path = url;
    if (mx_starts_with_nocase(url, "file://")) {
        path = url + 7;
        while (path[0] == '/' && path[1] == '/') path++;
        if (path[0] == '/' && path[1] >= '0' && path[1] <= '9' && path[2] == ':') path++;
        else if (path[0] == '/') {
            if (capacity < 3) return 0;
            out[0] = '0';
            out[1] = ':';
            mx_copy(out + 2, capacity - 2, path);
            goto cut;
        } else if (!(path[0] >= '0' && path[0] <= '9' && path[1] == ':')) {
            return 0;
        }
    } else if (!(url[0] >= '0' && url[0] <= '9' && url[1] == ':' && url[2] == '/')) {
        return 0;
    }
    mx_copy(out, capacity, path);
cut:
    /* a #fragment or ?query is not part of the file name */
    for (char* p = out; *p; p++)
        if (*p == '#' || *p == '?') { *p = '\0'; break; }
    return out[0] != '\0';
}
