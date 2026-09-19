//
// src/host/xdp_ingest.cpp - AF_XDP receive: NIC -> UMEM -> payload arena -> ring.
//
// Data path per frame:
//   1. The XDP program redirects a matching UDP frame into this socket's UMEM
//      and posts an RX descriptor. No skb, no IP stack, no socket buffer.
//   2. The RX thread copies the UDP payload into the arena slot the ring index
//      maps to, and publishes a CompletionDesc with ring_publish().
//   3. The UMEM frame goes straight back on the fill ring for the NIC to reuse.
//

#include "gnp/xdp_ingest.hpp"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <net/if.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/xsk.h>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

#include "gnp/gpu_poll.hpp"

namespace gnp {
namespace {

constexpr uint32_t kFrameSize = XSK_UMEM__DEFAULT_FRAME_SIZE;  // 4 KiB, one frame per packet
constexpr uint32_t kNumFrames = 4096;
constexpr uint32_t kRxRingSize = XSK_RING_CONS__DEFAULT_NUM_DESCS;
constexpr uint32_t kFillRingSize = XSK_RING_PROD__DEFAULT_NUM_DESCS;
constexpr uint32_t kRxBatch = 64;

inline void cpu_relax() {
#if defined(__x86_64__)
    _mm_pause();
#endif
}

/// Locate the UDP payload inside a raw Ethernet frame. The XDP program already
/// filtered on these headers; this re-checks bounds because the frame length
/// comes from the kernel, not from the headers.
bool udp_payload(const uint8_t* frame, uint32_t frame_len, const uint8_t** payload,
                 uint32_t* payload_len) {
    if (frame_len < sizeof(ethhdr) + sizeof(iphdr) + sizeof(udphdr)) return false;
    const auto* eth = reinterpret_cast<const ethhdr*>(frame);
    if (eth->h_proto != htons(ETH_P_IP)) return false;

    const auto* ip = reinterpret_cast<const iphdr*>(frame + sizeof(ethhdr));
    const uint32_t ip_len = ip->ihl * 4u;
    if (ip->protocol != IPPROTO_UDP || ip_len < sizeof(iphdr)) return false;

    const uint32_t udp_off = sizeof(ethhdr) + ip_len;
    if (udp_off + sizeof(udphdr) > frame_len) return false;
    const auto* udp = reinterpret_cast<const udphdr*>(frame + udp_off);

    // Use the UDP length, not the frame length: short frames are padded to 60 B.
    const uint32_t udp_len = ntohs(udp->len);
    if (udp_len < sizeof(udphdr) || udp_off + udp_len > frame_len) return false;

    *payload = frame + udp_off + sizeof(udphdr);
    *payload_len = udp_len - static_cast<uint32_t>(sizeof(udphdr));
    return true;
}

/// Sum one slot of a per-CPU u64 array map.
uint64_t percpu_sum(int map_fd, uint32_t key) {
    const int ncpu = libbpf_num_possible_cpus();
    if (map_fd < 0 || ncpu <= 0) return 0;
    std::vector<uint64_t> vals(static_cast<size_t>(ncpu));
    if (bpf_map_lookup_elem(map_fd, &key, vals.data()) != 0) return 0;
    uint64_t sum = 0;
    for (uint64_t v : vals) sum += v;
    return sum;
}

std::string default_bpf_obj() {
    char exe[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return "udp_redirect.bpf.o";
    exe[n] = '\0';
    std::string dir(exe);
    return dir.substr(0, dir.rfind('/') + 1) + "udp_redirect.bpf.o";
}

}  // namespace

struct XdpIngest {
    std::thread thread;
    std::atomic<bool> stop{false};
    IngestStats stats;

    bpf_object* obj = nullptr;
    int ifindex = 0;
    uint32_t queue_id = 0;
    uint16_t port = 0;
    uint32_t attach_flags = 0;
    bool attached = false;

