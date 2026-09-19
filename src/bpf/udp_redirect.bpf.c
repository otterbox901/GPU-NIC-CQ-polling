// SPDX-License-Identifier: GPL-2.0
//
// src/bpf/udp_redirect.bpf.c - steer one UDP port into an AF_XDP socket.
//
// Runs at the driver's XDP hook, before the kernel network stack sees the
// frame. IPv4/UDP frames whose destination port matches config_map[0] are
// redirected to the AF_XDP socket bound to the receiving RX queue; everything
// else is passed up the normal stack untouched.
//

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#define GNP_MAX_QUEUES 64

// Keyed by RX queue index. AF_XDP only accepts frames that arrived on the
// queue its socket is bound to, so only that queue's slot is ever filled.
struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, GNP_MAX_QUEUES);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

// [0] = destination UDP port to capture (host byte order), [1] = RX queue the
// socket is bound to. Set by userspace before attach.
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 2);
    __type(key, __u32);
    __type(value, __u32);
} config_map SEC(".maps");

// [0] = frames that matched the port, [1] = of those, on the bound queue.
// Read by userspace at exit to tell "never arrived" from "wrong queue".
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 2);
    __type(key, __u32);
    __type(value, __u64);
} stats_map SEC(".maps");

static __always_inline void count(__u32 key) {
    __u64 *c = bpf_map_lookup_elem(&stats_map, &key);
    if (c) (*c)++;
}

SEC("xdp")
int udp_redirect_prog(struct xdp_md *ctx) {
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end) return XDP_PASS;
    if (eth->h_proto != bpf_htons(ETH_P_IP)) return XDP_PASS;

    struct iphdr *ip = (void *)(eth + 1);
    if ((void *)(ip + 1) > data_end) return XDP_PASS;
    if (ip->protocol != IPPROTO_UDP) return XDP_PASS;
    // Only unfragmented datagrams: later fragments carry no UDP header.
    if (ip->frag_off & bpf_htons(0x3fff)) return XDP_PASS;

    __u32 ip_len = ip->ihl * 4;
    if (ip_len < sizeof(*ip)) return XDP_PASS;
    struct udphdr *udp = (void *)ip + ip_len;
    if ((void *)(udp + 1) > data_end) return XDP_PASS;

    __u32 key = 0;
    __u32 *port = bpf_map_lookup_elem(&config_map, &key);
    if (!port || bpf_ntohs(udp->dest) != *port) return XDP_PASS;

    count(0);
    key = 1;
    __u32 *queue = bpf_map_lookup_elem(&config_map, &key);
    if (queue && ctx->rx_queue_index == *queue) count(1);

    // No socket on this queue -> XDP_PASS rather than a silent drop.
    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
