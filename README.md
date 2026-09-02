<p align="center">
  <img src="assets/banner.jpg" alt="Sashfold — open-source web browser" width="720">
</p>

# Sashfold

[![CI](https://github.com/codingncaffeine/Sashfold/actions/workflows/ci.yml/badge.svg)](https://github.com/codingncaffeine/Sashfold/actions/workflows/ci.yml)

**A web browser engine written from scratch — every byte — for Windows, Linux, and macOS.**

Sashfold is an embeddable HTML/CSS engine and a browser built first for the readable web. It is early, it is public, and it measures itself on public yardsticks, never vibes.

*(A sash is the frame that holds a window's panes of glass; the fold is where a page's content begins. That is roughly the whole job description.)*

## The pledge

**Every byte of code that ships in a Sashfold binary is written in this repository.** The operating system's interfaces, the language runtime, and the OS-shipped TLS on Windows and macOS are the only things it links — no GTK, no Qt, no libwayland, no codec or font library, no vendored anything. [`tools/pledge-check.sh`](tools/pledge-check.sh) reads every release binary's import table in CI and fails the build if anything else appears.

The boundary, drawn exactly, is on the wiki: [The Pledge](https://github.com/codingncaffeine/Sashfold/wiki/The-Pledge).

## What Sashfold will never do

No telemetry. No sponsored tiles. No default-search auction. No account requirement. No cloud AI. No feature-removal churn. Every setting lives in a human-readable file, and every feature can be turned off.

## What works today

- **Browsing** the live web over HTTPS on Windows: a WHATWG URL parser, an HTTP/1.1 client with persistent connections, inflate, SChannel TLS with revocation, third-party cookies blocked by default, a freshness-honoring cache, HTTPS-first, downloads saved with the mark of the web and never opened.
- **Parsing** — the complete WHATWG HTML parser and encoding sniffing, at 100% on both html5lib suites.
- **Styling** — CSS syntax, selectors, the cascade with `inherit`/`initial`/`unset`, custom properties and `var()`, `calc()`, media queries, external stylesheets and `@import`, `@font-face` with TrueType sources, generated content, every named color.
- **Layout** — block and inline layout with margin collapsing, floats, flexbox, positioning with stacking contexts, inline-block, `vertical-align`, replaced boxes, percentage heights, and a block inside an inline box splitting it the way the specification says.
- **Text and pictures** in your installed fonts through a TrueType reader and rasterizer written here, with **Sashfold Mono** as the honest last fallback; PNG, GIF and JPEG decoders, `srcset` and `<picture>`.
- **A shell** — tabs, history that never refetches, forms without scripts, text selection and the clipboard, find in page, reader mode, keyboard link hints, devtools, every pixel of chrome drawn from a theme file — and a `--script` mode that drives it from a text file for CI.

Not written yet: scripting, grid, tables (cells stand in as inline-blocks), background images, WOFF fonts, windows and TLS on Linux and macOS, and more — the honest list is [Not written yet](https://github.com/codingncaffeine/Sashfold/wiki/Not-written-yet). What Sashfold cannot do, it does not do.

## Measured

| Yardstick | Score |
|---|---|
| html5lib tokenizer / tree construction | **7032 / 7032** and **1784 / 1784** (100%) |
| WPT URL parsing | **893 / 893** (100%) |
| WPT CSS reference tests, 13,477 tests over CSS2 and fifteen `css-*` directories | **5887 / 13477 (43.7%)** — the table is at [sashfold.com/wpt.html](https://sashfold.com/wpt.html) |
| The Sashfold 100 — a hundred live pages, rendered every night and published | [sashfold.com/sashfold100](https://sashfold.com/sashfold100/) |

Every score is enforced in CI: a test that stops passing fails the build. How each is scored is on [Measurements](https://github.com/codingncaffeine/Sashfold/wiki/Measurements).

## Building

Requires CMake ≥ 3.24, Ninja, and a C++23 compiler (gcc ≥ 13 or clang ≥ 16). Nothing else.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`bash tools/buildall.sh` runs the gcc and clang lanes plus the pledge and egress checks.

## Running

```
sashfold                                    # the browser window (Windows for now)
sashfold https://example.org/               # ...opened on a page
sashfold --render page.html -o out.png      # render a page headlessly
sashfold --bench page.html                  # time parse, style, layout, paint
sashfold --script tests/shell/live.script   # drive the shell from a text file
```

Every mode and flag is on [Running Sashfold](https://github.com/codingncaffeine/Sashfold/wiki/Running-Sashfold); the window's colors and sizes come from `themes/default.json` ([Themes](https://github.com/codingncaffeine/Sashfold/wiki/Themes)).

## The wiki

The details live on the [wiki](https://github.com/codingncaffeine/Sashfold/wiki): what works and how, the design decisions (the pledge, [why C++](https://github.com/codingncaffeine/Sashfold/wiki/Why-C%2B%2B), the security defaults), the tests and the tools.

## License

[BSD-2-Clause](LICENSE).
