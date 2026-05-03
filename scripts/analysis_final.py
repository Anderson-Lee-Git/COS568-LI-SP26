"""
Final milestone analysis script.

Reads the mixed-workload CSVs produced by scripts/run_final_benchmark.sh and
compares DynamicPGM, LIPP, the naive hybrid, and the Bloom hybrid across all
three datasets.

Outputs:
  analysis_results/final_summary.csv
  analysis_results/final_<dataset>_<workload>_throughput.png
  analysis_results/final_<dataset>_<workload>_size.png

Run from the project root:
    python scripts/analysis_final.py
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


RESULTS_DIR = "./results"
OUTPUT_DIR = "./analysis_results"
os.makedirs(OUTPUT_DIR, exist_ok=True)

DATASETS = {
    "FB": "fb_100M_public_uint64",
    "Books": "books_100M_public_uint64",
    "OSMC": "osmc_100M_public_uint64",
}

WORKLOADS = {
    "10pct_insert_90pct_lookup": {
        "label": "10% Insert / 90% Lookup",
        "suffix": "ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix",
    },
    "90pct_insert_10pct_lookup": {
        "label": "90% Insert / 10% Lookup",
        "suffix": "ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix",
    },
}

INDEX_MAP = {
    "DynamicPGM": "DPGM",
    "LIPP": "LIPP",
    "HybridPGMLipp": "Naive Hybrid",
    "BloomHybridPGMLipp": "Bloom Hybrid",
}
DISPLAY_ORDER = ["DPGM", "LIPP", "Naive Hybrid", "Bloom Hybrid"]
COLORS = {
    "DPGM": "#4C72B0",
    "LIPP": "#DD8452",
    "Naive Hybrid": "#55A868",
    "Bloom Hybrid": "#8172B3",
}

CSV_COLS = [
    "index_name",
    "build_time_ns1", "build_time_ns2", "build_time_ns3",
    "index_size_bytes",
    "mixed_throughput_mops1", "mixed_throughput_mops2", "mixed_throughput_mops3",
    "search_method", "value", "value2", "value3", "value4",
]
THROUGHPUT_COLS = [
    "mixed_throughput_mops1",
    "mixed_throughput_mops2",
    "mixed_throughput_mops3",
]


@dataclass
class Result:
    dataset: str
    workload: str
    index: str
    throughput_mean: float
    throughput_std: float
    index_size_mb: float
    search_method: str
    value: float | None
    value2: float | None
    value3: float | None
    value4: float | None


def csv_path(dataset_name: str, workload_suffix: str) -> str:
    filename = f"{dataset_name}_{workload_suffix}_results_table.csv"
    return os.path.join(RESULTS_DIR, filename)


def load_csv(path: str) -> pd.DataFrame | None:
    if not os.path.isfile(path):
        print(f"WARNING: result file not found: {path}", file=sys.stderr)
        return None
    return pd.read_csv(path, names=CSV_COLS, skiprows=1)


def best_row(df: pd.DataFrame, index_name: str) -> pd.Series | None:
    rows = df[df["index_name"] == index_name].copy()
    if rows.empty:
        return None
    rows["_avg_tput"] = rows[THROUGHPUT_COLS].mean(axis=1)
    return rows.loc[rows["_avg_tput"].idxmax()]


def optional_float(value) -> float | None:
    try:
        if pd.isna(value):
            return None
        return float(value)
    except (TypeError, ValueError):
        return None


def collect_results() -> list[Result]:
    results: list[Result] = []

    for dataset_label, dataset_name in DATASETS.items():
        for workload_key, workload_cfg in WORKLOADS.items():
            df = load_csv(csv_path(dataset_name, workload_cfg["suffix"]))
            if df is None:
                continue

            for raw_index, display_index in INDEX_MAP.items():
                row = best_row(df, raw_index)
                if row is None:
                    print(
                        f"WARNING: {raw_index} missing for {dataset_label} "
                        f"{workload_cfg['label']}",
                        file=sys.stderr,
                    )
                    continue

                tputs = [float(row[col]) for col in THROUGHPUT_COLS]
                results.append(
                    Result(
                        dataset=dataset_label,
                        workload=workload_key,
                        index=display_index,
                        throughput_mean=float(np.mean(tputs)),
                        throughput_std=float(np.std(tputs, ddof=1)),
                        index_size_mb=float(row["index_size_bytes"]) / (1024 ** 2),
                        search_method=str(row.get("search_method", "")),
                        value=optional_float(row.get("value")),
                        value2=optional_float(row.get("value2")),
                        value3=optional_float(row.get("value3")),
                        value4=optional_float(row.get("value4")),
                    )
                )

    return results


def save_summary(results: list[Result]) -> None:
    rows = [result.__dict__ for result in results]
    path = os.path.join(OUTPUT_DIR, "final_summary.csv")
    pd.DataFrame(rows).to_csv(path, index=False)
    print(f"Summary saved to: {path}")


def plot_metric(results: list[Result], dataset: str, workload_key: str,
                metric: str, ylabel: str, title: str, output_name: str) -> None:
    subset = [
        result for result in results
        if result.dataset == dataset and result.workload == workload_key
    ]
    by_index = {result.index: result for result in subset}
    values = [
        getattr(by_index[index], metric) if index in by_index else 0
        for index in DISPLAY_ORDER
    ]

    fig, ax = plt.subplots(figsize=(7, 4))
    x = np.arange(len(DISPLAY_ORDER))
    bars = ax.bar(
        x,
        values,
        color=[COLORS[index] for index in DISPLAY_ORDER],
        edgecolor="white",
        linewidth=0.5,
    )

    ax.set_xticks(x)
    ax.set_xticklabels(DISPLAY_ORDER, rotation=15, ha="right")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(axis="y", linestyle="--", alpha=0.5)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    top = max(values) if values else 0
    ax.set_ylim(0, top * 1.25 if top > 0 else 1)
    for bar, value in zip(bars, values):
        if value > 0:
            label = f"{value:.2f}" if metric == "throughput_mean" else f"{value:.0f}"
            ax.text(
                bar.get_x() + bar.get_width() / 2,
                bar.get_height() + ax.get_ylim()[1] * 0.02,
                label,
                ha="center",
                va="bottom",
                fontsize=8,
            )

    plt.tight_layout()
    path = os.path.join(OUTPUT_DIR, output_name)
    plt.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"Plot saved to: {path}")


def make_plots(results: list[Result]) -> None:
    for dataset_label in DATASETS:
        safe_dataset = dataset_label.lower()
        for workload_key, workload_cfg in WORKLOADS.items():
            title_base = f"{dataset_label} - {workload_cfg['label']}"
            plot_metric(
                results,
                dataset_label,
                workload_key,
                metric="throughput_mean",
                ylabel="Throughput (Mops/s)",
                title=f"{title_base}: Mixed Throughput",
                output_name=f"final_{safe_dataset}_{workload_key}_throughput.png",
            )
            plot_metric(
                results,
                dataset_label,
                workload_key,
                metric="index_size_mb",
                ylabel="Index Size (MB)",
                title=f"{title_base}: Index Size",
                output_name=f"final_{safe_dataset}_{workload_key}_size.png",
            )


def main() -> None:
    results = collect_results()
    if not results:
        print("No final benchmark results found.", file=sys.stderr)
        sys.exit(1)

    for result in results:
        print(
            f"[{result.dataset}][{result.workload}] {result.index:13s} "
            f"tput={result.throughput_mean:.3f} Mops/s "
            f"size={result.index_size_mb:.1f} MB"
        )

    save_summary(results)
    make_plots(results)


if __name__ == "__main__":
    main()
