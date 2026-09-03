#!/usr/bin/env python3
"""Validate that a benchmark run contains every coordinate in manifest.json."""

import csv
import json
import math
import os
import sys

UVC_HEADER = [
    "scheme", "circuit", "n", "B", "step", "setup_ms", "prove_ms", "verify_ms",
    "crs_g1_published", "crs_g2", "vk_st_abc_g1", "proof_bytes",
    "proof_bytes_compressed", "peak_mem_mb", "commit",
]
GROTH16_HEADER = [
    "circuit", "n", "step", "setup_ms", "prove_ms", "verify_ms", "crs_g1", "crs_g2", "proof_bytes",
]
GROTH16_MODES_HEADER = [
    "mode", "circuit", "n", "B", "step", "setup_ms", "prove_ms",
    "prove_ms_mean", "prove_ms_stddev", "prove_ms_min", "prove_ms_max",
    "verify_ms", "verify_ms_stddev", "crs_g1", "crs_g2", "proof_bytes",
    "num_constraints", "num_primary", "commit",
]
NOVA_HEADER = [
    "circuit", "n", "step", "run_id", "warmup", "setup_ms", "fold_ms",
    "total_fold_ms", "compress_ms", "verify_ms", "proof_bytes",
]

GROTH16_MODE_NAMES = ("ondemand", "fixedcrs", "singlestep")


def fail(message):
    raise ValueError(message)


def numeric(value, path, row_number, field):
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        fail("%s: row %d field %s must be numeric" % (path, row_number, field))
    if not math.isfinite(parsed):
        fail("%s: row %d field %s must be finite" % (path, row_number, field))


def integer(value, path, row_number, field):
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        fail("%s: row %d field %s must be an integer" % (path, row_number, field))
    if str(parsed) != value:
        fail("%s: row %d field %s must be an integer" % (path, row_number, field))
    return parsed


def read_rows(path, expected_header, scheme=None):
    if not os.path.isfile(path):
        fail("Missing required CSV: %s" % path)
    with open(path, newline="") as source:
        reader = csv.reader(source)
        try:
            header = next(reader)
        except StopIteration:
            fail("Empty CSV: %s" % path)
        if header != expected_header:
            if path.endswith("uvc_results.csv"):
                fail("Archived legacy UVC header rejected at %s; expected canonical UVC schema" % path)
            fail("Invalid header in %s; expected %s" % (path, ",".join(expected_header)))
        rows = []
        for row_number, row in enumerate(reader, 2):
            if len(row) != len(header):
                fail("%s: row %d has %d columns; expected %d" % (path, row_number, len(row), len(header)))
            record = dict(zip(header, row))
            if scheme is not None and record["scheme"] != scheme:
                fail("%s: row %d has scheme %r; expected %r" % (path, row_number, record["scheme"], scheme))
            rows.append((row_number, record))
    return rows


def validate_rows(rows, path, numeric_fields):
    for row_number, row in rows:
        for field in numeric_fields:
            numeric(row[field], path, row_number, field)
        for field in ("n", "step"):
            integer(row[field], path, row_number, field)


def require_unique(rows, path, key_fields):
    seen = set()
    for row_number, row in rows:
        key = tuple(row[field] for field in key_fields)
        if key in seen:
            fail("%s: duplicate key %s at row %d" % (path, key, row_number))
        seen.add(key)


def measured_steps(bound):
    steps = []
    step = 1
    while step <= bound:
        steps.append(step)
        step *= 2
    if steps[-1] != bound:
        steps.append(bound)
    return steps


