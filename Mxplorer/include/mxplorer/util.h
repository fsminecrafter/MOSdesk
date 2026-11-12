#ifndef MXPLORER_UTIL_H
#define MXPLORER_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Bump allocator. malloc() is a system call on Minimal-OS, so the DOM, the
 * style system and the JavaScript heap take memory in large chunks and free
 * it all at once when the page goes away. */
typedef struct mx_arena_chunk {
    struct mx_arena_chunk* next;
    size_t used;
    size_t capacity;
} mx_arena_chunk_t;

typedef struct {
    mx_arena_chunk_t* head;
    size_t total;          /* bytes handed out */
    size_t limit;          /* 0: no limit */
    int failed;
} mx_arena_t;

void* mx_arena_alloc(mx_arena_t* arena, size_t size);
void* mx_arena_zalloc(mx_arena_t* arena, size_t size);
char* mx_arena_strndup(mx_arena_t* arena, const char* text, size_t length);
char* mx_arena_strdup(mx_arena_t* arena, const char* text);
void mx_arena_free_all(mx_arena_t* arena);

char mx_lower(char c);
int mx_is_space(char c);
int mx_streq_nocase(const char* a, const char* b);
int mx_strneq_nocase(const char* a, const char* b, size_t length);
int mx_starts_with_nocase(const char* text, const char* prefix);
void mx_copy(char* out, size_t capacity, const char* text);
void mx_copy_n(char* out, size_t capacity, const char* text, size_t length);
void mx_append(char* out, size_t capacity, const char* text);
void mx_append_int(char* out, size_t capacity, long value);
size_t mx_utf8_encode(uint32_t codepoint, char* out);
uint32_t mx_hash(const char* text, size_t length);
const char* mx_memchr(const char* text, char c, size_t length);

/* Resolves `reference` against `base` (absolute http(s)/mos URL). */
int mx_url_resolve(const char* base, const char* reference, char* out,
                   size_t capacity);
/* Local file addresses: "0:/a/b", "file:///0:/a/b" (and "file:///a/b",
 * which means drive 0) give the Minimal-OS path "0:/a/b". Returns 0 for
 * other addresses. */
int mx_url_local_path(const char* url, char* out, size_t capacity);

/* Milliseconds since boot (host builds use a monotonic clock). */
long mx_now_ms(void);

/* A lock for state the fetch threads share with the main thread (cache.c,
 * the fetch pool in main.c). Host builds are single-threaded. */
#ifdef MX_HOST
typedef struct { int unused; } mx_lock_t;
static inline void mx_lock(mx_lock_t* l) { (void)l; }
static inline void mx_unlock(mx_lock_t* l) { (void)l; }
#else
#include "thread.h"
typedef mos_mutex_t mx_lock_t;
static inline void mx_lock(mx_lock_t* l) { mos_mutex_lock(l); }
static inline void mx_unlock(mx_lock_t* l) { mos_mutex_unlock(l); }
#endif

#endif
