#!/usr/bin/env bash
#
# run_iot_benchmarks.sh — Run IoT sensor fusion benchmarks for UVC.
#
# Benchmarks the weighted multi-sensor fusion (EMA) circuit across
# different sensor counts K and step bounds B, comparing UVC, Groth16,
# and Nova.
#
# Usage:
#   ./scripts/run_iot_benchmarks.sh              # Full state-bound paper-grade run
#   ./scripts/run_iot_benchmarks.sh --quick      # Quick state-bound smoke test
#   ./scripts/run_iot_benchmarks.sh --cpp-only   # C++ benchmarks only (UVC + Groth16)
#   ./scripts/run_iot_benchmarks.sh --nova-only  # Nova benchmarks only (Rust)
#   ./scripts/run_iot_benchmarks.sh --tables-only # Generate tables from existing CSVs
#
# Output:
#   results/YYYY-MM-DD_HHMMSS_TAG/
#     csv/uvc_results.csv
#     csv/groth16_results.csv
#     csv/nova_results.csv
#     tables/*.tex
#     system_info.txt
#     run.log

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
NOVA_DIR="$PROJECT_DIR/nova-bench"

# ── Defaults ──────────────────────────────────────────────────────
TAG="iot"
REPS=10
MODE="all"
QUICK=false

# IoT sensor fusion configurations: (circuit, K, B)
# K = number of sensors (constraints = K+1 per step)
#
# K=4: Small sensor node  (temp, humidity, pressure, light)
# K=6: Extended node       (+ wind speed, soil moisture)
# K=8: Multi-modal gateway (environmental + motion sensors)
#
# B = step bound (number of sequential fusion steps)
# B=64:   Short window   (~1 min at 1 Hz)
# B=256:  Medium window  (~4 min at 1 Hz)
# B=1024: Long window    (~17 min at 1 Hz)
CONFIGS=(
    "sensor_fusion:4:64"
    "sensor_fusion:4:256"
    "sensor_fusion:4:1024"
    "sensor_fusion:6:64"
    "sensor_fusion:6:256"
    "sensor_fusion:6:1024"
    "sensor_fusion:8:64"
    "sensor_fusion:8:256"
    "sensor_fusion:8:1024"
)

# Nova configs (steps parameter = number of folding steps)
NOVA_CONFIGS=(
    "sensor_fusion:4:64"
    "sensor_fusion:4:256"
    "sensor_fusion:4:1024"
    "sensor_fusion:6:64"
    "sensor_fusion:6:256"
    "sensor_fusion:6:1024"
    "sensor_fusion:8:64"
    "sensor_fusion:8:256"
    "sensor_fusion:8:1024"
)

# ── Parse arguments ─────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick)
            QUICK=true
            TAG="iot-quick"
            REPS=2
            CONFIGS=(
                "sensor_fusion:4:64"
                "sensor_fusion:8:64"
            )
            NOVA_CONFIGS=(
                "sensor_fusion:4:64"
                "sensor_fusion:8:64"
            )
            shift
            ;;
        --cpp-only)   MODE="cpp"; shift ;;
        --nova-only)  MODE="nova"; shift ;;
        --tables-only) MODE="tables"; shift ;;
        --reps)       REPS="$2"; shift 2 ;;
        --help|-h)
            head -22 "$0" | tail -21
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
BENCH_MODE_ARGS=()

if $QUICK; then
    BENCH_VERIFY_ARGS=(--verify-measured-only)
else
    BENCH_VERIFY_ARGS=()
fi
BENCH_COMMIT="$(git -C "$PROJECT_DIR" rev-parse --short HEAD)"

write_coordinate_manifest() {
    python3 - "$CSV_DIR/manifest.json" "$MODE" "${CONFIGS[@]}" -- "${NOVA_CONFIGS[@]}" <<'PY'
import json
import sys

output, mode, *configs = sys.argv[1:]
separator = configs.index("--")
cpp_configs = configs[:separator]
nova_configs = configs[separator + 1:]
if mode == "nova":
    selected_configs = nova_configs
    schemes = ["nova"]
elif mode == "cpp":
    selected_configs = cpp_configs
    schemes = ["uvc_gamma_v1", "groth16"]
elif mode == "all":
    selected_configs = cpp_configs
    schemes = ["uvc_gamma_v1", "groth16", "nova"]
else:
    selected_configs = cpp_configs
    schemes = []

coordinates = []
for config in selected_configs:
    circuit, n, bound = config.split(":")
    bound = int(bound)
    steps = []
    step = 1
    while step <= bound:
        steps.append(step)
        step *= 2
    if steps[-1] != bound:
        steps.append(bound)
    coordinates.append({"circuit": circuit, "n": int(n), "B": bound, "steps": steps})

with open(output, "w") as target:
    json.dump({"suite": "iot", "schemes": schemes, "coordinates": coordinates}, target)
    target.write("\n")
PY
}

