#!/usr/bin/env bash
#
# run_benchmarks.sh — Run all UVC paper benchmarks and save results.
#
# Usage:
#   ./scripts/run_benchmarks.sh              # Full state-bound paper-grade run
#   ./scripts/run_benchmarks.sh --quick      # Quick state-bound smoke test
#   ./scripts/run_benchmarks.sh --cpp-only   # C++ benchmarks only (UVC + Groth16)
#   ./scripts/run_benchmarks.sh --nova-only  # Nova benchmarks only (Rust)
#   ./scripts/run_benchmarks.sh --tables-only # Merge + generate tables from existing CSVs
#   ./scripts/run_benchmarks.sh --groth16-only # Groth16 modes only, at the bound-matched coordinates
#   ./scripts/run_benchmarks.sh --nova-runs N # Nova re-runs per configuration (default 5)
#
# Output:
#   results/YYYY-MM-DD_HHMMSS_TAG/
#     csv/uvc_results.csv
#     csv/groth16_results.csv
#     csv/nova_results.csv
#     csv/combined_results.csv
#     tables/*.tex
#     system_info.txt
#     run.log

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
NOVA_DIR="$PROJECT_DIR/nova-bench"

# ── Defaults (paper-grade) ──────────────────────────────────────────
TAG="paper"
REPS=10
MODE="all"
QUICK=false
NOVA_RUNS=5

# Paper Table 2 configurations: (circuit, n, B)
CONFIGS=(
    "mimc:270:16"
    "mimc:270:64"
    "hadamard:16:16"
    "hadamard:16:64"
    "hadamard:32:16"
    "hadamard:32:64"
    "scalable:1024:16"
    "scalable:1024:64"
    "scalable:16384:16"
    "scalable:16384:64"
)
GROTH16_CONFIGS=(
    "mimc:270:64"
    "hadamard:16:64"
    "hadamard:32:64"
    "scalable:1024:64"
    "scalable:16384:64"
)

# Nova step counts to benchmark (matches UVC B values)
NOVA_CONFIGS=(
    "mimc:270:64"
    "hadamard:16:64"
    "hadamard:32:64"
    "scalable:1024:64"
    "scalable:16384:64"
)

# ── Parse arguments ─────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick)
            QUICK=true
            TAG="quick"
            REPS=2
            NOVA_RUNS=1
            CONFIGS=(
                "mimc:270:16"
                "hadamard:16:16"
                "scalable:1024:16"
            )
            GROTH16_CONFIGS=(
                "mimc:270:16"
                "hadamard:16:16"
                "scalable:1024:16"
            )
            NOVA_CONFIGS=(
                "mimc:270:16"
                "hadamard:16:16"
                "scalable:1024:16"
            )
            shift
            ;;
        --cpp-only)   MODE="cpp"; shift ;;
        --nova-only)  MODE="nova"; shift ;;
        --tables-only) MODE="tables"; shift ;;
        --groth16-only)
            # Bound-matched Groth16 sweep: measures only the (circuit, B)
            # coordinates missing from the paper-grade run, so existing
            # UVC/Nova data stays untouched.
            MODE="groth16"
            TAG="g16-bound"
            GROTH16_CONFIGS=(
                "mimc:270:16"
                "hadamard:16:16"
                "hadamard:32:16"
                "scalable:1024:16"
                "scalable:16384:16"
            )
            shift
            ;;
        --reps)       REPS="$2"; shift 2 ;;
        --nova-runs)  NOVA_RUNS="$2"; shift 2 ;;
        --help|-h)
            head -18 "$0" | tail -17
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Run $0 --help for usage."
            exit 1
            ;;
    esac
done
UVC_RESULTS="uvc_results.csv"

if $QUICK; then
    BENCH_VERIFY_ARGS=(--verify-measured-only)
else
    BENCH_VERIFY_ARGS=()
