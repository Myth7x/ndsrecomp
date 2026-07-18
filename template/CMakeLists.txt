cmake_minimum_required(VERSION 3.24)
project(ndsrecomp LANGUAGES C)

option(NDSRECOMP_USE_COMPILER_CACHE
  "Use ccache or sccache when available"
  ON
)
option(NDSRECOMP_UNITY_BUILD
  "Compile generated translation units in grouped unity files"
  ON
)
set(NDSRECOMP_UNITY_BATCH_SIZE 4 CACHE STRING
  "Generated translation units per unity compilation"
)
set(NDSRECOMP_UNITY_BATCH_KIB 4096 CACHE STRING
  "Approximate maximum generated source size per unity compilation"
)
if(NDSRECOMP_UNITY_BATCH_SIZE LESS 1)
  message(FATAL_ERROR "NDSRECOMP_UNITY_BATCH_SIZE must be positive")
endif()
if(NDSRECOMP_UNITY_BATCH_KIB LESS 1)
  message(FATAL_ERROR "NDSRECOMP_UNITY_BATCH_KIB must be positive")
endif()
math(EXPR NDSRECOMP_UNITY_BATCH_BYTES
  "${NDSRECOMP_UNITY_BATCH_KIB} * 1024"
)

if(NDSRECOMP_USE_COMPILER_CACHE AND NOT CMAKE_C_COMPILER_LAUNCHER)
  find_program(NDSRECOMP_COMPILER_CACHE
    NAMES ccache sccache
    DOC "Compiler cache executable"
  )
  if(NDSRECOMP_COMPILER_CACHE)
    set(CMAKE_C_COMPILER_LAUNCHER "${NDSRECOMP_COMPILER_CACHE}"
      CACHE STRING "C compiler launcher" FORCE
    )
    message(STATUS "Using compiler cache: ${NDSRECOMP_COMPILER_CACHE}")
  endif()
endif()

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

if(NDSRECOMP_UNITY_BUILD)
  # CMake's built-in unity mode does not consistently group generated source
  # files across the supported CMake/compiler combinations. Generate small,
  # explicit include units instead: compiler dependency tracking still sees
  # every included shard, while the build starts one compiler per group.
  set(NDSRECOMP_TRANSLATION_SOURCES)
  set(NDSRECOMP_UNITY_INDEX 0)
  set(NDSRECOMP_UNITY_CONTENT)
  set(NDSRECOMP_UNITY_COUNT 0)
  set(NDSRECOMP_UNITY_BYTES 0)
  set(NDSRECOMP_UNITY_SEEN 0)
  list(LENGTH NDSRECOMP_SOURCES NDSRECOMP_SOURCE_COUNT)
  foreach(NDSRECOMP_SOURCE IN LISTS NDSRECOMP_SOURCES)
    file(SIZE "${CMAKE_CURRENT_SOURCE_DIR}/${NDSRECOMP_SOURCE}"
      NDSRECOMP_SOURCE_BYTES
    )
    math(EXPR NDSRECOMP_NEXT_BYTES
      "${NDSRECOMP_UNITY_BYTES} + ${NDSRECOMP_SOURCE_BYTES}"
    )
    if(NDSRECOMP_UNITY_COUNT GREATER 0 AND
       (NDSRECOMP_UNITY_COUNT EQUAL NDSRECOMP_UNITY_BATCH_SIZE OR
        NDSRECOMP_NEXT_BYTES GREATER NDSRECOMP_UNITY_BATCH_BYTES))
      set(NDSRECOMP_UNITY_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/ndsrecomp_unity_${NDSRECOMP_UNITY_INDEX}.c"
      )
      file(GENERATE OUTPUT "${NDSRECOMP_UNITY_FILE}"
        CONTENT "${NDSRECOMP_UNITY_CONTENT}"
      )
      list(APPEND NDSRECOMP_TRANSLATION_SOURCES "${NDSRECOMP_UNITY_FILE}")
      math(EXPR NDSRECOMP_UNITY_INDEX "${NDSRECOMP_UNITY_INDEX} + 1")
      set(NDSRECOMP_UNITY_CONTENT)
      set(NDSRECOMP_UNITY_COUNT 0)
      set(NDSRECOMP_UNITY_BYTES 0)
    endif()
    file(RELATIVE_PATH NDSRECOMP_INCLUDE
      "${CMAKE_CURRENT_BINARY_DIR}"
      "${CMAKE_CURRENT_SOURCE_DIR}/${NDSRECOMP_SOURCE}"
    )
    string(REPLACE "\\" "/" NDSRECOMP_INCLUDE "${NDSRECOMP_INCLUDE}")
    string(APPEND NDSRECOMP_UNITY_CONTENT
      "#include \"${NDSRECOMP_INCLUDE}\"\n"
    )
    math(EXPR NDSRECOMP_UNITY_COUNT "${NDSRECOMP_UNITY_COUNT} + 1")
    math(EXPR NDSRECOMP_UNITY_BYTES
      "${NDSRECOMP_UNITY_BYTES} + ${NDSRECOMP_SOURCE_BYTES}"
    )
    math(EXPR NDSRECOMP_UNITY_SEEN "${NDSRECOMP_UNITY_SEEN} + 1")
    if(NDSRECOMP_UNITY_SEEN EQUAL NDSRECOMP_SOURCE_COUNT)
      set(NDSRECOMP_UNITY_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/ndsrecomp_unity_${NDSRECOMP_UNITY_INDEX}.c"
      )
      file(GENERATE OUTPUT "${NDSRECOMP_UNITY_FILE}"
        CONTENT "${NDSRECOMP_UNITY_CONTENT}"
      )
      list(APPEND NDSRECOMP_TRANSLATION_SOURCES "${NDSRECOMP_UNITY_FILE}")
      math(EXPR NDSRECOMP_UNITY_INDEX "${NDSRECOMP_UNITY_INDEX} + 1")
      set(NDSRECOMP_UNITY_CONTENT)
      set(NDSRECOMP_UNITY_COUNT 0)
    endif()
  endforeach()
else()
  set(NDSRECOMP_TRANSLATION_SOURCES ${NDSRECOMP_SOURCES})
endif()

add_executable(ndsrecomp
  src/main.c
  src/easygl2d.c
  src/nds_gpu.c
  src/nds_runtime.c
  src/nds_video.c
  ${NDSRECOMP_TRANSLATION_SOURCES}
  generated/rom_data.c
  generated/arm7_data.c
)
target_include_directories(ndsrecomp PRIVATE src generated)
target_precompile_headers(ndsrecomp PRIVATE src/arm9_recomp.h)

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
  set_source_files_properties(${NDSRECOMP_TRANSLATION_SOURCES} PROPERTIES COMPILE_OPTIONS /O2)
else()
  target_compile_options(ndsrecomp PRIVATE -Wall -Wextra -Wpedantic)
  set_source_files_properties(${NDSRECOMP_TRANSLATION_SOURCES} PROPERTIES COMPILE_OPTIONS -O2)
endif()
