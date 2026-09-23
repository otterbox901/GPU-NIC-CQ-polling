# cmake/gnp_cuda.cmake
#
# Finds a working nvcc + host compiler pair and enables the CUDA language.
# CUDA is required: every receive path in this project polls from an SM, so
# there is no build without it. Failure here is fatal, not a fallback.
#
# Finding a *usable* nvcc takes more than a bare check_language(CUDA):
#  * nvcc rejects host compilers newer than it knows, and distro GCC routinely
#    runs ahead of CUDA. check_language() probes in a separate configure that
#    does not receive CMAKE_CUDA_HOST_COMPILER, so it always tried the default
#    g++ and failed. The probe does honour the CUDAHOSTCXX environment variable,
#    so each candidate host compiler is passed that way.
#  * nvcc is often installed outside PATH (/usr/local/cuda/bin).
#  * A failed probe is cached as NOTFOUND and never retried. A verified result
#    is recorded separately, so anything unverified is probed again on the next
#    configure and you never need to delete a build directory to recover.

include(CheckLanguage)

# Fast path: a previous configure already proved this compiler pair works.
set(_gnp_pair "${CMAKE_CUDA_COMPILER}|${CMAKE_CUDA_HOST_COMPILER}")
if(NOT (CMAKE_CUDA_COMPILER AND GNP_CUDA_VERIFIED_PAIR STREQUAL _gnp_pair))
    # --- locate nvcc: explicit setting, then CUDACXX, then PATH and usual prefixes
    if(CMAKE_CUDA_COMPILER)
        set(_gnp_nvcc "${CMAKE_CUDA_COMPILER}")
    elseif(DEFINED ENV{CUDACXX})
        set(_gnp_nvcc "$ENV{CUDACXX}")
    else()
        find_program(_gnp_nvcc NAMES nvcc
                     PATHS ENV CUDA_PATH ENV CUDA_HOME /usr/local/cuda /opt/cuda
                     PATH_SUFFIXES bin
                     NO_CACHE)
    endif()

    if(NOT _gnp_nvcc OR NOT EXISTS "${_gnp_nvcc}")
        unset(CMAKE_CUDA_COMPILER CACHE)
        message(FATAL_ERROR
            "gnp: no CUDA compiler found, and CUDA is required.\n"
            "     Install the CUDA Toolkit, or point CUDACXX / CMAKE_CUDA_COMPILER\n"
            "     at nvcc, and reconfigure.")
    endif()

    # --- host compiler candidates. An explicit choice is the only candidate;
    # otherwise the default g++ first, then versioned g++ newest to oldest.
    if(CMAKE_CUDA_HOST_COMPILER)
        set(_gnp_hosts "${CMAKE_CUDA_HOST_COMPILER}")
    elseif(DEFINED ENV{CUDAHOSTCXX})
        set(_gnp_hosts "$ENV{CUDAHOSTCXX}")
    else()
        set(_gnp_hosts "")
        foreach(_ver RANGE 9 20)
            find_program(_gnp_cxx NAMES "g++-${_ver}" "g++${_ver}" NO_CACHE)
            if(_gnp_cxx)
                list(PREPEND _gnp_hosts "${_gnp_cxx}")
            endif()
            unset(_gnp_cxx)
        endforeach()
        list(PREPEND _gnp_hosts "default")
    endif()

    # --- probe each pairing until nvcc accepts one
    set(ENV{CUDACXX} "${_gnp_nvcc}")
    set(_gnp_ok OFF)
    foreach(_host IN LISTS _gnp_hosts)
        unset(CMAKE_CUDA_COMPILER CACHE)
        unset(CMAKE_CUDA_COMPILER)
        if(_host STREQUAL "default")
            unset(ENV{CUDAHOSTCXX})
            message(STATUS "gnp: probing ${_gnp_nvcc} with the default host compiler")
        else()
            set(ENV{CUDAHOSTCXX} "${_host}")
            message(STATUS "gnp: probing ${_gnp_nvcc} with host compiler ${_host}")
        endif()

        check_language(CUDA)

        if(CMAKE_CUDA_COMPILER)
            if(NOT _host STREQUAL "default")
                set(CMAKE_CUDA_HOST_COMPILER "${_host}" CACHE FILEPATH "nvcc host compiler" FORCE)
            endif()
            set(_gnp_ok ON)
            break()
        endif()
    endforeach()

    if(NOT _gnp_ok)
        unset(GNP_CUDA_VERIFIED_PAIR CACHE)
        string(REPLACE ";" ", " _gnp_tried "${_gnp_hosts}")
        message(FATAL_ERROR
            "gnp: found ${_gnp_nvcc}, but it accepted none of these host compilers:\n"
            "     ${_gnp_tried}\n"
            "     Install a g++ this CUDA release supports (e.g. sudo dnf install gcc14-c++)\n"
            "     or pass -DCMAKE_CUDA_HOST_COMPILER=/path/to/g++ and reconfigure.\n"
            "     Details: CMakeFiles/CMakeConfigureLog.yaml")
    endif()

    set(GNP_CUDA_VERIFIED_PAIR "${CMAKE_CUDA_COMPILER}|${CMAKE_CUDA_HOST_COMPILER}" CACHE INTERNAL
        "nvcc|host compiler pair that passed the gnp probe")
endif()

if(CMAKE_CUDA_HOST_COMPILER)
    message(STATUS "gnp: using ${CMAKE_CUDA_HOST_COMPILER} as the nvcc host compiler")
endif()

# Must be set before enable_language(CUDA): that is when the default is captured.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES native)
endif()

set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

enable_language(CUDA)

message(STATUS "gnp: CUDA enabled (arch=${CMAKE_CUDA_ARCHITECTURES})")
