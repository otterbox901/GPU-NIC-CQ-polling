# cmake/gnp_doca.cmake - find DOCA GPUNetIO; sets GNP_WITH_GPUNETIO.
#
# The GPUNetIO path needs the DOCA SDK, which ships only for Ubuntu, RHEL/Rocky,
# Debian, SLES, Oracle, Azure and Amazon Linux - not Fedora. Build it inside
# NVIDIA's DOCA container instead: scripts/doca-build.sh.
#
#   doca-gpunetio  GPU handle, GPU memory, the device-side receive headers
#   doca-eth       doca_eth_rxq: the receive queue whose buffer lives in VRAM
#   doca-flow      hardware steering of one UDP port into that queue
#   doca-common    doca_dev, doca_mmap, doca_ctx, error strings
#
# Missing pieces leave GNP_WITH_GPUNETIO OFF and skip the target, exactly as
# gnp_xdp.cmake does for the AF_XDP path.

set(GNP_WITH_GPUNETIO OFF)

if(NOT GNP_ENABLE_GPUNETIO)
    message(STATUS "gnp: GPUNetIO path disabled (GNP_ENABLE_GPUNETIO=OFF)")
    return()
endif()

set(_gnp_doca_hint
    "     The GPUNetIO path needs the DOCA SDK. It has no Fedora build; run\n"
    "     scripts/doca-build.sh to build it inside NVIDIA's DOCA container.")

find_package(PkgConfig QUIET)
if(NOT PkgConfig_FOUND)
    message(WARNING "gnp: pkg-config not found, cannot look for DOCA.\n" ${_gnp_doca_hint})
    return()
endif()

pkg_check_modules(GNP_DOCA_GPUNETIO IMPORTED_TARGET doca-gpunetio)
pkg_check_modules(GNP_DOCA_ETH      IMPORTED_TARGET doca-eth)
pkg_check_modules(GNP_DOCA_FLOW     IMPORTED_TARGET doca-flow)
pkg_check_modules(GNP_DOCA_COMMON   IMPORTED_TARGET doca-common)

if(NOT (GNP_DOCA_GPUNETIO_FOUND AND GNP_DOCA_ETH_FOUND
        AND GNP_DOCA_FLOW_FOUND AND GNP_DOCA_COMMON_FOUND))
    message(WARNING "gnp: DOCA not found; the GPUNetIO path is not built.\n" ${_gnp_doca_hint})
    return()
endif()

set(GNP_WITH_GPUNETIO ON)
message(STATUS "gnp: GPUNetIO path enabled (DOCA ${GNP_DOCA_GPUNETIO_VERSION})")
