//
// src/gpunetio/setup.cpp - bring up the NIC -> VRAM receive path.
//
// Setup order per queue, and every step matters:
//   1. doca_eth_rxq_create + CYCLIC type: a receive queue whose packet buffer
//      we supply.
//   2. doca_gpu_mem_alloc(DOCA_GPU_MEM_TYPE_GPU): that buffer, in VRAM.
//   3. doca_gpu_dmabuf_fd + doca_mmap_set_dmabuf_memrange: hand the NIC a
//      dmabuf handle to GPU memory so it can DMA into it. Falls back to
//      doca_mmap_set_memrange, which needs the legacy nvidia-peermem module.
//   4. doca_ctx_set_datapath_on_gpu: the queue is driven from a CUDA kernel
//      (GDAKI), not from the CPU. This is what removes the host from the data
//      path entirely.
//   5. DOCA Flow: a root pipe matching one UDP destination port, forwarding to
//      an RSS pipe spread over the queues. This replaces the eBPF/XDP program
//      of the AF_XDP path - the match runs in NIC hardware.
//

#include "gnp/gpunetio.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include <netinet/in.h>
#include <unistd.h>

#include <cuda_runtime.h>

#include <doca_dev.h>
#include <doca_error.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_gpu_data_path.h>
#include <doca_flow.h>
#include <doca_gpunetio.h>
#include <doca_mmap.h>

#include "kernel.h"

namespace gnp {
namespace {

// Sized to match NVIDIA's simple-receive sample; big enough that the NIC does
// not wrap the cyclic buffer while a batch is being processed.
constexpr uint32_t kMaxPktNum = 16384;
constexpr uint32_t kMaxPktSize = 2048;
constexpr uint32_t kFlowCounters = 64;

/// Log a failed DOCA call and return false, so callers read as a flat chain.
bool fail(const char* what, doca_error_t err) {
    std::fprintf(stderr, "[gnp] %s: %s\n", what, doca_error_get_descr(err));
    return false;
}

size_t page_size() { return static_cast<size_t>(sysconf(_SC_PAGESIZE)); }

size_t align_up(size_t v, size_t a) { return ((v + a - 1) / a) * a; }

/// Open the NIC whose PCI address matches. DOCA has no open-by-address call;
/// its samples carry a helper like this one, so we keep our own rather than
/// depend on sample sources that are not part of the SDK.
bool open_nic_by_pci(const char* pci_addr, doca_dev** out) {
    doca_devinfo** list = nullptr;
    uint32_t count = 0;

    doca_error_t r = doca_devinfo_create_list(&list, &count);
    if (r != DOCA_SUCCESS) return fail("doca_devinfo_create_list", r);

    for (uint32_t i = 0; i < count; ++i) {
        uint8_t same = 0;
        if (doca_devinfo_is_equal_pci_addr(list[i], pci_addr, &same) != DOCA_SUCCESS || !same)
            continue;
        r = doca_dev_open(list[i], out);
        doca_devinfo_destroy_list(list);
        return r == DOCA_SUCCESS ? true : fail("doca_dev_open", r);
    }

    doca_devinfo_destroy_list(list);
    std::fprintf(stderr,
                 "[gnp] no DOCA device at PCI %s. The GPU-direct path needs a ConnectX-6 Dx or "
                 "newer, or a BlueField-2/3; list candidates with `doca_caps` or `ibv_devinfo`.\n",
                 pci_addr);
    return false;
}

/// One receive queue: a DOCA queue whose packet buffer lives in GPU memory.
struct Queue {
    doca_eth_rxq*     rxq_cpu = nullptr;
    doca_gpu_eth_rxq* rxq_gpu = nullptr;
    doca_ctx*         ctx = nullptr;
    doca_mmap*        mmap = nullptr;
    void*             pkt_addr = nullptr;  ///< the VRAM the NIC writes into
    int               dmabuf_fd = -1;
};

}  // namespace

struct GpunetioSession {
    doca_dev*        ddev = nullptr;
    doca_gpu*        gpu = nullptr;
    doca_flow_port*  port = nullptr;
    doca_flow_pipe*  rxq_pipe = nullptr;
    doca_flow_pipe*  root_pipe = nullptr;
    cudaStream_t     stream = nullptr;
    bool             flow_inited = false;

    std::vector<Queue> queues;

