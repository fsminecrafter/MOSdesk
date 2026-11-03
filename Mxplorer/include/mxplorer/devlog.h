#ifndef MXPLORER_DEVLOG_H
#define MXPLORER_DEVLOG_H

#include <stddef.h>
#include <stdint.h>

/* Console and network logs shown by the developer tools. */

typedef enum {
    MX_LOG_LOG,
    MX_LOG_INFO,
    MX_LOG_WARN,
    MX_LOG_ERROR,
    MX_LOG_DEBUG,
    MX_LOG_INPUT,     /* a command typed into the console */
    MX_LOG_RESULT,    /* its result */
    MX_LOG_SYSTEM     /* browser messages (page loads, HTTP) */
} mx_log_level_t;

#define MX_CONSOLE_MAX 400
#define MX_CONSOLE_TEXT 240
#define MX_NETWORK_MAX 64

typedef struct {
    uint8_t level;
    uint16_t repeat;          /* identical consecutive messages */
    int line;                 /* script line, 0 if none */
    long time_ms;
    char source[48];          /* "inline script 2", "app.js", "HTTP" */
    char text[MX_CONSOLE_TEXT];
} mx_console_entry_t;

typedef struct {
    char url[256];
    char kind[16];            /* document, stylesheet, script */
    char content_type[48];
    char error[96];
    int status;               /* HTTP status, 0 if the request failed */
    size_t size;
    long started_ms;
    long duration_ms;
    int redirects;
    int from_cache;
} mx_network_entry_t;

void mx_console_add(mx_log_level_t level, const char* source, int line,
                    const char* text);
void mx_console_clear(void);
int mx_console_count(void);
const mx_console_entry_t* mx_console_get(int index);
int mx_console_count_level(mx_log_level_t level);
unsigned mx_console_version(void);   /* changes on every add/clear */

void mx_network_clear(void);
mx_network_entry_t* mx_network_add(const char* url, const char* kind);
int mx_network_count(void);
const mx_network_entry_t* mx_network_get(int index);

#endif
