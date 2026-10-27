cmake_minimum_required(VERSION 3.20)
project(CinematicRAWStudioDecoder VERSION 0.1.0 LANGUAGES CXX)

set(CRAW_SPEC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../CinematicRAWStudio-Spec"
    CACHE PATH "Path to CinematicRAWStudio-Spec (golden vectors)")
option(CRAW_BUILD_TOOLS "Build craw_dec" ON)
option(CRAW_BUILD_TESTS "Build conformance tests" ON)
option(CRAW_FUZZ "Build libFuzzer target (clang)" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(CrawDecoder src/Decoder.cpp src/Reader.cpp)
add_library(CRAW::Decoder ALIAS CrawDecoder)
target_compile_features(CrawDecoder PUBLIC cxx_std_20)
target_include_directories(CrawDecoder PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
set_target_properties(CrawDecoder PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(CrawDecoder PRIVATE -Wall -Wextra -Wpedantic)
find_package(Threads REQUIRED)
target_link_libraries(CrawDecoder PUBLIC Threads::Threads)
if(ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    target_compile_options(CrawDecoder PRIVATE -march=armv8-a+crc)
endif()

if(CRAW_BUILD_TOOLS)
    add_executable(craw_dec tools/craw_dec.cpp)
    target_link_libraries(craw_dec PRIVATE CRAW::Decoder)

    # DaVinci Resolve workflow: CinemaDNG export and a virtual CinemaDNG mount.
    add_library(CrawClip STATIC tools/common/Clip.cpp)
    target_link_libraries(CrawClip PUBLIC CRAW::Decoder)
    add_executable(craw2dng tools/craw2dng.cpp)
    target_link_libraries(craw2dng PRIVATE CrawClip)

    find_path(FUSE_T_INCLUDE fuse.h PATHS "/Library/Application Support/fuse-t/include/fuse"
              /usr/local/include/fuse /usr/include/fuse NO_DEFAULT_PATH)
    find_library(FUSE_T_LIB NAMES fuse-t fuse PATHS /usr/local/lib /usr/lib)
    if(FUSE_T_INCLUDE AND FUSE_T_LIB)
        add_executable(craw_mount tools/craw_mount.cpp)
        target_include_directories(craw_mount PRIVATE ${FUSE_T_INCLUDE})
        target_compile_definitions(craw_mount PRIVATE FUSE_USE_VERSION=26 _FILE_OFFSET_BITS=64)
        target_link_libraries(craw_mount PRIVATE CrawClip ${FUSE_T_LIB})
        message(STATUS "craw_mount: FUSE at ${FUSE_T_LIB}")
    else()
        message(STATUS "craw_mount: FUSE not found (install fuse-t or libfuse), skipped")
    endif()
endif()

if(CRAW_BUILD_TESTS)
    enable_testing()
    add_executable(craw_dec_conformance tests/ConformanceTests.cpp)
    target_link_libraries(craw_dec_conformance PRIVATE CRAW::Decoder)
    target_compile_definitions(craw_dec_conformance PRIVATE CRAW_VECTORS_DIR="${CRAW_SPEC_DIR}/vectors")
    add_test(NAME conformance COMMAND craw_dec_conformance)
endif()

if(CRAW_FUZZ)
    add_executable(craw_fuzz_decoder tests/FuzzDecoder.cpp)
    target_compile_options(craw_fuzz_decoder PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(craw_fuzz_decoder PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(craw_fuzz_decoder PRIVATE CRAW::Decoder)
endif()

include(GNUInstallDirs)
install(TARGETS CrawDecoder EXPORT CrawDecoderTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT CrawDecoderTargets NAMESPACE CRAW:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrawDecoder)
