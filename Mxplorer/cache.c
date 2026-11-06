/* Disk cache for pages, their files and converted pictures. See
 * mxplorer/cache.h.
 *
 * Layout: 0:/data/mxcache/index.dat lists the entries (fixed-size
 * records); each body is 0:/data/mxcache/<key>.bin, the key being a
 * 64-bit hash of the address. The index is kept in memory and written
 * back by mx_cache_flush() (after each page load and on exit), not on
 * every file. A body whose file has gone is dropped when it is read. */
#include "mxplorer/cache.h"
#include "mxplorer/util.h"
#include "string.h"
#include "stdlib.h"

#ifdef MX_HOST
#include <stdio.h>
#include <time.h>
#else
#include "minimalos.h"
#endif

#define CACHE_DIR "0:/data/mxcache"
#define CACHE_INDEX CACHE_DIR "/index.dat"
#define CACHE_MAX_ENTRIES 1024u
#define CACHE_MAGIC 0x3143584Du        /* "MXC1" */
#define CACHE_WRITE_CHUNK 65536u

typedef struct {
    uint64_t key;
    uint64_t used;              /* LRU tick, higher = more recent */
    int64_t stored;             /* Unix seconds of the last download or check; 0 = unknown */
    int64_t expires;            /* Unix seconds it is fresh until; 0 = always ask */
    uint32_t size;
    int32_t status;
    char etag[128];
    char last_modified[64];
    char content_type[64];
} entry_t;

static entry_t entries[CACHE_MAX_ENTRIES];
static uint32_t entry_count;
static uint64_t tick;
static uint64_t used_bytes;
static uint32_t limit_mb = MX_CACHE_DEFAULT_MB;
static int loaded, dirty;

/* ---------------------------------------------------------------- files */

#ifdef MX_HOST
/* On the build PC the cache is off unless MX_CACHE_DIR names a folder. */
static const char* host_dir(void) { return getenv("MX_CACHE_DIR"); }

static int host_path(const char* path, char* out, size_t capacity) {
    const char* dir = host_dir();
    if (!dir || strncmp(path, CACHE_DIR, strlen(CACHE_DIR))) return 0;
    snprintf(out, capacity, "%s%s", dir, path + strlen(CACHE_DIR));
    return 1;
}

static int fs_write(const char* path, const void* data, size_t length) {
    char p[512];
    if (!host_path(path, p, sizeof(p))) return 0;
    FILE* f = fopen(p, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, length, f) == length;
    return fclose(f) == 0 && ok;
}

static char* fs_read(const char* path, size_t limit, size_t* length) {
    char p[512];
    if (!host_path(path, p, sizeof(p))) return NULL;
    FILE* f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* d = n >= 0 && (size_t)n <= limit ? (char*)malloc((size_t)n + 1) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d) { d[n] = 0; *length = (size_t)n; }
    return d;
}

static void fs_delete(const char* path) {
    char p[512];
    if (host_path(path, p, sizeof(p))) remove(p);
}

static int fs_ready(void) { return host_dir() != NULL; }

static int64_t unix_now(void) { return (int64_t)time(NULL); }
#else
static int fs_ready(void) {
    if (!mos_exists("0:/data") && mos_mkdir("0:/data") != SYS_SUCCESS) return 0;
    if (!mos_exists(CACHE_DIR) && mos_mkdir(CACHE_DIR) != SYS_SUCCESS) return 0;
    return 1;
}

static void fs_delete(const char* path) {
    if (mos_exists(path)) mos_syscall(SYS_DELETE, (long)(uintptr_t)path, 0, 0);
}

static int fs_write(const char* path, const void* data, size_t length) {
    fs_delete(path);
    if (mos_create(path, NULL, NULL) != SYS_SUCCESS) return 0;
    long fd = mos_open(path, SYS_O_RDWR);
    if (fd < 0) return 0;
    const char* p = (const char*)data;
    int ok = 1;
    for (size_t done = 0; done < length && ok;) {
        size_t n = length - done > CACHE_WRITE_CHUNK ? CACHE_WRITE_CHUNK : length - done;
        ok = mos_fwrite(fd, p + done, n) == (long)n;
        done += n;
    }
    if (mos_close(fd) != SYS_SUCCESS) ok = 0;
    if (!ok) fs_delete(path);
    return ok;
}

static char* fs_read(const char* path, size_t limit, size_t* length) {
    long fd = mos_open(path, SYS_O_RDONLY);
    if (fd < 0) return NULL;
    long n = mos_size(fd);
    char* d = n >= 0 && (size_t)n <= limit ? (char*)malloc((size_t)n + 1) : NULL;
    size_t got = 0;
    while (d && got < (size_t)n) {
        long r = mos_read(fd, d + got, (size_t)n - got);
        if (r <= 0) { free(d); d = NULL; break; }
        got += (size_t)r;
    }
    mos_close(fd);
    if (d) { d[n] = 0; *length = (size_t)n; }
    return d;
}

typedef struct {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
} cache_datetime_t;

