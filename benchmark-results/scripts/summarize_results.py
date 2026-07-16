#!/usr/bin/env python3
"""
summarize_results.py — Parse UVC benchmark CSVs and produce comparison tables.

Usage:
    python3 summarize_results.py ../csv
    python3 summarize_results.py ../csv --latex
    python3 summarize_results.py ../csv --save-dir ../
"""

import argparse
import csv
import os
import sys
from collections import defaultdict
from pathlib import Path


# ── Paper display names ───────────────────────────────────────────

CIRCUIT_DISPLAY = {
    "mimc": "MiMC hash chain",
    "hadamard": "Hadamard product",
    "matmul": "Hadamard product",      # legacy name
    "scalable": "Repeated squaring",
}

CIRCUIT_ORDER = ["mimc", "hadamard", "matmul", "scalable"]

# Paper Table 2 expects these steps
PAPER_STEPS = [1, 4, 16, 64]
PAPER_B_VALUES = [16, 64]


# ── CSV readers ───────────────────────────────────────────────────

def read_csv(path):
    """Read a CSV file, return list of dicts."""
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def normalize_circuit(name):
    """Normalize legacy circuit names."""
    if name == "matmul":
        return "hadamard"
    return name


def circuit_sort_key(circuit, n):
    """Sort key matching paper Table 2 order."""
    order = {"mimc": 0, "hadamard": 1, "matmul": 1, "scalable": 2}
    return (order.get(circuit, 99), int(n))


# ── Formatting ────────────────────────────────────────────────────

def fmt_ms(val):
    """Format milliseconds for display."""
    if val is None or val == "":
        return "---"
    v = float(val)
    if v < 1:
        return f"{v:.3f}"
    elif v < 100:
        return f"{v:.1f}"
    elif v < 10000:
        return f"{v:.0f}"
    else:
        return f"{v/1000:.1f}k"


def fmt_ms_latex(val):
    """Format milliseconds for LaTeX."""
    if val is None or val == "":
        return "---"
    v = float(val)
    if v < 1:
        return f"{v:.3f}"
    elif v < 100:
        return f"{v:.1f}"
    elif v < 10000:
        return f"{v:.0f}"
    else:
        return f"{v/1000:.1f}k"


def fmt_sec(val):
    """Format milliseconds as seconds."""
    if val is None or val == "":
        return "---"
    v = float(val) / 1000.0
    if v < 0.1:
        return f"<0.1"
    elif v < 10:
        return f"{v:.1f}"
    else:
        return f"{v:.0f}"


# ── Data loading ──────────────────────────────────────────────────

def load_all(csv_dir):
    """Load and index all benchmark CSVs."""
    uvc_rows = read_csv(os.path.join(csv_dir, "uvc_results.csv"))
    g16_rows = read_csv(os.path.join(csv_dir, "groth16_results.csv"))
    nova_rows = read_csv(os.path.join(csv_dir, "nova_results.csv"))

    # Index UVC by (circuit, n, B, step)
    uvc = {}
    for r in uvc_rows:
        c = normalize_circuit(r["circuit"])
        key = (c, int(r["n"]), int(r["B"]), int(r["step"]))
        uvc[key] = r

    # Groth16 by (circuit, n, step) — deduplicate
    g16 = {}
    for r in g16_rows:
        c = normalize_circuit(r["circuit"])
        key = (c, int(r["n"]), int(r["step"]))
        if key not in g16:
            g16[key] = r

    # Nova by (circuit, n, step) — keep last
    nova = {}
    for r in nova_rows:
        c = normalize_circuit(r.get("circuit", ""))
        key = (c, int(r["n"]), int(r["step"]))
        nova[key] = r

    return uvc, g16, nova


def get_circuit_configs(uvc):
    """Get unique (circuit, n) pairs sorted in paper order."""
    configs = set()
    for (c, n, B, step) in uvc.keys():
        configs.add((c, n))
    return sorted(configs, key=lambda x: circuit_sort_key(x[0], x[1]))


# ── Terminal summary ──────────────────────────────────────────────

def print_table(headers, rows, col_widths=None):
    """Print a formatted ASCII table."""
    if col_widths is None:
        col_widths = []
        for i, h in enumerate(headers):
            w = len(h)
            for row in rows:
                if i < len(row):
                    w = max(w, len(str(row[i])))
            col_widths.append(w)

    # Header
    hdr = "  ".join(h.rjust(w) for h, w in zip(headers, col_widths))
    sep = "  ".join("-" * w for w in col_widths)
    print(hdr)
    print(sep)

    # Rows
    for row in rows:
        line = "  ".join(str(v).rjust(w) for v, w in zip(row, col_widths))
        print(line)


