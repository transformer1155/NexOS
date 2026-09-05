# =====================================================================
#  MiniOS Win11 Desktop Simulator - CMake Build
#  A Windows 11 style desktop environment simulator (C++17 + SDL2)
#
#  Usage:
#    Linux/WSL:  sudo apt install libsdl2-dev cmake
#                cmake -B build && cmake --build build -j
#    Windows:    (MSYS2/MinGW) pacman -S mingw-w64-x86_64-SDL2 mingw-w64-x86_64-cmake
#                cmake -B build && cmake --build build -j
# =====================================================================
cmake_minimum_required(VERSION 3.16)
project(win11desktop CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- SDL2: prefer system package, else FetchContent from GitHub ---
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
  message(STATUS "SDL2 not found - fetching from source (first build takes a while)")
  include(FetchContent)
  FetchContent_Declare(SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.5)
  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL2)
endif()

file(GLOB SRC_FILES src/*.cpp)
add_executable(win11desktop ${SRC_FILES})

target_include_directories(win11desktop PRIVATE src)
if(TARGET SDL2::SDL2)
  target_link_libraries(win11desktop PRIVATE SDL2::SDL2)
else()
  target_link_libraries(win11desktop PRIVATE SDL2)
endif()

if(MSVC)
  target_compile_options(win11desktop PRIVATE /W3)
else()
  target_compile_options(win11desktop PRIVATE -Wall -Wextra)
endif()

# --selftest mode renders frames and saves PNG screenshots (headless-safe)
option(WIN11_SELFTEST "Build self-test screenshot mode" ON)

message(STATUS "SDL2 include: ${SDL2_INCLUDE_DIRS}")
message(STATUS "Build dir:    ${CMAKE_BINARY_DIR}")
