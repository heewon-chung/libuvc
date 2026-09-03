#!/usr/bin/env python3
"""
summarize_results.py — Parse UVC benchmark CSVs and produce comparison tables.

Usage:
    python3 scripts/summarize_results.py results/.../csv
    python3 scripts/summarize_results.py results/.../csv --save-dir results/...
    python3 scripts/summarize_results.py results/.../csv --latex
"""

import argparse
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bench_data
from bench_data import GROTH16_MODES


# ── Paper display names ───────────────────────────────────────────

CIRCUIT_DISPLAY = {
    "mimc": "MiMC hash chain",
    "hadamard": "Hadamard product",
    "matmul": "Hadamard product",      # legacy name
    "scalable": "Repeated squaring",
    "sensor_fusion": "Sensor fusion",
    "pagerank": "PageRank",
}

CIRCUIT_ORDER = {
    "mimc": 0, "hadamard": 1, "matmul": 1,
    "scalable": 2, "sensor_fusion": 3, "pagerank": 4,
}


# ── CSV readers ───────────────────────────────────────────────────

CANONICAL_UVC_HEADER = bench_data.CANONICAL_UVC_HEADER
normalize_circuit = bench_data.normalize_circuit


def circuit_sort_key(circuit, n):
    """Sort key matching paper order."""
    return (CIRCUIT_ORDER.get(circuit, 99), int(n))


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


def fmt_bytes(val):
    """Format a (possibly aggregated float) byte count."""
    if val is None or val == "":
        return "---"
    return str(int(float(val)))


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

def g16_row(data, mode, circuit, n, B, step):
    """One Groth16 mode row, with the legacy fallback for on-demand."""
    row = data["g16_modes"].get((mode, circuit, n, B, step))
    if row is None and mode == "ondemand":
        row = data["g16_legacy"].get((circuit, n, step))
    return row or {}


def any_b(data, circuit, n):
    """Any B value measured for this (circuit, n); used by B-independent lookups."""
    bs = sorted({B for (c, nn, B, _s) in data["uvc"] if c == circuit and nn == n})
    return bs[0] if bs else None


def get_circuit_configs(data):
    """Get unique (circuit, n) pairs sorted in paper order."""
    configs = {(c, n) for (c, n, _B, _step) in data["uvc"]}
    return sorted(configs, key=lambda x: circuit_sort_key(x[0], x[1]))


def get_b_values(data):
    """Get sorted unique B values from UVC data."""
    return sorted({B for (_c, _n, B, _step) in data["uvc"]})


def get_display_steps(data):
    """Get representative step values for display.

    Uses powers of 4 (1, 4, 16, 64, 256, 1024, ...) that exist in data,
    plus the maximum step if not already included.
    """
    all_steps = {step for (_c, _n, _B, step) in data["uvc"]}
    if not all_steps:
        return [1, 4, 16, 64]
    max_step = max(all_steps)
    display = []
    s = 1
    while s <= max_step:
        if s in all_steps:
            display.append(s)
        s *= 4
    if max_step not in display and max_step in all_steps:
        display.append(max_step)
    return sorted(display)


# ── ASCII table builder ──────────────────────────────────────────

def format_table(title, headers, rows):
    """Build a formatted ASCII table and return as string."""
    col_widths = []
    for i, h in enumerate(headers):
        w = len(h)
        for row in rows:
            if i < len(row):
                w = max(w, len(str(row[i])))
        col_widths.append(w)

    lines = []
    width = sum(col_widths) + 2 * (len(col_widths) - 1)
    width = max(width, len(title) + 4, 70)

    lines.append("")
    lines.append("=" * width)
    lines.append(f"  {title}")
    lines.append("=" * width)

    hdr = "  ".join(h.rjust(w) for h, w in zip(headers, col_widths))
    sep = "  ".join("-" * w for w in col_widths)
    lines.append(hdr)
    lines.append(sep)

    for row in rows:
        line = "  ".join(str(v).rjust(w) for v, w in zip(row, col_widths))
        lines.append(line)

    return "\n".join(lines) + "\n"


