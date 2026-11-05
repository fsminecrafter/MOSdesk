#include "mxplorer/devlog.h"
#include "mxplorer/util.h"
#include "string.h"
#ifndef MX_HOST
#include "minimalos.h"
#endif

static mx_console_entry_t console_entries[MX_CONSOLE_MAX];
static int console_first;
static int console_size;
static unsigned console_changes;
static mx_network_entry_t network_entries[MX_NETWORK_MAX];
static int network_size;

static mx_console_entry_t* console_at(int index) {
    return &console_entries[(console_first + index) % MX_CONSOLE_MAX];
}

void mx_console_add(mx_log_level_t level, const char* source, int line,
                    const char* text) {
    console_changes++;
    if (console_size) {
        mx_console_entry_t* last = console_at(console_size - 1);
        if (last->level == level && last->line == line &&
            !strcmp(last->text, text ? text : "") &&
            !strcmp(last->source, source ? source : "") &&
            level != MX_LOG_INPUT && level != MX_LOG_RESULT) {
            if (last->repeat < 65535) last->repeat++;
            return;
        }
    }
    mx_console_entry_t* entry;
    if (console_size < MX_CONSOLE_MAX) {
        entry = console_at(console_size++);
    } else {
        console_first = (console_first + 1) % MX_CONSOLE_MAX;
        entry = console_at(console_size - 1);
    }
    entry->level = (uint8_t)level;
    entry->repeat = 1;
    entry->line = line;
    entry->time_ms = mx_now_ms();
    mx_copy(entry->source, sizeof(entry->source), source ? source : "");
    mx_copy(entry->text, sizeof(entry->text), text ? text : "");
#ifndef MX_HOST
    /* Also to the kernel log, so 'make netboot' shows site errors live. */
    static const char* const names[] = {"log", "info", "warn", "error", "debug", "input", "result", "page"};
    static char log_line[sizeof(entry->text) + sizeof(entry->source) + 48];
    log_line[0] = 0;
    mx_append(log_line, sizeof(log_line), "console ");
    mx_append(log_line, sizeof(log_line),
              (unsigned)level < sizeof(names) / sizeof(names[0]) ? names[level] : "?");
    mx_append(log_line, sizeof(log_line), ": ");
    if (entry->source[0]) {
        mx_append(log_line, sizeof(log_line), entry->source);
        if (line > 0) {
            mx_append(log_line, sizeof(log_line), ":");
            mx_append_int(log_line, sizeof(log_line), line);
        }
        mx_append(log_line, sizeof(log_line), " ");
    }
    mx_append(log_line, sizeof(log_line), entry->text);
    mos_log(log_line);
#endif
}

void mx_console_clear(void) {
    console_first = console_size = 0;
    console_changes++;
}

int mx_console_count(void) {
    return console_size;
}

const mx_console_entry_t* mx_console_get(int index) {
    if (index < 0 || index >= console_size) return NULL;
    return console_at(index);
}

int mx_console_count_level(mx_log_level_t level) {
    int count = 0;
    for (int i = 0; i < console_size; i++)
        if (console_at(i)->level == level) count += console_at(i)->repeat;
    return count;
}

unsigned mx_console_version(void) {
    return console_changes;
}

void mx_network_clear(void) {
    network_size = 0;
}

mx_network_entry_t* mx_network_add(const char* url, const char* kind) {
    if (network_size == MX_NETWORK_MAX) {
        memmove(network_entries, network_entries + 1,
                sizeof(network_entries[0]) * (MX_NETWORK_MAX - 1));
        network_size--;
    }
    mx_network_entry_t* entry = &network_entries[network_size++];
    memset(entry, 0, sizeof(*entry));
    mx_copy(entry->url, sizeof(entry->url), url);
    mx_copy(entry->kind, sizeof(entry->kind), kind);
    entry->started_ms = mx_now_ms();
    return entry;
}

int mx_network_count(void) {
    return network_size;
}

const mx_network_entry_t* mx_network_get(int index) {
    return index >= 0 && index < network_size ? &network_entries[index] : NULL;
}
