#!/usr/bin/env python3
"""
cumulative_prove_time.py — Cumulative proving-time comparison.

Computes how total prover cost accumulates over B steps for UVC, the three
Groth16 baseline modes, and the two Nova series.  Outputs ASCII summary +
optional per-step CSV for plotting.

Cumulative definitions (see bench_data.py):
  UVC:               setup + step * median(prove)
  G16 on-demand:     Σ trapezoid of (setup_j + prove_j)   [prove-only variant too]
  G16 fixed-CRS:     setup(C_B) + Σ trapezoid of prove_j
  G16 single-step:   setup + Σ trapezoid of prove_j
  Nova fold-only:    setup + total_fold(step)
  Nova compressed:   setup + total_fold(step) + compress

Usage:
    python3 scripts/cumulative_prove_time.py <csv_dir>
    python3 scripts/cumulative_prove_time.py <csv_dir> --out cumulative.csv
    python3 scripts/cumulative_prove_time.py <csv_dir> --latex
"""

import argparse
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bench_data
from bench_data import (
    CANONICAL_UVC_HEADER, CIRCUIT_DISPLAY, CIRCUIT_ORDER,
    normalize_circuit, read_canonical_uvc, read_csv,
)


# ── Series definitions ───────────────────────────────────────────

# (csv column, ascii/latex header, key in the per-step dict)
SERIES = [
    ("uvc_cum_s", "UVC", "uvc_cum_ms"),
    ("g16_ondemand_cum_s", "G16 on-dem", "g16_ondemand_cum_ms"),
    ("g16_ondemand_prove_only_cum_s", "G16 od-prv", "g16_ondemand_prove_only_cum_ms"),
    ("g16_fixedcrs_cum_s", "G16 fixCRS", "g16_fixedcrs_cum_ms"),
    ("g16_singlestep_cum_s", "G16 single", "g16_singlestep_cum_ms"),
    ("nova_foldonly_cum_s", "Nova fold", "nova_foldonly_cum_ms"),
    ("nova_compressed_cum_s", "Nova compr", "nova_compressed_cum_ms"),
]

RATIOS = [
    ("ratio_uvc_over_g16_ondemand_prove_only", "U/G16odp", "g16_ondemand_prove_only_cum_ms"),
    ("ratio_uvc_over_g16_fixedcrs", "U/G16fix", "g16_fixedcrs_cum_ms"),
    ("ratio_uvc_over_g16_singlestep", "U/G16ss", "g16_singlestep_cum_ms"),
    ("ratio_uvc_over_nova_foldonly", "U/NovaF", "nova_foldonly_cum_ms"),
    ("ratio_uvc_over_nova_compressed", "U/NovaC", "nova_compressed_cum_ms"),
]

CSV_FIELDNAMES = (["circuit", "n", "B", "step"]
                  + [c for c, _h, _k in SERIES]
                  + [c for c, _h, _k in RATIOS])


# ── Data loading & cumulative computation ────────────────────────

def load_data(csv_dir):
    """Load the run via the shared loader."""
    return bench_data.load_run(csv_dir)


def get_configs(data):
    """Unique (circuit, n, B) triples, paper-ordered."""
    return bench_data.uvc_configs(data)


def compute_cumulative(data):
    """Return list of per-step cumulative dicts for every config."""
    rows = []
    for circuit, n, B in get_configs(data):
        for step in bench_data.measured_steps(B):
            entry = {
                "circuit": circuit,
                "n": n,
                "B": B,
                "step": step,
                "uvc_cum_ms": bench_data.cumulative_uvc(data, circuit, n, B, step),
                "g16_ondemand_cum_ms": bench_data.cumulative_groth16(
                    data, "ondemand", circuit, n, B, step),
                "g16_ondemand_prove_only_cum_ms": bench_data.cumulative_groth16(
                    data, "ondemand", circuit, n, B, step, proving_only=True),
                "g16_fixedcrs_cum_ms": bench_data.cumulative_groth16(
                    data, "fixedcrs", circuit, n, B, step),
                "g16_singlestep_cum_ms": bench_data.cumulative_groth16(
                    data, "singlestep", circuit, n, B, step),
                "nova_foldonly_cum_ms": bench_data.cumulative_nova(
                    data, "foldonly", circuit, n, step),
                "nova_compressed_cum_ms": bench_data.cumulative_nova(
                    data, "compressed", circuit, n, step),
            }
            rows.append(entry)
    return rows


