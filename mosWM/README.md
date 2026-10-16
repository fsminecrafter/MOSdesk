# mosWM

`mosWM` is the user-space window manager for Minimal-OS. It owns the display
and composites windows created by applications through the SDK's `window.h`
API.

## Build and run

Initialize the repository submodules and install the SDK toolchain as described
in the [repository README](../README.md). Build from the project root:

```sh
./mosWM/build.sh
```

The build creates `mosWM/dist/mosWM.run` and the installable
`mosWM/dist/mosWM.mpkg`. Installing the package places the executable at
`0:/programs/mosWM/mosWM.run` and the optional service definition at
`0:/services/mosWM.service`. Minimal-OS's service manager starts it after the
boot disk is mounted. Remove that service file and reboot to disable automatic
startup. Without the service file, the normal Minimal-OS terminal boots and
remains available.

To start the window manager manually instead, run this from the terminal:

```text
run 0:/programs/mosWM/mosWM.run
```

MOSdesk is hosted by mosWM. The installed mosWM service owns the framebuffer,
draws the desktop and taskbar, and provides MOSdesk Settings from the start
menu. Do not launch the legacy standalone framebuffer build alongside mosWM.

The Posy cursor pack is the default; the previous single-pointer cursor is
available as the Classic pack. MOSdesk Settings lets users choose the pack,
fill color, scale, pointer speed, one of six coordinated color themes, optional
password sign-in, single-click opening, and desktop grid snapping. Desktop
shortcut positions persist across restarts; the grid is on by default and can
be disabled for free placement. Single-click opening and password sign-in are
off by default. On first launch mosWM asks for a username, creates
`0:/users/<username>/` with `Documents`, `Pictures` and `Desktop`, and asks
whether to use password sign-in or automatic sign-in. The password prompt gates MOSdesk
only; it does not protect OS files or replace an OS account login. Posy's
animated busy and working cursors and its text, hand, help, forbidden, move,
pen, precise, and resize cursors are loaded as needed.

MOSdesk keeps the Home and System desktop shortcuts and adds shortcuts for
`.run` applications found directly in `0:/programs/` or one directory below
it (built-in `mos_<name>.run` apps are labelled "Name"), followed by the
files and folders in `0:/users/<username>/Desktop`; that folder is re-read
twice a second, so files other programs save there appear on their own.
Double-clicking an application shortcut launches its `.run` bundle by
default; text files open in the bundled editor, `0:/programs/mos_edit.run`. Bundles may include a root-level `icon.bmp` or `icon.ico`; these are
shown on the desktop.
The decoder supports uncompressed 24-bit or 32-bit BMP and DIB-based ICO
entries. Apps can set a 32×32 ARGB8888 icon for their own window with
`mos_window_set_icon`; mosWM displays it in the title bar and taskbar. See the
SDK README for the application API.

Home opens the user's `0:/users/<username>/` folder and System opens the `0:/` filesystem root
in the built-in Files window. Navigate folders with a double-click (or one
click when enabled); the toolbar jumps Home/System, goes up, refreshes, and
creates a folder. Entries are listed folders first, with an icon chosen from
the file type, and the list scrolls with the mouse wheel.

Right-click for actions:

- a file or folder in Files: Open, Set as wallpaper (QOI images), Copy,
  Rename, Delete;
- the empty part of a folder: Paste, New folder, Refresh;
- a desktop application icon: Open, Show in Files, Copy, Rename, Delete;
- the desktop itself: Open Files, Change wallpaper, Settings, Refresh desktop.

Rename and Delete ask for confirmation in a small dialog. MinimaFS has no
rename call, so a rename is a copy followed by a delete. The folders the
desktop is installed in (`0:/programs/mosWM`, `0:/programs/mosdesk`) and the
top-level system folders cannot be renamed or deleted from the shell.

Wallpapers are chosen in MOSdesk Settings > Appearance, which lists the six
built-in Minimal-OS themes and the bundled photos, with fill, crop, stretch
and rotate for photos. Double-clicking a QOI image in Files also applies it.
Photo previews are small `.thm` files in `assets/wallpapers/thumbs/`,
written by `mosWM/tools/generate_wallpapers.py` (`--thumbs-only` refreshes
just those).

## Rendering

mosWM repaints only what changed. The pointer is drawn as a small patch over
the finished scene, so moving the mouse updates two small rectangles, and an
idle desktop draws nothing at all. A window that moves, resizes or presents
new content, a hover highlight (desktop icon, panel, menu row), the clock
and the debug overlay each damage just the rectangle they cover: the scene
is redrawn clipped to those rectangles and only they are sent to the screen.
Everything else (a window opening, closing or changing focus, a new
wallpaper, a dialog) repaints the whole scene. Applications help by
presenting only when their content changes.

