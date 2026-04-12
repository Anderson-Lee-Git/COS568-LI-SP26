#!/usr/bin/env python3
"""Build LaTeX tables from benchmark CSV results.

Task-1 mode (default): reads analysis_results/*.csv (output of scripts/analysis.py)
  and prints or writes three combined tables.

Milestone-2 mode (--m2): reads results/ mixed-workload CSVs produced by
  scripts/run_hybrid_benchmark.sh and writes (or prints) table_m2.tex.
"""

from __future__ import annotations

import argparse
import math
import statistics
from pathlib import Path

import pandas as pd

# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

def fmt_mops(v: float) -> str:
    if pd.isna(v):
        return "---"
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return s if s else "0"


def latex_escape(s: str) -> str:
    return s.replace("%", r"\%")


# ---------------------------------------------------------------------------
# Task-1 table helpers (original functionality)
# ---------------------------------------------------------------------------

DATASET_ORDER = [("fb", "FB"), ("books", "Books"), ("osmc", "OSMC")]
INDEX_ORDER = ["BTree", "DynamicPGM", "LIPP"]
INDEX_DISPLAY = {
    "BTree": "B+Tree",
    "DynamicPGM": "DPGM",
    "LIPP": "LIPP",
}


def load_throughput_csv(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path, index_col=0)
    df.index = df.index.astype(str)
    return df


def cell_value(df: pd.DataFrame, dataset_key: str, index_key: str) -> float:
    return float(df.loc[dataset_key, index_key])


def format_column_with_best(values: list[float]) -> list[str]:
    """Format throughput cells; wrap in \\textbf when tied for max in this column."""
    finite = [float(v) for v in values if not pd.isna(v)]
    if not finite:
        return [fmt_mops(v) for v in values]
    best = max(finite)
    out: list[str] = []
    for v in values:
        s = fmt_mops(v)
        if not pd.isna(v) and math.isclose(float(v), best, rel_tol=1e-9, abs_tol=1e-12):
            out.append(rf"\textbf{{{s}}}")
        else:
            out.append(s)
    return out


def apply_column_bolds(matrix: list[list[float]]) -> list[list[str]]:
    """matrix[row][col] — bold maxima per column across rows."""
    if not matrix:
        return []
    nrows = len(matrix)
    ncols = len(matrix[0])
    result = [[""] * ncols for _ in range(nrows)]
    for j in range(ncols):
        col = [matrix[i][j] for i in range(nrows)]
        formatted = format_column_with_best(col)
        for i in range(nrows):
            result[i][j] = formatted[i]
    return result


def table_lookup_only(df: pd.DataFrame) -> str:
    lines = [
        r"\begin{table}[htbp]",
        r"\centering",
        r"\caption{Lookup-only throughput (Mops/s).}",
        r"\label{tab:lookup-only}",
        r"\begin{tabular}{@{}lccc@{}}",
        r"\toprule",
        r"\textbf{Index} & FB & Books & OSMC \\",
        r"\midrule",
    ]
    raw = [
        [cell_value(df, dk, idx) for dk, _ in DATASET_ORDER] for idx in INDEX_ORDER
    ]
    bolded = apply_column_bolds(raw)
    for idx, vals in zip(INDEX_ORDER, bolded):
        lines.append(
            f"{latex_escape(INDEX_DISPLAY[idx])} & {' & '.join(vals)} \\\\"
        )
    lines.extend([r"\bottomrule", r"\end{tabular}", r"\end{table}"])
    return "\n".join(lines)


def table_insert_lookup_combined(insert_df: pd.DataFrame, lookup_df: pd.DataFrame) -> str:
    lines = [
        r"\begin{table}[htbp]",
        r"\centering",
        r"\caption{Insert+Lookup throughput (Mops/s), 50\% insert / 50\% lookup.}",
        r"\label{tab:insert-lookup}",
        r"\begin{tabular}{@{}lcccccc@{}}",
        r"\toprule",
        r"\multirow{2}{*}{\textbf{Index}} & \multicolumn{3}{c}{Insert} & \multicolumn{3}{c}{Lookup After Insert} \\",
        r"\cmidrule(lr){2-4} \cmidrule(lr){5-7}",
        r"& FB & Books & OSMC & FB & Books & OSMC \\",
        r"\midrule",
    ]
    raw_ins = [
        [cell_value(insert_df, dk, idx) for dk, _ in DATASET_ORDER]
        for idx in INDEX_ORDER
    ]
    raw_lk = [
        [cell_value(lookup_df, dk, idx) for dk, _ in DATASET_ORDER]
        for idx in INDEX_ORDER
    ]
    bold_ins = apply_column_bolds(raw_ins)
    bold_lk = apply_column_bolds(raw_lk)
    for idx, ins, lk in zip(INDEX_ORDER, bold_ins, bold_lk):
        row = [latex_escape(INDEX_DISPLAY[idx])] + ins + lk
        lines.append(" & ".join(row) + r" \\")
    lines.extend([r"\bottomrule", r"\end{tabular}", r"\end{table}"])
    return "\n".join(lines)


