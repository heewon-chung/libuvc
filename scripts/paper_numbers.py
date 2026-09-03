#!/usr/bin/env python3
"""
paper_numbers.py — Print the exact numbers the paper text quotes.

Usage:
    python3 scripts/paper_numbers.py <csv_dir>

Also writes the same text to <csv_dir>/../tables/paper_numbers.txt.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bench_data
from bench_data import CIRCUIT_DISPLAY, GROTH16_MODES


def read_thread_counts(system_info_path):
    """Pull the OpenMP and rayon thread counts out of system_info.txt."""
    openmp, rayon = None, None
    if os.path.isfile(system_info_path):
        with open(system_info_path) as f:
            for line in f:
                line = line.strip()
                if line.startswith("OpenMP max threads:"):
                    openmp = line.split(":", 1)[1].strip()
                elif line.startswith("rayon threads:"):
                    rayon = line.split(":", 1)[1].strip()
    return openmp, rayon


def build_report(csv_dir):
    data = bench_data.load_run(csv_dir)
    out = []

    # 0. Estimator asymmetry, stated first (spec §6.1 [review N2]).
    out.append(bench_data.ESTIMATOR_NOTE)
    out.append("")
    out.append(f"Data: {csv_dir}")
    out.append("")

    configs = bench_data.uvc_configs(data)

    # 1. Cumulative UVC vs each Groth16 mode at step = B.
    out.append("── 1. Cumulative cost at step = B (seconds) ─────────────────────")
    for circuit, n, B in configs:
        name = CIRCUIT_DISPLAY.get(circuit, circuit)
        uvc_cum = bench_data.cumulative_uvc(data, circuit, n, B, B)
        out.append(f"{name}  n={n}  B={B}:  UVC = {uvc_cum / 1000.0:.2f} s")
        for mode in GROTH16_MODES:
            full = bench_data.cumulative_groth16(data, mode, circuit, n, B, B)
            prove = bench_data.cumulative_groth16(
                data, mode, circuit, n, B, B, proving_only=True)
            if full is None:
                out.append(f"    G16 {mode:<11}: no rows")
                continue
            r_full = uvc_cum / full if full else float("nan")
            r_prove = uvc_cum / prove if prove else float("nan")
            out.append(
                f"    G16 {mode:<11}: with setup = {full / 1000.0:.2f} s "
                f"(UVC/G16 = {r_full:.2f}), "
                f"without setup = {prove / 1000.0:.2f} s "
                f"(UVC/G16 = {r_prove:.2f})")
    out.append("")

    # 2. Nova compress_ms (median across runs).
    out.append("── 2. Nova compress_ms (median across runs) ─────────────────────")
    compress_all = []
    for circuit, n, B in configs:
        vals = [float(row["compress_ms"])
                for (c, nn, _s), row in data["nova"].items()
                if c == circuit and nn == n]
        if not vals:
            out.append(f"{CIRCUIT_DISPLAY.get(circuit, circuit)}  n={n}: no Nova rows")
            continue
        compress_all.extend(vals)
        out.append(f"{CIRCUIT_DISPLAY.get(circuit, circuit)}  n={n}  B={B}: "
                   f"{min(vals):.2f}–{max(vals):.2f} ms")
    if compress_all:
        out.append(f"ALL CONFIGS: {min(compress_all):.2f}–{max(compress_all):.2f} ms")
    out.append("")

    # 3. Nova proof_bytes.
    out.append("── 3. Nova proof_bytes (median across runs) ─────────────────────")
    bytes_all = [float(row["proof_bytes"]) for row in data["nova"].values()]
    if bytes_all:
        out.append(f"ALL CONFIGS: {min(bytes_all):.0f}–{max(bytes_all):.0f} bytes")
    else:
        out.append("no Nova rows")
    out.append("")

    # 4. UVC prove_ms / G16 on-demand prove_ms at step = B.
    out.append("── 4. UVC prove_ms / G16 on-demand prove_ms at step = B ─────────")
    for circuit, n, B in configs:
        uvc_row = data["uvc"].get((circuit, n, B, B))
        g16 = data["g16_modes"].get(("ondemand", circuit, n, B, B))
        if g16 is None:
            g16 = data["g16_legacy"].get((circuit, n, B))
        name = CIRCUIT_DISPLAY.get(circuit, circuit)
        if uvc_row is None or g16 is None:
            out.append(f"{name}  n={n}  B={B}: missing rows")
            continue
        uvc_p = float(uvc_row["prove_ms"])
        g16_p = float(g16["prove_ms"])
        out.append(f"{name}  n={n}  B={B}: "
                   f"UVC {uvc_p:.2f} ms / G16 {g16_p:.2f} ms = {uvc_p / g16_p:.2f}x")
    out.append("")

    # 5. Thread counts.
    out.append("── 5. Thread counts ────────────────────────────────────────────")
    sys_info = os.path.join(os.path.dirname(os.path.abspath(csv_dir)), "system_info.txt")
    openmp, rayon = read_thread_counts(sys_info)
    out.append(f"OpenMP max threads: {openmp if openmp is not None else 'not recorded'}")
    out.append(f"rayon threads: {rayon if rayon is not None else 'not recorded'}")

    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(
        description="Print the benchmark numbers quoted in the paper.")
    parser.add_argument("csv_dir", help="Directory containing the run's CSV files")
    args = parser.parse_args()

    report = build_report(args.csv_dir)
    sys.stdout.write(report)

    tables_dir = os.path.join(os.path.dirname(os.path.abspath(args.csv_dir)), "tables")
    os.makedirs(tables_dir, exist_ok=True)
    out_path = os.path.join(tables_dir, "paper_numbers.txt")
    with open(out_path, "w") as f:
        f.write(report)
    print(f"\n  Saved: {out_path}")


if __name__ == "__main__":
    main()
