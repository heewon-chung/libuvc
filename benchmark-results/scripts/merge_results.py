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


def read_csv(filepath):
    """Read a CSV file and return list of dicts."""
    if not os.path.exists(filepath):
        print(f"  Warning: {filepath} not found, skipping.")
        return []
    with open(filepath, 'r') as f:
        reader = csv.DictReader(f)
        return list(reader)


def dedup_groth16(rows):
    """Deduplicate Groth16 rows: keep first occurrence per (circuit, n, step)."""
    seen = {}
    for r in rows:
        key = (r['circuit'], r['n'], r['step'])
        if key not in seen:
            seen[key] = r
    return list(seen.values())


def merge_results(input_dir, output_path):
    """Merge results from all three systems."""
    uvc_rows = read_csv(os.path.join(input_dir, 'uvc_results.csv'))
    g16_rows = read_csv(os.path.join(input_dir, 'groth16_results.csv'))
    nova_rows = read_csv(os.path.join(input_dir, 'nova_results.csv'))
    uvc_bound_rows = read_csv(os.path.join(input_dir, 'uvc_bound_results.csv'))

    # Deduplicate Groth16 (same circuit run under different B values produces dupes)
    g16_rows = dedup_groth16(g16_rows)

    # Preserve legacy and state-bound rows independently even when their
    # benchmark coordinates are identical.
    uvc_by_key = {}
    for scheme, rows in (('uvc_prefix', uvc_rows), ('uvc_bound', uvc_bound_rows)):
        for r in rows:
            key = (scheme, r['circuit'], r['n'], r['B'], r['step'])
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
            nova.get('setup_ms', ''), nova.get('fold_ms', ''),
            nova.get('total_fold_ms', ''), nova.get('compress_ms', ''),
            nova.get('verify_ms', ''), nova.get('proof_bytes', ''),
        ])

    for r in g16_rows:
        all_rows.append([
            'groth16', r['circuit'], r['n'], '', r['step'],
            '', '', '', '', '', '', '',
            r.get('setup_ms', ''), r.get('prove_ms', ''),
            r.get('verify_ms', ''), r.get('crs_g1', ''),
            r.get('crs_g2', ''), r.get('proof_bytes', ''),
            '', '', '', '', '', '',
        ])
    for r in nova_rows:
        all_rows.append([
            'nova', r.get('circuit', ''), r.get('n', ''), '', r.get('step', ''),
            '', '', '', '', '', '', '', '', '', '', '', '', '',
            r.get('setup_ms', ''), r.get('fold_ms', ''),
            r.get('total_fold_ms', ''), r.get('compress_ms', ''),
            r.get('verify_ms', ''), r.get('proof_bytes', ''),
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
        ])
        writer.writerows(all_rows)

    print(f"Combined results written to {output_path}")
    print(f"  UVC pre-fix rows: {len(uvc_rows)}, UVC state-bound rows: {len(uvc_bound_rows)}, Groth16 rows: {len(g16_rows)}, Nova rows: {len(nova_rows)}")
    print(f"  Total combined rows: {len(all_rows)}")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Merge benchmark results')
    parser.add_argument('--input-dir', required=True, help='Directory with all CSV files')
    parser.add_argument('--output', default='combined_results.csv', help='Output file path')
    args = parser.parse_args()

    merge_results(args.input_dir, args.output)
