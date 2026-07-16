cmake_minimum_required(VERSION 3.24)
project(ndsrecomp LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(SDL3 CONFIG QUIET)
if(NOT TARGET SDL3::SDL3 AND NOT TARGET SDL3::SDL3-static)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.10/SDL3-3.4.10.tar.gz
    URL_HASH SHA256=12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785
  )
  FetchContent_MakeAvailable(SDL3)
endif()
find_package(OpenGL REQUIRED)

include(generated/recomp_sources.cmake)

add_executable(ndsrecomp
  src/main.c
  src/easygl2d.c
  src/nds_gpu.c
  src/nds_runtime.c
  src/nds_video.c
  ${NDSRECOMP_SOURCES}
  generated/rom_data.c
  generated/arm7_data.c
)
target_include_directories(ndsrecomp PRIVATE src generated)

if(TARGET SDL3::SDL3-static)
  target_link_libraries(ndsrecomp PRIVATE SDL3::SDL3-static)
else()
  target_link_libraries(ndsrecomp PRIVATE SDL3::SDL3)
endif()
target_link_libraries(ndsrecomp PRIVATE OpenGL::GL)
if(NOT MSVC)
  target_link_libraries(ndsrecomp PRIVATE m)
endif()

if(MSVC)
  target_compile_options(ndsrecomp PRIVATE /W4)
  set_source_files_properties(${NDSRECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS /O2)
else()
  target_compile_options(ndsrecomp PRIVATE -Wall -Wextra -Wpedantic)
  set_source_files_properties(${NDSRECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS -O2)
endif()
