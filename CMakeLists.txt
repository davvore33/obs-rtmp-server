# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 rtmp-server-source contributors

cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)
option(BUILD_TESTS "Build the RTMP protocol tests and the rtmp-probe tool" ON)

include(compilerconfig)
include(defaults)
include(helpers)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/finders")

# ---------------------------------------------------------------------------
# rtmp-core: the RTMP ingest server. Plain C++17 + sockets, no OBS or FFmpeg
# dependency, so the protocol can be tested without running OBS.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(rtmp-core STATIC)
target_sources(
  rtmp-core
  PRIVATE
    src/rtmp/amf0.cpp
    src/rtmp/chunk.cpp
    src/rtmp/flv.cpp
    src/rtmp/hub.cpp
    src/rtmp/server.cpp
    src/rtmp/session.cpp
    src/rtmp/socket.cpp
)
target_include_directories(rtmp-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(rtmp-core PUBLIC Threads::Threads)
if(OS_WINDOWS)
  target_link_libraries(rtmp-core PUBLIC ws2_32)
endif()
set_target_properties(rtmp-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# media-decoder: RTMP packets -> pictures and PCM in formats OBS accepts.
# ---------------------------------------------------------------------------
find_package(libobs REQUIRED)
# FFmpeg comes from the same obs-deps bundle OBS itself is built with, so the
# ABI matches the libavcodec that is already loaded in the OBS process.
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)

add_library(media-decoder STATIC src/media-decoder.cpp)
target_link_libraries(
  media-decoder
  PUBLIC OBS::libobs rtmp-core FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)
set_target_properties(media-decoder PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# The OBS module
# ---------------------------------------------------------------------------
add_library(${CMAKE_PROJECT_NAME} MODULE)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE media-decoder)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/rtmp-server-source.cpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# ---------------------------------------------------------------------------
# Tests and tools (protocol level, no OBS needed)
# ---------------------------------------------------------------------------
if(BUILD_TESTS)
  enable_testing()

  # The tools run from the build tree, not inside OBS.app, so they need to be
  # told where the OBS dependencies live (the Xcode build drops rpaths).
  # Release builds use the hardened runtime (needed to notarize the plugin),
  # which refuses libraries signed by another team such as OBS's FFmpeg; the
  # tools are never shipped, so they opt out.
  function(set_tool_runtime_paths target)
    if(APPLE)
      cmake_path(GET FFmpeg_avcodec_LIBRARY PARENT_PATH ffmpeg_lib_dir)
      cmake_path(SET obs_framework_dir NORMALIZE "${libobs_DIR}/../../..")
      set_target_properties(
        ${target}
        PROPERTIES XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "${ffmpeg_lib_dir} ${obs_framework_dir}"
      )
      foreach(variant Release RelWithDebInfo MinSizeRel)
        set_property(TARGET ${target} PROPERTY "XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME[variant=${variant}]" NO)
      endforeach()
    endif()
  endfunction()

  foreach(test_name amf0 chunk flv session hub fuzz)
    add_executable(test_${test_name} tests/test_${test_name}.cpp)
    target_link_libraries(test_${test_name} PRIVATE rtmp-core)
    add_test(NAME ${test_name} COMMAND test_${test_name})
  endforeach()

  # Listens like the OBS source and checks what arrives (optionally decoding it).
  add_executable(rtmp-probe tools/rtmp-probe.cpp)
  target_link_libraries(rtmp-probe PRIVATE media-decoder)
  set_tool_runtime_paths(rtmp-probe)

  # A real RTMP client (FFmpeg's) that sends a known test pattern.
  add_executable(test-publisher tools/test-publisher.cpp)
  target_link_libraries(test-publisher PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil)
  set_tool_runtime_paths(test-publisher)

  # macOS only: drives the built plugin inside the libobs of an installed OBS.
  set(OBS_APP_PATH "/Applications/OBS.app" CACHE PATH "OBS.app whose libobs the harness runs against")
  if(APPLE AND EXISTS "${OBS_APP_PATH}/Contents/Frameworks/libobs.framework")
    add_executable(obs-harness tools/obs-harness.cpp)
    target_link_libraries(obs-harness PRIVATE OBS::libobs)
    add_dependencies(obs-harness ${CMAKE_PROJECT_NAME})
    set_tool_runtime_paths(obs-harness)
    # ...but it must run against the installed OBS's libobs, not the build tree's.
    set_target_properties(
      obs-harness
      PROPERTIES XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "${OBS_APP_PATH}/Contents/Frameworks"
    )
    add_test(
      NAME obs_harness
      COMMAND
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/harness.sh" "$<TARGET_FILE:obs-harness>" "$<TARGET_FILE:test-publisher>"
        "$<TARGET_BUNDLE_DIR:${CMAKE_PROJECT_NAME}>"
    )
  endif()

  add_test(
    NAME e2e
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e.sh" "$<TARGET_FILE:rtmp-probe>" "$<TARGET_FILE:test-publisher>"
  )
endif()
