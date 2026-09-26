<p align="center">
  <img src="assets/banner.jpg" alt="Sashfold — open-source web browser" width="720">
</p>

# Sashfold

[![CI](https://github.com/codingncaffeine/Sashfold/actions/workflows/ci.yml/badge.svg)](https://github.com/codingncaffeine/Sashfold/actions/workflows/ci.yml)

**A web browser engine written from scratch — every byte — for Windows, Linux, and macOS.** It is early, it is public, and it measures itself on public yardsticks, never vibes.

## The pledge

Every byte of code that ships in a Sashfold binary is written in this repository. Only the operating system's interfaces, the language runtime and the OS-shipped TLS on Windows and macOS are linked, and [`tools/pledge-check.sh`](tools/pledge-check.sh) enforces that in CI.

No telemetry, no sponsored tiles, no default-search auction, no account, no cloud AI. The boundary, drawn exactly: [The Pledge](https://github.com/codingncaffeine/Sashfold/wiki/The-Pledge).

## What works today

- Browsing the live web over HTTPS — its own TLS on Linux, SChannel on Windows ([Networking](https://github.com/codingncaffeine/Sashfold/wiki/Networking)).
- The complete WHATWG HTML parser ([Parsing](https://github.com/codingncaffeine/Sashfold/wiki/Parsing)).
- CSS — the cascade, flexbox, grid, tables, `@font-face` ([Styling](https://github.com/codingncaffeine/Sashfold/wiki/Styling), [Layout](https://github.com/codingncaffeine/Sashfold/wiki/Layout)).
- Its own text rasterizer and image decoders ([Painting and text](https://github.com/codingncaffeine/Sashfold/wiki/Painting-and-text)).
- A JavaScript engine with the DOM, events, timers, modules and promises ([Scripting](https://github.com/codingncaffeine/Sashfold/wiki/Scripting)).
- A shell — tabs, history, containers, themes, devtools, reader mode ([The shell](https://github.com/codingncaffeine/Sashfold/wiki/The-shell)).

What Sashfold cannot do, it does not do: [Not written yet](https://github.com/codingncaffeine/Sashfold/wiki/Not-written-yet). The design is on the wiki too: [Architecture](https://github.com/codingncaffeine/Sashfold/wiki/Architecture), [Why C++](https://github.com/codingncaffeine/Sashfold/wiki/Why-C%2B%2B), [Security defaults](https://github.com/codingncaffeine/Sashfold/wiki/Security-defaults).

## Measured

| Yardstick | Score |
|---|---|
| html5lib tokenizer / tree construction | **7032 / 7032** and **1784 / 1784** (100%) |
| WPT URL parsing | **893 / 893** (100%) |
| Unicode bidi | **91,707 / 91,707** and **770,241 / 770,241** (100%) |
| Unicode line breaking | **16,672 / 16,672** (100%) |
| WPT CSS reference tests | **9872 / 15186 (65.0%)** — [sashfold.com/wpt.html](https://sashfold.com/wpt.html) |
| WPT testharness tests | **135632 / 159192 (85.2%)** — [sashfold.com/wpt-harness.html](https://sashfold.com/wpt-harness.html) |
| test262 | **41526 / 45148 (92.0%)** — [sashfold.com/test262.html](https://sashfold.com/test262.html) |
| The Sashfold 100 | [sashfold.com/sashfold100](https://sashfold.com/sashfold100/) · [the Linux render](https://sashfold.com/sashfold100/linux/) |

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

Downloads are on the [releases page](https://github.com/codingncaffeine/Sashfold/releases): a Windows x64 zip, a Linux x64 tarball and a Debian package, or `sashfold-bin` on the AUR.

```
sashfold                                    # the browser window (Windows and Linux)
sashfold https://example.org/               # ...opened on a page
sashfold --render page.html -o out.png      # render a page headlessly
sashfold --bench page.html                  # time parse, style, layout, paint
sashfold --script tests/shell/live.script   # drive the shell from a text file
```

Every mode and flag is on [Running Sashfold](https://github.com/codingncaffeine/Sashfold/wiki/Running-Sashfold); the window's look comes from a theme file ([Themes](https://github.com/codingncaffeine/Sashfold/wiki/Themes)).

## License

[BSD-2-Clause](LICENSE).
