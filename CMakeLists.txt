cmake_minimum_required(VERSION 3.18)

project(
    rockchip_yolo_cpp
    VERSION 0.3.0
    DESCRIPTION "C++ YOLO acceleration demo for Rockchip RK3576 and RK3588"
    LANGUAGES CXX)

# Compiler settings and build options.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Build libraries alone when included by another project.
set(NVR_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(NVR_STANDALONE ON)
endif()
option(NVR_BUILD_ENGINE "Build the Rockchip hardware pipeline" ON)
option(NVR_BUILD_APPS "Build the command-line and worker applications" ${NVR_STANDALONE})
option(NVR_BUILD_WEB "Build the demo web server" ON)

# Portable postprocessing has no Rockchip, FFmpeg, or JSON dependency.
add_library(
    nvr-postprocess STATIC
    src/engine/geometry.cpp
    src/engine/postprocess.cpp
    src/engine/task_model.cpp
    src/engine/task_decode.cpp
    src/engine/task_dense.cpp)
add_library(nvr::postprocess ALIAS nvr-postprocess)
target_include_directories(nvr-postprocess PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(nvr-postprocess PUBLIC cxx_std_17)
target_compile_options(nvr-postprocess PRIVATE -Wall -Wextra -Wconversion -Wno-sign-conversion)

if(NOT NVR_BUILD_ENGINE)
    return()
endif()

# Shared dependencies.
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

pkg_check_modules(
    AV
    REQUIRED
    IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale)

# Rockchip SDK headers and libraries.
set(ROCKCHIP_SDK
    ""
    CACHE PATH "SDK containing rga/ and rknn/")
set(MPP_ROOT
    ""
    CACHE PATH "MPP install prefix")

find_path(
    RGA_INCLUDE im2d.hpp
    HINTS ${ROCKCHIP_SDK}/rga/include
    PATH_SUFFIXES rga REQUIRED)
find_library(RGA_LIB rga HINTS ${ROCKCHIP_SDK}/rga/lib REQUIRED)

find_path(
    RKNN_INCLUDE rknn_api.h
    HINTS ${ROCKCHIP_SDK}/rknn/include
    PATH_SUFFIXES rknn REQUIRED)
find_library(RKNN_LIB rknnrt HINTS ${ROCKCHIP_SDK}/rknn/lib REQUIRED)

find_path(
    MPP_INCLUDE rk_mpi.h
    HINTS ${MPP_ROOT}/include
    PATH_SUFFIXES rockchip REQUIRED)
find_library(MPP_LIB rockchip_mpp HINTS ${MPP_ROOT}/lib REQUIRED)

add_library(nvr-task-json STATIC src/engine/task_json.cpp)
target_include_directories(nvr-task-json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nvr-task-json PUBLIC nvr::postprocess nlohmann_json::nlohmann_json)

# Hardware pipeline shared by the CLI and channel worker.
add_library(
    nvr-engine STATIC
    src/engine/frame.cpp
    src/engine/frame_queue.cpp
    src/engine/dma_buffer.cpp
    src/engine/decoder.cpp
    src/engine/recorder.cpp
    src/engine/preprocess.cpp
    src/engine/native_output.cpp
    src/engine/detector.cpp
    src/engine/detector_pool.cpp
    src/engine/jpeg_preview.cpp
    src/engine/mp4_stream.cpp)
add_library(nvr::engine ALIAS nvr-engine)

target_include_directories(nvr-engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(nvr-engine SYSTEM PUBLIC ${RGA_INCLUDE} ${RKNN_INCLUDE} ${MPP_INCLUDE})
target_compile_options(nvr-engine PRIVATE -Wall -Wextra -Wconversion -Wno-sign-conversion)
target_link_libraries(nvr-engine PUBLIC nvr-task-json nvr::postprocess PkgConfig::AV Threads::Threads
                                       nlohmann_json::nlohmann_json ${RGA_LIB} ${RKNN_LIB} ${MPP_LIB})

if(NOT NVR_BUILD_APPS)
    return()
endif()

# Metadata belongs to application setup, outside the hardware inference library.
find_package(yaml-cpp CONFIG REQUIRED)
add_library(nvr-metadata STATIC src/model/metadata.cpp)
target_include_directories(nvr-metadata PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nvr-metadata PUBLIC nvr::postprocess nlohmann_json::nlohmann_json yaml-cpp)
# JSON adapters are also used by the server, which does not link hardware libraries.
target_link_libraries(nvr-metadata PUBLIC nvr-task-json)

# Standalone command-line application.
add_executable(rk-nvr src/cli.cpp)
target_link_libraries(rk-nvr PRIVATE nvr-engine nvr-metadata)

install(TARGETS rk-nvr RUNTIME DESTINATION bin)

# Per-channel worker and browser video transport.
pkg_check_modules(AVDEVICE REQUIRED IMPORTED_TARGET libavdevice)
add_executable(rk-nvr-worker src/worker/main.cpp src/worker/control.cpp src/worker/runtime.cpp)
target_link_libraries(rk-nvr-worker PRIVATE nvr-engine nvr-metadata PkgConfig::AVDEVICE)

# Model inspection runs in a separate process from the server.
add_executable(rk-model-info src/model_probe.cpp)
target_include_directories(rk-model-info SYSTEM PRIVATE ${RKNN_INCLUDE})
target_link_libraries(rk-model-info PRIVATE nlohmann_json::nlohmann_json ${RKNN_LIB})

# Optional HTTP/WebSocket server and warehouse services.
if(NVR_BUILD_WEB)
    find_package(Drogon CONFIG REQUIRED)
    find_package(yaml-cpp CONFIG REQUIRED)
    find_package(SQLite3 REQUIRED)
    find_package(LibArchive REQUIRED)

    find_library(
        NVR_SQLITE_LIB sqlite3
        HINTS ${CMAKE_PREFIX_PATH}
        PATH_SUFFIXES lib lib/aarch64-linux-gnu REQUIRED)

    add_executable(
        rk-nvr-server
        src/server/main.cpp
        src/server/api/routes.cpp
        src/server/api/sockets.cpp
        src/server/api/live.cpp
        src/server/storage/store.cpp
        src/server/services/common.cpp
        src/server/services/devices.cpp
        src/server/services/application.cpp
        src/server/services/supervisor.cpp
        src/server/services/warehouse.cpp
        src/server/services/model_archive.cpp
        src/server/services/asset_validation.cpp)

    target_include_directories(rk-nvr-server PRIVATE src ${SQLITE3_INCLUDE_DIRS}
                                                     ${SQLite3_INCLUDE_DIRS})
    target_compile_options(rk-nvr-server PRIVATE -Wall -Wextra)
    target_link_libraries(rk-nvr-server PRIVATE nvr-metadata Drogon::Drogon yaml-cpp ${NVR_SQLITE_LIB}
                                                LibArchive::LibArchive nlohmann_json::nlohmann_json)
endif()