# ── Table generators (return strings) ────────────────────────────

def gen_prover_table(data):
    """Generate per-step proving time comparison with dynamic B columns."""
    configs = get_circuit_configs(data)
    b_values = get_b_values(data)
    steps = get_display_steps(data)

    headers = ["Circuit", "n", "Step"]
    for B in b_values:
        headers.append(f"Ours(B={B})")
    headers.extend(["G16 on-dem", "G16 fixCRS", "G16 single",
                    "Nova fold", "Nova compr"])

    num_cols = len(headers)
    rows = []

    for circuit, n in configs:
        ref_b = any_b(data, circuit, n)
        for j_idx, j in enumerate(steps):
            circ_col = CIRCUIT_DISPLAY.get(circuit, circuit) if j_idx == 0 else ""
            n_col = str(n) if j_idx == 0 else ""

            row = [circ_col, n_col, str(j)]
            for B in b_values:
                uvc_r = data["uvc"].get((circuit, n, B, j), {})
                row.append(fmt_ms(uvc_r.get("prove_ms")))

            for mode in GROTH16_MODES:
                row.append(fmt_ms(g16_row(data, mode, circuit, n, ref_b, j).get("prove_ms")))
            nv = data["nova"].get((circuit, n, j), {})
            row.append(fmt_ms(nv.get("fold_ms")))
            row.append(fmt_ms(nv.get("compress_ms")))
            rows.append(row)
        rows.append([""] * num_cols)

    return format_table("Per-step Proving Time (ms)", headers, rows)


def gen_verify_table(data):
    """Generate verification time comparison.

    Verification time is constant across B for UVC (always 3 pairings),
    so show a single UVC column. Falls through B values to find data.
    """
    configs = get_circuit_configs(data)
    b_values = get_b_values(data)
    steps = get_display_steps(data)

    headers = ["Circuit", "n", "Step", "Ours", "G16 on-dem",
               "G16 fixCRS", "G16 single", "Nova"]
    num_cols = len(headers)
    rows = []

    for circuit, n in configs:
        ref_b = any_b(data, circuit, n)
        for j_idx, j in enumerate(steps):
            circ_col = CIRCUIT_DISPLAY.get(circuit, circuit) if j_idx == 0 else ""
            n_col = str(n) if j_idx == 0 else ""

            # Try each B until we find data for this step
            uvc_r = {}
            for B in b_values:
                uvc_r = data["uvc"].get((circuit, n, B, j), {})
                if uvc_r:
                    break

            nv = data["nova"].get((circuit, n, j), {})

            row = [circ_col, n_col, str(j), fmt_ms(uvc_r.get("verify_ms"))]
            for mode in GROTH16_MODES:
                row.append(fmt_ms(g16_row(data, mode, circuit, n, ref_b, j).get("verify_ms")))
            row.append(fmt_ms(nv.get("verify_ms")))
            rows.append(row)
        rows.append([""] * num_cols)

    return format_table("Per-step Verification Time (ms)", headers, rows)


def gen_setup_table(data):
    """Generate setup cost comparison with dynamic B columns."""
    configs = get_circuit_configs(data)
    b_values = get_b_values(data)

    headers = ["Circuit", "n"]
    for B in b_values:
        headers.append(f"Ours(B={B})")
    headers.extend(["G16 on-dem(j=1)", "G16 fixCRS", "G16 single", "Nova"])
    rows = []

    for circuit, n in configs:
        ref_b = any_b(data, circuit, n)
        row = [CIRCUIT_DISPLAY.get(circuit, circuit), str(n)]
        for B in b_values:
            uvc_r = data["uvc"].get((circuit, n, B, 1), {})
            row.append(fmt_sec(uvc_r.get("setup_ms")))

        for mode in GROTH16_MODES:
            row.append(fmt_sec(g16_row(data, mode, circuit, n, ref_b, 1).get("setup_ms")))
        nv = data["nova"].get((circuit, n, 1), {})
        row.append(fmt_sec(nv.get("setup_ms")))
        rows.append(row)

    return format_table("Setup Cost (seconds)", headers, rows)


