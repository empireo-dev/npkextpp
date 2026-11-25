# npkextpp: the command line tool. Built as one self-contained executable: static
# runtime, no DLLs, no config or resource files next to it.
include("${PROJECT_SOURCE_DIR}/cmake/NpkWarnings.cmake")

# Third-party license texts, embedded so `npkextpp --licenses` can show them
# (BSD/MIT require the notices to travel with the binary).
set(_npk_license_files
    "LZ4|${PROJECT_SOURCE_DIR}/third_party/lz4/LICENSE"
    "Zstandard|${PROJECT_SOURCE_DIR}/third_party/zstd/LICENSE"
    "libdeflate|${PROJECT_SOURCE_DIR}/third_party/libdeflate/LICENSE"
    "CLI11|${PROJECT_SOURCE_DIR}/third_party/cli11/LICENSE"
    "FTXUI|${PROJECT_SOURCE_DIR}/third_party/ftxui/LICENSE"
)
set(NPK_LICENSE_TEXT "")
foreach(_item IN LISTS _npk_license_files)
    string(REPLACE "|" ";" _parts "${_item}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _file)
    file(READ "${_file}" _text)
    string(APPEND NPK_LICENSE_TEXT "=== ${_name} ===\n\n${_text}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_file}")
endforeach()
configure_file(src/licenses.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/licenses.cpp" @ONLY)

add_executable(npkextpp
    src/main.cpp
    src/cli_options.cpp
    src/console.cpp
    src/dashboard.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/licenses.cpp"
)
target_include_directories(npkextpp PRIVATE src)
target_compile_definitions(npkextpp PRIVATE NPKEXTPP_VERSION="${PROJECT_VERSION}")
target_link_libraries(npkextpp PRIVATE npk::pipeline CLI11::CLI11 ftxui::dom ftxui::screen)
npk_target_warnings(npkextpp)

if(MINGW)
    target_link_options(npkextpp PRIVATE -static)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NPKEXTPP_FULLY_STATIC)
        target_link_options(npkextpp PRIVATE -static)
    else()
        # glibc stays dynamic (always present); the C++ runtime is linked in.
        target_link_options(npkextpp PRIVATE -static-libstdc++ -static-libgcc)
    endif()
endif()