# ── Formatting helpers ───────────────────────────────────────────

def fmt_s(ms):
    """Format milliseconds as seconds for display."""
    if ms is None:
        return "---"
    s = ms / 1000.0
    if s < 0.01:
        return f"{s*1000:.1f}ms"
    if s < 0.1:
        return f"{s:.3f}"
    if s < 10:
        return f"{s:.2f}"
    if s < 1000:
        return f"{s:.1f}"
    return f"{s:.0f}"


def ratio_str(a, b):
    if a is None or b is None or b == 0:
        return "---"
    return f"{a / b:.2f}x"


# ── ASCII output ─────────────────────────────────────────────────

ASCII_HEADERS = (["Circuit", "n", "B"]
                 + [f"{h} (s)" for _c, h, _k in SERIES]
                 + [h for _c, h, _k in RATIOS])


def _ascii_values(r):
    vals = [fmt_s(r[k]) for _c, _h, k in SERIES]
    vals += [ratio_str(r["uvc_cum_ms"], r[k]) for _c, _h, k in RATIOS]
    return vals


def print_summary(rows):
    """Print per-config summary at max step (=B)."""
    summary = {}
    for r in rows:
        if r["step"] == r["B"]:
            summary[(r["circuit"], r["n"], r["B"])] = r

    col_w = [max(len(h), 11) for h in ASCII_HEADERS]
    col_w[0] = max(col_w[0], 17)
    col_w[1] = 6
    col_w[2] = 5

    total = sum(col_w) + 2 * (len(col_w) - 1)
    print()
    print("=" * total)
    print("  Cumulative Proving Time at step B (seconds)")
    print("=" * total)
    print("  ".join(h.rjust(w) for h, w in zip(ASCII_HEADERS, col_w)))
    print("  ".join("-" * w for w in col_w))

    for key in sorted(summary, key=lambda k: (CIRCUIT_ORDER.get(k[0], 99), k[1], k[2])):
        r = summary[key]
        vals = [CIRCUIT_DISPLAY.get(r["circuit"], r["circuit"]),
                str(r["n"]), str(r["B"])] + _ascii_values(r)
        print("  ".join(v.rjust(w) for v, w in zip(vals, col_w)))
    print()


def print_per_step(rows):
    """Print per-step cumulative breakdown for each config."""
    configs = []
    seen = set()
    for r in rows:
        key = (r["circuit"], r["n"], r["B"])
        if key not in seen:
            seen.add(key)
            configs.append(key)

    headers = ["Step"] + [f"{h} (s)" for _c, h, _k in SERIES] + [h for _c, h, _k in RATIOS]
    col_w = [max(len(h), 11) for h in headers]
    col_w[0] = 6

    for circuit, n, B in configs:
        config_rows = [r for r in rows
                       if r["circuit"] == circuit and r["n"] == n and r["B"] == B]
        name = CIRCUIT_DISPLAY.get(circuit, circuit)

        print()
        print(f"── {name}  n={n}  B={B} " + "─" * 50)
        print("  ".join(h.rjust(w) for h, w in zip(headers, col_w)))
        print("  ".join("-" * w for w in col_w))

        for r in config_rows:
            vals = [str(r["step"])] + _ascii_values(r)
            print("  ".join(v.rjust(w) for v, w in zip(vals, col_w)))
    print()


# ── CSV output ───────────────────────────────────────────────────

def _cell_s(ms):
    return f"{ms / 1000:.4f}" if ms is not None else ""


def _cell_ratio(a, b):
    if a is None or b is None or b == 0:
        return ""
    return f"{a / b:.4f}"


def write_csv_file(rows, path):
    """Write per-step cumulative data to CSV."""
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDNAMES)
        writer.writeheader()
        for r in rows:
            out = {"circuit": r["circuit"], "n": r["n"], "B": r["B"], "step": r["step"]}
            for col, _h, key in SERIES:
                out[col] = _cell_s(r[key])
            for col, _h, key in RATIOS:
                out[col] = _cell_ratio(r["uvc_cum_ms"], r[key])
            writer.writerow(out)
    print(f"  Saved: {path}")


# ── LaTeX output ─────────────────────────────────────────────────

