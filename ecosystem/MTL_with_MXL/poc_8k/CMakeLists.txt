cmake_minimum_required(VERSION 3.16)
project(poc_8k C)

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

# SVT-JPEG-XS (encoder + decoder in libSvtJpegxs)
find_library(SVTJPEGXS_LIB SvtJpegxs HINTS /usr/local/lib REQUIRED)
find_path(SVTJPEGXS_INCLUDE_DIR svt-jpegxs/SvtJpegxs.h HINTS /usr/local/include REQUIRED)

# pthreads
find_package(Threads REQUIRED)

# libjpeg (for thumbnail generation in receiver)
find_package(JPEG REQUIRED)

# ═══════════════════════════════════════════════════════
# Target 1: poc_8k_compositor
#   16× ST20 RX → 4×4 tile → SVT-JPEG-XS encode → ST22 TX
#   Links: MTL + SVT-JPEG-XS
# ═══════════════════════════════════════════════════════
add_executable(poc_8k_compositor
    src/compositor/main.c
)
target_include_directories(poc_8k_compositor PRIVATE
    ${MTL_INCLUDE_DIRS}
    ${SVTJPEGXS_INCLUDE_DIR}
)
target_link_directories(poc_8k_compositor PRIVATE
    ${MTL_LIBRARY_DIRS}
)
target_link_libraries(poc_8k_compositor PRIVATE
    ${MTL_LIBRARIES}
    ${SVTJPEGXS_LIB}
    JPEG::JPEG
    Threads::Threads
    rt
    m
)

# ═══════════════════════════════════════════════════════
# Target 2: poc_8k_compositor_tx
#   Reads codestreams from shm ring → ST22 TX on dedicated VF
#   Links: MTL only (no SVT-JPEG-XS, no MXL)
# ═══════════════════════════════════════════════════════
add_executable(poc_8k_compositor_tx
    src/compositor_tx/main.c
)
target_include_directories(poc_8k_compositor_tx PRIVATE
    ${MTL_INCLUDE_DIRS}
)
target_link_directories(poc_8k_compositor_tx PRIVATE
    ${MTL_LIBRARY_DIRS}
)
target_link_libraries(poc_8k_compositor_tx PRIVATE
    ${MTL_LIBRARIES}
    Threads::Threads
    rt
)

# ═══════════════════════════════════════════════════════
# Target 3: poc_8k_sender
#   ST22 RX (compressed, no decode) → MXL RDMA TX
#   Links: MTL + MXL
# ═══════════════════════════════════════════════════════
add_executable(poc_8k_sender
    src/sender/main.c
)
target_include_directories(poc_8k_sender PRIVATE
    ${MTL_INCLUDE_DIRS}
    ${MXL_INCLUDE_DIRS}
)
target_link_directories(poc_8k_sender PRIVATE
    ${MTL_LIBRARY_DIRS}
    ${MXL_LIBRARY_DIRS}
)
target_link_libraries(poc_8k_sender PRIVATE
    ${MTL_LIBRARIES}
    ${MXL_LIBRARIES}
    Threads::Threads
)

# ═══════════════════════════════════════════════════════
# Target 3: poc_8k_receiver
#   MXL RDMA RX → SVT-JPEG-XS decode → thumbnail → HTTP
#   Links: MXL + SVT-JPEG-XS + libjpeg
# ═══════════════════════════════════════════════════════
add_executable(poc_8k_receiver
    src/receiver/main.c
)
target_include_directories(poc_8k_receiver PRIVATE
    ${MXL_INCLUDE_DIRS}
    ${SVTJPEGXS_INCLUDE_DIR}
)
target_link_directories(poc_8k_receiver PRIVATE
    ${MXL_LIBRARY_DIRS}
)
target_link_libraries(poc_8k_receiver PRIVATE
    ${MXL_LIBRARIES}
    ${SVTJPEGXS_LIB}
    JPEG::JPEG
    Threads::Threads
    m
)

# ── Install ──
install(TARGETS poc_8k_compositor poc_8k_compositor_tx poc_8k_sender poc_8k_receiver
        RUNTIME DESTINATION bin)
