#!/usr/bin/env python3
"""Render docs/bench/results.csv (from scripts/bench.py) as README charts.

Writes a light and a dark SVG per chart into docs/img/, and prints the median
tables used in the README. Standard library only.

Every value is the median over runs. Colors follow the entity everywhere:
GPU poller = blue, CPU fallback = orange, and the GPU path's H2D flush = aqua
(categorical slots 1-3, validated together in both themes).
"""

import csv
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "docs/bench/results.csv"
OUT = ROOT / "docs/img"

THEMES = {
    "light": dict(surface="#fcfcfb", border="rgba(11,11,11,0.10)", primary="#0b0b0b",
                  secondary="#52514e", muted="#898781", grid="#e1e0d9", axis="#c3c2b7",
                  gpu="#2a78d6", cpu="#eb6834", flush="#1baf7a"),
    "dark": dict(surface="#1a1a19", border="rgba(255,255,255,0.10)", primary="#ffffff",
                 secondary="#c3c2b7", muted="#898781", grid="#2c2c2a", axis="#383835",
                 gpu="#3987e5", cpu="#d95926", flush="#199e70"),
}
NAMES = {"gpu": "GPU poller (CUDA)", "cpu": "CPU fallback (host thread)",
         "flush": "H2D flush (simulator)"}
FONT = 'system-ui, -apple-system, &quot;Segoe UI&quot;, sans-serif'

W, H = 720, 400
PLOT_L, PLOT_T, PLOT_B = 64, 112, H - 52


#data
def load():
    rows = list(csv.DictReader(open(DATA)))
    groups = defaultdict(list)
    for r in rows:
        groups[(r["scenario"], r["backend"], int(r["queues"]))].append(r)
    return groups


def median(groups, scenario, backend, column):
    """{queues: median value} for one scenario/backend."""
    out = {}
    for (s, b, q), rs in groups.items():
        if s == scenario and b == backend:
            if column == "cores_polling":
                vals = [float(r["cores_poll"]) + float(r["cores_other"]) for r in rs]
            else:
                vals = [float(r[column]) for r in rs]
            out[q] = statistics.median(vals)
    return dict(sorted(out.items()))


#svg helper
def nice_ticks(vmax, target=5):
    raw = vmax / target
    mag = 10 ** math.floor(math.log10(raw))
    step = next(m * mag for m in (1, 2, 2.5, 5, 10) if m * mag >= raw)
    top = step * math.ceil(vmax / step)
    n = int(round(top / step))
    return [i * step for i in range(n + 1)]


def fmt(v, digits=None):
    if digits is not None:
        return f"{v:.{digits}f}"
    if v == 0:
        return "0"
    if abs(v) >= 100:
        return f"{v:,.0f}"
    if abs(v) >= 10:
        return f"{v:.1f}".rstrip("0").rstrip(".")
    return f"{v:.2f}".rstrip("0").rstrip(".")


def text(x, y, s, fill, size=12, anchor="start", weight=400):
    return (f'<text x="{x:.1f}" y="{y:.1f}" fill="{fill}" font-size="{size}" '
            f'font-weight="{weight}" text-anchor="{anchor}">{escape(s)}</text>')


def column(x, y_top, y_base, w, fill):
    """Column with a 4px rounded data-end, square at the baseline."""
    h = y_base - y_top
    r = min(4.0, h / 2, w / 2)
    return (f'<path d="M{x:.1f},{y_base:.1f} V{y_top + r:.1f} Q{x:.1f},{y_top:.1f} '
            f'{x + r:.1f},{y_top:.1f} H{x + w - r:.1f} Q{x + w:.1f},{y_top:.1f} '
            f'{x + w:.1f},{y_top + r:.1f} V{y_base:.1f} Z" fill="{fill}"/>')


def frame(t, title, subtitle, series, y_label, desc):
    """Card, title, subtitle, legend (only for >= 2 series), y-axis label."""
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
        f'viewBox="0 0 {W} {H}" role="img" aria-labelledby="title desc" '
        f'font-family="{FONT}">',
        f'<title id="title">{escape(title)}</title>',
        f'<desc id="desc">{escape(desc)}</desc>',
        f'<rect x="0.5" y="0.5" width="{W - 1}" height="{H - 1}" rx="12" '
        f'fill="{t["surface"]}" stroke="{t["border"]}"/>',
        text(24, 36, title, t["primary"], 16, weight=600),
        text(24, 58, subtitle, t["secondary"], 13),
    ]
    if len(series) > 1:
        x = 24
        for key in series:
            parts.append(f'<rect x="{x}" y="72" width="12" height="12" rx="3" fill="{t[key]}"/>')
            parts.append(text(x + 18, 82, NAMES[key], t["secondary"], 12))
            x += 18 + 6.2 * len(NAMES[key]) + 24  # ~6 px per character at 12 px
    parts.append(text(24, PLOT_T - 16, y_label, t["muted"], 12))
    return parts