fi
BENCH_SOURCE_PATHS=(
    "CMakeLists.txt"
    ".gitmodules"
    ":(glob)scripts/*.sh"
    ":(glob)scripts/*.py"
    ":(glob)benchmark-results/scripts/*.py"
    ":(glob)src/**/*.cpp"
    ":(glob)src/**/*.hpp"
    ":(glob)src/**/*.tcc"
    "nova-bench/Cargo.toml"
    "nova-bench/Cargo.lock"
    ":(glob)nova-bench/src/**/*.rs"
    "depends"
)

ensure_benchmark_sources_clean() {
    local untracked
    if ! git -C "$PROJECT_DIR" diff --quiet HEAD -- "${BENCH_SOURCE_PATHS[@]}" ||
       ! git -C "$PROJECT_DIR" diff --cached --quiet HEAD -- "${BENCH_SOURCE_PATHS[@]}"; then
        echo "ERROR: benchmark source differs from HEAD; commit or revert the benchmark inputs before measuring."
        exit 1
    fi
    untracked="$(git -C "$PROJECT_DIR" ls-files --others --exclude-standard -- "${BENCH_SOURCE_PATHS[@]}")"
    if [[ -n "$untracked" ]]; then
        echo "ERROR: untracked benchmark source files are not bound to a commit:"
        echo "$untracked"
        exit 1
    fi
}

ensure_benchmark_sources_clean
BENCH_COMMIT_FULL="$(git -C "$PROJECT_DIR" rev-parse HEAD)"
BENCH_COMMIT="${BENCH_COMMIT_FULL:0:7}"

write_coordinate_manifest() {
    python3 - "$CSV_DIR/manifest.json" "$MODE" "${CONFIGS[@]}" -- "${GROTH16_CONFIGS[@]}" -- "${NOVA_CONFIGS[@]}" <<'PY'
import json
import sys

output, mode, *configs = sys.argv[1:]
first = configs.index("--")
second = configs.index("--", first + 1)
cpp_configs = configs[:first]
groth16_configs = configs[first + 1:second]
nova_configs = configs[second + 1:]
if mode == "nova":
    selected_configs = nova_configs
    schemes = ["nova"]
elif mode == "cpp":
    selected_configs = cpp_configs
    schemes = ["uvc_gamma_v1", "groth16", "groth16_fixedcrs", "groth16_singlestep"]
elif mode == "all":
    selected_configs = cpp_configs
    schemes = ["uvc_gamma_v1", "groth16", "groth16_fixedcrs", "groth16_singlestep", "nova"]
elif mode == "groth16":
    selected_configs = []
    schemes = ["groth16", "groth16_fixedcrs", "groth16_singlestep"]
else:
    selected_configs = cpp_configs
    schemes = []

def build(configs):
    out = []
    for config in configs:
        circuit, n, bound = config.split(":")
        bound = int(bound)
        steps = []
        step = 1
        while step <= bound:
            steps.append(step)
            step *= 2
        if steps[-1] != bound:
            steps.append(bound)
        out.append({"circuit": circuit, "n": int(n), "B": bound, "steps": steps})
    return out

coordinates = build(selected_configs)
# The Groth16 sweep runs its own, usually smaller, config list, so the three
# Groth16 modes are checked against these coordinates, not the UVC ones.
groth16_coordinates = build(groth16_configs) if mode in ("cpp", "all", "groth16") else []

with open(output, "w") as target:
    json.dump({"suite": "paper", "schemes": schemes, "coordinates": coordinates,
               "groth16_coordinates": groth16_coordinates}, target)
    target.write("\n")
PY
}

