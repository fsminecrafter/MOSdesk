# Mxplorer

Mxplorer is a native browser for Minimal-OS. It is intentionally not built on
EasyUI: the browser shell, HTTP transport, HTML parser, CSS parser, and page
renderer are separate modules.

## Current support

- Multiple closable, reorderable tabs; back, forward, refresh, home, and URL
  search controls; the HTML-rendered `mos://home` page; and the
  `mos://settings` page.
- `Resources/homepage.html` is the built-in HTML landing page. General settings
  let you choose Google, the Mxplorer alternative, or a custom web address;
  that choice is saved in `0:/data/mxplorer-homepage.txt`.
- `Resources/fonts/` bundles Inter Regular and Medium for anti-aliased browser
  chrome and page text. Install the `Resources` directory at
  `0:/programs/Mxplorer/Resources` alongside the browser so Mxplorer can load
  the bundled homepage and fonts. If the font files are unavailable, the
  renderer falls back to the system font and then its bitmap font.
- A toggleable developer panel (`Ctrl+Q`) with session console messages,
  request status, and a lightweight HTML source element outline. The startup
  page and Home button follow the selected homepage preference.
  JavaScript execution/console and a live DOM inspector are not implemented.
- HTML documents up to 96 KiB, with semantic block elements, headings,
  paragraphs, lists, line breaks, horizontal rules, links as text, and image
  alt-text placeholders. Scripts are ignored and never executed.
- Click-to-position address-bar caret, insertion/deletion, and arrow/Home/End
  navigation while editing.
- Embedded CSS with element, class, ID, and universal selectors; colors,
  backgrounds, font size/weight, text alignment, margins, padding, and
  `display: none`. The renderer wraps text, scrolls pages, and redraws only
  dirty browser chrome or page regions.
- MOSdesk accent/window-style colors.

HTML/CSS support is a deliberately small subset of the HTML Living Standard
and CSS; this is not a standards-complete browser. External stylesheets,
images, interactive links/forms, JavaScript, and advanced layout are not yet
implemented. HTTP pages can be fetched, and common HTTP redirects are followed
for up to five hops with a two-second budget per followed request. HTTPS uses
the kernel's TLS 1.3 client and is disabled by default. Certificate chains are
not authenticated, so the server identity cannot be trusted; enable HTTPS only
for non-sensitive browsing. The opt-in lasts only for the current browser
session. HTTP is unencrypted and should only be used for non-sensitive pages.

## Modules

- `main.c` - browser state, window loop, tabs, and navigation
- `http.c` - bounded HTTP/1.x retrieval
- `html.c` - bounded HTML parsing, layout, scrolling, and drawing
- `css.c` - supported CSS selectors and declarations
- `ui.c` - native browser chrome, settings, and dirty-region presentation
- `text.c` - shared smooth-text loading, width measurement, and drawing
- `Resources/homepage.html` - editable source for the Mxplorer alternative page
- `Resources/fonts/` - bundled Inter fonts and their license
- `examples/Mxplorer-Studio.c` - flattened single-file Run Studio / `.run`
  preview source. Refresh this snapshot when the modular sources change.

## Build

From the MOSdesk directory:

```sh
./Minimal-OS-SDK/build.sh --output Mxplorer.run Mxplorer
```

Initialize the `Minimal-OS-SDK` submodule first if it is not present. This
creates `Mxplorer.run` in the MOSdesk directory.

The `.run` bundle contains source-directory resources, but the current runtime
does not expose files embedded in a `.run` archive to the program. Keep the
`Resources` directory installed at the path above when deploying the app.

To build the standalone single-file version instead:

```sh
./Minimal-OS-SDK/build.sh --output /tmp/Mxplorer-Studio.run \
  Mxplorer/examples/Mxplorer-Studio.c
```

The standalone source uses the SDK window, text drawing, and networking APIs
and can be opened as one source file in Run Studio, including its bitmap-font
compatibility environment.

## Host tests

```sh
gcc -std=c11 -Wall -Wextra -Werror \
  -I Mxplorer/tests/host_stubs -I Mxplorer/include \
  Mxplorer/tests/html_test.c Mxplorer/html.c Mxplorer/css.c \
  -o /tmp/mxplorer-html-test
/tmp/mxplorer-html-test
```

JavaScript and advanced CSS/layout remain future work.
