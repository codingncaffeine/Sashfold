# Fuzzing

House rule: **every parser lands with its libFuzzer harness the same week it
is born**, with its corpus committed beside it. A fuzzer crash is a
stop-the-line P0.

M0 contains no parser of untrusted input (the PNG *encoder* produces bytes,
it does not consume them), so this directory currently holds only the rule
and the layout. The first harness — `fuzz/html_tokenizer/` — arrives with the
M1 HTML tokenizer, together with the CMake fuzz lane (clang
`-fsanitize=fuzzer,address`). No dead build options before there is something
to fuzz.

Planned harnesses, one per parser as each lands: html_tokenizer, html_tree,
css_tokenizer, url, inflate, png, gif, jpeg, ttf, http_headers, xkb,
x509_der, js_lexer.