def print_prover_table(uvc, g16, nova):
    """Print Table 2-style per-step proving time comparison."""
    configs = get_circuit_configs(uvc)

    print()
    print("=" * 90)
    print("  Per-step Proving Time (ms)")
    print("=" * 90)

    headers = ["Circuit", "n", "Step", "Ours(B=16)", "Ours(B=64)", "Groth16", "Nova"]
    rows = []

    for circuit, n in configs:
        for j_idx, j in enumerate(PAPER_STEPS):
            circ_col = CIRCUIT_DISPLAY.get(circuit, circuit) if j_idx == 0 else ""
            n_col = str(n) if j_idx == 0 else ""

            uvc16 = uvc.get((circuit, n, 16, j), {})
            uvc64 = uvc.get((circuit, n, 64, j), {})
            g = g16.get((circuit, n, j), {})
            nv = nova.get((circuit, n, j), {})

            rows.append([
                circ_col, n_col, str(j),
                fmt_ms(uvc16.get("prove_ms")),
                fmt_ms(uvc64.get("prove_ms")),
                fmt_ms(g.get("prove_ms")),
                fmt_ms(nv.get("fold_ms")),
            ])
        rows.append([""] * 7)  # blank separator

    print_table(headers, rows)


def print_verify_table(uvc, g16, nova):
    """Print verification time comparison."""
    configs = get_circuit_configs(uvc)

    print()
    print("=" * 90)
    print("  Per-step Verification Time (ms)")
    print("=" * 90)

    headers = ["Circuit", "n", "Step", "Ours(B=16)", "Ours(B=64)", "Groth16", "Nova"]
    rows = []

    for circuit, n in configs:
        for j_idx, j in enumerate(PAPER_STEPS):
            circ_col = CIRCUIT_DISPLAY.get(circuit, circuit) if j_idx == 0 else ""
            n_col = str(n) if j_idx == 0 else ""

            uvc16 = uvc.get((circuit, n, 16, j), {})
            uvc64 = uvc.get((circuit, n, 64, j), {})
            g = g16.get((circuit, n, j), {})
            nv = nova.get((circuit, n, j), {})

            rows.append([
                circ_col, n_col, str(j),
                fmt_ms(uvc16.get("verify_ms")),
                fmt_ms(uvc64.get("verify_ms")),
                fmt_ms(g.get("verify_ms")),
                fmt_ms(nv.get("verify_ms")),
            ])
        rows.append([""] * 7)

    print_table(headers, rows)


def print_setup_table(uvc, g16, nova):
    """Print setup cost comparison."""
    configs = get_circuit_configs(uvc)

    print()
    print("=" * 80)
    print("  Setup Cost Comparison (seconds)")
    print("=" * 80)

    headers = ["Circuit", "n", "Ours(B=16)", "Ours(B=64)", "Groth16", "Nova"]
    rows = []

    for circuit, n in configs:
        uvc16 = uvc.get((circuit, n, 16, 1), {})
        uvc64 = uvc.get((circuit, n, 64, 1), {})
        g = g16.get((circuit, n, 1), {})
        nv = nova.get((circuit, n, 1), {})

        rows.append([
            CIRCUIT_DISPLAY.get(circuit, circuit), str(n),
            fmt_sec(uvc16.get("setup_ms")),
            fmt_sec(uvc64.get("setup_ms")),
            fmt_sec(g.get("setup_ms")),
            fmt_sec(nv.get("setup_ms")),
        ])

    print_table(headers, rows)


def print_proof_size_table(uvc, g16, nova):
    """Print proof size comparison."""
    configs = get_circuit_configs(uvc)

    print()
    print("=" * 60)
    print("  Proof Size (bytes)")
    print("=" * 60)

    headers = ["Circuit", "n", "Ours", "Groth16", "Nova"]
    rows = []

    for circuit, n in configs:
        # Get any step's data
        uvc_r = uvc.get((circuit, n, 16, 1), uvc.get((circuit, n, 64, 1), {}))
        g = g16.get((circuit, n, 1), {})
        nv = nova.get((circuit, n, 1), {})

        rows.append([
            CIRCUIT_DISPLAY.get(circuit, circuit), str(n),
            uvc_r.get("proof_bytes", "---"),
            g.get("proof_bytes", "---"),
            nv.get("proof_bytes", "---"),
        ])

    print_table(headers, rows)


