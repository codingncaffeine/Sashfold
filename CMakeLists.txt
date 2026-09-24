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
    -Wall -Wextra -Wpedantic -Werror -ffp-contract=off
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wdouble-promotion)
endif()

# The first thread's stack: a page's recursion is measured against the
# stack it runs on (platform::js_stack_budget_for), and V8 lets a page
# twelve thousand calls deep; at the engine's ~2.8 KB a call that is more
# than the 8 MB a Linux process starts with, so Linux raises its own limit
# at start (platform::widen_main_thread_stack) and the two whose first
# thread is sized at link time are given the same 256 MB here — a reserve,
# never touched past what a script uses. On Windows the figure is also the
# reserve every later thread gets (platform/ScriptThread.h).
if(MINGW)
  add_link_options(-Wl,--stack,268435456)
elseif(APPLE)
  add_link_options(-Wl,-stack_size,0x10000000)
endif()

# The fuzzing lane (see fuzz/README.md): only on request, and only clang
# carries libFuzzer. Every object in the build gets the coverage hooks and
# both sanitizers, so a harness sees and checks the code it drives rather
# than its own file alone; the programs that are not harnesses link the
# same runtimes without libFuzzer's main.
option(SASHFOLD_FUZZ "Build libFuzzer harnesses (requires clang)" OFF)
option(SASHFOLD_PORTABLE "Link the C++ runtime into the sashfold binary, for a release that runs on older distributions" OFF)
if(SASHFOLD_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SASHFOLD_FUZZ requires a clang toolchain")
  endif()
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all -g -O1)
  add_link_options(-fsanitize=fuzzer-no-link,address,undefined)
endif()