    // GPU-visible control, written by the host, read by the kernel.
    uint32_t*  exit_flag_gpu = nullptr;
    uint32_t*  exit_flag_cpu = nullptr;
    PollStats* stats_gpu = nullptr;
    PollStats* stats_cpu = nullptr;
    doca_gpu_eth_rxq** rxqs_gpu = nullptr;
};

namespace {

/// Allocate one queue's VRAM packet buffer and start it with the data path on
/// the GPU. After this the NIC can DMA frames straight into GPU memory.
bool queue_create(GpunetioSession* s, Queue& q, int cuda_id) {
    doca_error_t r;
    uint32_t buf_size = 0;

    if ((r = doca_eth_rxq_create(s->ddev, kMaxPktNum, kMaxPktSize, &q.rxq_cpu)) != DOCA_SUCCESS)
        return fail("doca_eth_rxq_create", r);
    if ((r = doca_eth_rxq_set_type(q.rxq_cpu, DOCA_ETH_RXQ_TYPE_CYCLIC)) != DOCA_SUCCESS)
        return fail("doca_eth_rxq_set_type", r);
    if ((r = doca_eth_rxq_estimate_packet_buf_size(DOCA_ETH_RXQ_TYPE_CYCLIC, 0, 0, kMaxPktSize,
                                                   kMaxPktNum, 0, 0, 0, &buf_size)) != DOCA_SUCCESS)
        return fail("doca_eth_rxq_estimate_packet_buf_size", r);

    if ((r = doca_mmap_create(&q.mmap)) != DOCA_SUCCESS) return fail("doca_mmap_create", r);
    if ((r = doca_mmap_add_dev(q.mmap, s->ddev)) != DOCA_SUCCESS)
        return fail("doca_mmap_add_dev", r);

    const size_t bytes = align_up(buf_size, page_size());
    if ((r = doca_gpu_mem_alloc(s->gpu, bytes, page_size(), DOCA_GPU_MEM_TYPE_GPU, &q.pkt_addr,
                                nullptr)) != DOCA_SUCCESS || !q.pkt_addr)
        return fail("doca_gpu_mem_alloc (packet buffer in VRAM)", r);

    // dmabuf is the supported way to let the NIC reach GPU memory; the
    // memrange path needs the legacy nvidia-peermem module instead.
    if (doca_gpu_dmabuf_fd(s->gpu, q.pkt_addr, bytes, &q.dmabuf_fd) != DOCA_SUCCESS) {
        std::printf("[gnp] dmabuf unavailable, mapping %zu B of VRAM via nvidia-peermem\n", bytes);
        if ((r = doca_mmap_set_memrange(q.mmap, q.pkt_addr, bytes)) != DOCA_SUCCESS)
            return fail("doca_mmap_set_memrange", r);
    } else {
        if ((r = doca_mmap_set_dmabuf_memrange(q.mmap, q.dmabuf_fd, q.pkt_addr, 0, bytes)) !=
            DOCA_SUCCESS)
            return fail("doca_mmap_set_dmabuf_memrange", r);
    }

    if ((r = doca_mmap_set_permissions(q.mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE)) != DOCA_SUCCESS)
        return fail("doca_mmap_set_permissions", r);
    if ((r = doca_mmap_start(q.mmap)) != DOCA_SUCCESS) return fail("doca_mmap_start", r);
    if ((r = doca_eth_rxq_set_pkt_buf(q.rxq_cpu, q.mmap, 0, bytes)) != DOCA_SUCCESS)
        return fail("doca_eth_rxq_set_pkt_buf", r);

    // Pre-Hopper parts cannot drive the queue doorbell from the SM directly and
    // need the multicast-QP helper. sm_75 lands here.
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, cuda_id);
    if (prop.major < 9) {
        if ((r = doca_eth_rxq_gpu_enable_mcst_qp(q.rxq_cpu)) != DOCA_SUCCESS)
            return fail("doca_eth_rxq_gpu_enable_mcst_qp", r);
    }

