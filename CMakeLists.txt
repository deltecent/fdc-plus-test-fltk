cmake_minimum_required(VERSION 3.20)

project(fdcplustest VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Link the MSVC runtime statically so the .exe has no DLL dependencies.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# FLTK, fetched and built as a static library
# ---------------------------------------------------------------------------

include(FetchContent)

set(FLTK_BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_TEST          OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLUID         OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLTK_OPTIONS  OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_GL            OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FORMS         OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_HTML_DOCS     OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_PDF_DOCS      OFF CACHE BOOL "" FORCE)
set(FLTK_MSVC_RUNTIME_DLL    OFF CACHE BOOL "" FORCE)  # match our static MSVC runtime

# Always use FLTK's bundled image libraries. System copies (e.g. Homebrew's,
# which are arm64 only) would add runtime dependencies and break universal
# macOS builds.
set(FLTK_USE_SYSTEM_LIBJPEG  OFF CACHE BOOL "" FORCE)
set(FLTK_USE_SYSTEM_LIBPNG   OFF CACHE BOOL "" FORCE)
set(FLTK_USE_SYSTEM_ZLIB     OFF CACHE BOOL "" FORCE)

FetchContent_Declare(fltk
  GIT_REPOSITORY https://github.com/fltk/fltk.git
  GIT_TAG        release-1.4.5
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(fltk)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# fdcplustest
# ---------------------------------------------------------------------------

add_executable(fdcplustest WIN32
  src/main.cpp
  src/monitor.cpp
)

# Platform code lives in separate source files chosen here, so the sources
# themselves contain no conditional compilation.
if(WIN32)
  target_sources(fdcplustest PRIVATE
    src/serial_win32.cpp
    resources/fdcplustest.rc
  )
  target_compile_definitions(fdcplustest PRIVATE _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
elseif(APPLE)
  target_sources(fdcplustest PRIVATE
    src/serial_posix.cpp
    src/serial_macos.cpp
  )
else()
  target_sources(fdcplustest PRIVATE
    src/serial_posix.cpp
    src/serial_linux.cpp
  )
endif()

target_link_libraries(fdcplustest PRIVATE fltk::fltk Threads::Threads)

target_compile_definitions(fdcplustest PRIVATE
  FDCPLUSTEST_VERSION="${PROJECT_VERSION}"
)

if(MINGW)
  target_link_options(fdcplustest PRIVATE -static)
endif()

# ---------------------------------------------------------------------------
# Tests: the monitor against a fake FDC+
# ---------------------------------------------------------------------------

enable_testing()
add_executable(monitor_test tests/monitor_test.cpp src/monitor.cpp)
target_link_libraries(monitor_test PRIVATE Threads::Threads)
add_test(NAME monitor COMMAND monitor_test)
