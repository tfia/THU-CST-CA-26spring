#!/usr/bin/env python3

import os
import re

ROOT = os.path.dirname(os.path.abspath(__file__))
RESULTS_DIR = os.path.join(ROOT, "results")
ASSETS_DIR = os.path.join(ROOT, "assets")
MPLCONFIGDIR = os.path.join(ROOT, ".mplconfig")

os.environ.setdefault("MPLCONFIGDIR", MPLCONFIGDIR)

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


SIZE_RE = re.compile(
    r"Size:\s*([0-9.]+)\s*KB,\s*Latency:\s*([0-9.]+)\s*ns/access,\s*Stride:\s*([0-9.]+)\s*B"
)
ASSOC_RE = re.compile(
    r"Size:\s*([0-9.]+)\s*KB,\s*Latency:\s*([0-9.]+)\s*ns/access,\s*(?:Blocks:\s*([0-9.]+),\s*)?(?:Stride:\s*([0-9.]+)\s*B,\s*)?Ways:\s*([0-9.]+)"
)
CACHE_RE = re.compile(
    r"^(L1d|L2|L3)\s+([0-9]+[KMG])\s+\S+\s+([0-9]+)\s+"
)
COLORS = ["#1f77b4", "#d62728", "#2ca02c", "#ff7f0e", "#9467bd", "#8c564b"]


def parse_size_to_kb(token: str) -> float:
    value = float(token[:-1])
    unit = token[-1].upper()
    if unit == "K":
        return value
    if unit == "M":
        return value * 1024
    if unit == "G":
        return value * 1024 * 1024
    raise ValueError(f"unsupported size token: {token}")


def parse_lscpu_cache_info(path):
    cache_sizes_kb = {}
    line_size_b = None
    associativity = None

    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()

    for raw_line in lines:
        line = raw_line.strip()
        cache_match = CACHE_RE.match(line)
        if cache_match:
            level = cache_match.group(1)
            size_token = cache_match.group(2)
            ways = int(cache_match.group(3))
            cache_sizes_kb[level] = parse_size_to_kb(size_token)
            if level == "L1d":
                associativity = ways

        fields = line.split()
        if fields and fields[0] == "L1d" and len(fields) >= 8:
            line_size_b = float(fields[-1])

    if "L1d" not in cache_sizes_kb or "L2" not in cache_sizes_kb or "L3" not in cache_sizes_kb:
        raise ValueError(f"failed to parse cache sizes from {path}")
    if line_size_b is None:
        raise ValueError(f"failed to parse cache line size from {path}")
    if associativity is None:
        raise ValueError(f"failed to parse L1d associativity from {path}")

    return {
        "cache_sizes_kb": cache_sizes_kb,
        "line_size_b": line_size_b,
        "associativity": float(associativity),
    }


def parse_cache_size(path):
    points = []
    stride_bytes = None
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    for line in lines:
        match = SIZE_RE.search(line)
        if not match:
            continue
        size_kb = float(match.group(1))
        latency = float(match.group(2))
        stride_bytes = float(match.group(3))
        points.append((size_kb, latency))

    if not points:
        raise ValueError(f"no cache-size data found in {path}")

    return {"label": f"Stride {int(stride_bytes)} B", "points": points}


def parse_cache_line(path):
    points = []
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    for line in lines:
        match = SIZE_RE.search(line)
        if not match:
            continue
        stride_bytes = float(match.group(3))
        latency = float(match.group(2))
        points.append((stride_bytes, latency))

    if not points:
        raise ValueError(f"no cache-line data found in {path}")

    return {"label": "Measured latency", "points": points}


def parse_assoc(path):
    points = []
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    for line in lines:
        match = ASSOC_RE.search(line)
        if not match:
            continue
        latency = float(match.group(2))
        ways = float(match.group(5))
        points.append((ways, latency))

    if not points:
        raise ValueError(f"no associativity data found in {path}")

    return {"label": "Measured latency", "points": points}


def add_reference_lines(ax, references, unit):
    for idx, (value, label) in enumerate(references):
        ax.axvline(value, color="red", linestyle="--", linewidth=1.5, alpha=0.85)
        ha = "left"
        x_offset = 6
        if value > sum(ax.get_xlim()) / 2:
            ha = "right"
            x_offset = -6

        ax.annotate(
            f"{label} = {value:g} {unit}",
            xy=(value, 1.0),
            xycoords=("data", "axes fraction"),
            xytext=(x_offset, -10 - idx * 18),
            textcoords="offset points",
            color="red",
            va="top",
            ha=ha,
            fontsize=9,
            bbox={"facecolor": "white", "edgecolor": "none", "alpha": 0.85, "pad": 1.5},
            clip_on=False,
        )


