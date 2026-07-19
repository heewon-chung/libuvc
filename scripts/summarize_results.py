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

CANONICAL_UVC_HEADER = [
    "scheme", "circuit", "n", "B", "step", "setup_ms", "prove_ms",
    "verify_ms", "crs_g1_published", "crs_g2", "vk_st_abc_g1",
    "proof_bytes", "proof_bytes_compressed", "peak_mem_mb", "commit",
]


def read_csv(path):
    """Read a CSV file, return list of dicts."""
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def read_canonical_uvc(path):
    """Read canonical UVC rows or terminate with a path-specific error."""
    try:
        with open(path, newline="") as f:
            reader = csv.DictReader(f)
            if reader.fieldnames != CANONICAL_UVC_HEADER:
                raise ValueError
            rows = list(reader)
    except (OSError, ValueError):
        sys.exit(f"FATAL: {path}: not a canonical uvc_gamma_v1 results file (expected canonical header and scheme)")
    if any(row.get("scheme") != "uvc_gamma_v1" for row in rows):
        sys.exit(f"FATAL: {path}: not a canonical uvc_gamma_v1 results file (expected canonical header and scheme)")
    return rows


def normalize_circuit(name):
    """Normalize legacy circuit names."""
    if name == "matmul":
        return "hadamard"
    return name


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
    uvc_rows = read_canonical_uvc(os.path.join(csv_dir, "uvc_results.csv"))
    g16_rows = read_csv(os.path.join(csv_dir, "groth16_results.csv"))
    nova_rows = read_csv(os.path.join(csv_dir, "nova_results.csv"))

    # Index UVC by (circuit, n, B, step)
    uvc = {}
    for r in uvc_rows:
        c = normalize_circuit(r["circuit"])
        key = (c, int(r["n"]), int(r["B"]), int(r["step"]))
        uvc[key] = r

    # Groth16 by (circuit, n, step) — deduplicate (keep first)
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


def get_b_values(uvc):
    """Get sorted unique B values from UVC data."""
    return sorted(set(B for (c, n, B, step) in uvc.keys()))


def get_display_steps(uvc):
    """Get representative step values for display.

    Uses powers of 4 (1, 4, 16, 64, 256, 1024, ...) that exist in data,
    plus the maximum step if not already included.
    """
    all_steps = set(step for (c, n, B, step) in uvc.keys())
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

def gen_prover_table(uvc, g16, nova):
    """Generate per-step proving time comparison with dynamic B columns."""
    configs = get_circuit_configs(uvc)
    b_values = get_b_values(uvc)
    steps = get_display_steps(uvc)

    headers = ["Circuit", "n", "Step"]
    for B in b_values:
        headers.append(f"Ours(B={B})")
    headers.extend(["Groth16", "Nova"])

    num_cols = len(headers)
    rows = []

    for circuit, n in configs:
        for j_idx, j in enumerate(steps):
            circ_col = CIRCUIT_DISPLAY.get(circuit, circuit) if j_idx == 0 else ""
            n_col = str(n) if j_idx == 0 else ""

            row = [circ_col, n_col, str(j)]
            for B in b_values:
                uvc_r = uvc.get((circuit, n, B, j), {})
                row.append(fmt_ms(uvc_r.get("prove_ms")))

            g = g16.get((circuit, n, j), {})
            nv = nova.get((circuit, n, j), {})
            row.append(fmt_ms(g.get("prove_ms")))
            row.append(fmt_ms(nv.get("fold_ms")))
            rows.append(row)
        rows.append([""] * num_cols)

    return format_table("Per-step Proving Time (ms)", headers, rows)