def table_mixed_combined(mix1_df: pd.DataFrame, mix2_df: pd.DataFrame) -> str:
    col_a = latex_escape("10% Insert + 90% Lookup")
    col_b = latex_escape("90% Insert + 10% Lookup")
    lines = [
        r"\begin{table}[htbp]",
        r"\centering",
        r"\caption{Mixed workload throughput (Mops/s).}",
        r"\label{tab:mixed}",
        r"\begin{tabular}{@{}lcccccc@{}}",
        r"\toprule",
        rf"\multirow{{2}}{{*}}{{\textbf{{Index}}}} & \multicolumn{{3}}{{c}}{{{col_a}}} & \multicolumn{{3}}{{c}}{{{col_b}}} \\",
        r"\cmidrule(lr){2-4} \cmidrule(lr){5-7}",
        r"& FB & Books & OSMC & FB & Books & OSMC \\",
        r"\midrule",
    ]
    raw_m1 = [
        [cell_value(mix1_df, dk, idx) for dk, _ in DATASET_ORDER]
        for idx in INDEX_ORDER
    ]
    raw_m2 = [
        [cell_value(mix2_df, dk, idx) for dk, _ in DATASET_ORDER]
        for idx in INDEX_ORDER
    ]
    bold_m1 = apply_column_bolds(raw_m1)
    bold_m2 = apply_column_bolds(raw_m2)
    for idx, m1, m2 in zip(INDEX_ORDER, bold_m1, bold_m2):
        row = [latex_escape(INDEX_DISPLAY[idx])] + m1 + m2
        lines.append(" & ".join(row) + r" \\")
    lines.extend([r"\bottomrule", r"\end{tabular}", r"\end{table}"])
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Milestone-2 table
# ---------------------------------------------------------------------------

TPUT_COLS = [
    "mixed_throughput_mops1",
    "mixed_throughput_mops2",
    "mixed_throughput_mops3",
]
SIZE_COL = "index_size_bytes"

M2_CSV_COLS = [
    "index_name",
    "build_time_ns1", "build_time_ns2", "build_time_ns3",
    "index_size_bytes",
    "mixed_throughput_mops1", "mixed_throughput_mops2", "mixed_throughput_mops3",
    "search_method", "value", "value2",
]


def load_m2_csv(path: Path) -> pd.DataFrame:
    return pd.read_csv(path, names=M2_CSV_COLS, skiprows=1)


def best_row_m2(df: pd.DataFrame, index_name: str) -> pd.Series | None:
    rows = df[df["index_name"] == index_name].copy()
    if rows.empty:
        return None
    rows["_avg"] = rows[TPUT_COLS].mean(axis=1)
    return rows.loc[rows["_avg"].idxmax()]


def tput_stats(row: pd.Series) -> tuple[float, float]:
    """Return (mean, sample_std) for the three throughput columns."""
    vals = [float(row[c]) for c in TPUT_COLS]
    return statistics.mean(vals), statistics.stdev(vals)


def size_mb(row: pd.Series) -> float:
    return float(row[SIZE_COL]) / (1024 ** 2)


def hybrid_label(row: pd.Series) -> str:
    """Build a display label like 'Hybrid ($\\varepsilon{=}128$, $1\\permil$)'."""
    try:
        eps = int(float(row["value"]))
        flush = int(float(row["value2"]))
        return rf"Hybrid ($\varepsilon{{=}}{eps}$, ${flush}\permil$)"
    except (ValueError, TypeError):
        return "Hybrid"


def permil_cmd() -> str:
    return r"\permil"


