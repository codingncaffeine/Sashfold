# Fuzzing

House rule: **every parser lands with its libFuzzer harness the same week it
is born**, with its corpus committed beside it. A fuzzer crash is a
stop-the-line P0.

Build with `-DSASHFOLD_FUZZ=ON` on a clang toolchain, then e.g.:

    ./build/fuzz_html_tokenizer -timeout=5 fuzz/corpus/html_tokenizer

Harnesses:

- `html_tokenizer.cpp` — the HTML tokenizer (seed corpus in
  `corpus/html_tokenizer/`); a short smoke run also executes in CI.
- `html_tree.cpp` — tree construction, fed the same corpus.
- `css_tokenizer.cpp`, `css_parser.cpp` — the CSS front end.
- `url.cpp` — the URL parser.
- `inflate.cpp` — zlib, gzip and raw deflate streams.
- `http_response.cpp` — HTTP/1.1 response heads and bodies.
- `truetype.cpp` — the TrueType reader: table directory, cmap, glyf,
  composites (seed: a small Sashfold Mono from `gen_font --seed`); smoke run
  in CI.
- `png.cpp` — the PNG decoder: chunks, filters, interlace, palettes (seed: a
  small render by our own encoder); smoke run in CI.
- `gif.cpp` — the GIF decoder: blocks, color tables, LZW, interlace (seed:
  the 1x1 transparent pixel); smoke run in CI.
- `jpeg.cpp` — the JPEG decoder: markers, tables, the entropy decoder, the
  IDCT (seed: a tiny baseline file written by the test encoder); smoke run
  in CI.

Still to come, one per parser as each lands: xkb, x509_der, js_lexer.