def gen_proof_size_table(data):
    """Generate proof size comparison."""
    configs = get_circuit_configs(data)
    b_values = get_b_values(data)
    headers = ["Circuit", "n", "Ours", "Groth16", "Nova"]
    rows = []

    for circuit, n in configs:
        # Use any available B value for proof size (constant across B)
        uvc_r = {}
        for B in b_values:
            uvc_r = data["uvc"].get((circuit, n, B, 1), {})
            if uvc_r:
                break

        g = g16_row(data, "ondemand", circuit, n, any_b(data, circuit, n), 1)
        nv = data["nova"].get((circuit, n, 1), {})

        rows.append([
            CIRCUIT_DISPLAY.get(circuit, circuit), str(n),
            uvc_r.get("proof_bytes", "---"),
            g.get("proof_bytes", "---"),
            fmt_bytes(nv.get("proof_bytes")),
        ])

    return format_table("Proof Size (bytes)", headers, rows)


def gen_crossover_table(data):
    """Generate crossover analysis table with dynamic B columns.

    For each B, finds j* where UVC per-step update cost first becomes less
    than Groth16's re-prove-from-scratch cost (setup_j + prove_j) at step j.
    This is the marginal crossover: beyond j*, every additional step is
    cheaper with UVC than re-proving with Groth16.
    """
    configs = get_circuit_configs(data)
    b_values = get_b_values(data)

    headers = ["Circuit", "n"]
    for B in b_values:
        headers.append(f"B={B} j*")
    rows = []

    for circuit, n in configs:
        row = [CIRCUIT_DISPLAY.get(circuit, circuit), str(n)]
        for B in b_values:
            # Get UVC per-step proving time (median across all steps for this B)
            uvc_prove_times = []
            for (c2, n2, B2, s2) in data["uvc"]:
                if c2 == circuit and n2 == n and B2 == B:
                    uvc_prove_times.append(
                        float(data["uvc"][(c2, n2, B2, s2)]["prove_ms"]))
            if not uvc_prove_times:
                row.append("---")
                continue
            uvc_prove_per_step = sorted(uvc_prove_times)[len(uvc_prove_times) // 2]

            # Collect all available Groth16 on-demand steps for this (circuit, n)
            g16_steps = bench_data.measured_steps(B)

            found = None
            for j in g16_steps:
                g16_r = g16_row(data, "ondemand", circuit, n, B, j)
                if not g16_r:
                    continue
                g16_cost = float(g16_r["setup_ms"]) + float(g16_r["prove_ms"])
                if uvc_prove_per_step < g16_cost:
                    found = j
                    break

            if found:
                row.append(str(found))
            else:
                max_step = max(g16_steps) if g16_steps else B
                row.append(f">{max_step}")

        rows.append(row)

    return format_table(
        "Marginal Crossover (j* where UVC update < Groth16 re-prove at step j)",
        headers, rows)


def gen_memory_table(data):
    """Generate CRS size and peak memory table."""
    configs = get_circuit_configs(data)
    b_values = get_b_values(data)

    headers = ["Circuit", "n", "B", "CRS G1", "CRS G2", "Peak Mem (MB)"]
    rows = []

    for circuit, n in configs:
        for b_idx, B in enumerate(b_values):
            uvc_r = data["uvc"].get((circuit, n, B, 1), {})
            if not uvc_r:
                continue
            circ_col = CIRCUIT_DISPLAY.get(circuit, circuit) if b_idx == 0 else ""
            n_col = str(n) if b_idx == 0 else ""
            rows.append([
                circ_col, n_col, str(B),
                uvc_r.get("crs_g1", "---"),
                uvc_r.get("crs_g2", "---"),
                uvc_r.get("peak_mem_mb", "---"),
            ])

    return format_table("CRS Size and Memory Usage", headers, rows)


def gen_cumulative_table(data):
    """Generate cumulative total-time comparison for every (circuit, n, B).

    Series: UVC, the three Groth16 modes, and both Nova kinds, all at step = B.
    """
    headers = ["Circuit", "n", "B",
               "UVC(s)", "G16 on-dem(s)", "G16 fixCRS(s)", "G16 single(s)",
               "Nova fold(s)", "Nova compr(s)",
               "UVC/G16od", "UVC/G16fix", "UVC/G16ss",
               "UVC/NovaF", "UVC/NovaC"]
    rows = []

    for circuit, n, B in bench_data.uvc_configs(data):
        uvc_total = bench_data.cumulative_uvc(data, circuit, n, B, B)
        series = [bench_data.cumulative_groth16(data, m, circuit, n, B, B)
                  for m in GROTH16_MODES]
        series += [bench_data.cumulative_nova(data, k, circuit, n, B)
                   for k in bench_data.NOVA_KINDS]

        def sec(v):
            return f"{v / 1000.0:.1f}" if v is not None else "---"

        def ratio(v):
            return f"{uvc_total / v:.2f}x" if (v and uvc_total is not None) else "---"

        rows.append([CIRCUIT_DISPLAY.get(circuit, circuit), str(n), str(B),
                     sec(uvc_total)] + [sec(v) for v in series]
                    + [ratio(v) for v in series])

    return format_table("Cumulative Total Time (seconds) - setup + all proving steps",
                        headers, rows)


def gen_cumulative_csv(data, csv_path):
    """Write per-step cumulative time for every series to CSV for plotting."""
    fieldnames = [
        "circuit", "n", "B", "step", "uvc_cum_s",
        "g16_ondemand_cum_s", "g16_ondemand_prove_only_cum_s",
        "g16_fixedcrs_cum_s", "g16_singlestep_cum_s",
        "nova_foldonly_cum_s", "nova_compressed_cum_s",
    ]

    def sec(v):
        return f"{v / 1000.0:.4f}" if v is not None else ""

    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()

        for circuit, n, B in bench_data.uvc_configs(data):
            for step in bench_data.measured_steps(B):
                writer.writerow({
                    "circuit": circuit,
                    "n": n,
                    "B": B,
                    "step": step,
                    "uvc_cum_s": sec(bench_data.cumulative_uvc(data, circuit, n, B, step)),
                    "g16_ondemand_cum_s": sec(bench_data.cumulative_groth16(
                        data, "ondemand", circuit, n, B, step)),
                    "g16_ondemand_prove_only_cum_s": sec(bench_data.cumulative_groth16(
                        data, "ondemand", circuit, n, B, step, proving_only=True)),
                    "g16_fixedcrs_cum_s": sec(bench_data.cumulative_groth16(
                        data, "fixedcrs", circuit, n, B, step)),
                    "g16_singlestep_cum_s": sec(bench_data.cumulative_groth16(
                        data, "singlestep", circuit, n, B, step)),
                    "nova_foldonly_cum_s": sec(bench_data.cumulative_nova(
                        data, "foldonly", circuit, n, step)),
                    "nova_compressed_cum_s": sec(bench_data.cumulative_nova(
                        data, "compressed", circuit, n, step)),
                })

    print(f"  Saved: {os.path.basename(csv_path)}")


# ── Save to .txt files ───────────────────────────────────────────

TABLE_DEFS = [
    ("T01_proving_time",      "gen_prover_table"),
    ("T02_verification_time", "gen_verify_table"),
    ("T03_setup_cost",        "gen_setup_table"),
    ("T04_proof_size",        "gen_proof_size_table"),
    ("T05_crossover",         "gen_crossover_table"),
    ("T06_memory",            "gen_memory_table"),
    ("T07_cumulative",        "gen_cumulative_table"),
]

def save_tables(csv_dir, save_dir):
    """Save individual .txt table files and summary.txt to tables/ dir."""
    data = bench_data.load_run(csv_dir)
    if not data["uvc"]:
        print("No UVC data found.", file=sys.stderr)
        return

    tables_dir = os.path.join(save_dir, "tables")
    os.makedirs(tables_dir, exist_ok=True)

    # Read system_info.txt if it exists in the save_dir
    sys_info_path = os.path.join(save_dir, "system_info.txt")
    sys_info = ""
    if os.path.exists(sys_info_path):
        with open(sys_info_path) as f:
            sys_info = f.read()

    gen_funcs = {
        "gen_prover_table": gen_prover_table,
        "gen_verify_table": gen_verify_table,
        "gen_setup_table": gen_setup_table,
        "gen_proof_size_table": gen_proof_size_table,
        "gen_crossover_table": gen_crossover_table,
        "gen_memory_table": gen_memory_table,
        "gen_cumulative_table": gen_cumulative_table,
    }

    # Generate each table and save as individual .txt file
    all_tables = []
    for filename, func_name in TABLE_DEFS:
        table_text = gen_funcs[func_name](data)

        txt_path = os.path.join(tables_dir, f"{filename}.txt")
        with open(txt_path, "w") as f:
            f.write(table_text)
        print(f"  Saved: tables/{filename}.txt")

        all_tables.append(table_text)

    # Build summary.txt = system info + all tables
    summary_lines = []
    if sys_info:
        summary_lines.append("=" * 70)
        summary_lines.append("  System Information")
        summary_lines.append("=" * 70)
        summary_lines.append(sys_info.rstrip())
        summary_lines.append("")

    for t in all_tables:
        summary_lines.append(t)

    summary_path = os.path.join(tables_dir, "summary.txt")
    with open(summary_path, "w") as f:
        f.write("\n".join(summary_lines) + "\n")
    print(f"  Saved: tables/summary.txt")

    # Also write LaTeX tables
    latex_path = os.path.join(tables_dir, "tables_latex.tex")
    with open(latex_path, "w") as f:
        f.write(gen_proving_latex(data))
        f.write("\n\n")
        f.write(gen_verify_proof_latex(data))
        f.write("\n\n")
        f.write(gen_setup_latex(data))
    print(f"  Saved: tables/tables_latex.tex")

    # Per-step cumulative proving time CSV (for plotting)
    cum_csv_path = os.path.join(csv_dir, "cumulative_prove_time.csv")
    gen_cumulative_csv(data, cum_csv_path)

    return summary_path


# ── Terminal output ──────────────────────────────────────────────

def print_summary(csv_dir):
    """Print full terminal summary."""
    data = bench_data.load_run(csv_dir)

    if not data["uvc"]:
        print("No UVC data found.")
        return

    print()
    print("############################################################")
    print("  UVC Benchmark Summary")
    print(f"  Data: {csv_dir}")
    print(f"  UVC rows: {len(data['uvc'])}, "
          f"Groth16 rows: {len(data['g16_modes']) or len(data['g16_legacy'])}, "
          f"Nova rows: {len(data['nova'])}")
    print("############################################################")

    print(gen_prover_table(data))
    print(gen_verify_table(data))
    print(gen_setup_table(data))
    print(gen_proof_size_table(data))
    print(gen_crossover_table(data))
    print(gen_memory_table(data))
    print(gen_cumulative_table(data))
    print()


# ── LaTeX output ──────────────────────────────────────────────────

def latex_circuit_name(circuit):
    """LaTeX-formatted circuit name for multirow."""
    names = {
        "mimc": r"\shortstack{MiMC\\hash chain}",
        "hadamard": r"\shortstack{Hadamard\\product}",
        "scalable": r"\shortstack{Repeated\\squaring}",
        "sensor_fusion": r"\shortstack{Sensor\\fusion}",
        "pagerank": r"PageRank",
    }
    return names.get(circuit, circuit)


def latex_n(n):
    """Format n for LaTeX (use 2^k for powers of 2 >= 1024)."""
    import math
    if n >= 1024 and math.log2(n) == int(math.log2(n)):
        return f"$2^{{{int(math.log2(n))}}}$"
    return str(n)


def gen_proving_latex(data):
    """Generate per-step proving time LaTeX table with dynamic B columns."""
    configs = get_circuit_configs(data)
    b_values = get_b_values(data)
    steps = get_display_steps(data)
    num_b = len(b_values)

    lines = []
    lines.append(r"% Auto-generated by summarize_results.py")
    lines.append(r"\begin{table*}[t]")
    lines.append(r"\centering")
    lines.append(r"\caption{Per-step proving time (ms). "
                 r"$K$ is the number of sensors and $B$ is the step bound for our scheme. "
                 r"Our proving time is constant across steps for each~$B$.}")
    lines.append(r"\label{tab:bench-proving}")
    lines.append(r"\renewcommand{\arraystretch}{1.2}")
    lines.append(r"{\footnotesize\setlength{\tabcolsep}{3pt}")

    # Columns: Circuit, K, Step, Ours(B=b1), ..., Ours(B=bN), Groth16, Nova
    col_spec = "@{}ccr" + "c" * num_b + "ccccc@{}"
    lines.append(rf"\begin{{tabular}}{{{col_spec}}}")
    lines.append(r"\toprule")

    # Sub-headers
    sub_parts = [r"\textbf{Circuit}", "$K$", r"\textbf{Step}"]
    for B in b_values:
        sub_parts.append(rf"\textbf{{Ours}} ($B\!=\!{B}$)")
    sub_parts.extend([r"\textbf{G16 on-dem.}", r"\textbf{G16 fix.\ CRS}",
                      r"\textbf{G16 single}", r"\textbf{Nova fold}",
                      r"\textbf{Nova compr.}"])
    lines.append(" & ".join(sub_parts) + r" \\")
    lines.append(r"\midrule")

    for ci, (circuit, n) in enumerate(configs):
        num_steps = len(steps)
        ref_b = any_b(data, circuit, n)

        for j_idx, j in enumerate(steps):
            nv = data["nova"].get((circuit, n, j), {})

            if j_idx == 0:
                circ = rf"\multirow{{{num_steps}}}{{*}}{{{latex_circuit_name(circuit)}}}"
                n_col = rf"\multirow{{{num_steps}}}{{*}}{{{n}}}"
            else:
                circ = ""
                n_col = ""

            parts = [circ, n_col, str(j)]
            for B in b_values:
                uvc_r = data["uvc"].get((circuit, n, B, j), {})
                parts.append(fmt_ms(uvc_r.get("prove_ms")))
            for mode in GROTH16_MODES:
                parts.append(fmt_ms(g16_row(data, mode, circuit, n, ref_b, j).get("prove_ms")))
            parts.append(fmt_ms(nv.get("fold_ms")))
            parts.append(fmt_ms(nv.get("compress_ms")))
            lines.append(" & ".join(parts) + r" \\")

        if ci < len(configs) - 1:
            lines.append(r"\midrule")

    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"}% end footnotesize")
    lines.append(r"\end{table*}")

    return "\n".join(lines)