def y_axis(t, ticks, plot_r, y_of, suffix=""):
    parts = []
    for v in ticks:
        y = y_of(v)
        color = t["axis"] if v == 0 else t["grid"]
        parts.append(f'<line x1="{PLOT_L}" y1="{y:.1f}" x2="{plot_r}" y2="{y:.1f}" '
                     f'stroke="{color}" stroke-width="1"/>')
        parts.append(f'<text x="{PLOT_L - 10}" y="{y + 4:.1f}" fill="{t["muted"]}" font-size="12" '
                     f'text-anchor="end" style="font-variant-numeric: tabular-nums">'
                     f'{escape(fmt(v) + suffix)}</text>')
    return parts


#chart forms
def grouped_columns(theme, title, subtitle, y_label, x_title, cats, data, desc,
                    label="last", digits=None):
    """data: {series_key: {cat: value}}; cats: ordered x categories.

    label: "last" labels only the rightmost group (multi-series: selective
    labels, the axis and table carry the rest); "all" labels every column
    (fine for a single short series)."""
    t = THEMES[theme]
    series = list(data)
    plot_r = W - 24
    vmax = max(v for s in data.values() for v in s.values())
    ticks = nice_ticks(vmax)
    y_base, y_top = PLOT_B, PLOT_T

    def y_of(v):
        return y_base - (v / ticks[-1]) * (y_base - y_top)

    parts = frame(t, title, subtitle, series, y_label, desc)
    parts += y_axis(t, ticks, plot_r, y_of)

    band = (plot_r - PLOT_L) / len(cats)
    bar_w = min(24.0, band * 0.3)
    gap = 2.0
    group_w = len(series) * bar_w + (len(series) - 1) * gap
    for i, cat in enumerate(cats):
        cx = PLOT_L + band * (i + 0.5)
        x0 = cx - group_w / 2
        for j, key in enumerate(series):
            v = data[key].get(cat)
            if v is None:
                continue
            x = x0 + j * (bar_w + gap)
            top = min(y_of(v), y_base - 2)  # a zero keeps a 2px stub so it reads as 0, not missing
            parts.append(column(x, top, y_base, bar_w, t[key]))
            if label == "all" or (label == "last" and i == len(cats) - 1):
                parts.append(text(x + bar_w / 2, top - 6, fmt(v, digits), t["primary"], 12,
                                  anchor="middle", weight=600))
        parts.append(text(cx, y_base + 20, str(cat), t["muted"], 12, anchor="middle"))
    parts.append(text((PLOT_L + plot_r) / 2, y_base + 40, x_title, t["secondary"], 12,
                      anchor="middle"))
    parts.append("</svg>")
    return "\n".join(parts)


def lines(theme, title, subtitle, y_label, x_title, xs, data, desc, unit=""):
    """data: {series_key: {x: value}}; direct value labels at the line ends."""
    t = THEMES[theme]
    series = list(data)
    plot_r = W - 110  # room for end labels
    vmax = max(v for s in data.values() for v in s.values())
    ticks = nice_ticks(vmax)
    y_base, y_top = PLOT_B, PLOT_T

    def y_of(v):
        return y_base - (v / ticks[-1]) * (y_base - y_top)

    def x_of(x):
        return PLOT_L + 24 + (x - xs[0]) / (xs[-1] - xs[0]) * (plot_r - PLOT_L - 48)

    parts = frame(t, title, subtitle, series, y_label, desc)
    parts += y_axis(t, ticks, plot_r, y_of)
    for x in xs:
        parts.append(text(x_of(x), y_base + 20, str(x), t["muted"], 12, anchor="middle"))
    parts.append(text((PLOT_L + plot_r) / 2, y_base + 40, x_title, t["secondary"], 12,
                      anchor="middle"))

    for key in series:
        pts = [(x_of(x), y_of(v)) for x, v in sorted(data[key].items())]
        d = " ".join(f"{'M' if i == 0 else 'L'}{px:.1f},{py:.1f}" for i, (px, py) in enumerate(pts))
        parts.append(f'<path d="{d}" fill="none" stroke="{t[key]}" stroke-width="2" '
                     f'stroke-linejoin="round" stroke-linecap="round"/>')
        for px, py in pts:
            parts.append(f'<circle cx="{px:.1f}" cy="{py:.1f}" r="4" fill="{t[key]}" '
                         f'stroke="{t["surface"]}" stroke-width="2"/>')
        last_x, last_v = max(data[key].items())
        parts.append(text(x_of(last_x) + 12, y_of(last_v) + 4, f"{fmt(last_v)}{unit}",
                          t["primary"], 12, weight=600))
    parts.append("</svg>")
    return "\n".join(parts)