add_library(sashfold_core STATIC
  src/bindings/Binary.cpp
  src/bindings/CustomElements.cpp
  src/bindings/Document.cpp
  src/bindings/Events.cpp
  src/bindings/Fetch.cpp
  src/bindings/Fullscreen.cpp
  src/bindings/HtmlElements.cpp
  src/bindings/IndexedDb.cpp
  src/bindings/Intersection.cpp
  src/bindings/LayoutOracle.cpp
  src/bindings/Media.cpp
  src/bindings/Mutations.cpp
  src/bindings/Node.cpp
  src/bindings/Origin.cpp
  src/bindings/Range.cpp
  src/bindings/Reflect.cpp
  src/bindings/Traversal.cpp
  src/bindings/Realm.cpp
  src/bindings/Style.cpp
  src/bindings/StructuredClone.cpp
  src/bindings/Tasks.cpp
  src/bindings/Window.cpp
  src/bindings/WindowProxy.cpp
  src/bindings/Workers.cpp
  src/bindings/Xhr.cpp
  src/bindings/generated/HtmlElements.gen.cpp
  src/core/Base64.cpp
  src/core/Bidi.cpp
  src/core/AnimatedImage.cpp
  src/core/Bitmap.cpp
  src/core/LineBreak.cpp
  src/core/Bmp.cpp
  src/core/Gif.cpp
  src/core/Brotli.cpp
  src/core/Inflate.cpp
  src/core/Jpeg.cpp
  src/core/Json.cpp
  src/core/Normalize.cpp
  src/core/Png.cpp
  src/core/RoundRect.cpp
  src/core/Unicode.cpp
  src/core/Zip.cpp
  src/crypto/Aes.cpp
  src/crypto/AesGcm.cpp
  src/crypto/BigInt.cpp
  src/crypto/ChaCha20Poly1305.cpp
  src/crypto/Ec.cpp
  src/crypto/Rsa.cpp
  src/crypto/Sha2.cpp
  src/crypto/X25519.cpp
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
  src/media/Celt.cpp
  src/media/Opus.cpp
  src/media/RangeDecoder.cpp
  src/media/StreamBuffer.cpp
  src/media/VideoPipeline.cpp
  src/media/Vp9.cpp
  src/media/Wav.cpp
  src/media/WebM.cpp
  src/media/Yuv.cpp
  src/net/Cache.cpp
  src/net/Connections.cpp
  src/net/Cookies.cpp
  src/net/Csp.cpp
  src/net/DataUrl.cpp
  src/net/FetchPool.cpp
  src/net/Filters.cpp
  src/net/Http.cpp
  src/net/Idna.cpp
  src/net/Url.cpp
  src/net/tls/Der.cpp
  src/net/tls/Tls13.cpp
  src/net/tls/TrustStore.cpp
  src/net/tls/Validate.cpp
  src/net/tls/X509.cpp
  src/paint/Painter.cpp
  src/platform/Clipboard.cpp
  src/platform/AudioVirtual.cpp
  src/platform/PulseProtocol.cpp
  src/platform/MemoryWatch.cpp
  src/platform/Tls.cpp
  src/platform/Touch.cpp
  src/platform/Window.cpp
  src/storage/IndexedDb.cpp
  src/svg/Raster.cpp
  src/svg/Svg.cpp
  src/text/Cff.cpp
  src/text/Face.cpp
  src/text/FontManager.cpp
  src/text/Rasterizer.cpp
  src/text/SashfoldMono.cpp
  src/text/TrueType.cpp
  src/text/TrueTypeWriter.cpp
  src/text/Woff.cpp
  src/ui/Bookmarks.cpp
  src/ui/BookmarkSources.cpp
  src/ui/BookmarksPage.cpp
  src/ui/Browser.cpp
  src/ui/Cosmetic.cpp
  src/ui/Downloads.cpp
  src/ui/Forms.cpp
  src/ui/Icons.cpp
  src/ui/InternalPages.cpp
  src/ui/Frames.cpp
  src/ui/PageImages.cpp
  src/ui/Reader.cpp
  src/ui/Script.cpp
  src/ui/ShellLoader.cpp
  src/ui/SourceSet.cpp
  src/ui/Theme.cpp
  src/ui/ThemeGallery.cpp
  src/ui/ThemeImport.cpp
  src/html/DocumentBase.cpp
  src/html/Encoding.cpp
  src/html/InputStream.cpp
  src/html/PreloadScanner.cpp
  src/html/Serializer.cpp
  src/html/Tokenizer.cpp
  src/html/TreeBuilder.cpp
  src/html/TreeDump.cpp
  src/js/BigInteger.cpp
  src/js/Compiler.cpp
  src/js/Conversions.cpp
  src/js/Heap.cpp
  src/js/Interpreter.cpp
  src/js/Lexer.cpp
  src/js/Module.cpp
  src/js/Objects.cpp
  src/js/Parser.cpp
  src/js/Regex.cpp
  src/js/RuntimeArray.cpp
  src/js/RuntimeArrayBuffer.cpp
  src/js/RuntimeBigInt.cpp
  src/js/RuntimeCollections.cpp
  src/js/RuntimeDate.cpp
  src/js/RuntimeGenerator.cpp
  src/js/RuntimeIterator.cpp
  src/js/RuntimeJson.cpp
  src/js/RuntimeNumber.cpp
  src/js/RuntimeObject.cpp
  src/js/RuntimePromise.cpp
  src/js/RuntimeProxy.cpp
  src/js/RuntimeString.cpp
  src/js/RuntimeTypedArray.cpp
  src/js/Strings.cpp
  src/js/Vm.cpp
)
if(WIN32)
  target_sources(sashfold_core PRIVATE src/platform/win32/ClipboardWin32.cpp src/platform/win32/RandomWin32.cpp src/platform/win32/NetWin32.cpp src/platform/win32/TlsWin32.cpp src/platform/win32/MemoryWin32.cpp
    src/platform/win32/WindowWin32.cpp src/platform/win32/FontsWin32.cpp src/platform/AudioNone.cpp src/media/Vp9AcceleratorNone.cpp)
  target_link_libraries(sashfold_core PUBLIC ws2_32 secur32 user32 gdi32 bcrypt)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # The Wayland window speaks the wire protocol over the compositor's socket
  # itself, so the import table stays libc and the language runtime.
  target_sources(sashfold_core PRIVATE src/platform/posix/RandomPosix.cpp src/platform/posix/NetPosix.cpp src/platform/posix/TlsPosix.cpp src/platform/posix/MemoryPosix.cpp
    src/platform/posix/FontsPosix.cpp src/platform/linux/Wayland.cpp src/platform/linux/Xkb.cpp
    src/platform/linux/WindowWayland.cpp src/platform/linux/ClipboardWayland.cpp src/platform/linux/AudioPulse.cpp
    src/media/linux/Vp9Accelerators.cpp src/media/linux/Vp9Vulkan.cpp src/media/linux/Vp9Vaapi.cpp)
else()
  target_sources(sashfold_core PRIVATE src/platform/posix/ClipboardPosix.cpp src/platform/posix/RandomPosix.cpp src/platform/posix/NetPosix.cpp src/platform/posix/TlsPosix.cpp src/platform/posix/MemoryPosix.cpp
    src/platform/posix/WindowPosix.cpp src/platform/posix/FontsPosix.cpp src/platform/AudioNone.cpp src/media/Vp9AcceleratorNone.cpp)
endif()
if(NOT WIN32)
  # The memory watch's thread (platform/MemoryWatch): the one thread the
  # engine has beside its own. On Windows it is kernel32's CreateThread.
  find_package(Threads REQUIRED)
  target_link_libraries(sashfold_core PUBLIC Threads::Threads)
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
elseif(SASHFOLD_PORTABLE)
  # A Linux release binary: the C++ runtime linked in, so the tarball runs on
  # a distribution with an older libstdc++ than the one it was built against;
  # libc stays the system's, so the floor is the glibc of the build machine.
  target_link_options(sashfold PRIVATE -static-libstdc++ -static-libgcc)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # The binary's own functions by name in its dynamic symbol table, so that
  # the stack the memory watch writes on its way out reads as functions in
  # the journal and not as offsets. Exports only; the imports the pledge
  # counts are what they were.
  set_target_properties(sashfold PROPERTIES ENABLE_EXPORTS ON)