`mosdesk/tools/host_preview/damage_test.sh` checks this on the host: it runs
the real compositor through random changes and compares every partially
repainted frame with a full repaint of the same state. Run it after touching
anything that draws into the scene.

Settings > Debug turns on two overlays in the top-right corner, refreshed
once a second. The FPS meter shows main-loop frames per second and frame
time, how often the pointer was redrawn, how many mouse-move events arrived,
and how many partial and full scene repaints happened. The profiler adds the
average and peak time per frame spent in each stage of the loop (settings
poll, Files window, window surfaces, input, cursor and hover, desktop layer,
window draw, panel and menus, present, sleep). The loop is paced at 16 ms a
frame, so a healthy desktop reads 62.5 fps with most of each frame in Sleep.

## Window features

- Rounded, themed window chrome with configurable title-bar, text, border,
  content, button, and icon colors
- Move and resize by dragging the title bar and lower-right resize handle
- Drag to the screen sides for half-screen tiling, to the top edge to maximize,
  and to screen corners for quarter-screen tiling
- Focus, minimize, maximize, restore, and fullscreen, with an app switcher on
  the taskbar
- Content surfaces resized to match the app window; the app receives a
  `windowsizechange_recall` callback event with the new content dimensions
- Per-window input queues; keyboard input is routed to the focused window and
  pointer events to the window under the cursor

Window positions are desktop coordinates. Width and height describe the
application's content surface; decorations are managed separately by `mosWM`.
Applications render ARGB8888 pixels into their own buffer and publish updates
through the window API. The kernel copies those pixels into the corresponding
window surface; applications never receive another application's surface
pointer.

The focused window's owner may change the global pointer shape or cursor pack.
The kernel rejects cursor requests from unfocused windows and processes other
than the focused window's owner. Cursor pack paths must be under
`0:/programs/`; a pack directory contains `arrow.cur` and may provide
`busy.ani`, `working.ani`, `text.cur`, `hand.cur`, `help.cur`, `forbidden.cur`,
`move.cur`, `resize-ew.cur`, `resize-nesw.cur`, `resize-ns.cur`,
`resize-nwse.cur`, `precise.cur`, and `pen.cur`. Missing shapes fall back to
the pack's arrow cursor.

```c
mos_window_set_cursor_pack(window_id,
    "0:/programs/example/assets/cursors/Brand");
mos_window_set_cursor(window_id, SYSCALL_CURSOR_BUSY);
```

## Application API

Include `window.h` from the SDK. A window's owner can create, title, resize,
style, minimize, maximize, fullscreen, hide, show, or close its own windows.
The process that launched an application may also manage windows belonging to
its child processes; unrelated processes cannot query or modify them.

```c
#include "window.h"
#include "stdlib.h"
#include "string.h"

static uint32_t window_id;
static uint32_t* pixels;
static uint32_t surface_width = 640;
static uint32_t surface_height = 360;

static void resized(uint32_t id, uint32_t width, uint32_t height) {
    (void)id;
    uint32_t* resized_pixels =
        realloc(pixels, (size_t)width * height * sizeof(uint32_t));
    if (!resized_pixels) {
        mos_window_close(window_id);
        mos_exit(1);
    }
    pixels = resized_pixels;
    surface_width = width;
    surface_height = height;
    memset(pixels, 0, (size_t)width * height * sizeof(uint32_t));
}

static void close_requested(uint32_t id) {
    mos_window_close(id);
    free(pixels);
    mos_exit(0);
}

int main(void) {
    window_id = mos_window_create(0, 80, 64, surface_width, surface_height,
                                  "Example");
    if (!window_id) return 1;
    pixels = calloc((size_t)surface_width * surface_height, sizeof(uint32_t));
    if (!pixels) {
        mos_window_close(window_id);
        return 1;
    }

    mos_window_callbacks_t callbacks = {
        .onwindowclose = close_requested,
        .windowsizechange_recall = resized
    };

    for (;;) {
        mos_window_present(window_id, pixels,
                           surface_width * sizeof(uint32_t),
                           0, 0, surface_width, surface_height);
        mos_window_dispatch_events(window_id, &callbacks);
        mos_sleep(16);
    }
}
```

The event dispatcher invokes `onwindowclose`, `onwindowminimize`,
`onwindowmaximise`, `windowsizechange_recall`, and `onwindowfocus` callbacks
when the app polls its event queue. The app should handle a close request and
call `mos_window_close()` if it accepts it. A window may be made non-resizable
with `mos_window_set_resizable(id, false)`.

The window-server role is granted only when the installed `mosWM.run` is
launched by the OS terminal. An application that launches a copy as a child
does not gain server privileges. The server must also own the framebuffer
before it can register.
