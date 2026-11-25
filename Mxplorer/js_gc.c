/* Garbage collector for the script heap.
 *
 * Conservative mark-and-sweep (in the spirit of the Boehm collector): the
 * interpreter keeps heap pointers in C locals, on its own stacks
 * (the 1 MiB script stack, generator/async coroutine stacks, the scope
 * stack), in static variables and in DOM nodes, so there is no exact list
 * of references. Instead every word in those places that points into a
 * live heap block keeps that block alive, and the blocks it reaches are
 * scanned the same way. Nothing reachable is ever freed; at worst a stray
 * number that looks like a pointer keeps some garbage alive.
 *
 * Small blocks (up to 4 KiB) come from size-class chunks with free lists;
 * larger ones are allocated one by one. A collection runs when the heap
 * has grown by as much as was alive after the last one (at least 8 MiB),
 * and before the heap would take memory the system needs.
 *
 * Roots: the program's data and BSS (needs the __mos_data_start /
 * __mos_bss_end symbols from the SDK's link.ld; on the host,
 * __data_start / _end), the stacks and coroutines (js.c), and the
 * document's arena with its nodes' wrappers and listeners (js_dom.c).
 * Without the symbols the collector stays off and the heap only grows,
 * as it did before.
 */
#include "mxplorer/js_internal.h"
#include "string.h"
#include "stdlib.h"

#define GC_SMALL_MAX 4096u
#define GC_CHUNK_DATA (64u * 1024u)
#define GC_MIN_TRIGGER (8u * 1024u * 1024u)
#define JS_HEAP_RESERVE (96ull * 1024u * 1024u)

#ifdef MX_HOST
extern char __data_start[], _end[];
static char* data_lo(void) { return __data_start; }
static char* data_hi(void) { return _end; }
static int heap_may_grow(size_t total, size_t size) {
    return total + size < (size_t)2048u * 1024u * 1024u;
}
#else
#include "user_sysinfo.h"
extern char __mos_data_start[] __attribute__((weak));
extern char __mos_bss_end[] __attribute__((weak));
static char* data_lo(void) { return __mos_data_start; }
static char* data_hi(void) { return __mos_bss_end; }
static int heap_may_grow(size_t total, size_t size) {
    (void)total;
    uint64_t free_bytes = mos_sysinfo_ram_free();
    return free_bytes > JS_HEAP_RESERVE + size + GC_CHUNK_DATA;
}
#endif

static const uint32_t class_size[] = {
    16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512,
    640, 768, 896, 1024, 1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096};
#define GC_CLASSES (sizeof(class_size) / sizeof(class_size[0]))

typedef struct gc_chunk {
    char* data;
    char* end;          /* data + count * size */
    uint32_t size;      /* block size */
    uint32_t count;     /* blocks */
    uint32_t bump;      /* blocks handed out from fresh memory so far */
    uint32_t live;      /* blocks marked in the last collection */
    uint8_t cls;        /* size class, 0xFF: one large block */
    uint64_t* marks;
} gc_chunk;

static gc_chunk** chunks;           /* sorted by data address */
static uint32_t chunk_count, chunk_capacity;
static char* heap_lo;
static char* heap_hi;
static void* free_list[GC_CLASSES];
static gc_chunk* bump_chunk[GC_CLASSES];
static uint8_t size_to_class[GC_SMALL_MAX / 16 + 1];

static size_t heap_total;           /* bytes in chunks and large blocks */
static size_t allocated_since;      /* bytes handed out since the last collection */
static size_t live_after;           /* bytes alive after the last collection */
static size_t trigger;
static int inhibit;
static int enabled;
static unsigned collections;
static int stress;

static void** mark_stack;
static size_t mark_top, mark_capacity;
static int mark_failed;

/* ---------------- chunks ---------------- */

static void chunk_insert(gc_chunk* c) {
    if (chunk_count == chunk_capacity) {
        uint32_t capacity = chunk_capacity ? chunk_capacity * 2 : 64;
        gc_chunk** grown = (gc_chunk**)realloc(chunks, sizeof(gc_chunk*) * capacity);
        if (!grown) return;
        chunks = grown;
        chunk_capacity = capacity;
    }
    uint32_t lo = 0, hi = chunk_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (chunks[mid]->data < c->data) lo = mid + 1;
        else hi = mid;
    }
    memmove(chunks + lo + 1, chunks + lo, sizeof(gc_chunk*) * (chunk_count - lo));
    chunks[lo] = c;
    chunk_count++;
    if (!heap_lo || c->data < heap_lo) heap_lo = c->data;
    if (c->end > heap_hi) heap_hi = c->end;
}

