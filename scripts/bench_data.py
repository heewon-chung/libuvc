#!/usr/bin/env python3
"""
bench_data.py — Shared benchmark-CSV loader and cumulative-cost model.

One place that reads a run's CSV directory and hands every consumer
(cumulative_prove_time.py, summarize_results.py, gen_latex_tables.py,
merge_results.py, paper_numbers.py) the same indexed data.

Estimator asymmetry (deliberate, see spec §6.1 [review N2]):
  UVC cumulative     = setup + step * global median prove over measured steps
  Groth16 cumulative = causal trapezoid over measured steps <= step
"""

import csv
import os
import statistics
import sys


GROTH16_MODES = ("ondemand", "fixedcrs", "singlestep")
NOVA_KINDS = ("foldonly", "compressed")

ESTIMATOR_NOTE = (
    "UVC cumulative = setup + step × global median prove; "
    "Groth16 cumulative = causal trapezoid over measured steps"
)


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


CANONICAL_UVC_HEADER = [
    "scheme", "circuit", "n", "B", "step", "setup_ms", "prove_ms",
    "verify_ms", "crs_g1_published", "crs_g2", "vk_st_abc_g1",
    "proof_bytes", "proof_bytes_compressed", "peak_mem_mb", "commit",
]

NOVA_MEDIAN_FIELDS = (
    "setup_ms", "fold_ms", "total_fold_ms", "compress_ms",
    "verify_ms", "proof_bytes",
)


# ── Raw CSV reading ──────────────────────────────────────────────

def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def read_canonical_uvc(path):
    try:
        with open(path, newline="") as f:
            reader = csv.DictReader(f)
            if reader.fieldnames != CANONICAL_UVC_HEADER:
                raise ValueError
            rows = list(reader)
    except (OSError, ValueError):
        sys.exit(f"FATAL: {path}: not a canonical uvc_gamma_v1 results file "
                 f"(expected canonical header and scheme)")
    if any(row.get("scheme") != "uvc_gamma_v1" for row in rows):
        sys.exit(f"FATAL: {path}: not a canonical uvc_gamma_v1 results file "
                 f"(expected canonical header and scheme)")
    return rows


def normalize_circuit(name):
    return "hadamard" if name == "matmul" else name


def _iqr(values):
    if len(values) < 2:
        return 0.0
    q = statistics.quantiles(values, n=4)
    return q[2] - q[0]


def load_run(csv_dir):
    """Load and index every CSV of one benchmark run."""
    uvc_rows = read_canonical_uvc(os.path.join(csv_dir, "uvc_results.csv"))
    g16_rows = read_csv(os.path.join(csv_dir, "groth16_results.csv"))
    modes_rows = read_csv(os.path.join(csv_dir, "groth16_modes_results.csv"))
    nova_rows = read_csv(os.path.join(csv_dir, "nova_results.csv"))

    uvc = {}
    for r in uvc_rows:
        c = normalize_circuit(r["circuit"])
        r = dict(r, circuit=c)
        uvc[(c, int(r["n"]), int(r["B"]), int(r["step"]))] = r

    g16_legacy = {}
    for r in g16_rows:
        c = normalize_circuit(r["circuit"])
        r = dict(r, circuit=c)
        key = (c, int(r["n"]), int(r["step"]))
        if key not in g16_legacy:
            g16_legacy[key] = r

    g16_modes = {}
    for r in modes_rows:
        c = normalize_circuit(r["circuit"])
        r = dict(r, circuit=c)
        g16_modes[(r["mode"], c, int(r["n"]), int(r["B"]), int(r["step"]))] = r

    nova_runs = {}
    for r in nova_rows:
        c = normalize_circuit(r.get("circuit", ""))
        r = dict(r, circuit=c)
        nova_runs.setdefault((c, int(r["n"]), int(r["step"])), []).append(r)

    nova = {}
    for key, runs in nova_runs.items():
        agg = dict(runs[0])
        for field in NOVA_MEDIAN_FIELDS:
            vals = [float(r[field]) for r in runs if r.get(field) not in (None, "")]
            agg[field] = statistics.median(vals) if vals else 0.0
        agg["n_runs"] = len(runs)
        for field in ("fold_ms", "total_fold_ms", "compress_ms"):
            vals = [float(r[field]) for r in runs if r.get(field) not in (None, "")]
            agg[f"{field}_iqr"] = _iqr(vals)
        nova[key] = agg

    return {
        "uvc": uvc,
        "g16_legacy": g16_legacy,
        "g16_modes": g16_modes,
        "nova_runs": nova_runs,
        "nova": nova,
    }


