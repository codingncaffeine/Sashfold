# Fuzzing

House rule: **every parser lands with its libFuzzer harness the same week it
is born**, with its corpus committed beside it. A fuzzer crash is a
stop-the-line P0.

Build with `-DSASHFOLD_FUZZ=ON` on a clang toolchain, then e.g.:

    ./build/fuzz_html_tokenizer -timeout=5 fuzz/corpus/html_tokenizer

Harnesses so far:

- `html_tokenizer.cpp` — the HTML tokenizer (seed corpus in
  `corpus/html_tokenizer/`); a short smoke run also executes in CI.

Planned, one per parser as each lands: html_tree, css_tokenizer, url,
inflate, png, gif, jpeg, ttf, http_headers, xkb, x509_der, js_lexer.
