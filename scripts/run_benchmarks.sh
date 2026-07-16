#!/usr/bin/env bash
#
# run_benchmarks.sh — Run all UVC paper benchmarks and save results.
#
# Usage:
#   ./scripts/run_benchmarks.sh              # Full state-bound paper-grade run
#   ./scripts/run_benchmarks.sh --legacy     # Pre-fix compatibility run
#   ./scripts/run_benchmarks.sh --quick      # Quick state-bound smoke test
#   ./scripts/run_benchmarks.sh --cpp-only   # C++ benchmarks only (UVC + Groth16)
#   ./scripts/run_benchmarks.sh --nova-only  # Nova benchmarks only (Rust)
#   ./scripts/run_benchmarks.sh --tables-only # Merge + generate tables from existing CSVs
#
# Output:
#   results/YYYY-MM-DD_HHMMSS_TAG/
#     csv/uvc_bound_results.csv (state-bound; uvc_results.csv with --legacy)
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
TAG="paper_v2"
REPS=10
MODE="all"
LEGACY=false
QUICK=false

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
            CONFIGS=(
                "mimc:270:16"
                "hadamard:16:16"
                "scalable:1024:16"
            )
            NOVA_CONFIGS=(
                "mimc:270:64"
                "hadamard:16:64"
                "scalable:1024:64"
            )
            shift
            ;;
        --legacy)     LEGACY=true; shift ;;
        --cpp-only)   MODE="cpp"; shift ;;
        --nova-only)  MODE="nova"; shift ;;
        --tables-only) MODE="tables"; shift ;;
        --reps)       REPS="$2"; shift 2 ;;
        --help|-h)
            head -17 "$0" | tail -16
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Run $0 --help for usage."
            exit 1
            ;;
    esac
done
if $LEGACY; then
    if $QUICK; then
        TAG="quick_legacy"
    else
        TAG="paper_legacy"
    fi
    UVC_RESULTS="uvc_results.csv"
    BENCH_MODE_ARGS=(--legacy)
else
    UVC_RESULTS="uvc_bound_results.csv"
    BENCH_MODE_ARGS=()
fi

if $QUICK; then
    BENCH_VERIFY_ARGS=(--verify-measured-only)
else
    BENCH_VERIFY_ARGS=()
fi


# ── Output directory ────────────────────────────────────────────────
TIMESTAMP="$(date +%Y-%m-%d_%H%M%S)"
OUT_DIR="$PROJECT_DIR/results/${TIMESTAMP}_${TAG}"
CSV_DIR="$OUT_DIR/csv"
TABLE_DIR="$OUT_DIR/tables"
mkdir -p "$CSV_DIR" "$TABLE_DIR"

LOG="$OUT_DIR/run.log"
exec > >(tee -a "$LOG") 2>&1

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

# ── Build C++ ───────────────────────────────────────────────────────
build_all() {
    echo "  Building all benchmarks..."

    # ── C++ ──
    NCPU="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
    mkdir -p "$BUILD_DIR"

    if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
        cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
            -DMULTICORE=ON \
            -Wno-dev 2>&1 | tail -5
    fi

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
    echo "  Configurations: ${#CONFIGS[@]}"
    echo "============================================================"
    echo ""

    local i=0
    for config in "${CONFIGS[@]}"; do
        IFS=: read -r circuit n B <<< "$config"
        i=$((i + 1))

        run_bench "[$i/${#CONFIGS[@]}] $circuit n=$n B=$B" \
            "$BENCH" \
            --circuit "$circuit" --n "$n" --B "$B" \
            --output-dir "$CSV_DIR" --reps "$REPS" \
            ${BENCH_MODE_ARGS[@]+"${BENCH_MODE_ARGS[@]}"} ${BENCH_VERIFY_ARGS[@]+"${BENCH_VERIFY_ARGS[@]}"}
    done

    echo ""
    echo "C++ benchmarks complete."
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
            --output-dir "$CSV_DIR"
    done

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
    # the state-bound CSV or the explicitly requested legacy CSV.
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
}

# ── Verify data ────────────────────────────────────────────────────
run_verify() {
    local VERIFY_SCRIPT="$PROJECT_DIR/benchmark-results/scripts/verify_table2.py"
    local COMBINED="$CSV_DIR/combined_results.csv"

    if [[ -f "$VERIFY_SCRIPT" ]] && [[ -f "$COMBINED" ]]; then
        echo ""
        echo "------------------------------------------------------------"
        echo "  Verifying data completeness..."
        echo "------------------------------------------------------------"
        python3 "$VERIFY_SCRIPT" --input "$COMBINED" || true
    fi
}

# ── Main ────────────────────────────────────────────────────────────
case "$MODE" in
    cpp)    build_all; run_cpp ;;
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
        run_verify

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
        ;;
    *)
        echo "Unknown mode: $MODE"
        exit 1
        ;;
esac
