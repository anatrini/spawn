cmake_minimum_required(VERSION 3.19)

# Before project(): universal binary, macOS 11 or later.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS")

project(spawn VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SPAWN_MAX "Build the Max external" ON)
option(SPAWN_PD "Build the Pd external (Pd vanilla, plugdata)" ON)
option(SPAWN_TESTS "Build the tests of the process code (no Max or Pd needed)" OFF)

# ---------------------------------------------------------------- Max
if(SPAWN_MAX)
  # Max SDK: a symlink "max-sdk" in this folder (see README), or -DMAX_SDK_BASE_DIR=...
  set(MAX_SDK_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/max-sdk/source/max-sdk-base" CACHE PATH "max-sdk-base")
  if(NOT EXISTS "${MAX_SDK_BASE_DIR}/c74support/max-includes")
    message(FATAL_ERROR "Max SDK not found at ${MAX_SDK_BASE_DIR}\n"
                        "ln -s ~/Documents/Max\\ 9/Packages/max-sdk max-sdk\n"
                        "or build only for Pd with -DSPAWN_MAX=OFF")
  endif()

  add_library(spawn MODULE source/max/spawn.cpp source/core/proc.cpp)
  target_include_directories(spawn PRIVATE
    source/core
    "${MAX_SDK_BASE_DIR}/c74support/max-includes")
  target_compile_options(spawn PRIVATE -Wall -Wno-unused-parameter)
  target_link_options(spawn PRIVATE -undefined dynamic_lookup)
  set_target_properties(spawn PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION mxo
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/source/max/Info.plist.in")

  # Ad-hoc signature of the whole bundle (no developer certificate), then copy:
  # the repository is a Max package, the external goes to externals/.
  add_custom_command(TARGET spawn POST_BUILD
    COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:spawn>"
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_CURRENT_SOURCE_DIR}/externals/spawn.mxo"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_BUNDLE_DIR:spawn>"
            "${CMAKE_CURRENT_SOURCE_DIR}/externals/spawn.mxo")
endif()

# ---------------------------------------------------------------- Pd
if(SPAWN_PD)
  # m_pd.h: from -DPD_INCLUDE_DIR=..., otherwise the one of Pd 0.57-0, downloaded.
  set(PD_INCLUDE_DIR "" CACHE PATH "Folder with m_pd.h")
  if(NOT PD_INCLUDE_DIR)
    set(PD_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/pd-include")
    file(DOWNLOAD
      https://raw.githubusercontent.com/pure-data/pure-data/0.57-0/src/m_pd.h
      "${PD_INCLUDE_DIR}/m_pd.h"
      EXPECTED_HASH SHA256=9ad0a74e9ef2d46a4a8c28b3e59b6558a43afefa98455f55cb7af842141746f8)
  endif()

  add_library(spawn_pd MODULE source/pd/spawn_pd.cpp source/core/proc.cpp)
  target_include_directories(spawn_pd PRIVATE source/core "${PD_INCLUDE_DIR}")
  target_compile_options(spawn_pd PRIVATE -Wall -Wno-unused-parameter)
  target_link_options(spawn_pd PRIVATE -undefined dynamic_lookup)
  # .pd_darwin with both architectures: loaded by every Pd version and by plugdata.
  set_target_properties(spawn_pd PROPERTIES
    PREFIX ""
    OUTPUT_NAME spawn
    SUFFIX ".pd_darwin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/pd")
  add_custom_command(TARGET spawn_pd POST_BUILD
    COMMAND codesign --force --sign - "$<TARGET_FILE:spawn_pd>")
endif()

# ---------------------------------------------------------------- tests
if(SPAWN_TESTS)
  add_executable(test_proc tests/test_proc.cpp source/core/proc.cpp)
  target_include_directories(test_proc PRIVATE source/core)
endif()
