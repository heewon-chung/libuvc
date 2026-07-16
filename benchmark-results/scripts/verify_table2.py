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


# Expected configurations: (circuit, n, B) with expected measured steps
EXPECTED_CONFIGS = [
    # MiMC hash chain
    ("mimc", 270, 16),
    ("mimc", 270, 64),
    ("mimc", 1080, 16),
    ("mimc", 1080, 64),
    ("mimc", 4320, 16),
    ("mimc", 4320, 64),
    # Hadamard product
    ("hadamard", 32, 16),
    ("hadamard", 32, 64),
    ("hadamard", 128, 16),
    ("hadamard", 128, 64),
    ("hadamard", 512, 16),
    ("hadamard", 512, 64),
    # Repeated squaring
    ("scalable", 1024, 16),
    ("scalable", 1024, 64),
    ("scalable", 8192, 16),
    ("scalable", 8192, 64),
    ("scalable", 65536, 16),
    ("scalable", 65536, 64),
]


def get_expected_steps(B):
    """Powers of 2 from 1 up to B, plus B itself."""
    steps = []
    s = 1
    while s <= B:
        steps.append(s)
        s *= 2
    if steps[-1] != B:
        steps.append(B)
    return steps


def verify(input_path):
    with open(input_path, 'r') as f:
        rows = list(csv.DictReader(f))

    # Index rows by scheme and benchmark coordinate.  Legacy UVC CSVs do not
    # carry a scheme column, so retain their historical meaning explicitly.
    data = {}
    schemes = set()
    for r in rows:
        scheme = r.get('scheme') or 'uvc_prefix'
        if not r.get('B'):
            continue
        key = (scheme, r['circuit'], int(r['n']), int(r['B']), int(r['step']))
        data[key] = r
        schemes.add(scheme)

    total_checks = 0
    total_pass = 0
    total_fail = 0
    failures = []

    for circuit, n, B in EXPECTED_CONFIGS:
        expected_steps = get_expected_steps(B)
        config_label = f"{circuit} n={n} B={B}"

        for scheme in sorted(schemes):
            scheme_prefix = "" if schemes == {'uvc_prefix'} else f"{scheme} "
            if not any((scheme, circuit, n, B, step) in data for step in expected_steps):
                continue

            # Check all expected steps exist for every UVC scheme independently.
            missing_steps = []
            for step in expected_steps:
                key = (scheme, circuit, n, B, step)
                if key not in data:
                    missing_steps.append(step)

            total_checks += 1
            if missing_steps:
                total_fail += 1
                msg = f"  FAIL  {scheme_prefix}{config_label}: missing steps {missing_steps}"
                failures.append(msg)
                print(msg)
                continue

            config_ok = True
            for step in expected_steps:
                r = data[(scheme, circuit, n, B, step)]
                issues = []

                if scheme == 'groth16':
                    g16_prove = float(r.get('g16_prove_ms', r.get('prove_ms', 0)))
                    g16_proof = int(r.get('g16_proof_bytes', r.get('proof_bytes', 0)))
                    if g16_prove <= 0:
                        issues.append(f"g16_prove_ms={g16_prove}")
                    if g16_proof != 128:
                        issues.append(f"g16_proof_bytes={g16_proof} (expected 128)")
                else:
                    uvc_setup = float(r.get('uvc_setup_ms', r.get('setup_ms', 0)))
                    uvc_prove = float(r.get('uvc_prove_ms', r.get('prove_ms', 0)))
                    uvc_verify = float(r.get('uvc_verify_ms', r.get('verify_ms', 0)))
                    uvc_proof = int(r.get('uvc_proof_bytes', r.get('proof_bytes', 0)))
                    expected_uvc_proof = 160 if scheme == 'uvc_bound' else 128
                    if uvc_setup <= 0:
                        issues.append(f"uvc_setup_ms={uvc_setup}")
                    if uvc_prove <= 0:
                        issues.append(f"uvc_prove_ms={uvc_prove}")
                    if uvc_verify <= 0 or uvc_verify > 10:
                        issues.append(f"uvc_verify_ms={uvc_verify} (expected 0<v<=10)")
                    if uvc_proof != expected_uvc_proof:
                        issues.append(
                            f"uvc_proof_bytes={uvc_proof} (expected {expected_uvc_proof})")

                if issues:
                    config_ok = False
                    msg = f"  FAIL  {scheme_prefix}{config_label} step={step}: {', '.join(issues)}"
                    failures.append(msg)
                    print(msg)

            if config_ok:
                total_pass += 1
                print(f"  PASS  {scheme_prefix}{config_label}: {len(expected_steps)} steps OK")
            else:
                total_fail += 1

    # Summary
    print()
    print("=" * 60)
    print(f"Verification Summary: {total_pass} PASS, {total_fail} FAIL out of {total_checks} configs")
    print(f"Total data rows: {len(rows)}")
    print("=" * 60)

    if failures:
        print("\nFailures:")
        for f in failures:
            print(f)
        return 1

    print("\nAll configurations verified successfully.")
    return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Verify Table 2 data completeness')
    parser.add_argument('--input', required=True, help='Path to combined_results.csv')
    args = parser.parse_args()
    sys.exit(verify(args.input))
