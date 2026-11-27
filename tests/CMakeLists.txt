include("${PROJECT_SOURCE_DIR}/cmake/NpkWarnings.cmake")

add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE npk::core npk::lz4 npk::zstd npk::libdeflate)
npk_target_warnings(core_tests)
add_test(NAME core_tests COMMAND core_tests)

if(NPKEXTPP_BUILD_PIPELINE)
    add_executable(pipeline_tests pipeline_tests.cpp)
    target_link_libraries(pipeline_tests PRIVATE npk::pipeline npk::lz4 npk::zstd npk::libdeflate)
    target_include_directories(pipeline_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/pipeline/src")   # DiskModel
    npk_target_warnings(pipeline_tests)
    add_test(NAME pipeline_tests COMMAND pipeline_tests)
endif()

# Single-threaded reference extractor (core only), used by compare_with_npkext.py.
add_executable(core_extract tools/core_extract.cpp)
target_link_libraries(core_extract PRIVATE npk::core)
npk_target_warnings(core_extract)

# The pipeline and tuner against a simulated disk (docs/simulated_disk.md).
if(NPKEXTPP_BUILD_PIPELINE)
    add_executable(tune_sim tools/tune_sim.cpp sim/simulated_disk.cpp)
    target_link_libraries(tune_sim PRIVATE npk::pipeline CLI11::CLI11)
    npk_target_warnings(tune_sim)
endif()

# Test and tool binaries go to <bin folder>/tests/, next to the exe's folder.
if(PROJECT_IS_TOP_LEVEL)
    foreach(_npk_test_target IN ITEMS core_tests pipeline_tests core_extract tune_sim)
        if(TARGET ${_npk_test_target})
            set_target_properties(${_npk_test_target} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "$<1:${NPKEXTPP_BIN_DIR}/tests>")
        endif()
    endforeach()
endif()
