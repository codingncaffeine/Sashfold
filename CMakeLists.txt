cmake_minimum_required(VERSION 3.24)
project(sashfold LANGUAGES CXX VERSION 0.0.7)

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
    -Wall -Wextra -Wpedantic -Werror -ffp-contract=off
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wdouble-promotion)
endif()

# MinGW reserves 2 MB of stack by default, against 8 MB on Linux and
# macOS; the script engine's tree-walker budgets 1 MB of it for a script,
# so the Windows binaries are given the same room as the others.
if(MINGW)
  add_link_options(-Wl,--stack,8388608)
endif()

add_library(sashfold_core STATIC
  src/bindings/Document.cpp
  src/bindings/Events.cpp
  src/bindings/HtmlElements.cpp
  src/bindings/Node.cpp
  src/bindings/Realm.cpp
  src/bindings/Style.cpp
  src/bindings/Window.cpp
  src/core/Base64.cpp
  src/core/Bidi.cpp
  src/core/Bitmap.cpp
  src/core/Gif.cpp
  src/core/Inflate.cpp
  src/core/Jpeg.cpp
  src/core/Json.cpp
  src/core/Normalize.cpp
  src/core/Png.cpp
  src/core/RoundRect.cpp
  src/core/Unicode.cpp
  src/css/CounterStyles.cpp
  src/css/Grid.cpp
  src/css/Parser.cpp
  src/css/Selector.cpp
  src/css/StyleResolver.cpp
  src/css/Stylesheets.cpp
  src/css/Tokenizer.cpp
  src/dom/Dom.cpp
  src/layout/Controls.cpp
  src/layout/GridAlgorithm.cpp
  src/layout/Layout.cpp
  src/layout/TableBorders.cpp
  src/layout/TableStructure.cpp
  src/layout/TableWidths.cpp
  src/net/Cache.cpp
  src/net/Connections.cpp
  src/net/Cookies.cpp
  src/net/DataUrl.cpp
  src/net/Http.cpp
  src/net/Idna.cpp
  src/net/Url.cpp
  src/paint/Painter.cpp
  src/platform/Clipboard.cpp
  src/text/Face.cpp
  src/text/FontManager.cpp
  src/text/Rasterizer.cpp
  src/text/SashfoldMono.cpp
  src/text/TrueType.cpp
  src/text/TrueTypeWriter.cpp
  src/ui/Browser.cpp
  src/ui/Downloads.cpp
  src/ui/Forms.cpp
  src/ui/InternalPages.cpp
  src/ui/PageImages.cpp
  src/ui/Reader.cpp
  src/ui/Script.cpp
  src/ui/ShellLoader.cpp
  src/ui/SourceSet.cpp
  src/ui/Theme.cpp
  src/html/Encoding.cpp
  src/html/InputStream.cpp
  src/html/Serializer.cpp
  src/html/Tokenizer.cpp
  src/html/TreeBuilder.cpp
  src/html/TreeDump.cpp
  src/js/Conversions.cpp
  src/js/Heap.cpp
  src/js/Interpreter.cpp
  src/js/Lexer.cpp
  src/js/Objects.cpp
  src/js/Parser.cpp
  src/js/Regex.cpp
  src/js/RuntimeArray.cpp
  src/js/RuntimeDate.cpp
  src/js/RuntimeJson.cpp
  src/js/RuntimeNumber.cpp
  src/js/RuntimeObject.cpp
  src/js/RuntimeString.cpp
  src/js/Strings.cpp
)
if(WIN32)
  target_sources(sashfold_core PRIVATE src/platform/win32/ClipboardWin32.cpp src/platform/win32/NetWin32.cpp src/platform/win32/TlsWin32.cpp
    src/platform/win32/WindowWin32.cpp src/platform/win32/FontsWin32.cpp)
  target_link_libraries(sashfold_core PUBLIC ws2_32 secur32 user32 gdi32)
else()
  target_sources(sashfold_core PRIVATE src/platform/posix/ClipboardPosix.cpp src/platform/posix/NetPosix.cpp src/platform/posix/TlsPosix.cpp
    src/platform/posix/WindowPosix.cpp src/platform/posix/FontsPosix.cpp)
endif()
target_include_directories(sashfold_core PUBLIC src)
# The one place the version lives: the user-agent string and the about page read it from here.
target_compile_definitions(sashfold_core PUBLIC SASHFOLD_VERSION="${PROJECT_VERSION}")
target_link_libraries(sashfold_core PRIVATE sashfold_warnings)

add_executable(sashfold src/main.cpp)
target_link_libraries(sashfold PRIVATE sashfold_core sashfold_warnings)
if(MINGW)
  # Ship a binary that imports only OS libraries: the language runtime is
  # linked statically, so tools/pledge-check's allowlist stays kernel32+msvcrt.
  target_link_options(sashfold PRIVATE -static)
endif()

# gen_font writes Sashfold Mono as a TrueType file: the reader fixture and the
# fuzzing seed. A build tool, never shipped.
add_executable(gen_font tools/gen-font.cpp)
target_link_libraries(gen_font PRIVATE sashfold_core sashfold_warnings)

# sashfold100 writes the Sashfold 100 dashboard from the renders and reports
# that tools/sashfold100.sh leaves beside each page. A build tool, never shipped.
add_executable(sashfold100 tools/sashfold100.cpp)
target_link_libraries(sashfold100 PRIVATE sashfold_core sashfold_warnings)

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
  sashfold_fuzzer(inflate)
  sashfold_fuzzer(http_response)
  sashfold_fuzzer(truetype)
  sashfold_fuzzer(png)
  sashfold_fuzzer(gif)
  sashfold_fuzzer(jpeg)
endif()
