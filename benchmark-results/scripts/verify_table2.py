#!/usr/bin/env python3
"""
Verify that combined_results.csv contains all expected benchmark configurations
matching the paper's Table 2 data.

Usage:
    python verify_table2.py --input ../csv/combined_results.csv
"""

import argparse
import csv
import sys


CANONICAL_UVC_HEADER = [
    "scheme", "circuit", "n", "B", "step", "setup_ms", "prove_ms",
    "verify_ms", "crs_g1_published", "crs_g2", "vk_st_abc_g1",
    "proof_bytes", "proof_bytes_compressed", "peak_mem_mb", "commit",
]

# Must match scripts/run_benchmarks.sh.
EXPECTED_CONFIGS = [
    ("mimc", 270, 16),
    ("mimc", 270, 64),
    ("hadamard", 16, 16),
    ("hadamard", 16, 64),
    ("hadamard", 32, 16),
    ("hadamard", 32, 64),
    ("scalable", 1024, 16),
    ("scalable", 1024, 64),
    ("scalable", 16384, 16),
    ("scalable", 16384, 64),
]


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


def get_expected_steps(B):
    """Powers of 2 from 1 up to B."""
    steps = []
    step = 1
    while step <= B:
        steps.append(step)
        step *= 2
    return steps


def verify(input_path):
    rows = read_canonical_uvc(input_path)
    data = {
        (row["circuit"], int(row["n"]), int(row["B"]), int(row["step"])): row
        for row in rows
    }
    failures = []
    passed = 0

    for circuit, n, B in EXPECTED_CONFIGS:
        config_label = f"{circuit} n={n} B={B}"
        expected_steps = get_expected_steps(B)
        missing_steps = [
            step for step in expected_steps
            if (circuit, n, B, step) not in data
        ]
        if missing_steps:
            failures.append(
                f"  FAIL  {config_label}: missing steps {missing_steps}")
            continue

        issues = []
        for step in expected_steps:
            row = data[(circuit, n, B, step)]
            for field in ("setup_ms", "prove_ms", "verify_ms", "proof_bytes"):
                try:
                    value = float(row[field])
                except (KeyError, TypeError, ValueError):
                    value = 0
                if value <= 0:
                    issues.append(f"step={step} {field}={row.get(field, '')}")
            try:
                verify_ms = float(row["verify_ms"])
            except (KeyError, TypeError, ValueError):
                verify_ms = 0
            if verify_ms > 10:
                issues.append(
                    f"step={step} verify_ms={verify_ms} (expected 0<v<=10)")

        if issues:
            failures.append(f"  FAIL  {config_label}: {', '.join(issues)}")
        else:
            passed += 1
            print(f"  PASS  {config_label}: {len(expected_steps)} steps OK")

    for failure in failures:
        print(failure)

    print()
    print("=" * 60)
    print(f"Verification Summary: {passed} PASS, {len(failures)} FAIL out of {len(EXPECTED_CONFIGS)} configs")
    print(f"Total data rows: {len(rows)}")
    print("=" * 60)

    if failures:
        return 1
    print("\nAll configurations verified successfully.")
    return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Verify Table 2 data completeness')
    parser.add_argument('--input', required=True, help='Path to uvc_results.csv')
    args = parser.parse_args()
    sys.exit(verify(args.input))
