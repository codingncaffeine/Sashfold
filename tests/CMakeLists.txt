add_library(sashfold_test_support INTERFACE)
target_include_directories(sashfold_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

function(sashfold_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE sashfold_core sashfold_warnings sashfold_test_support)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

sashfold_test(test_bitmap)
sashfold_test(test_png)