def gen_verify_proof_latex(data):
    """Generate verification time + proof size LaTeX table.

    Verification is constant across B for UVC, so one column suffices.
    """
    configs = get_circuit_configs(data)
    b_values = get_b_values(data)
    ref_b = b_values[0] if b_values else 64

    lines = []
    lines.append(r"% Auto-generated verification + proof size")
    lines.append(r"\begin{table}[t]")
    lines.append(r"\centering")
    lines.append(r"\caption{Verification time (ms) and proof size (bytes). "
                 r"UVC verification is independent of~$B$.}")
    lines.append(r"\label{tab:bench-verify}")
    lines.append(r"\renewcommand{\arraystretch}{1.2}")
    lines.append(r"{\footnotesize")
    lines.append(r"\begin{tabular}{@{}cc ccc ccc @{}}")
    lines.append(r"\toprule")
    lines.append(r"& & \multicolumn{3}{c}{\textbf{Verification (ms)}} & "
                 r"\multicolumn{3}{c}{\textbf{Proof size (bytes)}} \\")
    lines.append(r"\cmidrule(lr){3-5} \cmidrule(lr){6-8}")
    lines.append(r"\textbf{Circuit} & $K$ & "
                 r"\textbf{Ours} & \textbf{Groth16} & \textbf{Nova} & "
                 r"\textbf{Ours} & \textbf{Groth16} & \textbf{Nova} \\")
    lines.append(r"\midrule")

    for circuit, n in configs:
        uvc_r = data["uvc"].get((circuit, n, ref_b, 1), {})
        g = g16_row(data, "ondemand", circuit, n, any_b(data, circuit, n), 1)
        nv = data["nova"].get((circuit, n, 1), {})

        cname = CIRCUIT_DISPLAY.get(circuit, circuit)
        lines.append(
            f"{cname} & {n} & "
            f"{fmt_ms(uvc_r.get('verify_ms'))} & "
            f"{fmt_ms(g.get('verify_ms'))} & "
            f"{fmt_ms(nv.get('verify_ms'))} & "
            f"{uvc_r.get('proof_bytes', '---')} & "
            f"{g.get('proof_bytes', '---')} & "
            f"{fmt_bytes(nv.get('proof_bytes'))} \\\\"
        )

    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"}% end footnotesize")
    lines.append(r"\end{table}")

    return "\n".join(lines)


