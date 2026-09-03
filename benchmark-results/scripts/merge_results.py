#!/usr/bin/env python3
"""
Merge UVC, Groth16, and Nova benchmark results into a combined CSV.

Usage:
    python merge_results.py --input-dir ../csv --output combined_results.csv
"""

import argparse
import csv
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "scripts"))

import bench_data


CANONICAL_UVC_HEADER = [
    "scheme", "circuit", "n", "B", "step", "setup_ms", "prove_ms",
    "verify_ms", "crs_g1_published", "crs_g2", "vk_st_abc_g1",
    "proof_bytes", "proof_bytes_compressed", "peak_mem_mb", "commit",
]


def read_csv(filepath):
    """Read a CSV file and return list of dicts."""
    if not os.path.exists(filepath):
        print(f"  Warning: {filepath} not found, skipping.")
        return []
    with open(filepath, 'r') as f:
        reader = csv.DictReader(f)
        return list(reader)


def read_canonical_uvc(filepath):
    try:
        with open(filepath, newline='') as f:
            reader = csv.DictReader(f)
            if reader.fieldnames != CANONICAL_UVC_HEADER:
                raise ValueError
            rows = list(reader)
    except (OSError, ValueError):
        sys.exit(f"FATAL: {filepath}: not a canonical uvc_gamma_v1 results file (expected canonical header and scheme)")
    if any(row.get("scheme") != "uvc_gamma_v1" for row in rows):
        sys.exit(f"FATAL: {filepath}: not a canonical uvc_gamma_v1 results file (expected canonical header and scheme)")
    return rows


def dedup_groth16(rows):
    """Deduplicate Groth16 rows: keep first occurrence per (circuit, n, step)."""
    seen = {}
    for r in rows:
        key = (r['circuit'], r['n'], r['step'])
        if key not in seen:
            seen[key] = r
    return list(seen.values())


NOVA_NUMERIC = ('setup_ms', 'fold_ms', 'total_fold_ms', 'compress_ms',
                'verify_ms', 'fold_ms_iqr', 'compress_ms_iqr')


def nova_cell(row, field):
    """Format one aggregated Nova field for the combined CSV."""
    value = row.get(field, '')
    if value == '':
        return ''
    if field in NOVA_NUMERIC:
        return f'{float(value):.2f}'
    if field in ('proof_bytes', 'n_runs'):
        return str(int(float(value)))
    return str(value)


def merge_results(input_dir, output_path):
    """Merge results from all three systems."""
    uvc_rows = read_canonical_uvc(os.path.join(input_dir, 'uvc_results.csv'))
    g16_rows = read_csv(os.path.join(input_dir, 'groth16_results.csv'))
    # Nova: multiple run_ids per step collapse to one aggregated (median) row.
    nova_rows = list(bench_data.load_run(input_dir)["nova"].values())

    # Deduplicate Groth16 (same circuit run under different B values produces dupes)
    g16_rows = dedup_groth16(g16_rows)

    uvc_by_key = {}
    for r in uvc_rows:
        key = (r['scheme'], r['circuit'], r['n'], r['B'], r['step'])
        uvc_by_key[key] = r

    # Groth16 keyed by (circuit, n, step)
    g16_by_key = {}
    for r in g16_rows:
        key = (r['circuit'], r['n'], r['step'])
        g16_by_key[key] = r

    # Nova keyed by (circuit, n, step) — keep last (largest run) for each key
    nova_by_key = {}
    for r in nova_rows:
        key = (r.get('circuit', ''), r.get('n', ''), r.get('step', ''))
        nova_by_key[key] = r

    # Keep each source measurement as a scheme-labelled row.  UVC rows retain
    # the comparison measurements historically included in this file; native
    # Groth16 and Nova rows make the scheme column unambiguous for consumers.
    all_rows = []
    for key in sorted(
            uvc_by_key, key=lambda k: (k[1], int(k[2]), int(k[3]), int(k[4]), k[0])):
        scheme, circuit, n, B, step = key
        uvc = uvc_by_key[key]
        g16 = g16_by_key.get((circuit, n, step), {})
        nova = nova_by_key.get((circuit, n, step), {})
        all_rows.append([
            scheme, circuit, n, B, step,
            uvc.get('setup_ms', ''), uvc.get('prove_ms', ''),
            uvc.get('verify_ms', ''),
            uvc.get('crs_g1', uvc.get('crs_g1_published', '')),
            uvc.get('crs_g2', ''), uvc.get('proof_bytes', ''),
            uvc.get('peak_mem_mb', ''),
            g16.get('setup_ms', ''), g16.get('prove_ms', ''),
            g16.get('verify_ms', ''), g16.get('crs_g1', ''),
            g16.get('crs_g2', ''), g16.get('proof_bytes', ''),
            nova_cell(nova, 'setup_ms'), nova_cell(nova, 'fold_ms'),
            nova_cell(nova, 'total_fold_ms'), nova_cell(nova, 'compress_ms'),
            nova_cell(nova, 'verify_ms'), nova_cell(nova, 'proof_bytes'),
            nova_cell(nova, 'fold_ms_iqr'), nova_cell(nova, 'compress_ms_iqr'),
            nova_cell(nova, 'n_runs'),
        ])

    for r in g16_rows:
        all_rows.append([
            'groth16', r['circuit'], r['n'], '', r['step'],
            '', '', '', '', '', '', '',
            r.get('setup_ms', ''), r.get('prove_ms', ''),
            r.get('verify_ms', ''), r.get('crs_g1', ''),
            r.get('crs_g2', ''), r.get('proof_bytes', ''),
            '', '', '', '', '', '', '', '', '',
        ])
    for r in nova_rows:
        all_rows.append([
            'nova', r.get('circuit', ''), r.get('n', ''), '', r.get('step', ''),
            '', '', '', '', '', '', '', '', '', '', '', '', '',
            nova_cell(r, 'setup_ms'), nova_cell(r, 'fold_ms'),
            nova_cell(r, 'total_fold_ms'), nova_cell(r, 'compress_ms'),
            nova_cell(r, 'verify_ms'), nova_cell(r, 'proof_bytes'),
            nova_cell(r, 'fold_ms_iqr'), nova_cell(r, 'compress_ms_iqr'),
            nova_cell(r, 'n_runs'),
        ])

    with open(output_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow([
            'scheme', 'circuit', 'n', 'B', 'step',
            'uvc_setup_ms', 'uvc_prove_ms', 'uvc_verify_ms',
            'uvc_crs_g1', 'uvc_crs_g2', 'uvc_proof_bytes', 'uvc_peak_mem_mb',
            'g16_setup_ms', 'g16_prove_ms', 'g16_verify_ms',
            'g16_crs_g1', 'g16_crs_g2', 'g16_proof_bytes',
            'nova_setup_ms', 'nova_fold_ms', 'nova_total_fold_ms',
            'nova_compress_ms', 'nova_verify_ms', 'nova_proof_bytes',
            'nova_fold_ms_iqr', 'nova_compress_ms_iqr', 'nova_n_runs',
        ])
        writer.writerows(all_rows)

    print(f"Combined results written to {output_path}")
    print(f"  UVC rows: {len(uvc_rows)}, Groth16 rows: {len(g16_rows)}, Nova rows: {len(nova_rows)}")
    print(f"  Total combined rows: {len(all_rows)}")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Merge benchmark results')
    parser.add_argument('--input-dir', required=True, help='Directory with all CSV files')
    parser.add_argument('--output', default='combined_results.csv', help='Output file path')
    args = parser.parse_args()

    merge_results(args.input_dir, args.output)