LATEX_HEADERS = ([r"\textbf{Ours}", r"\textbf{G16 on-dem.}",
                  r"\textbf{G16 on-dem.\ prove}", r"\textbf{G16 fix.\ CRS}",
                  r"\textbf{G16 single}", r"\textbf{Nova fold}",
                  r"\textbf{Nova compr.}"]
                 + [r"\textbf{Ours/G16 od-prv}", r"\textbf{Ours/G16 fix}",
                    r"\textbf{Ours/G16 ss}", r"\textbf{Ours/Nova fold}",
                    r"\textbf{Ours/Nova compr}"])


def latex_circuit(circuit):
    names = {
        "mimc": r"\shortstack{MiMC\\hash chain}",
        "hadamard": r"\shortstack{Hadamard\\product}",
        "scalable": r"\shortstack{Repeated\\squaring}",
        "sensor_fusion": r"\shortstack{Sensor\\fusion}",
        "pagerank": r"PageRank",
    }
    return names.get(circuit, circuit)


def print_latex(rows):
    """Print LaTeX table of cumulative totals at max step."""
    summary = {}
    for r in rows:
        if r["step"] == r["B"]:
            summary[(r["circuit"], r["n"], r["B"])] = r

    configs = sorted(summary, key=lambda k: (CIRCUIT_ORDER.get(k[0], 99), k[1], k[2]))

    groups = []
    prev = None
    for key in configs:
        c, n, B = key
        if (c, n) != prev:
            groups.append([])
            prev = (c, n)
        groups[-1].append(key)

    ncols = 3 + len(SERIES) + len(RATIOS)
    lines = [
        r"% Auto-generated by cumulative_prove_time.py",
        r"\begin{table*}[t]",
        r"\centering",
        r"\caption{Cumulative proving time (seconds) over $B$ steps.}",
        r"\label{tab:cumulative}",
        r"\renewcommand{\arraystretch}{1.2}",
        r"{\scriptsize\setlength{\tabcolsep}{3pt}",
        r"\begin{tabular}{@{}cc" + "r" * (ncols - 2) + r"@{}}",
        r"\toprule",
        " & ".join([r"\textbf{Circuit}", "$K$", "$B$"] + LATEX_HEADERS) + r" \\",
        r"\midrule",
    ]

    for gi, group in enumerate(groups):
        num = len(group)
        for bi, key in enumerate(group):
            r = summary[key]
            c, n, B = key
            if bi == 0:
                circ = rf"\multirow{{{num}}}{{*}}{{{latex_circuit(c)}}}"
                ncol = rf"\multirow{{{num}}}{{*}}{{{n}}}"
            else:
                circ, ncol = "", ""

            parts = [circ, ncol, str(B)]
            for _col, _h, key_ms in SERIES:
                v = r[key_ms]
                parts.append(f"{v / 1000:.1f}" if v is not None else "---")
            for _col, _h, key_ms in RATIOS:
                a, b = r["uvc_cum_ms"], r[key_ms]
                parts.append(f"{a / b:.2f}" if (a is not None and b) else "---")
            lines.append(" & ".join(parts) + r" \\")

        if gi < len(groups) - 1:
            lines.append(r"\midrule")

    lines += [
        r"\bottomrule",
        r"\end{tabular}",
        r"}% end scriptsize",
        r"\end{table*}",
    ]
    print("\n".join(lines))


# ── Main ─────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Compute cumulative proving time across steps.")
    parser.add_argument("csv_dir",
        help="Directory containing {uvc,groth16,groth16_modes,nova}_results.csv")
    parser.add_argument("--out",
        help="Write per-step cumulative CSV to this path")
    parser.add_argument("--latex", action="store_true",
        help="Print LaTeX table to stdout")
    parser.add_argument("--per-step", action="store_true",
        help="Show per-step cumulative breakdown")
    args = parser.parse_args()

    data = load_data(args.csv_dir)
    if not data["uvc"]:
        print("No UVC data found.", file=sys.stderr)
        sys.exit(1)

    rows = compute_cumulative(data)

    print()
    print(f"  Data: {args.csv_dir}")
    print(f"  Configs: {len(get_configs(data))}")

    if args.latex:
        print_latex(rows)
    else:
        print_summary(rows)
        if args.per_step:
            print_per_step(rows)

    if args.out:
        write_csv_file(rows, args.out)


if __name__ == "__main__":
    main()