    q.ctx = doca_eth_rxq_as_doca_ctx(q.rxq_cpu);
    if (!q.ctx) return fail("doca_eth_rxq_as_doca_ctx", DOCA_ERROR_UNEXPECTED);
    if ((r = doca_ctx_set_datapath_on_gpu(q.ctx, s->gpu)) != DOCA_SUCCESS)
        return fail("doca_ctx_set_datapath_on_gpu", r);
    if ((r = doca_ctx_start(q.ctx)) != DOCA_SUCCESS) return fail("doca_ctx_start", r);
    if ((r = doca_eth_rxq_get_gpu_handle(q.rxq_cpu, &q.rxq_gpu)) != DOCA_SUCCESS)
        return fail("doca_eth_rxq_get_gpu_handle", r);

    return true;
}

/// RSS pipe spreading matched UDP traffic over every queue.
bool create_rxq_pipe(GpunetioSession* s) {
    doca_flow_match match{};
    match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
    match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;

    std::vector<uint16_t> rss_queues(s->queues.size());
    for (size_t i = 0; i < s->queues.size(); ++i) {
        doca_eth_rxq_apply_queue_id(s->queues[i].rxq_cpu, static_cast<uint16_t>(i));
        rss_queues[i] = static_cast<uint16_t>(i);
    }

    doca_flow_fwd fwd{};
    fwd.type = DOCA_FLOW_FWD_RSS;
    fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
    fwd.rss.queues_array = rss_queues.data();
    fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
    fwd.rss.nr_queues = static_cast<uint32_t>(rss_queues.size());

    // Anything that reaches this pipe already matched the port, so a miss here
    // is traffic we asked for and cannot place: drop it rather than leak it to
    // the kernel stack.
    doca_flow_fwd miss{};
    miss.type = DOCA_FLOW_FWD_DROP;

    doca_flow_monitor monitor{};
    monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

    doca_flow_pipe_cfg* cfg = nullptr;
    doca_error_t r = doca_flow_pipe_cfg_create(&cfg, s->port);
    if (r != DOCA_SUCCESS) return fail("doca_flow_pipe_cfg_create", r);

    bool ok = (doca_flow_pipe_cfg_set_name(cfg, "GNP_RXQ_UDP_PIPE") == DOCA_SUCCESS) &&
              (doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC) == DOCA_SUCCESS) &&
              (doca_flow_pipe_cfg_set_is_root(cfg, false) == DOCA_SUCCESS) &&
              (doca_flow_pipe_cfg_set_match(cfg, &match, nullptr) == DOCA_SUCCESS) &&
              (doca_flow_pipe_cfg_set_monitor(cfg, &monitor) == DOCA_SUCCESS);
    if (ok) {
        r = doca_flow_pipe_create(cfg, &fwd, &miss, &s->rxq_pipe);
        ok = (r == DOCA_SUCCESS);
    }
    doca_flow_pipe_cfg_destroy(cfg);
    if (!ok) return fail("rxq pipe creation", r);

    doca_flow_pipe_entry* entry = nullptr;
    if ((r = doca_flow_pipe_basic_add_entry(0, s->rxq_pipe, &match, 0, nullptr, nullptr, nullptr,
                                            DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, nullptr, &entry)) !=
        DOCA_SUCCESS)
        return fail("rxq pipe entry", r);
    if ((r = doca_flow_entries_process(s->port, 0, 0, 0)) != DOCA_SUCCESS)
        return fail("doca_flow_entries_process", r);
    return true;
}

