# Fuzzing

House rule: **every parser lands with its libFuzzer harness the same week it
is born**, with its corpus committed beside it. A fuzzer crash is a
stop-the-line P0.

Build with `-DSASHFOLD_FUZZ=ON` on a clang toolchain, then e.g.:

    ./build/fuzz_html_tokenizer -timeout=5 fuzz/corpus/html_tokenizer

That build is instrumented end to end — the coverage hooks, AddressSanitizer
and UndefinedBehaviorSanitizer on every object, not only the harness — so a
harness sees and checks the code it drives. A run's `cov:` count is the
proof: a harness over a real decoder reports hundreds of edges, and one
reporting a dozen is looking at its own file. Undefined behaviour is fatal
in this build, so a signed overflow is a crash with an artifact, not a line
in the log.

`Options.cpp`, linked into every harness, turns off one AddressSanitizer
check, the allocator/deallocator match: libFuzzer's runtime on some
toolchains brings its own operator new and delete into the link, which
reach malloc and free directly and make a nothrow allocation freed through
them read as a mismatch in correct code. The sanitizer lane, with no
libFuzzer in the link, keeps that check.

Harnesses:

- `html_tokenizer.cpp` — the HTML tokenizer (seed corpus in
  `corpus/html_tokenizer/`); a short smoke run also executes in CI.
- `html_tree.cpp` — tree construction, fed the same corpus.
- `css_tokenizer.cpp`, `css_parser.cpp` — the CSS front end.
- `url.cpp` — the URL parser.
- `inflate.cpp` — zlib, gzip and raw deflate streams.
- `http_response.cpp` — HTTP/1.1 response heads and bodies.
- `x509.cpp` — DER, X.509 certificates and CRLs (seed corpus: the test
  PKI of `tests/fixtures/x509/`, as DER).
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
- `bmp.cpp` — the BMP and ICO decoders: the DIB headers, palettes, masks,
  the run-length forms, the icon directory and its AND mask (seeds: a
  24-bit and an RLE8 bitmap, a 16-pixel icon); smoke run in CI.

Still to come, one per parser as each lands: xkb, tls_records, js_lexer.
