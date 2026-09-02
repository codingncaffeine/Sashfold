<p align="center">
  <img src="assets/banner.jpg" alt="Sashfold — open-source web browser" width="720">
</p>

# Sashfold

[![CI](https://github.com/codingncaffeine/Sashfold/actions/workflows/ci.yml/badge.svg)](https://github.com/codingncaffeine/Sashfold/actions/workflows/ci.yml)

**A web browser engine written from scratch — every byte — for Windows, Linux, and macOS.**

Sashfold is an embeddable HTML/CSS engine and a browser built first for the readable web. It is early, it is public, and it measures itself on public yardsticks, never vibes.

*(A sash is the frame that holds a window's panes of glass; the fold is where a page's content begins. That is roughly the whole job description.)*

## The pledge

**Every byte of code that ships in a Sashfold binary is written in this repository.**

The boundary, drawn exactly:

| We call it platform | Because |
|---|---|
| OS system interfaces — Win32, AppKit (driven through the Objective-C runtime), the Linux kernel and display-server **wire protocols** | On each OS, that interface *is* the machine. On Linux we link no GTK, Qt, SDL, libwayland, or X libraries — Wayland is a protocol over a socket, and we speak it directly, the same way we speak HTTP |
| The language runtime — libc, libstdc++ | Every program on the OS links it; below it is kernel ABI |
| OS-shipped TLS on Windows/macOS (SChannel, Network.framework) | Ships with, and is patched by, the OS. Linux has no OS TLS — there, the TLS client is ours |

Everything above that line is written here: HTML and CSS parsing, layout, text — font parsing *and* rasterization — every image and compression codec, HTTP and cookies, the JavaScript engine, storage, devtools, the shells. Build tools (compiler, CMake, Ninja) never ship and are exempt. Data is not code: Unicode tables, conformance-test fixtures, your installed fonts, and your root-certificate store are inputs we parse with our own code — parsing the world's data is the job description.

**Enforced, not promised:** [`tools/pledge-check.sh`](tools/pledge-check.sh) reads the built binary's import table and fails the build if anything outside the per-OS allowlist appears. Zero dependencies is also a security posture: there is no supply chain.

## What Sashfold will never do

No telemetry. No sponsored tiles. No default-search auction. No account requirement. No cloud AI. No feature-removal churn. Every setting lives in a human-readable file, and every feature can be turned off.

## What works today

**Browsing.** Sashfold opens a window on Windows and browses the live web over HTTPS, every layer written here: the WHATWG URL parser at **893/893 (100%)** on WPT's `urltestdata`, an HTTP/1.1 client that keeps its connections open across a page's requests, inflate for gzip and deflate, TLS through the operating system's SChannel with real certificate validation (the badssl matrix is refused, and a certificate that fails lands on a page with no way through — on purpose), a cookie jar that **blocks third-party cookies by default**, a session cache that honors only explicit freshness, HTTPS-first for typed addresses, `strict-origin-when-cross-origin` referrers, and downloads that are saved with the mark of the web and never opened. Forms work without scripts: text fields, checkboxes, radio buttons, selects and buttons are drawn, take focus (a click, Tab, a label), typing and the arrow keys, and a GET form submits to its action with its data set urlencoded as the query (a form that posts says so instead). Text can be selected with the mouse or with Shift and the arrow keys and copied with Ctrl+C through the operating system's clipboard (a clipboard of the process's own stands in for scripted runs, and on the OSes without a backend yet); Ctrl+V pastes into a field or the address bar. Ctrl+F opens a find bar that counts the query's matches on the page as you type, highlights them all with the current one stronger, walks them with Enter and Shift+Enter (scrolling each into view), and closes with Escape. Reader mode (the pilcrow button at the right of the address bar) shows a page's article alone — the container of its paragraphs, found by scoring text, commas, link density and the names of classes and ids — with the site's heading, the source, absolute links and pictures, and none of the navigation, sidebars or comments. Pressing f with nothing focused puts a home-row label on every link in view; typing the label follows the link, Escape puts them away. F12 (or Ctrl+Shift+I) opens a devtools panel under the page: the document's tree on the left, and for the element under inspection — a click on the page or on a line of the tree — its box and computed style on the right, with the box overlaid on the page. The shell — tabs, history that keeps each page's bytes so Back never refetches, an address bar, link hit-testing, scrolling — is one OS-free component painted through **ThemeTokens**: every color and size of the chrome comes from `themes/default.json`, which reloads while the window is open. A `--script` mode drives that same shell from a text file; it is how the shell is tested in CI on all three OSes, with chrome screenshots compared byte-for-byte.

**Rendering.** Real pages render to pixels, end to end, all of it in this repository: the CSS tokenizer and parser (css-syntax-3, current draft, nesting-era block contents), selectors with specificity and an+b structural pseudo-classes, the cascade (UA stylesheet, `<style>` sheets, style attributes, importance/origin/specificity/order, inheritance, em/rem units), block and inline layout (margin collapsing, line boxes with greedy breaking, whitespace collapsing, `pre`, lists with markers, auto-margin centering, percentage widths; floats left and right with clearance, shrink-to-fit widths, and boxes that contain their floats; flexbox rows and columns that wrap, grow and shrink, with gaps, justify-content and align-items), and painting — backgrounds, solid borders, text decorations, and text in **your installed fonts**: a TrueType reader written here (TTF and TTC, composite glyphs, four cmap formats, every offset bounds-checked and fuzzed), a rasterizer written here (outlines flattened in fixed point, nonzero winding, 4×4 grayscale coverage, integer arithmetic only), and a font manager that catalogues the operating system's font directories by their naming tables alone, matches CSS `font-family`, weight and slant, and falls back by coverage when a page's font lacks a character. The last fallback is **Sashfold Mono**, an original monospace face authored as stroke tables in this repo, which draws an honest box for anything no font on the machine has. The reference tests render with that built-in face alone, no font files and no libm in the raster path, so **renders are byte-identical across compilers and operating systems** — enforced by a reference-test suite whose PNG goldens (compressed by our own fixed-Huffman deflate encoder, verified against an independent inflate written for the test) are compared byte-for-byte in CI; the outline rasterizer reproduces the built-in face's pixels exactly, and a test holds it to that. CFF-flavored OpenType is recognized and declined for now. External stylesheets load through the same session as the page (`<link rel="stylesheet">`, `@import`, the CSS charset rules, media queries evaluated against the real viewport), and images render — PNG (every color type, bit depth and filter, the interlaced layout), GIF (LZW, interlace, transparency; the first frame), and JPEG, baseline and progressive (an integer inverse DCT, chroma subsampling, restart markers), each decoder written here and fuzzed — chosen for the viewport through `srcset`, `sizes` and `<picture>` (a source whose type this engine cannot decode is passed over for the fallback), sized by CSS, attributes or themselves, drawn scaled with integer arithmetic. Acid1 runs and fails exactly where float layout should be, because floats are not written yet. `sashfold --render page.html -o out.png` is the whole pipeline in one flag.

**Parsing.** The WHATWG tokenizer at **7032/7032 (100%)** on the html5lib tokenizer suite, and tree construction — the complete insertion-mode machine, adoption agency, foster parenting, templates, fragments, SVG/MathML foreign content, the customizable-`<select>` parser with `<selectedcontent>` cloning — at **1784/1784 (100%)** on the html5lib tree-construction suite. Character encoding is sniffed the way the spec says: BOM first, then the `<meta>` prescan, then the windows-1252 fallback (UTF-8, UTF-16LE/BE, and windows-1252 decoders, all written here). Both scores are enforced in CI by baselines that only ratchet upward, and both parser halves ship with libFuzzer harnesses. `sashfold --dump-dom page.html` prints the tree of any file you feed it.

**Underneath.** A build with warnings-as-errors from commit one; an RGBA software canvas with source-over compositing; a from-scratch PNG encoder whose zlib container is written here, not linked; unit tests that validate the PNG output by re-decoding it with an independent decoder written for the test; `pledge-check`; CI for all three OSes.

Progressive JPEG, animated GIF, floats, flexbox, tables, and scripting are not written yet — and nothing stands in for them: what Sashfold cannot do, it does not do.

## The user agent string

Sashfold sends `Mozilla/5.0 (<platform>) Sashfold/<version>` — for example `Mozilla/5.0 (Windows NT 10.0; Win64; x64) Sashfold/0.0.3`. The `Mozilla/5.0` prefix and the platform token are the compatibility-shaped form every engine ships because servers still key on them; the honest part is the `Sashfold/<version>` suffix, the only thing that identifies the browser, and the version is the release's own. The string carries no build number, locale, or hardware detail, and it is the same for every user of a given platform.

## Building

Requires CMake ≥ 3.24, Ninja, and a C++23 compiler (gcc ≥ 13 or clang ≥ 16).

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`bash tools/buildall.sh` runs the gcc and clang lanes plus `pledge-check`.

## Running

```
sashfold                              # the browser window (Windows for now)
sashfold https://example.org/         # ...opened on a page
sashfold --script tests/shell/live.script   # drive the shell from a text file
sashfold --render page.html -o out.png      # render a page headlessly
sashfold --bench page.html                  # time parse, style, layout, paint
```

Every color and size of the window comes from `themes/default.json`; edit it while the window is open and the chrome follows.

## License

[BSD-2-Clause](LICENSE).