/// Root pipe: match IPv4/UDP on one destination port and send it to the RSS
/// pipe. Everything else falls through to the kernel stack untouched, so the
/// machine stays reachable while a run is in progress.
bool create_root_pipe(GpunetioSession* s, uint16_t udp_port) {
    doca_flow_match match{};
    match.outer.eth.type = htons(DOCA_FLOW_ETHER_TYPE_IPV4);
    match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    match.outer.ip4.next_proto = IPPROTO_UDP;
    match.outer.udp.l4_port.dst_port = htons(udp_port);

    doca_flow_fwd fwd{};
    fwd.type = DOCA_FLOW_FWD_PIPE;
    fwd.next_pipe = s->rxq_pipe;

    doca_flow_monitor monitor{};
    monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;

    doca_flow_pipe_cfg* cfg = nullptr;
    doca_error_t r = doca_flow_pipe_cfg_create(&cfg, s->port);
    if (r != DOCA_SUCCESS) return fail("doca_flow_pipe_cfg_create (root)", r);

    bool ok = (doca_flow_pipe_cfg_set_name(cfg, "GNP_ROOT_PIPE") == DOCA_SUCCESS) &&
              (doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_CONTROL) == DOCA_SUCCESS) &&
              (doca_flow_pipe_cfg_set_is_root(cfg, true) == DOCA_SUCCESS) &&
              (doca_flow_pipe_cfg_set_monitor(cfg, &monitor) == DOCA_SUCCESS);
    if (ok) {
        r = doca_flow_pipe_create(cfg, nullptr, nullptr, &s->root_pipe);
        ok = (r == DOCA_SUCCESS);
    }
    doca_flow_pipe_cfg_destroy(cfg);
    if (!ok) return fail("root pipe creation", r);

    doca_flow_pipe_entry* entry = nullptr;
    if ((r = doca_flow_pipe_control_add_entry(0, s->root_pipe, &match, nullptr, nullptr, nullptr,
                                              nullptr, nullptr, nullptr, 0, &fwd, nullptr,
                                              &entry)) != DOCA_SUCCESS)
        return fail("root pipe entry", r);
    if ((r = doca_flow_entries_process(s->port, 0, 0, 0)) != DOCA_SUCCESS)
        return fail("doca_flow_entries_process (root)", r);
    return true;
}

bool flow_start(GpunetioSession* s) {
    doca_flow_cfg* fcfg = nullptr;
    doca_error_t r = doca_flow_cfg_create(&fcfg);
    if (r != DOCA_SUCCESS) return fail("doca_flow_cfg_create", r);
    bool ok = (doca_flow_cfg_set_pipe_queues(fcfg, 1) == DOCA_SUCCESS) &&
              (doca_flow_cfg_set_mode_args(fcfg, "vnf") == DOCA_SUCCESS) &&
              (doca_flow_cfg_set_nr_counters(fcfg, kFlowCounters) == DOCA_SUCCESS);
    if (ok) {
        r = doca_flow_init(fcfg);
        ok = (r == DOCA_SUCCESS);
    }
    doca_flow_cfg_destroy(fcfg);
    if (!ok) return fail("doca_flow_init", r);
    s->flow_inited = true;

    doca_flow_port_cfg* pcfg = nullptr;
    if ((r = doca_flow_port_cfg_create(&pcfg)) != DOCA_SUCCESS)
        return fail("doca_flow_port_cfg_create", r);
    ok = (doca_flow_port_cfg_set_port_id(pcfg, 0) == DOCA_SUCCESS) &&
         (doca_flow_port_cfg_set_dev(pcfg, s->ddev) == DOCA_SUCCESS);
    if (ok) {
        r = doca_flow_port_start(pcfg, &s->port);
        ok = (r == DOCA_SUCCESS);
    }
    doca_flow_port_cfg_destroy(pcfg);
    if (!ok) return fail("doca_flow_port_start", r);
    return true;
}

void destroy(GpunetioSession* s) {
    if (!s) return;
    if (s->root_pipe) doca_flow_pipe_destroy(s->root_pipe);
    if (s->rxq_pipe) doca_flow_pipe_destroy(s->rxq_pipe);
    if (s->port) doca_flow_port_stop(s->port);
    if (s->flow_inited) doca_flow_destroy();

    for (Queue& q : s->queues) {
        if (q.ctx) doca_ctx_stop(q.ctx);
        if (q.rxq_cpu) doca_eth_rxq_destroy(q.rxq_cpu);
        if (q.mmap) doca_mmap_destroy(q.mmap);
        if (q.pkt_addr) doca_gpu_mem_free(s->gpu, q.pkt_addr);
    }
    if (s->exit_flag_gpu) doca_gpu_mem_free(s->gpu, s->exit_flag_gpu);
    if (s->stats_gpu) doca_gpu_mem_free(s->gpu, s->stats_gpu);
    if (s->rxqs_gpu) doca_gpu_mem_free(s->gpu, s->rxqs_gpu);
    if (s->stream) cudaStreamDestroy(s->stream);
    if (s->gpu) doca_gpu_destroy(s->gpu);
    if (s->ddev) doca_dev_close(s->ddev);
    delete s;
}

}  // namespace

const char* gpunetio_name() { return "gpunetio"; }