    void* umem_area = nullptr;
    xsk_umem* umem = nullptr;
    xsk_ring_prod fill{};
    xsk_ring_cons comp{};
    xsk_socket* xsk = nullptr;
    xsk_ring_cons rx{};
};

namespace {

bool load_and_attach(XdpIngest* h, const XdpConfig& xdp) {
    h->ifindex = static_cast<int>(if_nametoindex(xdp.iface));
    if (h->ifindex == 0) {
        std::fprintf(stderr, "[gnp] no such interface: %s\n", xdp.iface);
        return false;
    }

    const std::string path = xdp.bpf_obj ? xdp.bpf_obj : default_bpf_obj();
    h->obj = bpf_object__open_file(path.c_str(), nullptr);
    if (!h->obj) {
        std::fprintf(stderr, "[gnp] cannot open BPF object %s: %s\n", path.c_str(),
                     std::strerror(errno));
        h->obj = nullptr;
        return false;
    }
    if (int err = bpf_object__load(h->obj)) {
        std::fprintf(stderr, "[gnp] cannot load BPF object %s: %s (need root/CAP_BPF?)\n",
                     path.c_str(), std::strerror(-err));
        return false;
    }

    const int cfg_fd = bpf_object__find_map_fd_by_name(h->obj, "config_map");
    const uint32_t port_key = 0, queue_key = 1;
    const uint32_t port = xdp.port;
    if (cfg_fd < 0 || bpf_map_update_elem(cfg_fd, &port_key, &port, BPF_ANY) != 0 ||
        bpf_map_update_elem(cfg_fd, &queue_key, &xdp.queue_id, BPF_ANY) != 0) {
        std::fprintf(stderr, "[gnp] cannot write config_map\n");
        return false;
    }

    bpf_program* prog = bpf_object__find_program_by_name(h->obj, "udp_redirect_prog");
    if (!prog) {
        std::fprintf(stderr, "[gnp] udp_redirect_prog missing from %s\n", path.c_str());
        return false;
    }
    const int prog_fd = bpf_program__fd(prog);

    // Native (driver) mode first: the frame never becomes an skb. Generic mode
    // works on any interface but runs after skb allocation, so it is slower.
    // UPDATE_IF_NOEXIST: never replace someone else's XDP program.
    if (!xdp.skb_mode) {
        h->attach_flags = XDP_FLAGS_DRV_MODE | XDP_FLAGS_UPDATE_IF_NOEXIST;
        if (bpf_xdp_attach(h->ifindex, prog_fd, h->attach_flags, nullptr) == 0) {
            h->attached = true;
            return true;
        }
        std::fprintf(stderr, "[gnp] native XDP attach on %s failed; falling back to generic "
                             "(SKB) mode\n", xdp.iface);
    }
    h->attach_flags = XDP_FLAGS_SKB_MODE | XDP_FLAGS_UPDATE_IF_NOEXIST;
    if (int err = bpf_xdp_attach(h->ifindex, prog_fd, h->attach_flags, nullptr)) {
        std::fprintf(stderr, "[gnp] XDP attach on %s failed: %s%s\n", xdp.iface,
                     std::strerror(-err),
                     err == -EBUSY || err == -EEXIST ? " (another XDP program is attached)" : "");
        return false;
    }
    h->attached = true;
    return true;
}

bool open_socket(XdpIngest* h, const XdpConfig& xdp) {
    // UMEM pages are locked memory. Root has no limit; raise it where allowed.
    const rlimit unlimited{RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &unlimited);

    const size_t umem_bytes = static_cast<size_t>(kNumFrames) * kFrameSize;
    if (posix_memalign(&h->umem_area, static_cast<size_t>(getpagesize()), umem_bytes) != 0) {
        h->umem_area = nullptr;
        std::fprintf(stderr, "[gnp] cannot allocate %zu B of UMEM\n", umem_bytes);
        return false;
    }

    xsk_umem_config ucfg{};
    ucfg.fill_size = kFillRingSize;
    ucfg.comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    ucfg.frame_size = kFrameSize;
    ucfg.frame_headroom = 0;
    if (int err = xsk_umem__create(&h->umem, h->umem_area, umem_bytes, &h->fill, &h->comp,
                                   &ucfg)) {
        std::fprintf(stderr, "[gnp] xsk_umem__create failed: %s\n", std::strerror(-err));
        h->umem = nullptr;
        return false;
    }

    xsk_socket_config scfg{};
    scfg.rx_size = kRxRingSize;
    scfg.tx_size = 0;
    scfg.libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD;  // we attached our own program
    scfg.xdp_flags = h->attach_flags;
    scfg.bind_flags = XDP_USE_NEED_WAKEUP;
    if (int err = xsk_socket__create(&h->xsk, xdp.iface, xdp.queue_id, h->umem, &h->rx, nullptr,
                                     &scfg)) {
        std::fprintf(stderr, "[gnp] xsk_socket__create on %s queue %u failed: %s\n", xdp.iface,
                     xdp.queue_id, std::strerror(-err));
        h->xsk = nullptr;
        return false;
    }

    // Hand every fill-ring slot a frame so the NIC can start receiving.
    uint32_t idx = 0;
    if (xsk_ring_prod__reserve(&h->fill, kFillRingSize, &idx) != kFillRingSize) {
        std::fprintf(stderr, "[gnp] cannot prime the AF_XDP fill ring\n");
        return false;
    }
    for (uint32_t i = 0; i < kFillRingSize; ++i) {
        *xsk_ring_prod__fill_addr(&h->fill, idx + i) = static_cast<uint64_t>(i) * kFrameSize;
    }
    xsk_ring_prod__submit(&h->fill, kFillRingSize);

    const int map_fd = bpf_object__find_map_fd_by_name(h->obj, "xsks_map");
    if (map_fd < 0 || xsk_socket__update_xskmap(h->xsk, map_fd) != 0) {
        std::fprintf(stderr, "[gnp] cannot register the socket in xsks_map\n");
        return false;
    }
    return true;
}

void teardown(XdpIngest* h) {
    if (h->attached) bpf_xdp_detach(h->ifindex, h->attach_flags, nullptr);
    if (h->xsk) xsk_socket__delete(h->xsk);
    if (h->umem) xsk_umem__delete(h->umem);
    std::free(h->umem_area);
    if (h->obj) bpf_object__close(h->obj);
}

void rx_loop(XdpIngest* h, uint32_t queue, CompletionRing host_ring, CompletionDesc* device_descs,
             RingControl* ctrl, uint8_t* arena, uint32_t slot_bytes) {
    const auto* consumed =
        reinterpret_cast<const std::atomic<unsigned long long>*>(&ctrl->consumed);
    const int fd = xsk_socket__fd(h->xsk);

    uint64_t produced = 0;
    uint64_t flushed = 0;
    uint64_t overruns = 0;
    uint64_t dropped = 0;
    h->stats.start_ns = host_now_ns();

    auto flush = [&] {
        if (produced == flushed) return;
        backend_flush_descs(queue, host_ring.descs, device_descs, host_ring.capacity, flushed,
                            static_cast<uint32_t>(produced - flushed));
        flushed = produced;
    };
    auto kick = [&] {
        if (xsk_ring_prod__needs_wakeup(&h->fill)) {
            recvfrom(fd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
        }
    };

    while (!h->stop.load(std::memory_order_relaxed)) {
        uint32_t rx_idx = 0;
        const uint32_t n = xsk_ring_cons__peek(&h->rx, kRxBatch, &rx_idx);
        if (n == 0) {
            kick();
            cpu_relax();
            continue;
        }

        // Reserve fill slots up front so each frame is recycled as soon as its
        // payload has been copied out.
        uint32_t fill_idx = 0;
        while (xsk_ring_prod__reserve(&h->fill, n, &fill_idx) != n) {
            kick();
            if (h->stop.load(std::memory_order_relaxed)) goto done;
        }

        for (uint32_t i = 0; i < n; ++i) {
            const xdp_desc* d = xsk_ring_cons__rx_desc(&h->rx, rx_idx + i);
            const auto* frame = static_cast<const uint8_t*>(xsk_umem__get_data(h->umem_area, d->addr));

            const uint8_t* payload = nullptr;
            uint32_t len = 0;
            if (!udp_payload(frame, d->len, &payload, &len) || len > slot_bytes) {
                ++dropped;
            } else {
                // Free slots only appear after the poller advances `consumed`.
                while (produced - consumed->load(std::memory_order_acquire) >= host_ring.capacity) {
                    flush();
                    ++overruns;
                    cpu_relax();
                    if (h->stop.load(std::memory_order_relaxed)) break;
                }
                if (produced - consumed->load(std::memory_order_acquire) < host_ring.capacity) {
                    const uint64_t offset =
                        static_cast<uint64_t>(ring_slot(host_ring, produced)) * slot_bytes;
                    std::memcpy(arena + offset, payload, len);
                    ring_publish(host_ring, produced, offset, len,
                                 static_cast<uint32_t>(produced), host_now_ns());
                    ++produced;
                } else {
                    ++dropped;
                }
            }
            *xsk_ring_prod__fill_addr(&h->fill, fill_idx + i) = d->addr;
        }
        xsk_ring_prod__submit(&h->fill, n);
        xsk_ring_cons__release(&h->rx, n);
        flush();
    }

done:
    flush();
    backend_flush_wait(queue);
    backend_copy_stats(queue, h->stats);
    h->stats.produced = produced;
    h->stats.overruns = overruns;
    h->stats.dropped = dropped;
    h->stats.end_ns = host_now_ns();
}

}  // namespace

XdpIngest* xdp_ingest_start(uint32_t queue, const CompletionRing& host_ring,
                            CompletionDesc* device_descs, RingControl* ctrl, uint8_t* arena,
                            size_t arena_bytes, const RunConfig& cfg, const XdpConfig& xdp) {
    if (!xdp.iface || xdp.port == 0) {
        std::fprintf(stderr, "[gnp] --iface and --port are required\n");
        return nullptr;
    }
    if (!host_ring.descs || !device_descs || !ctrl || !arena ||
        arena_bytes < static_cast<size_t>(host_ring.capacity) * cfg.payload_bytes) {
        return nullptr;
    }

    auto* h = new XdpIngest();
    h->queue_id = xdp.queue_id;
    h->port = xdp.port;
    if (!load_and_attach(h, xdp) || !open_socket(h, xdp)) {
        teardown(h);
        delete h;
        return nullptr;
    }

    h->thread = std::thread(rx_loop, h, queue, host_ring, device_descs, ctrl, arena,
                            cfg.payload_bytes);
    pthread_setname_np(h->thread.native_handle(), "gnp-xdp-rx");
    return h;
}

void xdp_ingest_stop(XdpIngest* h, IngestStats& out) {
    if (!h) return;
    h->stop.store(true, std::memory_order_relaxed);
    if (h->thread.joinable()) h->thread.join();
    out = h->stats;

    // Frames the kernel dropped before they reached us: without these, a
    // replay whose counts don't match has no explanation.
    xdp_statistics ks{};
    socklen_t len = sizeof(ks);
    if (getsockopt(xsk_socket__fd(h->xsk), SOL_XDP, XDP_STATISTICS, &ks, &len) == 0) {
        std::printf("[gnp] AF_XDP kernel counters: rx_dropped=%llu rx_invalid=%llu "
                    "rx_ring_full=%llu fill_ring_empty=%llu\n",
                    static_cast<unsigned long long>(ks.rx_dropped),
                    static_cast<unsigned long long>(ks.rx_invalid_descs),
                    static_cast<unsigned long long>(ks.rx_ring_full),
                    static_cast<unsigned long long>(ks.rx_fill_ring_empty_descs));
    }

    // What the XDP program saw. Matched == 0 means the traffic never reached
    // the interface while we were attached; matched > on_queue means RSS put
    // some of it on another RX queue, where it went to the kernel stack instead.
    const int stats_fd = bpf_object__find_map_fd_by_name(h->obj, "stats_map");
    const uint64_t matched = percpu_sum(stats_fd, 0);
    const uint64_t on_queue = percpu_sum(stats_fd, 1);
    std::printf("[gnp] XDP program: %llu frames to UDP port %u, %llu of them on RX queue %u\n",
                static_cast<unsigned long long>(matched), h->port,
                static_cast<unsigned long long>(on_queue), h->queue_id);
    if (matched > on_queue) {
        std::printf("[gnp] %llu frames arrived on other RX queues and went to the kernel stack; "
                    "steer the port with ethtool -N (see README)\n",
                    static_cast<unsigned long long>(matched - on_queue));
    }

    teardown(h);
    delete h;
}

}  // namespace gnp
