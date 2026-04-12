#!/usr/bin/env python3
"""Build LaTeX tables from analysis_results/*.csv (output of scripts/analysis.py)."""

from __future__ import annotations

import argparse
import math
from pathlib import Path

import pandas as pd

# CSV row labels -> report column order (task_1_plan.md)
DATASET_ORDER = [("fb", "FB"), ("books", "Books"), ("osmc", "OSMC")]

# CSV column names -> display row labels
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


def fmt_mops(v: float) -> str:
    if pd.isna(v):
        return "---"
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return s if s else "0"


def latex_escape(s: str) -> str:
    return s.replace("%", r"\%")


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


def main() -> None:
    p = argparse.ArgumentParser(description="Emit LaTeX tables from analysis CSVs.")
    p.add_argument(
        "--dir",
        type=Path,
        default=Path("analysis_results"),
        help="Directory containing the five throughput CSV files.",
    )
    p.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Write combined LaTeX to this file (default: print to stdout).",
    )
    args = p.parse_args()
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
    else:
        print(text)


if __name__ == "__main__":
    main()
