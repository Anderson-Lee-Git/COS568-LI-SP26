"""
Milestone 2 analysis script.

Reads benchmark CSV results for the two mixed workloads (10% insert, 90% insert)
on the Facebook dataset and generates four bar plots:

  Plot 1 — Mixed throughput, 10% insert workload  (DPGM / LIPP / Hybrid)
  Plot 2 — Mixed throughput, 90% insert workload  (DPGM / LIPP / Hybrid)
  Plot 3 — Index size (MB), 10% insert workload   (DPGM / LIPP / Hybrid)
  Plot 4 — Index size (MB), 90% insert workload   (DPGM / LIPP / Hybrid)

Run from the project root:
    python scripts/analysis_hybrid.py
"""

import os
import sys
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
RESULTS_DIR = "./results"
OUTPUT_DIR  = "./analysis_results"
os.makedirs(OUTPUT_DIR, exist_ok=True)

DATASET = "fb_100M_public_uint64"
WORKLOADS = {
    "10% insert (90% lookup)": f"{DATASET}_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv",
    "90% insert (10% lookup)": f"{DATASET}_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix_results_table.csv",
}

THROUGHPUT_COLS = ["mixed_throughput_mops1", "mixed_throughput_mops2", "mixed_throughput_mops3"]
INDEX_SIZE_COL  = "index_size_bytes"

# Map index_name column values → display labels
INDEX_MAP = {
    "DynamicPGM":   "DPGM",
    "LIPP":         "LIPP",
    "HybridPGMLipp": "Hybrid",
}
DISPLAY_ORDER = ["DPGM", "LIPP", "Hybrid"]
COLORS        = {"DPGM": "#4C72B0", "LIPP": "#DD8452", "Hybrid": "#55A868"}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def best_row(df: pd.DataFrame, index_name: str) -> pd.Series | None:
    """Return the row with the highest average throughput for a given index."""
    rows = df[df["index_name"] == index_name]
    if rows.empty:
        return None
    rows = rows.copy()
    rows["_avg_tput"] = rows[THROUGHPUT_COLS].mean(axis=1)
    return rows.loc[rows["_avg_tput"].idxmax()]


def load_workload(csv_name: str):
    path = os.path.join(RESULTS_DIR, csv_name)
    if not os.path.isfile(path):
        print(f"WARNING: result file not found: {path}", file=sys.stderr)
        return None
    # The CSV header has 10 columns but HybridPGMLipp rows have 11
    # (search_method + pgm_error + flush per-mille), so we supply an
    # explicit wider name list and skip the embedded header row.
    cols = [
        "index_name",
        "build_time_ns1", "build_time_ns2", "build_time_ns3",
        "index_size_bytes",
        "mixed_throughput_mops1", "mixed_throughput_mops2", "mixed_throughput_mops3",
        "search_method", "value", "value2",
    ]
    return pd.read_csv(path, names=cols, skiprows=1)


# ---------------------------------------------------------------------------
# Collect data
# ---------------------------------------------------------------------------

throughput_data: dict[str, dict[str, float]] = {wl: {} for wl in WORKLOADS}
size_data:       dict[str, dict[str, float]] = {wl: {} for wl in WORKLOADS}

for wl_label, csv_name in WORKLOADS.items():
    df = load_workload(csv_name)
    if df is None:
        continue

    for raw_name, display_name in INDEX_MAP.items():
        row = best_row(df, raw_name)
        if row is None:
            print(f"  [{wl_label}] index '{raw_name}' not found in results.", file=sys.stderr)
            continue

        avg_tput = row[THROUGHPUT_COLS].mean()
        size_mb  = row[INDEX_SIZE_COL] / (1024 ** 2)

        throughput_data[wl_label][display_name] = avg_tput
        size_data[wl_label][display_name]       = size_mb

        print(f"  [{wl_label}] {display_name:12s}  tput={avg_tput:.3f} Mops/s  "
              f"size={size_mb:.1f} MB")


# ---------------------------------------------------------------------------
# Plotting
# ---------------------------------------------------------------------------