def print_crossover(uvc, g16):
    """Print crossover analysis: step j* where UVC cumulative < Groth16."""
    configs = get_circuit_configs(uvc)

    print()
    print("=" * 70)
    print("  Crossover Analysis (j* where UVC cumulative < Groth16 re-prove)")
    print("=" * 70)

    headers = ["Circuit", "n", "B=16 j*", "B=64 j*"]
    rows = []

    for circuit, n in configs:
        crossovers = {}
        for B in PAPER_B_VALUES:
            uvc_setup_r = uvc.get((circuit, n, B, 1))
            if not uvc_setup_r:
                crossovers[B] = "---"
                continue

            uvc_setup = float(uvc_setup_r.get("setup_ms", 0))
            uvc_cumul = uvc_setup
            found = None

            for j in PAPER_STEPS:
                uvc_r = uvc.get((circuit, n, B, j))
                g16_r = g16.get((circuit, n, j))
                if not uvc_r or not g16_r:
                    continue

                uvc_cumul += float(uvc_r["prove_ms"])
                g16_total = float(g16_r["setup_ms"]) + float(g16_r["prove_ms"])

                if uvc_cumul < g16_total and found is None:
                    found = j

            crossovers[B] = str(found) if found else ">64"

        rows.append([
            CIRCUIT_DISPLAY.get(circuit, circuit), str(n),
            crossovers.get(16, "---"),
            crossovers.get(64, "---"),
        ])

    print_table(headers, rows)


def print_summary(csv_dir):
    """Print full terminal summary."""
    uvc, g16, nova = load_all(csv_dir)

    if not uvc:
        print("No UVC data found.")
        return

    print()
    print("############################################################")
    print("  UVC Benchmark Summary")
    print(f"  Data: {csv_dir}")
    print(f"  UVC rows: {len(uvc)}, Groth16 rows: {len(g16)}, Nova rows: {len(nova)}")
    print("############################################################")

    print_prover_table(uvc, g16, nova)
    print_verify_table(uvc, g16, nova)
    print_setup_table(uvc, g16, nova)
    print_proof_size_table(uvc, g16, nova)
    print_crossover(uvc, g16)

    print()


# ── LaTeX output ──────────────────────────────────────────────────

def latex_circuit_name(circuit):
    """LaTeX-formatted circuit name for multirow."""
    names = {
        "mimc": r"\shortstack{MiMC\\hash chain}",
        "hadamard": r"\shortstack{Hadamard\\product}",
        "scalable": r"\shortstack{Repeated\\squaring}",
    }
    return names.get(circuit, circuit)


def latex_n(n):
    """Format n for LaTeX (use 2^k for powers of 2 >= 1024)."""
    import math
    if n >= 1024 and math.log2(n) == int(math.log2(n)):
        return f"$2^{{{int(math.log2(n))}}}$"
    return str(n)


