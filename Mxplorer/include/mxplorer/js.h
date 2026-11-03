#ifndef MXPLORER_JS_H
#define MXPLORER_JS_H

#include <stddef.h>
#include "mxplorer/dom.h"

/* A small JavaScript interpreter with DOM bindings. One page is active at
 * a time; mx_js_begin() discards the previous page's heap. */

typedef struct {
    void (*navigate)(const char* url, void* user);    /* location changes */
    void (*alert)(const char* text, void* user);
    /* Synchronous GET for fetch()/XMLHttpRequest/dynamic scripts. On
     * success *body is malloc()ed. NULL: network access is off. */
    int (*fetch)(void* user, const char* url, const char* kind, char** body,
                 size_t* length);
    void* user;
} mx_js_host_t;

typedef struct {
    int client_x;
    int client_y;
    int button;
    char key[24];        /* KeyboardEvent.key */
    int key_code;
    int shift, ctrl, alt;
} mx_js_event_info_t;

void mx_js_begin(mx_document_t* doc, const char* url, const mx_js_host_t* host);
void mx_js_end(void);
int mx_js_active(void);
/* Runs a script; errors go to the console. Returns 1 on success. */
int mx_js_run(const char* source, size_t length, const char* name);
/* Runs an ES module whose address is `url` (imports resolve against it). */
int mx_js_run_module(const char* source, size_t length, const char* url,
                     const char* name);
/* Sets the page's import map (JSON text of <script type=importmap>). */
void mx_js_set_import_map(const char* json, size_t length);
/* Evaluates console input and formats the result. */
int mx_js_eval(const char* source, char* result, size_t capacity);
/* Dispatches a DOM event with bubbling. Returns 1 if preventDefault(). */
int mx_js_dispatch(mx_node_t* target, const char* type,
                   const mx_js_event_info_t* info);
/* Fires an event on window and document (DOMContentLoaded, load). */
void mx_js_dispatch_window(const char* type);
/* Milliseconds until the next timer, or -1 when none are pending. */
long mx_js_next_timer(long now_ms);
void mx_js_run_timers(long now_ms);
size_t mx_js_heap_bytes(void);
/* Makes `node` available as $0 in the console (devtools selection). */
void mx_js_set_inspected(mx_node_t* node);
unsigned mx_js_timer_count(void);
/* Forgets cookies and localStorage of all sites. */
void mx_js_clear_storage(void);

#endif
