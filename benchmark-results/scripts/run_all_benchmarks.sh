#!/usr/bin/env bash
#
# run_all_benchmarks.sh — Run all UVC paper benchmarks and save results.
#
# Usage:
#   ./run_all_benchmarks.sh              # Full paper-grade run (all circuits, B=16/64, 10 reps)
#   ./run_all_benchmarks.sh --quick      # Quick smoke test (one per type, B=16, 2 reps)
#   ./run_all_benchmarks.sh --cpp-only   # C++ benchmarks only (UVC + Groth16)
#   ./run_all_benchmarks.sh --nova-only  # Nova benchmarks only (Rust)
#   ./run_all_benchmarks.sh --tables-only # Merge + generate tables from existing CSVs
#
# Expects:
#   - C++ binary at $REPO/build/bench_comparative (run: cd build && cmake .. && make bench_comparative)
#   - Rust project at $REPO/nova-bench (run: cd nova-bench && cargo build --release)
#
# Output:
#   benchmark-results/
#     csv/uvc_results.csv
#     csv/groth16_results.csv
#     csv/nova_results.csv
#     csv/combined_results.csv
#     tables/*.tex
#     system_info.txt
#     run.log

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUT_DIR="$REPO/benchmark-results"
CSVDIR="$OUT_DIR/csv"
TABLE_DIR="$OUT_DIR/tables"
BUILD="$REPO/build"
NOVA="$REPO/nova-bench"

# ── Defaults (paper-grade) ──────────────────────────────────────────
REPS=10
MODE="all"

# Paper Table 2 configurations: (circuit, n, B)
CONFIGS=(
    "mimc:270:16"
    "mimc:270:64"
    "mimc:1080:16"
    "mimc:1080:64"
    "mimc:4320:16"
    "mimc:4320:64"
    "hadamard:32:16"
    "hadamard:32:64"
    "hadamard:128:16"
    "hadamard:128:64"
    "hadamard:512:16"
    "hadamard:512:64"
    "scalable:1024:16"
    "scalable:1024:64"
    "scalable:8192:16"
    "scalable:8192:64"
    "scalable:65536:16"
    "scalable:65536:64"
)

# Nova configurations (B=64 only)
NOVA_CONFIGS=(
    "mimc:270:64"
    "mimc:1080:64"
    "mimc:4320:64"
    "hadamard:32:64"
    "hadamard:128:64"
    "hadamard:512:64"
    "scalable:1024:64"
    "scalable:8192:64"
    "scalable:65536:64"
)

# ── Parse arguments ─────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick)
            REPS=2
            CONFIGS=(
                "mimc:270:16"
                "hadamard:32:16"
                "scalable:1024:16"
            )
            NOVA_CONFIGS=(
                "mimc:270:64"
                "hadamard:32:64"
                "scalable:1024:64"
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

# ── Output directories ────────────────────────────────────────────────
mkdir -p "$CSVDIR" "$TABLE_DIR"

LOG="$OUT_DIR/run.log"
exec > >(tee -a "$LOG") 2>&1

echo "============================================================"
echo "  UVC Benchmark Suite"
echo "  $(date)"
echo "  Mode:    $MODE"
echo "  Reps:    $REPS"
echo "  Configs: ${#CONFIGS[@]} C++ + ${#NOVA_CONFIGS[@]} Nova"
echo "  Output:  $OUT_DIR"
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
    mkdir -p "$BUILD"

    if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
        cmake -S "$REPO" -B "$BUILD" \
            -DMULTICORE=ON \
            -Wno-dev 2>&1 | tail -5
    fi

    cmake --build "$BUILD" --target bench_comparative -j"$NCPU" 2>&1

    if [[ ! -x "$BUILD/bench_comparative" ]]; then
        echo "ERROR: C++ build failed."
        exit 1
    fi
    echo "  C++ binary: $BUILD/bench_comparative"

    # ── Rust (Nova) ──
    if [[ -d "$NOVA" ]]; then
        (cd "$NOVA" && cargo build --release 2>&1)
        echo "  Nova binary: $NOVA/target/release/nova-bench"
    fi

    echo ""
}

# ── C++ Benchmarks (UVC + Groth16) ─────────────────────────────────
run_cpp() {
    local BENCH="$BUILD/bench_comparative"

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
            --output-dir "$CSVDIR" --reps "$REPS"
    done

    echo ""
    echo "C++ benchmarks complete."
    echo "  UVC:     $CSVDIR/uvc_results.csv"
    echo "  Groth16: $CSVDIR/groth16_results.csv"
}

# ── Nova Benchmarks (Rust) ─────────────────────────────────────────
run_nova() {
    local NOVABIN="$NOVA/target/release/nova-bench"

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
            --output-dir "$CSVDIR"
    done

    echo ""
    echo "Nova benchmarks complete."
    echo "  Results: $CSVDIR/nova_results.csv"
}

# ── Merge & Generate Tables ────────────────────────────────────────
run_tables() {
    echo ""
    echo "============================================================"
    echo "  Merging Results & Generating Tables"
    echo "============================================================"
    echo ""

    # Merge CSVs
    if [[ -f "$SCRIPT_DIR/merge_results.py" ]]; then
        python3 "$SCRIPT_DIR/merge_results.py" \
            --input-dir "$CSVDIR" \
            --output "$CSVDIR/combined_results.csv"
    fi

    # Generate LaTeX tables
    if [[ -f "$SCRIPT_DIR/gen_latex_tables.py" ]]; then
        python3 "$SCRIPT_DIR/gen_latex_tables.py" \
            --input-dir "$CSVDIR" \
            --output-dir "$TABLE_DIR"
    fi

    # Summarize results (terminal + LaTeX)
    if [[ -f "$SCRIPT_DIR/summarize_results.py" ]]; then
        echo ""
        echo "------------------------------------------------------------"
        echo "  Generating summary..."
        echo "------------------------------------------------------------"
        python3 "$SCRIPT_DIR/summarize_results.py" "$CSVDIR" --save-dir "$OUT_DIR" | tee "$OUT_DIR/summary.txt"
        echo ""
        echo "  Summary:      $OUT_DIR/summary.txt"

        echo ""
        echo "------------------------------------------------------------"
        echo "  Generating LaTeX tables..."
        echo "------------------------------------------------------------"
        python3 "$SCRIPT_DIR/summarize_results.py" "$CSVDIR" --latex > "$OUT_DIR/tables_latex.tex"
        echo "  LaTeX tables: $OUT_DIR/tables_latex.tex"
    fi

    echo ""
    echo "  Tables: $TABLE_DIR/"
    ls "$TABLE_DIR/" 2>/dev/null || echo "  (no tables generated)"
}

# ── Verify data ────────────────────────────────────────────────────
run_verify() {
    local COMBINED="$CSVDIR/combined_results.csv"

    if [[ -f "$SCRIPT_DIR/verify_table2.py" ]] && [[ -f "$COMBINED" ]]; then
        echo ""
        echo "------------------------------------------------------------"
        echo "  Verifying data completeness..."
        echo "------------------------------------------------------------"
        python3 "$SCRIPT_DIR/verify_table2.py" --input "$COMBINED" || true
    fi
}

# ── Main ────────────────────────────────────────────────────────────
case "$MODE" in
    cpp)    build_all; run_cpp ;;
    nova)   build_all; run_nova ;;
    tables) run_tables ;;
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
        for f in "$CSVDIR"/*.csv; do
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