def gen_verify_table(uvc, g16, nova):
    """Generate verification time comparison.

    Verification time is constant across B for UVC (always 3 pairings),
    so show a single UVC column. Falls through B values to find data.
    """
    configs = get_circuit_configs(uvc)
    b_values = get_b_values(uvc)
    steps = get_display_steps(uvc)

    headers = ["Circuit", "n", "Step", "Ours", "Groth16", "Nova"]
    num_cols = len(headers)
    rows = []

    for circuit, n in configs:
        for j_idx, j in enumerate(steps):
            circ_col = CIRCUIT_DISPLAY.get(circuit, circuit) if j_idx == 0 else ""
            n_col = str(n) if j_idx == 0 else ""

            # Try each B until we find data for this step
            uvc_r = {}
            for B in b_values:
                uvc_r = uvc.get((circuit, n, B, j), {})
                if uvc_r:
                    break

            g = g16.get((circuit, n, j), {})
            nv = nova.get((circuit, n, j), {})

            rows.append([
                circ_col, n_col, str(j),
                fmt_ms(uvc_r.get("verify_ms")),
                fmt_ms(g.get("verify_ms")),
                fmt_ms(nv.get("verify_ms")),
            ])
        rows.append([""] * num_cols)

    return format_table("Per-step Verification Time (ms)", headers, rows)


def gen_setup_table(uvc, g16, nova):
    """Generate setup cost comparison with dynamic B columns."""
    configs = get_circuit_configs(uvc)
    b_values = get_b_values(uvc)

    headers = ["Circuit", "n"]
    for B in b_values:
        headers.append(f"Ours(B={B})")
    headers.extend(["Groth16(j=1)", "Nova"])
    rows = []

    for circuit, n in configs:
        row = [CIRCUIT_DISPLAY.get(circuit, circuit), str(n)]
        for B in b_values:
            uvc_r = uvc.get((circuit, n, B, 1), {})
            row.append(fmt_sec(uvc_r.get("setup_ms")))

        g = g16.get((circuit, n, 1), {})
        nv = nova.get((circuit, n, 1), {})
        row.append(fmt_sec(g.get("setup_ms")))
        row.append(fmt_sec(nv.get("setup_ms")))
        rows.append(row)

    return format_table("Setup Cost (seconds)", headers, rows)


def gen_proof_size_table(uvc, g16, nova):
    """Generate proof size comparison."""
    configs = get_circuit_configs(uvc)
    b_values = get_b_values(uvc)
    headers = ["Circuit", "n", "Ours", "Groth16", "Nova"]
    rows = []

    for circuit, n in configs:
        # Use any available B value for proof size (constant across B)
        uvc_r = {}
        for B in b_values:
            uvc_r = uvc.get((circuit, n, B, 1), {})
            if uvc_r:
                break

        g = g16.get((circuit, n, 1), {})
        nv = nova.get((circuit, n, 1), {})

        rows.append([
            CIRCUIT_DISPLAY.get(circuit, circuit), str(n),
            uvc_r.get("proof_bytes", "---"),
            g.get("proof_bytes", "---"),
            nv.get("proof_bytes", "---"),
        ])

    return format_table("Proof Size (bytes)", headers, rows)


