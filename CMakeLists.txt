cmake_minimum_required(VERSION 3.20)
project(cpct VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------
option(BUILD_CLI "Build the CLI tool" ON)
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_EXAMPLES "Build examples" OFF)

# ----------------------------------------------------------------------
# Dependencies  (vcpkg supplies FindXXX.cmake for all of these)
# ----------------------------------------------------------------------
find_package(minizip-ng REQUIRED)
find_package(LibArchive REQUIRED)
find_package(zstd REQUIRED)
find_package(BZip2 REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(fmt REQUIRED)
find_package(cxxopts REQUIRED)

# lzma-sdk  — C SDK for LZMA2 + AES
find_path(LZMA_SDK_INCLUDE_DIR
  NAMES "LzmaEnc.h"
  PATH_SUFFIXES "lzma-sdk" "lzma"
)

find_library(LZMA_SDK_LIBRARY
  NAMES lzma-sdk liblzma-sdk LZMA-SDK
  PATH_SUFFIXES lzma-sdk
)

if(LZMA_SDK_INCLUDE_DIR AND LZMA_SDK_LIBRARY)
  set(LZMA_SDK_AVAILABLE TRUE)
  message(STATUS "Found lzma-sdk: ${LZMA_SDK_INCLUDE_DIR}")
elseif(LZMA_SDK_INCLUDE_DIR)
  # Header found but no library — 7z writer will be stubbed
  message(WARNING "lzma-sdk headers found but no library; 7z writer not available")
  set(LZMA_SDK_AVAILABLE FALSE)
else()
  message(WARNING "lzma-sdk not found; 7z backend will be stubbed")
  set(LZMA_SDK_AVAILABLE FALSE)
endif()

# ----------------------------------------------------------------------
# Library  — libcompress
# ----------------------------------------------------------------------
add_library(libcompress STATIC
  libcompress/src/factory.cpp
  libcompress/src/format_detection.cpp
  libcompress/src/zip_archive.cpp
  libcompress/src/tar_archive.cpp
  libcompress/src/gzip_archive.cpp
  libcompress/src/bzip2_archive.cpp
  libcompress/src/xz_archive.cpp
  libcompress/src/zstd_archive.cpp
  libcompress/src/sevenz_archive.cpp
  libcompress/src/rar_archive.cpp
  libcompress/src/multipart.cpp
)

target_compile_definitions(libcompress PRIVATE _CRT_SECURE_NO_WARNINGS)

target_include_directories(libcompress
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libcompress/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(libcompress
  PUBLIC
    MINIZIP::minizip-ng
    LibArchive::LibArchive
    zstd::libzstd
    BZip2::BZip2
    LibLZMA::LibLZMA
    fmt::fmt
)

if(LZMA_SDK_AVAILABLE)
  target_compile_definitions(libcompress PRIVATE COMPRESS_HAVE_LZMA_SDK)
  target_link_libraries(libcompress PRIVATE ${LZMA_SDK_LIBRARY})
  target_include_directories(libcompress PRIVATE ${LZMA_SDK_INCLUDE_DIR})
endif()

# ----------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------
if(BUILD_CLI)
  add_subdirectory(cli)
endif()

# ----------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------
if(BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_subdirectory(tests)
endif()

# ----------------------------------------------------------------------
# Examples
# ----------------------------------------------------------------------
if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ----------------------------------------------------------------------
# Install
# ----------------------------------------------------------------------
include(GNUInstallDirs)

if(BUILD_CLI)
  install(TARGETS cpct RUNTIME DESTINATION bin)
endif()
