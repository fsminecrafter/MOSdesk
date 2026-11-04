#ifndef MXPLORER_MEDIA_H
#define MXPLORER_MEDIA_H

/* Pictures (.img) and videos (.vid) in pages: <img>, <video> and pages
 * that are a picture or video themselves. The formats are Minimal-OS's own
 * (mosmedia.h); other picture formats keep the placeholder.
 *
 * Files come from the network through the page's fetch function, or
 * straight from disk for local addresses (0:/..., file:///0:/...). Local
 * videos are read as they play; downloaded ones are kept in memory. */

#include <stddef.h>
#include <stdint.h>
#include "mxplorer/dom.h"
#include "mxplorer/gfx.h"
#include "mxplorer/html.h"

enum { MX_MEDIA_NONE, MX_MEDIA_IMAGE, MX_MEDIA_VIDEO };

/* MX_MEDIA_IMAGE for ...x.img, MX_MEDIA_VIDEO for ...x.vid (case and
 * ?query/#fragment ignored), else MX_MEDIA_NONE. */
int mx_media_kind_of_url(const char* url);

/* Starts a page. `fetch` loads network files (NULL: none); `local` allows
 * local files (only pages that are local files themselves get it). */
void mx_media_begin(mx_fetch_fn fetch, void* user, int local);
/* Frees everything and stops sound this page started. */
void mx_media_end(void);

/* The element's picture or video size in pixels, loading it on first use.
 * 0 when the element has none (yet) or it failed. */
int mx_media_natural_size(mx_node_t* n, int* w, int* h);
/* 1 when the element names a .img/.vid that could not be loaded. */
int mx_media_failed(mx_node_t* n);

/* Draws the element's content box (x, y, w, h). `opacity` 0-255. */
void mx_media_paint(const mx_canvas_t* c, mx_node_t* n, int x, int y, int w, int h,
                    unsigned opacity, uint32_t accent);

/* A click at (x, y) on a video whose content box is (bx, by, bw, bh).
 * Returns 1 when the video used it (play/pause, seek). */
int mx_media_click(mx_node_t* n, int x, int y, int bx, int by, int bw, int bh);
/* 1 when the element is a video with controls (a pointer cursor). */
int mx_media_pointer(mx_node_t* n);

/* Advances playing videos. Returns 1 when the page needs a redraw. */
int mx_media_tick(long now_ms);
/* 1 while a video is playing (the host should keep ticking quickly). */
int mx_media_playing(void);

/* JavaScript: HTMLMediaElement / HTMLImageElement. */
void mx_media_play(mx_node_t* n);
void mx_media_pause(mx_node_t* n);
int mx_media_paused(mx_node_t* n);
int mx_media_ended(mx_node_t* n);
double mx_media_current_time(mx_node_t* n);   /* seconds */
void mx_media_seek(mx_node_t* n, double seconds);
double mx_media_duration(mx_node_t* n);       /* seconds, 0 when unknown */
void mx_media_set_muted(mx_node_t* n, int muted);
int mx_media_muted(mx_node_t* n);

#endif
