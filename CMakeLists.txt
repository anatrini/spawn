cmake_minimum_required(VERSION 3.19)

# Before project(): universal binary, macOS 11 or later.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS")

project(spawn VERSION 0.3.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SPAWN_MAX "Build the Max external" ON)
option(SPAWN_PD "Build the Pd external (Pd vanilla, plugdata)" ON)
option(SPAWN_TESTS "Build the tests of the process code (no Max or Pd needed)" OFF)

if(WIN32)
  if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "On Windows, build with Visual Studio for x64: -A x64")
  endif()
  # The C++ runtime inside each binary: nothing to install on the target
  # computer, whatever Visual C++ runtime the host or the system has.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set(SPAWN_PROC_SOURCE source/core/proc_win.cpp)
  set(SPAWN_COMPILE_OPTIONS /W3 /utf-8)
else()
  set(SPAWN_PROC_SOURCE source/core/proc.cpp)
  set(SPAWN_COMPILE_OPTIONS -Wall -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------- Max
if(SPAWN_MAX)
  # Max SDK: a symlink "max-sdk" in this folder (see README), or -DMAX_SDK_BASE_DIR=...
  set(MAX_SDK_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/max-sdk/source/max-sdk-base" CACHE PATH "max-sdk-base")
  if(NOT EXISTS "${MAX_SDK_BASE_DIR}/c74support/max-includes")
    message(FATAL_ERROR "Max SDK not found at ${MAX_SDK_BASE_DIR}\n"
                        "ln -s ~/Documents/Max\\ 9/Packages/max-sdk max-sdk\n"
                        "or -DMAX_SDK_BASE_DIR=<folder of max-sdk-base>\n"
                        "or build only for Pd with -DSPAWN_MAX=OFF")
  endif()

  add_library(spawn MODULE source/max/spawn.cpp ${SPAWN_PROC_SOURCE})
  target_include_directories(spawn PRIVATE
    source/core
    "${MAX_SDK_BASE_DIR}/c74support/max-includes")
  target_compile_options(spawn PRIVATE ${SPAWN_COMPILE_OPTIONS})

  if(WIN32)
    target_compile_definitions(spawn PRIVATE
      WIN_VERSION MAXAPI_USE_MSCRT _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(spawn PRIVATE "${MAX_SDK_BASE_DIR}/c74support/max-includes/x64/MaxAPI.lib")
    # The generator expression keeps Visual Studio from adding a Release subfolder.
    # The repository is a Max package, the external goes to externals/.
    set_target_properties(spawn PROPERTIES
      PREFIX ""
      SUFFIX ".mxe64"
      LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/externals>"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/implib/max")
  else()
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

  add_library(spawn_pd MODULE source/pd/spawn_pd.cpp ${SPAWN_PROC_SOURCE})
  target_include_directories(spawn_pd PRIVATE source/core "${PD_INCLUDE_DIR}")
  target_compile_options(spawn_pd PRIVATE ${SPAWN_COMPILE_OPTIONS})

  if(WIN32)
    # Links to pd.dll, the name it has in Pd vanilla and in plugdata (standalone).
    set(PD_IMPORT_LIB "${CMAKE_CURRENT_BINARY_DIR}/pd.lib")
    add_custom_command(OUTPUT "${PD_IMPORT_LIB}"
      COMMAND "${CMAKE_AR}" /NOLOGO /MACHINE:X64 "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/source/pd/pd.def"
              "/OUT:${PD_IMPORT_LIB}"
      DEPENDS source/pd/pd.def
      VERBATIM)
    add_custom_target(spawn_pd_import_lib DEPENDS "${PD_IMPORT_LIB}")
    add_dependencies(spawn_pd spawn_pd_import_lib)
    target_link_libraries(spawn_pd PRIVATE "${PD_IMPORT_LIB}")
    # .dll: loaded by every 64-bit Pd version and by plugdata.
    set_target_properties(spawn_pd PROPERTIES
      PREFIX ""
      OUTPUT_NAME spawn
      SUFFIX ".dll"
      LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/pd>"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/implib/pd")
  else()
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
endif()

# ---------------------------------------------------------------- tests
if(SPAWN_TESTS)
  add_executable(test_proc tests/test_proc.cpp ${SPAWN_PROC_SOURCE})
  target_include_directories(test_proc PRIVATE source/core)
  target_compile_options(test_proc PRIVATE $<$<BOOL:${WIN32}>:/utf-8>)
endif()
