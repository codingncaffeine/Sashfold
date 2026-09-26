add_library(sashfold_test_support INTERFACE)
target_include_directories(sashfold_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

function(sashfold_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

sashfold_test(test_bitmap)
sashfold_test(test_memory_watch)
sashfold_test(test_icons)
sashfold_test(test_font)
sashfold_test(test_gif)
add_executable(test_animated_image test_animated_image.cpp)
target_link_libraries(test_animated_image PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_animated_image COMMAND test_animated_image ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/animated)
sashfold_test(test_bmp)
sashfold_test(test_frames)
sashfold_test(test_filters)
sashfold_test(test_csp)
sashfold_test(test_profile)
sashfold_test(test_svg)
# The generated bindings must be what the IDL generates: regenerate with
# the bindings-regen target when this fails.
add_test(NAME bindings_generated_current
  COMMAND gen_bindings ${CMAKE_SOURCE_DIR}/idl/html-elements.idl
    --check ${CMAKE_SOURCE_DIR}/src/bindings/generated/HtmlElements.gen.cpp)
add_executable(test_jpeg test_jpeg.cpp)
target_link_libraries(test_jpeg PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_jpeg COMMAND test_jpeg)
add_executable(test_png test_png.cpp)
target_link_libraries(test_png PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_png COMMAND test_png ${CMAKE_CURRENT_SOURCE_DIR}/goldens ${CMAKE_CURRENT_SOURCE_DIR}/shell/goldens)
sashfold_test(test_json)
sashfold_test(test_html_tokenizer)
sashfold_test(test_encoding)
sashfold_test(test_css_tokenizer)
sashfold_test(test_css_parser)
sashfold_test(test_css_selectors)
sashfold_test(test_style_resolver)
sashfold_test(test_stylesheets)
sashfold_test(test_source_set)
sashfold_test(test_floats)
sashfold_test(test_flex)
sashfold_test(test_grid)
sashfold_test(test_table)
sashfold_test(test_backgrounds)
sashfold_test(test_round_rect)
sashfold_test(test_first_letter)
sashfold_test(test_line_wrap)
sashfold_test(test_forms)
sashfold_test(test_reader)
# The bidirectional algorithm. Its own conformance files are 15 MB, so
# they are downloaded rather than committed (tools/ucd-fetch.sh); with no
# checkout at ucd/ the test runs its hand-written cases alone.
add_executable(test_bidi test_bidi.cpp)
target_link_libraries(test_bidi PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
if(EXISTS ${CMAKE_SOURCE_DIR}/ucd/BidiCharacterTest.txt)
  add_test(NAME test_bidi COMMAND test_bidi
    ${CMAKE_SOURCE_DIR}/ucd/BidiCharacterTest.txt
    ${CMAKE_SOURCE_DIR}/ucd/BidiTest.txt)
else()
  add_test(NAME test_bidi COMMAND test_bidi)
endif()
# Line breaking, the same way: LineBreakTest.txt is fetched, not vendored.
add_executable(test_line_break test_line_break.cpp)
target_link_libraries(test_line_break PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
if(EXISTS ${CMAKE_SOURCE_DIR}/ucd/LineBreakTest.txt)
  add_test(NAME test_line_break COMMAND test_line_break ${CMAKE_SOURCE_DIR}/ucd/LineBreakTest.txt)
else()
  add_test(NAME test_line_break COMMAND test_line_break)
endif()
sashfold_test(test_unicode)
sashfold_test(test_data_url)
sashfold_test(test_cookies)
sashfold_test(test_cache)
sashfold_test(test_connections)
sashfold_test(test_downloads)
sashfold_test(test_font_manager)

# The script engine.
sashfold_test(test_js_lexer)
sashfold_test(test_js_regex)
sashfold_test(test_js_heap)
sashfold_test(test_js_objects)
sashfold_test(test_js_parser)
sashfold_test(test_js_scopes)
sashfold_test(test_js_interpreter)
sashfold_test(test_js_runtime)
sashfold_test(test_js_strings)
sashfold_test(test_js_array)
sashfold_test(test_js_string)
sashfold_test(test_js_number)
sashfold_test(test_js_json)
sashfold_test(test_js_date)
sashfold_test(test_js_iterator)
sashfold_test(test_js_destructuring)
sashfold_test(test_js_class)
sashfold_test(test_js_collections)
sashfold_test(test_js_promise)
sashfold_test(test_js_generator)
sashfold_test(test_js_realm)
sashfold_test(test_js_async)
sashfold_test(test_js_typed_array)
sashfold_test(test_js_module)
sashfold_test(test_js_proxy)
sashfold_test(test_js_bigint)
sashfold_test(test_js_intl)
sashfold_test(test_crypto)
sashfold_test(test_bindings)
sashfold_test(test_canvas)
sashfold_test(test_workers)
sashfold_test(test_indexeddb)

add_executable(test_theme test_theme.cpp)
target_link_libraries(test_theme PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_theme COMMAND test_theme ${CMAKE_SOURCE_DIR}/themes/default.json
  ${CMAKE_SOURCE_DIR}/themes/high-contrast.json ${CMAKE_SOURCE_DIR}/themes/light.json
  ${CMAKE_SOURCE_DIR}/themes/nord.json ${CMAKE_SOURCE_DIR}/themes/solarized-dark.json)
add_executable(test_zip test_zip.cpp)
target_link_libraries(test_zip PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_zip COMMAND test_zip ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/themes)
sashfold_test(test_audio)
add_executable(test_webm test_webm.cpp)
target_link_libraries(test_webm PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_webm COMMAND test_webm ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/media)
# The RFC 8251 test vectors, when tools/opus-vectors-fetch.sh has fetched them.
add_executable(test_opus test_opus.cpp)
target_link_libraries(test_opus PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_opus COMMAND test_opus ${CMAKE_SOURCE_DIR}/opus-vectors)
sashfold_test(test_vp9)
sashfold_test(test_video)
# They load the GPU drivers, whose own leaks lsan-suppressions.txt names
# (test_bindings plays a video).
set_tests_properties(test_vp9 test_video test_bindings PROPERTIES ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan-suppressions.txt")
add_executable(test_theme_import test_theme_import.cpp)
target_link_libraries(test_theme_import PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_theme_import COMMAND test_theme_import ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/themes
  ${CMAKE_SOURCE_DIR}/themes/light.json)
add_executable(test_theme_gallery test_theme_gallery.cpp)
target_link_libraries(test_theme_gallery PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_theme_gallery COMMAND test_theme_gallery ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/themes)
sashfold_test(test_touch)
sashfold_test(test_bookmarks)
sashfold_test(test_bookmark_sources)
sashfold_test(test_window_events)
sashfold_test(test_fetch_pool)
sashfold_test(test_preload_scanner)
sashfold_test(test_prefetch)
sashfold_test(test_navigation)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # The Wayland window's two halves that need no compositor: the wire
  # client over a socketpair, and the xkb keymap parser over keymaps
  # libxkbcommon compiled.
  sashfold_test(test_wayland)
  add_executable(test_xkb test_xkb.cpp)
  target_link_libraries(test_xkb PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
  add_test(NAME test_xkb COMMAND test_xkb ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/xkb)
endif()

# The shell, driven headlessly by --script on every OS: chrome goldens are
# byte-identical because every pixel is ours.
add_test(NAME shell_smoke
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/smoke.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_forms
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/forms.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
# A video's frames painted alone; it stops, passing, without a video decoder.
add_test(NAME shell_video
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/video.script)
set_tests_properties(shell_video PROPERTIES ENVIRONMENT
  "SASHFOLD_MEDIA=1;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan-suppressions.txt")
# A page's element full screen: refused without a click, granted at one, left at Esc.
add_test(NAME shell_fullscreen
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/fullscreen.script)
# An <img>'s load and error events in tasks of their own, a new src heard
# of again, and `complete` false while a picture is on its way.
add_test(NAME shell_image_events
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/image-events.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
# A fetched stylesheet's bytes are decoded once for the page, however
# often a script's added sheets have its stylesheets collected again.
add_test(NAME shell_sheet_cache
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/sheet-cache.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
# The resize band along a shell-drawn frame's edges shows the arrows.
add_test(NAME shell_resize_band
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/resize-band.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_selection
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/selection.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_find
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/find.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_reader
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/reader.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_hints
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/hints.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_devtools
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/devtools.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_scroll
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/scroll.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_frames
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/frames.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_windows
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/windows.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_perf
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/perf.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_pictures
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/pictures.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_scripting
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/scripting.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_favicon
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/favicon.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_newtab
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/newtab.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_newtab_pictures
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/newtab-pictures.script
    --theme ${CMAKE_CURRENT_SOURCE_DIR}/shell/newtab-theme.json
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_blocking
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/blocking.script
    --blocklists ${CMAKE_CURRENT_SOURCE_DIR}/shell/blocklists
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_csp
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/csp.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_session
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/session.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_containers
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/containers.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_indexeddb
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/indexeddb.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_palette
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/palette.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_menus
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/menus.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_tokens
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/tokens.script
    --theme ${CMAKE_CURRENT_SOURCE_DIR}/shell/tokens-theme.json
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_frame
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/frame.script
    --theme ${CMAKE_CURRENT_SOURCE_DIR}/shell/frame-theme.json
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_newtab_colors
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/newtab-colors.script
    --theme ${CMAKE_CURRENT_SOURCE_DIR}/shell/newtab-colors-theme.json
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_theme_import
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/theme-import.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_themes_page
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/themes-page.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_theme_download
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/theme-download.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_status
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/status.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_resize
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/resize.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_bookmarks
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/bookmarks.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_modal
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/modal.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_bookmarks_manager
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/bookmarks-manager.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_bookmarks_theme
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/bookmarks-theme.script
    --theme ${CMAKE_CURRENT_SOURCE_DIR}/shell/frame-theme.json
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_animated_theme
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/animated-theme.script
    --theme ${CMAKE_CURRENT_SOURCE_DIR}/shell/animated-theme.json
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_addresses
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/addresses.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_pointing
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/pointing.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_tabs
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/tabs.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
# An option only a windowless run reads, with no such run named, opens
# nothing. Were it to, the window would be on a profile of the test's own
# and would close itself at once.
add_test(NAME cli_windowless_option_needs_its_run
  COMMAND sashfold --dump-layout ${CMAKE_CURRENT_SOURCE_DIR}/shell/cookie.html
    --profile ${CMAKE_BINARY_DIR}/test-cli-profile --exit-after 1)
set_tests_properties(cli_windowless_option_needs_its_run PROPERTIES
  PASS_REGULAR_EXPRESSION "--dump-layout goes with --render, --bench or --script")
# A page's script heap under a ceiling of 40 MB, which the script's page runs into.
add_test(NAME shell_memory
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/memory.script
    --js-heap-limit 40
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
# The chrome's words in a proportional face: Sashfold Test Sans, which the
# build writes (the sashfold_test_sans target), handed over by name with the
# machine's own fonts still off.
add_test(NAME shell_typeface
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/typeface.script
    --fonts builtin --add-font ${SASHFOLD_TEST_SANS}
    --theme ${CMAKE_CURRENT_SOURCE_DIR}/shell/typeface-theme.json
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_hidpi
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/hidpi.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_ime
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/ime.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_decorations
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/decorations.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)

# html5lib conformance: score against the fixtures, hold the committed
# baseline, ratchet it upward as the tokenizer grows.
add_executable(html5lib_tokenizer html5lib_tokenizer.cpp)
target_link_libraries(html5lib_tokenizer PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME html5lib_tokenizer
  COMMAND html5lib_tokenizer
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/html5lib/tokenizer
    ${CMAKE_CURRENT_SOURCE_DIR}/html5lib_tokenizer.baseline)

add_executable(html5lib_tree html5lib_tree.cpp)
target_link_libraries(html5lib_tree PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME html5lib_tree
  COMMAND html5lib_tree
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/html5lib/tree-construction
    ${CMAKE_CURRENT_SOURCE_DIR}/html5lib_tree.baseline)

# Reference tests: rendered pages compared byte-for-byte with committed
# goldens (the renderer is deterministic across OSes and compilers).
add_executable(reftest reftest.cpp)
target_link_libraries(reftest PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME reftest
  COMMAND reftest
    ${CMAKE_CURRENT_SOURCE_DIR}/pages
    ${CMAKE_CURRENT_SOURCE_DIR}/goldens)

# WPT URL conformance, ratcheted like the html5lib suites.
add_executable(wpt_url wpt_url.cpp)
target_link_libraries(wpt_url PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME wpt_url
  COMMAND wpt_url
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/wpt/urltestdata.json
    ${CMAKE_CURRENT_SOURCE_DIR}/wpt_url.baseline)

add_executable(test_inflate test_inflate.cpp)
target_link_libraries(test_inflate PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_inflate
  COMMAND test_inflate ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/inflate)

add_executable(test_brotli test_brotli.cpp)
target_link_libraries(test_brotli PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_brotli
  COMMAND test_brotli ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/brotli)

add_executable(test_woff test_woff.cpp)
target_link_libraries(test_woff PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_woff
  COMMAND test_woff ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fonts)

add_executable(test_http test_http.cpp)
target_link_libraries(test_http PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_http
  COMMAND test_http ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/inflate)

add_executable(test_truetype test_truetype.cpp)
target_link_libraries(test_truetype PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_truetype
  COMMAND test_truetype ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fonts/SashfoldMono.ttf)

add_executable(test_rasterizer test_rasterizer.cpp)
target_link_libraries(test_rasterizer PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_rasterizer
  COMMAND test_rasterizer ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fonts/SashfoldMono.ttf)

add_executable(test_layout test_layout.cpp)
target_link_libraries(test_layout PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_layout
  COMMAND test_layout ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fonts/SashfoldMono.ttf)

# The Web Platform Tests' CSS reference tests, scored against the checkout
# tools/wpt-fetch.sh makes at wpt/ (a fresh clone has none: fetch it to run
# this one). The baseline names every passing test and only grows.
add_executable(wpt_reftest wpt_reftest.cpp)
target_link_libraries(wpt_reftest PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
if(EXISTS ${CMAKE_SOURCE_DIR}/wpt/css)
  add_test(NAME wpt_reftest
    COMMAND wpt_reftest ${CMAKE_SOURCE_DIR}/wpt
      ${CMAKE_CURRENT_SOURCE_DIR}/wpt/directories.txt
      ${CMAKE_CURRENT_SOURCE_DIR}/wpt/passing.txt
      --revision ${CMAKE_CURRENT_SOURCE_DIR}/wpt/REVISION)
  set_tests_properties(wpt_reftest PROPERTIES TIMEOUT 1800)
endif()

# The suite's testharness.js tests — the scripted ones, scored by subtest —
# from the same checkout (tools/wpt-fetch.sh adds their directories).
add_executable(wpt_testharness wpt_testharness.cpp)
target_link_libraries(wpt_testharness PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
if(EXISTS ${CMAKE_SOURCE_DIR}/wpt/dom)
  add_test(NAME wpt_testharness
    COMMAND wpt_testharness ${CMAKE_SOURCE_DIR}/wpt
      ${CMAKE_CURRENT_SOURCE_DIR}/wpt/harness-directories.txt
      ${CMAKE_CURRENT_SOURCE_DIR}/wpt/harness-passing.txt
      --revision ${CMAKE_CURRENT_SOURCE_DIR}/wpt/REVISION)
  set_tests_properties(wpt_testharness PROPERTIES TIMEOUT 1800)
endif()

# test262, the ECMAScript conformance suite, scored against the checkout
# tools/test262-fetch.sh makes at test262/ (a fresh clone has none). The
# baseline names every passing test and only grows.
add_executable(test262_runner test262_runner.cpp)
target_link_libraries(test262_runner PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
if(EXISTS ${CMAKE_SOURCE_DIR}/test262/harness)
  add_test(NAME test262
    COMMAND test262_runner ${CMAKE_SOURCE_DIR}/test262
      ${CMAKE_CURRENT_SOURCE_DIR}/test262/directories.txt
      ${CMAKE_CURRENT_SOURCE_DIR}/test262/passing.txt
      --revision ${CMAKE_CURRENT_SOURCE_DIR}/test262/REVISION)
  set_tests_properties(test262 PROPERTIES TIMEOUT 3600)
endif()
add_executable(test_x509 test_x509.cpp)
target_link_libraries(test_x509 PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_x509 COMMAND test_x509 ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/x509)
add_executable(test_validate test_validate.cpp)
target_link_libraries(test_validate PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_validate COMMAND test_validate ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/x509)

# A whole TLS 1.3 handshake against the openssl command's own server on the
# loopback interface, when the configure step finds an openssl: a development
# tool, never linked, and the test SKIPs its live half without one. The
# crafted retry-request cases in the same file run everywhere.
add_executable(test_tls_handshake test_tls_handshake.cpp)
target_link_libraries(test_tls_handshake PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
find_program(SASHFOLD_OPENSSL openssl)
if(SASHFOLD_OPENSSL)
  add_test(NAME test_tls_handshake COMMAND test_tls_handshake ${SASHFOLD_OPENSSL})
else()
  add_test(NAME test_tls_handshake COMMAND test_tls_handshake)
endif()
set_tests_properties(test_tls_handshake PROPERTIES TIMEOUT 300)