/* Unix seconds from the system clock; 0 while the clock is not set
 * (then nothing counts as fresh, everything is checked with the server). */
static int64_t unix_now(void) {
    cache_datetime_t t;
    memset(&t, 0, sizeof(t));
    if (mos_syscall(SYS_GETTIME, (long)(uintptr_t)&t, 0, 0) != SYS_SUCCESS || t.year < 2024)
        return 0;
    int64_t y = t.year, m = t.month, d = t.day;
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
}
#endif

/* ---------------------------------------------------------------- index */

static uint64_t key_of(const char* url) {
    uint64_t h = 1469598103934665603ull;
    for (const char* p = url; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    return h ? h : 1;
}

static void body_path(uint64_t key, char* out, size_t capacity) {
    static const char hex[] = "0123456789abcdef";
    char name[17];
    for (int i = 0; i < 16; i++) name[i] = hex[(key >> (60 - 4 * i)) & 15];
    name[16] = 0;
    mx_copy(out, capacity, CACHE_DIR "/");
    mx_append(out, capacity, name);
    mx_append(out, capacity, ".bin");
}

static void load_index(void) {
    if (loaded) return;
    loaded = 1;
    entry_count = 0;
    used_bytes = 0;
    if (!fs_ready()) return;
    size_t n = 0;
    char* d = fs_read(CACHE_INDEX, 16u + sizeof(entries), &n);
    if (!d) return;
    uint32_t magic, count;
    if (n >= 16) {
        memcpy(&magic, d, 4);
        memcpy(&count, d + 4, 4);
        memcpy(&tick, d + 8, 8);
        if (magic == CACHE_MAGIC && count <= CACHE_MAX_ENTRIES && n >= 16u + count * sizeof(entry_t)) {
            memcpy(entries, d + 16, count * sizeof(entry_t));
            entry_count = count;
            for (uint32_t i = 0; i < count; i++) used_bytes += entries[i].size;
        }
    }
    free(d);
}

void mx_cache_flush(void) {
    if (!dirty || !fs_ready()) return;
    size_t n = 16u + entry_count * sizeof(entry_t);
    char* d = (char*)malloc(n);
    if (!d) return;
    uint32_t magic = CACHE_MAGIC;
    memcpy(d, &magic, 4);
    memcpy(d + 4, &entry_count, 4);
    memcpy(d + 8, &tick, 8);
    memcpy(d + 16, entries, entry_count * sizeof(entry_t));
    if (fs_write(CACHE_INDEX, d, n)) dirty = 0;
    free(d);
}

static entry_t* find(uint64_t key) {
    for (uint32_t i = 0; i < entry_count; i++)
        if (entries[i].key == key) return &entries[i];
    return NULL;
}

static void drop(entry_t* e) {
    char path[64];
    body_path(e->key, path, sizeof(path));
    fs_delete(path);
    used_bytes -= e->size;
    *e = entries[--entry_count];
    dirty = 1;
}

/* Least recently used first, until `need` more bytes fit. */
static void make_room(uint64_t need) {
    uint64_t limit = (uint64_t)limit_mb * 1024u * 1024u;
    while (entry_count && (used_bytes + need > limit || entry_count >= CACHE_MAX_ENTRIES)) {
        uint32_t oldest = 0;
        for (uint32_t i = 1; i < entry_count; i++)
            if (entries[i].used < entries[oldest].used) oldest = i;
        drop(&entries[oldest]);
    }
}

void mx_cache_init(void) { load_index(); }

void mx_cache_set_limit_mb(uint32_t mb) {
    limit_mb = mb;
    load_index();
    if (!limit_mb) mx_cache_clear();
    else make_room(0);
    mx_cache_flush();
}

uint32_t mx_cache_limit_mb(void) { return limit_mb; }
uint64_t mx_cache_used_bytes(void) { load_index(); return used_bytes; }
uint32_t mx_cache_count(void) { load_index(); return entry_count; }

void mx_cache_clear(void) {
    load_index();
    while (entry_count) drop(&entries[entry_count - 1]);
    used_bytes = 0;
    dirty = 1;
    mx_cache_flush();
}

/* ------------------------------------------------------------- policy */

static int has_token(const char* header, const char* token) {
    size_t n = strlen(token);
    for (const char* p = header; *p; p++) {
        if ((p == header || p[-1] == ' ' || p[-1] == ',') && !strncmp(p, token, n) &&
            (p[n] == 0 || p[n] == ',' || p[n] == ' ' || p[n] == '='))
            return 1;
    }
    return 0;
}

/* Seconds the response may be used without asking; 0 = ask every time. */
static int64_t max_age(const char* cache_control) {
    if (has_token(cache_control, "no-cache") || has_token(cache_control, "must-revalidate"))
        return 0;
    const char* p = strstr(cache_control, "max-age=");
    if (!p) return 0;
    int64_t v = 0;
    for (p += 8; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    return v;
}

static int cacheable_url(const char* url) {
    return !strncmp(url, "http://", 7) || !strncmp(url, "https://", 8);
}

static void set_freshness(entry_t* e, const mx_http_info_t* info, int64_t now) {
    e->stored = now;
    int64_t age = max_age(info->cache_control);
    e->expires = now && age ? now + age : 0;
    if (info->etag[0]) mx_copy(e->etag, sizeof(e->etag), info->etag);
    if (info->last_modified[0])
        mx_copy(e->last_modified, sizeof(e->last_modified), info->last_modified);
}

static int store(const char* url, const char* body, size_t length, const mx_http_info_t* info) {
    if (!limit_mb || has_token(info->cache_control, "no-store") || info->status != 200) return 0;
    if ((uint64_t)length > (uint64_t)limit_mb * 1024u * 1024u / 4u) return 0;  /* not one file filling it */
    if (!fs_ready()) return 0;
    uint64_t key = key_of(url);
    entry_t* old = find(key);
    if (old) drop(old);
    make_room(length);
    char path[64];
    body_path(key, path, sizeof(path));
    if (!fs_write(path, body, length)) return 0;
    entry_t* e = &entries[entry_count++];
    memset(e, 0, sizeof(*e));
    e->key = key;
    e->used = ++tick;
    e->size = (uint32_t)length;
    e->status = info->status;
    mx_copy(e->content_type, sizeof(e->content_type), info->content_type);
    set_freshness(e, info, unix_now());
    used_bytes += length;
    dirty = 1;
    return 1;
}

/* The stored body into `body`; 0 (and the entry dropped) when it cannot be read. */
static int read_body(entry_t* e, char* body, size_t capacity, size_t* length) {
    char path[64];
    body_path(e->key, path, sizeof(path));
    size_t n = 0;
    char* d = e->size < capacity ? fs_read(path, capacity - 1, &n) : NULL;
    if (!d || n != e->size) {
        free(d);
        drop(e);
        return 0;
    }
    memcpy(body, d, n);
    body[n] = 0;
    free(d);
    *length = n;
    e->used = ++tick;
    dirty = 1;
    return 1;
}

static void fill_info(const entry_t* e, const char* url, mx_http_info_t* info) {
    memset(info, 0, sizeof(*info));
    info->status = e->status ? e->status : 200;
    mx_copy(info->content_type, sizeof(info->content_type), e->content_type);
    mx_copy(info->final_url, sizeof(info->final_url), url);
    mx_copy(info->etag, sizeof(info->etag), e->etag);
    mx_copy(info->last_modified, sizeof(info->last_modified), e->last_modified);
}

int mx_cache_get(const char* url, char* body, size_t body_capacity, size_t* body_length,
                 int allow_insecure_tls, char* error, size_t error_capacity,
                 mx_http_info_t* info, int* how) {
    mx_http_info_t local;
    if (!info) info = &local;
    *how = MX_CACHE_NETWORK;
    if (!limit_mb || !cacheable_url(url))
        return mx_http_get(url, body, body_capacity, body_length, allow_insecure_tls, error,
                           error_capacity, info);
    load_index();
    entry_t* e = find(key_of(url));
    int64_t now = unix_now();
    if (e && e->expires && now && now < e->expires &&
        read_body(e, body, body_capacity, body_length)) {
        fill_info(e, url, info);
        *how = MX_CACHE_FRESH;
        return 1;
    }
    e = find(key_of(url));                      /* read_body may have dropped it */
    if (e) mx_http_set_validators(e->etag, e->last_modified);
    int ok = mx_http_get(url, body, body_capacity, body_length, allow_insecure_tls, error,
                         error_capacity, info);
    e = find(key_of(url));
    if (ok && info->status == 304 && e) {
        mx_http_info_t fresh = *info;
        if (read_body(e, body, body_capacity, body_length)) {
            set_freshness(e, &fresh, now);
            fill_info(e, url, info);
            info->elapsed_ms = fresh.elapsed_ms;
            *how = MX_CACHE_REVALIDATED;
            return 1;
        }
        /* the copy is gone: ask again without validators */
        ok = mx_http_get(url, body, body_capacity, body_length, allow_insecure_tls, error,
                         error_capacity, info);
    }
    if (!ok) {
        if (e && read_body(e, body, body_capacity, body_length)) {
            fill_info(e, url, info);
            *how = MX_CACHE_OFFLINE;
            return 1;
        }
        return 0;
    }
    /* a redirect is stored under the address it ended at, too */
    store(url, body, *body_length, info);
    if (info->final_url[0] && strcmp(info->final_url, url)) store(info->final_url, body, *body_length, info);
    return 1;
}

void mx_cache_replace(const char* url, const void* data, size_t length) {
    if (!limit_mb || !cacheable_url(url)) return;
    load_index();
    entry_t* e = find(key_of(url));
    if (!e) return;
    char path[64];
    body_path(e->key, path, sizeof(path));
    used_bytes -= e->size;
    if (!fs_write(path, data, length)) {
        e->size = 0;
        drop(e);
        return;
    }
    e->size = (uint32_t)length;
    used_bytes += length;
    mx_copy(e->content_type, sizeof(e->content_type), "image/x-mos-img");
    dirty = 1;
}
