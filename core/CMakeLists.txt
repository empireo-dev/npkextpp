# npk_core: the NXPK format and everything needed to turn one stored entry back
# into the original file. No threads and no I/O policy, so any project can take
# it as is (link npk::core).
include("${PROJECT_SOURCE_DIR}/cmake/NpkWarnings.cmake")

# The built-in list of the game's archives (see the file), one string literal per
# line: MSVC refuses a single literal over 16 KiB.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/data/known_archives.txt" npk_known_lines)
set(NPK_KNOWN_ARCHIVES "")
foreach(npk_line IN LISTS npk_known_lines)
    if(npk_line MATCHES "^[ \t]*#" OR npk_line STREQUAL "")
        continue()
    endif()
    string(REPLACE "\\" "\\\\" npk_line "${npk_line}")
    string(REPLACE "\"" "\\\"" npk_line "${npk_line}")
    string(APPEND NPK_KNOWN_ARCHIVES "    \"${npk_line}\\n\"\n")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/data/known_archives.txt")
configure_file(src/known_archives.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/known_archives.cpp" @ONLY)

add_library(npk_core STATIC
    src/error.cpp
    src/hash.cpp
    src/paths.cpp
    src/input_file.cpp
    src/archive.cpp
    src/decoder.cpp
    src/encoder.cpp
    src/archive_update.cpp
    src/filetypes.cpp
    src/resdb.cpp
    src/mounts.cpp
    src/install.cpp
    src/names.cpp
    src/plan.cpp
    src/inputs.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/known_archives.cpp"
)
add_library(npk::core ALIAS npk_core)

target_compile_features(npk_core PUBLIC cxx_std_23)
target_include_directories(npk_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(npk_core PRIVATE npk::lz4 npk::zstd npk::libdeflate)
npk_target_warnings(npk_core)
