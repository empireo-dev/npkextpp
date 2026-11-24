# Third-party libraries. Every dependency is vendored as its unchanged release
# archive (plus its license), so building never needs the network. FetchContent
# only unpacks the local archive into the build tree and checks its hash.
#
# Targets provided:
#   npk::lz4         LZ4 block format (lz4.c + lz4hc.c)
#   npk::zstd        Zstandard, static, single-threaded, no legacy formats
#   npk::libdeflate  libdeflate (raw deflate / zlib), static
#   CLI11::CLI11     header-only command line parser (only needed by the app)

include(FetchContent)

# Policies that the dependencies' own CMakeLists predate. NEW for CMP0091 makes
# them honour CMAKE_MSVC_RUNTIME_LIBRARY (static runtime for a single exe).
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

# --- LZ4 1.10.0 (BSD-2-Clause) ----------------------------------------------
# Built from its sources directly: we only need the block API, not lz4frame,
# lz4file or the CLI.
FetchContent_Declare(lz4_source
    URL "${CMAKE_CURRENT_SOURCE_DIR}/lz4/lz4-1.10.0.tar.gz"
    URL_HASH SHA256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
FetchContent_MakeAvailable(lz4_source)   # no top-level CMakeLists: unpack only

add_library(npk_lz4 STATIC
    "${lz4_source_SOURCE_DIR}/lib/lz4.c"
    "${lz4_source_SOURCE_DIR}/lib/lz4hc.c"
)
target_include_directories(npk_lz4 SYSTEM PUBLIC "${lz4_source_SOURCE_DIR}/lib")
set_target_properties(npk_lz4 PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(npk::lz4 ALIAS npk_lz4)

# --- Zstandard 1.5.7 (BSD-3-Clause, dual-licensed with GPLv2; used under BSD) -
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)  # we parallelise across entries
set(ZSTD_USE_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
FetchContent_Declare(zstd_source
    URL "${CMAKE_CURRENT_SOURCE_DIR}/zstd/zstd-1.5.7.tar.gz"
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd_source)

# zstd's assembly is built with -Wa,--noexecstack, which LTO drops with a warning;
# its C code gains nothing measurable from LTO here, so keep it out.
set_target_properties(libzstd_static PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION OFF
    INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
    INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF)

add_library(npk_zstd INTERFACE)
target_link_libraries(npk_zstd INTERFACE libzstd_static)
target_include_directories(npk_zstd SYSTEM INTERFACE "${zstd_source_SOURCE_DIR}/lib")
add_library(npk::zstd ALIAS npk_zstd)

# --- libdeflate 1.26 (MIT) ---------------------------------------------------
set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_GZIP_SUPPORT OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libdeflate_source
    URL "${CMAKE_CURRENT_SOURCE_DIR}/libdeflate/libdeflate-1.26.tar.gz"
    URL_HASH SHA256=125856d4656e0feab660f94842f835923410c9281fedbcee64598c918da42b5a
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
FetchContent_MakeAvailable(libdeflate_source)

add_library(npk_libdeflate INTERFACE)
target_link_libraries(npk_libdeflate INTERFACE libdeflate::libdeflate_static)
add_library(npk::libdeflate ALIAS npk_libdeflate)

# --- CLI11 2.7.2 (BSD-3-Clause), single header -------------------------------
if(NOT TARGET CLI11::CLI11)
    add_library(npk_cli11 INTERFACE)
    target_include_directories(npk_cli11 SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/cli11")
    add_library(CLI11::CLI11 ALIAS npk_cli11)
endif()