def gen_table2_latex(uvc, g16, nova, state_bound=False):
    """Generate Table 2 (per-step proving + verification + proof size) matching paper."""
    configs = get_circuit_configs(uvc)

    lines = []
    lines.append(r"% Auto-generated by summarize_results.py")
    lines.append(r"\begin{table*}[t]")
    lines.append(r"\centering")
    lines.append(r"\caption{Per-step proving and verification time (ms) across circuits. "
                 r"$n$ is the number of constraints per step and $B$ is the maximum steps for our scheme.}")
    lines.append(r"\label{tab:bench-perstep}")
    lines.append(r"\renewcommand{\arraystretch}{1.2}")
    lines.append(r"{\footnotesize\setlength{\tabcolsep}{2.5pt}")
    lines.append(r"\begin{tabular}{@{}ccr cccc cccc ccc @{}}")
    lines.append(r"\toprule")
    lines.append(r"& & & \multicolumn{4}{c}{\textbf{Proving time (ms)}} & "
                 r"\multicolumn{4}{c}{\textbf{Verification time (ms)}} & "
                 r"\multicolumn{3}{c}{\textbf{Proof size (bytes)}} \\")
    lines.append(r"\cmidrule(lr){4-7} \cmidrule(lr){8-11} \cmidrule(lr){12-14}")
    lines.append(r"\textbf{Circuit} & $n$ & \textbf{Step} & "
                 r"\textbf{Ours} ($B\!=\!16$) & \textbf{Ours} ($B\!=\!64$) & "
                 r"\textbf{Groth16} & \textbf{Nova} & "
                 r"\textbf{Ours} ($B\!=\!16$) & \textbf{Ours} ($B\!=\!64$) & "
                 r"\textbf{Groth16} & \textbf{Nova} & "
                 r"\textbf{Ours} & \textbf{Groth16} & \textbf{Nova} \\")
    lines.append(r"\midrule")

    for ci, (circuit, n) in enumerate(configs):
        num_steps = len(PAPER_STEPS)

        # Get proof sizes (constant across steps)
        uvc_proof = "160" if state_bound else "128"
        g16_proof = "128"
        nova_proof = "---"
        nv_any = nova.get((circuit, n, 1))
        if nv_any:
            nova_proof = nv_any.get("proof_bytes", "---")

        for j_idx, j in enumerate(PAPER_STEPS):
            uvc16 = uvc.get((circuit, n, 16, j), {})
            uvc64 = uvc.get((circuit, n, 64, j), {})
            g = g16.get((circuit, n, j), {})
            nv = nova.get((circuit, n, j), {})

            # Circuit name and n: multirow on first step
            if j_idx == 0:
                circ = rf"\multirow{{{num_steps}}}{{*}}{{{latex_circuit_name(circuit)}}}"
                n_col = rf"\multirow{{{num_steps}}}{{*}}{{{latex_n(n)}}}"
            else:
                circ = ""
                n_col = ""

            # Proving times
            p16 = fmt_ms_latex(uvc16.get("prove_ms"))
            p64 = fmt_ms_latex(uvc64.get("prove_ms"))
            pg = fmt_ms_latex(g.get("prove_ms"))
            pn = fmt_ms_latex(nv.get("fold_ms"))

            # Verification times
            v16 = fmt_ms_latex(uvc16.get("verify_ms"))
            v64 = fmt_ms_latex(uvc64.get("verify_ms"))
            vg = fmt_ms_latex(g.get("verify_ms"))
            vn = fmt_ms_latex(nv.get("verify_ms"))

            # Proof sizes: multirow on first step
            if j_idx == 0:
                ps_ours = rf"\multirow{{{num_steps}}}{{*}}{{{uvc_proof}}}"
                ps_g16 = rf"\multirow{{{num_steps}}}{{*}}{{{g16_proof}}}"
                ps_nova = rf"\multirow{{{num_steps}}}{{*}}{{{nova_proof}}}"
            else:
                ps_ours = ""
                ps_g16 = ""
                ps_nova = ""

            lines.append(
                f"{circ} & {n_col} & {j} & "
                f"{p16} & {p64} & {pg} & {pn} & "
                f"{v16} & {v64} & {vg} & {vn} & "
                f"{ps_ours} & {ps_g16} & {ps_nova} \\\\"
            )

        if ci < len(configs) - 1:
            lines.append(r"\midrule")

    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"}% end footnotesize")
    lines.append(r"\end{table*}")

    return "\n".join(lines)


def gen_setup_latex(uvc, g16, nova):
    """Generate setup cost comparison table."""
    configs = get_circuit_configs(uvc)

    lines = []
    lines.append(r"% Auto-generated setup comparison")
    lines.append(r"\begin{table}[t]")
    lines.append(r"\centering")
    lines.append(r"\caption{Setup cost comparison (seconds).}")
    lines.append(r"\label{tab:setup}")
    lines.append(r"\renewcommand{\arraystretch}{1.2}")
    lines.append(r"{\footnotesize")
    lines.append(r"\begin{tabular}{@{}llcccc@{}}")
    lines.append(r"\toprule")
    lines.append(r"\textbf{Circuit} & $n$ & \textbf{Ours} ($B\!=\!16$) & "
                 r"\textbf{Ours} ($B\!=\!64$) & \textbf{Groth16} & \textbf{Nova} \\")
    lines.append(r"\midrule")

    for circuit, n in configs:
        uvc16 = uvc.get((circuit, n, 16, 1), {})
        uvc64 = uvc.get((circuit, n, 64, 1), {})
        g = g16.get((circuit, n, 1), {})
        nv = nova.get((circuit, n, 1), {})

        cname = CIRCUIT_DISPLAY.get(circuit, circuit)
        lines.append(
            f"{cname} & {latex_n(n)} & "
            f"{fmt_sec(uvc16.get('setup_ms'))} & "
            f"{fmt_sec(uvc64.get('setup_ms'))} & "
            f"{fmt_sec(g.get('setup_ms'))} & "
            f"{fmt_sec(nv.get('setup_ms'))} \\\\"
        )

    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"}% end footnotesize")
    lines.append(r"\end{table}")

    return "\n".join(lines)