def set_decimal_log_ticks(ax, values):
    ticks = sorted({float(value) for value in values})
    ax.set_xticks(ticks)
    ax.xaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:g}"))
    ax.tick_params(axis="x", rotation=45)


def setup_axes(ax, title, x_label, y_label, log_x):
    ax.set_title(title)
    ax.set_xlabel(x_label)
    ax.set_ylabel(y_label)
    ax.grid(True, which="both", linestyle=":", linewidth=0.8, alpha=0.6)
    if log_x:
        ax.set_xscale("log", base=2)


def plot_cache_size(series, cache_info, output_path):
    fig, ax = plt.subplots(figsize=(12, 7))
    x_values = []
    for idx, item in enumerate(series):
        x = [point[0] for point in item["points"]]
        y = [point[1] for point in item["points"]]
        x_values.extend(x)
        ax.plot(x, y, marker="o", linewidth=2, markersize=5, color=COLORS[idx % len(COLORS)], label=item["label"])

    setup_axes(ax, "Cache Size Probe", "Working Set Size (KB)", "Latency (ns/access)", log_x=True)
    set_decimal_log_ticks(ax, x_values)
    add_reference_lines(
        ax,
        [
            (cache_info["cache_sizes_kb"]["L1d"], "L1d"),
            (cache_info["cache_sizes_kb"]["L2"], "L2"),
            (cache_info["cache_sizes_kb"]["L3"], "L3"),
        ],
        "KB",
    )
    ax.legend()
    fig.tight_layout()
    fig.savefig(output_path, dpi=200)
    plt.close(fig)


def plot_cache_line(series, cache_info, output_path):
    fig, ax = plt.subplots(figsize=(12, 7))
    item = series[0]
    x = [point[0] for point in item["points"]]
    y = [point[1] for point in item["points"]]
    ax.plot(x, y, marker="o", linewidth=2, markersize=6, color=COLORS[0], label=item["label"])

    setup_axes(ax, "Cache Line Probe", "Stride (B)", "Latency (ns/access)", log_x=True)
    set_decimal_log_ticks(ax, x)
    add_reference_lines(ax, [(cache_info["line_size_b"], "Line size")], "B")
    ax.legend()
    fig.tight_layout()
    fig.savefig(output_path, dpi=200)
    plt.close(fig)


def plot_associativity(series, cache_info, output_path):
    fig, ax = plt.subplots(figsize=(12, 7))
    item = series[0]
    x = [point[0] for point in item["points"]]
    y = [point[1] for point in item["points"]]
    ax.plot(x, y, marker="o", linewidth=2, markersize=6, color=COLORS[1], label=item["label"])

    setup_axes(ax, "Cache Associativity Probe", "Ways", "Latency (ns/access)", log_x=True)
    set_decimal_log_ticks(ax, x)
    add_reference_lines(ax, [(cache_info["associativity"], "L1d ways")], "ways")
    ax.legend()
    fig.tight_layout()
    fig.savefig(output_path, dpi=200)
    plt.close(fig)


def main():
    os.makedirs(ASSETS_DIR, exist_ok=True)
    os.makedirs(MPLCONFIGDIR, exist_ok=True)

    cache_info = parse_lscpu_cache_info(os.path.join(ROOT, "lscpu.log"))

    stride_files = sorted(
        os.path.join(RESULTS_DIR, name)
        for name in os.listdir(RESULTS_DIR)
        if re.fullmatch(r"results_stride_.*\.txt", name)
    )
    if not stride_files:
        raise SystemExit("no results_stride_*.txt files found")

    cache_size_series = [parse_cache_size(path) for path in stride_files]
    cache_line_series = [parse_cache_line(os.path.join(RESULTS_DIR, "results_cache_line.txt"))]
    assoc_series = [parse_assoc(os.path.join(RESULTS_DIR, "results_cache_assoc.txt"))]

    cache_size_path = os.path.join(ASSETS_DIR, "cache_size.png")
    cache_line_path = os.path.join(ASSETS_DIR, "cache_line.png")
    assoc_path = os.path.join(ASSETS_DIR, "cache_associativity.png")

    plot_cache_size(cache_size_series, cache_info, cache_size_path)
    plot_cache_line(cache_line_series, cache_info, cache_line_path)
    plot_associativity(assoc_series, cache_info, assoc_path)

    print(f"saved: {cache_size_path}")
    print(f"saved: {cache_line_path}")
    print(f"saved: {assoc_path}")


if __name__ == "__main__":
    main()