def bar_plot(ax, data_by_workload: dict[str, dict[str, float]],
             ylabel: str, title: str, fmt_fn=None):
    """Draw grouped bars: one group per workload, one bar per index."""
    workloads = list(data_by_workload.keys())
    n_groups  = len(workloads)
    n_bars    = len(DISPLAY_ORDER)
    bar_w     = 0.22
    group_gap = 0.1
    group_w   = n_bars * bar_w + group_gap

    x_centers = np.arange(n_groups) * group_w
    offsets    = np.linspace(-(n_bars - 1) / 2, (n_bars - 1) / 2, n_bars) * bar_w

    for idx_name, offset in zip(DISPLAY_ORDER, offsets):
        values = [data_by_workload[wl].get(idx_name, 0) for wl in workloads]
        bars = ax.bar(x_centers + offset, values, bar_w,
                      label=idx_name, color=COLORS[idx_name],
                      edgecolor="white", linewidth=0.5)
        for bar, val in zip(bars, values):
            if val > 0:
                label = fmt_fn(val) if fmt_fn else f"{val:.2f}"
                ax.text(bar.get_x() + bar.get_width() / 2,
                        bar.get_height() + ax.get_ylim()[1] * 0.01,
                        label, ha="center", va="bottom", fontsize=7, rotation=0)

    ax.set_xticks(x_centers)
    ax.set_xticklabels(workloads, fontsize=9)
    ax.set_ylabel(ylabel, fontsize=10)
    ax.set_title(title, fontsize=11, fontweight="bold")
    ax.legend(fontsize=9)
    ax.yaxis.set_minor_locator(ticker.AutoMinorLocator())
    ax.grid(axis="y", linestyle="--", alpha=0.5)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


fig, axes = plt.subplots(1, 2, figsize=(14, 5))
fig.suptitle("Milestone 2 — Facebook Dataset: Mixed Workload Comparison",
             fontsize=13, fontweight="bold", y=1.02)

bar_plot(axes[0], throughput_data,
         ylabel="Throughput (Mops/s)",
         title="Mixed Throughput",
         fmt_fn=lambda v: f"{v:.2f}")

bar_plot(axes[1], size_data,
         ylabel="Index Size (MB)",
         title="Index Size",
         fmt_fn=lambda v: f"{v:.0f}")

# Recompute y-limits with 20% headroom so value labels don't clip.
for ax in axes:
    current_top = ax.get_ylim()[1]
    ax.set_ylim(0, current_top * 1.25)

plt.tight_layout()

out_path = os.path.join(OUTPUT_DIR, "hybrid_benchmark_results.png")
plt.savefig(out_path, dpi=150, bbox_inches="tight")
print(f"\nPlot saved to: {out_path}")
plt.show()


# ---------------------------------------------------------------------------
# Individual plots (one per workload)  — useful for the report
# ---------------------------------------------------------------------------

for wl_label in WORKLOADS:
    fig2, (ax_tput, ax_size) = plt.subplots(1, 2, figsize=(10, 4))
    fig2.suptitle(f"Facebook — {wl_label}", fontsize=12, fontweight="bold")

    # Throughput
    names  = DISPLAY_ORDER
    tputs  = [throughput_data[wl_label].get(n, 0) for n in names]
    sizes  = [size_data[wl_label].get(n, 0)       for n in names]
    x      = np.arange(len(names))
    bar_w  = 0.45

    ax_tput.bar(x, tputs, bar_w, color=[COLORS[n] for n in names], edgecolor="white")
    ax_tput.set_xticks(x)
    ax_tput.set_xticklabels(names, fontsize=10)
    ax_tput.set_ylabel("Throughput (Mops/s)", fontsize=10)
    ax_tput.set_title("Mixed Throughput", fontsize=11)
    ax_tput.set_ylim(0, max(tputs or [1]) * 1.3)
    for xi, val in zip(x, tputs):
        if val > 0:
            ax_tput.text(xi, val + max(tputs) * 0.02, f"{val:.2f}",
                         ha="center", fontsize=9)
    ax_tput.grid(axis="y", linestyle="--", alpha=0.5)
    ax_tput.spines["top"].set_visible(False)
    ax_tput.spines["right"].set_visible(False)

    ax_size.bar(x, sizes, bar_w, color=[COLORS[n] for n in names], edgecolor="white")
    ax_size.set_xticks(x)
    ax_size.set_xticklabels(names, fontsize=10)
    ax_size.set_ylabel("Index Size (MB)", fontsize=10)
    ax_size.set_title("Index Size", fontsize=11)
    ax_size.set_ylim(0, max(sizes or [1]) * 1.3)
    for xi, val in zip(x, sizes):
        if val > 0:
            ax_size.text(xi, val + max(sizes) * 0.02, f"{val:.0f}",
                         ha="center", fontsize=9)
    ax_size.grid(axis="y", linestyle="--", alpha=0.5)
    ax_size.spines["top"].set_visible(False)
    ax_size.spines["right"].set_visible(False)

    plt.tight_layout()
    safe_label = wl_label.replace("%", "pct").replace(" ", "_").replace("(", "").replace(")", "").replace("/", "_")
    out2 = os.path.join(OUTPUT_DIR, f"hybrid_{safe_label}.png")
    plt.savefig(out2, dpi=150, bbox_inches="tight")
    print(f"Plot saved to: {out2}")
    plt.close(fig2)
