# MOSdesk

MOSdesk is a user-space desktop environment for [Minimal-OS](https://github.com/fsminecrafter/Minimal-OS). It renders the desktop as layered ARGB8888 surfaces, tracks dirty regions for efficient redraws, and exposes reusable text-rendering libraries for other Minimal-OS programs.

The project is split into a desktop implementation, the Minimal-OS SDK toolchain, and the underlying OS sources used to build and package the system.


<video src="https://github.com/fsminecrafter/MOSdesk/raw/main/mosdesk_demo.mp4" controls muted width="100%">
  <a href="mosdesk_demo.mp4">Watch the MOSdesk demo (mosdesk_demo.mp4)</a>
</video>

https://github.com/user-attachments/assets/f69558d4-2eb4-4ade-a4f1-567f23482287

## Features

- Composites desktop layers and window surfaces with dirty-rectangle updates
- Keeps desktop shortcut positions across restarts, with optional grid snapping
- Includes six coordinated color themes and eight photo wallpapers selectable in Settings > Appearance, with fill, crop, stretch, and rotation controls
- Renders smooth, anti-aliased OpenType/TrueType text (Inter, bundled) through the reusable `ttrend` / `ttrend_outline` libraries, with the bitmap font as a fallback
- Produces SDK `.slib` bundles that can be consumed by other freestanding programs
- Packages the desktop as a Minimal-OS deliver package (`.mpkg`) for installation

## Repository layout

- `mosdesk/` — MOSdesk Settings client, reusable file-type icon library, preview, and packaging metadata
- `mosdesk/apps/` — built-in apps shipped with MOSdesk (see [Built-in apps](#built-in-apps))
- `mosWM/` — the MOSdesk desktop, compositor, cursor, and protected application window API
- `Minimal-OS-SDK/` — toolchain and build utilities for freestanding C/C++ programs and `.slib` bundles
- `Minimal-OS/` — Minimal-OS source tree used by the SDK and runtime
- `msdk` — convenience wrapper for invoking the SDK toolchain

## Prerequisites

Before building MOSdesk, initialize the SDK and OS submodules and install the required toolchain:

```sh
git submodule update --init --recursive
cd Minimal-OS-SDK
./install-toolchain.sh
./install.sh
```

Then reload your shell or source your profile so `MSDK`/`msdk` are available on `PATH`.

For more details about the SDK, see [Minimal-OS-SDK/README.md](Minimal-OS-SDK/README.md).

## Build MOSdesk

From the repository root:

```sh
./mosdesk/build.sh
```

The build creates the MOSdesk Settings window client, exported text-rendering
libraries, the built-in apps, and the required mosWM desktop shell. It
produces `mosdesk/dist/mosdesk.mpkg` and `mosWM/dist/mosWM.mpkg`.

## Built-in apps

Apps in `mosdesk/apps/` are built and delivered with MOSdesk. Each one is
installed into `0:/programs` with a `mos_` prefix:

| Source | Built as | Installed at |
| --- | --- | --- |
| `mosdesk/apps/edit.c` | `mos_edit.run` | `0:/programs/mos_edit.run` |

To add an app, drop `<name>.c` (or a `<name>/` folder with a `main.c`) into
`mosdesk/apps/` and run `./mosdesk/build.sh`. For each app the build runs the
same thing you would by hand:

```sh
./Minimal-OS-SDK/build.sh --output mos_edit.run mosdesk/apps/edit.c
# or, with the SDK on PATH:
msdk --output mos_edit.run mosdesk/apps/edit.c
```

The desktop and Start menu list them without the prefix (`mos_edit.run` shows
as "Edit"). Apps can include `../settings.h` to find the user's folders.

`mos_edit` (MOS Edit) is a text editor built on the SDK's `easyui`. Double
clicking a text file (`.txt`, `.md`, `.c`, `.h`, `.log`, `.cfg`, ...) on the
desktop or in Files opens it there, and new documents are saved to the
user's `Documents` folder by default.

## Users and folders

On first launch MOSdesk asks for a username (letters, digits, `-` and `_`,
up to 31 characters). Everything that belongs to the user goes in their home
folder, which is created together with its standard folders:

```text
0:/users/<username>/
    Documents/
    Pictures/
    Desktop/      anything in here is shown on the desktop
```

Home on the desktop, in Files and in the Start menu opens
`0:/users/<username>`. The home and its three folders cannot be renamed or
deleted from the shell, and missing folders are recreated at startup.
Desktops set up before usernames existed ask for one once, after sign-in.

## Icons

MOSdesk ships a 16-icon, 64x64 pixel-art set (folder, drive, document, PDF,
image, audio, video, code, archive, settings, network, pointer, home,
computer, appearance, mouse). The art is defined in
`mosdesk/tools/icons/icons.py` and compiled into `mosWM/file_icon_data.h`
(palette + run-length encoded, ~20 KB). `moswm_draw_file_icon_sized()` draws any
size; 64 is pixel-perfect and other sizes are area-averaged.

```sh
python3 mosdesk/tools/icons/generate_icons.py   # regenerates the header, the
                                                # SVG sprite and icons/pixel/*.png
```

## Wallpapers

The packaged QOI versions arebounded to 1600 pixels on their longest edge
for desktop rendering. To generate more, install Pillow and run:

```sh
python3 -m pip install Pillow
python3 mosWM/tools/generate_wallpapers.py
```

In MOSdesk, open Settings > Appearance (or right-click the desktop and choose
Change wallpaper) and click a theme or photo to apply it. The same page has
the accent colours and the fill, crop, stretch, and rotate controls; it
scrolls with the mouse wheel or its scrollbar. The converter also writes the
small previews that page shows.

## Cursor showcase

```sh
./mosdesk/tools/host_preview/cursors.sh [outdir]
```

Builds the real mosWM cursor loader on the host and writes
`posy_cursor_states.png` (all 14 states), `posy_cursor_in_context.png`
(real compositor frames), `posy_cursor_animated.gif` (busy / working) and
`posy_cursor_source_128.png` (the 128 px source art). Needs gcc and Pillow.

## Scripted desktop session (images + mp4/gif)

```sh
./mosdesk/tools/host_preview/anim.sh [outdir]
```

Drives the real mosWM compositor and Settings client with scripted keyboard and
mouse input and writes PNG stills plus `mosdesk_demo.mp4` and `.gif` (ffmpeg
needed for the video). The repo-root `mosdesk_demo.mp4` is this script's output.
The session covers:

1. First run: choose a username, set a password, land on the desktop
2. Start menu with the System entries and the six built-in apps
3. Files: Home, Documents and Pictures, then opening a photo as the wallpaper
4. Desktop right-click menu, Change wallpaper
5. Settings: Minimal-OS theme, photo wallpapers, photo fit, pointer fill, Debug page
6. Window drag, maximize, restore and close
7. A restart: wrong then right password at sign-in, with the settings kept

The host build lists the built-in apps but cannot run them (they need the
Minimal-OS SDK), so they appear with the generic app icon and are not launched.
The host stubs use a stand-in password hash for the same reason, and the
taskbar clock is a fixed host stub. To update the video, run the script and copy
`mosdesk_demo.mp4` from the output folder to the repo root.

## Preview MOSdesk

To view the desktop layout in a browser without building or booting Minimal-OS:

```sh
./mosdesk/preview.sh
```

Open `/mosdesk/preview.html` on port 8000 in the Codespaces Ports panel. This
browser preview recreates the desktop appearance; it does not run the
Minimal-OS executable.

## Build mosWM

`mosWM` is the desktop shell and window manager. Build its desktop package with:

```sh
./mosWM/build.sh
```

This produces `mosWM/dist/mosWM.run` and the installable
`mosWM/dist/mosWM.mpkg`. Installing the package places its executable, default
cursor, and `mosWM.service` module definition in the OS. The service starts the
desktop after the boot disk is mounted. MOSdesk Settings is opened from the
Start menu and saves desktop preferences for future sessions. Its default
pointer is the Posy cursor pack; the previous single-pointer cursor is
available as Classic. Attribution is included in the package.

On the first MOSdesk launch, mosWM creates `0:/users/` and shows a one-time
setup: it asks for a username (creating `0:/users/<username>` with
`Documents`, `Pictures` and `Desktop`), then whether to enable password
sign-in. Choosing no enables automatic
sign-in; choosing yes sets the desktop password. Password sign-in is a
MOSdesk desktop gate, not an OS-level account or filesystem security boundary.
Settings includes Appearance (accent and wallpaper), Mouse, Cursor, Login,
Desktop, and Debug pages. Debug switches on an FPS meter and a per-stage
frame profiler that mosWM draws in the top-right corner of the screen.

The standalone manager can also be started manually from the Minimal-OS
terminal:

```text
run 0:/programs/mosWM/mosWM.run
```

The OS service manager starts the registered desktop as a supervised user
process. If the program or service definition is absent, service discovery
skips it and the built-in terminal remains available. Applications can use the
SDK's [`window.h`](Minimal-OS-SDK/include/window.h)
API to create and manage their own windows. Kernel-enforced ownership prevents
unrelated applications from accessing window state, input events, or frame
buffers. See [mosWM/README.md](mosWM/README.md) for the API and behavior.

## Build artifacts

After a successful build, the following files are produced in `mosdesk/dist/`:

- `mosdesk.run` — the desktop executable
- `ttrend_raster.slib` — raster text renderer library bundle
- `ttrend_outline.slib` — OpenType/TrueType (CFF and glyf) anti-aliased text renderer bundle
- `mosdesk.mpkg` — deliver package for Minimal-OS installation; it installs
  at `0:/`, with MOSdesk in `programs/mosdesk/` and the built-in apps in
  `programs/`
- `mosdesk/` — unpacked package directory used to assemble the final package

The `.slib` bundles are distribution artifacts for Minimal-OS. The runtime does not currently load shared libraries dynamically at runtime; instead, they are used as packaged SDK deliverables and metadata for other programs.

## Smooth text

`ttrend_outline` parses `.otf`/`.ttf` fonts in memory and rasterises glyphs with
exact-area coverage in integer fixed point (no floating point, no libm, no
malloc while drawing), 1/4-pixel glyph positioning and a small coverage cache.
`ttrend_font_load_path()` now accepts outline fonts too; `ttrend_font_is_vector()`
tells which kind was loaded. mosWM and MOSdesk Settings load `Inter-Regular.otf`
and `Inter-Medium.otf` (Latin subsets, SIL OFL 1.1, see `Inter-LICENSE.txt`) and
fall back to the bitmap font if the files are missing. Not implemented: hinting,
kerning/GPOS, CID-keyed CFF and variable fonts.

Host tests (no SDK needed):

```sh
cc -I mosdesk/libraries/ttrend_outline/include \
   mosdesk/libraries/ttrend_outline/tests/outline_render_test.c \
   mosdesk/libraries/ttrend_outline/src/ttrend_outline.c -o /tmp/outline_test
/tmp/outline_test mosdesk/fonts/Inter-Regular.otf /tmp/preview.ppm
```

## Notes

MOSdesk is designed to run as a user-space component within Minimal-OS rather than as a traditional desktop environment on a host OS. The build process expects the Minimal-OS SDK and supporting OS infrastructure to be present in the repository checkout.
