cmake_minimum_required(VERSION 3.16)
project(mtl_mxl_poc_14 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Build type defaults to RelWithDebInfo ──
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ── Compiler flags ──
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
add_compile_definitions(_GNU_SOURCE)

# ── Path to original poc source (reuse headers + common code) ──
set(POC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../poc/src")
set(POC_INC "${POC_SRC}/include")

# ── Dependencies ──
# MTL (Media Transport Library) — installed via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(MTL REQUIRED mtl)
list(TRANSFORM MTL_INCLUDE_DIRS APPEND "/mtl" OUTPUT_VARIABLE MTL_INCLUDE_DIRS_MTL)
list(APPEND MTL_INCLUDE_DIRS ${MTL_INCLUDE_DIRS_MTL})

# MXL — headers and libs from the MXL build tree
set(MXL_ROOT "" CACHE PATH "Path to MXL install or build prefix")

if(MXL_ROOT)
    get_filename_component(MXL_SRC_ROOT "${MXL_ROOT}/../.." ABSOLUTE)
    set(MXL_INCLUDE_DIRS "${MXL_SRC_ROOT}/lib/include" "${MXL_SRC_ROOT}/lib/fabrics/include")
    set(MXL_LIBRARY_DIRS "${MXL_ROOT}/lib" "${MXL_ROOT}/lib/fabrics/ofi" "${MXL_ROOT}/lib/internal")
    find_library(MXL_LIB mxl HINTS "${MXL_ROOT}/lib" REQUIRED)
    find_library(MXL_FABRICS_LIB mxl-fabrics HINTS "${MXL_ROOT}/lib" "${MXL_ROOT}/lib/fabrics/ofi" REQUIRED)
    set(MXL_LIBRARIES ${MXL_LIB} ${MXL_FABRICS_LIB})
else()
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

# cJSON (JSON parser)
pkg_check_modules(CJSON REQUIRED libcjson)

# ── Common library (reuses original poc/src/common + new poc_14-specific) ──
add_library(poc_14_common STATIC
    # Original poc common sources (reused as-is)
    ${POC_SRC}/common/frame_queue.c
    ${POC_SRC}/common/v210_utils.c
    ${POC_SRC}/common/json_utils.c
    ${POC_SRC}/common/stats.c
    ${POC_SRC}/common/influxdb.c
    ${POC_SRC}/common/thumbnail.c
    # New poc_14-specific sources
    src/common_14/config_parser.c
    src/common_14/influxdb_14.c
    src/common_14/thumbnail_14.c
)
target_compile_definitions(poc_14_common PUBLIC POC_14)
target_include_directories(poc_14_common PUBLIC
    ${POC_INC}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common_14
    ${CJSON_INCLUDE_DIRS}
    ${MTL_INCLUDE_DIRS}
    ${MXL_INCLUDE_DIRS}
)
target_link_libraries(poc_14_common PUBLIC
    ${MTL_LIBRARIES}
    ${MXL_LIBRARIES}
    ${CJSON_LIBRARIES}
    Threads::Threads
    CURL::libcurl
    JPEG::JPEG
)

# ── Synthetic ST2110-20 TX (14-stream pattern generator) ──
add_executable(synthetic_st20_tx_14
    src/synthetic_tx_14/main.c
)
target_include_directories(synthetic_st20_tx_14 PRIVATE
    ${POC_INC}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common_14
    ${CJSON_INCLUDE_DIRS}
    ${MTL_INCLUDE_DIRS}
)
target_link_directories(synthetic_st20_tx_14 PRIVATE
    ${MTL_LIBRARY_DIRS}
)
target_link_libraries(synthetic_st20_tx_14 PRIVATE
    poc_14_common
    ${MTL_LIBRARIES}
    Threads::Threads
)

# ── Host A: mtl_to_mxl_sender_14 (14-stream sender) ──
add_executable(mtl_to_mxl_sender_14
    src/sender_14/main.c
    # Reuse original poc sender components (compiled, not modified)
    ${POC_SRC}/sender/mxl_bridge.c
    ${POC_SRC}/sender/fabrics_initiator.c
)
target_include_directories(mtl_to_mxl_sender_14 PRIVATE
    ${POC_INC}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common_14
    ${CJSON_INCLUDE_DIRS}
    ${MTL_INCLUDE_DIRS}
    ${MXL_INCLUDE_DIRS}
)
target_link_directories(mtl_to_mxl_sender_14 PRIVATE
    ${MTL_LIBRARY_DIRS}
    ${MXL_LIBRARY_DIRS}
)
target_link_libraries(mtl_to_mxl_sender_14 PRIVATE
    poc_14_common
    ${MTL_LIBRARIES}
    ${MXL_LIBRARIES}
    Threads::Threads
)

# ── Host B: mxl_sink_receiver_14 (14-stream receiver) ──
add_executable(mxl_sink_receiver_14
    src/receiver_14/main.c
    # Reuse original poc receiver components (compiled, not modified)
    ${POC_SRC}/receiver/mxl_sink.c
    ${POC_SRC}/receiver/fabrics_target.c
    ${POC_SRC}/receiver/consumer.c
)
target_include_directories(mxl_sink_receiver_14 PRIVATE
    ${POC_INC}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common_14
    ${CJSON_INCLUDE_DIRS}
    ${MTL_INCLUDE_DIRS}
    ${MXL_INCLUDE_DIRS}
)
target_link_directories(mxl_sink_receiver_14 PRIVATE
    ${MTL_LIBRARY_DIRS}
    ${MXL_LIBRARY_DIRS}
)
target_link_libraries(mxl_sink_receiver_14 PRIVATE
    poc_14_common
    ${MXL_LIBRARIES}
    Threads::Threads
)

# ── Install ──
install(TARGETS synthetic_st20_tx_14 mtl_to_mxl_sender_14 mxl_sink_receiver_14
        RUNTIME DESTINATION bin)