static gc_chunk* chunk_find(const char* p) {
    uint32_t lo = 0, hi = chunk_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        gc_chunk* c = chunks[mid];
        if (p < c->data) hi = mid;
        else if (p >= c->end) lo = mid + 1;
        else return c;
    }
    return NULL;
}

static gc_chunk* chunk_new(uint32_t size, uint32_t count, uint8_t cls) {
    size_t mark_words = (count + 63) / 64;
    size_t header = (sizeof(gc_chunk) + mark_words * 8 + 15) & ~(size_t)15;
    size_t bytes = header + (size_t)size * count + 16;
    if (!heap_may_grow(heap_total, bytes)) return NULL;
    char* raw = (char*)malloc(bytes);
    if (!raw) return NULL;
    gc_chunk* c = (gc_chunk*)raw;
    memset(c, 0, header);
    c->marks = (uint64_t*)(raw + sizeof(gc_chunk));
    c->data = (char*)(((uintptr_t)(raw + header) + 15) & ~(uintptr_t)15);
    c->size = size;
    c->count = count;
    c->end = c->data + (size_t)size * count;
    c->cls = cls;
    c->bump = cls == 0xFF ? 1 : 0;
    heap_total += bytes;
    chunk_insert(c);
    return c;
}

/* ---------------- allocation ---------------- */

void js_gc_collect(void);

static void* alloc_small(uint32_t cls) {
    void* p = free_list[cls];
    if (p) {
        free_list[cls] = *(void**)p;
        return p;
    }
    gc_chunk* c = bump_chunk[cls];
    if (!c || c->bump == c->count) {
        uint32_t size = class_size[cls];
        c = chunk_new(size, GC_CHUNK_DATA / size, (uint8_t)cls);
        if (!c) return NULL;
        bump_chunk[cls] = c;
    }
    return c->data + (size_t)c->size * c->bump++;
}

static void* alloc_raw(size_t size) {
    if (size <= GC_SMALL_MAX) {
        uint32_t cls = size_to_class[(size + 15) / 16];
        void* p = alloc_small(cls);
        if (p) memset(p, 0, class_size[cls]);
        return p;
    }
    size = (size + 15) & ~(size_t)15;
    if (size > 0xFFFFFFF0u) return NULL;
    gc_chunk* c = chunk_new((uint32_t)size, 1, 0xFF);
    if (!c) return NULL;
    memset(c->data, 0, size);
    return c->data;
}

void* js_alloc(size_t size) {
    if (!size) size = 1;
    if (enabled && !inhibit && allocated_since >= trigger) js_gc_collect();
    void* p = alloc_raw(size);
    if (!p && enabled && !inhibit) {
        js_gc_collect();   /* the system is low on memory: free what we can */
        p = alloc_raw(size);
    }
    if (!p) {
        /* Out of memory: flag it (the interpreter stops at the next
         * statement) and hand out memory anyway so that half-finished
         * operations stay valid. */
        js_rt.out_of_memory = 1;
        p = calloc(1, size);
        if (!p) {
            static char last_resort[64];
            p = last_resort;   /* nothing left at all; the script is stopping */
        }
    }
    allocated_since += size;
    return p;
}

void js_gc_inhibit(int delta) {
    inhibit += delta;
}

/* ---------------- marking ---------------- */

static void mark_push(void* block) {
    if (mark_top == mark_capacity) {
        size_t capacity = mark_capacity ? mark_capacity * 2 : 4096;
        void** grown = (void**)realloc(mark_stack, sizeof(void*) * capacity);
        if (!grown) {
            mark_failed = 1;
            return;
        }
        mark_stack = grown;
        mark_capacity = capacity;
    }
    mark_stack[mark_top++] = block;
}

static inline void mark_word(uintptr_t w) {
    const char* p = (const char*)w;
    if (p < heap_lo || p >= heap_hi) return;
    gc_chunk* c = chunk_find(p);
    if (!c) return;
    uint32_t index = (uint32_t)((size_t)(p - c->data) / c->size);
    if (index >= c->bump) return;   /* never handed out */
    uint64_t bit = 1ull << (index & 63);
    uint64_t* word = &c->marks[index >> 6];
    if (*word & bit) return;
    *word |= bit;
    c->live++;
    mark_push(c->data + (size_t)c->size * index);
}

void js_gc_mark_range(const void* start, const void* end) {
    uintptr_t a = ((uintptr_t)start + 7) & ~(uintptr_t)7;
    uintptr_t b = (uintptr_t)end & ~(uintptr_t)7;
    for (; a < b; a += 8) mark_word(*(const uintptr_t*)a);
}