def gen_setup_latex(data):
    """Generate setup cost comparison table with dynamic B columns."""
    configs = get_circuit_configs(data)
    b_values = get_b_values(data)

    lines = []
    lines.append(r"% Auto-generated setup comparison")
    lines.append(r"\begin{table}[t]")
    lines.append(r"\centering")
    lines.append(r"\caption{Setup cost (seconds). "
                 r"UVC setup grows with~$B$; Groth16 setup grows with step~$j$.}")
    lines.append(r"\label{tab:setup}")
    lines.append(r"\renewcommand{\arraystretch}{1.2}")
    lines.append(r"{\footnotesize")

    col_spec = "@{}ll" + "c" * len(b_values) + "cccc@{}"
    lines.append(rf"\begin{{tabular}}{{{col_spec}}}")
    lines.append(r"\toprule")

    header_parts = [r"\textbf{Circuit}", "$K$"]
    for B in b_values:
        header_parts.append(rf"\textbf{{Ours}} ($B\!=\!{B}$)")
    header_parts.extend([r"\textbf{G16 on-dem.} ($j\!=\!1$)",
                         r"\textbf{G16 fix.\ CRS}", r"\textbf{G16 single}",
                         r"\textbf{Nova}"])
    lines.append(" & ".join(header_parts) + r" \\")
    lines.append(r"\midrule")

    for circuit, n in configs:
        ref_b = any_b(data, circuit, n)
        parts = [CIRCUIT_DISPLAY.get(circuit, circuit), str(n)]
        for B in b_values:
            uvc_r = data["uvc"].get((circuit, n, B, 1), {})
            parts.append(fmt_sec(uvc_r.get("setup_ms")))
        for mode in GROTH16_MODES:
            parts.append(fmt_sec(g16_row(data, mode, circuit, n, ref_b, 1).get("setup_ms")))
        nv = data["nova"].get((circuit, n, 1), {})
        parts.append(fmt_sec(nv.get("setup_ms")))
        lines.append(" & ".join(parts) + r" \\")

    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"}% end footnotesize")
    lines.append(r"\end{table}")

    return "\n".join(lines)


def print_latex(csv_dir):
    """Generate all LaTeX output to stdout."""
    data = bench_data.load_run(csv_dir)

    if not data["uvc"]:
        print("% No UVC data found.", file=sys.stderr)
        return

    print(gen_proving_latex(data))
    print()
    print()
    print(gen_verify_proof_latex(data))
    print()
    print()
    print(gen_setup_latex(data))


# ── Main ──────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Summarize UVC benchmark results into tables.")
    parser.add_argument("csv_dir",
        help="Directory containing {uvc,groth16,nova}_results.csv")
    parser.add_argument("--latex", action="store_true",
        help="Output LaTeX tables to stdout")
    parser.add_argument("--save-dir",
        help="Save numbered .txt table files to SAVE_DIR/tables/")
    args = parser.parse_args()

    if args.latex:
        print_latex(args.csv_dir)
    elif args.save_dir:
        print_summary(args.csv_dir)
        save_tables(args.csv_dir, args.save_dir)
    else:
        print_summary(args.csv_dir)


if __name__ == "__main__":
    main()
