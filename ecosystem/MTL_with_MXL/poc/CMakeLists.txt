cmake_minimum_required(VERSION 3.16)
project(mtl_mxl_poc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Build type defaults to RelWithDebInfo ──
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ── Compiler flags ──
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_compile_definitions(_GNU_SOURCE)

# ── Dependencies ──
# MTL (Media Transport Library) — installed via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(MTL REQUIRED mtl)
# MTL headers live under <prefix>/include/mtl/ but code uses #include <mtl_api.h>
list(TRANSFORM MTL_INCLUDE_DIRS APPEND "/mtl" OUTPUT_VARIABLE MTL_INCLUDE_DIRS_MTL)
list(APPEND MTL_INCLUDE_DIRS ${MTL_INCLUDE_DIRS_MTL})

# MXL — headers and libs from the MXL build tree
# Set MXL_ROOT to the MXL install/build prefix, e.g.:
#   cmake -DMXL_ROOT=/path/to/mxl-feature-fabrics-jonas-protocols/build
set(MXL_ROOT "" CACHE PATH "Path to MXL install or build prefix")

if(MXL_ROOT)
    # Resolve the MXL source root (two levels up from build/Linux-GCC-Release)
    get_filename_component(MXL_SRC_ROOT "${MXL_ROOT}/../.." ABSOLUTE)
    set(MXL_INCLUDE_DIRS "${MXL_SRC_ROOT}/lib/include" "${MXL_SRC_ROOT}/lib/fabrics/include")
    set(MXL_LIBRARY_DIRS "${MXL_ROOT}/lib" "${MXL_ROOT}/lib/fabrics/ofi" "${MXL_ROOT}/lib/internal")
    find_library(MXL_LIB mxl HINTS "${MXL_ROOT}/lib" REQUIRED)
    find_library(MXL_FABRICS_LIB mxl-fabrics HINTS "${MXL_ROOT}/lib" "${MXL_ROOT}/lib/fabrics/ofi" REQUIRED)
    set(MXL_LIBRARIES ${MXL_LIB} ${MXL_FABRICS_LIB})
else()
    # Fallback: assume MXL is installed system-wide or use pkg-config
    pkg_check_modules(MXL REQUIRED mxl)
    pkg_check_modules(MXL_FABRICS REQUIRED mxl-fabrics)
    list(APPEND MXL_LIBRARIES ${MXL_FABRICS_LIBRARIES})
endif()

# pthreads
find_package(Threads REQUIRED)

# libcurl (for InfluxDB push)
find_package(CURL REQUIRED)

# libjpeg (for thumbnail generation)
find_package(JPEG REQUIRED)

# ── Common helper library ──
add_library(poc_common STATIC
    src/common/frame_queue.c
    src/common/v210_utils.c
    src/common/json_utils.c
    src/common/stats.c
    src/common/influxdb.c
    src/common/thumbnail.c
    src/common/thumbnail_tx.c
)
target_include_directories(poc_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${MTL_INCLUDE_DIRS}
    ${MXL_INCLUDE_DIRS}
)
target_link_libraries(poc_common PUBLIC
    ${MTL_LIBRARIES}
    ${MXL_LIBRARIES}
    Threads::Threads
    CURL::libcurl
    JPEG::JPEG
)

# ── Host A: mtl_to_mxl_sender ──
add_executable(mtl_to_mxl_sender
    src/sender/main.c
    src/sender/mtl_rx.c
    src/sender/mxl_bridge.c
    src/sender/fabrics_initiator.c
)
target_include_directories(mtl_to_mxl_sender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${MTL_INCLUDE_DIRS}
    ${MXL_INCLUDE_DIRS}
)
target_link_directories(mtl_to_mxl_sender PRIVATE
    ${MTL_LIBRARY_DIRS}
    ${MXL_LIBRARY_DIRS}
)
target_link_libraries(mtl_to_mxl_sender PRIVATE
    poc_common
    ${MTL_LIBRARIES}
    ${MXL_LIBRARIES}
    Threads::Threads
)

# ── Host B: mxl_sink_receiver ──
add_executable(mxl_sink_receiver
    src/receiver/main.c
    src/receiver/mxl_sink.c
    src/receiver/fabrics_target.c
    src/receiver/consumer.c
)
target_include_directories(mxl_sink_receiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${MTL_INCLUDE_DIRS}
    ${MXL_INCLUDE_DIRS}
)
target_link_directories(mxl_sink_receiver PRIVATE
    ${MTL_LIBRARY_DIRS}
    ${MXL_LIBRARY_DIRS}
)
target_link_libraries(mxl_sink_receiver PRIVATE
    poc_common
    ${MXL_LIBRARIES}
    Threads::Threads
)

# ── Synthetic ST2110-20 TX source (test pattern generator) ──
add_executable(synthetic_st20_tx
    src/synthetic_tx/main.c
)
target_include_directories(synthetic_st20_tx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${MTL_INCLUDE_DIRS}
)
target_link_directories(synthetic_st20_tx PRIVATE
    ${MTL_LIBRARY_DIRS}
)
target_link_libraries(synthetic_st20_tx PRIVATE
    poc_common
    ${MTL_LIBRARIES}
    Threads::Threads
)

# ── Install ──
install(TARGETS mtl_to_mxl_sender mxl_sink_receiver synthetic_st20_tx RUNTIME DESTINATION bin)