def gen_crossover_table(uvc, g16):
    """Generate crossover analysis table with dynamic B columns.

    For each B, finds j* where UVC per-step update cost first becomes less
    than Groth16's re-prove-from-scratch cost (setup_j + prove_j) at step j.
    This is the marginal crossover: beyond j*, every additional step is
    cheaper with UVC than re-proving with Groth16.
    """
    configs = get_circuit_configs(uvc)
    b_values = get_b_values(uvc)

    headers = ["Circuit", "n"]
    for B in b_values:
        headers.append(f"B={B} j*")
    rows = []

    for circuit, n in configs:
        row = [CIRCUIT_DISPLAY.get(circuit, circuit), str(n)]
        for B in b_values:
            # Get UVC per-step proving time (median across all steps for this B)
            uvc_prove_times = []
            for (c2, n2, B2, s2) in uvc:
                if c2 == circuit and n2 == n and B2 == B:
                    uvc_prove_times.append(float(uvc[(c2, n2, B2, s2)]["prove_ms"]))
            if not uvc_prove_times:
                row.append("---")
                continue
            uvc_prove_per_step = sorted(uvc_prove_times)[len(uvc_prove_times) // 2]

            # Collect all available Groth16 steps for this (circuit, n)
            g16_steps = sorted(
                s for (c2, n2, s) in g16 if c2 == circuit and n2 == n
            )

            found = None
            for j in g16_steps:
                g16_r = g16.get((circuit, n, j))
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


def gen_memory_table(uvc):
    """Generate CRS size and peak memory table."""
    configs = get_circuit_configs(uvc)
    b_values = get_b_values(uvc)

    headers = ["Circuit", "n", "B", "CRS G1", "CRS G2", "Peak Mem (MB)"]
    rows = []

    for circuit, n in configs:
        for b_idx, B in enumerate(b_values):
            uvc_r = uvc.get((circuit, n, B, 1), {})
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


def gen_cumulative_table(uvc, g16, nova):
    """Generate cumulative proving time comparison at max B.

    Shows total time for all B steps: UVC (setup + B*prove) vs
    Groth16 (sum of setup_j + prove_j) vs Nova (setup + total_fold + compress).
    """
    configs = get_circuit_configs(uvc)
    b_values = get_b_values(uvc)
    max_b = max(b_values) if b_values else 64

    headers = ["Circuit", "n", "B",
               "UVC total(s)", "G16 total(s)", "Nova total(s)",
               "UVC/G16", "UVC/Nova"]
    rows = []

    for circuit, n in configs:
        for B in b_values:
            uvc_setup_r = uvc.get((circuit, n, B, 1))
            if not uvc_setup_r:
                continue

            uvc_setup = float(uvc_setup_r["setup_ms"])
            # Median per-step proving time
            uvc_prove_times = []
            for (c2, n2, B2, s2) in uvc:
                if c2 == circuit and n2 == n and B2 == B:
                    uvc_prove_times.append(float(uvc[(c2, n2, B2, s2)]["prove_ms"]))
            uvc_per_step = sorted(uvc_prove_times)[len(uvc_prove_times) // 2]
            uvc_total = (uvc_setup + B * uvc_per_step) / 1000.0

            # Groth16: sum over all steps up to B
            g16_total = 0.0
            g16_steps = sorted(
                s for (c2, n2, s) in g16 if c2 == circuit and n2 == n and s <= B
            )
            if g16_steps:
                # Approximate: use last available step's cost-per-step scaling
                last_step = max(g16_steps)
                g16_last = g16[(circuit, n, last_step)]
                # Groth16 cost at step j ~ (setup_1 + prove_1) * j / 1
                # More precisely: interpolate from available data
                # Sum = sum_{j=1}^{B} (setup_j + prove_j)
                # Use trapezoidal approximation with available data points
                prev_j = 0
                prev_cost = 0.0
                for j in g16_steps:
                    g16_r = g16[(circuit, n, j)]
                    cost = float(g16_r["setup_ms"]) + float(g16_r["prove_ms"])
                    # Trapezoidal: area = (j - prev_j) * (cost + prev_cost) / 2
                    g16_total += (j - prev_j) * (cost + prev_cost) / 2.0
                    prev_j = j
                    prev_cost = cost
                # Extend to B if last measured step < B
                if last_step < B:
                    g16_total += (B - last_step) * prev_cost
                g16_total /= 1000.0

            # Nova: setup + total_fold_ms at step B + compress
            nv_r = nova.get((circuit, n, B))
            nova_total = 0.0
            if nv_r:
                nova_total = (
                    float(nv_r["setup_ms"])
                    + float(nv_r["total_fold_ms"])
                    + float(nv_r["compress_ms"])
                ) / 1000.0

            circ_col = CIRCUIT_DISPLAY.get(circuit, circuit)
            ratio_g16 = f"{uvc_total/g16_total:.2f}x" if g16_total > 0 else "---"
            ratio_nova = f"{uvc_total/nova_total:.2f}x" if nova_total > 0 else "---"

            rows.append([
                circ_col, str(n), str(B),
                f"{uvc_total:.1f}",
                f"{g16_total:.1f}" if g16_total > 0 else "---",
                f"{nova_total:.1f}" if nova_total > 0 else "---",
                ratio_g16, ratio_nova,
            ])

    return format_table("Cumulative Total Time (seconds) — setup + all proving steps",
                        headers, rows)


def gen_cumulative_csv(uvc, g16, nova, csv_path):
    """Write per-step cumulative proving time to CSV for plotting.

    For each (circuit, n, B) config, outputs cumulative time at every
    power-of-2 step up to B.
    """
    configs = get_circuit_configs(uvc)
    b_values = get_b_values(uvc)

    fieldnames = [
        "circuit", "n", "B", "step",
        "uvc_cum_s", "g16_cum_s", "nova_cum_s",
    ]

    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()

        for circuit, n in configs:
            for B in b_values:
                uvc_setup_r = uvc.get((circuit, n, B, 1))
                if not uvc_setup_r:
                    continue

                uvc_setup = float(uvc_setup_r["setup_ms"])
                # Median per-step proving time
                uvc_prove_times = []
                for (c2, n2, B2, s2) in uvc:
                    if c2 == circuit and n2 == n and B2 == B:
                        uvc_prove_times.append(
                            float(uvc[(c2, n2, B2, s2)]["prove_ms"]))
                uvc_per_step = sorted(uvc_prove_times)[
                    len(uvc_prove_times) // 2]

                # Groth16 measured steps and per-step costs
                g16_measured = sorted(
                    s for (c2, n2, s) in g16
                    if c2 == circuit and n2 == n
                )
                g16_cost_at = {}
                for s in g16_measured:
                    r = g16[(circuit, n, s)]
                    g16_cost_at[s] = (
                        float(r["setup_ms"]) + float(r["prove_ms"]))

                # Nova setup + compress (constant across steps)
                nova_measured = sorted(
                    s for (c2, n2, s) in nova
                    if c2 == circuit and n2 == n
                )
                nova_setup = (float(nova[(circuit, n, nova_measured[0])][
                    "setup_ms"]) if nova_measured else 0)
                nova_compress = (float(nova[(circuit, n, nova_measured[0])][
                    "compress_ms"]) if nova_measured else 0)

                # Display steps: powers of 2 up to B
                display = []
                s = 1
                while s <= B:
                    display.append(s)
                    s *= 2
                if B not in display:
                    display.append(B)

                for step in display:
                    # UVC cumulative
                    uvc_cum = (uvc_setup + step * uvc_per_step) / 1000.0

                    # Groth16 cumulative (trapezoidal interpolation)
                    g16_cum = 0.0
                    prev_j, prev_cost = 0, 0.0
                    for j in g16_measured:
                        if j > step:
                            break
                        cost = g16_cost_at[j]
                        g16_cum += (j - prev_j) * (cost + prev_cost) / 2.0
                        prev_j, prev_cost = j, cost
                    if prev_j < step:
                        g16_cum += (step - prev_j) * prev_cost
                    g16_cum /= 1000.0

                    # Nova cumulative
                    nv_r = nova.get((circuit, n, step))
                    nova_cum = None
                    if nv_r:
                        nova_cum = (
                            float(nv_r["setup_ms"])
                            + float(nv_r["total_fold_ms"])
                            + float(nv_r["compress_ms"])
                        ) / 1000.0

                    writer.writerow({
                        "circuit": circuit,
                        "n": n,
                        "B": B,
                        "step": step,
                        "uvc_cum_s": f"{uvc_cum:.4f}",
                        "g16_cum_s": f"{g16_cum:.4f}",
                        "nova_cum_s": (f"{nova_cum:.4f}"
                                       if nova_cum is not None else ""),
                    })

    print(f"  Saved: {os.path.basename(csv_path)}")


# ── Save to .txt files ───────────────────────────────────────────

TABLE_DEFS = [
    ("T01_proving_time",      "gen_prover_table",      ("uvc", "g16", "nova")),
    ("T02_verification_time", "gen_verify_table",       ("uvc", "g16", "nova")),
    ("T03_setup_cost",        "gen_setup_table",        ("uvc", "g16", "nova")),
    ("T04_proof_size",        "gen_proof_size_table",   ("uvc", "g16", "nova")),
    ("T05_crossover",         "gen_crossover_table",    ("uvc", "g16")),
    ("T06_memory",            "gen_memory_table",       ("uvc",)),
    ("T07_cumulative",        "gen_cumulative_table",   ("uvc", "g16", "nova")),
]

def save_tables(csv_dir, save_dir):
    """Save individual .txt table files and summary.txt to tables/ dir."""
    uvc, g16, nova = load_all(csv_dir)
    if not uvc:
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

    data = {"uvc": uvc, "g16": g16, "nova": nova}
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
    for filename, func_name, arg_names in TABLE_DEFS:
        func = gen_funcs[func_name]
        args = [data[a] for a in arg_names]
        table_text = func(*args)

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
        f.write(gen_proving_latex(uvc, g16, nova))
        f.write("\n\n")
        f.write(gen_verify_proof_latex(uvc, g16, nova))
        f.write("\n\n")
        f.write(gen_setup_latex(uvc, g16, nova))
    print(f"  Saved: tables/tables_latex.tex")

    # Per-step cumulative proving time CSV (for plotting)
    cum_csv_path = os.path.join(csv_dir, "cumulative_prove_time.csv")
    gen_cumulative_csv(uvc, g16, nova, cum_csv_path)

    return summary_path


# ── Terminal output ──────────────────────────────────────────────

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

    print(gen_prover_table(uvc, g16, nova))
    print(gen_verify_table(uvc, g16, nova))
    print(gen_setup_table(uvc, g16, nova))
    print(gen_proof_size_table(uvc, g16, nova))
    print(gen_crossover_table(uvc, g16))
    print(gen_memory_table(uvc))
    print(gen_cumulative_table(uvc, g16, nova))
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


def gen_proving_latex(uvc, g16, nova):
    """Generate per-step proving time LaTeX table with dynamic B columns."""
    configs = get_circuit_configs(uvc)
    b_values = get_b_values(uvc)
    steps = get_display_steps(uvc)
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
    col_spec = "@{}ccr" + "c" * num_b + "cc@{}"
    lines.append(rf"\begin{{tabular}}{{{col_spec}}}")
    lines.append(r"\toprule")

    # Sub-headers
    sub_parts = [r"\textbf{Circuit}", "$K$", r"\textbf{Step}"]
    for B in b_values:
        sub_parts.append(rf"\textbf{{Ours}} ($B\!=\!{B}$)")
    sub_parts.extend([r"\textbf{Groth16}", r"\textbf{Nova}"])
    lines.append(" & ".join(sub_parts) + r" \\")
    lines.append(r"\midrule")

    for ci, (circuit, n) in enumerate(configs):
        num_steps = len(steps)

        for j_idx, j in enumerate(steps):
            g = g16.get((circuit, n, j), {})
            nv = nova.get((circuit, n, j), {})

            if j_idx == 0:
                circ = rf"\multirow{{{num_steps}}}{{*}}{{{latex_circuit_name(circuit)}}}"
                n_col = rf"\multirow{{{num_steps}}}{{*}}{{{n}}}"
            else:
                circ = ""
                n_col = ""

            parts = [circ, n_col, str(j)]
            for B in b_values:
                uvc_r = uvc.get((circuit, n, B, j), {})
                parts.append(fmt_ms(uvc_r.get("prove_ms")))
            parts.append(fmt_ms(g.get("prove_ms")))
            parts.append(fmt_ms(nv.get("fold_ms")))
            lines.append(" & ".join(parts) + r" \\")

        if ci < len(configs) - 1:
            lines.append(r"\midrule")

    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"}% end footnotesize")
    lines.append(r"\end{table*}")

    return "\n".join(lines)


def gen_verify_proof_latex(uvc, g16, nova):
    """Generate verification time + proof size LaTeX table.

    Verification is constant across B for UVC, so one column suffices.
    """
    configs = get_circuit_configs(uvc)
    b_values = get_b_values(uvc)
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
        uvc_r = uvc.get((circuit, n, ref_b, 1), {})
        g = g16.get((circuit, n, 1), {})
        nv = nova.get((circuit, n, 1), {})

        cname = CIRCUIT_DISPLAY.get(circuit, circuit)
        lines.append(
            f"{cname} & {n} & "
            f"{fmt_ms(uvc_r.get('verify_ms'))} & "
            f"{fmt_ms(g.get('verify_ms'))} & "
            f"{fmt_ms(nv.get('verify_ms'))} & "
            f"{uvc_r.get('proof_bytes', '---')} & "
            f"{g.get('proof_bytes', '---')} & "
            f"{nv.get('proof_bytes', '---')} \\\\"
        )

    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"}% end footnotesize")
    lines.append(r"\end{table}")

    return "\n".join(lines)


def gen_setup_latex(uvc, g16, nova):
    """Generate setup cost comparison table with dynamic B columns."""
    configs = get_circuit_configs(uvc)
    b_values = get_b_values(uvc)

    lines = []
    lines.append(r"% Auto-generated setup comparison")
    lines.append(r"\begin{table}[t]")
    lines.append(r"\centering")
    lines.append(r"\caption{Setup cost (seconds). "
                 r"UVC setup grows with~$B$; Groth16 setup grows with step~$j$.}")
    lines.append(r"\label{tab:setup}")
    lines.append(r"\renewcommand{\arraystretch}{1.2}")
    lines.append(r"{\footnotesize")

    col_spec = "@{}ll" + "c" * len(b_values) + "cc@{}"
    lines.append(rf"\begin{{tabular}}{{{col_spec}}}")
    lines.append(r"\toprule")

    header_parts = [r"\textbf{Circuit}", "$K$"]
    for B in b_values:
        header_parts.append(rf"\textbf{{Ours}} ($B\!=\!{B}$)")
    header_parts.extend([r"\textbf{Groth16} ($j\!=\!1$)", r"\textbf{Nova}"])
    lines.append(" & ".join(header_parts) + r" \\")
    lines.append(r"\midrule")

    for circuit, n in configs:
        parts = [CIRCUIT_DISPLAY.get(circuit, circuit), str(n)]
        for B in b_values:
            uvc_r = uvc.get((circuit, n, B, 1), {})
            parts.append(fmt_sec(uvc_r.get("setup_ms")))
        g = g16.get((circuit, n, 1), {})
        nv = nova.get((circuit, n, 1), {})
        parts.append(fmt_sec(g.get("setup_ms")))
        parts.append(fmt_sec(nv.get("setup_ms")))
        lines.append(" & ".join(parts) + r" \\")

    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    lines.append(r"}% end footnotesize")
    lines.append(r"\end{table}")

    return "\n".join(lines)


def print_latex(csv_dir):
    """Generate all LaTeX output to stdout."""
    uvc, g16, nova = load_all(csv_dir)

    if not uvc:
        print("% No UVC data found.", file=sys.stderr)
        return

    print(gen_proving_latex(uvc, g16, nova))
    print()
    print()
    print(gen_verify_proof_latex(uvc, g16, nova))
    print()
    print()
    print(gen_setup_latex(uvc, g16, nova))


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
