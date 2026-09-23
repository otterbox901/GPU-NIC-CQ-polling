cmake_minimum_required(VERSION 3.24)
project(gpu_nic_poll LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(GNP_ENABLE_XDP    "Build the AF_XDP ingest when libbpf, libxdp and clang exist" ON)
option(GNP_ENABLE_GPUNETIO "Build the GPU-direct receive path when the DOCA SDK exists" ON)
option(GNP_BUILD_TESTING "Build testing/: the simulated NIC (gnp_sim) and unit tests" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(gnp_cuda)   # CUDA is required; fatal if absent
include(gnp_xdp)    # -> GNP_WITH_XDP
include(gnp_doca)   # -> GNP_WITH_GPUNETIO

# Warnings on our own C++ only: nvcc takes host flags through -Xcompiler, and
# the CUDA headers are not -Wextra clean. Not -Werror, so a newer compiler
# cannot break the build.
function(gnp_set_warnings target)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
endfunction()

# ---------------------------------------------------------------------------
# gnp_core: everything except main(), shared by gnp and the testing targets.
# ---------------------------------------------------------------------------
set(GNP_SOURCES
    src/host/setup.cpp
    src/host/metrics.cpp
    src/host/ring.cpp
    src/host/cli.cpp)

list(APPEND GNP_SOURCES src/gpu/poll_kernel.cu src/gpu/utils.cu)

if(GNP_WITH_XDP)
    list(APPEND GNP_SOURCES src/host/xdp_ingest.cpp)
endif()

add_library(gnp_core STATIC ${GNP_SOURCES})
gnp_set_warnings(gnp_core)

target_include_directories(gnp_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

find_package(Threads REQUIRED)
target_link_libraries(gnp_core PUBLIC Threads::Threads)

set_target_properties(gnp_core PROPERTIES CUDA_RUNTIME_LIBRARY Shared)
# Link cudart explicitly: `gnp` itself has no .cu sources, so it is linked by
# the C++ driver and cannot be relied on to pick the runtime up implicitly.
find_package(CUDAToolkit REQUIRED)
target_link_libraries(gnp_core PUBLIC CUDA::cudart)
# -lineinfo keeps the poll loop attributable in Nsight Compute.
target_compile_options(gnp_core PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-lineinfo>)

if(GNP_WITH_XDP)
    target_compile_definitions(gnp_core PUBLIC GNP_WITH_XDP=1)
    target_link_libraries(gnp_core PUBLIC PkgConfig::GNP_LIBXDP PkgConfig::GNP_LIBBPF)
endif()

# ---------------------------------------------------------------------------
# gnp: the driver executable
# ---------------------------------------------------------------------------
add_executable(gnp src/host/main.cpp)
target_link_libraries(gnp PRIVATE gnp_core)
gnp_set_warnings(gnp)

if(GNP_WITH_XDP)
    # The XDP program is BPF bytecode, loaded at run time from next to `gnp`.
    set(GNP_BPF_OBJ "${CMAKE_CURRENT_BINARY_DIR}/udp_redirect.bpf.o")
    set(GNP_BPF_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/bpf/udp_redirect.bpf.c")
    add_custom_command(
        OUTPUT "${GNP_BPF_OBJ}"
        COMMAND "${GNP_BPF_CLANG}" -O2 -g -target bpf -Wall
                -I/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}
                -c "${GNP_BPF_SRC}" -o "${GNP_BPF_OBJ}"
        DEPENDS "${GNP_BPF_SRC}"
        COMMENT "Compiling XDP program udp_redirect.bpf.o"
        VERBATIM)
    add_custom_target(gnp_bpf ALL DEPENDS "${GNP_BPF_OBJ}")
    add_dependencies(gnp gnp_bpf)
endif()

# ---------------------------------------------------------------------------
# gnp_gpunetio: the GPU-direct receive path. Shares only the small core with
# `gnp` (config, CLI, metrics, the on_packet hook) - no ring, no ingest thread.
# ---------------------------------------------------------------------------
if(GNP_WITH_GPUNETIO)
    add_executable(gnp_gpunetio
        src/gpunetio/main_gpunetio.cpp
        src/gpunetio/setup.cpp
        src/gpunetio/receive_kernel.cu)
    target_include_directories(gnp_gpunetio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/gpunetio")
    target_link_libraries(gnp_gpunetio PRIVATE
        gnp_core
        PkgConfig::GNP_DOCA_GPUNETIO
        PkgConfig::GNP_DOCA_ETH
        PkgConfig::GNP_DOCA_FLOW
        PkgConfig::GNP_DOCA_COMMON)
    set_target_properties(gnp_gpunetio PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    gnp_set_warnings(gnp_gpunetio)
    # DOCA marks its experimental API with a deprecation attribute, so every
    # GPUNetIO call would warn. Silencing it here keeps the rest of -Wall
    # -Wextra meaningful on this target.
    target_compile_options(gnp_gpunetio PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-declarations>)
endif()

# ---------------------------------------------------------------------------
# testing/ (opt-in): simulated NIC, unit tests, benchmark tooling
# ---------------------------------------------------------------------------
if(GNP_BUILD_TESTING)
    enable_testing()
    add_subdirectory(testing)
endif()

message(STATUS "gnp: xdp=${GNP_WITH_XDP} gpunetio=${GNP_WITH_GPUNETIO} testing=${GNP_BUILD_TESTING}")