write_run_manifest() {
    local compiler machine timestamp csv_hashes current_commit
    ensure_benchmark_sources_clean
    current_commit="$(git -C "$PROJECT_DIR" rev-parse HEAD)"
    if [[ "$current_commit" != "$BENCH_COMMIT_FULL" ]]; then
        echo "ERROR: repository HEAD changed during benchmark run: $BENCH_COMMIT_FULL -> $current_commit"
        exit 1
    fi
    if [[ -f "$CSV_DIR/uvc_results.csv" ]]; then
        python3 - "$CSV_DIR/uvc_results.csv" "$BENCH_COMMIT" <<'PY'
import csv
import sys

path, expected = sys.argv[1:]
with open(path, newline="") as source:
    rows = list(csv.DictReader(source))
bad = [index for index, row in enumerate(rows, 2) if row.get("commit") != expected]
if bad:
    raise SystemExit(f"ERROR: {path}: row commits do not match run commit {expected}: {bad}")
PY
    fi
    compiler="$(c++ --version | head -1)"
    machine="$(uname -m) $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -a)"
    timestamp="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    csv_hashes="$(shasum -a 256 "$CSV_DIR"/*.csv)"
    RUN_MANIFEST_COMMIT="$BENCH_COMMIT_FULL" \
    RUN_MANIFEST_COMPILER="$compiler" \
    RUN_MANIFEST_MACHINE="$machine" \
    RUN_MANIFEST_TIMESTAMP="$timestamp" \
    RUN_MANIFEST_HASHES="$csv_hashes" \
    python3 - "$OUT_DIR/run_manifest.json" <<'PY'
import json
import os
import sys

hashes = {}
for line in os.environ["RUN_MANIFEST_HASHES"].splitlines():
    digest, path = line.split(maxsplit=1)
    hashes[os.path.basename(path)] = digest
with open(sys.argv[1], "w") as target:
    json.dump({
        "git_commit": os.environ["RUN_MANIFEST_COMMIT"],
        "compiler_version": os.environ["RUN_MANIFEST_COMPILER"],
        "cmake_options": "-DCURVE=ALT_BN128 -DWITH_PROCPS=OFF -DWITH_SUPERCOP=OFF -DUSE_ASM=OFF -DMULTICORE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -Wno-dev",
        "machine": os.environ["RUN_MANIFEST_MACHINE"],
        "timestamp": os.environ["RUN_MANIFEST_TIMESTAMP"],
        "csv_sha256": hashes,
    }, target, indent=2, sort_keys=True)
    target.write("\n")
PY
    chmod -R a-w "$OUT_DIR"
}


# ── Output directory ────────────────────────────────────────────────
TIMESTAMP="$(date +%Y-%m-%d_%H%M%S)"
OUT_DIR="$PROJECT_DIR/results/${TIMESTAMP}_${TAG}"
CSV_DIR="$OUT_DIR/csv"
TABLE_DIR="$OUT_DIR/tables"
mkdir -p "$CSV_DIR" "$TABLE_DIR"
seal_completed_run() {
    if [[ -f "$OUT_DIR/run_manifest.json" ]]; then
        chmod -R a-w "$OUT_DIR"
    fi
}
trap seal_completed_run EXIT

LOG="$OUT_DIR/run.log"
exec > >(tee -a "$LOG") 2>&1
write_coordinate_manifest

echo "============================================================"
echo "  UVC Benchmark Suite"
echo "  $(date)"
echo "  Mode: $MODE"
echo "  Tag:  $TAG"
echo "  Reps: $REPS"
echo "  Output: $OUT_DIR"
echo "============================================================"
echo ""

# ── System info ─────────────────────────────────────────────────────
{
    echo "Date:     $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "Hostname: $(hostname)"
    echo "OS:       $(uname -srm)"
    if [[ "$(uname)" == "Darwin" ]]; then
        echo "CPU:      $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
        echo "Cores:    $(sysctl -n hw.ncpu)"
        echo "RAM:      $(( $(sysctl -n hw.memsize) / 1073741824 )) GB"
    else
        echo "CPU:      $(lscpu 2>/dev/null | grep 'Model name' | sed 's/.*: *//' || echo unknown)"
        echo "Cores:    $(nproc)"
        echo "RAM:      $(free -h 2>/dev/null | awk '/Mem:/{print $2}' || echo unknown)"
    fi
    echo "Compiler: $(c++ --version 2>/dev/null | head -1 || echo unknown)"
    echo "Build:    Release -O2 -march=native"
    echo "Reps:     $REPS"
    echo "Configs:  ${#CONFIGS[@]} UVC + ${#GROTH16_CONFIGS[@]} Groth16 + ${#NOVA_CONFIGS[@]} Nova"
    echo "OMP_NUM_THREADS: ${OMP_NUM_THREADS:-unset}"
    echo "RAYON_NUM_THREADS: ${RAYON_NUM_THREADS:-unset}"
} > "$OUT_DIR/system_info.txt"

cat "$OUT_DIR/system_info.txt"
echo ""

# ── Helper ──────────────────────────────────────────────────────────
run_bench() {
    local name="$1"
    local bin="$2"
    shift 2
    local args=("$@")

    echo "------------------------------------------------------------"
    echo "  Running: $name"
    echo "  Command: $(basename "$bin") ${args[*]}"
    echo "------------------------------------------------------------"

    local start_time
    start_time=$(date +%s)

    "$bin" "${args[@]}" 2>&1
    local rc=$?

    local end_time
    end_time=$(date +%s)
    local elapsed=$(( end_time - start_time ))

    if [[ $rc -eq 0 ]]; then
        echo "  Done in ${elapsed}s"
    else
        echo "  FAILED (exit $rc) after ${elapsed}s"
    fi
    echo ""
    return $rc
}

# ── Build C++ ───────────────────────────────────────────────────────
build_all() {
    echo "  Building all benchmarks..."

    # ── C++ ──
    NCPU="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
    mkdir -p "$BUILD_DIR"

    # Always (re)configure with the pinned options — idempotent, and it
    # guarantees a reused cache cannot drift from the recorded provenance.
    cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
        -DCURVE=ALT_BN128 \
        -DWITH_PROCPS=OFF \
        -DWITH_SUPERCOP=OFF \
        -DUSE_ASM=OFF \
        -DMULTICORE=ON \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -Wno-dev 2>&1 | tail -5

    cmake --build "$BUILD_DIR" --target bench_comparative -j"$NCPU" 2>&1

    if [[ ! -x "$BUILD_DIR/bench_comparative" ]]; then
        echo "ERROR: C++ build failed."
        exit 1
    fi
    echo "  C++ binary: $BUILD_DIR/bench_comparative"

    # ── Rust (Nova) ──
    if [[ -d "$NOVA_DIR" ]]; then
        (cd "$NOVA_DIR" && cargo build --release 2>&1)
        echo "  Nova binary: $NOVA_DIR/target/release/nova-bench"
    fi

    echo ""
}

# ── C++ Benchmarks (UVC + Groth16) ─────────────────────────────────
run_cpp() {
    local BENCH="$BUILD_DIR/bench_comparative"

    if [[ ! -x "$BENCH" ]]; then
        echo "ERROR: $BENCH not found. Run build_all first."
        exit 1
    fi

    echo ""
    echo "============================================================"
    echo "  C++ Benchmarks: UVC + Groth16"
    echo "  Configurations: ${#CONFIGS[@]} UVC + ${#GROTH16_CONFIGS[@]} Groth16"
    echo "============================================================"
    echo ""

    local i=0
    for config in "${CONFIGS[@]}"; do
        [[ "$MODE" == "groth16" ]] && break   # --groth16-only: no UVC re-measurement
        IFS=: read -r circuit n B <<< "$config"
        i=$((i + 1))

        run_bench "[UVC $i/${#CONFIGS[@]}] $circuit n=$n B=$B" \
            "$BENCH" \
            --circuit "$circuit" --n "$n" --B "$B" --scheme uvc \
            --output-dir "$CSV_DIR" --reps "$REPS" --commit "$BENCH_COMMIT" \
            ${BENCH_VERIFY_ARGS[@]+"${BENCH_VERIFY_ARGS[@]}"}
    done

    i=0
    for config in "${GROTH16_CONFIGS[@]}"; do
        IFS=: read -r circuit n B <<< "$config"
        i=$((i + 1))

        run_bench "[Groth16 $i/${#GROTH16_CONFIGS[@]}] $circuit n=$n max-step=$B" \
            "$BENCH" \
            --circuit "$circuit" --n "$n" --B "$B" --scheme groth16 \
            --output-dir "$CSV_DIR" --reps "$REPS" --commit "$BENCH_COMMIT"
    done

    i=0
    for config in "${GROTH16_CONFIGS[@]}"; do
        IFS=: read -r circuit n B <<< "$config"
        i=$((i + 1))

        run_bench "[Groth16-fixedcrs $i/${#GROTH16_CONFIGS[@]}] $circuit n=$n max-step=$B" \
            "$BENCH" \
            --circuit "$circuit" --n "$n" --B "$B" --scheme groth16-fixedcrs \
            --output-dir "$CSV_DIR" --reps "$REPS" --commit "$BENCH_COMMIT"
    done

    i=0
    for config in "${GROTH16_CONFIGS[@]}"; do
        IFS=: read -r circuit n B <<< "$config"
        i=$((i + 1))

        run_bench "[Groth16-singlestep $i/${#GROTH16_CONFIGS[@]}] $circuit n=$n max-step=$B" \
            "$BENCH" \
            --circuit "$circuit" --n "$n" --B "$B" --scheme groth16-singlestep \
            --output-dir "$CSV_DIR" --reps "$REPS" --commit "$BENCH_COMMIT"
    done

    grep -m1 "OpenMP max threads" "$LOG" | sed 's/^ *//' >> "$OUT_DIR/system_info.txt"

    echo ""
    echo "C++ benchmarks complete."
    echo "  UVC:     $CSV_DIR/$UVC_RESULTS"
    echo "  Groth16: $CSV_DIR/groth16_results.csv"
    echo "  Groth16 modes: $CSV_DIR/groth16_modes_results.csv"
}

# ── Nova Benchmarks (Rust) ─────────────────────────────────────────
run_nova() {
    local NOVABIN="$NOVA_DIR/target/release/nova-bench"

    if [[ ! -x "$NOVABIN" ]]; then
        echo "  WARNING: $NOVABIN not found, skipping Nova benchmarks."
        return 0
    fi

    "$NOVABIN" --print-config 2>> "$OUT_DIR/system_info.txt"

    echo ""
    echo "============================================================"
    echo "  Nova Folding-Scheme Benchmarks"
    echo "  Configurations: ${#NOVA_CONFIGS[@]}"
    echo "============================================================"
    echo ""

    local i=0
    for config in "${NOVA_CONFIGS[@]}"; do
        IFS=: read -r circuit n steps <<< "$config"
        i=$((i + 1))

        run_bench "[$i/${#NOVA_CONFIGS[@]}] nova $circuit n=$n steps=$steps" \
            "$NOVABIN" \
            --circuit "$circuit" --n "$n" --steps "$steps" \
            --runs "$NOVA_RUNS" \
            --output-dir "$CSV_DIR"
    done

    grep -m1 "rayon_threads" "$LOG" | sed 's/^.*rayon_threads=/rayon threads: /' >> "$OUT_DIR/system_info.txt"

    echo ""
    echo "Nova benchmarks complete."
    echo "  Results: $CSV_DIR/nova_results.csv"
}

# ── Merge & Generate Tables ────────────────────────────────────────
run_tables() {
    echo ""
    echo "============================================================"
    echo "  Merging Results & Generating Tables"
    echo "============================================================"
    echo ""

    # Use the Python scripts from benchmark-results/scripts/ if they exist,
    # otherwise use our own summarize_results.py
    # These consumers receive the run's csv/ directory, which contains either
    # the state-bound CSV.
    local RESULTS_SCRIPTS_DIR="$PROJECT_DIR/benchmark-results/scripts"
    local MERGE_SCRIPT="$RESULTS_SCRIPTS_DIR/merge_results.py"
    local TABLE_SCRIPT="$RESULTS_SCRIPTS_DIR/gen_latex_tables.py"
    local SUMMARIZE="$SCRIPT_DIR/summarize_results.py"

    # Merge CSVs
    if [[ -f "$MERGE_SCRIPT" ]]; then
        python3 "$MERGE_SCRIPT" \
            --input-dir "$CSV_DIR" \
            --output "$CSV_DIR/combined_results.csv"
    fi

    # Generate LaTeX tables
    if [[ -f "$TABLE_SCRIPT" ]]; then
        python3 "$TABLE_SCRIPT" \
            --input-dir "$CSV_DIR" \
            --output-dir "$TABLE_DIR"
    fi

    # Summarize results: numbered .txt tables + summary.txt + LaTeX
    if [[ -f "$SUMMARIZE" ]]; then
        echo ""
        echo "------------------------------------------------------------"
        echo "  Generating tables..."
        echo "------------------------------------------------------------"
        python3 "$SUMMARIZE" "$CSV_DIR" --save-dir "$OUT_DIR"
    fi
    python3 "$SCRIPT_DIR/cumulative_prove_time.py" "$CSV_DIR"

    echo ""
    echo "  Tables: $TABLE_DIR/"
    ls "$TABLE_DIR/" 2>/dev/null || echo "  (no tables generated)"
    python3 "$PROJECT_DIR/scripts/verify_completeness.py" "$CSV_DIR"
}

# ── Verify data ────────────────────────────────────────────────────
run_verify() {
    local VERIFY_SCRIPT="$PROJECT_DIR/benchmark-results/scripts/verify_table2.py"
    local UVC_CSV="$CSV_DIR/uvc_results.csv"

    if [[ -f "$VERIFY_SCRIPT" ]] && [[ -f "$UVC_CSV" ]]; then
        echo ""
        echo "------------------------------------------------------------"
        echo "  Verifying data completeness..."
        echo "------------------------------------------------------------"
        python3 "$VERIFY_SCRIPT" --input "$UVC_CSV"
    fi
}

# ── Main ────────────────────────────────────────────────────────────
case "$MODE" in
    cpp)    build_all; run_cpp ;;
    groth16) build_all; run_cpp; write_run_manifest ;;
    nova)   build_all; run_nova ;;
    tables)
        # For --tables-only, look for existing CSVs
        if [[ ! -f "$CSV_DIR/$UVC_RESULTS" ]]; then
            # Try benchmark-results/csv first (canonical location)
            if [[ -f "$PROJECT_DIR/benchmark-results/csv/$UVC_RESULTS" ]]; then
                echo "  Using existing CSVs from: benchmark-results/csv/"
                CSV_DIR="$PROJECT_DIR/benchmark-results/csv"
            else
                # Try latest results dir (exclude the one we just created)
                LATEST="$(find "$PROJECT_DIR/results" -name "$UVC_RESULTS" -type f 2>/dev/null | head -1)"
                if [[ -n "$LATEST" ]]; then
                    CSV_DIR="$(dirname "$LATEST")"
                    echo "  Using existing CSVs from: $CSV_DIR"
                else
                    echo "  ERROR: No benchmark CSVs found. Run benchmarks first."
                    exit 1
                fi
            fi
        fi
        run_tables
        ;;
    all)
        build_all
        run_cpp
        run_nova
        run_tables
        if ! $QUICK; then
            run_verify
        fi

        echo ""
        echo "============================================================"
        echo "  All benchmarks complete!"
        echo ""
        echo "  Results: $OUT_DIR"
        echo ""
        echo "  Files:"
        for f in "$CSV_DIR"/*.csv; do
            [[ -f "$f" ]] || continue
            lines=$(wc -l < "$f")
            echo "    csv/$(basename "$f")  ($lines lines)"
        done
        for f in "$TABLE_DIR"/*.tex; do
            [[ -f "$f" ]] || continue
            echo "    tables/$(basename "$f")"
        done
        echo ""
        echo "  System info: $OUT_DIR/system_info.txt"
        echo "  Full log:    $LOG"
        echo "============================================================"
        write_run_manifest
        ;;
    *)
        echo "Unknown mode: $MODE"
        exit 1
        ;;
esac
