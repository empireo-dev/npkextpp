include("${PROJECT_SOURCE_DIR}/cmake/NpkWarnings.cmake")

add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE npk::core npk::lz4 npk::zstd npk::libdeflate)
npk_target_warnings(core_tests)
add_test(NAME core_tests COMMAND core_tests)

if(NPKEXTPP_BUILD_PIPELINE)
    add_executable(pipeline_tests pipeline_tests.cpp)
    target_link_libraries(pipeline_tests PRIVATE npk::pipeline npk::lz4 npk::zstd npk::libdeflate)
    npk_target_warnings(pipeline_tests)
    add_test(NAME pipeline_tests COMMAND pipeline_tests)
endif()

# Single-threaded reference extractor (core only), used by compare_with_npkext.py.
add_executable(core_extract tools/core_extract.cpp)
target_link_libraries(core_extract PRIVATE npk::core)
npk_target_warnings(core_extract)
