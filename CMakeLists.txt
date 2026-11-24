cmake_minimum_required(VERSION 3.25)

project(npkextpp
    VERSION 0.1.0
    DESCRIPTION "Extractor for NeoX NXPK (.npk) archives"
    LANGUAGES C CXX
)

# npkextpp is built either on its own (library + exe) or as a sub-project of
# another tool (e.g. nxscriptpp), which then links npk::core and, if it wants
# the multi-threaded extractor, npk::pipeline.
if(PROJECT_IS_TOP_LEVEL)
    set(_npk_top ON)
else()
    set(_npk_top OFF)
endif()

option(NPKEXTPP_BUILD_PIPELINE "Build the multi-threaded extraction pipeline (npk::pipeline)" ON)
option(NPKEXTPP_BUILD_APP      "Build the npkextpp executable" ${_npk_top})
option(NPKEXTPP_BUILD_TESTS    "Build the tests" OFF)
option(NPKEXTPP_FULLY_STATIC   "Linux: link fully statically (-static), including libc" OFF)

if(NPKEXTPP_BUILD_APP AND NOT NPKEXTPP_BUILD_PIPELINE)
    message(FATAL_ERROR "NPKEXTPP_BUILD_APP requires NPKEXTPP_BUILD_PIPELINE")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- One self-contained executable --------------------------------------------
# Only decided when we are the top-level project; a parent project chooses its
# own runtime and link settings.
if(_npk_top)
    set(BUILD_SHARED_LIBS OFF)

    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()

    # MSVC: static C/C++ runtime, no vcruntime DLLs next to the exe.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # Link-time optimisation for release builds when the toolchain supports it.
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _npk_ipo OUTPUT _npk_ipo_msg LANGUAGES C CXX)
    if(_npk_ipo)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    endif()
endif()

find_package(Threads REQUIRED)

add_subdirectory(third_party)
add_subdirectory(core)
if(NPKEXTPP_BUILD_PIPELINE)
    add_subdirectory(pipeline)
endif()
if(NPKEXTPP_BUILD_APP)
    add_subdirectory(app)
endif()
if(NPKEXTPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
