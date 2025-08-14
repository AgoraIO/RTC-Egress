cmake_minimum_required(VERSION 3.10)
project(AgoraRtcEgress)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set compiler flags for Linux target
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DTARGET_OS_LINUX")

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(OpenSSL REQUIRED)

# FFmpeg packages
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
    libswresample
)

# Agora SDK configuration
set(AGORA_SDK_DIR ${CMAKE_SOURCE_DIR}/agora_sdk)
set(AGORA_INCLUDE_DIR ${AGORA_SDK_DIR}/include)
set(AGORA_LIB_DIR ${AGORA_SDK_DIR})

# Find Agora SDK libraries
find_library(LIB_AGORA_RTC_SDK NAMES agora_rtc_sdk
    PATHS ${AGORA_LIB_DIR}
    NO_DEFAULT_PATH 
    REQUIRED
)

find_library(LIB_AOSL NAMES aosl
    PATHS ${AGORA_LIB_DIR}
    NO_DEFAULT_PATH
    REQUIRED
)

find_library(LIB_AGORA_FDKAAC NAMES libagora-fdkaac.so
    PATHS ${AGORA_LIB_DIR}
    NO_DEFAULT_PATH
    REQUIRED
)

# Print found libraries
message(STATUS "Found Agora SDK libraries:")
message(STATUS "  RTC SDK: ${LIB_AGORA_RTC_SDK}")
message(STATUS "  AOSL: ${LIB_AOSL}")
message(STATUS "  FDKAAC: ${LIB_AGORA_FDKAAC}")

# ============================================================================
# snapshot_sink library
# ============================================================================

add_library(snapshot_sink SHARED
    src/rtc_client.cpp
    src/snapshot_sink.cpp
    src/recording_sink.cpp
    src/ts_segment_manager.cpp
    src/metadata_manager.cpp
    src/video_frame.cpp
    src/video_compositor.cpp
    src/snapshot_encoder.cpp
    src/layout_detector.cpp
    src/common/sample_common.cpp
    src/common/ffmpeg_utils.cpp
    src/common/file_utils.cpp
)

target_include_directories(snapshot_sink PUBLIC
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_SOURCE_DIR}/src/common
    ${AGORA_INCLUDE_DIR}
)

target_link_libraries(snapshot_sink PUBLIC
    ${LIB_AGORA_RTC_SDK}
    ${LIB_AOSL}
    ${LIB_AGORA_FDKAAC}
    PkgConfig::FFMPEG
    yaml-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
    dl
    rt
    m
)

# ============================================================================
# eg_worker executable
# ============================================================================

add_executable(eg_worker
    src/main.cpp
    src/task_pipe.cpp
    src/task_connection_observer.cpp
    src/frame_processor.cpp
    src/common/opt_parser.cpp
)

target_include_directories(eg_worker PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
    ${AGORA_INCLUDE_DIR}
)

target_link_libraries(eg_worker PRIVATE
    snapshot_sink
    yaml-cpp
    pthread
    dl
    rt
    m
)

# ============================================================================
# Installation
# ============================================================================

# Custom target to copy Agora SDK libraries to project bin directory
add_custom_target(copy_agora_libs ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIB_AGORA_RTC_SDK} ${CMAKE_SOURCE_DIR}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIB_AOSL} ${CMAKE_SOURCE_DIR}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIB_AGORA_FDKAAC} ${CMAKE_SOURCE_DIR}/bin/
    DEPENDS eg_worker snapshot_sink
    COMMENT "Copying Agora SDK libraries to project bin directory"
)

install(TARGETS eg_worker snapshot_sink
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin
    ARCHIVE DESTINATION bin
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/include/ 
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)
