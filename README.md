<p align="center">
  <img src="assets/banner.jpg" alt="Sashfold — open-source web browser" width="720">
</p>

# Sashfold

[![CI](https://github.com/codingncaffeine/Sashfold/actions/workflows/ci.yml/badge.svg)](https://github.com/codingncaffeine/Sashfold/actions/workflows/ci.yml)

**A web browser engine written from scratch — every byte — for Windows, Linux, and macOS.**

Sashfold is three things growing on one engine: an embeddable HTML/CSS engine, a browser built first for the readable web, and — much later — Sashfold Studio, a site builder that owns its own layout engine. It is at the very beginning of a long, public roadmap, and it measures itself on public yardsticks, never vibes.

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

## Status — honest, updated per milestone

**M1 (current): HTML in, DOM out.**
The full parsing pipeline is up: the WHATWG tokenizer at **7032/7032 (100%)** on the html5lib tokenizer suite, and tree construction — the complete insertion-mode machine, adoption agency, foster parenting, templates, fragments, SVG/MathML foreign content — at **1761/1784 (98.7%)** on the html5lib tree-construction suite, including the 2025 relaxed `<select>` parser. Both scores are enforced in CI by baselines that only ratchet upward, and both parser halves ship with libFuzzer harnesses. `sashfold --dump-dom page.html` prints the tree of any file you feed it.

From M0: build system with warnings-as-errors from commit one; an RGBA software canvas with source-over compositing; a from-scratch PNG encoder whose zlib container is written here, not linked; unit tests that validate the PNG output by re-decoding it with an independent decoder written for the test; `pledge-check`; CI for all three OSes.

Nothing renders yet. The road runs:
**M1** HTML→DOM, graded publicly on html5lib-tests → **M2** CSS cascade + block/inline layout: first real pages render to PNG, with reference tests byte-identical across all three OSes → **M3** HTTP, cookies, a window: it becomes a browser → onward through images, flexbox, our own JavaScript engine, grid, and web-platform-tests scoring. Each milestone ships as a release with its numbers.

## Building

Requires CMake ≥ 3.24, Ninja, and a C++23 compiler (gcc ≥ 13 or clang ≥ 16).

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`bash tools/buildall.sh` runs the gcc and clang lanes plus `pledge-check`.

## License

[BSD-2-Clause](LICENSE).
