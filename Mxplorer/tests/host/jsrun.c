/* Runs a JavaScript file in an empty page and prints the console.
 *   jsrun <file.js> */
#include "mxplorer/html.h"
#include "mxplorer/js.h"
#include "mxplorer/devlog.h"
#include "mxplorer/text.h"
#include "mxplorer/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    static char src[4 << 20];
    size_t n = fread(src, 1, sizeof(src) - 1, f);
    fclose(f);
    src[n] = 0;
    static const char page[] = "<!doctype html><html><body><div id=app class='a b'>hi</div></body></html>";
    mx_html_options_t o = {"https://example.org/dir/page.html?x=1#top", 1024, 768, 0, 1, 0,
                           NULL, NULL, NULL, NULL, NULL};
    mx_html_load(page, sizeof(page) - 1, &o);
    mx_js_run(src, n, "test.js");
    /* let timers run */
    long clock = mx_now_ms(); for (int i = 0; i < 400 && mx_js_next_timer(clock) >= 0; i++) { clock += 5; mx_js_run_timers(clock + mx_js_next_timer(clock)); }
    for (int i = 0; i < mx_console_count(); i++) {
        const mx_console_entry_t* e = mx_console_get(i);
        for (int r = 0; r < e->repeat; r++)
            { if (e->level == MX_LOG_ERROR) printf("ERROR %s:%d ", e->source, e->line); printf("%s\n", e->text); }
    }
    return 0;
}
