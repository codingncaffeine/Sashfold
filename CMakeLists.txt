cmake_minimum_required(VERSION 3.24)
project(sashfold LANGUAGES CXX VERSION 0.1.0)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings are errors from commit one. A warning left to fester is a defect
# that has simply not been triggered yet.
add_library(sashfold_warnings INTERFACE)
if(MSVC)
  target_compile_options(sashfold_warnings INTERFACE /W4 /WX /permissive-)
else()
  target_compile_options(sashfold_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Werror
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wdouble-promotion)
endif()

add_library(sashfold_core STATIC
  src/core/Bitmap.cpp
  src/core/Json.cpp
  src/core/Png.cpp
  src/core/Unicode.cpp
  src/css/Parser.cpp
  src/css/Selector.cpp
  src/css/StyleResolver.cpp
  src/css/Tokenizer.cpp
  src/dom/Dom.cpp
  src/layout/Layout.cpp
  src/net/Url.cpp
  src/paint/Painter.cpp
  src/text/SashfoldMono.cpp
  src/html/Encoding.cpp
  src/html/InputStream.cpp
  src/html/Tokenizer.cpp
  src/html/TreeBuilder.cpp
  src/html/TreeDump.cpp
)
target_include_directories(sashfold_core PUBLIC src)
target_link_libraries(sashfold_core PRIVATE sashfold_warnings)

add_executable(sashfold src/main.cpp)
target_link_libraries(sashfold PRIVATE sashfold_core sashfold_warnings)
if(MINGW)
  # Ship a binary that imports only OS libraries: the language runtime is
  # linked statically, so tools/pledge-check's allowlist stays kernel32+msvcrt.
  target_link_options(sashfold PRIVATE -static)
endif()

enable_testing()
add_subdirectory(tests)

# Fuzzing lane: every parser gets a harness (see fuzz/README.md). Only built
# on request, and only clang carries libFuzzer.
option(SASHFOLD_FUZZ "Build libFuzzer harnesses (requires clang)" OFF)
if(SASHFOLD_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SASHFOLD_FUZZ requires a clang toolchain")
  endif()
  function(sashfold_fuzzer stem)
    add_executable(fuzz_${stem} fuzz/${stem}.cpp)
    target_link_libraries(fuzz_${stem} PRIVATE sashfold_core)
    target_compile_options(fuzz_${stem} PRIVATE -fsanitize=fuzzer,address,undefined -g -O1)
    target_link_options(fuzz_${stem} PRIVATE -fsanitize=fuzzer,address,undefined)
  endfunction()
  sashfold_fuzzer(html_tokenizer)
  sashfold_fuzzer(html_tree)
  sashfold_fuzzer(css_tokenizer)
  sashfold_fuzzer(css_parser)
  sashfold_fuzzer(url)
endif()