# ── Step model ───────────────────────────────────────────────────

def measured_steps(B):
    """Powers of two up to B, plus B itself."""
    steps = []
    step = 1
    while step <= B:
        steps.append(step)
        step *= 2
    if steps[-1] != B:
        steps.append(B)
    return steps


def trapezoid_cum(cost_at, step):
    """Causal trapezoid integration of cost_at over measured points <= step.

    Generalized from cumulative_prove_time.py:154-163: cost_at replaces
    g16_cost_at and sorted(cost_at) replaces g16_measured; the arithmetic
    (trapezoid rule with the prev_j/prev_cost accumulator, then the flat
    extension past the last measured point) is unchanged.
    """
    total = 0.0
    prev_j, prev_cost = 0, 0.0
    for j in sorted(cost_at.keys()):
        if j > step:
            break
        cost = cost_at[j]
        total += (j - prev_j) * (cost + prev_cost) / 2.0
        prev_j, prev_cost = j, cost
    if prev_j < step:
        total += (step - prev_j) * prev_cost
    return total


# ── Config helpers ───────────────────────────────────────────────

def uvc_configs(data):
    """Unique (circuit, n, B) triples in paper order."""
    configs = {(c, n, B) for (c, n, B, _s) in data["uvc"]}
    return sorted(configs, key=lambda k: (CIRCUIT_ORDER.get(k[0], 99), k[1], k[2]))


def uvc_steps(data, circuit, n, B):
    return sorted(s for (c, nn, bb, s) in data["uvc"]
                  if c == circuit and nn == n and bb == B)


def _mode_rows(data, mode, circuit, n, B):
    """Measured rows of one Groth16 mode, keyed by step; legacy fallback."""
    rows = {s: data["g16_modes"][(mode, circuit, n, B, s)]
            for (m, c, nn, bb, s) in data["g16_modes"]
            if m == mode and c == circuit and nn == n and bb == B}
    if not rows and mode == "ondemand":
        rows = {s: data["g16_legacy"][(circuit, n, s)]
                for (c, nn, s) in data["g16_legacy"]
                if c == circuit and nn == n}
    return rows


# ── Cumulative cost model ────────────────────────────────────────

def cumulative_uvc(data, circuit, n, B, step):
    """setup + step * median(prove_ms over measured steps)."""
    steps = uvc_steps(data, circuit, n, B)
    if not steps:
        return None
    setup = float(data["uvc"][(circuit, n, B, steps[0])]["setup_ms"])
    proves = sorted(float(data["uvc"][(circuit, n, B, s)]["prove_ms"]) for s in steps)
    return setup + step * proves[len(proves) // 2]


def cumulative_groth16(data, mode, circuit, n, B, step, proving_only=False):
    """Cumulative Groth16 cost at `step` for one mode, or None if unmeasured."""
    rows = _mode_rows(data, mode, circuit, n, B)
    if not rows:
        return None

    if mode == "ondemand":
        if proving_only:
            cost_at = {s: float(r["prove_ms"]) for s, r in rows.items()}
        else:
            cost_at = {s: float(r["setup_ms"]) + float(r["prove_ms"])
                       for s, r in rows.items()}
        return trapezoid_cum(cost_at, step)

    # fixedcrs / singlestep: one setup, then per-step proving
    cost_at = {s: float(r["prove_ms"]) for s, r in rows.items()}
    total = trapezoid_cum(cost_at, step)
    if not proving_only:
        total += float(rows[min(rows)]["setup_ms"])
    return total


def cumulative_nova(data, kind, circuit, n, step):
    """foldonly: setup + total_fold(step).  compressed: + compress."""
    row = data["nova"].get((circuit, n, step))
    if not row:
        return None
    total = float(row["setup_ms"]) + float(row["total_fold_ms"])
    if kind == "compressed":
        total += float(row["compress_ms"])
    return total
