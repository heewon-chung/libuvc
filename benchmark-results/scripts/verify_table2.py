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
import math


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


def canonical_integer(value):
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return None
    return parsed if str(parsed) == value else None


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
    data = {}
    duplicate_keys = []
    for row_number, row in enumerate(rows, 2):
        key_values = [canonical_integer(row[field]) for field in ("n", "B", "step")]
        if any(value is None for value in key_values):
            print(f"FAIL: row {row_number} has noncanonical n/B/step coordinates")
            return 1
        key = (row["circuit"], key_values[0], key_values[1], key_values[2])
        if key in data:
            duplicate_keys.append(key)
        data[key] = row
    if duplicate_keys:
        print(f"FAIL: duplicate UVC coordinates: {sorted(set(duplicate_keys))}")
        return 1

    expected_keys = {
        (circuit, n, B, step)
        for circuit, n, B in EXPECTED_CONFIGS
        for step in get_expected_steps(B)
    }
    unexpected_keys = sorted(set(data) - expected_keys)
    if unexpected_keys:
        print(f"FAIL: unexpected UVC coordinates: {unexpected_keys}")
        return 1

    commits = {row.get("commit", "") for row in rows}
    if len(commits) != 1 or "" in commits:
        print(f"FAIL: UVC rows must share one non-empty commit, got {sorted(commits)}")
        return 1
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
                if not math.isfinite(value) or value <= 0:
                    issues.append(f"step={step} {field}={row.get(field, '')}")
            for field in ("proof_bytes", "proof_bytes_compressed"):
                proof_bytes = canonical_integer(row.get(field))
                if proof_bytes != 160:
                    issues.append(f"step={step} {field}={row.get(field, '')} (expected canonical 160)")
            try:
                verify_ms = float(row["verify_ms"])
            except (KeyError, TypeError, ValueError):
                verify_ms = 0
            # Plausibility band: 3 pairings (~4-5 ms on M1 Max) plus the
            # O(|s_j|) state-commitment increment MSM. Multi-element states
            # (e.g. Hadamard d=32) legitimately reach ~10-12 ms once the
            # state entries are full-width field elements, so allow the MSM
            # headroom while still rejecting anything Nova-scale (>= 36 ms).
            try:
                state_elems = int(row.get("vk_st_abc_g1", 0)) // max(int(row["B"]), 1)
            except (KeyError, TypeError, ValueError, ZeroDivisionError):
                state_elems = 1
            verify_bound_ms = 10 + 0.25 * max(state_elems, 1)
            if verify_ms > verify_bound_ms:
                issues.append(
                    f"step={step} verify_ms={verify_ms} "
                    f"(expected 0<v<={verify_bound_ms:.1f})")

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
