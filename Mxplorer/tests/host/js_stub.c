#include "mxplorer/js.h"
void mx_js_begin(mx_document_t* d, const char* u, const mx_js_host_t* h) { (void)d; (void)u; (void)h; }
void mx_js_end(void) {}
int mx_js_active(void) { return 0; }
int mx_js_run(const char* s, size_t l, const char* n) { (void)s; (void)l; (void)n; return 1; }
int mx_js_eval(const char* s, char* r, size_t c) { (void)s; (void)r; (void)c; return 0; }
int mx_js_dispatch(mx_node_t* t, const char* ty, const mx_js_event_info_t* i) { (void)t; (void)ty; (void)i; return 0; }
void mx_js_dispatch_window(const char* t) { (void)t; }
long mx_js_next_timer(long n) { (void)n; return -1; }
void mx_js_run_timers(long n) { (void)n; }
size_t mx_js_heap_bytes(void) { return 0; }
unsigned mx_js_timer_count(void) { return 0; }
int mx_js_run_module(const char* s, size_t l, const char* u, const char* n) { (void)s; (void)l; (void)u; (void)n; return 1; }
void mx_js_set_import_map(const char* j, size_t l) { (void)j; (void)l; }
void mx_js_set_inspected(mx_node_t* n) { (void)n; }
void mx_js_clear_storage(void) {}
