#pragma once
//
// src/gpunetio/kernel.h - the one symbol setup.cpp needs from the .cu.
//
// Kept private to src/gpunetio/ so no public header mentions CUDA or DOCA.
//

#include <cuda_runtime.h>

#include "gnp/metrics.hpp"

struct doca_gpu_eth_rxq;

/// Launch the persistent receive kernel: one block per queue, `rxqs` and
/// `stats` each holding `n_queues` entries in GPU-visible memory. Returns 0 on
/// success. The kernel runs until `*exit_flag` becomes non-zero.
extern "C" int gnp_launch_receive_kernel(cudaStream_t stream, struct doca_gpu_eth_rxq** rxqs,
                                         uint32_t n_queues, uint32_t* exit_flag,
                                         gnp::PollStats* stats);
