#!/usr/bin/env python3
"""
cumulative_prove_time.py — Cumulative proving-time comparison.

Computes how total prover cost accumulates over B steps for UVC, Groth16,
and Nova.  Outputs ASCII summary + optional per-step CSV for plotting.

Cumulative definitions:
  UVC:     setup + Σ_{j=1}^{step} prove_j   (prove_j ≈ constant)
  Groth16: Σ_{j=1}^{step} (setup_j + prove_j)   (grows with j)
  Nova:    setup + total_fold(step) + compress

Usage:
    python3 scripts/cumulative_prove_time.py <csv_dir>
    python3 scripts/cumulative_prove_time.py <csv_dir> --out cumulative.csv
    python3 scripts/cumulative_prove_time.py <csv_dir> --latex
"""

import argparse
import csv
import os
import sys


# ── Display names & ordering ─────────────────────────────────────

CIRCUIT_DISPLAY = {
    "mimc": "MiMC hash chain",
    "hadamard": "Hadamard product",
    "matmul": "Hadamard product",
    "scalable": "Repeated squaring",
    "sensor_fusion": "Sensor fusion",
    "pagerank": "PageRank",
}

CIRCUIT_ORDER = {
    "mimc": 0, "hadamard": 1, "matmul": 1,
    "scalable": 2, "sensor_fusion": 3, "pagerank": 4,
}


# ── CSV loading ──────────────────────────────────────────────────

CANONICAL_UVC_HEADER = [
    "scheme", "circuit", "n", "B", "step", "setup_ms", "prove_ms",
    "verify_ms", "crs_g1_published", "crs_g2", "vk_st_abc_g1",
    "proof_bytes", "proof_bytes_compressed", "peak_mem_mb", "commit",
]


def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def read_canonical_uvc(path):
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
    return "hadamard" if name == "matmul" else name


def load_data(csv_dir):
    """Load and index benchmark CSVs."""
    uvc_rows = read_canonical_uvc(os.path.join(csv_dir, "uvc_results.csv"))
    g16_rows = read_csv(os.path.join(csv_dir, "groth16_results.csv"))
    nova_rows = read_csv(os.path.join(csv_dir, "nova_results.csv"))

    uvc = {}
    for r in uvc_rows:
        c = normalize_circuit(r["circuit"])
        key = (c, int(r["n"]), int(r["B"]), int(r["step"]))
        uvc[key] = r

    g16 = {}
    for r in g16_rows:
        c = normalize_circuit(r["circuit"])
        key = (c, int(r["n"]), int(r["step"]))
        if key not in g16:
            g16[key] = r

    nova = {}
    for r in nova_rows:
        c = normalize_circuit(r.get("circuit", ""))
        key = (c, int(r["n"]), int(r["step"]))
        nova[key] = r

    return uvc, g16, nova


def get_configs(uvc):
    """Unique (circuit, n, B) triples, paper-ordered."""
    configs = set()
    for (c, n, B, _) in uvc:
        configs.add((c, n, B))
    return sorted(configs, key=lambda x: (CIRCUIT_ORDER.get(x[0], 99), x[1], x[2]))


# ── Cumulative computation ───────────────────────────────────────