endif()

# gen_font writes Sashfold Mono as a TrueType file: the reader fixture and the
# fuzzing seed. A build tool, never shipped.
add_executable(gen_font tools/gen-font.cpp)
target_link_libraries(gen_font PRIVATE sashfold_core sashfold_warnings)

# Sashfold Test Sans: a proportional face of plain boxes that gen_font makes
# at build time, for the tests of what is set in a proportional face — the
# same bytes on every machine, and no font file of anyone else's in the tree.
set(SASHFOLD_TEST_SANS ${CMAKE_BINARY_DIR}/SashfoldTestSans.ttf)
add_custom_command(OUTPUT ${SASHFOLD_TEST_SANS}
  COMMAND gen_font ${SASHFOLD_TEST_SANS} --test-sans
  DEPENDS gen_font
  COMMENT "Writing Sashfold Test Sans")
add_custom_target(sashfold_test_sans ALL DEPENDS ${SASHFOLD_TEST_SANS})

# sashfold100 writes the Sashfold 100 dashboard from the renders and reports
# that tools/sashfold100.sh leaves beside each page. A build tool, never shipped.
add_executable(sashfold100 tools/sashfold100.cpp)
target_link_libraries(sashfold100 PRIVATE sashfold_core sashfold_warnings)

# js_probe runs a script on the engine and shows the tree, the completion and
# the bytecode of every body the bytecode tier compiled (tools/js-probe.cpp).
# A build tool for the script engine's own work, never shipped.
add_executable(js_probe tools/js-probe.cpp)
target_link_libraries(js_probe PRIVATE sashfold_core sashfold_warnings)

# png_diff says where two pictures differ and whether that is only inside
# the boxes given (tools/png-diff.cpp): what a change to the chrome shows
# before its goldens are blessed. A build tool, never shipped.
add_executable(png_diff tools/png-diff.cpp)
target_link_libraries(png_diff PRIVATE sashfold_core sashfold_warnings)

# png_sheet lays pictures out on one labelled sheet (tools/png-sheet.cpp);
# tools/theme-sheet.sh feeds it the chrome under each of many themes, for
# judging a look by eye. A build tool, never shipped.
add_executable(png_sheet tools/png-sheet.cpp)
target_link_libraries(png_sheet PRIVATE sashfold_core sashfold_warnings)

# gen_bindings turns idl/*.idl into src/bindings/generated/*.gen.cpp: the
# interfaces, their tags and their reflected attributes (tools/gen-bindings.cpp).
# The output is committed; the bindings-regen target rewrites it and a test
# fails when it is stale. A build tool, never shipped.
add_executable(gen_bindings tools/gen-bindings.cpp)
target_link_libraries(gen_bindings PRIVATE sashfold_warnings)
add_custom_target(bindings-regen
  COMMAND gen_bindings ${CMAKE_SOURCE_DIR}/idl/html-elements.idl
    --output ${CMAKE_SOURCE_DIR}/src/bindings/generated/HtmlElements.gen.cpp
  DEPENDS gen_bindings
  COMMENT "Regenerating the bindings from idl/")

enable_testing()
add_subdirectory(tests)

# Every parser gets a harness (see fuzz/README.md), each libFuzzer's main
# over the instrumented build above.
if(SASHFOLD_FUZZ)
  function(sashfold_fuzzer stem)
    add_executable(fuzz_${stem} fuzz/${stem}.cpp fuzz/Options.cpp)
    target_link_libraries(fuzz_${stem} PRIVATE sashfold_core)
    target_compile_options(fuzz_${stem} PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_${stem} PRIVATE -fsanitize=fuzzer)
  endfunction()
  sashfold_fuzzer(html_tokenizer)
  sashfold_fuzzer(html_tree)
  sashfold_fuzzer(css_tokenizer)
  sashfold_fuzzer(css_parser)
  sashfold_fuzzer(url)
  sashfold_fuzzer(inflate)
  sashfold_fuzzer(brotli)
  sashfold_fuzzer(http_response)
  sashfold_fuzzer(x509)
  sashfold_fuzzer(truetype)
  sashfold_fuzzer(png)
  sashfold_fuzzer(gif)
  sashfold_fuzzer(jpeg)
  sashfold_fuzzer(bmp)
  sashfold_fuzzer(svg)
  sashfold_fuzzer(zip)
  sashfold_fuzzer(webm)
endif()
