"""
Generate booktabs LaTeX tables for the final Milestone 3 report.

The tables are derived directly from the raw benchmark CSVs in ./results. The
first table lists every Bloom-hybrid configuration searched for the final
workloads. The second table selects the hyperparameter row with the highest mean
mixed throughput across the three repeated runs for each dataset, workload, and
index.

Run from the project root:
    python scripts/make_final_latex_table.py
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass
from typing import Any

import numpy as np
import pandas as pd


RESULTS_DIR = "./results"
OUTPUT_DIR = "./analysis_results"
OUTPUT_PATH = os.path.join(OUTPUT_DIR, "final_hyperparams_table.tex")
CONFIG_OUTPUT_PATH = os.path.join(OUTPUT_DIR, "final_config_search_table.tex")

DATASETS = {
    "FB": "fb_100M_public_uint64",
    "Books": "books_100M_public_uint64",
    "OSMC": "osmc_100M_public_uint64",
}

WORKLOADS = {
    "10\\% ins.": "ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix",
    "90\\% ins.": "ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix",
}

INDEX_MAP = {
    "DynamicPGM": "DPGM",
    "LIPP": "LIPP",
    "HybridPGMLipp": "Naive",
    "BloomHybridPGMLipp": "Bloom",
}

INDEX_ORDER = ["DynamicPGM", "LIPP", "HybridPGMLipp", "BloomHybridPGMLipp"]

CSV_COLS = [
    "index_name",
    "build_time_ns1",
    "build_time_ns2",
    "build_time_ns3",
    "index_size_bytes",
    "mixed_throughput_mops1",
    "mixed_throughput_mops2",
    "mixed_throughput_mops3",
    "search_method",
    "value",
    "value2",
    "value3",
    "value4",
]

THROUGHPUT_COLS = [
    "mixed_throughput_mops1",
    "mixed_throughput_mops2",
    "mixed_throughput_mops3",
]


@dataclass
class TableRow:
    dataset: str
    workload: str
    index: str
    throughput_mean: float
    throughput_std: float
    size_gb: float
    search_method: str
    pgm_error: str
    flush_per_mille: str
    bloom_bits: str
    bloom_hashes: str


@dataclass
class ConfigSearchRow:
    workload: str
    pgm_error: str
    flush_per_mille: str
    bloom_bits: str
    bloom_hashes: str
    dataset_count: int
    mean_throughput: float
    min_throughput: float
    max_throughput: float


def csv_path(dataset_name: str, workload_suffix: str) -> str:
    filename = f"{dataset_name}_{workload_suffix}_results_table.csv"
    return os.path.join(RESULTS_DIR, filename)


def load_csv(path: str) -> pd.DataFrame | None:
    if not os.path.isfile(path):
        print(f"WARNING: result file not found: {path}", file=sys.stderr)
        return None
    return pd.read_csv(path, names=CSV_COLS, skiprows=1)


def optional_int(value: Any) -> str:
    try:
        if pd.isna(value):
            return "--"
        return str(int(float(value)))
    except (TypeError, ValueError):
        return "--"


def clean_search_method(value: Any) -> str:
    if value is None or pd.isna(value):
        return "--"
    text = str(value)
    return "--" if text == "nan" or text == "" else text


def decode_params(row: pd.Series) -> tuple[str, str, str, str]:
    index_name = row["index_name"]
    if index_name == "DynamicPGM":
        return optional_int(row.get("value")), "--", "--", "--"
    if index_name == "HybridPGMLipp":
        return optional_int(row.get("value")), optional_int(row.get("value2")), "--", "--"
    if index_name == "BloomHybridPGMLipp":
        return (
            optional_int(row.get("value")),
            optional_int(row.get("value2")),
            optional_int(row.get("value3")),
            optional_int(row.get("value4")),
        )
    return "--", "--", "--", "--"


def best_row(df: pd.DataFrame, index_name: str) -> pd.Series | None:
    rows = df[df["index_name"] == index_name].copy()
    if rows.empty:
        return None
    rows["_avg_tput"] = rows[THROUGHPUT_COLS].mean(axis=1)
    return rows.loc[rows["_avg_tput"].idxmax()]


def collect_rows() -> list[TableRow]:
    table_rows: list[TableRow] = []
    for dataset_label, dataset_name in DATASETS.items():
        for workload_label, workload_suffix in WORKLOADS.items():
            df = load_csv(csv_path(dataset_name, workload_suffix))
            if df is None:
                continue

            for index_name in INDEX_ORDER:
                row = best_row(df, index_name)
                if row is None:
                    print(
                        f"WARNING: {index_name} missing for {dataset_label} "
                        f"{workload_label}",
                        file=sys.stderr,
                    )
                    continue

                tputs = np.array([float(row[col]) for col in THROUGHPUT_COLS])
                pgm_error, flush_per_mille, bloom_bits, bloom_hashes = decode_params(row)
                table_rows.append(
                    TableRow(
                        dataset=dataset_label,
                        workload=workload_label,
                        index=INDEX_MAP[index_name],
                        throughput_mean=float(np.mean(tputs)),
                        throughput_std=float(np.std(tputs, ddof=1)),
                        size_gb=float(row["index_size_bytes"]) / (1024 ** 3),
                        search_method=clean_search_method(row.get("search_method")),
                        pgm_error=pgm_error,
                        flush_per_mille=flush_per_mille,
                        bloom_bits=bloom_bits,
                        bloom_hashes=bloom_hashes,
                    )
                )
    return table_rows


def collect_config_search_rows() -> list[ConfigSearchRow]:
    grouped: dict[tuple[str, str, str, str, str], list[float]] = {}

    for _dataset_label, dataset_name in DATASETS.items():
        for workload_label, workload_suffix in WORKLOADS.items():
            df = load_csv(csv_path(dataset_name, workload_suffix))
            if df is None:
                continue

            bloom_rows = df[df["index_name"] == "BloomHybridPGMLipp"].copy()
            for _idx, row in bloom_rows.iterrows():
                pgm_error, flush_per_mille, bloom_bits, bloom_hashes = decode_params(row)
                key = (
                    workload_label,
                    pgm_error,
                    flush_per_mille,
                    bloom_bits,
                    bloom_hashes,
                )
                tputs = np.array([float(row[col]) for col in THROUGHPUT_COLS])
                grouped.setdefault(key, []).append(float(np.mean(tputs)))

    rows: list[ConfigSearchRow] = []
    for key, means in grouped.items():
        workload_label, pgm_error, flush_per_mille, bloom_bits, bloom_hashes = key
        rows.append(
            ConfigSearchRow(
                workload=workload_label,
                pgm_error=pgm_error,
                flush_per_mille=flush_per_mille,
                bloom_bits=bloom_bits,
                bloom_hashes=bloom_hashes,
                dataset_count=len(means),
                mean_throughput=float(np.mean(means)),
                min_throughput=float(np.min(means)),
                max_throughput=float(np.max(means)),
            )
        )

    return sorted(
        rows,
        key=lambda row: (
            row.workload,
            int(row.pgm_error),
            int(row.flush_per_mille),
            int(row.bloom_bits),
            int(row.bloom_hashes),
        ),
    )


def latex_escape(text: str) -> str:
    return (
        text.replace("\\", "\\textbackslash{}")
        .replace("&", "\\&")
        .replace("%", "\\%")
        .replace("_", "\\_")
        .replace("#", "\\#")
    )


def render_table(rows: list[TableRow]) -> str:
    lines = [
        "\\begin{table*}[t]",
        "\\centering",
        "\\small",
        "\\setlength{\\tabcolsep}{3pt}",
        "\\caption{Best observed configurations selected by mean mixed throughput over three runs. Flush is reported in per-mille of the initial bulk-loaded key count; Bloom columns apply only to the Bloom hybrid.}",
        "\\label{tab:final-hyperparams}",
        "\\resizebox{\\textwidth}{!}{%",
        "\\begin{tabular}{lllrrrllll}",
        "\\toprule",
        "Dataset & Workload & Index & Throughput & Std. & Size & Search & $\\epsilon$ & Flush & Bloom \\\\",
        " & & & (Mops/s) & & (GB) & & & (\\permil) & bits/hash \\\\",
        "\\midrule",
    ]

    previous_group: tuple[str, str] | None = None
    for row in rows:
        group = (row.dataset, row.workload)
        if previous_group is not None and group != previous_group:
            lines.append("\\addlinespace")
        previous_group = group

        bloom = "--"
        if row.bloom_bits != "--" and row.bloom_hashes != "--":
            bloom = f"{row.bloom_bits}/{row.bloom_hashes}"

        lines.append(
            " & ".join(
                [
                    latex_escape(row.dataset),
                    row.workload,
                    latex_escape(row.index),
                    f"{row.throughput_mean:.2f}",
                    f"{row.throughput_std:.2f}",
                    f"{row.size_gb:.2f}",
                    latex_escape(row.search_method),
                    row.pgm_error,
                    row.flush_per_mille,
                    bloom,
                ]
            )
            + " \\\\"
        )

    lines.extend(
        [
            "\\bottomrule",
            "\\end{tabular}%",
            "}",
            "\\end{table*}",
            "",
        ]
    )
    return "\n".join(lines)


def render_config_search_table(rows: list[ConfigSearchRow]) -> str:
    lines = [
        "\\begin{table*}[t]",
        "\\centering",
        "\\small",
        "\\caption{Bloom-hybrid configurations searched in the final benchmark. Throughput columns summarize the per-dataset mean throughput for each configuration after averaging the three repeated runs within each dataset.}",
        "\\label{tab:config-search}",
        "\\begin{tabular}{lllllrrrr}",
        "\\toprule",
        "Workload & $\\epsilon$ & Flush (\\permil) & Bits/key & Hashes & Datasets & Mean & Min & Max \\\\",
        " & & & & & & \\multicolumn{3}{c}{Throughput (Mops/s)} \\\\",
        "\\midrule",
    ]

    previous_workload: str | None = None
    for row in rows:
        if previous_workload is not None and row.workload != previous_workload:
            lines.append("\\addlinespace")
        previous_workload = row.workload

        lines.append(
            " & ".join(
                [
                    row.workload,
                    row.pgm_error,
                    row.flush_per_mille,
                    row.bloom_bits,
                    row.bloom_hashes,
                    str(row.dataset_count),
                    f"{row.mean_throughput:.2f}",
                    f"{row.min_throughput:.2f}",
                    f"{row.max_throughput:.2f}",
                ]
            )
            + " \\\\"
        )

    lines.extend(
        [
            "\\bottomrule",
            "\\end{tabular}",
            "\\end{table*}",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> None:
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    rows = collect_rows()
    config_rows = collect_config_search_rows()
    if not rows:
        print("No result rows found.", file=sys.stderr)
        sys.exit(1)
    if not config_rows:
        print("No BloomHybridPGMLipp configuration rows found.", file=sys.stderr)
        sys.exit(1)
    with open(CONFIG_OUTPUT_PATH, "w", encoding="utf-8") as file:
        file.write(render_config_search_table(config_rows))
    with open(OUTPUT_PATH, "w", encoding="utf-8") as file:
        file.write(render_table(rows))
    print(f"Configuration search table saved to: {CONFIG_OUTPUT_PATH}")
    print(f"LaTeX table saved to: {OUTPUT_PATH}")


if __name__ == "__main__":
    main()