static void drain(void) {
    while (mark_top) {
        char* block = (char*)mark_stack[--mark_top];
        gc_chunk* c = chunk_find(block);
        if (c) js_gc_mark_range(block, block + c->size);
    }
}

/* ---------------- collection ---------------- */

static void sweep(void) {
    void** tails[GC_CLASSES];
    for (uint32_t k = 0; k < GC_CLASSES; k++) {
        free_list[k] = NULL;
        tails[k] = &free_list[k];
    }
    size_t live_bytes = 0;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < chunk_count; i++) {
        gc_chunk* c = chunks[i];
        int empty = c->live == 0 && (c->cls == 0xFF || c != bump_chunk[c->cls]);
        if (empty) {
            size_t mark_words = (c->count + 63) / 64;
            size_t header = (sizeof(gc_chunk) + mark_words * 8 + 15) & ~(size_t)15;
            heap_total -= header + (size_t)c->size * c->count + 16;
            free(c);
            continue;
        }
        if (c->cls != 0xFF) {
            /* unmarked blocks go back on the free list (in address order,
             * which keeps new objects close together) */
            void** tail = tails[c->cls];
            for (uint32_t b = 0; b < c->bump; b++) {
                if (c->marks[b >> 6] & (1ull << (b & 63))) continue;
                void** block = (void**)(c->data + (size_t)c->size * b);
                *block = NULL;
                *tail = block;
                tail = block;
            }
            tails[c->cls] = tail;
        }
        live_bytes += (size_t)c->size * c->live;
        memset(c->marks, 0, ((c->count + 63) / 64) * 8);
        c->live = 0;
        chunks[kept++] = c;
    }
    chunk_count = kept;
    heap_lo = heap_hi = NULL;
    for (uint32_t i = 0; i < chunk_count; i++) {
        if (!heap_lo || chunks[i]->data < heap_lo) heap_lo = chunks[i]->data;
        if (chunks[i]->end > heap_hi) heap_hi = chunks[i]->end;
    }
    live_after = live_bytes;
}

void js_gc_collect(void) {
    if (!enabled || inhibit || !chunk_count) return;
    inhibit++;
    mark_top = 0;
    mark_failed = 0;

    /* registers of this and the calling frames end up on the stack */
    __builtin_unwind_init();
    js_gc_mark_range(data_lo(), data_hi());
    js_core_gc_roots();
    js_dom_gc_roots();
    drain();

    if (mark_failed) {
        /* could not finish marking: free nothing this time */
        for (uint32_t i = 0; i < chunk_count; i++) {
            memset(chunks[i]->marks, 0, ((chunks[i]->count + 63) / 64) * 8);
            chunks[i]->live = 0;
        }
    } else {
        sweep();
        collections++;
    }
    allocated_since = 0;
    trigger = live_after > GC_MIN_TRIGGER ? live_after : GC_MIN_TRIGGER;
    if (stress) trigger = (size_t)stress;
    inhibit--;
}

/* ---------------- lifetime ---------------- */

void js_gc_init(void) {
    js_gc_free_all();
    for (uint32_t k = 0, s = 0; s <= GC_SMALL_MAX / 16; s++) {
        while (class_size[k] < s * 16) k++;
        size_to_class[s] = (uint8_t)k;
    }
    enabled = data_lo() != NULL && data_hi() > data_lo();
    trigger = GC_MIN_TRIGGER;
#ifdef MX_HOST
    /* tests: collect every 64 KiB of allocation to flush out missed roots */
    const char* stress_env = getenv("MX_GC_STRESS");
    stress = stress_env ? (atoi(stress_env) > 0 ? atoi(stress_env) : 64 * 1024) : 0;
    if (getenv("MX_GC_OFF")) enabled = 0;
    if (stress) trigger = (size_t)stress;
#endif
}

void js_gc_free_all(void) {
    for (uint32_t i = 0; i < chunk_count; i++) free(chunks[i]);
    chunk_count = 0;
    heap_lo = heap_hi = NULL;
    for (uint32_t k = 0; k < GC_CLASSES; k++) {
        free_list[k] = NULL;
        bump_chunk[k] = NULL;
    }
    heap_total = allocated_since = live_after = 0;
    inhibit = 0;
}

size_t js_gc_heap_bytes(void) {
    return live_after + allocated_since;
}

/* memory the heap holds from the system (chunks and large blocks) */
size_t js_gc_footprint(void) {
    return heap_total;
}

unsigned js_gc_collections(void) {
    return collections;
}
