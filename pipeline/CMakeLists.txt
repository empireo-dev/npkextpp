# npk_pipeline: extracts a whole Plan on all cores.
#   Reader (1) → DecodeQueue → Decoders (one per hardware thread) → WriteQueue → Writers (adaptive)
# A Tuner watches how busy each stage is and adjusts the number of writers and
# the read block size while the job runs.
include("${PROJECT_SOURCE_DIR}/cmake/NpkWarnings.cmake")

add_library(npk_pipeline STATIC
    src/byte_budget.cpp
    src/stage_clock.cpp
    src/writeback.cpp
    src/reader.cpp
    src/decoder_pool.cpp
    src/writer_pool.cpp
    src/tuner.cpp
    src/index_writer.cpp
    src/pipeline.cpp
)
if(WIN32)
    target_sources(npk_pipeline PRIVATE src/output_file_win32.cpp)
else()
    target_sources(npk_pipeline PRIVATE src/output_file_posix.cpp)
endif()
add_library(npk::pipeline ALIAS npk_pipeline)

target_include_directories(npk_pipeline
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(npk_pipeline PUBLIC npk::core Threads::Threads)
npk_target_warnings(npk_pipeline)