def gen_figure2_latex(uvc, g16, nova):
    """Generate Figure 2 pgfplots coordinates from data."""
    configs = get_circuit_configs(uvc)

    # Select representative circuits for the figure (one per type, mid-range n)
    figure_circuits = [
        ("mimc", 1080, "MiMC hash chain ($n=1080$)"),
        ("hadamard", 128, "Hadamard product ($n=128$)"),
        ("scalable", 8192, r"Repeated squaring ($n=2^{13}$)"),
    ]

    lines = []
    lines.append("% Auto-generated pgfplots coordinates for Figure 2")
    lines.append("")

    for circuit, n, label in figure_circuits:
        lines.append(f"% {label} — Proving time")

        # Ours B=16
        coords = []
        for j in PAPER_STEPS:
            r = uvc.get((circuit, n, 16, j))
            if r:
                coords.append(f"({j},{fmt_ms_latex(r['prove_ms'])})")
        lines.append(f"% Ours (B=16): {' '.join(coords)}")

        # Ours B=64
        coords = []
        for j in PAPER_STEPS:
            r = uvc.get((circuit, n, 64, j))
            if r:
                coords.append(f"({j},{fmt_ms_latex(r['prove_ms'])})")
        lines.append(f"% Ours (B=64): {' '.join(coords)}")

        # Groth16
        coords = []
        for j in PAPER_STEPS:
            r = g16.get((circuit, n, j))
            if r:
                coords.append(f"({j},{fmt_ms_latex(r['prove_ms'])})")
        lines.append(f"% Groth16:     {' '.join(coords)}")

        # Nova
        coords = []
        for j in PAPER_STEPS:
            r = nova.get((circuit, n, j))
            if r:
                coords.append(f"({j},{fmt_ms_latex(r['fold_ms'])})")
        lines.append(f"% Nova:        {' '.join(coords)}")

        lines.append("")

        lines.append(f"% {label} — Verification time")
        # Ours B=64 (verification same across B)
        coords = []
        for j in PAPER_STEPS:
            r = uvc.get((circuit, n, 64, j))
            if r:
                coords.append(f"({j},{fmt_ms_latex(r['verify_ms'])})")
        lines.append(f"% Ours (B=64): {' '.join(coords)}")

        # Groth16
        coords = []
        for j in PAPER_STEPS:
            r = g16.get((circuit, n, j))
            if r:
                coords.append(f"({j},{fmt_ms_latex(r['verify_ms'])})")
        lines.append(f"% Groth16:     {' '.join(coords)}")

        # Nova
        coords = []
        for j in PAPER_STEPS:
            r = nova.get((circuit, n, j))
            if r:
                coords.append(f"({j},{fmt_ms_latex(r['verify_ms'])})")
        lines.append(f"% Nova:        {' '.join(coords)}")

        lines.append("")

    return "\n".join(lines)


def print_latex(csv_dir):
    """Generate all LaTeX output."""
    uvc, g16, nova = load_all(csv_dir)

    if not uvc:
        print("% No UVC data found.", file=sys.stderr)
        return

    print(gen_table2_latex(
        uvc, g16, nova,
        state_bound=os.path.exists(os.path.join(csv_dir, "uvc_bound_results.csv"))))
    print()
    print()
    print(gen_setup_latex(uvc, g16, nova))
    print()
    print()
    print(gen_figure2_latex(uvc, g16, nova))


# ── Main ──────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Summarize UVC benchmark results into tables.")
    parser.add_argument("csv_dir",
        help="Directory containing {uvc,groth16,nova}_results.csv")
    parser.add_argument("--latex", action="store_true",
        help="Output LaTeX tables instead of terminal tables")
    parser.add_argument("--save-dir",
        help="Save individual LaTeX files to this directory")
    args = parser.parse_args()

    if args.latex:
        print_latex(args.csv_dir)
    elif args.save_dir:
        # Print terminal summary AND save LaTeX files
        print_summary(args.csv_dir)

        uvc, g16, nova = load_all(args.csv_dir)
        if uvc:
            os.makedirs(args.save_dir, exist_ok=True)

            with open(os.path.join(args.save_dir, "table_perstep.tex"), "w") as f:
                f.write(gen_table2_latex(
                    uvc, g16, nova,
                    state_bound=os.path.exists(
                        os.path.join(args.csv_dir, "uvc_bound_results.csv"))))
            print(f"  Saved: {args.save_dir}/table_perstep.tex")

            with open(os.path.join(args.save_dir, "table_setup.tex"), "w") as f:
                f.write(gen_setup_latex(uvc, g16, nova))
            print(f"  Saved: {args.save_dir}/table_setup.tex")

            with open(os.path.join(args.save_dir, "figure_coords.tex"), "w") as f:
                f.write(gen_figure2_latex(uvc, g16, nova))
            print(f"  Saved: {args.save_dir}/figure_coords.tex")
    else:
        print_summary(args.csv_dir)


if __name__ == "__main__":
    main()
