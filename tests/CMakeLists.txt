add_library(sashfold_test_support INTERFACE)
target_include_directories(sashfold_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

function(sashfold_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

sashfold_test(test_bitmap)
sashfold_test(test_font)
sashfold_test(test_gif)
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
sashfold_test(test_forms)
sashfold_test(test_unicode)
sashfold_test(test_data_url)
sashfold_test(test_cookies)
sashfold_test(test_cache)
sashfold_test(test_connections)
sashfold_test(test_downloads)
sashfold_test(test_font_manager)

add_executable(test_theme test_theme.cpp)
target_link_libraries(test_theme PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
add_test(NAME test_theme COMMAND test_theme ${CMAKE_SOURCE_DIR}/themes/default.json)

# The shell, driven headlessly by --script on every OS: chrome goldens are
# byte-identical because every pixel is ours.
add_test(NAME shell_smoke
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/smoke.script
    --downloads ${CMAKE_BINARY_DIR}/test-downloads)
add_test(NAME shell_forms
  COMMAND sashfold --script ${CMAKE_CURRENT_SOURCE_DIR}/shell/forms.script
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