write_run_manifest() {
    local full_commit compiler machine timestamp csv_hashes
    full_commit="$(git -C "$PROJECT_DIR" rev-parse HEAD)"
    compiler="$(c++ --version | head -1)"
    machine="$(uname -m) $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -a)"
    timestamp="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    csv_hashes="$(shasum -a 256 "$CSV_DIR"/*.csv)"
    RUN_MANIFEST_COMMIT="$full_commit" \
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

LOG="$OUT_DIR/run.log"
exec > >(tee -a "$LOG") 2>&1
write_coordinate_manifest

echo "============================================================"
echo "  UVC IoT Sensor Fusion Benchmarks"
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
    echo "Configs:  ${#CONFIGS[@]} C++ + ${#NOVA_CONFIGS[@]} Nova"
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

# ── Build ───────────────────────────────────────────────────────────
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
    echo "  C++ IoT Sensor Fusion: UVC + Groth16"
    echo "  Configurations: ${#CONFIGS[@]}"
    echo "============================================================"
    echo ""

    local i=0
    for config in "${CONFIGS[@]}"; do
        IFS=: read -r circuit K B <<< "$config"
        i=$((i + 1))

        run_bench "[$i/${#CONFIGS[@]}] $circuit K=$K B=$B" \
            "$BENCH" \
            --circuit "$circuit" --n "$K" --B "$B" \
            --output-dir "$CSV_DIR" --reps "$REPS" --commit "$BENCH_COMMIT" \
            ${BENCH_MODE_ARGS[@]+"${BENCH_MODE_ARGS[@]}"} ${BENCH_VERIFY_ARGS[@]+"${BENCH_VERIFY_ARGS[@]}"}
    done

    echo ""
    echo "C++ IoT benchmarks complete."
    echo "  UVC:     $CSV_DIR/$UVC_RESULTS"
    echo "  Groth16: $CSV_DIR/groth16_results.csv"
}

# ── Nova Benchmarks (Rust) ─────────────────────────────────────────
run_nova() {
    local NOVABIN="$NOVA_DIR/target/release/nova-bench"

    if [[ ! -x "$NOVABIN" ]]; then
        echo "  WARNING: $NOVABIN not found, skipping Nova benchmarks."
        return 0
    fi

    echo ""
    echo "============================================================"
    echo "  Nova IoT Sensor Fusion Benchmarks"
    echo "  Configurations: ${#NOVA_CONFIGS[@]}"
    echo "============================================================"
    echo ""

    local i=0
    for config in "${NOVA_CONFIGS[@]}"; do
        IFS=: read -r circuit K steps <<< "$config"
        i=$((i + 1))

        run_bench "[$i/${#NOVA_CONFIGS[@]}] nova $circuit K=$K steps=$steps" \
            "$NOVABIN" \
            --circuit "$circuit" --n "$K" --steps "$steps" \
            --output-dir "$CSV_DIR"
    done

    echo ""
    echo "Nova IoT benchmarks complete."
    echo "  Results: $CSV_DIR/nova_results.csv"
}

# ── Merge & Generate Tables ────────────────────────────────────────
run_tables() {
    echo ""
    echo "============================================================"
    echo "  Merging Results & Generating Tables"
    echo "============================================================"
    echo ""

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

    echo ""
    echo "  Tables: $TABLE_DIR/"
    ls "$TABLE_DIR/" 2>/dev/null || echo "  (no tables generated)"
    python3 "$PROJECT_DIR/scripts/verify_completeness.py" "$CSV_DIR"
}

# ── Main ────────────────────────────────────────────────────────────
case "$MODE" in
    cpp)    build_all; run_cpp ;;
    nova)   build_all; run_nova ;;
    tables)
        # For --tables-only, look for existing CSVs
        if [[ ! -f "$CSV_DIR/$UVC_RESULTS" ]]; then
            LATEST="$(find "$PROJECT_DIR/results" -name "$UVC_RESULTS" -type f 2>/dev/null | head -1)"
            if [[ -n "$LATEST" ]]; then
                CSV_DIR="$(dirname "$LATEST")"
                echo "  Using existing CSVs from: $CSV_DIR"
            else
                echo "  ERROR: No IoT benchmark CSVs found. Run benchmarks first."
                exit 1
            fi
        fi
        run_tables
        ;;
    all)
        build_all
        run_cpp
        run_nova
        run_tables

        echo ""
        echo "============================================================"
        echo "  All IoT sensor fusion benchmarks complete!"
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
