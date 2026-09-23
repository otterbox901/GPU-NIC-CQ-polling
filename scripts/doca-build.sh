#!/usr/bin/env bash
# Build the GPUNetIO path inside NVIDIA's DOCA container.
#
# DOCA has no Fedora build, and this machine has no ConnectX NIC anyway, so the
# container exists to compile and link against genuine DOCA headers - not to
# run anything. It ships DOCA 3.5 and CUDA 13 already, so no Dockerfile of our
# own is needed.
#
#   scripts/doca-build.sh            configure + build
#   scripts/doca-build.sh --shell    drop into the container instead
#
# The build tree lands in build/gpunetio/, owned by your user via --userns.

set -euo pipefail

IMAGE="${GNP_DOCA_IMAGE:-nvcr.io/nvidia/doca/doca:devel-cuda13.0.0-3.5.0-devel-host}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! podman image exists "$IMAGE" 2>/dev/null; then
    echo "[gnp] pulling $IMAGE (~15 GB, once)" >&2
    podman pull "$IMAGE"
fi

# Only ask for a TTY when we have one, so piping the output stays quiet.
TTY_FLAGS=()
[[ -t 0 ]] && TTY_FLAGS=(-it)

run() {
    podman run --rm "${TTY_FLAGS[@]}" \
        --userns=keep-id \
        -v "$ROOT":/src:Z \
        -w /src \
        "$IMAGE" "$@"
}

if [[ "${1:-}" == "--shell" ]]; then
    exec run bash
fi

# nvcc cannot probe a GPU here (the container has no driver), so the preset
# pins CMAKE_CUDA_ARCHITECTURES instead of using `native`.
run bash -lc "cmake --preset gpunetio $* && cmake --build --preset gpunetio -j\$(nproc)"