#charts
def main():
    if not DATA.exists():
        sys.exit(f"{DATA} not found - run scripts/bench.py first")
    g = load()
    OUT.mkdir(parents=True, exist_ok=True)

    cpu_cores = {b: median(g, "paced", b, "cores_polling") for b in ("gpu", "cpu")}
    latency = {b: median(g, "paced", b, "lat_mean_us") for b in ("gpu", "cpu")}
    tput = {b: median(g, "unpaced", b, "achieved_mpps") for b in ("gpu", "cpu")}
    split = {"gpu": median(g, "split", "gpu", "lat_mean_us")}
    # The GPU mean beside its sampled H2D flush, both from the "copy" runs
    # (--copy-timing). Side by side, not stacked: the flush reads slightly high
    # (see metrics.cpp), so it can exceed the mean, and a stack would hide that.
    copy_mean = median(g, "copy", "gpu", "lat_mean_us")
    flush = median(g, "copy", "gpu", "copy_us")
    split_bars = {"gpu": copy_mean, "flush": flush, "cpu": latency["cpu"]}
    queues = sorted(cpu_cores["cpu"])
    split_q = sorted(split["gpu"])
    split_cats = [f"{q} × {400 // q}k" for q in split_q]
    split_data = {"gpu": {c: split["gpu"][q] for c, q in zip(split_cats, split_q)}}

    def desc(d):
        return "; ".join(f"{NAMES[b]}: " + ", ".join(f"{k} -> {fmt(v)}" for k, v in s.items())
                         for b, s in d.items())

    charts = {
        "latency-split": lambda th: grouped_columns(
            th, "The GPU's latency is almost all H2D flush",
            "Mean detection latency beside the sampled flush time, 50 kpps per queue.",
            "µs", "queues", queues, split_bars, desc(split_bars), digits=2),
        "host-cpu": lambda th: grouped_columns(
            th, "Host CPU spent on polling",
            "Cores busy in the poll path, 50 kpps per queue. The GPU poller uses none.",
            "cores", "queues", queues, cpu_cores, desc(cpu_cores), digits=2),
        "latency": lambda th: grouped_columns(
            th, "Detection latency, publish to observe",
            "Mean per run, 50 kpps per queue. The GPU path includes a PCIe copy the CPU path skips.",
            "µs", "queues", queues, latency, desc(latency), digits=2),
        "throughput": lambda th: lines(
            th, "Throughput, unpaced",
            "Packets delivered per second, 64-entry rings. Both backends scale with queues.",
            "Mpps", "queues", queues, tput, desc(tput), unit=" Mpps"),
        "multiqueue-latency": lambda th: grouped_columns(
            th, "GPU: same 400 kpps, split over more queues",
            "Mean detection latency. Each queue has its own producer, copy stream and poller.",
            "µs", "queues × rate per queue", split_cats, split_data, desc(split_data),
            label="all", digits=1),
    }
    for name, make in charts.items():
        for th in THEMES:
            svg = make(th)
            suffix = "" if th == "light" else "-dark"
            (OUT / f"{name}{suffix}.svg").write_text(svg + "\n")
    print(f"wrote {len(charts) * len(THEMES)} SVGs to {OUT}\n")

    # Tables for the README (the charts' table-view twins)
    print("| queues | host cores polling (GPU / CPU) | mean latency µs (GPU / CPU) "
          "| unpaced Mpps (GPU / CPU) |")
    print("|---|---|---|---|")
    for q in queues:
        print(f"| {q} | {cpu_cores['gpu'][q]:.2f} / {cpu_cores['cpu'][q]:.2f} "
              f"| {latency['gpu'][q]:.1f} / {latency['cpu'][q]:.2f} "
              f"| {tput['gpu'][q]:.1f} / {tput['cpu'][q]:.1f} |")
    print("\n| queues | GPU mean µs | GPU H2D flush µs | CPU mean µs |")
    print("|---|---|---|---|")
    for q in queues:
        print(f"| {q} | {copy_mean[q]:.2f} | {flush[q]:.2f} | {latency['cpu'][q]:.2f} |")
    print("\n| queues × rate | mean latency µs |\n|---|---|")
    for c in split_cats:
        print(f"| {c} | {split_data['gpu'][c]:.1f} |")


if __name__ == "__main__":
    main()
