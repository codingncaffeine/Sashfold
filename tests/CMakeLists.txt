add_library(sashfold_test_support INTERFACE)
target_include_directories(sashfold_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

function(sashfold_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

sashfold_test(test_bitmap)
sashfold_test(test_png)
sashfold_test(test_json)
sashfold_test(test_html_tokenizer)
sashfold_test(test_encoding)
sashfold_test(test_css_tokenizer)
sashfold_test(test_css_parser)

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