def make_m2_table(results_dir: Path, dataset: str = "fb_100M_public_uint64") -> str:
    """Read the two mixed-workload CSVs and return a LaTeX table string."""
    wl_lookup = f"{dataset}_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv"
    wl_insert = f"{dataset}_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix_results_table.csv"

    df_lookup = load_m2_csv(results_dir / wl_lookup)
    df_insert = load_m2_csv(results_dir / wl_insert)

    rows: list[tuple[str, str, float, float, float]] = []

    for wl_label, wl_display, df in [
        ("lookup-heavy", r"90\% Lookup, 10\% Insert", df_lookup),
        ("insert-heavy", r"10\% Lookup, 90\% Insert", df_insert),
    ]:
        for raw_name in ["DynamicPGM", "LIPP", "HybridPGMLipp"]:
            row = best_row_m2(df, raw_name)
            if row is None:
                continue
            mean, std = tput_stats(row)
            sz = size_mb(row)
            if raw_name == "DynamicPGM":
                try:
                    eps = int(float(row["value"]))
                    label = rf"DPGM ($\varepsilon{{=}}{eps}$)"
                except (ValueError, TypeError):
                    label = "DPGM"
            elif raw_name == "LIPP":
                label = "LIPP"
            else:
                label = hybrid_label(row)
            rows.append((wl_display, label, mean, std, sz))

    # Find the best throughput per workload group for bolding
    lookup_means = [r[2] for r in rows if r[0] == r"90\% Lookup, 10\% Insert"]
    insert_means = [r[2] for r in rows if r[0] == r"10\% Lookup, 90\% Insert"]
    best_lookup = max(lookup_means) if lookup_means else float("inf")
    best_insert = max(insert_means) if insert_means else float("inf")

    def fmt_tput(mean: float, std: float, workload_display: str) -> str:
        best = best_lookup if workload_display == r"90\% Lookup, 10\% Insert" else best_insert
        s = rf"${mean:.3f} \pm {std:.3f}$"
        if math.isclose(mean, best, rel_tol=1e-9):
            s = rf"\textbf{{{s}}}"
        return s

    # Build table
    lines = [
        r"\begin{table}[h]",
        r"\centering",
        r"\caption{Benchmark results on the Facebook dataset (\texttt{fb\_100M}), 3 runs each.",
        r"  Throughput is the average mixed Mops/s $\pm$ sample standard deviation.",
        r"  Index size is measured at the end of the run.",
        r"  Best hyperparameters selected by highest average throughput per workload.",
        r"  Hybrid uses $\varepsilon{=}128$, flush $1\permil$ for the lookup-heavy workload",
        r"  and $\varepsilon{=}128$, flush $10\permil$ for the insert-heavy workload.}",
        r"\label{tab:results}",
        r"\smallskip",
        r"\begin{tabular}{llrr}",
        r"\toprule",
        r"\textbf{Index} & \textbf{Workload} & \textbf{Throughput (Mops/s)} & \textbf{Index Size (MB)} \\",
        r"\midrule",
    ]

    prev_wl = None
    for wl_display, label, mean, std, sz in rows:
        if prev_wl is not None and prev_wl != wl_display:
            lines.append(r"\midrule")
        tput_cell = fmt_tput(mean, std, wl_display)
        lines.append(f"{label} & {wl_display} & {tput_cell} & {sz:.0f} \\\\")
        prev_wl = wl_display

    lines.extend([r"\bottomrule", r"\end{tabular}", r"\end{table}"])
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    p = argparse.ArgumentParser(description="Emit LaTeX tables from benchmark CSVs.")
    p.add_argument(
        "--m2",
        action="store_true",
        help="Generate the Milestone-2 table (table_m2.tex) from results/ CSVs.",
    )
    p.add_argument(
        "--results-dir",
        type=Path,
        default=Path("results"),
        help="Directory containing the mixed-workload result CSVs (--m2 only).",
    )
    p.add_argument(
        "--dir",
        type=Path,
        default=Path("analysis_results"),
        help="Directory containing Task-1 throughput CSV files (default mode).",
    )
    p.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Write LaTeX to this file (default: print to stdout).",
    )
    args = p.parse_args()

    if args.m2:
        text = make_m2_table(args.results_dir)
    else:
        base = args.dir
        lookup = load_throughput_csv(base / "lookuponly_throughput.csv")
        insert_t = load_throughput_csv(base / "insertlookup_insert_throughput.csv")
        lookup_t = load_throughput_csv(base / "insertlookup_lookup_throughput.csv")
        mix1 = load_throughput_csv(base / "insertlookup_mix1_throughput.csv")
        mix2 = load_throughput_csv(base / "insertlookup_mix2_throughput.csv")

        blocks = [
            "% Requires: \\usepackage{booktabs} and \\usepackage{multirow}",
            "",
            table_lookup_only(lookup),
            "",
            table_insert_lookup_combined(insert_t, lookup_t),
            "",
            table_mixed_combined(mix1, mix2),
            "",
        ]
        text = "\n".join(blocks)

    if args.output:
        args.output.write_text(text, encoding="utf-8")
        print(f"Written to {args.output}")
    else:
        print(text)


if __name__ == "__main__":
    main()