GpunetioSession* gpunetio_start(const RunConfig& cfg, const GpunetioConfig& gcfg) {
    if (!gcfg.nic_pci || !gcfg.gpu_pci || gcfg.port == 0) {
        std::fprintf(stderr, "[gnp] --nic, --gpu and --port are required\n");
        return nullptr;
    }

    auto* s = new GpunetioSession();
    doca_error_t r;

    if (!open_nic_by_pci(gcfg.nic_pci, &s->ddev)) {
        destroy(s);
        return nullptr;
    }
    if ((r = doca_gpu_create(gcfg.gpu_pci, &s->gpu)) != DOCA_SUCCESS) {
        fail("doca_gpu_create", r);
        destroy(s);
        return nullptr;
    }
    if (cudaStreamCreateWithFlags(&s->stream, cudaStreamNonBlocking) != cudaSuccess) {
        std::fprintf(stderr, "[gnp] cudaStreamCreateWithFlags failed\n");
        destroy(s);
        return nullptr;
    }

    int cuda_id = 0;
    cudaGetDevice(&cuda_id);

    if (!flow_start(s)) {
        destroy(s);
        return nullptr;
    }

    s->queues.resize(cfg.queues);
    for (Queue& q : s->queues) {
        if (!queue_create(s, q, cuda_id)) {
            destroy(s);
            return nullptr;
        }
    }

    if (!create_rxq_pipe(s) || !create_root_pipe(s, gcfg.port)) {
        destroy(s);
        return nullptr;
    }

    // Control and counters in memory both sides can reach, then the queue
    // handle array the kernel indexes by blockIdx.
    void* gpu_ptr = nullptr;
    void* cpu_ptr = nullptr;
    if ((r = doca_gpu_mem_alloc(s->gpu, sizeof(uint32_t), 4096, DOCA_GPU_MEM_TYPE_CPU_GPU, &gpu_ptr,
                                &cpu_ptr)) != DOCA_SUCCESS) {
        fail("doca_gpu_mem_alloc (exit flag)", r);
        destroy(s);
        return nullptr;
    }
    s->exit_flag_gpu = static_cast<uint32_t*>(gpu_ptr);
    s->exit_flag_cpu = static_cast<uint32_t*>(cpu_ptr);
    *s->exit_flag_cpu = 0;

    const size_t stats_bytes = sizeof(PollStats) * s->queues.size();
    if ((r = doca_gpu_mem_alloc(s->gpu, stats_bytes, 4096, DOCA_GPU_MEM_TYPE_CPU_GPU, &gpu_ptr,
                                &cpu_ptr)) != DOCA_SUCCESS) {
        fail("doca_gpu_mem_alloc (stats)", r);
        destroy(s);
        return nullptr;
    }
    s->stats_gpu = static_cast<PollStats*>(gpu_ptr);
    s->stats_cpu = static_cast<PollStats*>(cpu_ptr);
    std::memset(s->stats_cpu, 0, stats_bytes);

    const size_t handles_bytes = sizeof(doca_gpu_eth_rxq*) * s->queues.size();
    if ((r = doca_gpu_mem_alloc(s->gpu, handles_bytes, 4096, DOCA_GPU_MEM_TYPE_CPU_GPU, &gpu_ptr,
                                &cpu_ptr)) != DOCA_SUCCESS) {
        fail("doca_gpu_mem_alloc (queue handles)", r);
        destroy(s);
        return nullptr;
    }
    s->rxqs_gpu = static_cast<doca_gpu_eth_rxq**>(gpu_ptr);
    auto** handles_cpu = static_cast<doca_gpu_eth_rxq**>(cpu_ptr);
    for (size_t i = 0; i < s->queues.size(); ++i) handles_cpu[i] = s->queues[i].rxq_gpu;

    if (gnp_launch_receive_kernel(s->stream, s->rxqs_gpu, static_cast<uint32_t>(s->queues.size()),
                                  s->exit_flag_gpu, s->stats_gpu) != 0) {
        std::fprintf(stderr, "[gnp] receive kernel launch failed\n");
        destroy(s);
        return nullptr;
    }

    return s;
}

void gpunetio_stop(GpunetioSession* s, PollStats& out) {
    if (!s) return;

    *s->exit_flag_cpu = 1;
    cudaStreamSynchronize(s->stream);

    out = stats_aggregate(s->stats_cpu, static_cast<uint32_t>(s->queues.size()));
    destroy(s);
}

}  // namespace gnp
