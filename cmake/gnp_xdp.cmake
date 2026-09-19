# cmake/gnp_xdp.cmake - find what the AF_XDP ingest needs; sets GNP_WITH_XDP.
#
#   libxdp   xsk_socket__create() and friends (they left libbpf in libbpf 1.0)
#   libbpf   loading the BPF object, maps, bpf_xdp_attach()
#   clang    compiling src/bpf/udp_redirect.bpf.c to BPF bytecode
#
# Anything missing leaves GNP_WITH_XDP OFF with a warning. gnp still builds,
# but exits at startup explaining that it has no packet source.

set(GNP_WITH_XDP OFF)

if(NOT GNP_ENABLE_XDP)
    message(STATUS "gnp: AF_XDP ingest disabled (GNP_ENABLE_XDP=OFF)")
    return()
endif()

set(_gnp_xdp_hint "Install them (Fedora: dnf install clang llvm libbpf-devel libxdp-devel kernel-headers) and reconfigure.")

find_package(PkgConfig QUIET)
if(NOT PkgConfig_FOUND)
    message(WARNING "gnp: pkg-config not found, cannot look for libbpf/libxdp. ${_gnp_xdp_hint}")
    return()
endif()

pkg_check_modules(GNP_LIBBPF IMPORTED_TARGET libbpf)
pkg_check_modules(GNP_LIBXDP IMPORTED_TARGET libxdp)
if(NOT GNP_LIBBPF_FOUND OR NOT GNP_LIBXDP_FOUND)
    message(WARNING "gnp: libbpf and/or libxdp not found; AF_XDP ingest disabled. ${_gnp_xdp_hint}")
    return()
endif()

find_program(GNP_BPF_CLANG NAMES clang)
if(NOT GNP_BPF_CLANG)
    message(WARNING "gnp: clang not found, cannot build the XDP program; AF_XDP ingest disabled. ${_gnp_xdp_hint}")
    return()
endif()

set(GNP_WITH_XDP ON)
message(STATUS "gnp: AF_XDP ingest enabled (libbpf ${GNP_LIBBPF_VERSION}, libxdp ${GNP_LIBXDP_VERSION}, ${GNP_BPF_CLANG})")
