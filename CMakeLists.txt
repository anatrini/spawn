cmake_minimum_required(VERSION 3.19)

# Before project(): universal binary, macOS 11 or later.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS")

project(spawn VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Max SDK: a symlink "max-sdk" in this folder (see README), or -DMAX_SDK_BASE_DIR=...
set(MAX_SDK_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/max-sdk/source/max-sdk-base" CACHE PATH "max-sdk-base")
if(NOT EXISTS "${MAX_SDK_BASE_DIR}/c74support/max-includes")
  message(FATAL_ERROR "Max SDK not found at ${MAX_SDK_BASE_DIR}\n"
                      "ln -s ~/Documents/Max\\ 9/Packages/max-sdk max-sdk")
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

option(SPAWN_TESTS "Build the tests of the process code (no Max needed)" OFF)
if(SPAWN_TESTS)
  add_executable(test_proc tests/test_proc.cpp source/core/proc.cpp)
  target_include_directories(test_proc PRIVATE source/core)
endif()
