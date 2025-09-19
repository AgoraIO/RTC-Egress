# CMakeLists.txt for testing real components

cmake_minimum_required(VERSION 3.10)
project(ComponentTests)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set compiler flags for Linux target (matching main project)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DTARGET_OS_LINUX")

# Find required packages
find_package(GTest REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(OpenSSL REQUIRED)

# FFmpeg packages (needed by real components)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
    libswresample
)

# Mocked tests removed per user request - keeping only real component tests

# Create simplified real component test executable (without SDK dependencies)
add_executable(real_component_tests_simple
    real_component_tests_simple.cpp
)

# Create REAL VideoCompositor test executable (with actual source code)
add_executable(video_compositor_real_test
    video_compositor_real_test.cpp
    ../src/video_compositor.cpp
    ../src/video_frame.cpp
    ../src/layout_detector.cpp
    ../src/common/ffmpeg_utils.cpp
    ../src/common/file_utils.cpp
    ../src/common/helper.cpp
)

# Create REAL file generation test executable (TS/JPEG/MP4 with actual components)
add_executable(real_file_generation_tests
    real_file_generation_tests.cpp
    ../src/recording_sink.cpp
    ../src/snapshot_sink.cpp
    ../src/video_compositor.cpp
    ../src/video_frame.cpp
    ../src/layout_detector.cpp
    ../src/snapshot_encoder.cpp
    ../src/ts_segment_manager.cpp
    ../src/metadata_manager.cpp
    ../src/common/ffmpeg_utils.cpp
    ../src/common/file_utils.cpp
    ../src/common/helper.cpp
)

# Create A/V synchronization test executable
add_executable(av_sync_tests
    av_sync_tests.cpp
    ../src/recording_sink.cpp
    ../src/video_compositor.cpp
    ../src/video_frame.cpp
    ../src/layout_detector.cpp
    ../src/snapshot_encoder.cpp
    ../src/ts_segment_manager.cpp
    ../src/metadata_manager.cpp
    ../src/common/ffmpeg_utils.cpp
    ../src/common/file_utils.cpp
    ../src/common/helper.cpp
)

# Create composition mode test executable
add_executable(composition_tests
    composition_tests.cpp
    ../src/recording_sink.cpp
    ../src/snapshot_sink.cpp
    ../src/video_compositor.cpp
    ../src/video_frame.cpp
    ../src/layout_detector.cpp
    ../src/snapshot_encoder.cpp
    ../src/ts_segment_manager.cpp
    ../src/metadata_manager.cpp
    ../src/common/ffmpeg_utils.cpp
    ../src/common/file_utils.cpp
    ../src/common/helper.cpp
)

# Link with Google Test

target_link_libraries(real_component_tests_simple
    GTest::GTest
    GTest::Main
    pthread
)

# Include directories for real VideoCompositor test
target_include_directories(video_compositor_real_test PRIVATE
    ${CMAKE_SOURCE_DIR}/../src/include
    ${CMAKE_SOURCE_DIR}/../src/common
)

target_link_libraries(video_compositor_real_test
    GTest::GTest
    GTest::Main
    PkgConfig::FFMPEG
    yaml-cpp
    pthread
    dl
    rt
    m
)

# Include directories for real file generation tests
target_include_directories(real_file_generation_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/../src/include
    ${CMAKE_SOURCE_DIR}/../src/common
)

target_link_libraries(real_file_generation_tests
    GTest::GTest
    GTest::Main
    PkgConfig::FFMPEG
    yaml-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
    dl
    rt
    m
)

# Include directories for A/V sync tests
target_include_directories(av_sync_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/../src/include
    ${CMAKE_SOURCE_DIR}/../src/common
)

target_link_libraries(av_sync_tests
    GTest::GTest
    GTest::Main
    PkgConfig::FFMPEG
    yaml-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
    dl
    rt
    m
)

# Include directories for composition tests
target_include_directories(composition_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/../src/include
    ${CMAKE_SOURCE_DIR}/../src/common
)

target_link_libraries(composition_tests
    GTest::GTest
    GTest::Main
    PkgConfig::FFMPEG
    yaml-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
    dl
    rt
    m
)

# Enable testing
enable_testing()
add_test(NAME RealComponentTests COMMAND real_component_tests_simple)
add_test(NAME RealVideoCompositorTests COMMAND video_compositor_real_test)
add_test(NAME RealFileGenerationTests COMMAND real_file_generation_tests)
add_test(NAME AVSyncTests COMMAND av_sync_tests)
add_test(NAME CompositionTests COMMAND composition_tests)

# Custom target to run tests
add_custom_target(run_simple_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --verbose
    DEPENDS real_component_tests_simple
    COMMENT "Running simple tests"
)

# Additional test: snapshot user filtering behavior
add_executable(snapshot_filter_tests
    snapshot_filter_tests.cpp
    ../src/snapshot_sink.cpp
    ../src/metadata_manager.cpp
    ../src/video_compositor.cpp
    ../src/video_frame.cpp
    ../src/layout_detector.cpp
    ../src/snapshot_encoder.cpp
    ../src/common/ffmpeg_utils.cpp
    ../src/common/file_utils.cpp
    ../src/common/helper.cpp
)

target_include_directories(snapshot_filter_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/../src/include
    ${CMAKE_SOURCE_DIR}/../src/common
)

target_link_libraries(snapshot_filter_tests
    GTest::GTest
    GTest::Main
    PkgConfig::FFMPEG
    yaml-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
    dl
    rt
    m
)

add_test(NAME SnapshotFilterTests COMMAND snapshot_filter_tests)
