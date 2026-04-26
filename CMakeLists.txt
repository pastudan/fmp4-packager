cmake_minimum_required(VERSION 3.15)
project(fmp4-packager CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# FFmpeg via pkg-config: libavformat for MKV demux, libavcodec for AAC encode &
# audio decode, libswresample for FLTP 48k stereo conversion, libavutil for
# AVChannelLayout / av_rescale_q. We deliberately do NOT use the libavformat
# mp4 muxer; Fmp4Writer emits boxes directly.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswresample
)

find_package(CURL REQUIRED)

add_executable(fmp4-packager
    src/server.cpp
    src/Session.cpp
    src/Demuxer.cpp
    src/AacTranscoder.cpp
    src/HttpReader.cpp
    src/HlsManifest.cpp
    src/BoxBuilder.cpp
    src/Fmp4Writer.cpp
)

target_include_directories(fmp4-packager PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(fmp4-packager
    PkgConfig::FFMPEG
    CURL::libcurl
    pthread
)

target_compile_options(fmp4-packager PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# Optional: unit tests (built with -DBUILD_TESTS=ON).
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    add_executable(test_box_builder
        tests/test_box_builder.cpp
        src/BoxBuilder.cpp
    )
    target_include_directories(test_box_builder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(test_box_builder PRIVATE -Wall -Wextra)

    add_executable(test_init_roundtrip
        tests/test_init_roundtrip.cpp
        src/Fmp4Writer.cpp
        src/BoxBuilder.cpp
    )
    target_include_directories(test_init_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_init_roundtrip PkgConfig::FFMPEG)
    target_compile_options(test_init_roundtrip PRIVATE -Wall -Wextra -Wno-unused-parameter)

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_segment_roundtrip.cpp)
        add_executable(test_segment_roundtrip
            tests/test_segment_roundtrip.cpp
            src/Fmp4Writer.cpp
            src/BoxBuilder.cpp
        )
        target_include_directories(test_segment_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(test_segment_roundtrip PkgConfig::FFMPEG)
        target_compile_options(test_segment_roundtrip PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_determinism.cpp)
        add_executable(test_determinism
            tests/test_determinism.cpp
            src/Fmp4Writer.cpp
            src/BoxBuilder.cpp
        )
        target_include_directories(test_determinism PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(test_determinism PkgConfig::FFMPEG)
        target_compile_options(test_determinism PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endif()

message(STATUS "FFmpeg libraries: ${FFMPEG_LIBRARIES}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
