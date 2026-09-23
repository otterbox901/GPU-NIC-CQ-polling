//
// src/gpunetio/main_gpunetio.cpp - gnp_gpunetio: receive straight into VRAM.
//
// Same shape as src/host/main.cpp - allocate, launch, sleep, report - but with
// nothing at all on the data path. There is no ingest thread here: the NIC
// writes frames into GPU memory and the persistent kernel reads them there, so
// this thread only starts the run and collects counters at the end.
//

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "gnp/cli.hpp"
#include "gnp/common.hpp"
#include "gnp/gpunetio.hpp"
#include "gnp/metrics.hpp"

namespace {

void usage(const char* argv0) {
    std::printf("usage: %s --nic PCI --gpu PCI --port N [options]\n", argv0);
    std::printf(
        "  --nic PCI         NIC PCIe address, e.g. 0000:c1:00.0 (required)\n"
        "  --gpu PCI         GPU PCIe address, e.g. 0000:01:00.0 (required)\n"
        "  --port N          destination UDP port to steer into GPU memory (required)\n");
    gnp::print_common_usage(gnp::FlagScope::kCore);
}

bool parse_args(int argc, char** argv, gnp::RunConfig& cfg, gnp::GpunetioConfig& gcfg,
                bool& want_help) {
    for (int i = 1; i < argc; ++i) {
        switch (gnp::parse_common_flag(argc, argv, i, cfg, gnp::FlagScope::kCore)) {
            case gnp::FlagResult::kOk: continue;
            case gnp::FlagResult::kBad: return false;
            case gnp::FlagResult::kHelp: want_help = true; return true;
            case gnp::FlagResult::kNotMine: break;
        }

        const char* a = argv[i];
        uint64_t v = 0;
        bool ok = true;
        if (!std::strcmp(a, "--nic")) {
            ok = i + 1 < argc;
            if (ok) gcfg.nic_pci = argv[++i];
        } else if (!std::strcmp(a, "--gpu")) {
            ok = i + 1 < argc;
            if (ok) gcfg.gpu_pci = argv[++i];
        } else if (!std::strcmp(a, "--port")) {
            ok = gnp::take_u64(argc, argv, i, v) && v >= 1 && v <= 65535;
            gcfg.port = static_cast<uint16_t>(v);
        } else {
            std::fprintf(stderr, "[gnp] unknown option: %s\n", a);
            return false;
        }
        if (!ok) {
            std::fprintf(stderr, "[gnp] bad or missing value for %s\n", a);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    gnp::RunConfig cfg;
    gnp::GpunetioConfig gcfg;
    bool want_help = false;
    if (!parse_args(argc, argv, cfg, gcfg, want_help)) return 2;
    if (want_help) {
        usage(argv[0]);
        return 0;
    }
    if (!gcfg.nic_pci || !gcfg.gpu_pci || gcfg.port == 0) {
        std::fprintf(stderr, "[gnp] --nic, --gpu and --port are required\n");
        usage(argv[0]);
        return 2;
    }

    cfg.target_pps = 0;  // arrival rate is whatever the wire delivers

    const uint64_t t0 = gnp::host_now_ns();
    gnp::GpunetioSession* s = gnp::gpunetio_start(cfg, gcfg);
    if (!s) return 1;

    std::printf("[gnp] steering UDP port %u on %s into %s memory, %u queue(s), for %u ms...\n",
                gcfg.port, gcfg.nic_pci, gcfg.gpu_pci, cfg.queues, cfg.duration_ms);

    // Steady state: the NIC and the GPU do all the work.
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.duration_ms));

    gnp::PollStats poll{};
    gnp::gpunetio_stop(s, poll);
    const uint64_t t1 = gnp::host_now_ns();

    // There is no separate producer to account for: the NIC delivered exactly
    // what the kernel observed, so the "producer" row mirrors it and the window
    // is the host's own run time.
    gnp::IngestStats ingest;
    ingest.produced = poll.packets;
    ingest.start_ns = t0;
    ingest.end_ns = t1;

    gnp::report(cfg, &poll, &ingest, 1, gnp::gpunetio_name(), "NIC -> VRAM (GPUDirect)");
    return 0;
}