def compute_cumulative(uvc, g16, nova):
    """Return list of per-step cumulative dicts for every config."""
    configs = get_configs(uvc)
    rows = []

    for circuit, n, B in configs:
        # ── UVC ──
        uvc_steps = sorted(
            s for (c, nn, bb, s) in uvc if c == circuit and nn == n and bb == B
        )
        uvc_setup = float(uvc[(circuit, n, B, uvc_steps[0])]["setup_ms"])
        prove_vals = [float(uvc[(circuit, n, B, s)]["prove_ms"]) for s in uvc_steps]
        uvc_prove = sorted(prove_vals)[len(prove_vals) // 2]  # median

        # ── Groth16 ──
        g16_measured = sorted(s for (c, nn, s) in g16 if c == circuit and nn == n)
        g16_cost_at = {}
        for s in g16_measured:
            r = g16[(circuit, n, s)]
            g16_cost_at[s] = float(r["setup_ms"]) + float(r["prove_ms"])

        # ── Nova ──
        nova_measured = sorted(s for (c, nn, s) in nova if c == circuit and nn == n)
        nova_setup = float(nova[(circuit, n, nova_measured[0])]["setup_ms"]) if nova_measured else 0
        nova_compress = float(nova[(circuit, n, nova_measured[0])]["compress_ms"]) if nova_measured else 0

        # ── Display steps: powers of 2 up to B ──
        display = []
        s = 1
        while s <= B:
            display.append(s)
            s *= 2
        if B not in display:
            display.append(B)

        for step in display:
            # UVC cumulative
            uvc_cum = uvc_setup + step * uvc_prove

            # Groth16 cumulative via trapezoidal interpolation
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

            # Nova cumulative
            nv_r = nova.get((circuit, n, step))
            nova_cum = None
            if nv_r:
                nova_cum = (
                    float(nv_r["setup_ms"])
                    + float(nv_r["total_fold_ms"])
                    + float(nv_r["compress_ms"])
                )

            rows.append({
                "circuit": circuit,
                "n": n,
                "B": B,
                "step": step,
                "uvc_cum_ms": uvc_cum,
                "g16_cum_ms": g16_cum,
                "nova_cum_ms": nova_cum,
            })

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

def print_summary(rows):
    """Print per-config summary at max step (=B)."""
    # Group by (circuit, n, B), take last entry (step == B)
    summary = {}
    for r in rows:
        key = (r["circuit"], r["n"], r["B"])
        if r["step"] == r["B"]:
            summary[key] = r

    headers = ["Circuit", "n", "B",
               "UVC (s)", "Groth16 (s)", "Nova (s)",
               "UVC/G16", "UVC/Nova"]
    col_w = [max(len(h), 18) for h in headers]
    col_w[1] = 6
    col_w[2] = 5

    print()
    print("=" * 90)
    print("  Cumulative Proving Time at step B (seconds)")
    print("=" * 90)
    hdr = "  ".join(h.rjust(w) for h, w in zip(headers, col_w))
    sep = "  ".join("-" * w for w in col_w)
    print(hdr)
    print(sep)

    for key in sorted(summary, key=lambda k: (CIRCUIT_ORDER.get(k[0], 99), k[1], k[2])):
        r = summary[key]
        vals = [
            CIRCUIT_DISPLAY.get(r["circuit"], r["circuit"]),
            str(r["n"]),
            str(r["B"]),
            fmt_s(r["uvc_cum_ms"]),
            fmt_s(r["g16_cum_ms"]),
            fmt_s(r["nova_cum_ms"]),
            ratio_str(r["uvc_cum_ms"], r["g16_cum_ms"]),
            ratio_str(r["uvc_cum_ms"], r["nova_cum_ms"]),
        ]
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

    for circuit, n, B in configs:
        config_rows = [r for r in rows
                       if r["circuit"] == circuit and r["n"] == n and r["B"] == B]
        name = CIRCUIT_DISPLAY.get(circuit, circuit)

        print()
        print(f"── {name}  n={n}  B={B} " + "─" * 50)
        headers = ["Step", "UVC (s)", "Groth16 (s)", "Nova (s)", "UVC/G16", "UVC/Nova"]
        col_w = [6, 12, 12, 12, 9, 9]
        hdr = "  ".join(h.rjust(w) for h, w in zip(headers, col_w))
        sep = "  ".join("-" * w for w in col_w)
        print(hdr)
        print(sep)

        for r in config_rows:
            vals = [
                str(r["step"]),
                fmt_s(r["uvc_cum_ms"]),
                fmt_s(r["g16_cum_ms"]),
                fmt_s(r["nova_cum_ms"]),
                ratio_str(r["uvc_cum_ms"], r["g16_cum_ms"]),
                ratio_str(r["uvc_cum_ms"], r["nova_cum_ms"]),
            ]
            print("  ".join(v.rjust(w) for v, w in zip(vals, col_w)))
    print()


# ── CSV output ───────────────────────────────────────────────────

def write_csv_file(rows, path):
    """Write per-step cumulative data to CSV."""
    fieldnames = [
        "circuit", "n", "B", "step",
        "uvc_cum_s", "g16_cum_s", "nova_cum_s",
    ]
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for r in rows:
            writer.writerow({
                "circuit": r["circuit"],
                "n": r["n"],
                "B": r["B"],
                "step": r["step"],
                "uvc_cum_s": f"{r['uvc_cum_ms'] / 1000:.4f}",
                "g16_cum_s": f"{r['g16_cum_ms'] / 1000:.4f}",
                "nova_cum_s": f"{r['nova_cum_ms'] / 1000:.4f}" if r["nova_cum_ms"] else "",
            })
    print(f"  Saved: {path}")


# ── LaTeX output ─────────────────────────────────────────────────

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
        key = (r["circuit"], r["n"], r["B"])
        if r["step"] == r["B"]:
            summary[key] = r

    configs = sorted(summary, key=lambda k: (CIRCUIT_ORDER.get(k[0], 99), k[1], k[2]))

    # Group by (circuit, n) for multirow
    groups = []
    prev = None
    for key in configs:
        c, n, B = key
        if (c, n) != prev:
            groups.append([])
            prev = (c, n)
        groups[-1].append(key)

    lines = [
        r"% Auto-generated by cumulative_prove_time.py",
        r"\begin{table}[t]",
        r"\centering",
        r"\caption{Cumulative proving time (seconds) over $B$ steps.}",
        r"\label{tab:cumulative}",
        r"\renewcommand{\arraystretch}{1.2}",
        r"{\footnotesize",
        r"\begin{tabular}{@{}ccrrrrrr@{}}",
        r"\toprule",
        (r"\textbf{Circuit} & $K$ & $B$ & "
         r"\textbf{Ours} & \textbf{Groth16} & \textbf{Nova} & "
         r"\textbf{Ours/G16} & \textbf{Ours/Nova} \\"),
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

            uvc_s = r["uvc_cum_ms"] / 1000
            g16_s = r["g16_cum_ms"] / 1000
            nova_s = r["nova_cum_ms"] / 1000 if r["nova_cum_ms"] else None

            parts = [circ, ncol, str(B)]
            parts.append(f"{uvc_s:.1f}")
            parts.append(f"{g16_s:.1f}")
            parts.append(f"{nova_s:.1f}" if nova_s else "---")
            parts.append(f"{uvc_s/g16_s:.2f}" if g16_s else "---")
            parts.append(f"{uvc_s/nova_s:.2f}" if nova_s else "---")
            lines.append(" & ".join(parts) + r" \\")

        if gi < len(groups) - 1:
            lines.append(r"\midrule")

    lines += [
        r"\bottomrule",
        r"\end{tabular}",
        r"}% end footnotesize",
        r"\end{table}",
    ]
    print("\n".join(lines))


# ── Main ─────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Compute cumulative proving time across steps.")
    parser.add_argument("csv_dir",
        help="Directory containing {uvc,groth16,nova}_results.csv")
    parser.add_argument("--out",
        help="Write per-step cumulative CSV to this path")
    parser.add_argument("--latex", action="store_true",
        help="Print LaTeX table to stdout")
    parser.add_argument("--per-step", action="store_true",
        help="Show per-step cumulative breakdown")
    args = parser.parse_args()

    uvc, g16, nova = load_data(args.csv_dir)
    if not uvc:
        print("No UVC data found.", file=sys.stderr)
        sys.exit(1)

    rows = compute_cumulative(uvc, g16, nova)

    print()
    print(f"  Data: {args.csv_dir}")
    print(f"  Configs: {len(get_configs(uvc))}")

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
