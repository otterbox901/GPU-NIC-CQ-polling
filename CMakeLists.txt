cmake_minimum_required(VERSION 3.24)
project(gpu_nic_poll LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(GNP_ENABLE_CUDA   "Use the CUDA backend when a CUDA compiler exists" ON)
option(GNP_ENABLE_XDP    "Build the AF_XDP ingest when libbpf, libxdp and clang exist" ON)
option(GNP_BUILD_TESTING "Build testing/: the simulated NIC (gnp_sim) and unit tests" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(gnp_cuda)   # -> GNP_WITH_CUDA
include(gnp_xdp)    # -> GNP_WITH_XDP

# ---------------------------------------------------------------------------
# gnp_core: everything except main(), shared by gnp and the testing targets.
# ---------------------------------------------------------------------------
set(GNP_SOURCES
    src/host/setup.cpp
    src/host/metrics.cpp
    src/host/ring.cpp
    src/host/cli.cpp)

if(GNP_WITH_CUDA)
    list(APPEND GNP_SOURCES src/gpu/poll_kernel.cu src/gpu/utils.cu)
else()
    list(APPEND GNP_SOURCES src/gpu/poll_cpu.cpp)
endif()

if(GNP_WITH_XDP)
    list(APPEND GNP_SOURCES src/host/xdp_ingest.cpp)
endif()

add_library(gnp_core STATIC ${GNP_SOURCES})

target_include_directories(gnp_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

find_package(Threads REQUIRED)
target_link_libraries(gnp_core PUBLIC Threads::Threads)

if(GNP_WITH_CUDA)
    target_compile_definitions(gnp_core PUBLIC GNP_WITH_CUDA=1)
    set_target_properties(gnp_core PROPERTIES CUDA_RUNTIME_LIBRARY Shared)
    # Link cudart explicitly: `gnp` itself has no .cu sources, so it is linked by
    # the C++ driver and cannot be relied on to pick the runtime up implicitly.
    find_package(CUDAToolkit REQUIRED)
    target_link_libraries(gnp_core PUBLIC CUDA::cudart)
    # -lineinfo keeps the poll loop attributable in Nsight Compute.
    target_compile_options(gnp_core PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-lineinfo>)
endif()

if(GNP_WITH_XDP)
    target_compile_definitions(gnp_core PUBLIC GNP_WITH_XDP=1)
    target_link_libraries(gnp_core PUBLIC PkgConfig::GNP_LIBXDP PkgConfig::GNP_LIBBPF)
endif()

# ---------------------------------------------------------------------------
# gnp: the driver executable
# ---------------------------------------------------------------------------
add_executable(gnp src/host/main.cpp)
target_link_libraries(gnp PRIVATE gnp_core)

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
# testing/ (opt-in): simulated NIC, unit tests, benchmark tooling
# ---------------------------------------------------------------------------
if(GNP_BUILD_TESTING)
    enable_testing()
    add_subdirectory(testing)
endif()

message(STATUS "gnp: cuda=${GNP_WITH_CUDA} xdp=${GNP_WITH_XDP} testing=${GNP_BUILD_TESTING}")