def main(argv):
    if len(argv) != 2:
        print("Usage: %s <run-csv-dir>" % argv[0], file=sys.stderr)
        return 2
    csv_dir = os.path.abspath(argv[1])
    manifest_path = os.path.join(csv_dir, "manifest.json")
    if not os.path.isfile(manifest_path):
        fail("Missing coordinate manifest: %s" % manifest_path)
    with open(manifest_path) as source:
        manifest = json.load(source)
    if not isinstance(manifest, dict) or not isinstance(manifest.get("coordinates"), list):
        fail("Invalid coordinate manifest: %s" % manifest_path)

    uvc_path = os.path.join(csv_dir, "uvc_results.csv")
    uvc_rows = read_rows(uvc_path, UVC_HEADER, "uvc_gamma_v1")
    validate_rows(uvc_rows, uvc_path, UVC_HEADER[5:14])
    for row_number, row in uvc_rows:
        integer(row["B"], uvc_path, row_number, "B")
        for field in ("proof_bytes", "proof_bytes_compressed"):
            if integer(row[field], uvc_path, row_number, field) != 160:
                fail("%s: row %d field %s must equal 160" % (uvc_path, row_number, field))
    require_unique(uvc_rows, uvc_path, ("scheme", "circuit", "n", "B", "step"))
    uvc_index = {(row["circuit"], int(row["n"]), int(row["B"]), int(row["step"])) for _, row in uvc_rows}

    groth_path = os.path.join(csv_dir, "groth16_results.csv")
    groth_rows = read_rows(groth_path, GROTH16_HEADER)
    validate_rows(groth_rows, groth_path, GROTH16_HEADER[3:])
    groth_index = {(row["circuit"], int(row["n"]), int(row["step"])) for _, row in groth_rows}
    require_unique(groth_rows, groth_path, ("circuit", "n", "step"))

    schemes = manifest.get("schemes", [])
    if not isinstance(schemes, list):
        fail("Invalid schemes in coordinate manifest: %s" % manifest_path)

    modes_index = set()
    if "groth16_fixedcrs" in schemes:
        modes_path = os.path.join(csv_dir, "groth16_modes_results.csv")
        modes_rows = read_rows(modes_path, GROTH16_MODES_HEADER)
        validate_rows(modes_rows, modes_path, GROTH16_MODES_HEADER[5:18])
        for row_number, row in modes_rows:
            integer(row["B"], modes_path, row_number, "B")
        require_unique(modes_rows, modes_path, ("mode", "circuit", "n", "B", "step"))
        modes_index = {
            (row["mode"], row["circuit"], int(row["n"]), int(row["B"]), int(row["step"]))
            for _, row in modes_rows
        }

    nova_index = set()
    if "nova" in schemes:
        nova_path = os.path.join(csv_dir, "nova_results.csv")
        nova_rows = read_rows(nova_path, NOVA_HEADER)
        validate_rows(nova_rows, nova_path, NOVA_HEADER[5:])
        nova_index = {(row["circuit"], int(row["n"]), int(row["step"])) for _, row in nova_rows}
        require_unique(nova_rows, nova_path, ("circuit", "n", "step", "run_id"))

    errors = []
    expected_uvc = set()
    expected_groth = set()
    expected_modes = set()
    expected_nova = set()
    for coordinate in manifest["coordinates"]:
        try:
            circuit = coordinate["circuit"]
            n = int(coordinate["n"])
            bound = int(coordinate["B"])
            steps = coordinate["steps"]
        except (KeyError, TypeError, ValueError):
            fail("Invalid coordinate in %s: %r" % (manifest_path, coordinate))
        if not isinstance(circuit, str) or not isinstance(steps, list):
            fail("Invalid coordinate in %s: %r" % (manifest_path, coordinate))
        if steps != measured_steps(bound):
            fail("Invalid measured steps for %s n=%d B=%d in %s" % (circuit, n, bound, manifest_path))
        for step in steps:
            if not isinstance(step, int):
                fail("Non-integer manifest step for %s n=%d B=%d" % (circuit, n, bound))
            expected_uvc.add((circuit, n, bound, step))
            expected_groth.add((circuit, n, step))
            if "nova" in schemes:
                expected_nova.add((circuit, n, step))
            if (circuit, n, bound, step) not in uvc_index:
                errors.append("Missing UVC row for %s n=%d B=%d step=%d" % (circuit, n, bound, step))
            if (circuit, n, step) not in groth_index:
                errors.append("Missing Groth16 row for %s n=%d B=%d step=%d" % (circuit, n, bound, step))
            if "groth16_fixedcrs" in schemes:
                for mode in GROTH16_MODE_NAMES:
                    expected_modes.add((mode, circuit, n, bound, step))
                    if (mode, circuit, n, bound, step) not in modes_index:
                        errors.append("Missing Groth16 %s row for %s n=%d B=%d step=%d"
                                      % (mode, circuit, n, bound, step))
            if "nova" in schemes and (circuit, n, step) not in nova_index:
                errors.append("Missing Nova row for %s n=%d B=%d step=%d" % (circuit, n, bound, step))
    for key in sorted(uvc_index - expected_uvc):
        errors.append("Unexpected UVC row for %s n=%d B=%d step=%d" % key)
    for key in sorted(groth_index - expected_groth):
        errors.append("Unexpected Groth16 row for %s n=%d step=%d" % key)
    for key in sorted(modes_index - expected_modes):
        errors.append("Unexpected Groth16 %s row for %s n=%d B=%d step=%d" % key)
    for key in sorted(nova_index - expected_nova):
        errors.append("Unexpected Nova row for %s n=%d step=%d" % key)
    if errors:
        raise ValueError("Completeness check failed:\n" + "\n".join(errors))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print("verify_completeness.py: %s" % error, file=sys.stderr)
        sys.exit(1)
