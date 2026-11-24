# npk_core: the NXPK format and everything needed to turn one stored entry back
# into the original file. No threads and no I/O policy, so any project can take
# it as is (link npk::core).
include("${PROJECT_SOURCE_DIR}/cmake/NpkWarnings.cmake")

add_library(npk_core STATIC
    src/error.cpp
    src/hash.cpp
    src/paths.cpp
    src/input_file.cpp
    src/archive.cpp
    src/decoder.cpp
    src/filetypes.cpp
    src/resdb.cpp
    src/mounts.cpp
    src/install.cpp
    src/names.cpp
    src/plan.cpp
    src/inputs.cpp
)
add_library(npk::core ALIAS npk_core)

target_compile_features(npk_core PUBLIC cxx_std_23)
target_include_directories(npk_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(npk_core PRIVATE npk::lz4 npk::zstd npk::libdeflate)
npk_target_warnings(npk_core)
