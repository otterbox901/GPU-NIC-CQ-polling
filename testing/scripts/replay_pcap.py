#!/usr/bin/env python3
"""Replay the UDP packets of a pcap onto an interface, to feed `gnp`.

Frames are sent at layer 2 (one raw AF_PACKET socket), so they arrive at the receiving
interface's XDP hook exactly like traffic from a real NIC. Sending from the
same host with a normal UDP socket would not work: loopback traffic never
passes through the XDP program attached to another interface.

  # write a sample capture (no pcap handy)
  testing/scripts/replay_pcap.py make --out udp.pcap --count 10000 --size 512

  # replay it at 10 kpps into veth1, rewritten to hit gnp on the peer veth0
  sudo testing/scripts/replay_pcap.py send --pcap udp.pcap --iface veth1 \\
      --dst-mac "$(cat /sys/class/net/veth0/address)" --dst-ip 10.99.0.1 \\
      --dst-port 9000 --pps 10000

Only IPv4/UDP packets are replayed; everything else in the pcap is skipped.
Requires scapy (pip install scapy / dnf install python3-scapy); sending needs root.
For line-rate replay on real hardware, use tcpreplay instead (see README).
"""

import argparse
import sys
import time

try:
    from scapy.all import IP, UDP, Ether, Raw, conf, get_if_hwaddr, rdpcap, wrpcap
except ImportError:
    sys.exit("replay_pcap.py needs scapy: pip install scapy (or dnf install python3-scapy)")


def cmd_make(args):
    pkts = []
    for i in range(args.count):
        payload = i.to_bytes(4, "little") + bytes(max(0, args.size - 4))
        pkts.append(
            Ether(src="02:00:00:00:00:01", dst="02:00:00:00:00:02")
            / IP(src="10.99.0.2", dst="10.99.0.1")
            / UDP(sport=40000, dport=args.port)
            / Raw(payload)
        )
    wrpcap(args.out, pkts)
    print(f"wrote {len(pkts)} UDP packets ({args.size} B payload, dport {args.port}) to {args.out}")


def cmd_send(args):
    src_mac = get_if_hwaddr(args.iface)
    frames = []
    for pkt in rdpcap(args.pcap):
        if IP not in pkt or UDP not in pkt:
            continue
        ip = pkt[IP].copy()
        if args.dst_ip:
            ip.dst = args.dst_ip
        if args.dst_port:
            ip[UDP].dport = args.dst_port
        del ip.chksum, ip.len
        del ip[UDP].chksum, ip[UDP].len
        dst_mac = args.dst_mac or (pkt[Ether].dst if Ether in pkt else "ff:ff:ff:ff:ff:ff")
        frames.append(Ether(src=src_mac, dst=dst_mac) / ip)
        if args.count and len(frames) >= args.count:
            break

    if not frames:
        sys.exit(f"no IPv4/UDP packets in {args.pcap}")

    payload_bytes = sum(len(f[UDP].payload) for f in frames)
    raw = [bytes(f) for f in frames]
    interval = 1.0 / args.pps if args.pps else 0.0

    # One socket for the whole replay: sendp() opens and binds a new one per
    # packet, which caps it at a few dozen packets per second.
    sock = conf.L2socket(iface=args.iface)
    try:
        start = time.perf_counter()
        for i, frame in enumerate(raw):
            if interval:
                delay = start + i * interval - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
            sock.send(frame)
        elapsed = time.perf_counter() - start
    finally:
        sock.close()
    print(f"sent {len(frames)} UDP packets ({payload_bytes} payload bytes) on {args.iface} "
          f"in {elapsed:.2f} s")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(required=True)

    mk = sub.add_parser("make", help="write a sample UDP pcap")
    mk.add_argument("--out", default="udp.pcap")
    mk.add_argument("--count", type=int, default=10000)
    mk.add_argument("--size", type=int, default=512, help="UDP payload bytes (default 512)")
    mk.add_argument("--port", type=int, default=9000, help="UDP destination port (default 9000)")
    mk.set_defaults(func=cmd_make)

    sd = sub.add_parser("send", help="replay a pcap's UDP packets onto an interface")
    sd.add_argument("--pcap", required=True)
    sd.add_argument("--iface", required=True, help="interface to transmit on")
    sd.add_argument("--dst-mac", help="rewrite destination MAC (the receiving interface's)")
    sd.add_argument("--dst-ip", help="rewrite destination IPv4 address")
    sd.add_argument("--dst-port", type=int, help="rewrite UDP destination port")
    sd.add_argument("--count", type=int, default=0, help="stop after N packets, 0 = all")
    sd.add_argument("--pps", type=float, default=0, help="pace to R packets/s, 0 = as fast as possible")
    sd.set_defaults(func=cmd_send)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
